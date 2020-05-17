/*	$OpenBSD: tls13_lib.c,v 1.45 2020/05/17 19:07:15 beck Exp $ */
/*
 * Copyright (c) 2018, 2019 Joel Sing <jsing@openbsd.org>
 * Copyright (c) 2019 Bob Beck <beck@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stddef.h>

#include <openssl/evp.h>

#include "ssl_locl.h"
#include "tls13_internal.h"

/*
 * Downgrade sentinels - RFC 8446 section 4.1.3, magic values which must be set
 * by the server in server random if it is willing to downgrade but supports
 * TLSv1.3
 */
const uint8_t tls13_downgrade_12[8] = {
	0x44, 0x4f, 0x57, 0x4e, 0x47, 0x52, 0x44, 0x01,
};
const uint8_t tls13_downgrade_11[8] = {
	0x44, 0x4f, 0x57, 0x4e, 0x47, 0x52, 0x44, 0x00,
};

/*
 * HelloRetryRequest hash - RFC 8446 section 4.1.3.
 */
const uint8_t tls13_hello_retry_request_hash[32] = {
	0xcf, 0x21, 0xad, 0x74, 0xe5, 0x9a, 0x61, 0x11,
	0xbe, 0x1d, 0x8c, 0x02, 0x1e, 0x65, 0xb8, 0x91,
	0xc2, 0xa2, 0x11, 0x16, 0x7a, 0xbb, 0x8c, 0x5e,
	0x07, 0x9e, 0x09, 0xe2, 0xc8, 0xa8, 0x33, 0x9c,
};

/*
 * Certificate Verify padding - RFC 8446 section 4.4.3.
 */
const uint8_t tls13_cert_verify_pad[64] = {
	0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
	0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
	0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
	0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
	0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
	0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
	0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
	0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
};

const uint8_t tls13_cert_client_verify_context[] =
    "TLS 1.3, client CertificateVerify";
const uint8_t tls13_cert_server_verify_context[] =
    "TLS 1.3, server CertificateVerify";

const EVP_AEAD *
tls13_cipher_aead(const SSL_CIPHER *cipher)
{
	if (cipher == NULL)
		return NULL;
	if (cipher->algorithm_ssl != SSL_TLSV1_3)
		return NULL;

	switch (cipher->algorithm_enc) {
	case SSL_AES128GCM:
		return EVP_aead_aes_128_gcm();
	case SSL_AES256GCM:
		return EVP_aead_aes_256_gcm();
	case SSL_CHACHA20POLY1305:
		return EVP_aead_chacha20_poly1305();
	}

	return NULL;
}

const EVP_MD *
tls13_cipher_hash(const SSL_CIPHER *cipher)
{
	if (cipher == NULL)
		return NULL;
	if (cipher->algorithm_ssl != SSL_TLSV1_3)
		return NULL;

	switch (cipher->algorithm2) {
	case SSL_HANDSHAKE_MAC_SHA256:
		return EVP_sha256();
	case SSL_HANDSHAKE_MAC_SHA384:
		return EVP_sha384();
	}

	return NULL;
}

static void
tls13_alert_received_cb(uint8_t alert_desc, void *arg)
{
	struct tls13_ctx *ctx = arg;

	if (alert_desc == TLS13_ALERT_CLOSE_NOTIFY) {
		ctx->close_notify_recv = 1;
		ctx->ssl->internal->shutdown |= SSL_RECEIVED_SHUTDOWN;
		S3I(ctx->ssl)->warn_alert = alert_desc;
		return;
	}

	if (alert_desc == TLS13_ALERT_USER_CANCELED) {
		/*
		 * We treat this as advisory, since a close_notify alert
		 * SHOULD follow this alert (RFC 8446 section 6.1).
		 */
		return;
	}

	/* All other alerts are treated as fatal in TLSv1.3. */
	S3I(ctx->ssl)->fatal_alert = alert_desc;

	SSLerror(ctx->ssl, SSL_AD_REASON_OFFSET + alert_desc);
	ERR_asprintf_error_data("SSL alert number %d", alert_desc);

	SSL_CTX_remove_session(ctx->ssl->ctx, ctx->ssl->session);
}

static void
tls13_alert_sent_cb(uint8_t alert_desc, void *arg)
{
	struct tls13_ctx *ctx = arg;

	if (alert_desc == SSL_AD_CLOSE_NOTIFY) {
		ctx->close_notify_sent = 1;
		return;
	}

	if (alert_desc == SSL_AD_USER_CANCELLED) {
		return;
	}

	/* All other alerts are treated as fatal in TLSv1.3. */
	SSLerror(ctx->ssl, SSL_AD_REASON_OFFSET + alert_desc);
}

static void
tls13_legacy_handshake_message_recv_cb(void *arg)
{
	struct tls13_ctx *ctx = arg;
	SSL *s = ctx->ssl;
	CBS cbs;

	if (s->internal->msg_callback == NULL)
		return;

	tls13_handshake_msg_data(ctx->hs_msg, &cbs);
	s->internal->msg_callback(0, TLS1_3_VERSION, SSL3_RT_HANDSHAKE,
	    CBS_data(&cbs), CBS_len(&cbs), s, s->internal->msg_callback_arg);
}

static void
tls13_legacy_handshake_message_sent_cb(void *arg)
{
	struct tls13_ctx *ctx = arg;
	SSL *s = ctx->ssl;
	CBS cbs;

	if (s->internal->msg_callback == NULL)
		return;

	tls13_handshake_msg_data(ctx->hs_msg, &cbs);
	s->internal->msg_callback(1, TLS1_3_VERSION, SSL3_RT_HANDSHAKE,
	    CBS_data(&cbs), CBS_len(&cbs), s, s->internal->msg_callback_arg);
}

static int
tls13_legacy_ocsp_status_recv_cb(void *arg)
{
	struct tls13_ctx *ctx = arg;
	SSL *s = ctx->ssl;
	int ret;

	if (s->ctx->internal->tlsext_status_cb == NULL ||
	    s->internal->tlsext_ocsp_resp == NULL)
		return 1;

	ret = s->ctx->internal->tlsext_status_cb(s,
	    s->ctx->internal->tlsext_status_arg);
	if (ret < 0) {
		ctx->alert = TLS13_ALERT_INTERNAL_ERROR;
		SSLerror(s, ERR_R_MALLOC_FAILURE);
		return 0;
	}
	if (ret == 0) {
		ctx->alert = TLS13_ALERT_BAD_CERTIFICATE_STATUS_RESPONSE;
		SSLerror(s, SSL_R_INVALID_STATUS_RESPONSE);
		return 0;
	}

	return 1;
}

static int
tls13_phh_update_local_traffic_secret(struct tls13_ctx *ctx)
{
	struct tls13_secrets *secrets = ctx->hs->secrets;

	if (ctx->mode == TLS13_HS_CLIENT)
		return (tls13_update_client_traffic_secret(secrets) &&
		    tls13_record_layer_set_write_traffic_key(ctx->rl,
			&secrets->client_application_traffic));
	return (tls13_update_server_traffic_secret(secrets) &&
	    tls13_record_layer_set_read_traffic_key(ctx->rl,
	    &secrets->server_application_traffic));
}

static int
tls13_phh_update_peer_traffic_secret(struct tls13_ctx *ctx)
{
	struct tls13_secrets *secrets = ctx->hs->secrets;

	if (ctx->mode == TLS13_HS_CLIENT)
		return (tls13_update_server_traffic_secret(secrets) &&
		    tls13_record_layer_set_read_traffic_key(ctx->rl,
		    &secrets->server_application_traffic));
	return (tls13_update_client_traffic_secret(secrets) &&
	    tls13_record_layer_set_write_traffic_key(ctx->rl,
	    &secrets->client_application_traffic));
}

/*
 * XXX arbitrarily chosen limit of 100 post handshake handshake
 * messages in an hour - to avoid a hostile peer from constantly
 * requesting certificates or key renegotiaitons, etc.
 */
static int
tls13_phh_limit_check(struct tls13_ctx *ctx)
{
	time_t now = time(NULL);

	if (ctx->phh_last_seen > now - TLS13_PHH_LIMIT_TIME) {
		if (ctx->phh_count > TLS13_PHH_LIMIT)
			return 0;
	} else
		ctx->phh_count = 0;
	ctx->phh_count++;
	ctx->phh_last_seen = now;
	return 1;
}

static ssize_t
tls13_key_update_recv(struct tls13_ctx *ctx, CBS *cbs)
{
	uint8_t alert = TLS13_ALERT_INTERNAL_ERROR;
	uint8_t key_update_request;
	ssize_t ret;

	if (!CBS_get_u8(cbs, &key_update_request)) {
		alert = TLS13_ALERT_DECODE_ERROR;
		goto err;
	}
	if (CBS_len(cbs) != 0) {
		alert = TLS13_ALERT_DECODE_ERROR;
		goto err;
	}
	if (key_update_request > 1) {
		alert = TLS13_ALERT_ILLEGAL_PARAMETER;
		goto err;
	}

	if (!tls13_phh_update_peer_traffic_secret(ctx))
		goto err;

	if (key_update_request) {
		CBB cbb;
		CBS cbs; /* XXX */

		tls13_handshake_msg_free(ctx->hs_msg);
		ctx->hs_msg = tls13_handshake_msg_new();

		if (!tls13_handshake_msg_start(ctx->hs_msg, &cbb, TLS13_MT_KEY_UPDATE))
			goto err;
		if (!CBB_add_u8(&cbb, 0))
			goto err;
		if (!tls13_handshake_msg_finish(ctx->hs_msg))
			goto err;

		ctx->key_update_request = key_update_request;
		tls13_handshake_msg_data(ctx->hs_msg, &cbs);
		ret = tls13_record_layer_phh(ctx->rl, &cbs);

		tls13_handshake_msg_free(ctx->hs_msg);
		ctx->hs_msg = NULL;
	} else
		ret = TLS13_IO_SUCCESS;

	return ret;
 err:
	return tls13_send_alert(ctx->rl, alert);
}

static void
tls13_phh_done_cb(void *cb_arg)
{
	struct tls13_ctx *ctx = cb_arg;

	if (ctx->key_update_request) {
		tls13_phh_update_local_traffic_secret(ctx);
		ctx->key_update_request = 0;
	}
}

static ssize_t
tls13_phh_received_cb(void *cb_arg, CBS *cbs)
{
	ssize_t ret = TLS13_IO_FAILURE;
	struct tls13_ctx *ctx = cb_arg;
	CBS phh_cbs;

	if (!tls13_phh_limit_check(ctx))
		return tls13_send_alert(ctx->rl, TLS13_ALERT_UNEXPECTED_MESSAGE);

	if ((ctx->hs_msg == NULL) &&
	    ((ctx->hs_msg = tls13_handshake_msg_new()) == NULL))
		return TLS13_IO_FAILURE;

	if (!tls13_handshake_msg_set_buffer(ctx->hs_msg, cbs))
		return TLS13_IO_FAILURE;

	if ((ret = tls13_handshake_msg_recv(ctx->hs_msg, ctx->rl))
	    != TLS13_IO_SUCCESS)
		return ret;

	if (!tls13_handshake_msg_content(ctx->hs_msg, &phh_cbs))
		return TLS13_IO_FAILURE;

	switch(tls13_handshake_msg_type(ctx->hs_msg)) {
	case TLS13_MT_KEY_UPDATE:
		ret = tls13_key_update_recv(ctx, &phh_cbs);
		break;
	case TLS13_MT_NEW_SESSION_TICKET:
		/* XXX do nothing for now and ignore this */
		break;
	case TLS13_MT_CERTIFICATE_REQUEST:
		/* XXX add support if we choose to advertise this */
		/* FALLTHROUGH */
	default:
		ret = TLS13_IO_FAILURE; /* XXX send alert */
		break;
	}

	tls13_handshake_msg_free(ctx->hs_msg);
	ctx->hs_msg = NULL;
	return ret;
}

static const struct tls13_record_layer_callbacks rl_callbacks = {
	.wire_read = tls13_legacy_wire_read_cb,
	.wire_write = tls13_legacy_wire_write_cb,
	.alert_recv = tls13_alert_received_cb,
	.alert_sent = tls13_alert_sent_cb,
	.phh_recv = tls13_phh_received_cb,
	.phh_sent = tls13_phh_done_cb,
};

struct tls13_ctx *
tls13_ctx_new(int mode)
{
	struct tls13_ctx *ctx = NULL;

	if ((ctx = calloc(sizeof(struct tls13_ctx), 1)) == NULL)
		goto err;

	ctx->mode = mode;

	if ((ctx->rl = tls13_record_layer_new(&rl_callbacks, ctx)) == NULL)
		goto err;

	ctx->handshake_message_sent_cb = tls13_legacy_handshake_message_sent_cb;
	ctx->handshake_message_recv_cb = tls13_legacy_handshake_message_recv_cb;
	ctx->ocsp_status_recv_cb = tls13_legacy_ocsp_status_recv_cb;

	ctx->middlebox_compat = 1;

	return ctx;

 err:
	tls13_ctx_free(ctx);

	return NULL;
}

void
tls13_ctx_free(struct tls13_ctx *ctx)
{
	if (ctx == NULL)
		return;

	tls13_error_clear(&ctx->error);
	tls13_record_layer_free(ctx->rl);
	tls13_handshake_msg_free(ctx->hs_msg);

	freezero(ctx, sizeof(struct tls13_ctx));
}

int
tls13_cert_add(CBB *cbb, X509 *cert)
{
	CBB cert_data, cert_exts;
	uint8_t *data;
	int cert_len;

	if ((cert_len = i2d_X509(cert, NULL)) < 0)
		return 0;

	if (!CBB_add_u24_length_prefixed(cbb, &cert_data))
		return 0;
	if (!CBB_add_space(&cert_data, &data, cert_len))
		return 0;
	if (i2d_X509(cert, &data) != cert_len)
		return 0;

	if (!CBB_add_u16_length_prefixed(cbb, &cert_exts))
		return 0;

	if (!CBB_flush(cbb))
		return 0;

	return 1;
}

int
tls13_synthetic_handshake_message(struct tls13_ctx *ctx)
{
	struct tls13_handshake_msg *hm = NULL;
	unsigned char buf[EVP_MAX_MD_SIZE];
	size_t hash_len;
	CBB cbb;
	CBS cbs;
	SSL *s = ctx->ssl;
	int ret = 0;

	/*
	 * Replace ClientHello with synthetic handshake message - see
	 * RFC 8446 section 4.4.1.
	 */
	if (!tls1_transcript_hash_init(s))
		goto err;
	if (!tls1_transcript_hash_value(s, buf, sizeof(buf), &hash_len))
		goto err;

	if ((hm = tls13_handshake_msg_new()) == NULL)
		goto err;
	if (!tls13_handshake_msg_start(hm, &cbb, TLS13_MT_MESSAGE_HASH))
		goto err;
	if (!CBB_add_bytes(&cbb, buf, hash_len))
		goto err;
	if (!tls13_handshake_msg_finish(hm))
		goto err;

	tls13_handshake_msg_data(hm, &cbs);

	tls1_transcript_reset(ctx->ssl);
	if (!tls1_transcript_record(ctx->ssl, CBS_data(&cbs), CBS_len(&cbs)))
		goto err;

	ret = 1;

 err:
	tls13_handshake_msg_free(hm);

	return ret;
}

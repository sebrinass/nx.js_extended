#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "address.h"
#include "config.h"
#include "dtls_srtp.h"
#if CONFIG_MBEDTLS_DEBUG
#include "mbedtls/debug.h"
#endif
#include "mbedtls/sha256.h"
#include "mbedtls/ssl.h"
#include "ports.h"
#include "socket.h"
#include "utils.h"

// How often dtls_srtp_write() retries a mbedTLS "would block" answer before
// it gives up and reports the error.  The retry used to be unbounded.
#define DTLS_SRTP_WRITE_MAX_RETRIES 8

// 0040: the DTLS handshake retransmission schedule.
//
// RFC 6347 section 4.2.4.1 allows 1 s .. 60 s.  1000/4000 means: the first
// flight is repeated after 1 s, the next after 2 s (3 s in total), the next
// after 4 s (7 s in total) and then mbedTLS gives up with
// MBEDTLS_ERR_SSL_TIMEOUT.
//
// WHY 4 s and not the mbedTLS default of 60 s: the whole ICE round has a
// 14 s budget (0039) and the DTLS endpoint timeout of peer_connection.c is
// 9 s.  A handshake that keeps retransmitting for a minute would outlive
// both of them and the session would be torn down long before the
// retransmissions could help.
//
// WHY it has to be set at all: without it mbedTLS uses the default
// (1000/60000), which never gives up in time for our budgets.
#define DTLS_SRTP_HANDSHAKE_TIMEOUT_MIN_MS 1000
#define DTLS_SRTP_HANDSHAKE_TIMEOUT_MAX_MS 4000

// 0040: the MTU of the DTLS records we send.
//
// 1200 bytes fits into a 1500 byte path without IP fragmentation and also
// survives the smaller paths (PPPoE / CGNAT tunnels) that a 1500 byte
// record would be fragmented into -- and a fragmented UDP datagram that
// loses ONE fragment is lost completely.
//
// mbedTLS limits the OUTGOING record payload to `mtu - overhead`
// (mbedtls_ssl_get_max_out_record_payload()).  Note that the peer's own
// flight (its Certificate message) is fragmented by the PEER, with the
// peer's own MTU; we cannot influence that.
#define DTLS_SRTP_MTU 1200

// 0040: the timer context of the DTLS handshake.
//
// It used to be a function local static of dtls_srtp_do_handshake(), which
// re-registered the timer callback on EVERY handshake step.  That is
// harmless looking, but mbedtls_ssl_set_timer_cb() ends with
// mbedtls_ssl_set_timer(ssl, 0) -- it CANCELS the running timer.  The
// DTLS retransmission timer was therefore cancelled ~30 times per second
// (peer_connection_loop() runs once per rendered frame) and the handshake
// flight was NEVER retransmitted: a single lost ClientHello killed the
// handshake until peer_connection.c gave the endpoint up after 3.5 s and
// restarted ICE.
//
// The timer context lives here, at file scope, and the callback is
// registered ONCE (in dtls_srtp_init()).
static mbedtls_timing_delay_context g_dtls_srtp_timer;

int dtls_srtp_udp_send(void* ctx, const uint8_t* buf, size_t len) {
  DtlsSrtp* dtls_srtp = (DtlsSrtp*)ctx;
  UdpSocket* udp_socket = (UdpSocket*)dtls_srtp->user_data;

  int ret = udp_socket_sendto(udp_socket, dtls_srtp->remote_addr, buf, len);

  LOGD("dtls_srtp_udp_send (%d)", ret);

  return ret;
}

// 0040: the DEFAULT bio read.
//
// It must not block and it must not return 0:
//   * 0 means "the peer closed the connection" to mbedTLS
//     (MBEDTLS_ERR_SSL_CONN_EOF == -0x7280), and
//   * a blocking read stalls the whole peer connection loop -- and with it
//     the ICE retransmission timer, the consent freshness timer and the
//     media pump.
//
// So: no datagram -> MBEDTLS_ERR_SSL_WANT_READ ("come back later"), which
// is what mbedTLS expects from a non blocking bio.
//
// NOTE: peer_connection.c replaces this callback with
// peer_connection_dtls_srtp_recv(), which is non blocking as well.  This
// one is the fallback for every other user of dtls_srtp_init().
int dtls_srtp_udp_recv(void* ctx, uint8_t* buf, size_t len) {
  DtlsSrtp* dtls_srtp = (DtlsSrtp*)ctx;
  UdpSocket* udp_socket = (UdpSocket*)dtls_srtp->user_data;
  Address from;
  int ret;

  memset(&from, 0, sizeof(from));
  ret = udp_socket_recvfrom_nonblocking(udp_socket, &from, buf, len);
  if (ret <= 0) {
    // Nothing there (or a read error): "would block", NOT "eof".
    return MBEDTLS_ERR_SSL_WANT_READ;
  }

  LOGD("dtls_srtp_udp_recv (%d)", ret);

  return ret;
}

// 0040: the bio read wrapper -- the ONE place where the read path is
// measured.
//
// Every read of the DTLS transport goes through here, no matter which
// callback is installed, so the counters below are complete:
//
//   bio_read_calls   every call
//   bio_want_reads   the calls that found no data
//   bio_eof_returns  the calls that returned 0  (MUST STAY 0)
//   bio_max_call_us  the worst single call
//
// ★ These are the CRITERIA of this patch: they are numbers that CHANGE when
//   the read path breaks.  bio_eof_returns in particular goes from 0 to 1
//   the moment somebody returns 0 from the bio -- which is the defect that
//   made the other stack report "stuck on negotiating connecting" with
//   -0x7280.
static int dtls_srtp_bio_recv(void* ctx, unsigned char* buf, size_t len) {
  DtlsSrtp* dtls_srtp = (DtlsSrtp*)ctx;
  const uint64_t started_us = ports_get_monotonic_time_us();
  int ret = dtls_srtp->udp_recv(ctx, buf, len);
  const uint64_t elapsed_us = ports_get_monotonic_time_us() - started_us;

  dtls_srtp->bio_read_calls++;
  if (ret == 0) {
    dtls_srtp->bio_eof_returns++;
    LOGE("the DTLS bio returned 0 -- mbedTLS will read that as EOF (-0x7280)");
  } else if (ret < 0) {
    dtls_srtp->bio_want_reads++;
  }
  if (elapsed_us > dtls_srtp->bio_max_call_us) {
    dtls_srtp->bio_max_call_us = (uint32_t)elapsed_us;
  }

  return ret;
}

// 0006: the SHA-256 digest of the certificate as RAW BYTES.
//
// libwebrtc compares the digest BYTES (SSLFingerprint::operator== compares
// the digest, not the printed string), so the digest is needed in this form
// as well: the printable form is only good for the log and for the SDP.
static void dtls_srtp_x509_digest_bytes(const mbedtls_x509_crt* crt, unsigned char digest[32]) {
  mbedtls_sha256_context sha256_ctx;

  mbedtls_sha256_init(&sha256_ctx);
  mbedtls_sha256_starts(&sha256_ctx, 0);
  mbedtls_sha256_update(&sha256_ctx, crt->raw.p, crt->raw.len);
  mbedtls_sha256_finish(&sha256_ctx, digest);
  mbedtls_sha256_free(&sha256_ctx);
}

static void dtls_srtp_x509_digest(const mbedtls_x509_crt* crt, char* buf) {
  int i;
  unsigned char digest[32];

  dtls_srtp_x509_digest_bytes(crt, digest);

  for (i = 0; i < 32; i++) {
    snprintf(buf, 4, "%.2X:", digest[i]);
    buf += 3;
  }

  *(--buf) = '\0';
}

// Do not verify CA
//
// 0006: the official native client (BoringSSL with SSL_VERIFY_PEER and its
// own verify callback) does NOT check the CA chain and it does NOT check the
// validity period -- it only checks the fingerprint digest that the SDP
// announced.  The clock of the Switch is not always in sync with the clock
// of the cloud, so a certificate that is not valid YET (or not valid ANY
// MORE) must not break the handshake.
//
// 0007: the verification is switched off COMPLETELY now
// (MBEDTLS_SSL_VERIFY_NONE, see dtls_srtp_init()) instead of clearing a
// fixed list of flags.
//
//   * WHY not "clear N flags": the list is open.  0006 cleared
//     NOT_TRUSTED / CN_MISMATCH / BAD_KEY / EXPIRED / FUTURE, and
//     MBEDTLS_X509_BADCERT_BAD_PURPOSE / BAD_KEY_USAGE /
//     UNEXPECTED_CERT ... are just as possible on a certificate that the
//     cloud happens to use.  A flag that is missed breaks the handshake.
//     mbedTLS 3.4.0 can also fail the verification with a hard error that
//     a flag clearing callback cannot fix at all.
//
//   * ★ The certificate is still authenticated -- by the FINGERPRINT.  The
//     fingerprint comparison stays FAIL HARD: a mismatch or a fingerprint
//     that cannot be parsed aborts the handshake (dtls_srtp_handshake()
//     returns -1).  "The X.509 chain is not verified" must NOT become
//     "everything is accepted" -- that would open the door for a man in
//     the middle.  This is the difference between the model of the official
//     native client (no CA chain, no validity period, only the SDP
//     fingerprint) and "no authentication at all".
//
// ★ This callback is NOT called any more: with MBEDTLS_SSL_VERIFY_NONE
//   mbedtls_ssl_parse_certificate() -> ssl_parse_certificate_verify()
//   returns 0 BEFORE it calls f_vrfy (third_party/mbedtls/library/
//   ssl_tls.c).  That is safe: the callback only ever cleared flags and did
//   nothing else.  It is kept and stays registered as a safety net in case
//   the auth mode is ever switched back to MBEDTLS_SSL_VERIFY_REQUIRED.
//
// ★ The peer certificate is still there for the fingerprint check: the
//   chain is parsed and handed to the session
//   (ssl->session_negotiate->peer_cert) BEFORE the verification step, and
//   MBEDTLS_SSL_KEEP_PEER_CERTIFICATE is enabled, so
//   mbedtls_ssl_get_peer_cert() returns the certificate in either auth
//   mode.  The fingerprint comparison is therefore still the guard.
static int dtls_srtp_cert_verify(void* data, mbedtls_x509_crt* crt, int depth, uint32_t* flags) {
  *flags &= ~(MBEDTLS_X509_BADCERT_NOT_TRUSTED | MBEDTLS_X509_BADCERT_CN_MISMATCH | MBEDTLS_X509_BADCERT_BAD_KEY);
#ifdef MBEDTLS_X509_BADCERT_EXPIRED
  *flags &= ~(uint32_t)MBEDTLS_X509_BADCERT_EXPIRED;
#endif
#ifdef MBEDTLS_X509_BADCERT_FUTURE
  *flags &= ~(uint32_t)MBEDTLS_X509_BADCERT_FUTURE;
#endif
  return 0;
}

static int dtls_srtp_selfsign_cert(DtlsSrtp* dtls_srtp) {
  int ret;

  mbedtls_x509write_cert crt;

  unsigned char* cert_buf = NULL;
#if CONFIG_MBEDTLS_2_X
  mbedtls_mpi serial;
#else
  const char* serial = "peer";
#endif
  const char* pers = "dtls_srtp";

  // 0037: the buffer is sized explicitly, not after RSA_KEY_LENGTH -- the
  // certificate is an EC key now, so the RSA key length says nothing about
  // how much room the PEM needs.
  cert_buf = (unsigned char*)malloc(CONFIG_DTLS_CERT_BUFFER_SIZE);
  if (cert_buf == NULL) {
    LOGE("malloc failed");
    return -1;
  }

  mbedtls_ctr_drbg_seed(&dtls_srtp->ctr_drbg, mbedtls_entropy_func, &dtls_srtp->entropy, (const unsigned char*)pers, strlen(pers));

#if CONFIG_DTLS_USE_ECDSA
  // 0037: ECDSA P-256 (SECP256R1) -- the same key type the official native
  // client uses (BoringSSL, self signed).  A 1024 bit RSA certificate is
  // refused by every modern TLS stack by default.
  ret = mbedtls_pk_setup(&dtls_srtp->pkey, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
  if (ret != 0) {
    LOGE("mbedtls_pk_setup(ECKEY) failed -0x%.4x", (unsigned int)-ret);
  }
  ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(dtls_srtp->pkey), mbedtls_ctr_drbg_random, &dtls_srtp->ctr_drbg);
  if (ret != 0) {
    LOGE("mbedtls_ecp_gen_key failed -0x%.4x", (unsigned int)-ret);
  } else {
    LOGI("DTLS certificate: ECDSA P-256 (SECP256R1)");
  }
#else
  mbedtls_pk_setup(&dtls_srtp->pkey, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
  mbedtls_rsa_gen_key(mbedtls_pk_rsa(dtls_srtp->pkey), mbedtls_ctr_drbg_random, &dtls_srtp->ctr_drbg, RSA_KEY_LENGTH, 65537);
#endif

  mbedtls_x509write_crt_init(&crt);

  mbedtls_x509write_crt_set_subject_key(&crt, &dtls_srtp->pkey);

  mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);

  mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);

  mbedtls_x509write_crt_set_subject_key(&crt, &dtls_srtp->pkey);

  mbedtls_x509write_crt_set_issuer_key(&crt, &dtls_srtp->pkey);

  mbedtls_x509write_crt_set_subject_name(&crt, "CN=dtls_srtp");

  mbedtls_x509write_crt_set_issuer_name(&crt, "CN=dtls_srtp");

#if CONFIG_MBEDTLS_2_X
  mbedtls_mpi_init(&serial);
  mbedtls_mpi_fill_random(&serial, 16, mbedtls_ctr_drbg_random, &dtls_srtp->ctr_drbg);
  ret = mbedtls_x509write_crt_set_serial(&crt, &serial);
  if (ret < 0) {
    LOGE("mbedtls_x509write_crt_set_serial failed -0x%.4x", (unsigned int)-ret);
  }
#else
  mbedtls_x509write_crt_set_serial_raw(&crt, (unsigned char*)serial, strlen(serial));
#endif

  mbedtls_x509write_crt_set_validity(&crt, "20180101000000", "20280101000000");

  ret = mbedtls_x509write_crt_pem(&crt, cert_buf, CONFIG_DTLS_CERT_BUFFER_SIZE, mbedtls_ctr_drbg_random, &dtls_srtp->ctr_drbg);

  if (ret < 0) {
    LOGE("mbedtls_x509write_crt_pem failed -0x%.4x", (unsigned int)-ret);
  }

  mbedtls_x509_crt_parse(&dtls_srtp->cert, cert_buf, CONFIG_DTLS_CERT_BUFFER_SIZE);

  mbedtls_x509write_crt_free(&crt);

  free(cert_buf);

  return ret;
}

#if CONFIG_MBEDTLS_DEBUG
static void dtls_srtp_debug(void* ctx, int level, const char* file, int line, const char* str) {
  LOGD("%s:%04d: %s", file, line, str);
}
#endif

int dtls_srtp_init(DtlsSrtp* dtls_srtp, DtlsSrtpRole role, void* user_data) {
  static const mbedtls_ssl_srtp_profile default_profiles[] = {
      MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80,
      MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_32,
      MBEDTLS_TLS_SRTP_NULL_HMAC_SHA1_80,
      MBEDTLS_TLS_SRTP_NULL_HMAC_SHA1_32,
      MBEDTLS_TLS_SRTP_UNSET};

  dtls_srtp->role = role;
  dtls_srtp->state = DTLS_SRTP_STATE_INIT;
  dtls_srtp->user_data = user_data;
  dtls_srtp->udp_send = dtls_srtp_udp_send;
  dtls_srtp->udp_recv = dtls_srtp_udp_recv;

  // 0035: the diagnostics start out "unknown", not "ok".
  dtls_srtp->last_mbedtls_ret = 0;
  dtls_srtp->fingerprint_check = 0;
  dtls_srtp->handshake_active = 0;
  dtls_srtp->last_write_is_handshake = 0;
  dtls_srtp->remote_fingerprint_alg[0] = '\0';
  // 0040: the read path counters start at zero.  bio_eof_returns has to
  // STAY at zero -- it is the criterion that turns red when the read path
  // starts returning 0 (which mbedTLS reads as EOF, -0x7280).
  dtls_srtp->bio_read_calls = 0;
  dtls_srtp->bio_want_reads = 0;
  dtls_srtp->bio_eof_returns = 0;
  dtls_srtp->bio_max_call_us = 0;

  mbedtls_ssl_config_init(&dtls_srtp->conf);
  mbedtls_ssl_init(&dtls_srtp->ssl);

  mbedtls_x509_crt_init(&dtls_srtp->cert);
  mbedtls_pk_init(&dtls_srtp->pkey);
  mbedtls_entropy_init(&dtls_srtp->entropy);
  mbedtls_ctr_drbg_init(&dtls_srtp->ctr_drbg);
#if CONFIG_MBEDTLS_DEBUG
  mbedtls_debug_set_threshold(3);
  mbedtls_ssl_conf_dbg(&dtls_srtp->conf, dtls_srtp_debug, NULL);
#endif
  dtls_srtp_selfsign_cert(dtls_srtp);

  mbedtls_ssl_conf_verify(&dtls_srtp->conf, dtls_srtp_cert_verify, NULL);

  // 0007: no X.509 verification at all -- the fingerprint that the SDP
  // announced is the only guard, exactly like the official native client
  // (BoringSSL + SSL_VERIFY_PEER + a verify callback of its own that only
  // checks the fingerprint digest).
  //
  // ★ The fingerprint comparison stays FAIL HARD: a mismatch or a
  //   fingerprint that cannot be parsed aborts the handshake, so this is
  //   "the CA chain and the validity period are not checked", NOT
  //   "everything is accepted".  See the comment above
  //   dtls_srtp_cert_verify().
  mbedtls_ssl_conf_authmode(&dtls_srtp->conf, MBEDTLS_SSL_VERIFY_NONE);

  mbedtls_ssl_conf_ca_chain(&dtls_srtp->conf, &dtls_srtp->cert, NULL);

  mbedtls_ssl_conf_own_cert(&dtls_srtp->conf, &dtls_srtp->cert, &dtls_srtp->pkey);

  mbedtls_ssl_conf_rng(&dtls_srtp->conf, mbedtls_ctr_drbg_random, &dtls_srtp->ctr_drbg);

  mbedtls_ssl_conf_read_timeout(&dtls_srtp->conf, 1000);

  // 0040: the DTLS handshake retransmission schedule.
  //
  // mbedTLS defaults to 1000/60000 (RFC 6347's 1 s .. 60 s).  60 s is far
  // longer than our ICE round (14 s) and our DTLS endpoint timeout (9 s),
  // so the handshake would be torn down long before mbedTLS gave up.
  // 1000/4000 repeats the flight after 1 s, 3 s and 7 s and then gives up.
  mbedtls_ssl_conf_handshake_timeout(&dtls_srtp->conf,
                                     DTLS_SRTP_HANDSHAKE_TIMEOUT_MIN_MS,
                                     DTLS_SRTP_HANDSHAKE_TIMEOUT_MAX_MS);

  if (dtls_srtp->role == DTLS_SRTP_ROLE_SERVER) {
    mbedtls_ssl_config_defaults(&dtls_srtp->conf,
                                MBEDTLS_SSL_IS_SERVER,
                                MBEDTLS_SSL_TRANSPORT_DATAGRAM,
                                MBEDTLS_SSL_PRESET_DEFAULT);

    mbedtls_ssl_cookie_init(&dtls_srtp->cookie_ctx);

    mbedtls_ssl_cookie_setup(&dtls_srtp->cookie_ctx, mbedtls_ctr_drbg_random, &dtls_srtp->ctr_drbg);

    mbedtls_ssl_conf_dtls_cookies(&dtls_srtp->conf, mbedtls_ssl_cookie_write, mbedtls_ssl_cookie_check, &dtls_srtp->cookie_ctx);

  } else {
    mbedtls_ssl_config_defaults(&dtls_srtp->conf,
                                MBEDTLS_SSL_IS_CLIENT,
                                MBEDTLS_SSL_TRANSPORT_DATAGRAM,
                                MBEDTLS_SSL_PRESET_DEFAULT);
  }

  dtls_srtp_x509_digest(&dtls_srtp->cert, dtls_srtp->local_fingerprint);

  LOGD("local fingerprint: %s", dtls_srtp->local_fingerprint);

  mbedtls_ssl_conf_dtls_srtp_protection_profiles(&dtls_srtp->conf, default_profiles);

  mbedtls_ssl_conf_srtp_mki_value_supported(&dtls_srtp->conf, MBEDTLS_SSL_DTLS_SRTP_MKI_UNSUPPORTED);

  mbedtls_ssl_conf_cert_req_ca_list(&dtls_srtp->conf, MBEDTLS_SSL_CERT_REQ_CA_LIST_DISABLED);

  mbedtls_ssl_setup(&dtls_srtp->ssl, &dtls_srtp->conf);
  dtls_srtp->initialized = 1;

  // 0040: the DTLS timer callback is registered ONCE, here.
  //
  // It used to be registered on every handshake step (dtls_srtp_do_handshake
  // ran once per rendered frame).  mbedtls_ssl_set_timer_cb() ends with
  // mbedtls_ssl_set_timer(ssl, 0) -- it CANCELS the running timer -- so the
  // DTLS retransmission timer was cancelled ~30 times per second and the
  // handshake flight was never retransmitted.  A single lost ClientHello
  // therefore killed the whole handshake.
  //
  // mbedtls_ssl_session_reset() keeps "all application-set variables,
  // function pointers and data", so the callback survives the session reset
  // that the retry path does.
  mbedtls_ssl_set_timer_cb(&dtls_srtp->ssl, &g_dtls_srtp_timer,
                            mbedtls_timing_set_delay, mbedtls_timing_get_delay);

  // 0040: cap the size of the records we send.
  mbedtls_ssl_set_mtu(&dtls_srtp->ssl, DTLS_SRTP_MTU);

  return 0;
}

void dtls_srtp_deinit(DtlsSrtp* dtls_srtp) {
  mbedtls_ssl_free(&dtls_srtp->ssl);
  mbedtls_ssl_config_free(&dtls_srtp->conf);

  mbedtls_x509_crt_free(&dtls_srtp->cert);
  mbedtls_pk_free(&dtls_srtp->pkey);
  mbedtls_entropy_free(&dtls_srtp->entropy);
  mbedtls_ctr_drbg_free(&dtls_srtp->ctr_drbg);

  if (dtls_srtp->role == DTLS_SRTP_ROLE_SERVER) {
    mbedtls_ssl_cookie_free(&dtls_srtp->cookie_ctx);
  }

  if (dtls_srtp->state == DTLS_SRTP_STATE_CONNECTED) {
    srtp_dealloc(dtls_srtp->srtp_in);
    srtp_dealloc(dtls_srtp->srtp_out);
  }
  dtls_srtp->initialized = 0;
}

static int dtls_srtp_key_derivation(DtlsSrtp* dtls_srtp, const unsigned char* master_secret, size_t secret_len, const unsigned char* randbytes, size_t randbytes_len, mbedtls_tls_prf_types tls_prf_type) {
  int ret;
  const char* dtls_srtp_label = "EXTRACTOR-dtls_srtp";
  uint8_t key_material[DTLS_SRTP_KEY_MATERIAL_LENGTH];
  // Export keying material
  if ((ret = mbedtls_ssl_tls_prf(tls_prf_type, master_secret, secret_len, dtls_srtp_label,
                                 randbytes, randbytes_len, key_material, sizeof(key_material))) != 0) {
    LOGE("mbedtls_ssl_tls_prf failed(%d)", ret);
    return ret;
  }

  const uint8_t* client_key = key_material;
  const uint8_t* server_key = client_key + SRTP_MASTER_KEY_LENGTH;
  const uint8_t* client_salt = server_key + SRTP_MASTER_KEY_LENGTH;
  const uint8_t* server_salt = client_salt + SRTP_MASTER_SALT_LENGTH;
  const uint8_t *local_key, *remote_key, *local_salt, *remote_salt;
  if (dtls_srtp->role == DTLS_SRTP_ROLE_SERVER) {
    local_key = server_key;
    local_salt = server_salt;
    remote_key = client_key;
    remote_salt = client_salt;
  } else {
    local_key = client_key;
    local_salt = client_salt;
    remote_key = server_key;
    remote_salt = server_salt;
  }
  // derive inbounds keys

  memset(&dtls_srtp->remote_policy, 0, sizeof(dtls_srtp->remote_policy));

  srtp_crypto_policy_set_rtp_default(&dtls_srtp->remote_policy.rtp);
  srtp_crypto_policy_set_rtcp_default(&dtls_srtp->remote_policy.rtcp);

  memcpy(dtls_srtp->remote_policy_key, remote_key, SRTP_MASTER_KEY_LENGTH);
  memcpy(dtls_srtp->remote_policy_key + SRTP_MASTER_KEY_LENGTH, remote_salt, SRTP_MASTER_SALT_LENGTH);

  dtls_srtp->remote_policy.ssrc.type = ssrc_any_inbound;
  dtls_srtp->remote_policy.key = dtls_srtp->remote_policy_key;
  dtls_srtp->remote_policy.next = NULL;

  if (srtp_create(&dtls_srtp->srtp_in, &dtls_srtp->remote_policy) != srtp_err_status_ok) {
    LOGD("Error creating inbound SRTP session for component");
    return -1;
  }

  LOGI("Created inbound SRTP session");

  // derive outbounds keys
  memset(&dtls_srtp->local_policy, 0, sizeof(dtls_srtp->local_policy));

  srtp_crypto_policy_set_rtp_default(&dtls_srtp->local_policy.rtp);
  srtp_crypto_policy_set_rtcp_default(&dtls_srtp->local_policy.rtcp);

  memcpy(dtls_srtp->local_policy_key, local_key, SRTP_MASTER_KEY_LENGTH);
  memcpy(dtls_srtp->local_policy_key + SRTP_MASTER_KEY_LENGTH, local_salt, SRTP_MASTER_SALT_LENGTH);

  dtls_srtp->local_policy.ssrc.type = ssrc_any_outbound;
  dtls_srtp->local_policy.key = dtls_srtp->local_policy_key;
  dtls_srtp->local_policy.next = NULL;

  if (srtp_create(&dtls_srtp->srtp_out, &dtls_srtp->local_policy) != srtp_err_status_ok) {
    LOGE("Error creating outbound SRTP session");
    return -1;
  }

  LOGI("Created outbound SRTP session");
  dtls_srtp->state = DTLS_SRTP_STATE_CONNECTED;
  return 0;
}

#if CONFIG_MBEDTLS_2_X
static int dtls_srtp_key_derivation_cb(void* context,
                                       const unsigned char* ms,
                                       const unsigned char* kb,
                                       size_t maclen,
                                       size_t keylen,
                                       size_t ivlen,
                                       const unsigned char client_random[32],
                                       const unsigned char server_random[32],
                                       mbedtls_tls_prf_types tls_prf_type) {
#else
static void dtls_srtp_key_derivation_cb(void* context,
                                        mbedtls_ssl_key_export_type secret_type,
                                        const unsigned char* secret,
                                        size_t secret_len,
                                        const unsigned char client_random[32],
                                        const unsigned char server_random[32],
                                        mbedtls_tls_prf_types tls_prf_type) {
#endif
  DtlsSrtp* dtls_srtp = (DtlsSrtp*)context;

  unsigned char master_secret[48];
  unsigned char randbytes[64];

  memcpy(randbytes, client_random, 32);
  memcpy(randbytes + 32, server_random, 32);

#if CONFIG_MBEDTLS_2_X
  memcpy(master_secret, ms, sizeof(master_secret));
  return dtls_srtp_key_derivation(dtls_srtp, master_secret, sizeof(master_secret), randbytes, sizeof(randbytes), tls_prf_type);
#else
  memcpy(master_secret, secret, sizeof(master_secret));
  dtls_srtp_key_derivation(dtls_srtp, master_secret, sizeof(master_secret), randbytes, sizeof(randbytes), tls_prf_type);
#endif
}

static int dtls_srtp_do_handshake(DtlsSrtp* dtls_srtp) {
  int ret;

  // 0040: the timer callback is NOT registered here any more.
  //
  // It used to be, on every single handshake step.  That silently cancelled
  // the DTLS retransmission timer (mbedtls_ssl_set_timer_cb() calls
  // mbedtls_ssl_set_timer(ssl, 0)), so the handshake flight was never
  // repeated and a single lost ClientHello killed the handshake.
  // It is registered once in dtls_srtp_init() now.

#if CONFIG_MBEDTLS_2_X
  mbedtls_ssl_conf_export_keys_ext_cb(&dtls_srtp->conf, dtls_srtp_key_derivation_cb, dtls_srtp);
#else
  mbedtls_ssl_set_export_keys_cb(&dtls_srtp->ssl, dtls_srtp_key_derivation_cb, dtls_srtp);
#endif

  // 0040: the read path goes through the measuring wrapper.
  //
  // The wrapper counts every read, how many of them found no data, how many
  // of them (wrongly) returned 0 and how long the worst one took.  The
  // counters are the criteria of this patch: bio_eof_returns must stay 0
  // and bio_max_call_us must stay in the sub millisecond range.
  mbedtls_ssl_set_bio(&dtls_srtp->ssl, dtls_srtp, dtls_srtp->udp_send,
                       dtls_srtp_bio_recv, NULL);

  // 0040: re-assert the MTU before every step.
  //
  // mbedTLS overwrites ssl->mtu with the value the PEER announced in its
  // ClientHello (ssl_parse_client_hello).  We are the DTLS client, so the
  // peer never sends a ClientHello and the value cannot be overwritten --
  // but re-asserting it costs one store and makes the behaviour independent
  // of the role.
  mbedtls_ssl_set_mtu(&dtls_srtp->ssl, DTLS_SRTP_MTU);

  // Do exactly one non-blocking handshake step. The peer connection loop calls
  // us again after polling the UDP socket, preserving MbedTLS' DTLS state.
  //
  // 0035: remember the raw return code.  It is the only thing that says WHY
  // the handshake stalled (WANT_READ = "the peer did not answer", a negative
  // SSL code = "the peer said no").
  dtls_srtp->handshake_active = 1;
  ret = mbedtls_ssl_handshake(&dtls_srtp->ssl);
  dtls_srtp->handshake_active = 0;
  dtls_srtp->last_mbedtls_ret = ret;
  return ret;
}

static int dtls_srtp_handshake_server(DtlsSrtp* dtls_srtp) {
  int ret;

  while (1) {
    unsigned char client_ip[] = "test";

    mbedtls_ssl_session_reset(&dtls_srtp->ssl);

    mbedtls_ssl_set_client_transport_id(&dtls_srtp->ssl, client_ip, sizeof(client_ip));

    ret = dtls_srtp_do_handshake(dtls_srtp);

    if (ret == MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED) {
      LOGD("DTLS hello verification requested");

    } else if (ret != 0) {
      LOGE("failed! mbedtls_ssl_handshake returned -0x%.4x", (unsigned int)-ret);

      break;

    } else {
      break;
    }
  }

  LOGD("DTLS server handshake done");

  return ret;
}

static int dtls_srtp_handshake_client(DtlsSrtp* dtls_srtp) {
  int ret;

  ret = dtls_srtp_do_handshake(dtls_srtp);
  if (ret != 0 && ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
    LOGE("failed! mbedtls_ssl_handshake returned -0x%.4x\n\n", (unsigned int)-ret);
  }

  if (ret == 0)
    LOGD("DTLS client handshake done");

  return ret;
}

// 0035: the fingerprint is a PUBLIC negotiated value (it is in the SDP), but
// the log still should not carry the whole thing around: the algorithm, the
// length and the first 8 characters are enough to tell two fingerprints
// apart.  The length is bounded because remote_fingerprint is filled with
// strncpy() and is NOT guaranteed to be NUL terminated.
static int dtls_srtp_bounded_len(const char* str, int max_len) {
  int len = 0;
  if (!str) {
    return 0;
  }
  while (len < max_len && str[len] != '\0') {
    len++;
  }
  return len;
}

static void dtls_srtp_fingerprint_summary(const char* fingerprint, char* out, size_t out_len) {
  int len;
  int i;

  if (!out || out_len == 0) {
    return;
  }
  out[0] = '\0';
  if (!fingerprint) {
    return;
  }
  len = dtls_srtp_bounded_len(fingerprint, DTLS_SRTP_FINGERPRINT_LENGTH);
  for (i = 0; i < len && i < 8 && (size_t)(i + 1) < out_len; ++i) {
    out[i] = fingerprint[i];
  }
  out[i] = '\0';
}

// 0006: is this a hex digit?
static int dtls_srtp_is_hex_digit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// 0006: the value of a hex digit.
static unsigned char dtls_srtp_hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return (unsigned char)(c - '0');
  }
  if (c >= 'a' && c <= 'f') {
    return (unsigned char)(c - 'a' + 10);
  }
  return (unsigned char)(c - 'A' + 10);
}

// 0006: the hex value of a fingerprint ("AB:CD:EF:...") as RAW BYTES.
//
// The peer may print the fingerprint in upper case (that is what libwebrtc
// does -- SSLFingerprint::GetRfc4572Fingerprint() hex encodes the digest
// and upper cases it) or in lower case, and it may or may not use ":" as the
// separator.  The comparison therefore happens on the BYTES and never on the
// strings.
//
// The value may also carry its algorithm ("sha-256 AB:CD:..."); the
// algorithm token is skipped when it is not a hex string (so a fingerprint
// that is written without a separator does not lose its first byte).
//
// Returns 0 on success and -1 when the value is not a well formed
// fingerprint of the expected length.  A value that cannot be parsed is a
// MISMATCH -- it is never silently accepted.
static int dtls_srtp_fingerprint_to_bytes(const char* value, size_t value_len, unsigned char* out, size_t out_len) {
  size_t i = 0;
  size_t bytes = 0;
  int high = -1;

  if (!value || !out || out_len == 0) {
    return -1;
  }

  // Skip the leading white space.
  while (i < value_len && (value[i] == ' ' || value[i] == '\t')) {
    i++;
  }

  // Skip the algorithm token ("sha-256"), but only when it is NOT a hex
  // string.
  {
    size_t j = i;
    while (j < value_len && value[j] != ' ' && value[j] != '\t') {
      j++;
    }
    if (j > i && j < value_len) {
      int all_hex = 1;
      size_t k;
      for (k = i; k < j; k++) {
        if (!dtls_srtp_is_hex_digit(value[k])) {
          all_hex = 0;
          break;
        }
      }
      if (!all_hex) {
        i = j + 1;
        while (i < value_len && (value[i] == ' ' || value[i] == '\t')) {
          i++;
        }
      }
    }
  }

  for (; i < value_len; i++) {
    char c = value[i];

    if (c == ':' || c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      // A separator is only allowed BETWEEN two bytes.
      if (high >= 0) {
        return -1;
      }
      continue;
    }

    if (!dtls_srtp_is_hex_digit(c)) {
      return -1;
    }

    if (high < 0) {
      high = dtls_srtp_hex_value(c);
    } else {
      if (bytes >= out_len) {
        return -1;
      }
      out[bytes] = (unsigned char)((high << 4) | dtls_srtp_hex_value(c));
      bytes++;
      high = -1;
    }
  }

  // A dangling half byte and a wrong length are both a mismatch.
  if (high >= 0 || bytes != out_len) {
    return -1;
  }

  return 0;
}

// 0006: compare two byte strings in constant time.
static int dtls_srtp_bytes_equal(const unsigned char* a, const unsigned char* b, size_t len) {
  unsigned char diff = 0;
  size_t i;

  for (i = 0; i < len; i++) {
    diff |= (unsigned char)(a[i] ^ b[i]);
  }

  return diff == 0;
}

// 0006: does the fingerprint the peer announced in its SDP belong to the
// certificate the peer actually presented?
//
// libwebrtc compares the digest BYTES, so the case of the hex digits and
// the separators of the announced value do not matter.  The old code
// compared the two STRINGS, which turned a fingerprint printed in lower
// case into a mismatch.
//
// Returns 1 when they match, 0 when they do not match and -1 when the
// announced fingerprint could not be parsed at all.
static int dtls_srtp_fingerprint_matches(const char* announced, size_t announced_len, const unsigned char* digest) {
  unsigned char expected[32];

  if (!digest) {
    return -1;
  }

  if (dtls_srtp_fingerprint_to_bytes(announced, announced_len, expected, sizeof(expected)) != 0) {
    return -1;
  }

  return dtls_srtp_bytes_equal(expected, digest, sizeof(expected)) ? 1 : 0;
}

int dtls_srtp_handshake(DtlsSrtp* dtls_srtp, Address* addr) {
  int ret;
  dtls_srtp->remote_addr = addr;
  // 0035: every attempt starts with an unknown fingerprint verdict.
  dtls_srtp->fingerprint_check = 0;

  if (dtls_srtp->role == DTLS_SRTP_ROLE_SERVER) {
    ret = dtls_srtp_handshake_server(dtls_srtp);
  } else {
    ret = dtls_srtp_handshake_client(dtls_srtp);
  }

  // A peer certificate exists only once the DTLS handshake completes. Treat
  // WANT_READ/WANT_WRITE as normal progress instead of a fingerprint failure.
  if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
    return ret;
  if (ret != 0)
    return ret;

  const mbedtls_x509_crt* remote_crt;
  if ((remote_crt = mbedtls_ssl_get_peer_cert(&dtls_srtp->ssl)) != NULL) {
    unsigned char remote_digest[32];
    int fingerprint_match;

    dtls_srtp_x509_digest_bytes(remote_crt, remote_digest);

    // 0035: the printable form is still filled in -- the engine shows it in
    // its diagnostics.
    dtls_srtp_x509_digest(remote_crt, dtls_srtp->actual_remote_fingerprint);

    // 0006: compare the DIGEST BYTES, not the two strings.  The peer may
    // print its fingerprint in lower case (libwebrtc prints it in upper
    // case) -- the old strncmp() turned that difference into a mismatch
    // even though the certificate was the right one.
    fingerprint_match = dtls_srtp_fingerprint_matches(
        dtls_srtp->remote_fingerprint,
        (size_t)dtls_srtp_bounded_len(dtls_srtp->remote_fingerprint, DTLS_SRTP_FINGERPRINT_LENGTH),
        remote_digest);

    if (fingerprint_match != 1) {
      char expected_prefix[16];
      char actual_prefix[16];
      dtls_srtp_fingerprint_summary(dtls_srtp->remote_fingerprint, expected_prefix, sizeof(expected_prefix));
      dtls_srtp_fingerprint_summary(dtls_srtp->actual_remote_fingerprint, actual_prefix, sizeof(actual_prefix));
      // 0035: remember the verdict -- this is the difference between "the
      // handshake never started" and "the handshake ran and the peer was
      // rejected".
      dtls_srtp->fingerprint_check = -1;
      if (fingerprint_match < 0) {
        LOGE("DTLS fingerprint: the fingerprint the peer announced cannot be parsed: alg=%s len=%d prefix=%s",
             dtls_srtp->remote_fingerprint_alg[0] ? dtls_srtp->remote_fingerprint_alg : "?",
             dtls_srtp_bounded_len(dtls_srtp->remote_fingerprint, DTLS_SRTP_FINGERPRINT_LENGTH),
             expected_prefix);
      } else {
        LOGE("DTLS fingerprint mismatch: expected alg=%s len=%d prefix=%s, actual len=%d prefix=%s",
             dtls_srtp->remote_fingerprint_alg[0] ? dtls_srtp->remote_fingerprint_alg : "?",
             dtls_srtp_bounded_len(dtls_srtp->remote_fingerprint, DTLS_SRTP_FINGERPRINT_LENGTH),
             expected_prefix,
             dtls_srtp_bounded_len(dtls_srtp->actual_remote_fingerprint, DTLS_SRTP_FINGERPRINT_LENGTH),
             actual_prefix);
      }
      return -1;
    }
    dtls_srtp->fingerprint_check = 1;

  } else {
    // 0035: the handshake finished but the peer sent no certificate.
    dtls_srtp->fingerprint_check = -2;
    LOGE("no remote fingerprint");
    return -1;
  }

  mbedtls_dtls_srtp_info dtls_srtp_negotiation_result;
  mbedtls_ssl_get_dtls_srtp_negotiation_result(&dtls_srtp->ssl, &dtls_srtp_negotiation_result);

  return ret;
}

void dtls_srtp_reset_session(DtlsSrtp* dtls_srtp) {
  if (!dtls_srtp->initialized) {
    dtls_srtp->state = DTLS_SRTP_STATE_INIT;
    return;
  }

  if (dtls_srtp->state == DTLS_SRTP_STATE_CONNECTED) {
    srtp_dealloc(dtls_srtp->srtp_in);
    srtp_dealloc(dtls_srtp->srtp_out);
    dtls_srtp->srtp_in = NULL;
    dtls_srtp->srtp_out = NULL;
  }

  mbedtls_ssl_session_reset(&dtls_srtp->ssl);
  // 0040: re-arm the DTLS timer after the reset.
  //
  // mbedtls_ssl_session_reset() keeps the timer callbacks, but it cancels
  // the running timer -- which is exactly what we want here: the timer of
  // the half finished handshake must not fire into the new one.
  mbedtls_ssl_set_timer_cb(&dtls_srtp->ssl, &g_dtls_srtp_timer,
                            mbedtls_timing_set_delay, mbedtls_timing_get_delay);
  mbedtls_ssl_set_mtu(&dtls_srtp->ssl, DTLS_SRTP_MTU);
  dtls_srtp->state = DTLS_SRTP_STATE_INIT;
}

// The raw mbedTLS return value of the last dtls_srtp_write() call.
// See dtls_srtp_get_last_write_result() in dtls_srtp.h.
static int g_dtls_srtp_last_write_result = 0;

int dtls_srtp_get_last_write_result(void) {
  return g_dtls_srtp_last_write_result;
}

int dtls_srtp_write(DtlsSrtp* dtls_srtp, const unsigned char* buf, size_t len) {
  int ret = 0;
  int attempt;

  // mbedTLS asks for a retry when the socket is not ready yet.  The retry
  // used to be unbounded, which can spin forever inside the engine's loop
  // thread and hang the whole page.  Cap it and hand the last error back.
  for (attempt = 0; attempt < DTLS_SRTP_WRITE_MAX_RETRIES; attempt++) {
    ret = mbedtls_ssl_write(&dtls_srtp->ssl, buf, len);
    if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
      break;
    }
  }

  g_dtls_srtp_last_write_result = ret;
  return ret;
}

int dtls_srtp_read(DtlsSrtp* dtls_srtp, unsigned char* buf, size_t len) {
  memset(buf, 0, len);
  return mbedtls_ssl_read(&dtls_srtp->ssl, buf, len);
}

int dtls_srtp_probe(uint8_t* buf) {
  if (buf == NULL)
    return 0;

  LOGD("DTLS content type: %d", buf[0]);
  // Handle change-cipher-spec, alert, handshake and application-data records.
  return (buf[0] >= 0x14 && buf[0] <= 0x17);
}

int dtls_srtp_decrypt_rtp_packet(DtlsSrtp* dtls_srtp, uint8_t* packet, int* bytes) {
  return srtp_unprotect(dtls_srtp->srtp_in, packet, bytes);
}

int dtls_srtp_decrypt_rtcp_packet(DtlsSrtp* dtls_srtp, uint8_t* packet, int* bytes) {
  return srtp_unprotect_rtcp(dtls_srtp->srtp_in, packet, bytes);
}

void dtls_srtp_encrypt_rtp_packet(DtlsSrtp* dtls_srtp, uint8_t* packet, int* bytes) {
  srtp_protect(dtls_srtp->srtp_out, packet, bytes);
}

int dtls_srtp_encrypt_rctp_packet(DtlsSrtp* dtls_srtp, uint8_t* packet, int* bytes) {
  return srtp_protect_rtcp(dtls_srtp->srtp_out, packet, bytes);
}

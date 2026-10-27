#ifndef DTLS_SRTP_H_
#define DTLS_SRTP_H_

#include <stdio.h>
#include <stdlib.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ssl_cookie.h>
#include <mbedtls/timing.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/x509_csr.h>

#include <srtp2/srtp.h>

#include "address.h"

#define SRTP_MASTER_KEY_LENGTH 16
#define SRTP_MASTER_SALT_LENGTH 14
#define DTLS_SRTP_KEY_MATERIAL_LENGTH 60
#define DTLS_SRTP_FINGERPRINT_LENGTH 160

typedef enum DtlsSrtpRole {

  DTLS_SRTP_ROLE_CLIENT,
  DTLS_SRTP_ROLE_SERVER

} DtlsSrtpRole;

typedef enum DtlsSrtpState {

  DTLS_SRTP_STATE_INIT,
  DTLS_SRTP_STATE_HANDSHAKE,
  DTLS_SRTP_STATE_CONNECTED

} DtlsSrtpState;

typedef struct DtlsSrtp {
  // MbedTLS
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config conf;
  mbedtls_ssl_cookie_ctx cookie_ctx;
  mbedtls_x509_crt cert;
  mbedtls_pk_context pkey;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctr_drbg;

  // SRTP
  srtp_policy_t remote_policy;
  srtp_policy_t local_policy;
  srtp_t srtp_in;
  srtp_t srtp_out;
  unsigned char remote_policy_key[SRTP_MASTER_KEY_LENGTH + SRTP_MASTER_SALT_LENGTH];
  unsigned char local_policy_key[SRTP_MASTER_KEY_LENGTH + SRTP_MASTER_SALT_LENGTH];

  int (*udp_send)(void* ctx, const unsigned char* buf, size_t len);
  int (*udp_recv)(void* ctx, unsigned char* buf, size_t len);

  Address* remote_addr;

  DtlsSrtpRole role;
  DtlsSrtpState state;
  int initialized;

  // ---------------------------------------------------------------
  // 0035 diagnostics: what the DTLS handshake actually returned.
  //
  // Until now the handshake result lived in a local variable of
  // peer_connection_loop() and died with the stack frame, so the engine
  // could never tell WHY the handshake did not finish.  These fields keep
  // the verdict around long enough for the engine to report it.
  //
  // last_mbedtls_ret     the raw return code of the last
  //                       mbedtls_ssl_handshake() step (MBEDTLS_ERR_SSL_* /
  //                       MBEDTLS_ERR_X509_*; 0 means "the handshake is
  //                       finished").
  // fingerprint_check    0 = not checked yet, 1 = the peer's certificate
  //                       matches the fingerprint from the offer, -1 =
  //                       mismatch, -2 = the handshake finished without a
  //                       peer certificate.
  // handshake_active     1 while mbedtls_ssl_handshake() is running.
  // last_write_is_handshake
  //                       1 when the last DTLS record that went out was a
  //                       handshake record (0 = application data).
  // remote_fingerprint_alg
  //                       the algorithm of the fingerprint the peer
  //                       announced ("sha-256"); empty when the offer
  //                       carried none.
  // ---------------------------------------------------------------
  int last_mbedtls_ret;
  int fingerprint_check;
  int handshake_active;
  int last_write_is_handshake;
  char remote_fingerprint_alg[16];

  char local_fingerprint[DTLS_SRTP_FINGERPRINT_LENGTH];
  char remote_fingerprint[DTLS_SRTP_FINGERPRINT_LENGTH];
  char actual_remote_fingerprint[DTLS_SRTP_FINGERPRINT_LENGTH];

  // ---------------------------------------------------------------
  // 0040: what the DTLS bio (the read path) actually did.
  //
  // WHY: the read path is the place where a DTLS handshake dies
  // silently.  mbedTLS reads a 0 from the bio as "the peer closed the
  // connection" (MBEDTLS_ERR_SSL_CONN_EOF == -0x7280) and a blocking
  // read in the bio stalls the whole peer connection loop -- and with
  // it the ICE retransmission timer.
  //
  // bio_read_calls    how many times the bio was asked for data.
  // bio_want_reads    how many of those found nothing (returned WANT_READ).
  //                   A healthy handshake produces MANY of these: it is
  //                   the normal "not yet" answer.
  // bio_eof_returns   how many times the bio returned 0.  ★ THIS MUST
  //                   STAY 0.  It is the counter that turns red the
  //                   moment somebody reintroduces a `return 0` into the
  //                   read path -- the exact defect that made the other
  //                   stack report -0x7280 ("stuck on negotiating
  //                   connecting").
  // bio_max_call_us   the worst single read, in microseconds.  A read
  //                   that blocks shows up here immediately (the budget
  //                   for one read is a few hundred microseconds).
  // ---------------------------------------------------------------
  uint32_t bio_read_calls;
  uint32_t bio_want_reads;
  uint32_t bio_eof_returns;
  uint32_t bio_max_call_us;

  void* user_data;
} DtlsSrtp;

int dtls_srtp_init(DtlsSrtp* dtls_srtp, DtlsSrtpRole role, void* user_data);

void dtls_srtp_deinit(DtlsSrtp* dtls_srtp);

int dtls_srtp_create_cert(DtlsSrtp* dtls_srtp);

int dtls_srtp_handshake(DtlsSrtp* dtls_srtp, Address* addr);

void dtls_srtp_reset_session(DtlsSrtp* dtls_srtp);

int dtls_srtp_write(DtlsSrtp* dtls_srtp, const uint8_t* buf, size_t len);

/**
 * The raw return value of the last dtls_srtp_write() call.
 *
 * mbedTLS returns the number of bytes written on success and a NEGATIVE
 * error code on failure.  usrsctp's output callback contract is the
 * opposite: 0 means success and a POSITIVE errno means failure.  Keeping
 * the raw value here makes the conversion a one-liner and lets the self
 * test show what the transport really returned.
 */
int dtls_srtp_get_last_write_result(void);

int dtls_srtp_read(DtlsSrtp* dtls_srtp, uint8_t* buf, size_t len);

void dtls_srtp_sctp_to_dtls(DtlsSrtp* dtls_srtp, uint8_t* packet, int bytes);

int dtls_srtp_probe(uint8_t* buf);

int dtls_srtp_decrypt_rtp_packet(DtlsSrtp* dtls_srtp, uint8_t* packet, int* bytes);

int dtls_srtp_decrypt_rtcp_packet(DtlsSrtp* dtls_srtp, uint8_t* packet, int* bytes);

void dtls_srtp_encrypt_rtp_packet(DtlsSrtp* dtls_srtp, uint8_t* packet, int* bytes);

int dtls_srtp_encrypt_rctp_packet(DtlsSrtp* dtls_srtp, uint8_t* packet, int* bytes);

#endif  // DTLS_SRTP_H_

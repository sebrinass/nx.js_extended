#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Reconcile libpeer's mbedtls with the engine's mbedtls.
#
# libpeer's vendored mbedtls 3.4.0 (the OpenNOW-Switch copy) carries an
# override block at the end of mbedtls_config.h, written by
# extern/libpeer/CMakeLists.txt itself:
#
#     /* OpenNOW Switch mbedtls overrides */
#     #define MBEDTLS_NO_PLATFORM_ENTROPY
#     #define MBEDTLS_TEST_NULL_ENTROPY
#     #undef  MBEDTLS_TIMING_C
#     #undef  MBEDTLS_NET_C
#
# That block is the REFERENCE IMPLEMENTATION for a Switch build (OpenNOW-Switch
# is a working GeForce-NOW client for the Switch, MIT).  This script keeps it
# and applies the two deviations that OUR tree needs, each of them backed by a
# concrete compile/link failure in THIS tree:
#
#   MBEDTLS_TIMING_C   ON  (OpenNOW: #undef)
#       extern/libpeer/src/dtls_srtp.c:374 calls
#           mbedtls_ssl_set_timer_cb(&ssl, &timer, mbedtls_timing_set_delay,
#                                     mbedtls_timing_get_delay);
#       UNCONDITIONALLY -- the two symbols are function pointers, so they have
#       to exist at link time.  With MBEDTLS_TIMING_C off, libpeer.a carries an
#       unresolved reference to both and the ENGINE's final link fails with
#       "undefined reference to `mbedtls_timing_set_delay'".
#
#   MBEDTLS_NET_C      ON  (OpenNOW: #undef)
#       The ENGINE's own source/tls.cc uses mbedtls_net_send() /
#       mbedtls_net_recv() / mbedtls_net_free() (mbedtls_ssl_set_bio) and
#       mbedtls_net_context.  With MBEDTLS_NET_C off, source/tls.cc does not
#       even compile ("mbedtls/net_sockets.h declares nothing").
#
#   MBEDTLS_ENTROPY_HARDWARE_ALT   ON  (our addition, not in OpenNOW)
#       The Switch has no /dev/urandom, so MBEDTLS_NO_PLATFORM_ENTROPY has to
#       stay ON -- but then mbedtls registers NO entropy source at all and
#       mbedtls_entropy_func() returns
#       MBEDTLS_ERR_ENTROPY_NO_SOURCES_DEFINED.  libpeer calls
#           mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, ...)
#       INTERNALLY (src/dtls_srtp.c:88, src/ssl_transport.c:59) and ignores
#       the return value, so the DRBG would stay unseeded and the DTLS
#       certificate generation right after it (mbedtls_rsa_gen_key with
#       mbedtls_ctr_drbg_random) would fail -> no WebRTC at all.
#       MBEDTLS_ENTROPY_HARDWARE_ALT lets the ENGINE provide the source:
#       source/nx_entropy.cc implements mbedtls_hardware_poll() on top of
#       libnx' randomGet() (OS-seeded ChaCha).  Real entropy, no /dev/urandom.
#
# NOTE: MBEDTLS_NET_C / MBEDTLS_TIMING_C can only be enabled for a
# Switch/newlib build at all because tools/Switch-libpeer.cmake passes
# -D__unix__ to the mbedtls sub-project (mbedtls' net_sockets.c and
# timing.c refuse to compile on anything that is not Unix or Windows).
#
# This is a build-time fixup of the third-party working tree; it is NOT
# committed.  Re-running the script is idempotent.
#
# Usage:
#   tools/fix-mbedtls-config.sh [path-to-libpeer-dir]
# ---------------------------------------------------------------------------
set -euo pipefail

if [ "$#" -ge 1 ]; then
	LIBPEER_DIR="$1"
else
	TOPDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
	LIBPEER_DIR="${TOPDIR}/extern/libpeer"
fi

MBEDTLS_CONFIG="${LIBPEER_DIR}/third_party/mbedtls/include/mbedtls/mbedtls_config.h"
if [ ! -f "${MBEDTLS_CONFIG}" ]; then
	echo "error: mbedtls config not found: ${MBEDTLS_CONFIG}" >&2
	exit 1
fi

echo "==> reconciling mbedtls config for the engine: ${MBEDTLS_CONFIG}"
sed -i \
	-e 's|^//#define MBEDTLS_SSL_DTLS_SRTP$|#define MBEDTLS_SSL_DTLS_SRTP|' \
	-e 's|^/\* #undef MBEDTLS_NO_PLATFORM_ENTROPY (engine needs real entropy) \*/|#define MBEDTLS_NO_PLATFORM_ENTROPY|' \
	-e 's|^/\* #undef MBEDTLS_NO_PLATFORM_ENTROPY \*/|#define MBEDTLS_NO_PLATFORM_ENTROPY|' \
	-e 's|^//#define MBEDTLS_NO_PLATFORM_ENTROPY$|#define MBEDTLS_NO_PLATFORM_ENTROPY|' \
	-e 's|^/\* #undef MBEDTLS_TEST_NULL_ENTROPY (engine needs real entropy) \*/|#define MBEDTLS_TEST_NULL_ENTROPY|' \
	-e 's|^/\* #undef MBEDTLS_TEST_NULL_ENTROPY \*/|#define MBEDTLS_TEST_NULL_ENTROPY|' \
	-e 's|^//#define MBEDTLS_TEST_NULL_ENTROPY$|#define MBEDTLS_TEST_NULL_ENTROPY|' \
	-e 's|^/\* #undef MBEDTLS_NET_C \*/|#define MBEDTLS_NET_C|' \
	-e 's|^//#define MBEDTLS_NET_C$|#define MBEDTLS_NET_C|' \
	-e 's|^#undef MBEDTLS_NET_C$|#define MBEDTLS_NET_C|' \
	-e 's|^/\* #undef MBEDTLS_TIMING_C \*/|#define MBEDTLS_TIMING_C|' \
	-e 's|^//#define MBEDTLS_TIMING_C$|#define MBEDTLS_TIMING_C|' \
	-e 's|^#undef MBEDTLS_TIMING_C$|#define MBEDTLS_TIMING_C|' \
	-e 's|^/\* #undef MBEDTLS_ENTROPY_HARDWARE_ALT \*/|#define MBEDTLS_ENTROPY_HARDWARE_ALT|' \
	-e 's|^//#define MBEDTLS_ENTROPY_HARDWARE_ALT$|#define MBEDTLS_ENTROPY_HARDWARE_ALT|' \
	-e 's|^#undef MBEDTLS_ENTROPY_HARDWARE_ALT$|#define MBEDTLS_ENTROPY_HARDWARE_ALT|' \
	"${MBEDTLS_CONFIG}"

echo "==> mbedtls config state after fixup:"
grep -n -E '^[[:space:]]*(/\* )?#(define|undef)[[:space:]]+MBEDTLS_(SSL_DTLS_SRTP|NO_PLATFORM_ENTROPY|TEST_NULL_ENTROPY|NET_C|TIMING_C|ENTROPY_HARDWARE_ALT)\b' \
	"${MBEDTLS_CONFIG}" || true

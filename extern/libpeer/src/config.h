#ifndef CONFIG_H_
#define CONFIG_H_

// uncomment this if you want to handshake with a aiortc
// #define CONFIG_DTLS_USE_ECDSA 1

// 0037: the buffer the PEM encoded self signed certificate is written into.
// It used to be sized after RSA_KEY_LENGTH, which is meaningless now that
// the certificate is an EC key (an EC certificate is SMALLER than the RSA
// one, so the old size was enough -- but the coupling was nonsense).
#ifndef CONFIG_DTLS_CERT_BUFFER_SIZE
#define CONFIG_DTLS_CERT_BUFFER_SIZE 4096
#endif

#define SCTP_MTU (1200)
#define CONFIG_MTU (1300)

// Size of the INBOUND packet buffers.
//
// CONFIG_MTU is the OUTBOUND MTU: it is what we advertise and what our own
// packets are sized to.  An inbound RTP packet is bigger than that, because
// the sender's payload plus the 10-byte SRTP header plus the 10-byte
// authentication tag have to fit in one datagram.  With a 1300-byte receive
// buffer such a datagram is truncated, the SRTP authentication tag no longer
// matches, and the packet is dropped as if it had been lost.
//
// 2048 is the value the Switch/xbox WebRTC ports use: it covers a 1500-byte
// RTP payload plus SRTP overhead with room to spare.
#ifndef CONFIG_PACKET_BUFFER_SIZE
#define CONFIG_PACKET_BUFFER_SIZE (2048)
#endif

#ifndef CONFIG_USE_LWIP
#define CONFIG_USE_LWIP 0
#endif

#ifndef CONFIG_MBEDTLS_DEBUG
#define CONFIG_MBEDTLS_DEBUG 0
#endif

#ifndef CONFIG_MBEDTLS_2_X
#define CONFIG_MBEDTLS_2_X 0
#endif

#if CONFIG_MBEDTLS_2_X
#define RSA_KEY_LENGTH 512
#else
#define RSA_KEY_LENGTH 1024
#endif

// 0037: the DTLS self signed certificate is an ECDSA P-256 key.
//
// The official native client (StarRailCloud 4.6.0) is BoringSSL with a self
// signed ECDSA P-256 certificate, and every modern TLS stack refuses a
// 1024 bit RSA certificate by default (OpenSSL 3 with SECLEVEL >= 2, the
// BoringSSL default).  The old RSA-1024 certificate was therefore the prime
// suspect for "ICE connects, DTLS handshake fails".
//
// libpeer already has the ECDSA branch (src/dtls_srtp.c) and it generates
// exactly SECP256R1 / P-256, so this is only the switch.
#ifndef CONFIG_DTLS_USE_ECDSA
#define CONFIG_DTLS_USE_ECDSA 1
#endif

#ifndef CONFIG_USE_USRSCTP
#define CONFIG_USE_USRSCTP 1
#endif

#ifndef CONFIG_SDP_BUFFER_SIZE
#define CONFIG_SDP_BUFFER_SIZE 8096
#endif

#ifndef CONFIG_MQTT_BUFFER_SIZE
#define CONFIG_MQTT_BUFFER_SIZE 4096
#endif

#ifndef CONFIG_HTTP_BUFFER_SIZE
#define CONFIG_HTTP_BUFFER_SIZE 4096
#endif

#ifndef CONFIG_TLS_READ_TIMEOUT
#define CONFIG_TLS_READ_TIMEOUT 3000
#endif

#ifndef CONFIG_KEEPALIVE_TIMEOUT
#define CONFIG_KEEPALIVE_TIMEOUT 10000
#endif

#ifndef CONFIG_ICE_CONSENT_INTERVAL_MS
#define CONFIG_ICE_CONSENT_INTERVAL_MS 2500
#endif

#ifndef CONFIG_AUDIO_DURATION
#define CONFIG_AUDIO_DURATION 20
#endif

#ifndef CONFIG_MAX_NALU_SIZE
#define CONFIG_MAX_NALU_SIZE (2 * 1024 * 1024)
#endif

#define CONFIG_IPV6 0
// empty will use first active interface
#define CONFIG_IFACE_PREFIX ""

// #define LOG_LEVEL LEVEL_DEBUG

// Log output is redirected to the engine (see src/utils.h).  On the Switch a
// single printf-family call to stdout reaches libnx's software console renderer
// and crashes the app with a Data Abort @ 0x0 -- see patches/0022-NOTES.md.
// peer_log() is implemented by the engine, in
// source/webrtc_backend_libpeer.cc, and writes to stderr, which the engine
// redirects to sdmc:/switch/nxjs-debug.log.
#ifndef LOG_REDIRECT
#define LOG_REDIRECT 1
#endif

// Disable MQTT and HTTP signaling
// #define DISABLE_PEER_SIGNALING 1

#endif  // CONFIG_H_

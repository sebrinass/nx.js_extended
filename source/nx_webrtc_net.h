#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The Switch's current IPv4 address, in NETWORK byte order.
 *
 * libnx' nifmGetCurrentIpAddress() hands the address over as a
 * "struct in_addr", i.e. the four bytes in memory are the four address
 * octets.  That is exactly what libpeer's Address.sin.sin_addr wants, so
 * the value can be copied over byte for byte.
 *
 * Returns 0 when the address cannot be determined.  Loopback (127.0.0.0/8)
 * and link local (169.254.0.0/16) are rejected as well: an ICE host
 * candidate that points at them can never be used.
 */
uint32_t nx_webrtc_local_ipv4(void);

/**
 * The same address, formatted as a dotted quad ("192.168.1.42").
 *
 * Writes at most `out_len` bytes including the terminating NUL and returns
 * true on success.  On failure `out` is set to the empty string.
 */
bool nx_webrtc_local_ipv4_string(char *out, int out_len);

/**
 * The hook libpeer calls to get the local address.
 *
 * The Switch has no getifaddrs(): the libnx compatibility stub returns an
 * empty list, so libpeer's ports_get_host_addr() could not build a host
 * candidate at all and only the public STUN candidate survived -> no direct
 * LAN path.
 *
 * libpeer declares this symbol WEAK, so a libpeer that is not linked against
 * the engine still builds and falls back to gethostid().  The engine defines
 * it strongly, which makes the linker prefer this one.
 *
 * Returns 1 on success and fills `out` with the network order address.
 */
int nx_libpeer_get_local_ipv4(uint32_t *out);

#ifdef __cplusplus
}
#endif

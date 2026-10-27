// Entropy source for mbedtls on the Nintendo Switch.
//
// The Switch has no /dev/urandom, so mbedtls' built-in platform entropy
// source (library/entropy_poll.c) always fails.  libnx' randomGet() is
// seeded by the OS (ChaCha) and is used instead.
//
// Two callbacks live here:
//
//   1. nx_mbedtls_entropy_func() -- the callback the ENGINE hands to
//      mbedtls_ctr_drbg_seed() (source/tls.cc, source/crypto.cc).
//
//   2. mbedtls_hardware_poll() -- mbedtls' "hardware entropy alternative"
//      (MBEDTLS_ENTROPY_HARDWARE_ALT).  mbedtls registers it as an entropy
//      source, which is what makes the LIBRARY-INTERNAL seeding work:
//      libpeer's own mbedtls_ctr_drbg_seed() call uses mbedtls_entropy_func(),
//      and that one goes through the registered sources.  Without this
//      function libpeer's DTLS handshake could never seed its RNG.
//
// Note that MBEDTLS_NO_PLATFORM_ENTROPY has to stay enabled: mbedtls aborts
// the whole entropy gather as soon as ONE source fails, and the platform
// source (which wants /dev/urandom) fails on every call on the Switch.

#include "nx_entropy.h"

#include <mbedtls/entropy.h>

#include <switch.h>

extern "C" int nx_mbedtls_entropy_func(void *data, unsigned char *output, size_t len) {
	(void)data;
	if (len == 0) {
		return 0;
	}
	if (output == nullptr) {
		return -1;
	}
	randomGet(output, len);
	return 0;
}

#if defined(MBEDTLS_ENTROPY_HARDWARE_ALT)
extern "C" int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen) {
	(void)data;
	if (olen != nullptr) {
		*olen = 0;
	}
	if (len == 0) {
		return 0;
	}
	if (output == nullptr) {
		return -1;
	}
	randomGet(output, len);
	if (olen != nullptr) {
		*olen = len;
	}
	return 0;
}
#endif

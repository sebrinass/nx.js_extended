#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * mbedtls entropy callback for the Nintendo Switch.
 *
 * The Switch has no /dev/urandom, so mbedtls' platform entropy source
 * (library/entropy_poll.c) cannot work at all: MBEDTLS_NO_PLATFORM_ENTROPY
 * stays ENABLED in the mbedtls config that libpeer builds, and the entropy
 * comes from libnx' randomGet() (OS-seeded ChaCha) instead.
 *
 * This is the callback the engine hands to mbedtls_ctr_drbg_seed().
 * The signature matches mbedtls 3.x:
 *
 *     int (*f_entropy)(void *data, unsigned char *output, size_t len)
 *
 * (mbedtls 2.x used a 4-argument callback with a size_t *olen out-parameter;
 * the engine is built against libpeer's mbedtls 3.4.0, hence 3 arguments.)
 */
int nx_mbedtls_entropy_func(void *data, unsigned char *output, size_t len);

#ifdef __cplusplus
}
#endif

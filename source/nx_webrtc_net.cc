// The Switch's local IPv4 address, for libpeer's ICE host candidate.
//
// The Switch has no getifaddrs(): the libnx compatibility stub returns an
// empty list, so libpeer could not build a host candidate at all and only
// the public STUN candidate survived -> no direct LAN path.
//
// The address comes from the nifm service (nifmGetCurrentIpAddress), which
// is the only API that reports which address the console actually has.
// libnx' gethostid() asks the same service, but it opens and closes a nifm
// session on EVERY call, and it falls back to 127.0.0.1 when the query
// fails -- which would turn into a bogus loopback ICE candidate.
//
// The engine owns this: libpeer asks for the address through the weak hook
// nx_libpeer_get_local_ipv4() (see source/nx_webrtc_net.h).

#include "nx_webrtc_net.h"

#include <stdio.h>
#include <string.h>

#include <switch.h>

namespace {

// The address is handed over as a network order "struct in_addr", i.e. the
// four bytes in memory are the four address octets.  Test the octets and not
// the integer value so the check does not depend on the host endianness.
bool ipv4_is_usable(uint32_t ip) {
	const uint8_t *octets = reinterpret_cast<const uint8_t *>(&ip);
	if (octets[0] == 0) {
		return false; // 0.0.0.0 - no address yet
	}
	if (octets[0] == 127) {
		return false; // 127.0.0.0/8 - loopback
	}
	if (octets[0] == 169 && octets[1] == 254) {
		return false; // 169.254.0.0/16 - link local, not routable
	}
	return true;
}

} // namespace

extern "C" uint32_t nx_webrtc_local_ipv4(void) {
	// nifmInitialize() is idempotent (libnx keeps the service session open
	// until nifmExit()), so calling it on every query is cheap and makes the
	// provider work again after source/nifm.cc's nifmExit() ran.
	if (R_FAILED(nifmInitialize(NifmServiceType_User))) {
		return 0;
	}

	uint32_t ip = 0;
	if (R_FAILED(nifmGetCurrentIpAddress(&ip))) {
		return 0;
	}
	if (!ipv4_is_usable(ip)) {
		return 0;
	}
	return ip;
}

extern "C" bool nx_webrtc_local_ipv4_string(char *out, int out_len) {
	if (out == nullptr || out_len <= 0) {
		return false;
	}
	out[0] = '\0';

	const uint32_t ip = nx_webrtc_local_ipv4();
	if (ip == 0) {
		return false;
	}

	const uint8_t *octets = reinterpret_cast<const uint8_t *>(&ip);
	snprintf(out, static_cast<size_t>(out_len), "%u.%u.%u.%u", octets[0],
	         octets[1], octets[2], octets[3]);
	return true;
}

extern "C" int nx_libpeer_get_local_ipv4(uint32_t *out) {
	if (out == nullptr) {
		return 0;
	}
	const uint32_t ip = nx_webrtc_local_ipv4();
	if (ip == 0) {
		return 0;
	}
	*out = ip;
	return 1;
}

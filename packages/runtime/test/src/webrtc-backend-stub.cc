// Host stub for the native WebRTC backend.
//
// The host test binary (packages/runtime/test) has no libpeer: libpeer needs
// a devkitPro-cross-compiled mbedtls with MBEDTLS_SSL_DTLS_SRTP, libsrtp and
// usrsctp, none of which exist on the host.
//
// This file implements the SAME C ABI (source/webrtc_backend.h) with a
// deterministic fake, so the portable V8 half (source/webrtc.cc) can be
// compiled and exercised in the host test binary.
//
// The fake is deliberately dumb: it remembers what it was told and replays
// canned events. It is NOT a WebRTC stack and must never be mistaken for one.

#include "webrtc_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr int kMaxPeers = 8;
constexpr int kMaxEvents = 32;
constexpr int kTextLen = 512;

struct Event {
	int kind;
	int peer_id;
	int num;
	int num2;
	char text[kTextLen];
	char text2[64];
	unsigned char *bytes;
	size_t bytes_len;
};

struct Peer {
	bool used;
	char *remote_sdp;
	char *local_sdp;
	Event events[kMaxEvents];
	int event_count;
};

Peer g_peers[kMaxPeers];

Peer *peer_for(int peer_id) {
	if (peer_id < 0 || peer_id >= kMaxPeers)
		return nullptr;
	return g_peers[peer_id].used ? &g_peers[peer_id] : nullptr;
}

Event *push_event(Peer *p, int kind) {
	if (p->event_count >= kMaxEvents)
		return nullptr;
	Event *e = &p->events[p->event_count++];
	memset(e, 0, sizeof(*e));
	e->kind = kind;
	return e;
}

} // namespace

extern "C" {

int nx_webrtc_backend_init(void) { return 0; }

void nx_webrtc_backend_deinit(void) {
	for (int i = 0; i < kMaxPeers; i++) {
		if (g_peers[i].used)
			nx_webrtc_backend_destroy(i);
	}
}

int nx_webrtc_backend_create(int peer_id, int ice_server_count,
                             const char *const *urls,
                             const char *const *usernames,
                             const char *const *credentials) {
	(void)ice_server_count;
	(void)urls;
	(void)usernames;
	(void)credentials;
	if (peer_id < 0 || peer_id >= kMaxPeers)
		return -1;
	Peer *p = &g_peers[peer_id];
	if (p->used)
		return -1;
	memset(p, 0, sizeof(*p));
	p->used = true;
	return 0;
}

void nx_webrtc_backend_destroy(int peer_id) {
	Peer *p = peer_for(peer_id);
	if (p == nullptr)
		return;
	free(p->remote_sdp);
	free(p->local_sdp);
	for (int i = 0; i < p->event_count; i++) {
		free(p->events[i].bytes);
	}
	memset(p, 0, sizeof(*p));
}

int nx_webrtc_backend_set_remote_description(int peer_id, const char *sdp,
                                             int is_offer) {
	(void)is_offer;
	Peer *p = peer_for(peer_id);
	if (p == nullptr || sdp == nullptr)
		return -1;
	free(p->remote_sdp);
	p->remote_sdp = strdup(sdp);
	return 0;
}

char *nx_webrtc_backend_create_answer(int peer_id) {
	Peer *p = peer_for(peer_id);
	if (p == nullptr)
		return nullptr;
	// A minimal, well-formed answer. The host harness only cares that the
	// binding marshals the string correctly.
	static const char *kAnswer =
	    "v=0\r\n"
	    "o=- 0 0 IN IP4 0.0.0.0\r\n"
	    "s=-\r\n"
	    "t=0 0\r\n"
	    "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
	    "a=mid:0\r\n"
	    "a=recvonly\r\n"
	    "a=candidate:1 1 UDP 2130706431 127.0.0.1 5000 typ host\r\n";
	char *out = strdup(kAnswer);
	free(p->local_sdp);
	p->local_sdp = strdup(kAnswer);

	// Replay one ICE candidate so the host harness can assert on the
	// sdpMid / sdpMLineIndex plumbing.
	Event *e = push_event(p, NX_WEBRTC_EV_ICE_CANDIDATE);
	if (e) {
		snprintf(e->text, kTextLen,
		         "candidate:1 1 UDP 2130706431 127.0.0.1 5000 typ host");
		snprintf(e->text2, sizeof(e->text2), "0");
		e->num = 0;
	}
	return out;
}

void nx_webrtc_backend_set_local_description(int peer_id, const char *sdp) {
	Peer *p = peer_for(peer_id);
	if (p == nullptr || sdp == nullptr)
		return;
	free(p->local_sdp);
	p->local_sdp = strdup(sdp);
}

int nx_webrtc_backend_add_ice_candidate(int peer_id, const char *candidate) {
	Peer *p = peer_for(peer_id);
	if (p == nullptr || candidate == nullptr)
		return -1;
	return 0;
}

int nx_webrtc_backend_dc_send(int peer_id, int sid, const uint8_t *data,
                               size_t len) {
	Peer *p = peer_for(peer_id);
	if (p == nullptr || data == nullptr || len == 0)
		return -1;
	// Echo the payload back as a datachannelmessage event, so the host
	// harness can assert on the binary round-trip.
	Event *e = push_event(p, NX_WEBRTC_EV_DATACHANNEL_MESSAGE);
	if (e) {
		e->num = sid;
		e->bytes = static_cast<unsigned char *>(malloc(len));
		if (e->bytes) {
			memcpy(e->bytes, data, len);
			e->bytes_len = len;
		}
	}
	return 0;
}

int nx_webrtc_backend_dc_close(int peer_id, int sid) {
	(void)sid;
	Peer *p = peer_for(peer_id);
	if (p == nullptr)
		return -1;
	return 0;
}

int nx_webrtc_backend_get_stats(int peer_id, char *out, size_t out_len) {
	if (out == nullptr || out_len == 0)
		return -1;
	Peer *p = peer_for(peer_id);
	if (p == nullptr) {
		out[0] = '\0';
		return 0;
	}
	int n = snprintf(out, out_len,
	                 "{\"rttMs\":0,\"packetsLost\":0,\"framesReceived\":0}");
	return n < 0 ? 0 : n;
}

int nx_webrtc_backend_state(int peer_id) {
	Peer *p = peer_for(peer_id);
	if (p == nullptr)
		return -1;
	return 1; // PEER_CONNECTION_NEW
}

void nx_webrtc_backend_tick(void) {
	// Nothing to pump: the fake has no sockets.
}

void nx_webrtc_backend_poll_events(nx_webrtc_event_cb cb, void *userdata) {
	if (cb == nullptr)
		return;
	for (int i = 0; i < kMaxPeers; i++) {
		Peer *p = &g_peers[i];
		if (!p->used)
			continue;
		while (p->event_count > 0) {
			Event *e = &p->events[0];
			nx_webrtc_event ev;
			memset(&ev, 0, sizeof(ev));
			ev.kind = e->kind;
			ev.peer_id = i;
			ev.num = e->num;
			ev.num2 = e->num2;
			ev.text = e->text;
			ev.text2 = e->text2;
			ev.text_len = strlen(e->text);
			ev.bytes = e->bytes;
			ev.bytes_len = e->bytes_len;
			cb(&ev, userdata);
			free(e->bytes);
			memmove(&p->events[0], &p->events[1],
			        sizeof(Event) * (size_t)(p->event_count - 1));
			p->event_count--;
		}
	}
}

} // extern "C"

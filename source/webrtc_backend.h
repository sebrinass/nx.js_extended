#pragma once
/**
 * C ABI between the portable V8 binding (source/webrtc.cc) and the
 * platform-specific WebRTC backend.
 *
 * Why a C ABI instead of calling libpeer directly from webrtc.cc:
 *   - libpeer is C and needs a devkitPro cross-compiled mbedtls with
 *     MBEDTLS_SSL_DTLS_SRTP; the host test binary has neither.
 *   - Keeping webrtc.cc free of libpeer lets the host test binary compile
 *     the *binding* (SDP parsing, event dispatch, marshalling) and exercise
 *     it against a fake backend. See engine/AGENTS.md "Host-Platform Test
 *     Binary" rules 1-4.
 *
 * Threading contract:
 *   - Every function here is called on the JS loop thread, EXCEPT the
 *     backend's internal libpeer callbacks, which fire synchronously from
 *     nx_webrtc_backend_tick(). libpeer is poll-based (single-threaded),
 *     so there is no network thread to marshal.
 *   - nx_webrtc_backend_poll_events() invokes the callback with pointers
 *     that are only valid for the duration of the call.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Event kinds. Mirrored in packages/runtime/src/native-webrtc.ts. */
typedef enum nx_webrtc_event_kind {
	NX_WEBRTC_EV_ICE_CANDIDATE = 0,
	NX_WEBRTC_EV_DATACHANNEL,
	NX_WEBRTC_EV_DATACHANNEL_OPEN,
	NX_WEBRTC_EV_DATACHANNEL_CLOSE,
	NX_WEBRTC_EV_DATACHANNEL_MESSAGE,
	NX_WEBRTC_EV_TRACK,
	NX_WEBRTC_EV_ICE_STATE,
	NX_WEBRTC_EV_CONN_STATE,
	NX_WEBRTC_EV_ERROR,
} nx_webrtc_event_kind;

/**
 * One event.
 *
 * Field meaning per kind:
 *   ICE_CANDIDATE       text = candidate (W3C form, no "a=" prefix)
 *                       text2 = sdpMid, num = sdpMLineIndex
 *   DATACHANNEL         num = sid, text = label
 *   DATACHANNEL_OPEN    num = sid
 *   DATACHANNEL_CLOSE   num = sid
 *   DATACHANNEL_MESSAGE num = sid, bytes/bytes_len = payload
 *   TRACK               text = "video"|"audio", num = sdpMid
 *                       text2 = mediaType: "video/H264"|"video/H265"|"audio/opus"
 *                       (★ the NEGOTIATED codec -- the JS side configures its
 *                        decoder from it, so it must not be a fixed string)
 *   ICE_STATE           text = state name
 *   CONN_STATE          text = state name
 *   ERROR               text = message
 */
typedef struct nx_webrtc_event {
	int kind;
	int peer_id;
	int num;  /* sid / sdpMLineIndex */
	int num2; /* reserved */
	const char *text;      /* candidate / label / mediaType / state / error */
	const char *text2;     /* sdpMid */
	size_t text_len;
	const uint8_t *bytes;  /* DataChannel payload */
	size_t bytes_len;
} nx_webrtc_event;

/** Event sink. Called on the loop thread from nx_webrtc_backend_poll_events. */
typedef void (*nx_webrtc_event_cb)(const nx_webrtc_event *ev, void *userdata);

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

/** The error codes nx_webrtc_backend_create() can return. */
enum {
	/** The peer could not be created (bad id, or peer_connection_create failed). */
	NX_WEBRTC_BACKEND_ERR_CREATE = -1,
	/**
	 * The backend runtime could not be brought up (0041): peer_init()
	 * failed, i.e. libsrtp's srtp_init() failed.
	 *
	 * WITHOUT the runtime (srtp + usrsctp) no SCTP association can ever be
	 * built, so the peer would die at the SCTP step right after the DTLS
	 * handshake.  The caller gets a distinct code so it can tell the user
	 * WHY instead of saying "failed to create peer".
	 */
	NX_WEBRTC_BACKEND_ERR_INIT = -2,
};

/** Bring up the backend (srtp_init + usrsctp init on libpeer). 0 = ok. */
int nx_webrtc_backend_init(void);

/** Tear the backend down. */
void nx_webrtc_backend_deinit(void);

/* ------------------------------------------------------------------ */
/* peer                                                                */
/* ------------------------------------------------------------------ */

/**
 * Create a peer. `peer_id` is the caller's handle (the V8 side owns the id
 * space).
 *
 * ICE servers are passed as three parallel arrays (url / username /
 * credential) so the C side never has to parse JSON. libpeer keeps at most
 * 5 ICE servers (PeerConfiguration.ice_servers[5]); extra entries are
 * ignored. Entries may be NULL.
 *
 * Returns 0 on success, negative on failure.
 */
int nx_webrtc_backend_create(int peer_id, int ice_server_count,
                             const char *const *urls,
                             const char *const *usernames,
                             const char *const *credentials);

/** Destroy a peer and release everything it owns. Idempotent. */
void nx_webrtc_backend_destroy(int peer_id);

/**
 * Feed the remote description (the server's offer).
 * `is_offer` is 1 for an offer, 0 for an answer.
 * Returns 0 on success.
 */
int nx_webrtc_backend_set_remote_description(int peer_id, const char *sdp,
                                             int is_offer);

/**
 * Produce the local answer SDP (offer/answer aligned + munged).
 * Returns a malloc'd NUL-terminated string the caller must free(), or NULL.
 */
char *nx_webrtc_backend_create_answer(int peer_id);

/**
 * Accept the local description AFTER the page SDK munged it
 * (docs/08 §4.4 risk 3). The backend re-syncs its mid mapping from it.
 */
void nx_webrtc_backend_set_local_description(int peer_id, const char *sdp);

/** Add a remote ICE candidate. Returns 0 on success. */
int nx_webrtc_backend_add_ice_candidate(int peer_id, const char *candidate);

/** Send a binary DataChannel payload. Returns 0 on success. */
int nx_webrtc_backend_dc_send(int peer_id, int sid, const uint8_t *data,
                               size_t len);

/** Close a DataChannel. Returns 0 on success. */
int nx_webrtc_backend_dc_close(int peer_id, int sid);

/**
 * Fill `out` with a JSON stats blob. Returns the number of bytes written
 * (excluding the NUL), or negative on failure.
 */
int nx_webrtc_backend_get_stats(int peer_id, char *out, size_t out_len);

/** Current connection state ordinal (mirrors libpeer's PeerConnectionState). */
int nx_webrtc_backend_state(int peer_id);

/**
 * Tell the backend that the ANSWER went out to the cloud (patch 0037).
 *
 * WHY: the ICE connectivity check must not start before the answer is on the
 * wire.  The cloud cannot answer a check that arrives before it has
 * processed our answer -- it does not know our ICE credentials yet -- and it
 * drops it.  The old code sent the first check one frame after
 * createAnswer(), i.e. before the answer had been sent at all; the next one
 * went out 100 frames later (~3.3 s at 30 fps).  That is the "sometimes it
 * connects after 3 seconds" number.
 *
 * WHO calls it: the page (through the facade), right after it has sent the
 * answer over the signalling channel.  If the report never arrives the
 * backend starts the check anyway after PEER_ANSWER_SENT_FALLBACK_MS, so a
 * page that does not implement the report still connects.
 */
void nx_webrtc_backend_notify_answer_sent(int peer_id);

/* ------------------------------------------------------------------ */
/* pump + event drain                                                  */
/* ------------------------------------------------------------------ */

/**
 * Pump the state machine: read sockets, run ICE/DTLS/SRTP/SCTP, deliver
 * media. MUST be called on the loop thread; it may block for ~1 ms while
 * the ICE connectivity check is in flight.
 */
void nx_webrtc_backend_tick(void);

/** Drain queued events into `cb`. Must be called on the loop thread. */
void nx_webrtc_backend_poll_events(nx_webrtc_event_cb cb, void *userdata);

#ifdef __cplusplus
}
#endif

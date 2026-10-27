// libpeer backend for the native WebRTC binding.
//
// Compiled ONLY for the device build: it needs extern/libpeer plus a
// devkitPro-cross-compiled mbedtls with MBEDTLS_SSL_DTLS_SRTP.
// The host test binary uses packages/runtime/test/src/webrtc-backend-stub.cc
// instead (same C ABI, fake behaviour).
//
// Threading: libpeer is POLL-BASED and single-threaded. There is no network
// thread: everything happens inside nx_webrtc_backend_tick() (called from the
// JS loop thread). The libpeer callbacks below therefore run on the loop
// thread and may touch V8 -- but we still funnel them through the event queue
// so the V8 side has a single, ordered drain point.
//
// See docs/17-native绑定设计.md for the full mapping table.

#include "webrtc_backend.h"
#include "nx_webrtc_net.h"

// 0018: the audio ingest seam -- on_audio_packet() hands every RTP audio
// packet (with metadata) to the Opus track's jitter buffer via
// nx_opus_track_push_net() (opus-decoder.h).  The header is safe to include
// here: this backend is device-only and the types.h/V8 it pulls in are the
// same ones the rest of engine/source compiles against.
#include "opus-decoder.h"

// libpeer's public headers are a thin shim; the real declarations live in
// src/. We include the src copies directly (the Makefile adds
// -Iextern/libpeer/src and -Iextern/libpeer/include).
#include "peer.h"
#include "peer_connection.h"

// mbedTLS' error codes.  The table that translates a negative return code
// into a name lives in this file (see mbedtls_error_name() below) and is
// transcribed from the headers that ship with libpeer -- the engine compiles
// against extern/libpeer/build/dist/include, which is mbedTLS 3.4.0.
#include <mbedtls/ssl.h>
#include <mbedtls/x509.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

// ---------------------------------------------------------------------------
// libpeer's log sink.
//
// libpeer's LOG_PRINT() macro used to write straight to stdout.  On the
// Switch, stdout is bound to libnx's SOFTWARE CONSOLE RENDERER (libnx.a:
// console.o -> dotab_stdout): one printf-family call from libpeer reaches
// ConsoleSwRenderer_drawChar(), which dereferences a NULL renderer context
// and kills the whole app with a Data Abort @ 0x0.  peer_connection_create()
// -> agent_create() -> udp_socket_open() hits it, because udp_socket_open()
// logs the receive buffer size.  See patches/0022-NOTES.md.
//
// peer_log() is the far end of libpeer's LOG_REDIRECT switch (the header is
// extern/libpeer/src/utils.h, patched by
// tools/libpeer-switch/0002-no-stdout-log.patch).  Every libpeer log line
// ends up here and is written to stderr, which the engine redirects to
// sdmc:/switch/nxjs-debug.log (source/main.cc, the freopen of LOG_FILENAME).
//
// stderr is safe by construction: libnx binds the console renderer to stdout
// ONLY (dotab_stdout), and the default devoptab for every other handle is
// libsysbase's dotab_stdnull (a sink).  So a log line can never reach
// ConsoleSwRenderer_drawChar().
//
// The signature matches extern/libpeer/src/utils.h exactly; the extern "C" is
// mandatory because libpeer is C and this file is C++.
//
// NOTE: DTLS key material must NEVER be logged.  libpeer carried a printf
// block that dumped it (dead code, "#if 0"); the same patch deleted it, so
// no path is left that could print key material.
// ---------------------------------------------------------------------------
extern "C" void peer_log(char *level_tag, const char *file_name,
                         int line_number, const char *fmt, ...) {
	va_list ap;

	fprintf(stderr, "[peer] %s\t%s\t%d\t", level_tag ? level_tag : "?",
	        file_name ? file_name : "?", line_number);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);
}

// ---------------------------------------------------------------------------
// libpeer's diagnostic channel (0035).
//
// libpeer has its own diagnostic log (peer_connection_diag_log() in
// src/peer_connection.c): it reports the DTLS handshake progress ("dtls_start",
// "dtls_pending ret=<mbedTLS code>", "dtls_endpoint_timeout"), the ICE
// candidate pair states ("ice_pairs", "ice_pair[..]") and the consent
// requests ("ice_consent").
//
// It is OFF unless BOTH of these are done: the callback has to be registered
// AND the channel has to be enabled.  The engine did neither, so the whole
// DTLS/ICE story was invisible -- and the DTLS handshake's verdict itself
// died with the stack frame of peer_connection_loop().
//
// The callback writes to stderr, exactly like peer_log() above: stderr has
// NO libnx console devoptab, so a log line can never reach
// ConsoleSwRenderer_drawChar() (the crash of 0022).  Only the DTLS/ICE
// categories are let through; RTP / RTCP / TWCC stay off.
//
// ★ DTLS KEY MATERIAL MUST NEVER BE LOGGED.  libpeer carried a printf block
// that dumped it (dead code, deleted by 0022); nothing here touches the key
// material.  The fingerprints are reduced to algorithm + length + the first 8
// characters INSIDE libpeer, so the full value never even reaches this file.
// ---------------------------------------------------------------------------
extern "C" void nx_peer_diagnostic_callback(const char *message) {
	if (message == nullptr)
		return;

	const char *body = message;
	if (strncmp(body, "LIBPEER ", 8) == 0)
		body += 8;
	else if (strncmp(body, "SCTP ", 5) == 0)
		// 0041: libpeer has TWO diagnostic channels and they use TWO
		// different prefixes:
		//   peer_connection_diag_log() -> "LIBPEER " (8 chars), src/peer_connection.c
		//   sctp_diag_log()             -> "SCTP "   (5 chars), src/sctp.c
		// Only the first one was stripped, so EVERY SCTP line arrived as
		// "SCTP create_failed ..." -- which matches no kKeep entry and was
		// therefore dropped 100% of the time.  That is why the device log
		// carried "SCTP create socket failed" and never the reason.
		body += 5;

	// Only these categories: the DTLS handshake, the ICE pair states, the
	// DTLS records that arrive, the DTLS -> SCTP hand-over, and (0041) the
	// SCTP layer's own lines.
	static const char *kKeep[] = {
		"dtls",            // dtls_start / dtls_pending / dtls_done / dtls_*
		"ice",             // ice_pairs / ice_pair[..] / ice_consent
		"udp_class=dtls",  // the DTLS records that arrive
		"transport_completed",
		"sctp_create",     // the DTLS -> SCTP hand-over
		// 0041: the SCTP channel (src/sctp.c).  "create_failed" is the
		// fingerprint of THIS bug: it is the line that says WHY
		// sctp_create_association() refused to build the association.
		"create_failed",   // ★★ sctp_create_association()'s failure reason
		"assoc ",          // the SCTP association state machine
		"bind ",           // usrsctp_bind()
		"connect ",        // usrsctp_connect()
		"internal_",       // the internal SCTP INIT / COOKIE handshake
		"dcep_",           // the DataChannel open / ack / control
		"tx ",             // the SCTP chunks that go out
		"rx ",             // the SCTP chunks that come in
		"notification ",   // the SCTP notifications
		"sendv_failed",
		"shutdown_",       // usrsctp_finish()
		"udp_class=rtp",   // the RTP packet class counters (audio / video / unmatched)
		"learned_audio_ssrc", // the audio SSRC libpeer learned by sniffing the PT
		// 0041: the PLI state machine (inner 0011) + the RTP class lines
		// that answer: did our keyframe request reach the cloud, and what
		// did we actually receive?
		"pli_state",       // pli_state sent=... keyframes=... mediaSsrc=... (inner 0011)
		"pli_sent",        // pli_sent count=... mediaSsrc=... keyframes=... (inner 0011)
		"rtcp_pli",        // rtcp_pli sent=... mediaSsrc=... bytes=... (libpeer upstream)
		"udp_class=unknown", // the UDP packets that matched NO class at all
		"rtp_decrypt_failed", // SRTP decryption failed => that packet is GONE
		// 0048/R2: the AIMD timeline (inner 0008) and the heal loop
		// (inner 0013).  Both are libpeer diagnostic lines that the
		// engine must let through -- without these two keys the device
		// log carries NONE of them, because the filter compares the
		// stripped body against kKeep[] and drops everything unmatched.
		"remb_state",       // remb_state declared=... measuredLast=... stressTotal=... (inner 0008)
		"heal_state",       // heal_state pliSent=... pliAnswered=... idrSeen=... (inner 0013)
	};
	bool keep = false;
	for (size_t i = 0; i < sizeof(kKeep) / sizeof(kKeep[0]); i++) {
		if (strncmp(body, kKeep[i], strlen(kKeep[i])) == 0) {
			keep = true;
			break;
		}
	}
	if (!keep)
		return;

	fprintf(stderr, "[peer-diag] %s\n", body);
	fflush(stderr);
}

// ---------------------------------------------------------------------------
// libpeer's SCTP diagnostic channel (0041).
//
// libpeer has a SECOND diagnostic channel next to the peer_connection one:
// sctp_diag_log() in src/sctp.c.  It needs the same two things the
// peer_connection channel needs -- a callback AND the enable flag.
//
// ★ libpeer ALREADY wires the two together:
//     src/peer_connection.c:205  sctp_set_diagnostic_callback(callback);
//     src/peer_connection.c:210  sctp_set_diagnostics_enabled(enabled);
// so peer_connection_set_diagnostic_callback() /
// peer_connection_set_diagnostics_enabled() switch the SCTP channel on as
// well.  The two calls below are the EXPLICIT form of the same thing: they
// make the dependency visible from the engine side and they survive a
// libpeer that stops propagating.
//
// The callback type is `void (*)(const char*)` (src/sctp.h) and
// nx_peer_diagnostic_callback() is exactly `void (const char*)` -- the types
// are compatible, so NO adapter is needed.  What DID need fixing is the
// PREFIX and the FILTER: see nx_peer_diagnostic_callback() above.
//
// NOTE: extern/libpeer/include/ installs only peer.h / peer_connection.h /
// peer_signaling.h -- there is no sctp.h there -- so the two entry points
// are declared here.  The signatures are copied verbatim from
// extern/libpeer/src/sctp.h:
//
//     void sctp_set_diagnostic_callback(void (*callback)(const char*));
//     void sctp_set_diagnostics_enabled(int enabled);
// ---------------------------------------------------------------------------
extern "C" void sctp_set_diagnostic_callback(void (*callback)(const char *));
extern "C" void sctp_set_diagnostics_enabled(int enabled);

namespace {

// ---------------------------------------------------------------------------
// Limits
// ---------------------------------------------------------------------------

constexpr int kMaxPeers = 8;
constexpr int kMaxEvents = 64;
constexpr int kMaxIceServers = 5; // PeerConfiguration.ice_servers[5]
constexpr int kMaxSections = 16;
constexpr int kMaxCandidates = 16;
constexpr int kTextLen = 512;
constexpr int kMidLen = 32;
// One ICE candidate line ("candidate:... typ host") is well below 192
// bytes; the buffer only has to survive the copy.
constexpr int kCandidateLen = 192;
// How many video payload types of one SDP we keep track of.  The cloud's
// offer lists five H.264 profiles plus H.265, so this is plenty.
constexpr int kMaxVideoPayloadTypes = 16;

// ---------------------------------------------------------------------------
// Event queue
// ---------------------------------------------------------------------------

struct Event {
	int kind;
	int peer_id;
	int num;
	int num2;
	char text[kTextLen];
	char text2[kMidLen];
	uint8_t *bytes; // malloc'd, owned by the event
	size_t bytes_len;
};

struct EventQueue {
	Event items[kMaxEvents];
	int head;
	int count;
};

void queue_reset(EventQueue *q) {
	for (int i = 0; i < q->count; i++) {
		Event *e = &q->items[(q->head + i) % kMaxEvents];
		free(e->bytes);
		e->bytes = nullptr;
	}
	q->head = 0;
	q->count = 0;
}

Event *queue_push(EventQueue *q, int kind, int peer_id) {
	if (q->count >= kMaxEvents) {
		// Drop the oldest event rather than growing without bound.
		Event *old = &q->items[q->head];
		free(old->bytes);
		old->bytes = nullptr;
		q->head = (q->head + 1) % kMaxEvents;
		q->count--;
	}
	Event *e = &q->items[(q->head + q->count) % kMaxEvents];
	memset(e, 0, sizeof(*e));
	e->kind = kind;
	e->peer_id = peer_id;
	q->count++;
	return e;
}

// ---------------------------------------------------------------------------
// Peer state
// ---------------------------------------------------------------------------

struct Peer {
	bool used;
	PeerConnection *pc;

	// Copy of the remote offer. Needed to align our answer's mids with the
	// server's (libpeer emits "video"/"audio"/"datachannel"; the server
	// emits "0"/"1"/"2") and to resolve the sdpMid of our local ICE
	// candidates.
	char *remote_offer;

	// The local ICE candidates, as libpeer reported them. They are kept so
	// the self test can show what the engine would actually offer: on the
	// Switch there has to be a HOST candidate (the LAN address), otherwise
	// only the public STUN candidate exists and a direct connection is
	// impossible.
	char local_candidates[kMaxCandidates][kCandidateLen];
	int local_candidate_count;

	// The video codec that was negotiated for this connection.  It is
	// CODEC_H265 when the answer carries H.265 (libpeer then decodes
	// both codecs and picks the one by the RTP payload type).
	MediaCodec video_codec;

	// ★★ 0052: the REAL sid of the DataChannel (the SCTP stream id of the
	//   channel the cloud opened).
	//
	//   -1 = not learned yet.  The sid does not exist while the SCTP
	//   association is still coming up: libpeer's association-up callback
	//   fires BEFORE the cloud's DCEP OPEN, and the sid is born in that
	//   DCEP OPEN (libpeer: sctp_parse_data_channel_open() -> sctp_record_open()).
	//
	//   It is learned from two places, both in this file:
	//   on_dc_channel_opened()  (the cloud's DCEP OPEN arrived)
	//   on_dc_message()          (a message arrived on that stream)
	//
	//   The first sid learned WINS and is never overwritten: the cloud SDK
	//   uses a single DataChannel, so a second value would be a bug, not
	//   an update.
	int dc_sid;

	// Event queue.
	EventQueue events;
};

Peer g_peers[kMaxPeers];

// ---------------------------------------------------------------------------
// The backend runtime's lifetime (0041)
// ---------------------------------------------------------------------------
//
// libpeer's runtime -- libsrtp + usrsctp -- is brought up by peer_init() and
// torn down by peer_deinit().  peer_init() is NOT idempotent in the plain
// sense: peer.c counts the calls (peer_runtime_users) and only runs
// srtp_init() on the 0 -> 1 transition, but EVERY peer_init() bumps the
// counter and EVERY peer_deinit() lowers it.  So the engine calls
// peer_init() exactly once and peer_deinit() exactly once.
//
// g_backend_peer_count is the engine's own count of live peers: it is raised
// by a successful nx_webrtc_backend_create() and lowered by
// nx_webrtc_backend_destroy().  When it reaches 0 the runtime goes down
// with the last peer -- that is the balance for the peer_init() of
// nx_webrtc_backend_init(), and it is what keeps the runtime's lifetime
// tied to the peers' lifetime (no premature teardown, no leak).
int g_backend_peer_count = 0;
bool g_backend_runtime_up = false;

Peer *peer_for(int peer_id) {
	if (peer_id < 0 || peer_id >= kMaxPeers)
		return nullptr;
	return g_peers[peer_id].used ? &g_peers[peer_id] : nullptr;
}

// ---------------------------------------------------------------------------
// The peer teardown WITHOUT the reference counting (0041).
//
// nx_webrtc_backend_deinit() has to destroy the remaining peers, and it must
// not recurse back into nx_webrtc_backend_destroy() -- that one lowers the
// counter and calls nx_webrtc_backend_deinit() again when it hits 0.
// ---------------------------------------------------------------------------
void destroy_peer(int peer_id) {
	Peer *p = peer_for(peer_id);
	if (p == nullptr)
		return;
	if (p->pc) {
		peer_connection_destroy(p->pc);
		p->pc = nullptr;
	}
	free(p->remote_offer);
	p->remote_offer = nullptr;
	p->local_candidate_count = 0;
	p->video_codec = CODEC_NONE;
	queue_reset(&p->events);
	p->used = false;
}

// ---------------------------------------------------------------------------
// Minimal SDP handling
// ---------------------------------------------------------------------------

struct SdpSection {
	int index;       // m-line index
	char media[24];  // "video" / "audio" / "application"
	char mid[kMidLen];
	bool has_mid;
};

// Extract the media kind from an m= line: "m=video 9 UDP/TLS/RTP/SAVPF 96".
void sdp_media_kind(const char *line, char *out, size_t out_len) {
	out[0] = '\0';
	if (strncmp(line, "m=", 2) != 0)
		return;
	const char *p = line + 2;
	size_t i = 0;
	while (*p && *p != ' ' && i + 1 < out_len) {
		out[i++] = *p++;
	}
	out[i] = '\0';
}

// Iterate the SDP line by line. Lines are separated by "\r\n" (libpeer
// requires CRLF -- peer_connection_set_remote_description() scans for
// "\r\n" and silently parses NOTHING if the SDP is LF-only).
//
// `cb` receives (line, length, userdata). Return non-zero from the callback
// to stop early.
typedef int (*SdpLineCb)(const char *line, size_t len, void *ud);

void sdp_foreach_line(const char *sdp, SdpLineCb cb, void *ud) {
	const char *p = sdp;
	while (p && *p) {
		const char *eol = strstr(p, "\r\n");
		size_t len = eol ? (size_t)(eol - p) : strlen(p);
		if (cb(p, len, ud) != 0)
			return;
		if (!eol)
			return;
		p = eol + 2;
	}
}

// Pass 1: collect the m-line sections and their mids.
struct SectionCollector {
	SdpSection *sections;
	int max;
	int count;
	int current; // index of the section being filled, -1 = session level
};

int collect_section_line(const char *line, size_t len, void *ud) {
	SectionCollector *sc = static_cast<SectionCollector *>(ud);
	if (len >= 2 && line[0] == 'm' && line[1] == '=') {
		if (sc->count >= sc->max)
			return 1; // stop
		SdpSection *s = &sc->sections[sc->count];
		memset(s, 0, sizeof(*s));
		s->index = sc->count;
		sdp_media_kind(line, s->media, sizeof(s->media));
		sc->count++;
		sc->current = sc->count - 1;
		return 0;
	}
	if (sc->current >= 0 && len > 6 && strncmp(line, "a=mid:", 6) == 0) {
		SdpSection *s = &sc->sections[sc->current];
		size_t n = len - 6;
		if (n >= kMidLen)
			n = kMidLen - 1;
		memcpy(s->mid, line + 6, n);
		s->mid[n] = '\0';
		s->has_mid = true;
	}
	return 0;
}

int sdp_parse_sections(const char *sdp, SdpSection *sections, int max) {
	SectionCollector sc{sections, max, 0, -1};
	sdp_foreach_line(sdp, collect_section_line, &sc);
	return sc.count;
}

// ---------------------------------------------------------------------------
// Video codec negotiation
// ---------------------------------------------------------------------------
//
// The video codec is NOT hardcoded: the cloud sends H.264 today and
// H.265 when the official page runs in its NativeH265 mode (the offer
// carries "a=rtpmap:96 H265/90000" -- see the captured offer in
// pc/analysis/data/mihoyo-offer-sdp.redacted.txt).
//
// The codec is recognised by the ENCODING NAME of the rtpmap line, never
// by the payload type NUMBER:
//   * the cloud picks the payload type numbers in ITS offer and the facade
//     renumbers the answer to the offer's numbers (facade/src/sdp.ts
//     renumberAnswerPayloadTypes), so a hard coded number ("96 is H.265")
//     is wrong half of the time;
//   * the encoding name is what the codec IS: "H264" / "AVC1" for H.264
//     and "H265" / "HVC1" / "HEV1" for H.265.

// The video codec of an SDP.
enum VideoCodec {
	VIDEO_CODEC_UNKNOWN = 0,
	VIDEO_CODEC_H264,
	VIDEO_CODEC_H265,
};

// One video payload type of an SDP.
struct VideoPayloadType {
	int payload_type;
	bool is_h265;
};

// Compare the encoding name of an rtpmap with one of the known names.
//
// The comparison is case insensitive and stops at the first '.', so both
// "H265" and the WebCodecs flavoured "hvc1.1.6.L93.B0" match "H265" /
// "HVC1".
bool sdp_encoding_matches(const char *encoding, const char *name) {
	size_t i = 0;
	while (name[i] != '\0' && encoding[i] != '\0') {
		char a = encoding[i];
		char b = name[i];
		if (a >= 'a' && a <= 'z')
			a = (char)(a - 'a' + 'A');
		if (b >= 'a' && b <= 'z')
			b = (char)(b - 'a' + 'A');
		if (a != b)
			return false;
		i++;
	}
	if (name[i] != '\0')
		return false;
	return encoding[i] == '\0' || encoding[i] == '.';
}

// Is this rtpmap encoding name an H.265 one?
bool sdp_encoding_is_h265(const char *encoding) {
	return sdp_encoding_matches(encoding, "H265") ||
	       sdp_encoding_matches(encoding, "HVC1") ||
	       sdp_encoding_matches(encoding, "HEV1");
}

// Is this rtpmap encoding name an H.264 one?
bool sdp_encoding_is_h264(const char *encoding) {
	return sdp_encoding_matches(encoding, "H264") ||
	       sdp_encoding_matches(encoding, "AVC1");
}

struct VideoPayloadCollector {
	VideoPayloadType *out;
	int max;
	int count;
	bool in_video;
};

// Collect the video payload types of an SDP.
//
// Only the codecs we can decode are collected: the rtx / red / ulpfec
// payload types of the cloud's offer are of no interest here.
int collect_video_payload_line(const char *line, size_t len, void *ud) {
	VideoPayloadCollector *c = static_cast<VideoPayloadCollector *>(ud);
	if (len >= 2 && line[0] == 'm' && line[1] == '=') {
		c->in_video = (len > 7 && strncmp(line, "m=video", 7) == 0);
		return 0;
	}
	if (!c->in_video)
		return 0;
	if (len <= 9 || strncmp(line, "a=rtpmap:", 9) != 0)
		return 0;

	// "a=rtpmap:<payload type> <encoding name>/<clock rate>[/<channels>]"
	const char *p = line + 9;
	char *end = nullptr;
	const long payload_type = strtol(p, &end, 10);
	if (end == p || *end != ' ')
		return 0;
	p = end + 1;

	size_t n = 0;
	while (p + n < line + len && p[n] != '/' && p[n] != ' ')
		n++;
	if (n == 0)
		return 0;

	char encoding[32];
	if (n >= sizeof(encoding))
		n = sizeof(encoding) - 1;
	memcpy(encoding, p, n);
	encoding[n] = '\0';

	const bool is_h265 = sdp_encoding_is_h265(encoding);
	if (!is_h265 && !sdp_encoding_is_h264(encoding))
		return 0;
	if (c->count >= c->max)
		return 0;

	c->out[c->count].payload_type = (int)payload_type;
	c->out[c->count].is_h265 = is_h265;
	c->count++;
	return 0;
}

// The video payload types of an SDP, in the order the peer listed them.
int sdp_video_payload_types(const char *sdp, VideoPayloadType *out, int max) {
	if (sdp == nullptr || out == nullptr || max <= 0)
		return 0;
	VideoPayloadCollector c{out, max, 0, false};
	sdp_foreach_line(sdp, collect_video_payload_line, &c);
	return c.count;
}

// The video codec of an SDP: the first codec we can decode, in the order
// the peer listed its payload types (that order is the peer's preference).
VideoCodec sdp_video_codec(const char *sdp) {
	VideoPayloadType types[kMaxVideoPayloadTypes];
	const int n = sdp_video_payload_types(sdp, types, kMaxVideoPayloadTypes);
	if (n <= 0)
		return VIDEO_CODEC_UNKNOWN;
	return types[0].is_h265 ? VIDEO_CODEC_H265 : VIDEO_CODEC_H264;
}

// Does the video section of the SDP carry this codec at all?
bool sdp_video_has_codec(const char *sdp, VideoCodec codec) {
	VideoPayloadType types[kMaxVideoPayloadTypes];
	const int n = sdp_video_payload_types(sdp, types, kMaxVideoPayloadTypes);
	for (int i = 0; i < n; i++) {
		const bool is_h265 = types[i].is_h265;
		if ((codec == VIDEO_CODEC_H265 && is_h265) ||
		    (codec == VIDEO_CODEC_H264 && !is_h265))
			return true;
	}
	return false;
}

// The video codec to use for a connection.
//
// The codec comes from the SDP that carries the negotiation (the remote
// offer before the answer exists, the answer afterwards).  When nothing
// can be parsed the fallback is H.264: that is what the cloud sends
// today, so the behaviour does not change.
MediaCodec negotiated_video_codec(const char *sdp) {
	switch (sdp_video_codec(sdp)) {
	case VIDEO_CODEC_H265:
		return CODEC_H265;
	case VIDEO_CODEC_H264:
		return CODEC_H264;
	case VIDEO_CODEC_UNKNOWN:
	default:
		return CODEC_H264;
	}
}

struct DonDiffCollector {
	char prefix[32];
	int value;
	bool in_video;
};

// The "sprop-max-don-diff" of a payload type (RFC 7798 section 7.1).
//
// 0 when the parameter is absent -- and then the DONL field is not in the
// stream at all.
int collect_don_diff_line(const char *line, size_t len, void *ud) {
	DonDiffCollector *c = static_cast<DonDiffCollector *>(ud);
	if (len >= 2 && line[0] == 'm' && line[1] == '=') {
		c->in_video = (len > 7 && strncmp(line, "m=video", 7) == 0);
		return 0;
	}
	if (!c->in_video)
		return 0;
	const size_t prefix_len = strlen(c->prefix);
	if (len <= prefix_len || strncmp(line, c->prefix, prefix_len) != 0)
		return 0;
	const char *key = "sprop-max-don-diff=";
	const char *hit = strstr(line + prefix_len, key);
	if (hit == nullptr)
		return 0;
	c->value = atoi(hit + strlen(key));
	return 1; // stop, one fmtp line is enough
}

int sdp_don_diff(const char *sdp, int payload_type) {
	if (sdp == nullptr || payload_type <= 0)
		return 0;
	DonDiffCollector c;
	snprintf(c.prefix, sizeof(c.prefix), "a=fmtp:%d ", payload_type);
	c.value = 0;
	c.in_video = false;
	sdp_foreach_line(sdp, collect_don_diff_line, &c);
	return c.value;
}

// Hand the negotiated video payload types over to libpeer.
//
// The payload type numbers are NOT fixed: the cloud picks them in its
// offer and the facade renumbers the answer to the offer's numbers
// (facade/src/sdp.ts renumberAnswerPayloadTypes).  libpeer therefore has
// to be told which number carries H.265 -- that number is what decides
// which depacketizer runs.
void apply_video_payload_types(PeerConnection *pc, const char *sdp) {
	if (pc == nullptr)
		return;
	VideoPayloadType types[kMaxVideoPayloadTypes];
	const int n = sdp_video_payload_types(sdp, types, kMaxVideoPayloadTypes);
	int h265_payload_type = 0;
	for (int i = 0; i < n; i++) {
		if (types[i].is_h265) {
			h265_payload_type = types[i].payload_type;
			break;
		}
	}
	peer_connection_set_video_h265_payload_type(pc, h265_payload_type);
	// RFC 7798 section 4.4.3: the DONL field is only present when the
	// SDP announces "sprop-max-don-diff > 0".  The cloud does not use
	// it, so it stays off unless the SDP says otherwise.
	peer_connection_set_video_h265_don(pc,
	                                   sdp_don_diff(sdp, h265_payload_type) > 0 ? 1 : 0);
}

// Pass 2: collect the "a=candidate:" lines together with the index of the
// m-section they belong to.
//
// libpeer appends every candidate at the very END of the SDP, so they belong
// to the LAST m-section. We still track the section per line so we stay
// correct if that ever changes.
struct CandidateCollector {
	char candidates[kMaxCandidates][kTextLen];
	int section_index[kMaxCandidates];
	int count;
	int current;
};

int collect_candidate_line(const char *line, size_t len, void *ud) {
	CandidateCollector *cc = static_cast<CandidateCollector *>(ud);
	if (len >= 2 && line[0] == 'm' && line[1] == '=') {
		cc->current++;
		return 0;
	}
	const char *prefix = "a=candidate:";
	const size_t plen = 11;
	if (len <= plen || strncmp(line, prefix, plen) != 0)
		return 0;
	if (cc->count >= kMaxCandidates)
		return 1;
	// Strip the "a=" prefix -> the W3C candidate form is
	// "candidate:foundation component transport priority address port typ type".
	const char *cand = line + 2;
	size_t n = len - 2;
	if (n >= kTextLen)
		n = kTextLen - 1;
	memcpy(cc->candidates[cc->count], cand, n);
	cc->candidates[cc->count][n] = '\0';
	cc->section_index[cc->count] = cc->current;
	cc->count++;
	return 0;
}

// ---------------------------------------------------------------------------
// Mid alignment
// ---------------------------------------------------------------------------

// Map our local m-sections onto the server's offer sections so that the
// answer we send and the ICE candidates we report carry the SERVER's mids
// (the server matches candidates by mid; see docs/06 §4.4).
//
// Matching is by media kind first (video -> video, application ->
// application), falling back to the same index. Each offer section is used
// at most once.
void align_sections(const SdpSection *local, int local_count,
                    const SdpSection *offer, int offer_count, char (*out_mid)[kMidLen],
                    int *out_offer_index) {
	bool used[32] = {false};
	for (int i = 0; i < local_count; i++) {
		const SdpSection *l = &local[i];
		int pick = -1;

		for (int j = 0; j < offer_count && j < 32; j++) {
			if (used[j])
				continue;
			if (offer[j].media[0] && l->media[0] &&
			    strcmp(offer[j].media, l->media) == 0) {
				pick = j;
				break;
			}
		}
		if (pick < 0) {
			for (int j = 0; j < offer_count && j < 32; j++) {
				if (!used[j] && j == i) {
					pick = j;
					break;
				}
			}
		}
		if (pick < 0) {
			// No counterpart: keep our own mid / index.
			snprintf(out_mid[i], kMidLen, "%s", l->mid);
			out_offer_index[i] = l->index;
			continue;
		}
		used[pick] = true;
		snprintf(out_mid[i], kMidLen, "%s",
		         offer[pick].has_mid ? offer[pick].mid : l->mid);
		out_offer_index[i] = offer[pick].index;
	}
}

// ---------------------------------------------------------------------------
// Answer munging
// ---------------------------------------------------------------------------

struct AnswerMunger {
	char (*out_mid)[kMidLen];
	const int *offer_index;
	int current;
	std::string *out;
};

// Replace our mid with the server's, and force "recvonly" (we are a pure
// receiver; docs/04 §4.2).
//
// NOTE: payload-type alignment with the offer is NOT implemented here yet.
// The reference implementation lives in facade/src/sdp.ts (`mungedAnswer`)
// and must be ported to C (or the munging moved to the TS side) before the
// server accepts our answer. See docs/17 §5.
int munge_answer_line(const char *line, size_t len, void *ud) {
	AnswerMunger *m = static_cast<AnswerMunger *>(ud);
	if (len >= 2 && line[0] == 'm' && line[1] == '=') {
		m->current++;
		m->out->append(line, len);
		m->out->append("\r\n");
		return 0;
	}
	if (m->current >= 0 && len > 6 && strncmp(line, "a=mid:", 6) == 0) {
		m->out->append("a=mid:");
		m->out->append(m->out_mid[m->current]);
		m->out->append("\r\n");
		return 0;
	}
	// ★ 0020: the length check was `len == 11`, but "a=sendrecv" is **10**
	// characters long -- so this branch could **never** fire and the
	// "force recvonly" behaviour it implements never happened.  We were
	// sending "a=sendrecv" to the server while the comment above this
	// function promised "recvonly" (we are a pure receiver, docs/04 §4.2).
	//
	// The server offers "a=sendonly"; answering "a=sendrecv" claims we also
	// want to send media, which we never do -- and it is not what the
	// reference implementation (the official browser) does.
	if (m->current >= 0 && len == 10 && strncmp(line, "a=sendrecv", 10) == 0) {
		m->out->append("a=recvonly\r\n");
		return 0;
	}
	m->out->append(line, len);
	m->out->append("\r\n");
	return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// libpeer callbacks
// ---------------------------------------------------------------------------

namespace {

// libpeer hands us the WHOLE local SDP (not a single candidate). We split it
// into per-candidate events and attach the server's mid.
void on_ice_candidate(char *sdp_text, void *userdata) {
	Peer *p = static_cast<Peer *>(userdata);
	if (p == nullptr || sdp_text == nullptr)
		return;

	SdpSection local[kMaxSections];
	int local_count = sdp_parse_sections(sdp_text, local, kMaxSections);

	SdpSection offer[kMaxSections];
	int offer_count = 0;
	if (p->remote_offer)
		offer_count = sdp_parse_sections(p->remote_offer, offer, kMaxSections);

	char aligned[kMaxSections][kMidLen];
	int offer_index[kMaxSections];
	for (int i = 0; i < kMaxSections; i++) {
		aligned[i][0] = '\0';
		offer_index[i] = i;
	}
	align_sections(local, local_count, offer, offer_count, aligned,
	               offer_index);

	CandidateCollector cc;
	memset(&cc, 0, sizeof(cc));
	cc.current = -1;
	sdp_foreach_line(sdp_text, collect_candidate_line, &cc);

	for (int i = 0; i < cc.count; i++) {
		int sec = cc.section_index[i];
		if (sec < 0 || sec >= local_count)
			sec = local_count > 0 ? local_count - 1 : 0;
		Event *e = queue_push(&p->events, NX_WEBRTC_EV_ICE_CANDIDATE, 0);
		e->peer_id = 0; // filled in by the drain loop
		snprintf(e->text, kTextLen, "%s", cc.candidates[i]);
		snprintf(e->text2, kMidLen, "%s", aligned[sec]);
		e->num = offer_index[sec];

		// Remember the candidate: the self test prints the local ICE
		// candidate list, and it has to come from the engine's own view of
		// what was gathered (not from the page's own view of the event).
		snprintf(p->local_candidates[i], kCandidateLen, "%s", cc.candidates[i]);
	}
	p->local_candidate_count = cc.count;
}

void on_ice_state_change(PeerConnectionState state, void *userdata) {
	Peer *p = static_cast<Peer *>(userdata);
	if (p == nullptr)
		return;
	const char *name = peer_connection_state_to_string(state);
	Event *e = queue_push(&p->events, NX_WEBRTC_EV_ICE_STATE, 0);
	snprintf(e->text, kTextLen, "%s", name ? name : "unknown");
	Event *c = queue_push(&p->events, NX_WEBRTC_EV_CONN_STATE, 0);
	snprintf(c->text, kTextLen, "%s", name ? name : "unknown");
}

void on_dc_message(char *msg, size_t len, void *userdata, uint16_t sid) {
	Peer *p = static_cast<Peer *>(userdata);
	if (p == nullptr)
		return;
	// ★★ 0052: a message carries the REAL sid => learn it (the first one
	//   wins; the cloud SDK uses a single DataChannel).
	if (p->dc_sid < 0)
		p->dc_sid = (int)sid;
	Event *e = queue_push(&p->events, NX_WEBRTC_EV_DATACHANNEL_MESSAGE, 0);
	e->num = (int)sid;
	if (msg && len > 0) {
		e->bytes = static_cast<uint8_t *>(malloc(len));
		if (e->bytes) {
			memcpy(e->bytes, msg, len);
			e->bytes_len = len;
		}
	}
}

// ★★ 0052: the sid of the cloud's DataChannel, or -1 when it is not known yet.
//
// ★ Why this exists (the root cause of "the DataChannel never opened"):
//
//   libpeer's association-up callback (sctp->onopen) fires when the SCTP
//   ASSOCIATION comes up -- sctp.c: SCTP_COOKIE_ECHO / SCTP_COOKIE_ACK
//   (internal impl) and SCTP_COMM_UP (usrsctp impl).  The cloud's DCEP OPEN
//   is parsed LATER, in sctp_parse_data_channel_open(), and THAT is where the
//   sid and the label are born.  => at association-up there is no sid yet.
//
//   The old code answered that with the sentinel -1, and the facade keyed
//   its fallback channel by -1: the channel was never the cloud's channel,
//   a send with sid=-1 became uint16_t 65535 (a stream the cloud does not
//   have), and the "open" event carried a sid that meant nothing.
//
//   Three sources, most reliable first:
//     ① p->dc_sid                  -- learned from the DCEP OPEN / a message
//     ② peer_connection_get_datachannel_sid() -- asks libpeer (label independent)
//     ③ -1                         -- genuinely not known yet
static int nx_webrtc_resolve_dc_sid(Peer *p) {
	if (p == nullptr || p->pc == nullptr)
		return -1;
	if (p->dc_sid >= 0)
		return p->dc_sid;
	uint16_t sid = 0;
	if (peer_connection_get_datachannel_sid(p->pc, &sid) == 0)
		return (int)sid;
	return -1;
}

void on_dc_open(void *userdata) {
	Peer *p = static_cast<Peer *>(userdata);
	if (p == nullptr)
		return;
	// ★★ 0052: the sid is no longer hard-wired to -1.  It is asked for
	//   (nx_webrtc_resolve_dc_sid) and is -1 only while the cloud's DCEP
	//   OPEN has not arrived yet -- which is the normal state at this very
	//   moment, because this callback fires on association-up.
	//
	//   When the channel REALLY opens, on_dc_channel_opened() below reports
	//   the real sid (and the real label) -- the facade then upgrades the
	//   channel it already has instead of building a second one.
	const int sid = nx_webrtc_resolve_dc_sid(p);
	Event *e = queue_push(&p->events, NX_WEBRTC_EV_DATACHANNEL_OPEN, 0);
	e->num = sid;
}

// ★★ 0052: the cloud's DCEP OPEN arrived => the REAL sid and the REAL label
//   exist for the first time.
//
//   libpeer calls this (peer_connection_ondatachannelopened) exactly once
//   per DCEP OPEN of the peer.  The engine reports both to JS:
//
//     · NX_WEBRTC_EV_DATACHANNEL       -- sid + label (the facade upgrades
//       its fallback channel in place and gets the real label)
//     · NX_WEBRTC_EV_DATACHANNEL_OPEN  -- the real sid (the facade marks
//       the channel open)
//
//   The JS side already carries both: source/webrtc.cc puts "sid" and
//   "label" into the datachannel payload, and the runtime's
//   native-webrtc.ts forwards them to onDataChannel(sid, label).
void on_dc_channel_opened(uint16_t sid, const char *label, void *userdata) {
	Peer *p = static_cast<Peer *>(userdata);
	if (p == nullptr)
		return;
	// ★ 幂等：第一个学到的 sid 就算数（单 DataChannel 拓扑）⇒ 不被后来的覆盖。
	if (p->dc_sid < 0)
		p->dc_sid = (int)sid;
	// ★ 先派发 `datachannel`（带真 sid + 真 label）：facade 会把那条
	//   「sid 未知」的兜底通道**就地升级**（不建第二条、不重复派发 ondatachannel）。
	Event *dc = queue_push(&p->events, NX_WEBRTC_EV_DATACHANNEL, 0);
	dc->num = (int)sid;
	snprintf(dc->text, kTextLen, "%s", label ? label : "");
	// ★ 再派发 `datachannelopen`（带真 sid）：facade 把通道置为 open。
	Event *e = queue_push(&p->events, NX_WEBRTC_EV_DATACHANNEL_OPEN, 0);
	e->num = (int)sid;
}

void on_dc_close(void *userdata) {
	Peer *p = static_cast<Peer *>(userdata);
	if (p == nullptr)
		return;
	// ★★ 0052: 通道关闭时带着**真 sid**（学到的那个）⇒ facade 能精确关掉那条通道；
	//   学不到就还是 -1（facade 有「只有一条通道时关掉它」的兜底）。
	Event *e = queue_push(&p->events, NX_WEBRTC_EV_DATACHANNEL_CLOSE, 0);
	e->num = nx_webrtc_resolve_dc_sid(p);
}

void on_video_track(uint8_t *data, size_t size, void *userdata) {
	Peer *p = static_cast<Peer *>(userdata);
	if (p == nullptr)
		return;
	Event *e = queue_push(&p->events, NX_WEBRTC_EV_TRACK, 0);
	snprintf(e->text, kTextLen, "video");
	// ★ The negotiated codec, not a hardcoded "video": the JS side has to
	// configure the decoder with the right codec string (hvc1.* vs avc1.*),
	// otherwise the H.265 access units are fed to the H.264 decoder and
	// every frame is dropped (a black screen, not garbage).
	snprintf(e->text2, kMidLen, p->video_codec == CODEC_H265 ? "video/H265" : "video/H264");
	e->num = 0;
	// The media IS handed to JS.  The "native rendering" route (docs/08
	// §6.4 方案乙) was retired: the official page renders the frames
	// itself, and our own client decodes them in JS through the engine's
	// VideoDecoder / OpusTrack.
	//
	// libpeer calls us once per access unit, already depacketized
	// (rtp_decode_h264): Annex-B form, 4-byte start codes.
	if (data && size > 0) {
		e->bytes = static_cast<uint8_t *>(malloc(size));
		if (e->bytes) {
			memcpy(e->bytes, data, size);
			e->bytes_len = size;
		}
	}
}

// 0018 (A2): THE audio path.  This REPLACES the old on_audio_track() raw
// payload callback and its NX_WEBRTC_EV_TRACK event chain wholesale:
//
//   * libpeer's audio dispatch is an else-if (rtp.c: on_audio_packet vs
//     on_packet) -- registering onaudiopacket makes the old onaudiotrack
//     route fall silent PERMANENTLY.  Both are therefore never registered
//     together; only this one goes into the config below.
//   * The event chain ("track event -> JS -> OpusTrack.push -> 0062 queue
//     -> decode") is migrated into this one call: the packet, its RTP
//     timestamp/sequence and its RED redundant block go straight into the
//     Opus track's jitter buffer (audio-jitter.{h,cc}) on the decode
//     thread's 20 ms pull cadence.  The JS round-trip is gone on purpose:
//     the metadata (seq/ts/redundant) that sequencing needs cannot survive
//     the old bare-payload event, and the loop thread's stalls (video long
//     tasks) used to burst it anyway.
//   * LIFETIME (code-map / 0018 contract): packet->data and
//     packet->redundant point into libpeer's network buffer and die when
//     this callback returns.  nx_audio_jitter_push copies BOTH immediately
//     -- no pointer is ever kept.
//   * The poison gate (RED packet whose strip failed, red_stripped==0) is
//     defended inside nx_audio_jitter_push (design 3.2 step 0); libpeer
//     drops those packets too (impl-libpeer2), so this is the second lock
//     on the same door, not a replacement.
void on_audio_packet(const PeerAudioPacket *packet, void *userdata) {
	(void)userdata; // the jitter route needs no per-peer state
	if (packet == nullptr)
		return;
	nx_opus_track_push_net(packet->data, packet->len, packet->timestamp,
	                       packet->ssrc, packet->sequence,
	                       packet->payload_type, packet->red_stripped,
	                       packet->redundant, packet->redundant_len,
	                       packet->redundant_ts_offset);
}

} // namespace

// ---------------------------------------------------------------------------
// Backend API
// ---------------------------------------------------------------------------

namespace {

// A tiny bounds-checked JSON writer.
//
// The stats blob grew from three integers to a list of strings (the local
// ICE candidates), so chaining snprintf() calls is no longer safe: one
// overflowing field would silently cut the JSON in half.
struct JsonOut {
	char *data;
	size_t cap;
	size_t len;
};

void json_raw(JsonOut &o, const char *s) {
	if (o.data == nullptr || o.cap == 0 || s == nullptr) {
		return;
	}
	const size_t n = strlen(s);
	if (o.len + n + 1 > o.cap) {
		return;
	}
	memcpy(o.data + o.len, s, n);
	o.len += n;
	o.data[o.len] = '\0';
}

void json_fmt(JsonOut &o, const char *fmt, ...) {
	char tmp[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	json_raw(o, tmp);
}

void json_string(JsonOut &o, const char *s) {
	json_raw(o, "\"");
	for (const char *p = (s != nullptr) ? s : ""; *p; p++) {
		char c[8];
		if (*p == '"' || *p == '\\') {
			snprintf(c, sizeof(c), "\\%c", *p);
		} else if ((unsigned char)*p < 0x20) {
			snprintf(c, sizeof(c), "\\u%04x", (unsigned char)*p);
		} else {
			snprintf(c, sizeof(c), "%c", *p);
		}
		json_raw(o, c);
	}
	json_raw(o, "\"");
}

// ---------------------------------------------------------------------------
// mbedTLS error code -> name (0035).
//
// The DTLS handshake hands back a negative mbedTLS code; a bare number in the
// log ("-0x6c00") tells nobody anything.  The table below uses the macros of
// the mbedTLS headers that ship with libpeer
// (extern/libpeer/build/dist/include/mbedtls/ssl.h + x509.h, mbedTLS 3.4.0)
// -- the constants come from the header, so they can not drift.
//
// Only the codes a DTLS handshake can produce are listed; everything else
// falls through to "unknown".
// ---------------------------------------------------------------------------
const char *mbedtls_error_name(int ret) {
	switch (ret) {
	case 0:
		return "OK";
	case MBEDTLS_ERR_SSL_CRYPTO_IN_PROGRESS:
		return "MBEDTLS_ERR_SSL_CRYPTO_IN_PROGRESS";
	case MBEDTLS_ERR_SSL_FEATURE_UNAVAILABLE:
		return "MBEDTLS_ERR_SSL_FEATURE_UNAVAILABLE";
	case MBEDTLS_ERR_SSL_BAD_INPUT_DATA:
		return "MBEDTLS_ERR_SSL_BAD_INPUT_DATA";
	case MBEDTLS_ERR_SSL_INVALID_MAC:
		return "MBEDTLS_ERR_SSL_INVALID_MAC";
	case MBEDTLS_ERR_SSL_INVALID_RECORD:
		return "MBEDTLS_ERR_SSL_INVALID_RECORD";
	case MBEDTLS_ERR_SSL_CONN_EOF:
		return "MBEDTLS_ERR_SSL_CONN_EOF";
	case MBEDTLS_ERR_SSL_DECODE_ERROR:
		return "MBEDTLS_ERR_SSL_DECODE_ERROR";
	case MBEDTLS_ERR_SSL_NO_RNG:
		return "MBEDTLS_ERR_SSL_NO_RNG";
	case MBEDTLS_ERR_SSL_NO_CLIENT_CERTIFICATE:
		return "MBEDTLS_ERR_SSL_NO_CLIENT_CERTIFICATE";
	case MBEDTLS_ERR_SSL_UNSUPPORTED_EXTENSION:
		return "MBEDTLS_ERR_SSL_UNSUPPORTED_EXTENSION";
	case MBEDTLS_ERR_SSL_NO_APPLICATION_PROTOCOL:
		return "MBEDTLS_ERR_SSL_NO_APPLICATION_PROTOCOL";
	case MBEDTLS_ERR_SSL_PRIVATE_KEY_REQUIRED:
		return "MBEDTLS_ERR_SSL_PRIVATE_KEY_REQUIRED";
	case MBEDTLS_ERR_SSL_CA_CHAIN_REQUIRED:
		return "MBEDTLS_ERR_SSL_CA_CHAIN_REQUIRED";
	case MBEDTLS_ERR_SSL_UNEXPECTED_MESSAGE:
		return "MBEDTLS_ERR_SSL_UNEXPECTED_MESSAGE";
	case MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE:
		return "MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE";
	case MBEDTLS_ERR_SSL_UNRECOGNIZED_NAME:
		return "MBEDTLS_ERR_SSL_UNRECOGNIZED_NAME";
	case MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY:
		return "MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY";
	case MBEDTLS_ERR_SSL_BAD_CERTIFICATE:
		return "MBEDTLS_ERR_SSL_BAD_CERTIFICATE";
	case MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET:
		return "MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET";
	case MBEDTLS_ERR_SSL_CANNOT_READ_EARLY_DATA:
		return "MBEDTLS_ERR_SSL_CANNOT_READ_EARLY_DATA";
	case MBEDTLS_ERR_SSL_CANNOT_WRITE_EARLY_DATA:
		return "MBEDTLS_ERR_SSL_CANNOT_WRITE_EARLY_DATA";
	case MBEDTLS_ERR_SSL_ALLOC_FAILED:
		return "MBEDTLS_ERR_SSL_ALLOC_FAILED";
	case MBEDTLS_ERR_SSL_HW_ACCEL_FAILED:
		return "MBEDTLS_ERR_SSL_HW_ACCEL_FAILED";
	case MBEDTLS_ERR_SSL_HW_ACCEL_FALLTHROUGH:
		return "MBEDTLS_ERR_SSL_HW_ACCEL_FALLTHROUGH";
	case MBEDTLS_ERR_SSL_BAD_PROTOCOL_VERSION:
		return "MBEDTLS_ERR_SSL_BAD_PROTOCOL_VERSION";
	case MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE:
		return "MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE";
	case MBEDTLS_ERR_SSL_SESSION_TICKET_EXPIRED:
		return "MBEDTLS_ERR_SSL_SESSION_TICKET_EXPIRED";
	case MBEDTLS_ERR_SSL_PK_TYPE_MISMATCH:
		return "MBEDTLS_ERR_SSL_PK_TYPE_MISMATCH";
	case MBEDTLS_ERR_SSL_UNKNOWN_IDENTITY:
		return "MBEDTLS_ERR_SSL_UNKNOWN_IDENTITY";
	case MBEDTLS_ERR_SSL_INTERNAL_ERROR:
		return "MBEDTLS_ERR_SSL_INTERNAL_ERROR";
	case MBEDTLS_ERR_SSL_COUNTER_WRAPPING:
		return "MBEDTLS_ERR_SSL_COUNTER_WRAPPING";
	case MBEDTLS_ERR_SSL_WAITING_SERVER_HELLO_RENEGO:
		return "MBEDTLS_ERR_SSL_WAITING_SERVER_HELLO_RENEGO";
	case MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED:
		return "MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED";
	case MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL:
		return "MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL";
	case MBEDTLS_ERR_SSL_WANT_READ:
		return "MBEDTLS_ERR_SSL_WANT_READ";
	case MBEDTLS_ERR_SSL_WANT_WRITE:
		return "MBEDTLS_ERR_SSL_WANT_WRITE";
	case MBEDTLS_ERR_SSL_TIMEOUT:
		return "MBEDTLS_ERR_SSL_TIMEOUT";
	case MBEDTLS_ERR_SSL_CLIENT_RECONNECT:
		return "MBEDTLS_ERR_SSL_CLIENT_RECONNECT";
	case MBEDTLS_ERR_SSL_UNEXPECTED_RECORD:
		return "MBEDTLS_ERR_SSL_UNEXPECTED_RECORD";
	case MBEDTLS_ERR_SSL_NON_FATAL:
		return "MBEDTLS_ERR_SSL_NON_FATAL";
	case MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER:
		return "MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER";
	case MBEDTLS_ERR_SSL_CONTINUE_PROCESSING:
		return "MBEDTLS_ERR_SSL_CONTINUE_PROCESSING";
	case MBEDTLS_ERR_SSL_ASYNC_IN_PROGRESS:
		return "MBEDTLS_ERR_SSL_ASYNC_IN_PROGRESS";
	case MBEDTLS_ERR_SSL_EARLY_MESSAGE:
		return "MBEDTLS_ERR_SSL_EARLY_MESSAGE";
	case MBEDTLS_ERR_SSL_UNEXPECTED_CID:
		return "MBEDTLS_ERR_SSL_UNEXPECTED_CID";
	case MBEDTLS_ERR_SSL_VERSION_MISMATCH:
		return "MBEDTLS_ERR_SSL_VERSION_MISMATCH";
	case MBEDTLS_ERR_SSL_BAD_CONFIG:
		return "MBEDTLS_ERR_SSL_BAD_CONFIG";
	case MBEDTLS_ERR_X509_FEATURE_UNAVAILABLE:
		return "MBEDTLS_ERR_X509_FEATURE_UNAVAILABLE";
	case MBEDTLS_ERR_X509_UNKNOWN_OID:
		return "MBEDTLS_ERR_X509_UNKNOWN_OID";
	case MBEDTLS_ERR_X509_INVALID_FORMAT:
		return "MBEDTLS_ERR_X509_INVALID_FORMAT";
	case MBEDTLS_ERR_X509_INVALID_VERSION:
		return "MBEDTLS_ERR_X509_INVALID_VERSION";
	case MBEDTLS_ERR_X509_INVALID_SERIAL:
		return "MBEDTLS_ERR_X509_INVALID_SERIAL";
	case MBEDTLS_ERR_X509_INVALID_ALG:
		return "MBEDTLS_ERR_X509_INVALID_ALG";
	case MBEDTLS_ERR_X509_INVALID_NAME:
		return "MBEDTLS_ERR_X509_INVALID_NAME";
	case MBEDTLS_ERR_X509_INVALID_DATE:
		return "MBEDTLS_ERR_X509_INVALID_DATE";
	case MBEDTLS_ERR_X509_INVALID_SIGNATURE:
		return "MBEDTLS_ERR_X509_INVALID_SIGNATURE";
	case MBEDTLS_ERR_X509_INVALID_EXTENSIONS:
		return "MBEDTLS_ERR_X509_INVALID_EXTENSIONS";
	case MBEDTLS_ERR_X509_UNKNOWN_VERSION:
		return "MBEDTLS_ERR_X509_UNKNOWN_VERSION";
	case MBEDTLS_ERR_X509_UNKNOWN_SIG_ALG:
		return "MBEDTLS_ERR_X509_UNKNOWN_SIG_ALG";
	case MBEDTLS_ERR_X509_SIG_MISMATCH:
		return "MBEDTLS_ERR_X509_SIG_MISMATCH";
	case MBEDTLS_ERR_X509_CERT_VERIFY_FAILED:
		return "MBEDTLS_ERR_X509_CERT_VERIFY_FAILED";
	case MBEDTLS_ERR_X509_CERT_UNKNOWN_FORMAT:
		return "MBEDTLS_ERR_X509_CERT_UNKNOWN_FORMAT";
	case MBEDTLS_ERR_X509_BAD_INPUT_DATA:
		return "MBEDTLS_ERR_X509_BAD_INPUT_DATA";
	case MBEDTLS_ERR_X509_ALLOC_FAILED:
		return "MBEDTLS_ERR_X509_ALLOC_FAILED";
	case MBEDTLS_ERR_X509_FILE_IO_ERROR:
		return "MBEDTLS_ERR_X509_FILE_IO_ERROR";
	case MBEDTLS_ERR_X509_BUFFER_TOO_SMALL:
		return "MBEDTLS_ERR_X509_BUFFER_TOO_SMALL";
	case MBEDTLS_ERR_X509_FATAL_ERROR:
		return "MBEDTLS_ERR_X509_FATAL_ERROR";
	default:
		return "unknown";
	}
}

// DtlsSrtpState (src/dtls_srtp.h)
const char *dtls_state_name(int state) {
	switch (state) {
	case 0:
		return "INIT";
	case 1:
		return "HANDSHAKE";
	case 2:
		return "CONNECTED";
	default:
		return "unknown";
	}
}

// DtlsSrtpRole (src/dtls_srtp.h): 0 = client, 1 = server
const char *dtls_role_name(int role) {
	switch (role) {
	case 0:
		return "client";
	case 1:
		return "server";
	default:
		return "unknown";
	}
}

// The fingerprint verdict (DtlsSrtp.fingerprint_check)
const char *dtls_fingerprint_check_name(int check) {
	switch (check) {
	case 0:
		return "unknown";
	case 1:
		return "match";
	case -1:
		return "mismatch";
	case -2:
		return "no-peer-certificate";
	default:
		return "unknown";
	}
}

// The return code of dtls_srtp_handshake().  libpeer returns -1 for the two
// failures that happen AFTER the TLS handshake itself succeeded (the peer's
// certificate does not match the fingerprint / there is no peer certificate)
// -- -1 is not an mbedTLS code, so it has to be named separately.
const char *dtls_handshake_ret_name(int ret, int fingerprint_check) {
	if (ret == -1) {
		return (fingerprint_check == -2) ? "no-peer-certificate"
		                                 : "fingerprint-mismatch";
	}
	return mbedtls_error_name(ret);
}

// One ICE candidate pair as JSON.
void json_ice_pair(JsonOut &o, const PeerCandidatePairDiag *pair) {
	json_raw(o, "{\"local\":{\"ip\":");
	json_string(o, pair->local_ip);
	json_fmt(o, ",\"port\":%u", (unsigned)pair->local_port);
	json_raw(o, ",\"typ\":");
	json_string(o, pair->local_type);
	json_raw(o, "},\"remote\":{\"ip\":");
	json_string(o, pair->remote_ip);
	json_fmt(o, ",\"port\":%u", (unsigned)pair->remote_port);
	json_raw(o, ",\"typ\":");
	json_string(o, pair->remote_type);
	json_raw(o, "},\"state\":");
	json_string(o, pair->state);
	json_fmt(o, ",\"conncheck\":%d", pair->conncheck);
	json_fmt(o, ",\"selected\":%s", pair->selected ? "true" : "false");
	json_fmt(o, ",\"nominated\":%s", pair->nominated ? "true" : "false");
	// 0040/F1: how many STUN binding responses THIS pair received.
	//
	// THIS is the number that answers "did the cloud answer this IP?".
	// `conncheck` only says how many checks WE sent -- and the app side
	// deliberately leaves it out of its comparison, so it is only reliable
	// in the first snapshot.  `responsesReceived` comes straight from the
	// engine and is reliable in every snapshot.
	json_fmt(o, ",\"responsesReceived\":%u", (unsigned)pair->responses_received);
	json_raw(o, "}");
}

// One local ICE candidate as JSON.
void json_ice_local_candidate(JsonOut &o, const PeerLocalCandidateDiag *cand) {
	json_raw(o, "{\"ip\":");
	json_string(o, cand->ip);
	json_fmt(o, ",\"port\":%u", (unsigned)cand->port);
	json_raw(o, ",\"typ\":");
	json_string(o, cand->type);
	json_raw(o, ",\"proto\":");
	json_string(o, cand->proto);
	json_raw(o, ",\"state\":");
	json_string(o, cand->state);
	json_raw(o, "}");
}

// A fingerprint as JSON: algorithm + length + the first 8 characters.
// (libpeer already reduced the value -- the full fingerprint never gets
// this far.)
void json_fingerprint(JsonOut &o, const char *alg, int len, const char *prefix) {
	json_raw(o, "{\"alg\":");
	json_string(o, alg);
	json_fmt(o, ",\"len\":%d", len);
	json_raw(o, ",\"prefix\":");
	json_string(o, prefix);
	json_raw(o, "}");
}

} // namespace

extern "C" {

int nx_webrtc_backend_init(void) {
	// 0041: ★ THE SCTP DIAGNOSTIC CHANNEL.
	//
	// libpeer has a second, independent diagnostic channel next to the
	// peer_connection one (sctp_diag_log() in src/sctp.c), and libpeer
	// ALREADY propagates the peer_connection channel into it
	// (src/peer_connection.c:205/210).  So the SCTP channel was NOT
	// "never enabled" -- it was never enabled **because this whole function
	// never ran**, and even once it runs, the lines need the "SCTP " prefix
	// to be stripped (see nx_peer_diagnostic_callback() above) or the filter
	// drops them.
	//
	// The explicit calls make that dependency visible from the engine side.
	sctp_set_diagnostic_callback(nx_peer_diagnostic_callback);
	sctp_set_diagnostics_enabled(1);

	// 0035: turn libpeer's diagnostic channel on and route it into the
	// engine's log file (stderr -> sdmc:/switch/nxjs-debug.log).
	//
	// Without this the DTLS handshake progress and the ICE candidate pair
	// states are lost: the channel needs BOTH a callback and the enable
	// flag, and the engine set neither.
	peer_connection_set_diagnostic_callback(nx_peer_diagnostic_callback);
	peer_connection_set_diagnostics_enabled(1);

	// peer_init() does srtp_init() + usrsctp_init(). Must happen before the
	// first peer_connection_create().
	//
	// ★★ THIS FUNCTION HAD NO CALL SITE AT ALL UNTIL 0041.  The declaration
	// (source/webrtc_backend.h) and the definition (right here) were the only
	// two places in the whole engine that mentioned it, so peer_init() never
	// ran: srtp_init() never ran, usrsctp_init() never ran, and
	// `sctp_runtime_initialized` stayed 0 for the whole process.
	//
	// sctp_create_association() refuses to build an association in that
	// state (src/sctp.c: the runtime check) and returns -1 -- which is the
	// "SCTP create socket failed" the real device logged RIGHT AFTER A
	// SUCCESSFUL DTLS HANDSHAKE.
	//
	// Idempotence: peer_init() is counted (peer.c: peer_runtime_users), so
	// calling it twice would leave the counter at 2 and peer_deinit() would
	// have to run twice.  The flag below keeps it at exactly one call.
	if (g_backend_runtime_up)
		return 0;
	if (peer_init() != 0) {
		// peer_init() only fails when srtp_init() fails.
		fprintf(stderr,
		        "[nx-webrtc] backend init FAILED: libsrtp srtp_init() failed\n");
		fflush(stderr);
		return -1;
	}
	g_backend_runtime_up = true;
	return 0;
}

void nx_webrtc_backend_deinit(void) {
	// 0041: the peers go FIRST (each one closes its SCTP association), then
	// libpeer's runtime goes down.  The order matters: usrsctp_finish()
	// waits until the associations are gone.
	for (int i = 0; i < kMaxPeers; i++) {
		if (g_peers[i].used) {
			destroy_peer(i);
		}
	}
	g_backend_peer_count = 0;

	if (g_backend_runtime_up) {
		peer_deinit();
		g_backend_runtime_up = false;
	}
}

int nx_webrtc_backend_create(int peer_id, int ice_server_count,
                             const char *const *urls,
                             const char *const *usernames,
                             const char *const *credentials) {
	if (peer_id < 0 || peer_id >= kMaxPeers)
		return -1;
	Peer *p = &g_peers[peer_id];
	if (p->used)
		return -1;

	// 0041: the runtime has to be up BEFORE the first peer is created:
	// peer_init() runs srtp_init() + usrsctp_init(), and without them
	// sctp_create_association() can never succeed (the SCTP association is
	// built right after the DTLS handshake).
	//
	// THIS IS THE CALL SITE nx_webrtc_backend_init() NEVER HAD.
	if (nx_webrtc_backend_init() != 0)
		return NX_WEBRTC_BACKEND_ERR_INIT;

	PeerConfiguration config;
	memset(&config, 0, sizeof(config));

	// We are a pure receiver: H264 video in, Opus audio in, and a
	// DataChannel for the input packets.
	//
	// ★ 0017: the audio codec MUST be Opus, not G.711.
	//
	// Mihoyo's cloud offer only ever carries these two audio payload
	// types (evidence: pc/analysis/data/mihoyo-offer-sdp.redacted.txt,
	// captured from a real session):
	//
	//     m=audio 9 UDP/TLS/RTP/SAVPF 63 111
	//     a=rtpmap:63  red/48000/2
	//     a=rtpmap:111 opus/48000/2
	//
	// There is NO PCMU (0) and NO PCMA (8) in it.  libpeer picks the
	// audio m-line it advertises purely from this field
	// (peer_connection.c: `switch (pc->config.audio_codec)`), so with
	// CODEC_PCMA we offered "m=audio ... 8" (PCMA) and the server has
	// nothing to match it against -> the audio m-line is rejected in the
	// answer and the cloud never sends us a single audio packet.
	//
	// CODEC_OPUS is safe even though libpeer labels it "not implemented
	// yet" (peer_connection.h): that comment is about *decoding* Opus to
	// PCM, not about being able to negotiate it.  libpeer has
	// sdp_append_opus() (sdp.c) which writes "m=audio ... 111" +
	// "a=rtpmap:111 opus/48000/2", and its RTP layer routes Opus frames
	// through rtp_decode_generic(), which does NOT filter by payload
	// type -- the raw Opus frames are handed to on_audio_packet() as-is
	// (0018: with timestamp/sequence/RED, into the jitter buffer).
	// Decoding them to PCM is patch 0014's job (nx_opus_track).
	//
	// The video codec is negotiated, not hardcoded: negotiated_video_codec()
	// picks it from the SDP that carries the negotiation (the remote offer
	// in nx_webrtc_backend_set_remote_description(), the answer in
	// nx_webrtc_backend_set_local_description()).  At this point there is
	// no SDP yet, so the fallback is used -- H.264, which is what the
	// cloud sends today, so the behaviour does not change.
	config.video_codec = negotiated_video_codec(nullptr);
	config.audio_codec = CODEC_OPUS;
	config.datachannel = DATA_CHANNEL_BINARY;
	config.onvideotrack = on_video_track;
	// 0018 (A2): register onaudiopacket and NOTHING else on the audio side.
	// libpeer's routing is else-if -- with onaudiopacket registered the old
	// onaudiotrack raw-payload route (and with it the NX_WEBRTC_EV_TRACK
	// audio event chain) is dead code by construction, and that is exactly
	// the intent: the "event push -> nx_opus_track_push" downstream was
	// migrated wholesale into on_audio_packet -> nx_opus_track_push_net ->
	// jitter buffer.  Do NOT also set config.onaudiotrack: a half-migrated
	// double feed would interleave two timelines into one decoder.
	config.onaudiopacket = on_audio_packet;
	config.user_data = p;

	int n = ice_server_count;
	if (n > kMaxIceServers)
		n = kMaxIceServers;
	for (int i = 0; i < n; i++) {
		if (!urls || !urls[i])
			continue;
		config.ice_servers[i].urls = urls[i];
		if (usernames)
			config.ice_servers[i].username = usernames[i];
		if (credentials)
			config.ice_servers[i].credential = credentials[i];
	}

	PeerConnection *pc = peer_connection_create(&config);
	if (pc == nullptr)
		return -1;

	p->pc = pc;
	p->used = true;
	p->local_candidate_count = 0;
	// ★★ 0052: no DataChannel sid yet (the cloud's DCEP OPEN has not
	//   arrived).  It is learned in on_dc_channel_opened() / on_dc_message().
	p->dc_sid = -1;
	p->video_codec = config.video_codec;
	queue_reset(&p->events);
	g_backend_peer_count++;

	// libpeer's ICE state callback.
	peer_connection_oniceconnectionstatechange(pc, on_ice_state_change);
	// libpeer's ICE candidate callback. NOTE: it delivers the whole local
	// SDP, not a single candidate -- see on_ice_candidate() above.
	peer_connection_onicecandidate(pc, on_ice_candidate);
	// DataChannel callbacks.
	peer_connection_ondatachannel(pc, on_dc_message, on_dc_open, on_dc_close);
	// ★★ 0052: the callback that carries the REAL sid + label.  It fires
	//   when the cloud's DCEP OPEN arrives -- the only moment at which
	//   they exist.
	peer_connection_ondatachannelopened(pc, on_dc_channel_opened);

	return 0;
}

void nx_webrtc_backend_destroy(int peer_id) {
	destroy_peer(peer_id);

	// 0041: the runtime's lifetime follows the peers'.  When the last peer
	// is gone, libpeer's runtime (srtp + usrsctp) goes down with it -- that
	// is the peer_deinit() that balances the peer_init() of
	// nx_webrtc_backend_init().  nx_webrtc_backend_deinit() is idempotent,
	// so calling it here is safe even when the peer was already gone.
	if (g_backend_peer_count > 0) {
		g_backend_peer_count--;
		if (g_backend_peer_count == 0)
			nx_webrtc_backend_deinit();
	}
}

int nx_webrtc_backend_set_remote_description(int peer_id, const char *sdp,
                                             int is_offer) {
	Peer *p = peer_for(peer_id);
	if (p == nullptr || p->pc == nullptr || sdp == nullptr)
		return -1;

	// Keep our own copy: the mid alignment needs the server's mids.
	free(p->remote_offer);
	p->remote_offer = strdup(sdp);

	// Negotiate the video codec.  The remote offer is the negotiation
	// input: it lists the codecs the cloud is able to send.  When it
	// carries H.265 the H.265 depacketizer is enabled and the answer
	// offers BOTH codecs (libpeer then writes "m=video ... 96 97"), so
	// that dropping the H.265 payload types from the answer later on
	// still leaves a usable video section.
	const MediaCodec codec = negotiated_video_codec(sdp);
	if (codec == CODEC_H265 && p->video_codec != CODEC_H265) {
		peer_connection_set_video_codec(p->pc, CODEC_H265);
		p->video_codec = CODEC_H265;
	}
	// The payload type numbers are NOT fixed: the cloud picks them in its
	// offer.  libpeer needs to know which one carries H.265, because that
	// is what decides which depacketizer runs.  This has to happen AFTER
	// the codec switch above: switching the codec rebuilds the decoder.
	apply_video_payload_types(p->pc, sdp);

	// libpeer scans for "\r\n"; an LF-only SDP is parsed as EMPTY (and
	// silently breaks DTLS/ICE). Normalize before handing it over.
	std::string normalized;
	normalized.reserve(strlen(sdp) + 16);
	for (const char *q = sdp; *q; q++) {
		if (*q == '\r') {
			continue;
		}
		if (*q == '\n') {
			normalized.append("\r\n");
		} else {
			normalized.push_back(*q);
		}
	}
	if (normalized.empty() || normalized.compare(normalized.size() - 2, 2,
	                                             "\r\n") != 0) {
		normalized.append("\r\n");
	}

	peer_connection_set_remote_description(
	    p->pc, normalized.c_str(),
	    is_offer ? SDP_TYPE_OFFER : SDP_TYPE_ANSWER);
	return 0;
}

char *nx_webrtc_backend_create_answer(int peer_id) {
	Peer *p = peer_for(peer_id);
	if (p == nullptr || p->pc == nullptr)
		return nullptr;

	const char *raw = peer_connection_create_answer(p->pc);
	if (raw == nullptr)
		return nullptr;

	// Munging: replace our mids with the server's and force recvonly.
	SdpSection local[kMaxSections];
	int local_count = sdp_parse_sections(raw, local, kMaxSections);
	SdpSection offer[kMaxSections];
	int offer_count = 0;
	if (p->remote_offer)
		offer_count = sdp_parse_sections(p->remote_offer, offer, kMaxSections);

	char aligned[kMaxSections][kMidLen];
	int offer_index[kMaxSections];
	for (int i = 0; i < kMaxSections; i++) {
		aligned[i][0] = '\0';
		offer_index[i] = i;
	}
	align_sections(local, local_count, offer, offer_count, aligned,
	               offer_index);

	std::string out;
	AnswerMunger m{aligned, offer_index, -1, &out};
	sdp_foreach_line(raw, munge_answer_line, &m);

	char *result = strdup(out.c_str());
	return result;
}

void nx_webrtc_backend_set_local_description(int peer_id, const char *sdp) {
	Peer *p = peer_for(peer_id);
	if (p == nullptr || p->pc == nullptr || sdp == nullptr)
		return;
	// The answer we send IS the negotiation result: it names the codec
	// the cloud is going to use.  The facade may have changed it on the
	// way (it strips the H.265 payload types when the page does not want
	// H.265), so the depacketizer follows the ANSWER, not the offer.
	//
	//   * the answer carries H.265 -> keep both codecs and let the RTP
	//     payload type decide which one arrives;
	//   * the answer carries H.264 only -> the cloud sends H.264, so the
	//     H.264 depacketizer has to be the one that runs.  This is the
	//     fallback: it is what happens today.
	if (sdp_video_has_codec(sdp, VIDEO_CODEC_H265)) {
		if (p->video_codec != CODEC_H265) {
			peer_connection_set_video_codec(p->pc, CODEC_H265);
			p->video_codec = CODEC_H265;
		}
		apply_video_payload_types(p->pc, sdp);
	} else if (sdp_video_has_codec(sdp, VIDEO_CODEC_H264) &&
	           p->video_codec == CODEC_H265) {
		peer_connection_set_video_codec(p->pc, CODEC_H264);
		p->video_codec = CODEC_H264;
	}

	// NOTE: libpeer DECLARES peer_connection_set_local_description() but
	// never DEFINES it (src/peer_connection.h:165 vs src/peer_connection.c).
	// Calling it is a link error, so we deliberately do NOT call it.
	//
	// What we do instead: re-read the mids from the munged SDP so that any
	// candidate we report afterwards carries the mid the server actually
	// received.
	SdpSection local[kMaxSections];
	int local_count = sdp_parse_sections(sdp, local, kMaxSections);
	SdpSection offer[kMaxSections];
	int offer_count = 0;
	if (p->remote_offer)
		offer_count = sdp_parse_sections(p->remote_offer, offer, kMaxSections);

	char aligned[kMaxSections][kMidLen];
	int offer_index[kMaxSections];
	for (int i = 0; i < kMaxSections; i++) {
		aligned[i][0] = '\0';
		offer_index[i] = i;
	}
	align_sections(local, local_count, offer, offer_count, aligned,
	               offer_index);
}

int nx_webrtc_backend_add_ice_candidate(int peer_id, const char *candidate) {
	Peer *p = peer_for(peer_id);
	if (p == nullptr || p->pc == nullptr || candidate == nullptr)
		return -1;

	// libpeer's parser (ice_candidate_from_description) requires a
	// "typ <type>" token and ignores anything after it. The SDK's candidates
	// may carry a "ufrag <x>" extension and may omit "typ" -> normalize.
	std::string norm;
	norm.reserve(strlen(candidate) + 32);
	const char *c = candidate;
	if (strncmp(c, "a=", 2) == 0)
		c += 2;
	if (strncmp(c, "candidate:", 10) == 0)
		c += 10;

	// Copy up to the first " ufrag " / " raddr " style extension.
	const char *stop = c;
	while (*stop) {
		if ((stop[0] == ' ' && strncmp(stop + 1, "ufrag", 5) == 0) ||
		    (stop[0] == ' ' && strncmp(stop + 1, "raddr", 5) == 0)) {
			break;
		}
		stop++;
	}
	norm.assign(c, (size_t)(stop - c));
	while (!norm.empty() && (norm.back() == ' ' || norm.back() == '\r' ||
	                         norm.back() == '\n')) {
		norm.pop_back();
	}
	if (norm.find(" typ ") == std::string::npos) {
		norm.append(" typ host");
	}

	// libpeer wants the "candidate:" prefix back.
	std::string full("candidate:");
	full.append(norm);

	return peer_connection_add_ice_candidate(p->pc, const_cast<char *>(full.c_str()));
}

int nx_webrtc_backend_dc_send(int peer_id, int sid, const uint8_t *data,
                               size_t len) {
	Peer *p = peer_for(peer_id);
	if (p == nullptr || p->pc == nullptr)
		return -1;
	if (data == nullptr || len == 0)
		return -1;
	// ★★ 0052: sid < 0 (= "the engine does not know the sid") ⇒ use the
	//   REAL sid of the cloud's channel.  -1 handed to libpeer becomes
	//   uint16_t 65535 => the data goes to a stream the cloud does not
	//   have => the cloud ignores it.  That was the direct reason why
	//   "20029 was sent 0 times" (the heartbeat never reached the cloud).
	int send_sid = sid;
	if (send_sid < 0) {
		const int real = nx_webrtc_resolve_dc_sid(p);
		if (real >= 0)
			send_sid = real;
	}
	return peer_connection_datachannel_send_binary_sid(
	    p->pc, const_cast<char *>(reinterpret_cast<const char *>(data)),
	    len, (uint16_t)send_sid);
}

int nx_webrtc_backend_dc_close(int peer_id, int sid) {
	Peer *p = peer_for(peer_id);
	if (p == nullptr || p->pc == nullptr)
		return -1;
	// libpeer has no "close one channel" API; the whole association is torn
	// down by peer_connection_close().
	(void)sid;
	return 0;
}

int nx_webrtc_backend_get_stats(int peer_id, char *out, size_t out_len) {
	Peer *p = peer_for(peer_id);
	if (out == nullptr || out_len == 0)
		return -1;
	out[0] = '\0';
	if (p == nullptr || p->pc == nullptr)
		return -1;

	int rtt = peer_connection_get_rtt_ms(p->pc);
	PeerVideoRtpStats vstats;
	memset(&vstats, 0, sizeof(vstats));
	peer_connection_get_video_rtp_stats(p->pc, &vstats);

	// The three fields the WebRTC self test needs on real hardware:
	//
	//   iceTieBreaker  -- must not be 0 (strict ICE peers reject that) and
	//                     is a 64 bit value, so it travels as a hex STRING:
	//                     a JSON number cannot carry 64 bits exactly.
	//   lastDtlsWrite  -- the raw mbedTLS return value of the last DTLS
	//                     write.  Positive = bytes written, negative =
	//                     mbedTLS error code.
	//   localCandidates / localIp -- the ICE host candidate has to carry the
	//                     console's LAN address, otherwise only the public
	//                     STUN candidate exists and a direct connection is
	//                     impossible.
	const uint64_t tie_breaker = peer_connection_get_ice_tie_breaker(p->pc);
	const int last_dtls_write = peer_connection_get_last_dtls_write_result(p->pc);

	// 0035: the DTLS and ICE internals.  This is the snapshot that says WHY
	// the DTLS handshake did not finish -- until now the only thing the
	// engine could report was the result of the SCTP payload write.
	//
	// The structure is ~7 KB, so it lives in a function local static (the
	// backend is single threaded: everything happens on the JS loop thread).
	static PeerConnectionDiagnostics diag;
	memset(&diag, 0, sizeof(diag));
	peer_connection_get_diagnostics(p->pc, &diag);

	char local_ip[64];
	local_ip[0] = '\0';
	nx_webrtc_local_ipv4_string(local_ip, sizeof(local_ip));

	const char *state_name =
	    peer_connection_state_to_string(peer_connection_get_state(p->pc));

	JsonOut o{out, out_len, 0};
	json_raw(o, "{");
	json_fmt(o, "\"rttMs\":%d,", rtt);
	// ★★ 0052: the DataChannel's own numbers.
	//
	//   dcSid     -- the REAL sid of the cloud's DataChannel (-1 while the
	//                cloud's DCEP OPEN has not arrived).  Before 0052 this
	//                number did not exist anywhere: the engine reported the
	//                sentinel -1 and the report could not tell "unknown"
	//                from "the cloud never opened a channel".
	//   sctpRttMs -- the smoothed RTT of the SCTP transport itself (the
	//                DataChannel path), which does not depend on the cloud
	//                echoing anything.
	json_fmt(o, "\"dcSid\":%d,", p->dc_sid);
	json_fmt(o, "\"sctpRttMs\":%d,", peer_connection_get_sctp_rtt_ms(p->pc));
	json_fmt(o, "\"packetsLost\":%u,", vstats.sequence_gaps);
	json_fmt(o, "\"framesReceived\":%u,", vstats.access_units_completed);
	// 0042: the video RTP counters, one by one.
	//
	//   The two fields above are a lossy summary: "packetsLost" is really
	//   the sequence-gap count and "framesReceived" is really the number of
	//   access units that were DELIVERED.  Neither of them says WHY a frame
	//   was not delivered, and that is exactly the question of the
	//   2026-10-23 run (2261 access units, 0 key frames, 0 decoded frames):
	//   the access units were dropped by the RTP reassembler, and the only
	//   way to tell "the link lost packets" from "the reassembler gave up
	//   too early" is the counters below.
	//
	//   They are all counters that CHANGE when the fault is present, so a
	//   report that shows them is a criterion and not a decoration:
	//     videoRtpForcedSkips  -- the reassembler gave up on a hole
	//     videoAuDropped       -- access units thrown away because of it
	//     videoAuCompleted     -- access units delivered
	//     videoNackRequests    -- retransmission requests we sent
	//     videoRtpLateDropped  -- packets that arrived too late to be used
	//                             (the retransmissions that the 30 ms hold
	//                             window threw away)
	json_fmt(o, "\"videoRtpPackets\":%u,", vstats.packets_received);
	json_fmt(o, "\"videoRtpSequenceGaps\":%u,", vstats.sequence_gaps);
	json_fmt(o, "\"videoRtpReordered\":%u,", vstats.reordered_packets);
	json_fmt(o, "\"videoRtpLateDropped\":%u,", vstats.late_packets_dropped);
	json_fmt(o, "\"videoRtpForcedSkips\":%u,", vstats.forced_sequence_skips);
	json_fmt(o, "\"videoRtpBuffered\":%u,", vstats.reorder_buffered_packets);
	json_fmt(o, "\"videoAuCompleted\":%u,", vstats.access_units_completed);
	json_fmt(o, "\"videoAuDropped\":%u,", vstats.access_units_dropped);
	json_fmt(o, "\"videoNackRequests\":%u,", vstats.nack_requests);
	json_fmt(o, "\"videoNackPackets\":%u,", vstats.nack_packets_requested);
	// 0044: the key frame / PLI counters (inner patch 0011).
	//
	//   ★ The two questions of this round:
	//     · did we ever ASK the cloud for a key frame?  (videoPliSent)
	//     · did one arrive?                            (videoKeyframesReceived)
	//
	//   Before 0011 the answer to the first question was ALWAYS "no": the
	//   function that builds the RTCP PLI (peer_connection_send_rtcp_pil)
	//   had no caller anywhere in the tree -- we never sent a single PLI.
	//
	//   The two "ms" fields are AGES ("how long ago", 0 = never):
	//     videoPliLastMs      -- since the last PLI went out
	//     videoLastKeyframeMs -- since the last key frame arrived
	//
	//   ★ This quartet IS the criterion of this round:
	//     pliSent > 0 && keyframesReceived > 0  => the cloud answers us
	//     pliSent > 0 && keyframesReceived == 0  => the cloud does NOT
	//                                               answer our requests
	//                                               (=> the search goes to
	//                                                the cloud side)
	//     pliSent == 0 && keyframesReceived == 0  => we never asked (that was
	//                                               the old behaviour)
	//
	//   ★ A key frame is counted when its NAL unit is ASSEMBLED, so a key
	//   frame that was thrown away by the reassembler is counted as well --
	//   that is what tells "the cloud never sent one" apart from "we lost
	//   it" (the latter shows up as videoAuDropped >> videoAuCompleted).
	json_fmt(o, "\"videoKeyframesReceived\":%u,", vstats.keyframes_received);
	json_fmt(o, "\"videoPliSent\":%u,", vstats.pli_sent);
	json_fmt(o, "\"videoPliLastMs\":%u,", vstats.pli_last_sent_ms);
	json_fmt(o, "\"videoLastKeyframeMs\":%u,", vstats.last_keyframe_ms);
	json_raw(o, "\"state\":");
	json_string(o, state_name != nullptr ? state_name : "unknown");
	json_raw(o, ",");
	json_fmt(o, "\"iceTieBreaker\":\"0x%016llx\",",
	         (unsigned long long)tie_breaker);
	json_fmt(o, "\"iceTieBreakerDecimal\":\"%llu\",",
	         (unsigned long long)tie_breaker);
	json_fmt(o, "\"lastDtlsWrite\":%d,", last_dtls_write);
	json_raw(o, "\"localIp\":");
	json_string(o, local_ip);
	json_raw(o, ",");
	json_fmt(o, "\"localCandidateCount\":%d,", p->local_candidate_count);
	json_raw(o, "\"localCandidates\":[");
	for (int i = 0; i < p->local_candidate_count && i < kMaxCandidates; i++) {
		if (i > 0) {
			json_raw(o, ",");
		}
		json_string(o, p->local_candidates[i]);
	}
	json_raw(o, "],");

	// ------------------------------------------------------------------
	// DTLS (0035)
	// ------------------------------------------------------------------
	json_raw(o, "\"dtlsState\":");
	json_string(o, dtls_state_name(diag.dtls_state));
	json_raw(o, ",");
	json_raw(o, "\"dtlsRole\":");
	json_string(o, dtls_role_name(diag.dtls_role));
	json_raw(o, ",");
	// The verdict of the last handshake attempt.  -1 is libpeer's own
	// "the handshake ran but the peer was rejected" code.
	json_fmt(o, "\"dtlsHandshakeRet\":%d,", diag.dtls_handshake_ret);
	json_raw(o, "\"dtlsHandshakeRetName\":");
	json_string(o, dtls_handshake_ret_name(diag.dtls_handshake_ret,
	                                       diag.dtls_fingerprint_check));
	json_raw(o, ",");
	// The raw mbedTLS code of the last handshake step.
	json_fmt(o, "\"dtlsMbedtlsRet\":%d,", diag.dtls_mbedtls_ret);
	json_raw(o, "\"dtlsMbedtlsRetName\":");
	json_string(o, mbedtls_error_name(diag.dtls_mbedtls_ret));
	json_raw(o, ",");
	json_fmt(o, "\"dtlsHandshakeAttempts\":%d,", diag.dtls_handshake_attempts);
	json_fmt(o, "\"dtlsHandshakeStartedMs\":%u,",
	         (unsigned)diag.dtls_handshake_started_ms);
	json_fmt(o, "\"dtlsHandshakeElapsedMs\":%u,",
	         (unsigned)diag.dtls_handshake_elapsed_ms);
	// The same value as lastDtlsWrite (that is the SCTP payload write) --
	// kept so the new field carries the old semantics unchanged.
	json_fmt(o, "\"dtlsLastWrite\":%d,", diag.dtls_last_write_result);
	json_fmt(o, "\"dtlsLastWriteIsHandshake\":%s,",
	         diag.dtls_last_write_is_handshake ? "true" : "false");
	// The fingerprint of OUR answer (algorithm + length + first 8 chars).
	json_raw(o, "\"dtlsFingerprint\":");
	json_fingerprint(o, diag.dtls_fingerprint_alg, diag.dtls_fingerprint_len,
	                 diag.dtls_fingerprint_prefix);
	json_raw(o, ",");
	// The fingerprint libpeer compares the peer's certificate against.
	json_raw(o, "\"dtlsRemoteFingerprint\":");
	json_fingerprint(o, diag.dtls_remote_fingerprint_alg,
	                 diag.dtls_remote_fingerprint_len,
	                 diag.dtls_remote_fingerprint_prefix);
	json_raw(o, ",");
	// The fingerprint the offer really carries.
	json_raw(o, "\"dtlsOfferFingerprint\":");
	json_fingerprint(o, diag.dtls_offer_fingerprint_alg,
	                 diag.dtls_offer_fingerprint_len,
	                 diag.dtls_offer_fingerprint_prefix);
	json_raw(o, ",");
	// 0 unknown / 1 match / -1 mismatch / -2 no peer certificate
	json_fmt(o, "\"dtlsFingerprintCheck\":%d,", diag.dtls_fingerprint_check);
	json_raw(o, "\"dtlsFingerprintCheckName\":");
	json_string(o, dtls_fingerprint_check_name(diag.dtls_fingerprint_check));
	json_raw(o, ",");
	// The a=setup: value the offer carried.
	json_raw(o, "\"dtlsSetupAttr\":");
	json_string(o, diag.dtls_setup_attr);
	json_raw(o, ",");

	// ------------------------------------------------------------------
	// DTLS read path (0040)
	// ------------------------------------------------------------------
	// What the DTLS bio (the read path) did.  dtlsBioEofReturns MUST stay
	// 0: a 0 from the bio is what mbedTLS reads as "the peer closed the
	// connection" (MBEDTLS_ERR_SSL_CONN_EOF, -0x7280), and it is the
	// counter that turns red the moment somebody reintroduces it.
	// dtlsBioMaxCallUs is the worst single read -- a blocking read shows
	// up there immediately.
	json_fmt(o, "\"dtlsBioReadCalls\":%u,",
	         (unsigned)diag.dtls_bio_read_calls);
	json_fmt(o, "\"dtlsBioWantReads\":%u,",
	         (unsigned)diag.dtls_bio_want_reads);
	json_fmt(o, "\"dtlsBioEofReturns\":%u,",
	         (unsigned)diag.dtls_bio_eof_returns);
	json_fmt(o, "\"dtlsBioMaxCallUs\":%u,",
	         (unsigned)diag.dtls_bio_max_call_us);
	// 0040: the REMB (bandwidth feedback) counters.  ★ REMB is a PICTURE
	// QUALITY feature, not a connectivity one.
	json_fmt(o, "\"rembSent\":%u,", (unsigned)diag.remb_sent);
	json_fmt(o, "\"rembBitrateBps\":%u,", (unsigned)diag.remb_bitrate_bps);

	// ------------------------------------------------------------------
	// ICE (0035)
	// ------------------------------------------------------------------
	json_fmt(o, "\"iceConsentSent\":%u,",
	         (unsigned)diag.ice_consent_requests_sent);
	json_fmt(o, "\"iceConsentAttempts\":%u,",
	         (unsigned)diag.ice_consent_attempts);
	json_fmt(o, "\"iceBindingResponsesReceived\":%u,",
	         (unsigned)diag.ice_binding_responses_received);
	// 0040/F2: the binding responses that arrived and were DROPPED
	// (foreign transaction id, wrong source address, failed integrity
	// check).  0 means "the cloud never sent us a response we had to
	// throw away" -- which is what tells "the cloud never answered" and
	// "the cloud answered and we dropped the answer" apart.
	json_fmt(o, "\"iceRejectedResponses\":%u,",
	         (unsigned)diag.ice_rejected_responses);
	// 0040/F3: how long after the answer went out did the FIRST
	// connectivity check leave?  -1 = no check has gone out yet.  Before
	// the 0037 fix this was ~3.3 s (100 frames at 30 fps); after it, a
	// few tens of milliseconds.
	json_fmt(o, "\"iceFirstCheckMs\":%d,", diag.ice_first_check_ms);
	// The absolute stamps, so the app can compute any other delta.
	json_fmt(o, "\"iceFirstCheckAtMs\":%u,",
	         (unsigned)diag.ice_first_check_at_ms);
	json_fmt(o, "\"iceAnswerSentMs\":%u,", (unsigned)diag.ice_answer_sent_ms);
	json_fmt(o, "\"iceRttMs\":%u,", (unsigned)diag.ice_rtt_ms);
	json_fmt(o, "\"iceCandidatePairCount\":%d,",
	         diag.ice_candidate_pair_count);
	json_fmt(o, "\"iceLocalCandidateCount\":%d,",
	         diag.ice_local_candidate_count);

	// ------------------------------------------------------------------
	// SCTP (0041)
	// ------------------------------------------------------------------
	// The SCTP association is built right after the DTLS handshake
	// succeeds.  Before 0041 a failure there was COMPLETELY invisible: the
	// engine only logged "SCTP create socket failed" and the report could
	// not say WHY.
	//
	// ★ HOW TO READ THE THREE FIELDS (AGENTS.md §6.2 rule 8 -- the
	// criterion has to CHANGE when the fault happens):
	//
	//   sctpCreateAttempts == 0
	//       -> the association was NEVER tried: the DTLS handshake never
	//          completed.  sctpCreateRet is then meaningless and
	//          sctpCreateFailReason is "none".
	//
	//   sctpCreateAttempts >= 1
	//       -> sctpCreateRet is the real return value of
	//          sctp_create_association() (0 = ok, -1 = failed) and
	//          sctpCreateFailReason says WHY it failed:
	//          "runtime_not_initialized" (peer_init() never ran -- the 0041
	//          bug), "socket_present", "allocation_failed", "socket_failed",
	//          "setup_failed", "invalid_context" or "unknown".
	int sctp_attempts = 0;
	int sctp_ret = 0;
	const char *sctp_fail_reason = "unknown";
	peer_connection_get_sctp_create_info(p->pc, &sctp_attempts, &sctp_ret,
	                                      &sctp_fail_reason);
	json_fmt(o, "\"sctpCreateAttempts\":%d,", sctp_attempts);
	json_fmt(o, "\"sctpCreateRet\":%d,", sctp_ret);
	json_raw(o, "\"sctpCreateFailReason\":");
	json_string(o, sctp_fail_reason != nullptr ? sctp_fail_reason : "unknown");
	json_raw(o, ",");
	// ★ 0041: the SCTP OUTPUT RING BUFFER depth -- a LIVE counter (it
	// changes every tick), unlike the three frozen values above.
	//
	// sctp_outgoing_data_cb() can only ENQUEUE (it runs inside usrsctp and
	// cannot write to the DTLS channel); sctp_drain() is what actually
	// writes the packets out.
	//
	//   0            -> nothing is waiting: either there is no association
	//                   yet, or everything (the INIT included) went out.
	//   > 0, staying -> the packets are NOT leaving the device: the drain
	//                   path is broken.  Then look for "dtls_write_failed"
	//                   in the diagnostic log.
	//
	// => this is ONE ORDER OF MAGNITUDE faster at telling "our side is
	// broken" from "the cloud did not answer" than waiting for the
	// association to reach COMM_UP.
	json_fmt(o, "\"sctpOutQueued\":%d,",
	         peer_connection_get_sctp_out_queued(p->pc));

	json_raw(o, "\"iceCandidatePairs\":[");
	{
		int emitted = 0;
		for (int i = 0; i < diag.ice_candidate_pair_count &&
		                i < PEER_DIAG_MAX_CANDIDATE_PAIRS;
		     i++) {
			if (emitted > 0) {
				json_raw(o, ",");
			}
			json_ice_pair(o, &diag.ice_candidate_pairs[i]);
			emitted++;
		}
	}
	json_raw(o, "],");

	json_raw(o, "\"iceSelectedPair\":");
	if (diag.ice_selected_pair_index >= 0 &&
	    diag.ice_selected_pair_index < PEER_DIAG_MAX_CANDIDATE_PAIRS) {
		json_ice_pair(o, &diag.ice_candidate_pairs[diag.ice_selected_pair_index]);
	} else {
		json_raw(o, "null");
	}
	json_raw(o, ",");

	json_raw(o, "\"iceNominatedPair\":");
	if (diag.ice_nominated_pair_index >= 0 &&
	    diag.ice_nominated_pair_index < PEER_DIAG_MAX_CANDIDATE_PAIRS) {
		json_ice_pair(o, &diag.ice_candidate_pairs[diag.ice_nominated_pair_index]);
	} else {
		json_raw(o, "null");
	}
	json_raw(o, ",");

	json_raw(o, "\"iceLocalCandidates\":[");
	{
		int emitted = 0;
		for (int i = 0; i < diag.ice_local_candidate_count &&
		                i < PEER_DIAG_MAX_LOCAL_CANDIDATES;
		     i++) {
			if (emitted > 0) {
				json_raw(o, ",");
			}
			json_ice_local_candidate(o, &diag.ice_local_candidates[i]);
			emitted++;
		}
	}
	json_raw(o, "]}");

	return (int)o.len;
}

int nx_webrtc_backend_state(int peer_id) {
	Peer *p = peer_for(peer_id);
	if (p == nullptr || p->pc == nullptr)
		return -1;
	return (int)peer_connection_get_state(p->pc);
}

// 0037: the page reports that the answer went out to the cloud.
//
// Until then no ICE connectivity check leaves: the cloud cannot answer a
// check that arrives before it has processed our answer (it does not know
// our ICE credentials yet) and drops it.  The old code sent the first check
// one frame after createAnswer() -- before the answer had been sent at all
// -- and the next one only 100 frames (~3.3 s) later.  That is the
// "sometimes it connects after 3 seconds" number.
void nx_webrtc_backend_notify_answer_sent(int peer_id) {
	Peer *p = peer_for(peer_id);
	if (p == nullptr || p->pc == nullptr)
		return;
	peer_connection_set_answer_sent(p->pc, 1);
}

// 0037: how many times the libpeer poll loop runs per tick.
//
// libpeer is poll-based and the tick is driven by the RENDER FRAME, so one
// loop per frame caps the socket drain and the ICE/DTLS progress at the
// frame rate.  The ICE retransmission schedule is a wall clock deadline
// now (libpeer patch 0005), so the extra loops are not needed for the
// TIMING any more -- they give the socket more turns per frame.
//
// Frame budget: one loop costs at most ~1 ms while the ICE check is in
// flight (the poll timeout of the socket read); in the COMPLETED state it
// is non blocking and costs ~0.  N = 3 therefore costs <= 3 ms per frame
// during ICE (19 % of a 16 ms frame, ~7 % of the 32..43 ms the device
// really renders at) and ~0 while the media flows.
static const int kWebrtcLoopsPerTick = 3;

void nx_webrtc_backend_tick(void) {
	for (int i = 0; i < kMaxPeers; i++) {
		Peer *p = &g_peers[i];
		if (!p->used || p->pc == nullptr)
			continue;
		// libpeer is poll-based: this is where the sockets are read, ICE
		// runs, DTLS handshakes and SCTP messages are processed. It may
		// block for ~1 ms while the ICE connectivity check is in flight.
		for (int n = 0; n < kWebrtcLoopsPerTick; n++) {
			peer_connection_loop(p->pc);
		}
	}
}

void nx_webrtc_backend_poll_events(nx_webrtc_event_cb cb, void *userdata) {
	if (cb == nullptr)
		return;
	for (int i = 0; i < kMaxPeers; i++) {
		Peer *p = &g_peers[i];
		if (!p->used)
			continue;
		while (p->events.count > 0) {
			// auto: `Event` is ambiguous out here (the anonymous-namespace
			// struct vs libnx's kernel Event typedef via types.h).
			auto *e = &p->events.items[p->events.head];
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
			// The callback may have re-entered and queued more events;
			// advance carefully.
			p->events.head = (p->events.head + 1) % kMaxEvents;
			p->events.count--;
			free(e->bytes);
			e->bytes = nullptr;
		}
	}
}

} // extern "C"

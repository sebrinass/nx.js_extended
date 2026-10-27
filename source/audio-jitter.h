#pragma once

// 0018-tierA: NetEQ-style packet jitter buffer for the live Opus track.
//
// WHY THIS FILE EXISTS (see .local/rev-audio/jitter-buffer-design.md, 0018)
// The audio path used to be push-driven all the way: libpeer handed the raw
// RTP payload to on_audio_track() with the timestamp/sequence THROWN AWAY,
// the JS event queue delivered the packet to nx_opus_track_push() and the
// decode thread decoded it the moment it arrived.  Network jitter therefore
// hit the audio graph's ring buffer directly (audio-graph.cc:519: any render
// quantum with avail < Q pads and counts an underrun), and the recovery from
// those gaps was a hard cut (the 0071 decay-fade), which is audible.
//
// This module is the missing layer: a PACKET buffer indexed by RTP sequence
// number plus a playout state machine that produces exactly one 20 ms frame
// per tick -- decoding a real packet when one is there and CONCEALING when
// not (RED redundant-block refill -> opus PLC -> decay-fade EXPAND -> 20 ms
// cos/sin crossfade MERGE back onto real audio).  That is the core of the
// official client's NetEQ behaviour (field trial
// WebRTC-Audio-NetEqDelayManagerConfig/quantile:0.96 + WebRTC-Audio-Red-For-Opus).
//
// Threading contract:
//   push()   - network side.  On this engine that is the JS LOOP thread
//              (libpeer is poll-based; the onaudiopacket callback fires from
//              nx_webrtc_backend_tick()).  It copies the payload (and the RED
//              redundant block) into a slot immediately: both pointers die
//              when the callback returns (libpeer's agent buffer).
//   pull()   - the track's DECODE thread only (20 ms cadence).  It decodes
//              through the opaque nx_opus_decoder handle passed in.
//   get_stats() / reset() - any thread (JS loop for diagnostics).
//   One internal mutex guards the slot array, the two sequence frontiers and
//   the counters.  pull() holds it across the decode call on purpose: one
//   Opus frame decode is well under a millisecond, and the alternative
//   (lock-free slot hand-off) buys nothing at 50 packets/s.
//
// This module must stay free of V8 (design 3.1): the decoder seam is a
// forward declaration plus two extern functions re-declared in the .cc, so
// including this header never drags in types.h.

#include <stddef.h>
#include <stdint.h>

// ---- frame geometry (Opus over RTP at 48 kHz, 20 ms ptime) -----------------
#define AJ_FRAME_MS 20
#define AJ_FRAME_SAMPLES 960 // 20 ms @ 48 kHz
#define AJ_CHANNELS 2
#define AJ_RTP_CLOCK_HZ 48000
// One RTP timestamp step of a 20 ms frame (opus/48 kHz).  The RED redundant
// block's ts offset is expressed in the same units (960 = one frame back).
#define AJ_FRAME_TS_STEP 960

// ---- packet buffer ---------------------------------------------------------
// Fixed slot ring, indexed by seq % AJ_CAPACITY.  256 >= 2x the official
// max_packets_in_buffer default of 200/2; the real occupancy is bounded by
// the target level (a handful of frames), the rest is head-room for reorder.
#define AJ_CAPACITY 256
// Opus single-frame payload cap (CELT 20 ms, RFC 6716).  Anything bigger is
// not a 20 ms streaming frame and is refused at the door.
#define AJ_MAX_PAYLOAD 1275
// A packet 1..2 behind the insert frontier is still slotted when its playout
// position has not been consumed yet (design 3.2: "迟到 1~2 包照常消费" --
// this is the direct cure for the small-stutter symptom).  Further behind it
// is dropped: the frame has already been played or concealed (merge timing
// has passed).
#define AJ_LATE_TOLERANCE 2

// ---- levels ----------------------------------------------------------------
// 档 A (0018-tierA): FIXED target level, no IAT histogram.  The hook for the
// official DelayManager behaviour (IAT histogram, quantile 0.96, forget
// factor 0.996, start_forget_weight 2.0, target=(q_packets+1)*20 ms clamped
// to [40,120]) is marked in audio-jitter.cc and lands in 档 B.
#define AJ_TARGET_MS 60
// Pull starts only after this many frames are buffered (design 3.4 preheat,
// 档 A A3 = 6 frames), so playback never starts into a hole.  Steady-state
// buffer depth then hovers around this value (one frame in per tick, one out)
// -- 120 ms, the design's acceptance bound for buffered_ms.
#define AJ_PREHEAT_FRAMES 6
// Depth safety cap (design 3.5 ACCELERATE's "解一帧丢一帧" simplified form):
// when the buffer holds more than this, the pull sheds the excess down to the
// cap immediately.  It brakes three things 档 A has no ACCELERATE for: startup
// bursts (the loop thread drains a socket backlog into one push batch),
// decode-thread stalls, and slow clock drift between the sender and us.
// The margin above AJ_PREHEAT_FRAMES (4 frames) absorbs ordinary network
// clustering so normal operation never sheds.  The real OLA accelerate is
// 档 B (B4).
#define AJ_MAX_BUFFER_FRAMES 10
// 0071 decay-fade curve reused by EXPAND (audio-graph.cc:507): the envelope
// multiplies by 0.8 every 8 samples and dies at 1/256 (~5 ms), so a long gap
// deflates to clean silence instead of a held drone.
#define AJ_EXPAND_DECAY_STEP 8
#define AJ_EXPAND_DECAY_GAIN 0.8f
#define AJ_EXPAND_GAIN_FLOOR (1.f / 256.f)

// Decision modes (aligned with NetEQ's decision_logic; PREEMPTIVE and
// ACCELERATE are reserved names -- 档 A sheds instead of stretching).
typedef enum {
	AJ_MODE_NORMAL = 0,  // decode one real frame
	AJ_MODE_PREEMPTIVE,  // (档 B) stretch 1.15x to buy time when running low
	AJ_MODE_ACCELERATE,  // shed/merge to lose time when running high
	AJ_MODE_PLC,         // gap 1..3: codec concealment (opus PLC)
	AJ_MODE_EXPAND,      // gap 4+: decayed waveform extrapolation (0071 curve)
	AJ_MODE_MERGE,       // gap -> real transition: 20 ms cos/sin crossfade
} aj_mode_t;

// Opaque buffer handle.  The decoder passed to pull() is opaque too (it is
// nx_opus_decoder_t from opus-decoder.h -- named struct here so this header
// stays V8-free).
typedef struct nx_audio_jitter nx_audio_jitter_t;

// Counters for the diagnostic surface (design 4).  Key names on the JS side
// use the official spelling (audio_jitter_*) so the numbers can be lined up
// against the WebRTC getStats() of the official client.  NOTE the pairing
// caveat from the design: stream_underrun_count counts ONE per render QUANTUM
// and is not comparable to concealmentEvents -- use the gap histogram here.
struct aj_stats {
	// push side (network thread)
	uint64_t pushed;
	uint64_t red_unstripped_dropped; // poison gate: pt==63 && red_stripped==0
	uint64_t late_dropped;
	uint64_t dup_dropped;
	uint64_t oversized_dropped; // payload > AJ_MAX_PAYLOAD (or empty)
	// pull side (decode thread)
	uint64_t redundant_used; // RED block refilled a hole and was decoded
	uint64_t fec_used;       // 档 A always 0 (FEC level cut; TODO 档 B B5)
	uint64_t plc_frames;
	uint64_t expand_frames;
	uint64_t merge_frames;
	uint64_t accel_frames;   // frames shed by the depth cap
	uint64_t preempt_frames; // 档 A always 0 (档 B B3)
	// Gap length histogram: how many frames one concealment run lasted.
	// Buckets: 1 / 2 / 3-5 / 6+.  This is the number to reconcile against
	// the official client's concealmentEvents granularity.
	uint64_t gap_hist[4];
	// levels (ms)
	uint32_t target_level_ms; // AJ_TARGET_MS in 档 A (adaptive in 档 B)
	uint32_t buffered_ms;     // current depth: filled unconsumed frames * 20
	uint32_t iat_q96_ms;      // 档 A always 0 (IAT hook; 档 B B1)
};

// ---- lifecycle -------------------------------------------------------------
// `channels` is the decoder's channel count (1 or 2); pull() always PRODUCES
// stereo (a mono decoder is duplicated).  Returns NULL only on allocation
// failure.
nx_audio_jitter_t *nx_audio_jitter_create(int channels);
void nx_audio_jitter_destroy(nx_audio_jitter_t *j);

// ---- network ingest (loop thread) ------------------------------------------
// Feed one RTP audio packet.  `payload` and `redundant` are BORROWED (libpeer
// network buffer, dead when the callback returns) -- both are memcpy'd into
// slots here.  `redundant_ts_offset` is in 48 kHz samples; the redundant
// block's own timestamp is rtp_ts - redundant_ts_offset (960 = one frame).
// `red_stripped` uses libpeer's meaning (peer_connection.h): 1 = this packet
// was RED and the strip produced a clean primary; 0 = a bare Opus packet
// (nothing to strip).  The one toxic combination is payload_type == 63 with
// red_stripped == 0 -- a RED wrapper whose strip did NOT succeed, whose bytes
// are wrapper garbage, not Opus.  The poison gate (design 3.2 step 0) drops
// exactly that combination, so the flag's meaning on the non-63 path is
// never load-bearing.
//
// Returns 0 = buffered, -1 = dropped (poison / late / dup / oversized / the
// sequence jumped so hard the buffer reset).
int nx_audio_jitter_push(nx_audio_jitter_t *j, const uint8_t *payload,
                         size_t size, uint32_t rtp_ts, uint32_t ssrc,
                         uint16_t seq, uint8_t payload_type, int red_stripped,
                         const uint8_t *redundant, size_t redundant_size,
                         uint32_t redundant_ts_offset);

// ---- decode-thread pull (20 ms tick) ---------------------------------------
// Produce exactly AJ_FRAME_SAMPLES * AJ_CHANNELS interleaved stereo floats
// (48 kHz): decode a real frame, conceal (PLC/EXPAND) or crossfade (MERGE).
// `dec` may be NULL, in which case the PLC level is skipped and gaps go
// straight to EXPAND.  The returned mode is for observation.
aj_mode_t nx_audio_jitter_pull(nx_audio_jitter_t *j,
                               float *out_interleaved_960x2,
                               struct nx_opus_decoder *dec);

// Preheat done?  The tick loop must NOT write into the graph ring before this
// is true (design 3.4: play-in would otherwise start on top of a hole).
int nx_audio_jitter_ready(const nx_audio_jitter_t *j);

// ---- stream events ---------------------------------------------------------
// seek / reconnect: empty the buffer and re-arm the preheat.  Counter
// totals are kept (they are session-cumulative); the gap histogram flushes
// any open gap first.  Aligned with nx_audio_stream_flush()'s semantics.
void nx_audio_jitter_reset(nx_audio_jitter_t *j);

void nx_audio_jitter_get_stats(const nx_audio_jitter_t *j,
                               struct aj_stats *out);

// 档 B hook: switch the fixed 档 A target over to an adaptive one (the caller
// owns the IAT histogram then).  Ignored values < AJ_FRAME_MS are clamped.
void nx_audio_jitter_set_target_ms(nx_audio_jitter_t *j, uint32_t ms);

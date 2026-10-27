// audio-jitter.cc -- 0018-tierA packet jitter buffer.  See audio-jitter.h.
//
// The layout follows the design doc's section numbers so the code can be
// checked against it:
//   3.2  packet buffer (256 seq-indexed slots, LATE_TOLERANCE=2)
//   3.4  preheat before the first pull
//   3.5  decision modes (档 A: NORMAL / PLC / EXPAND / MERGE; the two
//        time-stretch modes are shed-only, see the depth cap below)
//   3.6  concealment ladder: RED refill (done in push) -> opus PLC (<=3)
//        -> decayed EXPAND (0071 curve)   [FEC level cut in 档 A]
//   3.7  RED redundant-block refill at push time
//   3.8  MERGE: 20 ms cos/sin equal-power crossfade back onto real audio
//   3.3  IAT-adaptive target level: 档 B (the hook is marked in push)

#include "audio-jitter.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mutex>
#include <new>

// ---------------------------------------------------------------------------
// Decoder seam (opus-decoder.h).
//
// Re-declared here instead of #include "opus-decoder.h": that header drags in
// types.h (V8, libuv, libnx) for its binding declaration, and this module
// must stay V8-free (design 3.1).  The signatures are identical to
// opus-decoder.h -- same types (nx_opus_decoder_t == struct nx_opus_decoder),
// so the mangling matches.  pull() decodes through this seam; everything else
// in this file is self-contained.
// ---------------------------------------------------------------------------
struct nx_opus_decoder;
int nx_opus_decoder_decode(struct nx_opus_decoder *d, const uint8_t *data,
                           size_t size, float *out, int max_frames,
                           int *out_frames);
int nx_opus_decoder_plc(struct nx_opus_decoder *d, float *out, int max_frames,
                        int *out_frames);

// The decoder writes `nb_samples * channels` floats and nb_samples follows
// the PACKET (a 120 ms packet = 5760 samples at 48 kHz) -- its `max_frames`
// argument is a capacity hint it does not enforce, so the scratch buffer has
// to hold the worst case or a multi-frame packet would smash the stack.
constexpr int kDecMaxSamples = (AJ_RTP_CLOCK_HZ * 120) / 1000; // 5760

constexpr float kHalfPi = 1.57079632679489661923f;

// One buffer slot: ONE 20 ms Opus packet (design 3.2).
struct aj_slot {
	bool used = false;
	bool from_redundant = false; // content came from a RED redundant block
	uint16_t seq = 0;
	uint32_t rtp_ts = 0;
	uint16_t size = 0;
	uint8_t data[AJ_MAX_PAYLOAD];
};

// ---------------------------------------------------------------------------
// The buffer (design 3.1/3.2).
//
// TWO sequence frontiers, on purpose (the design doc's expected_seq is used
// for both roles in different paragraphs; keeping them apart is what makes
// "迟到 1~2 包照常消费" possible):
//   expected_seq  INSERT frontier (push side): the next in-order sequence
//                 number; advances across consecutive filled slots.
//   play_seq      PAYOUT frontier (pull side): the frame being played this
//                 tick; advances exactly one per tick (or more when shedding).
// A packet behind expected_seq but still at/after play_seq is "late but not
// yet consumed" -- it gets slotted and plays normally (the stutter cure).
// Anything behind play_seq has been played or concealed already and cannot
// be refilled (the merge timing has passed).
// ---------------------------------------------------------------------------
struct nx_audio_jitter {
	mutable std::mutex mu; // slots + frontiers + counters

	int channels = 2; // decoder channel count (1 or 2); output is stereo

	// ---- packet buffer ----
	aj_slot slots[AJ_CAPACITY];
	bool expected_valid = false;
	uint16_t expected_seq = 0;
	bool play_valid = false;
	uint16_t play_seq = 0;
	bool ssrc_valid = false;
	uint32_t ssrc = 0;

	// ---- levels (档 A: fixed; the IAT hook is in push) ----
	uint32_t target_ms = AJ_TARGET_MS;

	// ---- concealment state (single consumer: the decode thread; also
	//      touched by reset() under the same mutex) ----
	bool concealing = false;          // the current gap has produced output
	uint32_t consecutive_missing = 0; // frames concealed in this gap run
	bool was_expand = false;          // previous tick was EXPAND (decay run)
	float expand_gain = 1.f;          // 0071 envelope, persists across frames
	int expand_samples = 0;           // samples since this EXPAND run began
	float last_real[AJ_FRAME_SAMPLES * AJ_CHANNELS];    // last decoded frame
	float conceal_tail[AJ_FRAME_SAMPLES * AJ_CHANNELS]; // last concealed frame
	float expand_wave[AJ_FRAME_SAMPLES * AJ_CHANNELS];  // waveform to decay
	// Decoder output scratch (channels-interleaved, up to 120 ms).
	float dec_scratch[kDecMaxSamples * AJ_CHANNELS];

	// ---- counters (design 4) ----
	struct aj_stats st;
};

namespace {

// Filled unconsumed frames at/after the playout frontier: the "water level"
// (design 3.5: buffered_ms = 未消费帧数 x 20).
uint32_t buffered_frames_locked(const nx_audio_jitter_t *j) {
	uint32_t n = 0;
	for (int i = 0; i < AJ_CAPACITY; i++) {
		const aj_slot *s = &j->slots[i];
		if (s->used && (int16_t)(s->seq - j->play_seq) >= 0)
			n++;
	}
	return n;
}

// Record the length of the gap run that just ended (design 4 histogram:
// 1 / 2 / 3-5 / 6+).
void record_gap_locked(nx_audio_jitter_t *j) {
	const uint32_t n = j->consecutive_missing;
	if (n == 0)
		return;
	if (n == 1)
		j->st.gap_hist[0]++;
	else if (n == 2)
		j->st.gap_hist[1]++;
	else if (n <= 5)
		j->st.gap_hist[2]++;
	else
		j->st.gap_hist[3]++;
}

// Empty the buffer and re-arm the preheat (design 3.2 step 3/4, 3.4, and the
// public reset).  Counter totals survive; an open gap is flushed into the
// histogram first so its length is not lost.
void reset_locked(nx_audio_jitter_t *j) {
	record_gap_locked(j);
	for (int i = 0; i < AJ_CAPACITY; i++)
		j->slots[i].used = false;
	j->expected_valid = false;
	j->expected_seq = 0;
	j->play_valid = false;
	j->play_seq = 0;
	j->ssrc_valid = false;
	j->ssrc = 0;
	j->concealing = false;
	j->consecutive_missing = 0;
	j->was_expand = false;
	j->expand_gain = 1.f;
	j->expand_samples = 0;
	memset(j->last_real, 0, sizeof(j->last_real));
	memset(j->conceal_tail, 0, sizeof(j->conceal_tail));
	memset(j->expand_wave, 0, sizeof(j->expand_wave));
}

// Arm the two frontiers on `seq` (first packet, or after a reset).
void rebase_locked(nx_audio_jitter_t *j, uint16_t seq) {
	j->expected_valid = true;
	j->expected_seq = seq;
	j->play_valid = true;
	j->play_seq = seq;
}

// Design 3.2: "消费后 expected_seq++ 并顺手消费连续的后继槽" -- the insert
// frontier walks forward across consecutive filled slots, so out-of-order
// arrivals that complete a run immediately pull it along.
void advance_insert_frontier_locked(nx_audio_jitter_t *j) {
	if (!j->expected_valid)
		return;
	for (;;) {
		const aj_slot *s = &j->slots[j->expected_seq % AJ_CAPACITY];
		if (!s->used || s->seq != j->expected_seq)
			break;
		j->expected_seq++;
	}
}

// Decoder output (j->channels interleaved) -> exactly one stereo 960-frame,
// zero-padded or trimmed to size (the timeline model is 20 ms ptime).
void to_stereo_frame(const nx_audio_jitter_t *j, const float *in, int n,
                     float *out) {
	const int ch = j->channels;
	for (int i = 0; i < AJ_FRAME_SAMPLES; i++) {
		if (i < n) {
			out[i * 2] = in[i * ch];
			out[i * 2 + 1] = (ch >= 2) ? in[i * ch + 1] : in[i * ch];
		} else {
			out[i * 2] = 0.f;
			out[i * 2 + 1] = 0.f;
		}
	}
}

} // namespace

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

nx_audio_jitter_t *nx_audio_jitter_create(int channels) {
	// First startup proof (0018).  Once per process: the numbers are
	// compile-time constants and this line is what a device log should grep
	// for when checking that tier A actually shipped.
	static bool announced = false;
	if (!announced) {
		announced = true;
		fprintf(stderr,
		        "[audio-jitter] version=0018-tierA target=%dms preheat=%d "
		        "cap=%dms late_tol=%d\n",
		        AJ_TARGET_MS, AJ_PREHEAT_FRAMES,
		        AJ_MAX_BUFFER_FRAMES * AJ_FRAME_MS, AJ_LATE_TOLERANCE);
	}

	nx_audio_jitter_t *j = new (std::nothrow) nx_audio_jitter();
	if (j == nullptr)
		return nullptr;
	j->channels = (channels == 1) ? 1 : 2;
	j->target_ms = AJ_TARGET_MS;
	return j;
}

void nx_audio_jitter_destroy(nx_audio_jitter_t *j) {
	delete j;
}

// ---------------------------------------------------------------------------
// push (network side / JS loop thread) -- design 3.2 + 3.7
// ---------------------------------------------------------------------------

int nx_audio_jitter_push(nx_audio_jitter_t *j, const uint8_t *payload,
                         size_t size, uint32_t rtp_ts, uint32_t ssrc,
                         uint16_t seq, uint8_t payload_type, int red_stripped,
                         const uint8_t *redundant, size_t redundant_size,
                         uint32_t redundant_ts_offset) {
	if (j == nullptr)
		return -1;

	// ---- 档 B hook (design 3.3, IAT histogram) --------------------------
	// The official DelayManager derives its target level from the packets'
	// inter-arrival times: steady_clock arrival stamps are taken HERE on
	// every push (and on every redundant refill), fed into a 5 ms-bucket
	// histogram with exponential forgetting (0.996, doubled to 0.992 during
	// the first 100 packets / 2 s), and the target becomes
	// (ceil(q96/20)+1)*20 ms clamped to [40,120].  档 A runs the fixed
	// AJ_TARGET_MS instead -- when B1/B2 land, iat_update() goes in this
	// spot and nx_audio_jitter_set_target_ms() is fed from the quantile.
	// --------------------------------------------------------------------

	std::lock_guard<std::mutex> lk(j->mu);

	// Step 0 -- poison gate (design 3.2): an un-strippable RED packet is the
	// wrapper bytes mistaken for Opus.  libpeer drops these on its side too
	// (0018 contract); this is the engine's own line of defence so a future
	// libpeer regression cannot feed garbage to the decoder.
	if (payload_type == 63 && !red_stripped) {
		j->st.red_unstripped_dropped++;
		return -1;
	}
	if (payload == nullptr || size == 0 || size > AJ_MAX_PAYLOAD) {
		j->st.oversized_dropped++;
		return -1;
	}

	// SSRC change = a different stream (design 6: "SSRC 变 → reset").
	if (j->ssrc_valid && j->ssrc != ssrc)
		reset_locked(j);
	j->ssrc = ssrc;
	j->ssrc_valid = true;

	// Design 3.2 step 1: the first packet arms both frontiers.  pull() stays
	// out of the picture until the preheat is satisfied (design 3.4).
	if (!j->expected_valid)
		rebase_locked(j, seq);

	int16_t dist = (int16_t)(seq - j->expected_seq);

	// Design 3.2 step 3: a jump of half the ring is not reordering -- it is
	// a wrap/stream anomaly.  Reset and re-base on this packet.  (The int16
	// difference above is the design 6 wrap-safe distance: sequence numbers
	// are uint16 and wrap at 65535 -> 0.)
	if (dist >= AJ_CAPACITY / 2) {
		reset_locked(j);
		rebase_locked(j, seq);
		dist = 0;
	}

	if (dist < 0) {
		// Behind the insert frontier.
		if (-dist > AJ_LATE_TOLERANCE) {
			// Design 3.2 step 2: too far behind -- by now this frame has
			// certainly been played or concealed (merge timing passed).
			j->st.late_dropped++;
			return -1;
		}
		// Late by 1..2: still useful iff the playout has NOT consumed that
		// position yet ("迟到 1~2 包且该帧还没消费 → 照常入槽" -- the direct
		// cure for the small-stutter symptom).
		if ((int16_t)(seq - j->play_seq) < 0) {
			j->st.late_dropped++;
			return -1;
		}
	}

	aj_slot *s = &j->slots[seq % AJ_CAPACITY];
	if (s->used) {
		if (s->seq == seq && s->rtp_ts == rtp_ts) {
			// Design 3.2 step 4: a plain duplicate.
			j->st.dup_dropped++;
			return -1;
		}
		// Same slot, but a different sequence number (a frame from 256
		// packets ago) or the same sequence number with a different
		// timestamp (wrap/replay): both mean the timeline is no longer the
		// one this buffer was tracking -- design 3.2 step 4 resets.
		reset_locked(j);
		rebase_locked(j, seq);
		s = &j->slots[seq % AJ_CAPACITY];
	}

	s->used = true;
	s->from_redundant = false;
	s->seq = seq;
	s->rtp_ts = rtp_ts;
	s->size = (uint16_t)size;
	memcpy(s->data, payload, size);
	j->st.pushed++;
	advance_insert_frontier_locked(j);

	// ---- RED redundant-block refill (design 3.7) ------------------------
	// The block is the PREVIOUS frame(s) carried piggy-back on this packet
	// (RFC 2198; the official stream is fmtp 63 111/111, one level, offset
	// 960 = one 20 ms frame).  It is copied into the hole it protects NOW,
	// so when the playout arrives the hole is already a real frame.
	//
	// ts index (design 3.7 "拍板：ts 索引为主"): ts and seq advance together
	// (ts = ts0 + 960*(seq-seq0) at 20 ms ptime), so the redundant block's
	// sequence position is exact arithmetic from the offset -- no scan, and
	// multi-level offsets land on the right slot instead of blindly on
	// seq-1.  The seq-1 "fast path" is just the offset==960 case.
	if (redundant != nullptr && redundant_size > 0 &&
	    redundant_size <= AJ_MAX_PAYLOAD) {
		// Only whole-frame offsets map exactly onto a sequence position;
		// anything else is a guess, and a wrong guess fills the wrong hole,
		// so those simply skip (the hole falls back to PLC).
		if ((redundant_ts_offset % AJ_FRAME_TS_STEP) == 0) {
			uint32_t back = redundant_ts_offset / AJ_FRAME_TS_STEP;
			if (back < 1)
				back = 1;
			if (back < AJ_CAPACITY / 2) {
				const uint16_t r_seq = (uint16_t)(seq - (uint16_t)back);
				const uint32_t r_ts = rtp_ts - redundant_ts_offset;
				// Only a hole whose playout has not passed can be refilled
				// -- a frame already concealed is exactly the "merge 时机
				// 已过" case of design 3.7.  An already-used slot (real
				// packet won the race, or a stale frame) is left alone.
				if ((int16_t)(r_seq - j->play_seq) >= 0) {
					aj_slot *rs = &j->slots[r_seq % AJ_CAPACITY];
					if (!rs->used) {
						rs->used = true;
						rs->from_redundant = true;
						rs->seq = r_seq;
						rs->rtp_ts = r_ts;
						rs->size = (uint16_t)redundant_size;
						memcpy(rs->data, redundant, redundant_size);
						advance_insert_frontier_locked(j);
					}
				}
			}
		}
	}
	return 0;
}

// ---------------------------------------------------------------------------
// pull (decode thread, 20 ms tick) -- design 3.4/3.5/3.6/3.8
// ---------------------------------------------------------------------------

aj_mode_t nx_audio_jitter_pull(nx_audio_jitter_t *j, float *out,
                               struct nx_opus_decoder *dec) {
	memset(out, 0, sizeof(float) * AJ_FRAME_SAMPLES * AJ_CHANNELS);
	if (j == nullptr)
		return AJ_MODE_EXPAND;

	std::lock_guard<std::mutex> lk(j->mu);
	if (!j->play_valid)
		return AJ_MODE_EXPAND; // before preheat / after reset; loop gates on
		                       // nx_audio_jitter_ready()

	// ---- depth safety cap (design 3.5 ACCELERATE, simplified) ----------
	// One extra frame is shed per unit of depth over the cap, all in this
	// tick: a burst (thread stall, post-stall catch-up) drops back to the
	// live edge immediately instead of becoming permanent latency.  The OLA
	// time-stretch version is 档 B (B4/B3); this is the "解一帧丢一帧"
	// simplification design 3.9 explicitly allows.
	while (buffered_frames_locked(j) > AJ_MAX_BUFFER_FRAMES) {
		aj_slot *s = &j->slots[j->play_seq % AJ_CAPACITY];
		s->used = false; // a real frame (dropped) or a stale collision (freed)
		j->play_seq++;
		j->st.accel_frames++;
	}

	aj_slot *s = &j->slots[j->play_seq % AJ_CAPACITY];
	if (s->used && s->seq != j->play_seq)
		s->used = false; // never decode a slot that is not ours
	const bool have = s->used && s->seq == j->play_seq;
	const bool from_red = have && s->from_redundant;

	// ---- decode the head frame (if there is one) -----------------------
	float frame[AJ_FRAME_SAMPLES * AJ_CHANNELS];
	bool real = false;
	if (have && dec != nullptr) {
		int n = 0;
		if (nx_opus_decoder_decode(dec, s->data, s->size, j->dec_scratch,
		                           kDecMaxSamples, &n) == 0 &&
		    n > 0) {
			to_stereo_frame(j, j->dec_scratch, n, frame);
			if (n != AJ_FRAME_SAMPLES) {
				// The timeline model is one 20 ms frame per RTP packet /
				// per pull (design 3.1).  Anything else is trimmed/padded;
				// say so once so a device log can catch a ptime surprise.
				static bool warned = false;
				if (!warned) {
					warned = true;
					fprintf(stderr,
					        "[audio-jitter] frame_len=%d (want %d) "
					        "trimmed/padded; 20ms ptime assumed\n",
					        n, AJ_FRAME_SAMPLES);
				}
			}
			real = true;
		}
		// decode failure (corrupt payload) falls through to concealment
	}

	aj_mode_t mode;

	if (real) {
		// ---- design 3.6 level 1 verdict: a RED-refilled frame decodes
		//      exactly like the real one would have.
		if (from_red)
			j->st.redundant_used++;
		memcpy(j->last_real, frame, sizeof(frame));
		if (j->concealing) {
			// ---- MERGE (design 3.8) --------------------------------
			// End of a gap: crossfade the concealed tail into the real
			// frame over one full frame with equal-power cos/sin curves.
			// No hard cut, and no double-fade against audio-graph's
			// conceal_gain (0071) -- that one only fires if the ring
			// underruns, which this layer exists to prevent.
			for (int i = 0; i < AJ_FRAME_SAMPLES; i++) {
				const float t = (float)i / (float)AJ_FRAME_SAMPLES;
				const float g_old = cosf(t * kHalfPi);
				const float g_new = sinf(t * kHalfPi);
				out[i * 2] = j->conceal_tail[i * 2] * g_old +
				             frame[i * 2] * g_new;
				out[i * 2 + 1] = j->conceal_tail[i * 2 + 1] * g_old +
				                 frame[i * 2 + 1] * g_new;
			}
			j->st.merge_frames++;
			record_gap_locked(j); // the gap run ends here
			j->concealing = false;
			j->consecutive_missing = 0;
			j->was_expand = false;
			j->expand_gain = 1.f;
			j->expand_samples = 0;
			mode = AJ_MODE_MERGE;
		} else {
			memcpy(out, frame, sizeof(frame));
			mode = AJ_MODE_NORMAL;
		}
	} else {
		// ---- gap at the playout frontier: conceal (design 3.6) ------
		// Level 1 (RED refill) already happened at push time or the block
		// never existed.  Level 2 (inband FEC, decode the NEXT packet with
		// decode_fec=1) is cut in 档 A -- TODO 档 B B5: switch-ffmpeg 7.1's
		// native opus FEC decode path is unverified; when it lands, look at
		// the slot for play_seq+1 and recover the frame from it before PLC.
		j->concealing = true;
		j->consecutive_missing++;
		if (j->consecutive_missing == 1) {
			// Fresh gap: the waveform to fall back on is the last real
			// frame (EXPAND may need it if PLC output never comes).
			memcpy(j->expand_wave, j->last_real, sizeof(j->expand_wave));
		}

		bool plc_ok = false;
		if (j->consecutive_missing <= 3 && dec != nullptr) {
			int n = 0;
			if (nx_opus_decoder_plc(dec, j->dec_scratch, kDecMaxSamples,
			                        &n) == 0 &&
			    n > 0) {
				to_stereo_frame(j, j->dec_scratch, n, frame);
				plc_ok = true;
			}
		}

		if (plc_ok) {
			// ---- level 3: codec concealment (design 3.6).  Only for the
			//      first 1..3 frames of a gap: the synthesis is fresh and
			//      plausible there; after that it is not trusted.
			memcpy(out, frame, sizeof(frame));
			memcpy(j->expand_wave, frame,
			       sizeof(j->expand_wave)); // freshest synth for EXPAND
			j->st.plc_frames++;
			j->was_expand = false;
			mode = AJ_MODE_PLC;
		} else {
			// ---- level 4: EXPAND (design 3.6 / 0071 decay-fade) -----
			// Repeat the last waveform under an envelope that multiplies by
			// 0.8 every 8 samples and dies at 1/256 (~5 ms) -- the exact
			// curve audio-graph.cc:507 applies on a ring underrun, moved
			// here so the ring's fallback is no longer part of normal
			// playback.  A long gap deflates to clean silence.
			if (!j->was_expand) {
				j->expand_gain = 1.f;
				j->expand_samples = 0;
			}
			float g = j->expand_gain;
			int es = j->expand_samples;
			for (int i = 0; i < AJ_FRAME_SAMPLES; i++) {
				if (es > 0 && (es % AJ_EXPAND_DECAY_STEP) == 0) {
					g *= AJ_EXPAND_DECAY_GAIN;
					if (g < AJ_EXPAND_GAIN_FLOOR)
						g = 0.f;
				}
				out[i * 2] = j->expand_wave[i * 2] * g;
				out[i * 2 + 1] = j->expand_wave[i * 2 + 1] * g;
				es++;
			}
			j->expand_gain = g;
			j->expand_samples = es;
			j->was_expand = true;
			j->st.expand_frames++;
			mode = AJ_MODE_EXPAND;
		}
		// Every concealed output is kept for the MERGE crossfade (3.8).
		memcpy(j->conceal_tail, out, sizeof(j->conceal_tail));
	}

	// The head position is consumed either way: a real frame (or a hole)
	// has now been played.  This is the only place play_seq advances apart
	// from the shed loop above.
	s->used = false;
	j->play_seq++;
	return mode;
}

// ---------------------------------------------------------------------------
// introspection / stream events
// ---------------------------------------------------------------------------

int nx_audio_jitter_ready(const nx_audio_jitter_t *j) {
	if (j == nullptr)
		return 0;
	std::lock_guard<std::mutex> lk(j->mu);
	if (!j->play_valid)
		return 0;
	return buffered_frames_locked(j) >= AJ_PREHEAT_FRAMES;
}

void nx_audio_jitter_reset(nx_audio_jitter_t *j) {
	if (j == nullptr)
		return;
	std::lock_guard<std::mutex> lk(j->mu);
	reset_locked(j);
}

void nx_audio_jitter_get_stats(const nx_audio_jitter_t *j,
                               struct aj_stats *out) {
	if (out == nullptr)
		return;
	memset(out, 0, sizeof(*out));
	if (j == nullptr)
		return;
	std::lock_guard<std::mutex> lk(j->mu);
	*out = j->st;
	out->target_level_ms = j->target_ms;
	out->buffered_ms = buffered_frames_locked(j) * AJ_FRAME_MS;
	// iat_q96_ms stays 0 in 档 A (the IAT histogram is 档 B / design 3.3).
}

void nx_audio_jitter_set_target_ms(nx_audio_jitter_t *j, uint32_t ms) {
	if (j == nullptr)
		return;
	if (ms < AJ_FRAME_MS)
		ms = AJ_FRAME_MS;
	std::lock_guard<std::mutex> lk(j->mu);
	j->target_ms = ms;
}

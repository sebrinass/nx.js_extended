// Raw Opus (RFC 6716) decoding.  See opus-decoder.h for the rationale.
//
// The decoder is ffmpeg's NATIVE opus decoder (ff_opus_decoder).  The
// Switch's switch-ffmpeg 7.1 build ships it -- verified at the symbol
// level with
//
//   aarch64-none-elf-nm --defined-only \
//     C:/devkitPro/portlibs/switch/lib/libavcodec.a
//   -> D ff_opus_decoder, T ff_opus_parse_packet, T opus_decode_packet
//
// so there is no external libopus to add and no new portlibs dependency.
#include "opus-decoder.h"

#include "audio-graph.h"
#include "audio-jitter.h"
#include "audio.h"
#include "error.h"
#include "util.h"
#include "wrap.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

using namespace v8;

namespace {

// Opus always decodes at 48 kHz (RFC 6716 section 4).
constexpr uint32_t kOpusRate = 48000;

// Longest Opus packet we accept: 120 ms of audio (the RFC's maximum frame
// duration is 60 ms, and a packet may carry up to 48 frames).
constexpr int kMaxPacketMs = 120;
constexpr int kMaxFrames = (kOpusRate * kMaxPacketMs) / 1000;

// --- decode thread (B1, 0062; reworked by 0018) ----------------------------
//
// nx_opus_track_push used to decode inline on the JS main thread.  On device
// the main thread is blocked by video-decode long tasks (p50 73 ms / p90
// 390 ms / max 671 ms), so audio packets arrived in bursts separated by dead
// air -- the ring drained dry between bursts and no amount of prebuffer
// tuning could cover a 671 ms stall.  Moonlight solves this with a packet
// queue + a dedicated decode thread (moonlight-common-c AudioStream.c:385
// + the XITRIX renderer), so 0062 copied that architecture.
//
// 0018 replaces the queue with the jitter buffer (audio-jitter.{h,cc}) and
// turns the decode thread into a 20 ms TICK: every tick pulls exactly one
// 960-sample stereo frame from the jitter buffer (real packet / RED refill /
// PLC / EXPAND / MERGE, see the design doc) and writes it into the graph
// ring.  The tick is a perfect clock on the ring's input side, so network
// jitter is absorbed by the jitter buffer and the ring's underrun path
// (audio-graph.cc:519, 0071 decay-fade) is only ever a last-resort fallback
// -- normally it never fires.
//
//   - the network side (JS loop thread, inside the webrtc backend's
//     onaudiopacket callback) calls nx_opus_track_push_net(), which copies
//     the packet (+ its RED redundant block) straight into a jitter slot;
//   - this thread pulls/conceals on its 20 ms cadence and writes the ring;
//   - the thread is created with the track and joined in destroy.

// --- prebuffer ("prime") policy ---------------------------------------------
//
// The stream-source node is drained by the audio thread the moment it starts
// playing, while the producer only pushes when the JS main thread gets a frame
// (p50 50 ms / p90 76 ms / max 1488 ms on device).  Without a pre-charge the
// 1 s ring sits at ~0 frames, so every render quantum that arrives before the
// next push is short (partial underrun -> silence padding) -- which is heard as
// chopped audio.
//
// Holding the gate shut until the ring holds NX_OPUS_PRIME_TARGET_MS of audio
// turns that into one clean startup delay.  200 ms is ~2.6x the p90 frame gap
// (76 ms) and the 1 s ring holds it with room to spare, so the steady-state
// ring can absorb a whole late frame without ever going short.
//
// 300 ms (raised from 200 on device evidence, 2026-10-26): the on-device
// session still saw 2012 underruns, with `pending` swinging 0..581 ms and
// JS main-thread long tasks up to 671 ms -- 200 ms could not cover a single
// long task, let alone the tail.  300 ms buys headroom against the observed
// worst-case stall while keeping startup latency imperceptible.
#ifndef NX_OPUS_PRIME_TARGET_MS
#define NX_OPUS_PRIME_TARGET_MS 300
#endif

// Give up waiting for a ring that never fills (bad target, stalled producer)
// after this many pushes -- permanent silence is worse than a short quantum.
// At the 50 pps the cloud sends this is ~3 s.
#ifndef NX_OPUS_PRIME_MAX_PUSHES
#define NX_OPUS_PRIME_MAX_PUSHES 150
#endif

// 0066: the old NX_OPUS_MAX_REPRIMES cap is gone with the mid-stream
// re-prime branch it bounded -- the gate is never re-armed by a drain any
// more, only by a play() after a pause.  (0067 removed the A1 backlog flush
// that used to arm it mid-stream too: on device it fired 102 times in 45
// minutes, and every firing was itself a 100 ms hole.)

// A3 (0064): fill target for a RE-charge, in ms.  The first prime still
// waits for the full NX_OPUS_PRIME_TARGET_MS (300 ms), but a re-charge only
// waits for 100 ms (4800 frames at 48 kHz): 0060's on-device session
// re-primed 22 times x 300 ms = 6.6 s of self-inflicted silence, and most
// of that wall was the target, not the number of re-charges.  A real break
// now costs one clean 100 ms gap.
#ifndef NX_OPUS_REPRIME_TARGET_MS
#define NX_OPUS_REPRIME_TARGET_MS 100
#endif

// A2 (0065): the decode-thread concealment ("stream dark -> decay-fade the
// last frame into silence") is GONE with 0018.  The jitter pull produces a
// frame on every tick no matter what -- real, PLC, or the same decay-fade
// curve as EXPAND (design 3.6 level 4) -- so the producer side never goes
// dark and the ring never needs this second fade layer.  Keeping it would
// double-fade the same audio against the jitter layer's MERGE crossfade.

// Duration of ONE frame, in tenths of a millisecond, from the TOC
// configuration number (RFC 6716 section 3.2.1, Table 2).
//
//   0..11   SILK only, NB/MB/WB -> 10, 20, 40, 60 ms
//   12..15  Hybrid, SWB/FB      -> 10, 20 ms
//   16..31  CELT only           -> 2.5, 5, 10, 20 ms
//
// Tenths of a millisecond because 2.5 ms is not a whole millisecond.
int frameTenthsMs(int config) {
	if (config < 12) {
		static const int kSilk[4] = {100, 200, 400, 600};
		return kSilk[config & 3];
	}
	if (config < 16) {
		static const int kHybrid[2] = {100, 200};
		return kHybrid[config & 1];
	}
	static const int kCelt[4] = {25, 50, 100, 200};
	return kCelt[config & 3];
}

// True for the configurations whose frame DURATION is fixed by the TOC.
// A CELT frame's duration follows from its byte size instead, so the packet
// duration is only an estimate for CELT.
bool configHasFixedDuration(int config) { return config < 16; }

} // namespace

// --- Opus packet structure (RFC 6716 section 3.2) ---------------------------

bool nx_opus_packet_parse(const uint8_t *data, size_t size,
                          nx_opus_packet_info *out) {
	if (!data || size < 1 || !out)
		return false;

	*out = nx_opus_packet_info{};
	const uint8_t toc = data[0];
	out->config = (toc >> 3) & 0x1f;
	out->frame_code = toc & 0x07;
	out->frame_tenths_ms = frameTenthsMs(out->config);

	switch (out->frame_code) {
	case 0:
		out->frame_count = 1;
		break;
	case 1:
	case 2:
		out->frame_count = 2;
		break;
	case 3:
		// Reserved (RFC 6716 Table 3) -- not a valid packet.
		return false;
	default:
		out->frame_count = out->frame_code - 3;
		break;
	}

	out->total_tenths_ms = out->frame_tenths_ms * out->frame_count;
	out->samples = (uint64_t)out->frame_tenths_ms * 48ull / 10ull *
	               (uint64_t)out->frame_count;
	out->exact = configHasFixedDuration(out->config);
	return true;
}

// --- The decoder ------------------------------------------------------------

struct nx_opus_decoder {
	AVCodecContext *ctx = nullptr;
	AVPacket *pkt = nullptr;
	AVFrame *frame = nullptr;
	int channels = 2;
	uint32_t sample_rate = kOpusRate;
	// B1 (0062): decoding now runs on the track's own thread while these
	// counters are read from the JS thread (opusTrackStats) -- so the integer
	// counters are atomics and the loudness accumulators are mutex-guarded
	// (they are doubles; a torn read would poison RMS forever).  The mutex
	// is mutable: the getters take a const pointer.
	std::atomic<uint64_t> packets{0};
	std::atomic<uint64_t> frames{0};
	std::atomic<uint64_t> errors{0};
	mutable std::mutex stats_mutex;
	double sum_sq = 0;
	double peak = 0;
	// 0018: PLC probe state (see nx_opus_decoder_plc).  Only touched from
	// the track's decode thread (the jitter pull is the single caller).
	bool plc_probe_done = false; // a flush-packet PLC was attempted once
	bool plc_disabled = false;   // no concealment from this build's opus
};

nx_opus_decoder_t *nx_opus_decoder_create(int channels, char *errbuf,
                                          size_t errbuf_size) {
	if (channels < 1)
		channels = 1;
	if (channels > 2)
		channels = 2;

	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
	if (!codec) {
		if (errbuf && errbuf_size)
			snprintf(errbuf, errbuf_size,
			         "this ffmpeg build has no opus decoder");
		return nullptr;
	}

	nx_opus_decoder_t *d = new nx_opus_decoder();
	d->channels = channels;

	d->ctx = avcodec_alloc_context3(codec);
	if (!d->ctx) {
		if (errbuf && errbuf_size)
			snprintf(errbuf, errbuf_size, "avcodec_alloc_context3 failed");
		delete d;
		return nullptr;
	}

	// Opus is always 48 kHz; the channel count comes from the SDP
	// ("a=rtpmap:<pt> opus/48000/2" -> 2).
	av_channel_layout_default(&d->ctx->ch_layout, channels);
	d->ctx->sample_rate = (int)kOpusRate;

	if (avcodec_open2(d->ctx, codec, nullptr) < 0) {
		if (errbuf && errbuf_size)
			snprintf(errbuf, errbuf_size, "avcodec_open2(opus) failed");
		avcodec_free_context(&d->ctx);
		delete d;
		return nullptr;
	}

	d->pkt = av_packet_alloc();
	d->frame = av_frame_alloc();
	if (!d->pkt || !d->frame) {
		if (errbuf && errbuf_size)
			snprintf(errbuf, errbuf_size, "av_packet_alloc failed");
		nx_opus_decoder_destroy(d);
		return nullptr;
	}
	return d;
}

void nx_opus_decoder_destroy(nx_opus_decoder_t *d) {
	if (!d)
		return;
	if (d->frame)
		av_frame_free(&d->frame);
	if (d->pkt)
		av_packet_free(&d->pkt);
	if (d->ctx)
		avcodec_free_context(&d->ctx);
	delete d;
}

int nx_opus_decoder_decode(nx_opus_decoder_t *d, const uint8_t *data,
                            size_t size, float *out, int max_frames,
                            int *out_frames) {
	if (out_frames)
		*out_frames = 0;
	if (!d || !d->ctx || !data || size == 0 || !out || max_frames <= 0)
		return -1;

	// The RTP payload of an Opus packet IS the Opus packet (RFC 7587), so
	// it goes to the decoder unchanged.
	d->pkt->data = const_cast<uint8_t *>(data);
	d->pkt->size = (int)size;
	int ret = avcodec_send_packet(d->ctx, d->pkt);
	// 0018: rescue from a PLC probe's flush semantics (see
	// nx_opus_decoder_plc).  A zero-size "flush packet" leaves the decoder
	// in draining mode (avcodec.h: "the decoder has been flushed, and no
	// new packets can be sent"), so the FIRST real packet after a probe
	// fails to send.  One avcodec_flush_buffers() + retry brings the
	// decoder back; PLC is then off for good (a one-shot concealment per
	// gap is all the flush trick can ever give).
	if (ret < 0 && d->plc_probe_done &&
	    (ret == AVERROR_EOF || ret == AVERROR(EINVAL))) {
		static bool rescued = false;
		if (!rescued) {
			rescued = true;
			fprintf(stderr,
			        "[audio-jitter] plc_probe left the decoder draining; "
			        "rescued with avcodec_flush_buffers\n");
		}
		avcodec_flush_buffers(d->ctx);
		d->plc_disabled = true;
		d->plc_probe_done = false;
		ret = avcodec_send_packet(d->ctx, d->pkt);
	}
	// The packet buffer is only valid during the call.
	d->pkt->data = nullptr;
	d->pkt->size = 0;
	d->packets.fetch_add(1, std::memory_order_relaxed);
	if (ret < 0) {
		d->errors.fetch_add(1, std::memory_order_relaxed);
		return -1;
	}

	int total = 0;
	for (;;) {
		ret = avcodec_receive_frame(d->ctx, d->frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			break;
		if (ret < 0) {
			d->errors++;
			break;
		}

		const int nb = d->frame->nb_samples;
		const int ch = d->frame->ch_layout.nb_channels;
		if (nb <= 0 || ch <= 0) {
			av_frame_unref(d->frame);
			continue;
		}
		if (total + nb > max_frames) {
			// Cannot happen for a well-formed Opus packet (120 ms cap),
			// but never write past the caller's buffer.
			d->errors.fetch_add(1, std::memory_order_relaxed);
			av_frame_unref(d->frame);
			break;
		}

		// The native opus decoder emits planar float (FLTP); handle the
		// interleaved case too so this keeps working if the sample format
		// ever changes.
		if (d->frame->format == AV_SAMPLE_FMT_FLTP) {
			for (int i = 0; i < nb; i++) {
				for (int c = 0; c < ch; c++) {
					const float v = reinterpret_cast<const float *>(
					                    d->frame->extended_data[c])[i];
					out[(total + i) * ch + c] = v;
				}
			}
		} else if (d->frame->format == AV_SAMPLE_FMT_FLT) {
			const float *src =
			    reinterpret_cast<const float *>(d->frame->data[0]);
			for (int i = 0; i < nb * ch; i++)
				out[total * ch + i] = src[i];
		} else {
			d->errors.fetch_add(1, std::memory_order_relaxed);
			av_frame_unref(d->frame);
			break;
		}

		{
			// Loudness accumulators are read from the JS thread.
			std::lock_guard<std::mutex> lock(d->stats_mutex);
			for (int i = 0; i < nb * ch; i++) {
				const float v = out[total * ch + i];
				d->sum_sq += (double)v * (double)v;
				const double a = fabs((double)v);
				if (a > d->peak)
					d->peak = a;
			}
		}
		total += nb;
		av_frame_unref(d->frame);
	}

	d->frames.fetch_add((uint64_t)total, std::memory_order_relaxed);
	if (out_frames)
		*out_frames = total;
	return 0;
}

// 0018: packet-loss concealment (design 3.6 level 3; A5).
//
// libopus exposes PLC as "decode a NULL packet".  libavcodec's send/receive
// API has NO per-packet loss signal: avcodec.h states an empty packet
// (data NULL, size 0) "is considered a flush packet, which signals the end
// of the stream".  What actually happens is still useful for one frame --
// the codec's decode callback runs once with a zero-size payload, which is
// exactly the libopus-style PLC trigger -- but afterwards the decoder is in
// draining mode and only avcodec_flush_buffers() brings it back (see the
// rescue in nx_opus_decoder_decode above).
//
// So this is a one-shot-per-gap probe:
//   - the first call may return ONE concealed frame (when this ffmpeg
//     build's opus decoder conceals for zero-size input);
//   - any later call fails fast and disables PLC;
//   - the caller (audio-jitter pull) falls back to its PCM-domain EXPAND
//     whenever *out_frames comes back 0 -- which is the degradation the
//     design explicitly allows ("不行就只用 EXPAND 级").
// Returns 0 when the call ran (*out_frames may still be 0) and -1 when PLC
// is unavailable.  The decoder stays usable either way.
int nx_opus_decoder_plc(nx_opus_decoder_t *d, float *out, int max_frames,
                        int *out_frames) {
	if (out_frames)
		*out_frames = 0;
	if (!d || !d->ctx || !out || max_frames <= 0)
		return -1;
	if (d->plc_disabled)
		return -1;

	d->plc_probe_done = true;
	d->pkt->data = nullptr;
	d->pkt->size = 0;
	int ret = avcodec_send_packet(d->ctx, d->pkt); // empty non-NULL = flush
	if (ret < 0) {
		// Already drained (AVERROR_EOF) or unusable -- no PLC from here.
		d->plc_disabled = true;
		return -1;
	}

	int total = 0;
	for (;;) {
		ret = avcodec_receive_frame(d->ctx, d->frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			break;
		if (ret < 0)
			break;
		const int nb = d->frame->nb_samples;
		const int ch = d->frame->ch_layout.nb_channels;
		if (nb <= 0 || ch <= 0) {
			av_frame_unref(d->frame);
			continue;
		}
		if (total + nb > max_frames) {
			av_frame_unref(d->frame);
			break;
		}
		if (d->frame->format == AV_SAMPLE_FMT_FLTP) {
			for (int i = 0; i < nb; i++)
				for (int c = 0; c < ch; c++)
					out[(total + i) * ch + c] =
					    reinterpret_cast<const float *>(
					        d->frame->extended_data[c])[i];
		} else if (d->frame->format == AV_SAMPLE_FMT_FLT) {
			const float *src =
			    reinterpret_cast<const float *>(d->frame->data[0]);
			for (int i = 0; i < nb * ch; i++)
				out[total * ch + i] = src[i];
		} else {
			av_frame_unref(d->frame);
			break;
		}
		total += nb;
		av_frame_unref(d->frame);
	}

	// One-shot probe verdict for device triage ("[audio-jitter]" lines are
	// the 0018 grep surface).
	static bool probed = false;
	if (!probed) {
		probed = true;
		fprintf(stderr, "[audio-jitter] plc_probe ret=%d frames=%d\n", ret,
		        total);
	}
	if (total == 0) {
		// Nothing concealed -> this build has no usable PLC; never touch
		// the flush path again.
		d->plc_disabled = true;
	}
	if (out_frames)
		*out_frames = total;
	return 0;
}

int nx_opus_decoder_channels(const nx_opus_decoder_t *d) {
	return d ? d->channels : 0;
}

uint32_t nx_opus_decoder_sample_rate(const nx_opus_decoder_t *d) {
	return d ? d->sample_rate : 0;
}

uint64_t nx_opus_decoder_frames(const nx_opus_decoder_t *d) {
	return d ? d->frames.load(std::memory_order_relaxed) : 0;
}

uint64_t nx_opus_decoder_packets(const nx_opus_decoder_t *d) {
	return d ? d->packets.load(std::memory_order_relaxed) : 0;
}

double nx_opus_decoder_rms(const nx_opus_decoder_t *d) {
	if (!d)
		return 0;
	std::lock_guard<std::mutex> lock(d->stats_mutex);
	const uint64_t frames = d->frames.load(std::memory_order_relaxed);
	if (frames == 0)
		return 0;
	return sqrt(d->sum_sq / (double)frames);
}

double nx_opus_decoder_peak(const nx_opus_decoder_t *d) {
	if (!d)
		return 0;
	std::lock_guard<std::mutex> lock(d->stats_mutex);
	return d->peak;
}

uint64_t nx_opus_decoder_errors(const nx_opus_decoder_t *d) {
	return d ? d->errors.load(std::memory_order_relaxed) : 0;
}

// --- The live track ---------------------------------------------------------

struct nx_opus_track {
	nx_opus_decoder_t *dec = nullptr;
	nx_audio_node *node = nullptr;
	SwrContext *swr = nullptr;
	int channels = 2;
	uint32_t graph_rate = kOpusRate;
	// B1 (0062): set from the JS thread, read on both threads.
	std::atomic<bool> destroyed{false};
	std::vector<float> converted; // interleaved stereo float32 at graph_rate
	// A small ring of the newest decoded PCM, so a diagnostic page can draw
	// a waveform without the track having to hand out the whole stream.
	// B1 (0062): written by the decode thread, read by the JS thread
	// (opusTrackRead) -- guarded by state_mutex.
	std::vector<float> tap;
	uint64_t tap_write = 0;

	// --- prebuffer ("prime") state --------------------------------------
	// B1 (0062): this state machine used to run only on pushes from the JS
	// thread; now the decode thread runs it after every packet and the JS
	// thread still runs it on set_playing() -- so it is guarded by
	// state_mutex.  The render thread never reads these.
	uint32_t target_frames = 0;  // pre-charge target in graph frames (0 = off)
	// A3 (0064): the gate's CURRENT fill target -- the full prebuffer target
	// for the first prime, the short re-charge target afterwards (a re-charge
	// must not re-create the 300 ms wall of silence; see 0060's postmortem).
	uint32_t cur_target = 0;
	uint32_t reprime_target_frames = 0; // re-charge target in graph frames
	std::atomic<bool> want_playing{false}; // what JS asked for (play()/pause())
	bool priming = false;        // gate held shut until pending >= target_frames
	uint32_t prime_pushes = 0;   // pushes spent in the current prime attempt
	uint32_t reprime_count = 0;  // times the gate was re-armed after a drain
	uint32_t prime_timeouts = 0; // prime attempts abandoned at the push cap
	uint64_t underrun_seen = 0;  // underrun count at the previous push
	uint64_t consumed_seen = 0;  // media clock at the previous push

	// --- B1 (0062): guards the prebuffer state machine + the tap ---------
	std::mutex state_mutex;

	// --- 0018: packet jitter buffer + 20 ms decode tick -----------------
	// Replaces the 0062 bounded packet queue and its event-driven decode
	// loop.  The network side (JS loop thread, via
	// nx_opus_track_push_net) pushes every RTP packet -- with its
	// timestamp/sequence and RED redundant block -- into the jitter buffer;
	// the decode thread pulls one 960-sample stereo frame per 20 ms tick
	// (real / concealed / merged) and writes the graph ring.  See
	// audio-jitter.h and .local/rev-audio/jitter-buffer-design.md.
	nx_audio_jitter_t *jit = nullptr;
	// Legacy-push continuation state (nx_opus_track_push, no RTP
	// metadata): each call is placed one synthetic 20 ms frame further
	// along a private timeline.  net_seen ends that path for good -- once
	// real network traffic has been seen, the jitter timeline belongs to
	// it and interleaving synthesized frames would corrupt the ordering.
	bool net_seen = false;
	uint16_t synth_seq = 0;
	uint32_t synth_ts = 0;
	// Tick/quit handshake (0062 legacy field names): the thread parks on
	// this cv until the next tick or a destroy.
	std::mutex q_mutex;
	std::condition_variable q_cv;
	std::thread thread;
	std::atomic<bool> quit{false};
};

// How many stereo frames the diagnostic tap keeps.
constexpr uint32_t kTapFrames = 4096;

namespace {

// Build the resampler that turns the decoder's output (1 or 2 channels at
// 48 kHz) into the graph's bus format (stereo float32 at the graph rate).
// Returns NULL when no conversion is needed.
SwrContext *make_swr(int in_channels, uint32_t in_rate, int out_channels,
                     uint32_t out_rate) {
	if (in_rate == out_rate && in_channels == out_channels)
		return nullptr;
	SwrContext *swr = nullptr;
	AVChannelLayout in_layout;
	av_channel_layout_default(&in_layout, in_channels);
	AVChannelLayout out_layout;
	av_channel_layout_default(&out_layout, out_channels);
	if (swr_alloc_set_opts2(&swr, &out_layout, AV_SAMPLE_FMT_FLT, (int)out_rate,
	                        &in_layout, AV_SAMPLE_FMT_FLT, (int)in_rate, 0,
	                        nullptr) < 0)
		return nullptr;
	if (swr_init(swr) < 0) {
		swr_free(&swr);
		return nullptr;
	}
	return swr;
}

} // namespace

// B1 (0062): forward declaration -- the thread body is defined below the
// create function, which spawns it.
static void opus_track_thread_main(nx_opus_track_t *t);

// ---------------------------------------------------------------------------
// 0018: the network ingest route.
//
// The webrtc backend's onaudiopacket callback (webrtc_backend_libpeer.cc,
// JS loop thread) delivers every audio RTP packet with its metadata straight
// into THE live Opus track -- the old "event queue -> JS -> track.push"
// round-trip is gone with the on_audio_track chain (the two libpeer routes
// are else-if exclusive, so registering onaudiopacket stops the old one).
//
// The registry is the seam between "which track" and "the only track": the
// FIRST track created wins and destroy unregisters it.  Single audio track
// per session is the app's topology (app.js AudioOut); a second track simply
// does not receive network audio.  Guarded by a mutex even though create /
// destroy / the callback all run on the loop thread -- the lock is cheap and
// a GC finalizer timing assumption is not worth betting a crash on.
namespace {
std::mutex g_net_sink_mutex;
nx_opus_track_t *g_net_sink = nullptr;
} // namespace

int nx_opus_track_push_net(const uint8_t *data, size_t size, uint32_t rtp_ts,
                           uint32_t ssrc, uint16_t seq, uint8_t payload_type,
                           int red_stripped, const uint8_t *redundant,
                           size_t redundant_size,
                           uint32_t redundant_ts_offset) {
	nx_opus_track_t *t;
	{
		std::lock_guard<std::mutex> lock(g_net_sink_mutex);
		t = g_net_sink;
	}
	if (!t || t->destroyed.load(std::memory_order_relaxed) || !t->jit)
		return -1;
	if (!t->net_seen)
		t->net_seen = true; // legacy pushes are off from here on
	return nx_audio_jitter_push(t->jit, data, size, rtp_ts, ssrc, seq,
	                            payload_type, red_stripped, redundant,
	                            redundant_size, redundant_ts_offset);
}

nx_opus_track_t *nx_opus_track_create(nx_audio_graph *graph, int channels,
                                       char *errbuf, size_t errbuf_size) {
	if (!graph) {
		if (errbuf && errbuf_size)
			snprintf(errbuf, errbuf_size, "expected an audio graph");
		return nullptr;
	}

	nx_opus_track_t *t = new nx_opus_track();
	t->channels = channels < 1 ? 1 : (channels > 2 ? 2 : channels);
	t->graph_rate = (uint32_t)(graph->sample_rate > 0 ? graph->sample_rate
	                                                   : (double)kOpusRate);

	t->dec = nx_opus_decoder_create(t->channels, errbuf, errbuf_size);
	if (!t->dec) {
		delete t;
		return nullptr;
	}

	// 0018: the resampler input is the jitter pull's OUTPUT -- stereo at
	// 48 kHz -- regardless of the decoder's channel count (a mono decoder
	// is duplicated to stereo inside the pull), so the swr in-layout is
	// always 2ch.
	t->swr = make_swr(2, kOpusRate, 2, t->graph_rate);
	if (!t->swr && t->graph_rate != kOpusRate) {
		// The fast path does not apply and the resampler could not be
		// built -> the track cannot deliver the graph's format.
		if (errbuf && errbuf_size)
			snprintf(errbuf, errbuf_size,
			         "cannot build a resampler for 2 ch @ %u Hz -> 2 ch @ %u Hz",
			         kOpusRate, t->graph_rate);
		nx_opus_decoder_destroy(t->dec);
		delete t;
		return nullptr;
	}

	// A stream-source node: the render thread drains it a quantum at a time.
	t->node = nx_audio_node_create(graph, NX_AUDIO_NODE_STREAM_SOURCE);
	if (!t->node) {
		if (errbuf && errbuf_size)
			snprintf(errbuf, errbuf_size,
			         "cannot create the stream-source node");
		if (t->swr)
			swr_free(&t->swr);
		nx_opus_decoder_destroy(t->dec);
		delete t;
		return nullptr;
	}

	// Pre-charge target, in graph frames. Clamped to half the ring so it is
	// always reachable even when the sink is not draining.
	if (NX_OPUS_PRIME_TARGET_MS > 0) {
		const uint32_t capacity = nx_audio_stream_capacity(t->node);
		uint32_t target = (uint32_t)(((uint64_t)t->graph_rate *
		                              (uint64_t)NX_OPUS_PRIME_TARGET_MS) /
		                             1000ull);
		if (target > capacity / 2)
			target = capacity / 2;
		t->target_frames = target;
	}
	// A3 (0064): re-charges fill to the SHORT target (<= the full target),
	// so a real break costs one clean 100 ms gap instead of a 300 ms wall.
	t->reprime_target_frames = t->target_frames;
	if (NX_OPUS_REPRIME_TARGET_MS > 0) {
		uint32_t rtarget = (uint32_t)(((uint64_t)t->graph_rate *
		                               (uint64_t)NX_OPUS_REPRIME_TARGET_MS) /
		                              1000ull);
		if (rtarget > t->target_frames)
			rtarget = t->target_frames;
		t->reprime_target_frames = rtarget;
	}
	// Armed from the start: until the ring holds `target_frames`, the sink sees
	// silence and consumes nothing (which is what lets the ring fill at all).
	t->priming = t->target_frames > 0;
	t->cur_target = t->target_frames;

	// 0018: the packet jitter buffer (design 3.1).  One per track; the
	// decode thread pulls from it on a 20 ms cadence and the network side
	// pushes into it from the webrtc backend's onaudiopacket callback.
	t->jit = nx_audio_jitter_create(t->channels);
	if (!t->jit) {
		if (errbuf && errbuf_size)
			snprintf(errbuf, errbuf_size, "cannot create the jitter buffer");
		if (t->node)
			nx_audio_node_release(t->node);
		if (t->swr)
			swr_free(&t->swr);
		nx_opus_decoder_destroy(t->dec);
		delete t;
		return nullptr;
	}

	t->converted.assign((size_t)kMaxFrames * 2 * 2, 0.0f);
	t->tap.assign((size_t)kTapFrames * 2, 0.0f);

	// 0018: the network ingest route.  The first track wins; destroy
	// unregisters (see the registry above).
	{
		std::lock_guard<std::mutex> lock(g_net_sink_mutex);
		if (!g_net_sink)
			g_net_sink = t;
	}

	// B1 (0062): start the decode thread last, after every field it touches
	// is initialized.  It parks on the tick cadence from the start (0018:
	// nothing to wait for -- it just emits nothing until the jitter
	// preheat is satisfied).
	t->thread = std::thread(opus_track_thread_main, t);
	return t;
}

// Recompute the sink gate: the render thread only drains while JS wants output
// AND the ring has been charged to `target_frames`.  Nothing about the samples
// themselves changes -- a gated node outputs silence and consumes nothing.
// B1 (0062): caller must hold t->state_mutex.
static void opus_track_apply_gate(nx_opus_track_t *t) {
	nx_audio_stream_set_playing(t->node,
	                            t->want_playing.load(std::memory_order_relaxed) &&
	                                !t->priming);
}

// Prebuffer state machine.  B1 (0062): runs on the decode thread after every
// packet and on the JS thread from set_playing() -- callers must hold
// t->state_mutex.  No loops, no sleeps: it is a handful of integer compares
// against the ring's own counters.
static void opus_track_service_prebuffer(nx_opus_track_t *t) {
	if (!t || !t->node)
		return;
	std::lock_guard<std::mutex> lock(t->state_mutex);
	if (t->target_frames == 0) {
		// Prebuffer disabled (NX_OPUS_PRIME_TARGET_MS == 0) -- pass through.
		opus_track_apply_gate(t);
		return;
	}
	if (!t->want_playing.load(std::memory_order_relaxed)) {
		// Nothing plays: keep the gate armed and keep the two observations
		// current so a stale count cannot re-arm it later.  A3 (0064): the
		// NEXT charge is a first-class prime -- it fills to the full target.
		t->underrun_seen = nx_audio_stream_underrun_count(t->node);
		t->consumed_seen = nx_audio_stream_consumed(t->node);
		t->priming = true;
		t->prime_pushes = 0;
		t->cur_target = t->target_frames;
		opus_track_apply_gate(t);
		return;
	}

	const uint32_t target = t->cur_target; // A3 (0064): the CURRENT gate target
	const uint32_t pending = nx_audio_stream_pending(t->node);
	const uint64_t underruns = nx_audio_stream_underrun_count(t->node);
	const uint64_t consumed = nx_audio_stream_consumed(t->node);
	t->underrun_seen = underruns;
	t->consumed_seen = consumed;

	// 0066: the "ring ran dry => close the gate and re-charge" branch is
	// GONE.  On device it fired 41 times in 44 minutes -- each firing was a
	// hard cut to a 100 ms wall of silence (priming=true shuts the gate,
	// the sink eats nothing, output is silent until the re-charge target
	// re-accumulates), which is exactly the stutter users heard.  From now
	// on the gate only closes for two first-class cases, both handled
	// elsewhere:  the cold start armed in nx_opus_track_create and a JS
	// pause/resume (the !want_playing branch above re-arms it).  A
	// mid-stream drain is NOT a gate event any more: 0059's
	// repeat-last-frame covers the quantum-level
	// hole (2.67 ms), 0065's concealment fades the stream out once it is
	// truly dark (> 60 ms), and when packets come back the ring refills and
	// playback continues -- Moonlight's "keep playing, never gate" policy.
	//
	// Bounded-recharge bookkeeping (prime_pushes / reprime_count /
	// prime_timeouts) stays, but nothing re-arms the gate mid-stream any
	// more (0067 removed the A1 backlog flush): reprimeCount /
	// primeTimeouts reading 0 in the field is the EXPECTED steady state.

	if (t->priming) {
		t->prime_pushes++;
		if (pending >= target) {
			t->priming = false;   // charged -- let the sink drain it
			t->prime_pushes = 0;
		} else if (t->prime_pushes >= (uint32_t)NX_OPUS_PRIME_MAX_PUSHES) {
			// Bounded wait: a ring that cannot fill must not hold output shut
			// forever (permanent silence is worse than a partial quantum).
			t->priming = false;
			t->prime_pushes = 0;
			t->prime_timeouts++;
		}
	}
	opus_track_apply_gate(t);
}

// 0018: hand one pulled frame to the graph.  `interleaved` is the jitter
// pull's output -- exactly AJ_FRAME_SAMPLES stereo frames (960) at 48 kHz,
// already decoded or concealed.  Convert to the graph's bus format, write
// the ring, then service the prebuffer gate and the diagnostic tap.  This
// replaces the tail of the old opus_track_decode_and_write (the decode now
// happens inside nx_audio_jitter_pull).
static void opus_track_write_stereo(nx_opus_track_t *t, const float *interleaved,
                                    int frames) {
	const float *src = interleaved;
	int src_frames = frames;
	float *dst = t->converted.data();
	int dst_frames = frames;

	if (t->swr) {
		const uint8_t *in_data = reinterpret_cast<const uint8_t *>(src);
		uint8_t *out_data = reinterpret_cast<uint8_t *>(dst);
		const int cap = (int)(t->converted.size() / 2);
		int got = swr_convert(t->swr, &out_data, cap, &in_data, src_frames);
		if (got < 0) {
			got = 0;
		}
		dst_frames = got;
	} else {
		// No resampler: the pull's output already IS the graph's bus
		// format (stereo at the graph rate).
		dst = const_cast<float *>(src);
	}

	// Hand the PCM to the graph.  The ring may be full (the render thread
	// has not drained it yet, or the sink is gated); then the overflow is
	// simply dropped, which is what a live stream wants -- the pull keeps
	// the timeline at the live edge regardless (the jitter depth cap sheds
	// the backlog on its side).
	uint32_t remaining = (uint32_t)dst_frames;
	const float *p = dst;
	while (remaining > 0) {
		const uint32_t wrote = nx_audio_stream_write(t->node, p, remaining);
		p += (size_t)wrote * 2;
		remaining -= wrote;
		if (wrote == 0)
			break; // the ring is full and the render thread is not keeping up
	}

	// Re-evaluate the prebuffer gate now that the ring grew.
	opus_track_service_prebuffer(t);

	// Keep the newest PCM for the diagnostic tap (the JS thread reads it).
	if (dst_frames > 0) {
		const uint32_t n =
		    (uint32_t)dst_frames < kTapFrames ? (uint32_t)dst_frames : kTapFrames;
		const float *tp = dst + ((size_t)dst_frames - n) * 2;
		std::lock_guard<std::mutex> lock(t->state_mutex);
		const uint64_t start = t->tap_write % kTapFrames;
		for (uint32_t i = 0; i < n; i++) {
			const uint64_t idx = (start + i) % kTapFrames;
			t->tap[(size_t)idx * 2] = tp[(size_t)i * 2];
			t->tap[(size_t)idx * 2 + 1] = tp[(size_t)i * 2 + 1];
		}
		t->tap_write += n;
	}
}

// 0018 (A7): the decode thread is a 20 ms TICK, not an event follower.
// Every tick pulls exactly one 960-sample stereo frame from the jitter
// buffer (design 3.1: real decode / PLC / EXPAND / MERGE -- the pull ALWAYS
// produces a full frame) and writes it into the graph ring, so the ring's
// input side is a perfect clock and network jitter is absorbed upstream.
//
// Cadence (design 6): absolute-time (`next_tick += 20 ms`), never drifting.
// A scheduler stall is caught up frame-by-frame with no content skipped
// (the wait returns immediately while behind), up to kMaxCatchUpMs -- past
// that the cadence re-anchors to "now" and the jitter's own depth cap sheds
// the backlog.  Never touches V8.
static void opus_track_thread_main(nx_opus_track_t *t) {
	using clock = std::chrono::steady_clock;
	constexpr auto kTick = std::chrono::milliseconds(AJ_FRAME_MS);
	constexpr auto kMaxCatchUp = std::chrono::milliseconds(100);
	auto next_tick = clock::now();
	for (;;) {
		{
			std::unique_lock<std::mutex> lock(t->q_mutex);
			while (!t->quit.load(std::memory_order_relaxed) &&
			       clock::now() < next_tick) {
				t->q_cv.wait_until(lock, next_tick);
			}
			if (t->quit.load(std::memory_order_relaxed))
				return;
		}

		// Before the preheat is satisfied the pull must NOT run: the ring
		// would charge with silence and playback would start on top of a
		// hole (design 3.4).  The cadence still advances.
		if (nx_audio_jitter_ready(t->jit)) {
			float pcm[AJ_FRAME_SAMPLES * AJ_CHANNELS];
			nx_audio_jitter_pull(t->jit, pcm, t->dec);
			opus_track_write_stereo(t, pcm, AJ_FRAME_SAMPLES);
		}

		next_tick += kTick;
		if (clock::now() - next_tick > kMaxCatchUp)
			next_tick = clock::now(); // huge stall: re-anchor, no skip spiral
	}
}

int nx_opus_track_push(nx_opus_track_t *t, const uint8_t *data, size_t size,
                       uint32_t *out_frames) {
	if (out_frames)
		*out_frames = 0;
	// 0018: the LEGACY entry (no RTP metadata).  The live WebRTC path goes
	// through nx_opus_track_push_net() instead.  A metadata-less packet
	// cannot be placed on the network timeline, so each call advances a
	// PRIVATE synthetic one (one 20 ms frame per call) and feeds the same
	// jitter buffer -- direct users (tests, pages driving an OpusTrack by
	// hand) keep working through the same 20 ms pull pipeline.  Once real
	// network traffic has been seen the two must not interleave, and the
	// synthetic side steps aside.
	//
	// (0062's bounded queue is gone with the event-driven decode loop it
	// served; the jitter buffer is the transport now.)
	if (!t || t->destroyed.load(std::memory_order_relaxed) || !t->jit)
		return -1;
	if (!data || size == 0)
		return -1;
	if (t->net_seen)
		return -1;
	const uint16_t seq = t->synth_seq++;
	const uint32_t ts = t->synth_ts;
	t->synth_ts += AJ_FRAME_TS_STEP;
	// ssrc 0, payload_type 0 (not RED), red_stripped 0 (a bare Opus packet,
	// libpeer's meaning): no redundancy and no poison-gate exposure (the
	// gate only fires on payload_type 63).
	return nx_audio_jitter_push(t->jit, data, size, ts, 0, seq, 0, 0, nullptr,
	                            0, 0);
}

void nx_opus_track_set_playing(nx_opus_track_t *t, bool playing) {
	if (!t || t->destroyed.load(std::memory_order_relaxed) || !t->node)
		return;
	t->want_playing.store(playing, std::memory_order_relaxed);
	// Turning output on must not wait for the next packet: if the ring is
	// already charged, start draining now.  Turning it off takes effect at once
	// and re-arms the gate for the next start.
	opus_track_service_prebuffer(t);
}

nx_audio_node *nx_opus_track_node(nx_opus_track_t *t) {
	return t ? t->node : nullptr;
}

uint32_t nx_opus_track_read(nx_opus_track_t *t, float *out, uint32_t out_len) {
	if (!t || t->destroyed.load(std::memory_order_relaxed) || !out || out_len < 2)
		return 0;
	const uint32_t max_frames = out_len / 2;
	const uint64_t available = t->tap_write < kTapFrames ? t->tap_write : kTapFrames;
	const uint32_t frames =
	    available < max_frames ? (uint32_t)available : max_frames;
	if (frames == 0)
		return 0;
	// The newest frames are at the end of the ring.  B1 (0062): the decode
	// thread writes this ring, so take state_mutex.
	std::lock_guard<std::mutex> lock(t->state_mutex);
	const uint64_t end = t->tap_write % kTapFrames;
	const uint64_t start = (end + kTapFrames - frames) % kTapFrames;
	for (uint32_t i = 0; i < frames; i++) {
		const uint64_t idx = (start + i) % kTapFrames;
		out[(size_t)i * 2] = t->tap[(size_t)idx * 2];
		out[(size_t)i * 2 + 1] = t->tap[(size_t)idx * 2 + 1];
	}
	return frames;
}

void nx_opus_track_destroy(nx_opus_track_t *t) {
	if (!t || t->destroyed.exchange(true, std::memory_order_relaxed))
		return;
	// 0018: this track stops being the network ingest route.
	{
		std::lock_guard<std::mutex> lock(g_net_sink_mutex);
		if (g_net_sink == t)
			g_net_sink = nullptr;
	}
	// B1 (0062): stop the decode thread FIRST, so nothing can touch the
	// decoder or the node while they are torn down.  0066: set `quit` under
	// q_mutex so a consumer sitting between its predicate check and the
	// block cannot miss the wakeup -- after we take the lock, the consumer
	// is either parked in q_cv (and notify_all below reaches it) or about
	// to re-check `quit` and leave on its own.
	{
		std::lock_guard<std::mutex> lock(t->q_mutex);
		t->quit.store(true, std::memory_order_relaxed);
	}
	t->q_cv.notify_all();
	if (t->thread.joinable())
		t->thread.join();
	// The node is released BEFORE the decoder so the render thread cannot
	// see a node whose producer is gone.
	if (t->node) {
		nx_audio_stream_set_playing(t->node, false);
		nx_audio_node_release(t->node);
		t->node = nullptr;
	}
	if (t->swr) {
		swr_free(&t->swr);
		t->swr = nullptr;
	}
	if (t->jit) {
		nx_audio_jitter_destroy(t->jit);
		t->jit = nullptr;
	}
	if (t->dec) {
		nx_opus_decoder_destroy(t->dec);
		t->dec = nullptr;
	}
}

// --- V8 bindings ------------------------------------------------------------

namespace {

nx_opus_track_t *get_track(Isolate *iso, Local<Value> val) {
	nx_opus_track_t *t = nx::Unwrap<nx_opus_track_t>(val);
	if (!t)
		nx_throw(iso, "expected an Opus track handle");
	return t;
}

void free_track(nx_opus_track_t *t) {
	nx_opus_track_destroy(t);
	delete t;
}

// The node belongs to the track, so the JS wrapper must NOT free it.
void keep_node(nx_audio_node *) {}

// opusTrackNew(ctxHandle, channels) -> track handle
void nx_opus_track_new(const FunctionCallbackInfo<Value> &info) {
	Isolate *iso = info.GetIsolate();
	nx_audio_ctx_t *ctx = nx::Unwrap<nx_audio_ctx_t>(info[0]);
	if (!ctx) {
		nx_throw(iso, "expected an AudioContext handle");
		return;
	}
	int channels = 2;
	if (info.Length() > 1 && info[1]->IsNumber()) {
		channels = info[1]->Int32Value(iso->GetCurrentContext()).FromJust();
	}
	char err[256] = {};
	nx_opus_track_t *t =
	    nx_opus_track_create(ctx->graph, channels, err, sizeof(err));
	if (!t) {
		nx_throw(iso, err[0] ? err : "cannot create the Opus track");
		return;
	}
	Local<Object> obj = nx::NewWrapped(iso);
	nx::Wrap<nx_opus_track_t>(iso, obj, t, free_track);
	info.GetReturnValue().Set(obj);
}

// opusTrackPush(track, packet) -> PCM frames produced
void nx_opus_track_push_cb(const FunctionCallbackInfo<Value> &info) {
	Isolate *iso = info.GetIsolate();
	nx_opus_track_t *t = get_track(iso, info[0]);
	if (!t)
		return;
	size_t n = 0;
	uint8_t *p = NX_GetBufferSource(iso, &n, info[1]);
	if (!p) {
		nx_throw(iso, "expected the Opus packet bytes");
		return;
	}
	uint32_t frames = 0;
	if (nx_opus_track_push(t, p, n, &frames) < 0) {
		nx_throw(iso, "cannot decode the Opus packet");
		return;
	}
	info.GetReturnValue().Set(Number::New(iso, (double)frames));
}

// opusTrackSetPlaying(track, playing)
void nx_opus_track_set_playing_cb(const FunctionCallbackInfo<Value> &info) {
	Isolate *iso = info.GetIsolate();
	nx_opus_track_t *t = get_track(iso, info[0]);
	if (!t)
		return;
	const bool playing = info[1]->BooleanValue(iso);
	nx_opus_track_set_playing(t, playing);
}

// opusTrackNode(track) -> AudioNode handle
void nx_opus_track_node_cb(const FunctionCallbackInfo<Value> &info) {
	Isolate *iso = info.GetIsolate();
	nx_opus_track_t *t = get_track(iso, info[0]);
	if (!t)
		return;
	nx_audio_node *node = nx_opus_track_node(t);
	if (!node) {
		info.GetReturnValue().SetNull();
		return;
	}
	Local<Object> obj = nx::NewWrapped(iso);
	nx::Wrap<nx_audio_node>(iso, obj, node, keep_node);
	info.GetReturnValue().Set(obj);
}

// opusTrackRead(track, out) -> the number of stereo frames written into `out`
void nx_opus_track_read_cb(const FunctionCallbackInfo<Value> &info) {
	Isolate *iso = info.GetIsolate();
	nx_opus_track_t *t = get_track(iso, info[0]);
	if (!t)
		return;
	size_t len = 0;
	uint8_t *data = NX_GetBufferSource(iso, &len, info[1]);
	if (!data) {
		nx_throw(iso, "expected a Float32Array");
		return;
	}
	const uint32_t frames =
	    nx_opus_track_read(t, reinterpret_cast<float *>(data),
	                       (uint32_t)(len / sizeof(float)));
	info.GetReturnValue().Set(Integer::NewFromUnsigned(iso, frames));
}

// opusTrackStats(track) -> a snapshot of the track's counters
void nx_opus_track_stats(const FunctionCallbackInfo<Value> &info) {
	Isolate *iso = info.GetIsolate();
	nx_opus_track_t *t = get_track(iso, info[0]);
	if (!t)
		return;
	Local<Context> ctx = iso->GetCurrentContext();
	Local<Object> r = Object::New(iso);
	const nx_opus_decoder_t *d = t->dec;
	r->Set(ctx, nx_str(iso, "packets"),
	       Number::New(iso, (double)nx_opus_decoder_packets(d))).Check();
	r->Set(ctx, nx_str(iso, "frames"),
	       Number::New(iso, (double)nx_opus_decoder_frames(d))).Check();
	r->Set(ctx, nx_str(iso, "rms"), Number::New(iso, nx_opus_decoder_rms(d))).Check();
	r->Set(ctx, nx_str(iso, "peak"),
	       Number::New(iso, nx_opus_decoder_peak(d))).Check();
	r->Set(ctx, nx_str(iso, "errors"),
	       Number::New(iso, (double)nx_opus_decoder_errors(d))).Check();
	r->Set(ctx, nx_str(iso, "sampleRate"),
	       Number::New(iso, (double)nx_opus_decoder_sample_rate(d))).Check();
	r->Set(ctx, nx_str(iso, "channels"),
	       Integer::New(iso, nx_opus_decoder_channels(d))).Check();
	r->Set(ctx, nx_str(iso, "graphRate"),
	       Number::New(iso, (double)t->graph_rate)).Check();
	// Prebuffer / jitter diagnostics.  `underrun` and `pending` are the ring's
	// own counters (audio-graph.cc); `targetFrames` is what this track holds the
	// gate shut for; `reprimeCount` counts gate re-arms -- nothing re-arms it
	// mid-stream any more (0066 dropped the drain branch, 0067 the A1 flush),
	// so a steady 0 is the healthy read.
	// B1 (0062): the prebuffer state lives on the decode thread now, so take
	// state_mutex for the snapshot (the ring counters are atomics).
	uint64_t underrun = 0;
	uint32_t pending = 0;
	bool priming = false;
	bool playing_flag = false;
	uint32_t target = 0;
	uint32_t reprimes = 0;
	uint32_t timeouts = 0;
	if (t->node) {
		underrun = nx_audio_stream_underrun_count(t->node);
		pending = nx_audio_stream_pending(t->node);
	}
	{
		std::lock_guard<std::mutex> lock(t->state_mutex);
		priming = t->priming;
		playing_flag = t->want_playing.load(std::memory_order_relaxed) && !t->priming;
		target = t->target_frames;
		reprimes = t->reprime_count;
		timeouts = t->prime_timeouts;
	}
	r->Set(ctx, nx_str(iso, "underrun"),
	       Number::New(iso, (double)underrun)).Check();
	r->Set(ctx, nx_str(iso, "pending"),
	       Number::New(iso, (double)pending)).Check();
	r->Set(ctx, nx_str(iso, "targetFrames"),
	       Number::New(iso, (double)target)).Check();
	r->Set(ctx, nx_str(iso, "reprimeCount"),
	       Number::New(iso, (double)reprimes)).Check();
	r->Set(ctx, nx_str(iso, "primeTimeouts"),
	       Number::New(iso, (double)timeouts)).Check();
	// 0018: `queueDrops` keeps its legacy meaning ("shed at the door") but
	// is now sourced from the jitter buffer's drop counters (the 0062
	// bounded queue is gone).  The audio_jitter_* keys use the official
	// spelling (design 4) so they can be lined up against the WebRTC
	// getStats() of the official client; the panel that draws them is a
	// later step.  NOTE the pairing caveat from the design: `underrun`
	// counts one per render QUANTUM and is NOT the official
	// concealmentEvents -- reconcile with the gap buckets below.  Healthy
	// session: underrun ~0, late/dup/red_unstripped small, gap buckets grow
	// only when the network actually lost something.
	struct aj_stats js;
	nx_audio_jitter_get_stats(t->jit, &js);
	auto set_num = [&](const char *key, double v) {
		r->Set(ctx, nx_str(iso, key), Number::New(iso, v)).Check();
	};
	set_num("queueDrops", (double)(js.late_dropped + js.dup_dropped +
	                               js.red_unstripped_dropped +
	                               js.oversized_dropped));
	set_num("audio_jitter_target_ms", (double)js.target_level_ms);
	set_num("audio_jitter_buffered_ms", (double)js.buffered_ms);
	set_num("audio_jitter_iat_q96_ms", (double)js.iat_q96_ms);
	set_num("audio_jitter_pushed", (double)js.pushed);
	set_num("audio_jitter_late_dropped", (double)js.late_dropped);
	set_num("audio_jitter_dup_dropped", (double)js.dup_dropped);
	set_num("audio_jitter_red_unstripped_dropped",
	        (double)js.red_unstripped_dropped);
	set_num("audio_jitter_redundant_used", (double)js.redundant_used);
	set_num("audio_jitter_fec_used", (double)js.fec_used);
	set_num("audio_jitter_plc_frames", (double)js.plc_frames);
	set_num("audio_jitter_expand_frames", (double)js.expand_frames);
	set_num("audio_jitter_merge_frames", (double)js.merge_frames);
	set_num("audio_jitter_accel_frames", (double)js.accel_frames);
	// Gap-length histogram (design 4): one bucket per concealment RUN,
	// 1 / 2 / 3-5 / 6+ frames.  This is the number to reconcile against
	// concealmentEvents, not `underrun`.
	set_num("audio_jitter_gap_1", (double)js.gap_hist[0]);
	set_num("audio_jitter_gap_2", (double)js.gap_hist[1]);
	set_num("audio_jitter_gap_3_5", (double)js.gap_hist[2]);
	set_num("audio_jitter_gap_6_plus", (double)js.gap_hist[3]);
	r->Set(ctx, nx_str(iso, "priming"), Boolean::New(iso, priming)).Check();
	r->Set(ctx, nx_str(iso, "playing"),
	       Boolean::New(iso, playing_flag)).Check();
	info.GetReturnValue().Set(r);
}

// opusPacketInfo(bytes) -> the RFC 6716 TOC of one packet
void nx_opus_packet_info_cb(const FunctionCallbackInfo<Value> &info) {
	Isolate *iso = info.GetIsolate();
	size_t n = 0;
	uint8_t *p = NX_GetBufferSource(iso, &n, info[0]);
	if (!p) {
		nx_throw(iso, "expected the Opus packet bytes");
		return;
	}
	nx_opus_packet_info pi;
	if (!nx_opus_packet_parse(p, n, &pi)) {
		info.GetReturnValue().SetNull();
		return;
	}
	Local<Context> ctx = iso->GetCurrentContext();
	Local<Object> r = Object::New(iso);
	r->Set(ctx, nx_str(iso, "config"), Integer::New(iso, pi.config)).Check();
	r->Set(ctx, nx_str(iso, "frameCode"), Integer::New(iso, pi.frame_code)).Check();
	r->Set(ctx, nx_str(iso, "frameCount"), Integer::New(iso, pi.frame_count)).Check();
	r->Set(ctx, nx_str(iso, "frameTenthsMs"),
	       Integer::New(iso, pi.frame_tenths_ms)).Check();
	r->Set(ctx, nx_str(iso, "totalTenthsMs"),
	       Integer::New(iso, pi.total_tenths_ms)).Check();
	r->Set(ctx, nx_str(iso, "samples"),
	       Number::New(iso, (double)pi.samples)).Check();
	r->Set(ctx, nx_str(iso, "exact"), Boolean::New(iso, pi.exact)).Check();
	info.GetReturnValue().Set(r);
}

} // namespace

void nx_init_opus_decoder(Isolate *iso, Local<Object> init_obj) {
	NX_SET_FUNC(init_obj, "opusTrackNew", nx_opus_track_new);
	NX_SET_FUNC(init_obj, "opusTrackPush", nx_opus_track_push_cb);
	NX_SET_FUNC(init_obj, "opusTrackSetPlaying", nx_opus_track_set_playing_cb);
	NX_SET_FUNC(init_obj, "opusTrackNode", nx_opus_track_node_cb);
	NX_SET_FUNC(init_obj, "opusTrackRead", nx_opus_track_read_cb);
	NX_SET_FUNC(init_obj, "opusTrackStats", nx_opus_track_stats);
	NX_SET_FUNC(init_obj, "opusPacketInfo", nx_opus_packet_info_cb);
}

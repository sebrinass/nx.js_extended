// Portable ffmpeg media decode pipeline. See media-decoder.h for the
// architecture and threading contract. No V8 / libnx — compiled into both the
// device runtime and the host nxjs-test binary.
#include "media-decoder.h"
#include "audio-graph.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#if defined(NX_ENABLE_NVTEGRA)
// Hardware decode (Tegra X1 NVDEC).  Only the Switch build defines
// NX_ENABLE_NVTEGRA: the host nxjs-test binary links a stock ffmpeg that
// has neither AV_PIX_FMT_NVTEGRA nor the nvtegra hwcontext.
extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_nvtegra.h>
#include <libavutil/nvtegra.h>
}
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <errno.h> // AVERROR(ETIMEDOUT) etc in nx_media_fail_kind
#include <math.h>
#include <mutex>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <thread>
#include <vector>

namespace {

// Quiet ffmpeg's default stderr chatter (codec warnings during seeks, etc.);
// real errors still surface through the nx_media error paths.
struct av_log_quiet {
	av_log_quiet() { av_log_set_level(AV_LOG_ERROR); }
} av_log_quiet_init;

// Ledger #115 (2026-07-10): bumped from 3 to 12. The 3-slot ring kept
// producer constantly blocked in enqueue_video (post-#113 diag showed
// depth=3 constantly), which starved the shared decode thread of time to
// process audio packets — audio ring drained to 0 every ~1 s, triggering
// Fix B's wall-extrapolation-then-resume-jump-back cycle. 12 slots
// (~500 ms of video buffer at 24 fps) gives the decoder headroom to keep
// audio flowing evenly.
constexpr int RING_SLOTS = 12;
// Cut #22b Stage 2: audio-tap window size for the visualizer readers.
// Must be a power of two (masked with TAP_LEN - 1 in the writer). 1024
// samples ≈ 21.3 ms at 48 kHz — matches the pre-migration (QuickJS-era)
// video.c FFT window so per-bin magnitudes land in the same range as
// spectraplay's `vizFreqGain=30` was originally tuned for.
constexpr uint32_t TAP_LEN = 1024;
constexpr uint32_t TAP_MASK = TAP_LEN - 1;
// Log2(TAP_LEN) — the FFT butterfly loop needs a compile-time bit count.
constexpr int TAP_LOG2 = 10;
// Local pi (avoid depending on <math.h>'s M_PI which is a non-portable
// GNU/BSD extension — devkitPro's newlib does provide it, but keep it
// robust against toolchain changes).
constexpr double TAP_PI = 3.14159265358979323846;
// Present a frame when its PTS is within this much of the clock (one frame of
// slack at 24 fps is ~41 ms; this is just sub-frame jitter tolerance).
constexpr double PRESENT_EPSILON = 0.001;
// ---------------------------------------------------------------------------
// Ledger #112 (2026-07-10) — diagnostic probes for the ~1s periodic video
// stutter reported on webgl_materials_video. Toggled by MEDIA_DIAG_112. Logs
// three signals to stderr (routed to nxjs-debug.log):
//   [md-diag:enq]  per-video-frame sws_scale ms + video-ring depth
//   [md-diag:snap] per audio-clock resync event with delta + interval
//   [md-diag:prs]  per 60 nx_media_present calls: min/max/avg ring depth,
//                  no-frame count (candidate<0), skip count (>1 slot behind)
// All three tag prefixes are unique so `grep '\[md-diag:' log` returns the
// full record. Rate: enq ≈ 24/s, snap on-demand (expected ≤1/s), prs ≈ 1/s.
// Total log volume: ~2 KB/s for a 480x204 24fps stream — trivial.
// ---------------------------------------------------------------------------
#define MEDIA_DIAG_112 0

// ---------------------------------------------------------------------------
// NVDEC wiring diagnostics (patch 0006, 2026-10-21)
// ---------------------------------------------------------------------------
// 1 = on (default) / 0 = off.  Turn it off with `make NX_NVDEC_DIAG=0`
// (the Makefile knob) or by defining NX_NVDEC_DIAG=0.
//
// Every line is tagged "[nvd]" and goes to stderr, which the Switch runtime
// redirects into nxjs-debug.log on the SD card.  Volume is tiny: one line
// per media open, one line when the first hardware frame lands, and one
// summary line per ~2 s of hardware decoding.
//
// The point of these lines is to answer one question on real hardware:
// "did this playback actually go through NVDEC, and at what resolution?"
// ---------------------------------------------------------------------------
#ifndef NX_NVDEC_DIAG
#define NX_NVDEC_DIAG 1
#endif

// ---------------------------------------------------------------------------
// Metadata diagnostics (patch 0018, 2026-10-22)
// ---------------------------------------------------------------------------
// 1 = on (default) / 0 = off.  Define NX_MEDIA_META_DIAG=0 to silence.
//
// One "[md-meta]" line per media open, on stderr (the Switch runtime routes
// stderr into nxjs-debug.log).  It states the numbers the page is supposed
// to see on <video>: the video dimensions, the duration and whether that
// duration is KNOWN (dur_known=0 = a streamed source with no container
// duration -> the page must show NaN, not 0).
//
// The point is to make "the metadata was missing" a one-glance question on
// real hardware: if the line is there, the engine knew the numbers and the
// gap is above the engine; if it is not, the open never happened.
// ---------------------------------------------------------------------------
#ifndef NX_MEDIA_META_DIAG
#define NX_MEDIA_META_DIAG 1
#endif

struct video_slot {
	// The frame buffer.  BGRA (4 B/px) by default; planar I420 (Y|U|V
	// contiguous, 1.5 B/px) when the media was opened with want_yuv.
	// The hardware decoder (NVDEC) writes the SAME layout as the software
	// decoder, so the consumer has one code path for both.
	uint8_t *bgra = nullptr;
	double pts = 0;
};

} // namespace

// ---------------------------------------------------------------------------
// NVDEC chroma layout hypotheses (patch 0021, 2026-10-22)
// ---------------------------------------------------------------------------
// The hwcontext says the surface is NV12 (interleaved chroma); the engine
// has to hand planar I420 to Skia.  Which of the possible layouts the
// hardware actually produced is MEASURED, not assumed -- see
// nvd_eval_chroma() / nvd_decide_chroma().
// ---------------------------------------------------------------------------
#define NVD_FMT_NV12 0  // interleaved: U at even bytes, V at odd bytes
#define NVD_FMT_I420 1  // planar, chroma rows of (width/2) bytes
#define NVD_FMT_I420W 2 // planar, chroma rows of linesize[1] bytes
#define NVD_FMT_COUNT 3

struct nx_media {
	// Caller-owned abort latch (2026-09-17). Polled by libavformat through
	// `interrupt_callback` during EVERY blocking IO operation — the open,
	// the HLS playlist/segment fetches, and av_read_frame during playback.
	// Setting it makes the in-flight call return AVERROR_EXIT promptly
	// instead of running to the 30s rw_timeout.
	//
	// This is what makes a decoder cancellable. Without it, tearing down a
	// decoder whose open is blocked on a dead host froze the V8 main thread
	// for the whole rw_timeout, because close()/the finalizer JOIN the open
	// thread. Owned by the caller (the decoder outlives the media), so it
	// stays valid for the whole media lifetime.
	std::atomic<bool> *abort_flag = nullptr;

	// ---- IO (file or memory) ----
	FILE *file = nullptr;
	int64_t file_size = 0;
	const uint8_t *mem = nullptr;
	size_t mem_size = 0;
	size_t mem_pos = 0;
	// Owns the memory buffer for the media's lifetime (the decode thread
	// streams from it until destroy).
	std::shared_ptr<void> mem_hold;

	// ---- ffmpeg ----
	AVIOContext *avio = nullptr;
	AVFormatContext *fmt = nullptr;
	AVCodecContext *vctx = nullptr;
	AVCodecContext *actx = nullptr;
	int vstream = -1;
	int astream = -1;
	SwsContext *sws = nullptr;
	SwrContext *swr = nullptr;

	// ---- metadata ----
	int width = 0;
	int height = 0;
	double duration = 0;
	// True when `duration` came from the container. A streamed source (raw
	// H.264, no moov) has no container duration -> false -> the JS layer
	// reports NaN, never a made-up number.
	bool duration_known = false;
	double vframe_dur = 1.0 / 30; // nominal frame duration (seek slack)

	// ---- output pixel format (2026-09-06) ----
	// When true, the ring slots hold planar I420 (Y|U|V contiguous, 1.5 B/px)
	// instead of BGRA (4 B/px) — see nx_media_open's want_yuv. yuv_cs is the
	// neutral color-space tag (0=709ltd,1=601ltd,2=full,3=709full) derived
	// from the first decoded frame's colorimetry.
	bool out_yuv = false;
	int yuv_cs = 0;

#if defined(NX_ENABLE_NVTEGRA)
	// ---- hardware decode (NVTEGRA / NVDEC) ----
	// The device context is created once per media and handed to libavcodec
	// before avcodec_open2(); libavcodec then builds the AVHWFramesContext
	// itself and decodes into AV_PIX_FMT_NVTEGRA frames.
	//
	// want_hw is the caller's request (nx_media_open's want_hw_decode);
	// hw_decode is what actually happened. The two differ when the device
	// or the hwaccel was refused and the decoder fell back to software.
	bool want_hw = false;
	bool hw_decode = false;
	AVBufferRef *hw_device_ref = nullptr;
	// Number of frames the hardware decoder actually produced. Written by
	// the decode thread, read by the main thread (diagnostics).
	std::atomic<uint64_t> hw_frames{0};

	// ---- NVDEC chroma placement (2026-10-22, patch 0021) ----
	//
	// The luma at frame->data[0] is always right (device check: the Y
	// fingerprint is a clean gradient and the picture moves).  Where the
	// *chroma* plane starts is NOT knowable from the API, and on the real
	// device (2026-10-22) it turned out to be at NEITHER of the two
	// documented offsets:
	//
	//   * ffmpeg's nvtegra hwcontext models the surface as NV12 whose luma
	//     height is aligned to 32 lines -> data[1] = data[0] + pitch * 736
	//     = 942080 (720p);
	//   * NVDEC itself only knows the picture geometry -> the UV plane
	//     follows the 720 luma rows it wrote -> data[0] + pitch * 720
	//     = 921600 (720p).
	//
	// Device evidence: the byte at +942080 was never written by NVDEC at
	// all (first run of the day: zeros; second run: leftover heap).  So
	// the offset (and the pixel format) are MEASURED from the surface
	// itself -- real chroma is spatially smooth, a wrong offset or a wrong
	// format is noise.  See nvd_decide_chroma().
	//
	// nvd_chroma_off: byte offset of the chroma plane from frame->data[0]
	// (-1 = not measured yet).  nvd_chroma_fmt: the layout hypothesis that
	// won (NVD_FMT_*).  nvd_chroma_mad: its smoothness score (lower is
	// better).  nvd_chroma_age: frames since the last re-measurement.
	int nvd_chroma_off = -1;
	int nvd_chroma_fmt = NVD_FMT_NV12;
	double nvd_chroma_mad = 0.0;
	uint64_t nvd_chroma_age = 0;
#endif

	// ---- video presentation ring (SPSC: decode thread -> main thread) ----
	video_slot slots[RING_SLOTS];
	std::atomic<uint64_t> vwrite{0};
	std::atomic<uint64_t> vread{0};

	// ---- demux-ahead decouple (2026-09-06, decode thread only) ----
	// The decode thread reads packets into these per-stream queues so that a
	// full video ring can no longer BLOCK the thread (in enqueue_video) and
	// thereby stop audio production — the coupling that starved audio and
	// jittered the video clock at a 30 Hz present. Video is enqueued
	// non-blocking (a decoded frame that doesn't fit is held in `v_pending`);
	// audio is decoded only while the audio ring is under a small time target,
	// so it neither blocks nor runs so far ahead that it starves video.
	std::deque<AVPacket *> vpktq; // compressed video packets awaiting decode
	std::deque<AVPacket *> apktq; // compressed audio packets awaiting decode
	AVFrame *v_pending = nullptr; // decoded video frame the full ring rejected
	bool v_pending_valid = false;
	double v_pending_pts = 0;
	bool demux_eof = false; // av_read_frame hit EOF; queues still draining

	// ---- audio output ----
	nx_audio_node *audio_node = nullptr;
	double audio_out_rate = 48000;
	std::vector<float> audio_scratch;
	// Mapping from the stream node's consumed-frame counter to media time:
	// audio_time(consumed) = audio_pts_base + (consumed - audio_base) / rate.
	std::atomic<bool> audio_clock_valid{false};
	std::atomic<double> audio_pts_base{0};
	std::atomic<uint64_t> audio_base{0};

	// ---- clock (main thread only) ----
	double clock_base = 0;
	std::chrono::steady_clock::time_point clock_anchor;
	bool clock_running = false;
	// Ledger #115: monotonic gate on clock_now returns. `at` itself is
	// monotonic (consumed only increases), but Fix B's wall-extrapolation
	// during audio stall can outrun `at`; when audio resumes, returning
	// the fresh `at` causes a visible frame regression (~150 ms every
	// ~1 s under Citron's audio-underrun cycle). `last_clock` gates
	// clock_now to `max(computed, last_clock)` — no regression, ever.
	// Reset to seek target in do_seek so seeks work.
	double last_clock = 0;
	// Cut #22c (2026-07-10): last audio `consumed` sample count observed by
	// clock_now(). When this matches the current value on a fresh call,
	// the audio consumer has NOT advanced since — treat the audio clock as
	// stalled and let wall time free-run rather than snap wall back to a
	// frozen `at`. Prevents the video-ring-full / audio-underrun deadlock
	// that stalls webgl_materials_video after a seek (see #109). Reset to 0
	// whenever `audio_clock_valid` transitions false → true so a fresh seek
	// anchor's first main-thread poll doesn't false-positive.
	uint64_t audio_consumed_last = 0;

	// ---- presentation quality counters (main thread only) ----
	uint64_t presented_frames = 0;
	uint64_t dropped_frames = 0;

	// ---- audio tap (Cut #22b Stage 2 — visualizer surface) ----
	// Rolling mono downmix of the resampled audio. Written by the decode
	// thread inside enqueue_audio; read by the main thread through
	// nx_media_read_waveform / _read_spectrum / _read_audio_levels.
	std::mutex tap_mutex;
	float tap_ring[TAP_LEN] = {};
	uint32_t tap_write_pos = 0;
	uint64_t tap_written = 0;

	// ---- control ----
	std::mutex ctl_mutex;
	std::condition_variable ctl_cv;
	std::thread thread;
	std::atomic<bool> quit{false};
	std::atomic<bool> playing{false};
	std::atomic<bool> looping{false};
	std::atomic<bool> seek_requested{false};
	std::atomic<double> seek_target{0};
	std::atomic<bool> seeking{false};
	// After a seek, present the first decoded frame even if its PTS is
	// slightly past the (paused) clock — the user expects to see the frame
	// nearest the seek target.
	std::atomic<bool> present_force{false};
	std::atomic<bool> eof{false};
	std::atomic<bool> fatal{false};
	char error_buf[256] = {};
	// Loop playback runs in a monotonic PTS domain: each wrap adds the media
	// duration so frame/audio PTS keep increasing past the clock.
	double loop_offset = 0; // decode thread only
};

namespace {

// ---------------------------------------------------------------------------
// Custom AVIO (stdio file or memory buffer)
// ---------------------------------------------------------------------------

int avio_read_cb(void *opaque, uint8_t *buf, int n) {
	nx_media *m = static_cast<nx_media *>(opaque);
	if (m->file) {
		size_t r = fread(buf, 1, (size_t)n, m->file);
		return r > 0 ? (int)r : AVERROR_EOF;
	}
	size_t rem = m->mem_size - m->mem_pos;
	if (rem == 0)
		return AVERROR_EOF;
	size_t r = rem < (size_t)n ? rem : (size_t)n;
	memcpy(buf, m->mem + m->mem_pos, r);
	m->mem_pos += r;
	return (int)r;
}

int64_t avio_seek_cb(void *opaque, int64_t offset, int whence) {
	nx_media *m = static_cast<nx_media *>(opaque);
	if (whence & AVSEEK_SIZE)
		return m->file ? m->file_size : (int64_t)m->mem_size;
	whence &= ~AVSEEK_FORCE;
	if (m->file) {
		if (fseek(m->file, (long)offset, whence) != 0)
			return -1;
		return (int64_t)ftell(m->file);
	}
	int64_t base = whence == SEEK_CUR    ? (int64_t)m->mem_pos
	               : whence == SEEK_END ? (int64_t)m->mem_size
	                                    : 0;
	int64_t pos = base + offset;
	if (pos < 0 || pos > (int64_t)m->mem_size)
		return -1;
	m->mem_pos = (size_t)pos;
	return pos;
}

// ---------------------------------------------------------------------------
// Decode thread
// ---------------------------------------------------------------------------

void set_fatal(nx_media *m, const char *what, int averr) {
	if (m->fatal.load())
		return;
	char detail[128] = {};
	if (averr != 0)
		av_strerror(averr, detail, sizeof(detail));
	snprintf(m->error_buf, sizeof(m->error_buf), "%s%s%s", what,
	         averr ? ": " : "", detail);
	m->fatal.store(true);
}

double frame_pts_seconds(nx_media *m, AVFrame *frame, int stream_index) {
	int64_t ts = frame->best_effort_timestamp;
	if (ts == AV_NOPTS_VALUE)
		ts = frame->pts;
	if (ts == AV_NOPTS_VALUE)
		return 0;
	return ts * av_q2d(m->fmt->streams[stream_index]->time_base);
}

// Sleep briefly, returning false if the thread should abandon its current
// blocking operation (quit or a pending seek).
bool decode_wait(nx_media *m) {
	if (m->quit.load() || m->seek_requested.load())
		return false;
	std::this_thread::sleep_for(std::chrono::milliseconds(5));
	return true;
}

// Derive the neutral color-space tag from a decoded frame's colorimetry.
// Cheap; the value is stable, so re-deriving is harmless.
//   0 = Rec709 limited, 1 = Rec601 limited, 2 = JPEG/full range,
//   3 = Rec709 full.
int derive_yuv_colorspace(AVFrame *frame, int height) {
	if (frame->color_range == AVCOL_RANGE_JPEG)
		return 2; // JPEG/full range
	if (frame->colorspace == AVCOL_SPC_BT709)
		return 0; // Rec709 limited
	if (frame->colorspace == AVCOL_SPC_BT470BG ||
	    frame->colorspace == AVCOL_SPC_SMPTE170M)
		return 1; // Rec601 limited
	return (height >= 720) ? 0 : 1; // unspecified -> guess by size
}

// Non-blocking: convert `frame` into the next ring slot IF one is free.
// Returns 1 = enqueued, 0 = ring full (caller retries / holds the frame),
// -1 = fatal. Demux-ahead (2026-09-06): the decode thread must never block on
// a full video ring — that stalls audio production — so the slow path uses
// this and stashes a rejected frame in `v_pending` instead of waiting.
#if defined(NX_ENABLE_NVTEGRA)
// ---------------------------------------------------------------------------
// NVDEC chroma placement -- measured, not guessed (patch 0021, 2026-10-22)
// ---------------------------------------------------------------------------
// The luma at frame->data[0] is always right.  Where the chroma plane is,
// and whether it is interleaved (NV12) or planar (I420), is decided by
// MEASURING the surface:
//
//   * real chroma is spatially smooth -- neighbouring samples differ by a
//     couple of units;
//   * a wrong offset, a wrong pixel format or memory nobody ever wrote
//     gives high-frequency noise -- tens of units.
//
// So every plausible offset is tried under every plausible layout, the
// smoothness of the result is scored, and the best one wins.  The scores are
// printed, so one device run answers the question on its own.
//
// Why this replaced the probe of patch 0013: on the real device (2026-10-22,
// Switch, 720p) the byte at +942080 was never written by NVDEC at all -- the
// first run of the day read back as zeros, the second as leftover heap, with
// the *same* Y fingerprint.  "Did the disputed band change between two
// frames of the same pool slot?" can only ever answer "aligned" for such a
// band, so the probe always picked the offset that is NOT chroma and turned
// a decodable picture into rainbow garbage.  A measurement cannot be fooled
// by a picture that happens to be static.
//
// ★ PATCH 0023 (2026-10-22): the smoothness score alone is NOT enough --
// it cannot tell "the chroma plane" from "the chroma plane, shifted".
//
// Measured on the real device, byte for byte (720p, surface 1413120 B):
//
//   921600 = 16 rows of luma padding that NVDEC never wrote (all zero)
//            followed by the real chroma, shifted up by 16 rows.
//   942080 = the real chroma (first row e8 ab d4 9f e1 a8 db a6 ...).
//
// A 16-row shift of a smooth picture is STILL smooth, so the padding's
// score (mad=3.19) even BEAT the real chroma's (mad=3.82) -- 0021 always
// picked the padding and the picture stayed a snowstorm.
//
// Hence the extra hard criterion below: a candidate whose FIRST chroma row
// is entirely zero cannot be the start of a written plane (that is exactly
// what padding looks like) and is rejected before the smoothness score is
// even looked at.  With that filter the real chroma is the only survivor.
//
// So the smoothness score keeps two jobs only: "does this region look like
// chroma at all?" (sanity filter) and "which of two neighbouring candidates
// is the better one?" (tie-break).  It is no longer the selector, because
// it provably cannot tell "correct" from "shifted by 15 rows".
//
// The two documented candidates, for the record:
//   off_model = frame->data[1] - frame->data[0] -- ffmpeg's surface model
//                (luma height aligned to 32 rows -> 942080 for 1280x720).
//                On the real device this is where the chroma really is
//                (byte-identical to the PC reference frame).
//   off_tight = linesize[0] * FFALIGN(height, 16) -- the picture geometry
//                (921600).  On the real device this is the 16 rows of
//                padding NVDEC never wrote.
//
// ---------------------------------------------------------------------------

// How many chroma rows to measure.  Spread over the whole picture: the top
// of a video is often a static bar, which would blind a top-only probe.
static const int NVD_PROBE_ROWS = 8;

// A measured candidate.
struct nvd_cand {
	bool ok = false;
	long off = 0; // byte offset from the start of the surface
	int fmt = NVD_FMT_NV12;
	double mad = 0.0; // mean |delta| between neighbouring chroma samples
	double u = 0.0, v = 0.0;
	long umin = 0, umax = 0, vmin = 0, vmax = 0;
	int zero = 0; // 1 = every sampled byte was 0x00 (nobody wrote it)
	int first_zero = 0; // 1 = the FIRST chroma row is all 0x00 (padding)
};

// A chroma plane whose neighbours differ by less than this is believable;
// noise is two to three times that.
static const double NVD_CHROMA_GOOD_MAD = 24.0;

// Frames between two re-measurements (the "rollback" the old probe lacked).
static const uint64_t NVD_CHROMA_RECHECK_FRAMES = 120;

// ★ 0024: 「luma/chroma 相关性」这个选择器已删除 —— 0023 真机实测它把**对的**
// 判成了**错的**（why=correlation）。理由与新判据见 nvd_decide_placement()。

// Score one candidate: how smooth is the chroma there, and does it look like
// chroma at all?  Returns false when the candidate does not fit in the map.
static bool nvd_eval_chroma(const uint8_t *base, size_t map_size, long off,
                             int cpitch, int scw, int sch, int fmt, int nrows,
                             struct nvd_cand *out) {
	const int step = (sch / nrows) > 0 ? (sch / nrows) : 1;
	double usum = 0.0, vsum = 0.0, dsum = 0.0;
	long count = 0, nsamp = 0;
	long umin = 255, umax = 0, vmin = 255, vmax = 0;
	int zero = 1;
	// ★ 0023: does the FIRST chroma row of this candidate consist of zeros?
	// A plane that starts with an all-zero row was never written there --
	// that is what the padding between the luma and the chroma looks like.
	int first_zero = 1;
	for (int k = 0; k < nrows; k++) {
		const int row = k * step;
		if (row >= sch) break;
		size_t need;
		if (fmt == NVD_FMT_NV12)
			need = (size_t)off + (size_t)(row + 1) * (size_t)cpitch;
		else if (fmt == NVD_FMT_I420)
			need = (size_t)off + (size_t)scw * (size_t)sch +
			       (size_t)(row + 1) * (size_t)scw;
		else
			need = (size_t)off + (size_t)cpitch * (size_t)sch +
			       (size_t)(row + 1) * (size_t)cpitch;
		if (need > map_size) continue; // this row lies outside the surface
		const uint8_t *pu;
		const uint8_t *pv;
		int ustep;
		if (fmt == NVD_FMT_NV12) {
			pu = base + off + (size_t)row * (size_t)cpitch;
			pv = pu + 1;
			ustep = 2;
		} else if (fmt == NVD_FMT_I420) {
			pu = base + off + (size_t)row * (size_t)scw;
			pv = pu + (size_t)scw * (size_t)sch;
			ustep = 1;
		} else {
			pu = base + off + (size_t)row * (size_t)cpitch;
			pv = pu + (size_t)cpitch * (size_t)sch;
			ustep = 1;
		}
		int u_prev = 0, v_prev = 0;
		int row_zero = 1;
		for (int c = 0; c < scw; c++) {
			const int u = pu[c * ustep];
			const int v = pv[c * ustep];
			if (u != 0 || v != 0) {
				zero = 0;
				row_zero = 0;
			}
			usum += u; vsum += v; nsamp++;
			if (u < umin) umin = u;
			if (u > umax) umax = u;
			if (v < vmin) vmin = v;
			if (v > vmax) vmax = v;
			if (c > 0) {
				int d = u - u_prev; d = d < 0 ? -d : d;
				dsum += d; count++;
				d = v - v_prev; d = d < 0 ? -d : d;
				dsum += d; count++;
			}
			u_prev = u; v_prev = v;
		}
		if (k == 0) first_zero = row_zero;
	}
	if (!nsamp) return false;
	out->ok = true;
	out->off = off;
	out->fmt = fmt;
	out->mad = count ? dsum / (double)count : 1e9;
	out->u = usum / (double)nsamp;
	out->v = vsum / (double)nsamp;
	out->umin = umin; out->umax = umax;
	out->vmin = vmin; out->vmax = vmax;
	out->zero = zero;
	out->first_zero = first_zero;
	return true;
}

static const char *nvd_fmt_name(int fmt) {
	if (fmt == NVD_FMT_NV12) return "nv12";
	if (fmt == NVD_FMT_I420) return "i420";
	return "i420w";
}

// One chroma sample, bounds checked.  Returns false when it lies outside
// the surface.
static bool nvd_chroma_sample(const uint8_t *base, size_t map_size, long off,
                               int cpitch, int scw, int sch, int fmt, int row,
                               int col, int *u, int *v) {
	const uint8_t *pu;
	const uint8_t *pv;
	int ustep;
	size_t need;
	if (fmt == NVD_FMT_NV12) {
		pu = base + off + (size_t)row * (size_t)cpitch;
		pv = pu + 1;
		ustep = 2;
		need = (size_t)off + (size_t)(row + 1) * (size_t)cpitch;
	} else if (fmt == NVD_FMT_I420) {
		pu = base + off + (size_t)row * (size_t)scw;
		pv = pu + (size_t)scw * (size_t)sch;
		ustep = 1;
		need = (size_t)off + (size_t)scw * (size_t)sch +
		       (size_t)(row + 1) * (size_t)scw;
	} else {
		pu = base + off + (size_t)row * (size_t)cpitch;
		pv = pu + (size_t)cpitch * (size_t)sch;
		ustep = 1;
		need = (size_t)off + (size_t)cpitch * (size_t)sch +
		       (size_t)(row + 1) * (size_t)cpitch;
	}
	if (need > map_size) return false;
	*u = pu[col * ustep];
	*v = pv[col * ustep];
	return true;
}

// Print one candidate's fingerprint: U/V statistics, smoothness and a few
// sample points.  This is what makes a device run conclusive.
static void nvd_log_cand(const uint8_t *base, size_t map_size, long off,
                          int cpitch, int scw, int sch,
                          const struct nvd_cand *c) {
	if (!c->ok) {
		fprintf(stderr, "[nvd] cand off=%ld fmt=%s: (not measurable)\n", off,
		        nvd_fmt_name(c->fmt));
		return;
	}
	int u00 = 0, v00 = 0, umid = 0, vmid = 0, ubot = 0, vbot = 0;
	nvd_chroma_sample(base, map_size, off, cpitch, scw, sch, c->fmt, 0, 0,
	                  &u00, &v00);
	nvd_chroma_sample(base, map_size, off, cpitch, scw, sch, c->fmt, sch / 2,
	                  scw / 2, &umid, &vmid);
	nvd_chroma_sample(base, map_size, off, cpitch, scw, sch, c->fmt, sch - 1,
	                  scw - 1, &ubot, &vbot);
	fprintf(stderr,
	        "[nvd] cand off=%ld fmt=%s: u=%.1f [%.0f..%.0f] v=%.1f [%.0f..%.0f] "
	        "mad=%.2f zero=%d fz=%d | s: u(0,0)=%d v(0,0)=%d u(mid)=%d v(mid)=%d "
	        "u(bot)=%d v(bot)=%d\n",
	        off, nvd_fmt_name(c->fmt), c->u, (double)c->umin, (double)c->umax,
	        c->v, (double)c->vmin, (double)c->vmax, c->mad, c->zero, c->first_zero,
	        u00, v00, umid, vmid, ubot, vbot);
}

// ---------------------------------------------------------------------------
// ★ 0023: the hard criteria and the placement decision
// ---------------------------------------------------------------------------

// Does this candidate survive the hard criteria?
//
//   zero       -- not a single byte was ever written there (unwritten
//                  memory; it is smooth, which is exactly why it fooled
//                  the smoothness score).
//   first_zero -- the FIRST chroma row is all zeros.  A plane cannot start
//                  in the middle of unwritten memory: this is the padding
//                  in front of the chroma plane (16 rows of it on the real
//                  device), and the chroma that follows it only *looks*
//                  like a plane because a 16-row shift of a smooth
//                  picture is still smooth.
//
// Both are content checks, so the model offset is not believed blindly --
// it has to look like written chroma like every other candidate.
static bool nvd_cand_passes(const struct nvd_cand *c) {
	return c->ok && !c->zero && !c->first_zero;
}

// Score one offset under every layout and keep the best (smoothest) one
// that survives the hard criteria.  Returns false when no layout of this
// offset survives.
static bool nvd_eval_offset(const uint8_t *base, size_t map_size, long off,
                            int cpitch, int scw, int sch,
                            struct nvd_cand *out) {
	struct nvd_cand best;
	best.ok = false;
	for (int fmt = 0; fmt < NVD_FMT_COUNT; fmt++) {
		struct nvd_cand c;
		if (!nvd_eval_chroma(base, map_size, off, cpitch, scw, sch, fmt,
		                     NVD_PROBE_ROWS, &c))
			continue;
		if (!nvd_cand_passes(&c)) continue;
		if (!best.ok || c.mad < best.mad) best = c;
	}
	if (!best.ok) return false;
	*out = best;
	return true;
}


// The outcome of the placement decision.
struct nvd_decision {
	struct nvd_cand pick; // the winner (ok == false when nothing passed)
	int mode = 0;         // 1 = tight, 2 = model, 3 = measured by the scan
	int scanned = 0;      // how many offsets the scan looked at
	const char *why = ""; // why this one won
};

// How many offsets the scan may look at.
static const int NVD_MAX_SCAN = 512;

// Decide where the chroma plane is.
//
// The candidates, in order of trust:
//
//   1. ffmpeg's own surface model (off_model).  It is the only candidate
//      with an independent geometric derivation -- the engine read
//      frame->data[1] straight from the very first upstream version, before
//      patch 0013 touched this at all -- and on the real device it is where
//      the chroma really is (byte-identical to the PC reference frame).
//      It still has to pass the hard criteria: the model is *checked*, not
//      believed.
//   2. the first plane the scan finds (padding excluded).
//   3. the smoothest candidate that passes the hard criteria.
//
// The smoothness score only says "does this region look like chroma?" and
// breaks ties between neighbours.  It is never the selector: a shifted view
// of a smooth plane is just as smooth as the plane itself, which is
// precisely how 0021 kept picking the padding.
static void nvd_decide_placement(const uint8_t *base, size_t map_size,
                                 long delta, int pitch, int cpitch, int scw,
                                 int sch, long off_tight, long off_model,
                                 bool scan, struct nvd_decision *d) {
	d->pick.ok = false;
	d->mode = 0;
	d->scanned = 0;
	d->why = "";

	// 1. the model offset, under every layout.
	struct nvd_cand model_cand;
	model_cand.ok = false;
	nvd_eval_offset(base, map_size, off_model, cpitch, scw, sch, &model_cand);

	// 2./3. the scan: the first plausible plane and the smoothest one.
	struct nvd_cand first_good;
	struct nvd_cand best_mad;
	first_good.ok = false;
	best_mad.ok = false;
	if (scan && off_tight >= 0 && (size_t)off_tight < map_size) {
		const long limit = (long)map_size;
		for (long off = off_tight; off + pitch <= limit; off += pitch) {
			d->scanned++;
			struct nvd_cand c;
			if (!nvd_eval_offset(base, map_size, off, cpitch, scw, sch, &c))
				continue;
			if (!best_mad.ok || c.mad < best_mad.mad) best_mad = c;
			if (!first_good.ok && c.mad <= NVD_CHROMA_GOOD_MAD) first_good = c;
			if (d->scanned >= NVD_MAX_SCAN) break;
		}
	}

	// ★★ 0024: 模型优先（model-first）—— 0023 真机失败的教训
	//
	// 0023 在真机上把**对的**判成了**错的**（why=correlation）：
	//   ① 上一轮 0023 把 chroma 写进了 921600（padding 区）⇒ **自我污染**
	//   ② 这一轮 921600 不再全零 ⇒ 「首行全零 = padding」硬判据**失效**
	//   ③ 0023 于是退回「相关性」来选 ⇒ 扫描到的 first_good = 921600
	//      （那里现在已经长得像 chroma）⇒ 相关性更高 ⇒ **把对的判成错的**
	//
	// 为什么现在不这么判 —— **有机制依据，不是盲信**：
	//   ffmpeg 的 nvtegra 路径**把色度偏移按 `fr->data[1] - fr->data[0]`
	//   告诉 NVDEC**（NVC5B0_SET_PICTURE_CHROMA_OFFSET0）
	//   ⇒ **硬件就写在模型偏移上** ⇒ 模型偏移是**机制上**正确的那一个
	//   （旁证：真机 map_size=1413120 == av_image_get_buffer_size(NV12,1280,736,32)，
	//     736 = 720 按 32 行 GOB 对齐 ⇒ 硬件写的就是 1280×736 起的那个位置）
	//
	// ⇒ **默认用模型**；内容判据只在**能证明「连模型都不对」**时才介入（极端兜底）。
	// ⇒ **不再用「相关性」当选择器** —— 0023 真机就是它把对的判成错的。
	//   （「相关性」函数已一并删除：它在本机实测中把对的判成错的。）
	//
	// 为了能判「模型也不对」，这里**不过硬判据**地再算一次模型偏移：
	// nvd_eval_offset() 只保留**通过硬判据**的排布 ⇒ 模型若首行全零，
	// 它的 first_zero 信息就丢了 ⇒ 必须单独算。
	struct nvd_cand model_best;  // 模型偏移处「有数据」的最好排布
	struct nvd_cand model_any;   // 模型偏移处任意排布（兜底）
	model_best.ok = false;
	model_any.ok = false;
	for (int fmt = 0; fmt < NVD_FMT_COUNT; fmt++) {
		struct nvd_cand c;
		if (!nvd_eval_chroma(base, map_size, off_model, cpitch, scw, sch,
		                     fmt, NVD_PROBE_ROWS, &c))
			continue;
		if (!model_any.ok || c.mad < model_any.mad) model_any = c;
		// 全零的行 mad = 0（"完美平滑"）⇒ 「有数据」必须单独判
		if (!c.zero && !c.first_zero &&
		    (!model_best.ok || c.mad < model_best.mad))
			model_best = c;
	}

	if (model_best.ok) {
		// ★ 模型偏移处确实有数据 ⇒ **用模型**（内容判据不参与选择）
		d->pick = model_cand.ok ? model_cand : model_best;
		d->mode = 2;
		d->why = "model";
	} else if (first_good.ok) {
		// ★ 极端兜底：**连模型都不对**（模型偏移处没人写过）才走到这里
		d->pick = first_good;
		d->mode = 3;
		d->why = model_any.ok ? "scan(model-provably-wrong)" : "scan";
	} else if (nvd_cand_passes(&best_mad) &&
	           best_mad.mad <= NVD_CHROMA_GOOD_MAD) {
		d->pick = best_mad;
		d->mode = 3;
		d->why = "smoothest";
	} else if (model_cand.ok) {
		d->pick = model_cand;
		d->mode = 2;
		d->why = "model(no-threshold)";
	} else if (model_any.ok) {
		d->pick = model_any;
		d->mode = 2;
		d->why = "model(last-resort)";
	}
}

// Measure the surface and pick the chroma plane.  Everything measured is
// printed, so one device run answers "which offset, which format".
static void nvd_decide_chroma(nx_media *m, AVFrame *frame, AVNVTegraMap *map) {
	const uint8_t *base = (const uint8_t *)av_nvtegra_map_get_addr(map);
	const size_t map_size = av_nvtegra_map_get_size(map);
	const int pitch = frame->linesize[0] > 0 ? frame->linesize[0] : frame->width;
	const int cpitch = frame->linesize[1] > 0 ? frame->linesize[1] : frame->width;
	const int scw = (frame->width + 1) / 2;
	const int sch = (frame->height + 1) / 2;
	const int h16 = (frame->height + 15) & ~15;
	// Where the luma starts inside the surface (normally 0).
	const long delta = (long)((const uint8_t *)frame->data[0] - base);
	// The two documented candidates, in surface coordinates.
	const long off_tight = delta + (long)pitch * h16;
	const long off_model = delta + (long)(frame->data[1] - frame->data[0]);

	fprintf(stderr,
	        "[nvd] surface: map_size=%zu luma_at=%ld pitch=%d cpitch=%d "
	        "rows=%d off_tight=%ld off_model=%ld\n",
	        map_size, delta, pitch, cpitch, frame->height, off_tight,
	        off_model);
	fflush(stderr);

	// The two documented candidates, measured under every layout.
	for (int k = 0; k < 2; k++) {
		const long off = k ? off_model : off_tight;
		for (int fmt = 0; fmt < NVD_FMT_COUNT; fmt++) {
			struct nvd_cand c;
			if (!nvd_eval_chroma(base, map_size, off, cpitch, scw, sch, fmt,
			                     NVD_PROBE_ROWS, &c)) {
				fprintf(stderr,
				        "[nvd] cand off=%ld fmt=%s: outside surface\n", off,
				        nvd_fmt_name(fmt));
				continue;
			}
			nvd_log_cand(base, map_size, off, cpitch, scw, sch, &c);
		}
		fprintf(stderr, "[nvd] cand off=%ld raw[0..15]:", off);
		for (int i = 0; i < 16 && (size_t)(off + i) < map_size; i++)
			fprintf(stderr, " %02x", base[off + i]);
		fprintf(stderr, "\n");
	}
	fflush(stderr);

	// Scan the rest of the surface row by row.  The FIRST offset that looks
	// like chroma is the start of the chroma plane: everything between the
	// luma and the chroma is memory the decoder never wrote.
	struct nvd_decision d;
	nvd_decide_placement(base, map_size, delta, pitch, cpitch, scw, sch,
	                     off_tight, off_model, true, &d);

	// Pick it.  Nothing convincing -> ffmpeg's model, which is what the
	// engine read before patch 0013 touched any of this.
	int off_use = (int)(off_model - delta);
	int fmt_use = NVD_FMT_NV12;
	double mad_use = 1e9;
	if (d.pick.ok) {
		off_use = (int)(d.pick.off - delta);
		fmt_use = d.pick.fmt;
		mad_use = d.pick.mad;
	}
	m->nvd_chroma_off = off_use;
	m->nvd_chroma_fmt = fmt_use;
	m->nvd_chroma_mad = mad_use;
	// mode: 1 = the documented tight offset, 2 = ffmpeg's model, 3 = measured
	int mode = 3;
	if (off_use == (int)(off_tight - delta))
		mode = 1;
	else if (off_use == (int)(off_model - delta))
		mode = 2;
	fprintf(stderr,
	        "[nvd] chroma placement: model=%ld tight=%ld -> using %d (mode=%d) "
	        "fmt=%s mad=%.2f scan=%d why=%s%s\n",
	        off_model - delta, off_tight - delta, off_use, mode,
	        nvd_fmt_name(fmt_use), mad_use, d.scanned, d.why,
	        d.pick.ok ? "" : " (nothing passed the hard criteria -- using ffmpeg's model)");
	fflush(stderr);
}

// Re-measure now and then: if the offset/format in use stopped looking like
// chroma while another candidate looks better, switch to it and say so.
// This is the rollback the old probe never had.
static void nvd_recheck_chroma(nx_media *m, AVFrame *frame, AVNVTegraMap *map) {
	const uint8_t *base = (const uint8_t *)av_nvtegra_map_get_addr(map);
	const size_t map_size = av_nvtegra_map_get_size(map);
	const int pitch = frame->linesize[0] > 0 ? frame->linesize[0] : frame->width;
	const int cpitch = frame->linesize[1] > 0 ? frame->linesize[1] : frame->width;
	const int scw = (frame->width + 1) / 2;
	const int sch = (frame->height + 1) / 2;
	const int h16 = (frame->height + 15) & ~15;
	const long delta = (long)((const uint8_t *)frame->data[0] - base);
	const long off_tight = delta + (long)pitch * h16;
	const long off_model = delta + (long)(frame->data[1] - frame->data[0]);

	struct nvd_cand cur;
	if (!nvd_eval_chroma(base, map_size, delta + m->nvd_chroma_off, cpitch, scw,
	                     sch, m->nvd_chroma_fmt, NVD_PROBE_ROWS, &cur)) {
		// The placement in use cannot even be read -> decide again.
		nvd_decide_chroma(m, frame, map);
		return;
	}
	if (!nvd_cand_passes(&cur)) {
		// The placement in use turned out to be padding (or memory nobody
		// ever wrote) -> decide again, this time over the whole surface.
		fprintf(stderr,
		        "[nvd] chroma re-check: current off=%d fmt=%s is not a written "
		        "plane (zero=%d fz=%d) -> re-deciding\n",
		        m->nvd_chroma_off, nvd_fmt_name(m->nvd_chroma_fmt), cur.zero,
		        cur.first_zero);
		fflush(stderr);
		nvd_decide_chroma(m, frame, map);
		return;
	}

	// The placement in use still looks like chroma.  Only switch when
	// something else is clearly better -- and only to a candidate that
	// passes the same hard criteria, so this can never walk back into the
	// padding 0021 used to pick.
	struct nvd_decision d;
	nvd_decide_placement(base, map_size, delta, pitch, cpitch, scw, sch,
	                     off_tight, off_model, false, &d);
	if (!d.pick.ok) return;                          // nothing better known
	if (d.pick.off == delta + m->nvd_chroma_off) return; // the same place
	if (d.pick.mad >= cur.mad * 0.5) return;         // the current one is fine
	fprintf(stderr,
	        "[nvd] chroma re-check: current off=%d fmt=%s mad=%.2f -> off=%ld "
	        "fmt=%s mad=%.2f (switched)\n",
	        m->nvd_chroma_off, nvd_fmt_name(m->nvd_chroma_fmt), cur.mad,
	        d.pick.off - delta, nvd_fmt_name(d.pick.fmt), d.pick.mad);
	m->nvd_chroma_off = (int)(d.pick.off - delta);
	m->nvd_chroma_fmt = d.pick.fmt;
	m->nvd_chroma_mad = d.pick.mad;
	fflush(stderr);
}
#endif

int try_enqueue_video(nx_media *m, AVFrame *frame, double pts) {
	if (m->vwrite.load(std::memory_order_relaxed) -
	        m->vread.load(std::memory_order_acquire) >=
	    RING_SLOTS)
		return 0; // ring full
	uint64_t w = m->vwrite.load(std::memory_order_relaxed);
	video_slot *slot = &m->slots[w % RING_SLOTS];

#if defined(NX_ENABLE_NVTEGRA)
	// Hardware frames come out as AV_PIX_FMT_NVTEGRA.  If libavcodec fell
	// back to the software decoder (hwaccel init failed) the frame is a
	// normal software frame and takes the BGRA / I420 path below.
	if (frame->format == AV_PIX_FMT_NVTEGRA) {
		// The decoder recycles its frame buffers, so the pixels have to be
		// copied before the slot is published.  The NVDEC surface on Switch
		// is created by libnx with NvKind_Pitch, i.e. it is *pitch-linear*:
		// frame->data[0] is the luma plane and frame->data[1] the
		// interleaved chroma plane, with no GOB swizzle to undo.  The
		// surface is CPU-cacheable, so the CPU cache has to be invalidated
		// before reading what the GPU wrote.
		AVNVTegraMap *map = av_nvtegra_frame_get_fbuf_map(frame);
		if (!map) {
			set_fatal(m, "hardware frame has no nvtegra map", 0);
			return -1;
		}
		// The surface must be 8-bit NV12: a luma plane plus an interleaved
		// 8-bit chroma plane.  10-bit streams would come out as P010
		// (16-bit samples) and monochrome as GRAY8 (no chroma plane at
		// all) -- neither can be de-interleaved by the code below, so
		// refuse them loudly instead of decoding garbage.  (The open
		// already keeps 10-bit streams on the software decoder when the
		// bit depth is known; this is the belt-and-braces check for the
		// streams where it is not.)
		AVHWFramesContext *hwfc = frame->hw_frames_ctx
		                             ? (AVHWFramesContext *)frame->hw_frames_ctx->data
		                             : NULL;
		if (!hwfc || hwfc->sw_format != AV_PIX_FMT_NV12) {
			set_fatal(m, "hardware decoder produced an unsupported "
			             "surface format (expected 8-bit NV12)", 0);
			return -1;
		}
		av_nvtegra_map_cache_op(map, NVMAP_CACHE_OP_INV,
		                        av_nvtegra_map_get_addr(map),
		                        av_nvtegra_map_get_size(map));

		int hw_w = frame->width < m->width ? frame->width : m->width;
		int hw_h = frame->height < m->height ? frame->height : m->height;
		hw_w &= ~1; // NV12 needs even dimensions
		hw_h &= ~1;
		if (hw_w <= 0 || hw_h <= 0) {
			set_fatal(m, "hardware frame has no usable area", 0);
			return -1;
		}

		// The ring slot holds the SAME layout the software path produces:
		// planar I420 (Y|U|V contiguous) when the media was opened with
		// want_yuv, BGRA otherwise.  Keeping one layout for both decoders
		// means the consumer (the <video> element, the WebCodecs decoder)
		// has a single code path, and the host never has to know whether
		// the frame came from NVDEC or from libavcodec.
		//
		// The NVDEC surface is NV12 (interleaved chroma), so the YUV path
		// de-interleaves U and V here -- on the decode thread, off the main
		// thread -- instead of letting the host do it every frame.
		const size_t slot_bytes =
		    m->out_yuv ? nx_media_i420_size(m->width, m->height)
		               : (size_t)m->width * m->height * 4;
		if (!slot->bgra) {
			slot->bgra = (uint8_t *)malloc(slot_bytes);
			if (!slot->bgra) {
				set_fatal(m, "out of memory", 0);
				return -1;
			}
		}

		if (m->out_yuv) {
			// Planar I420 packed contiguously: Y (W*H) then U then V
			// (each (W/2)*(H/2)).  Same layout as the software path
			// below and the same layout Skia's SkYUVAPixmaps kY_U_V/k420
			// tight packing expects, so the GPU upload reads it directly.
			const int cw = (m->width + 1) / 2, ch = (m->height + 1) / 2;
			uint8_t *y = slot->bgra;
			uint8_t *u = y + (size_t)m->width * m->height;
			uint8_t *v = u + (size_t)cw * ch;
			for (int row = 0; row < hw_h; row++) {
				uint8_t *dst = y + (size_t)row * m->width;
				memcpy(dst, frame->data[0] + (size_t)row * frame->linesize[0],
				       (size_t)hw_w);
				if (hw_w < m->width)
					memset(dst + hw_w, 0, (size_t)(m->width - hw_w));
			}
			// NV12 chroma is interleaved U,V — split it into the two
			// planes Skia wants.  The chroma plane is half resolution:
			// hw_w/2 samples per row, hw_h/2 rows.
			//
			// WHERE the chroma plane starts, and whether it is interleaved
			// (NV12) or planar (I420), is MEASURED from the surface -- see
			// nvd_decide_chroma().  On the real device (2026-10-22) the byte
			// at ffmpeg's own offset (data[1] - data[0]) turned out to be
			// memory NVDEC never wrote, and the byte at the documented
			// "tight" offset was not chroma either, so neither side of that
			// argument can be taken on faith.
			if (m->nvd_chroma_off < 0) {
				nvd_decide_chroma(m, frame, map);
			} else if (++m->nvd_chroma_age >= NVD_CHROMA_RECHECK_FRAMES) {
				m->nvd_chroma_age = 0;
				nvd_recheck_chroma(m, frame, map);
			}
			if (m->nvd_chroma_off < 0) {
				// measurement failed -- fall back to the documented NVDEC
				// placement (the picture geometry).
				m->nvd_chroma_off = frame->linesize[0] *
				                    ((frame->height + 15) & ~15);
				m->nvd_chroma_fmt = NVD_FMT_NV12;
			}
			const int off_chroma = m->nvd_chroma_off;
			const int fmt_chroma = m->nvd_chroma_fmt;
			const int src_cw = (frame->width + 1) / 2;
			const int src_ch = (frame->height + 1) / 2;
			const int hw_cw = hw_w / 2;
			int n = hw_cw < src_cw ? hw_cw : src_cw;
			// Never read past the end of the surface: a wrong offset would
			// otherwise pull in whatever lives behind it.
			const size_t map_size = av_nvtegra_map_get_size(map);
			const uint8_t *map_base =
			    (const uint8_t *)av_nvtegra_map_get_addr(map);
			// Where the luma starts inside the surface (normally 0), and
			// therefore where the chroma plane starts inside it.
			const long luma_at =
			    (long)((const uint8_t *)frame->data[0] - map_base);
			const long abs_off = luma_at + off_chroma;
			size_t avail = 0;
			if (abs_off >= 0 && (size_t)abs_off < map_size)
				avail = map_size - (size_t)abs_off;
			int rows_ok = hw_h / 2;
			if (fmt_chroma == NVD_FMT_NV12) {
				// ★ 0023: NV12 keeps U and V *interleaved inside one
				// row*, so one chroma row costs ONE row of the surface
				// (cpitch bytes) -- not two of them.  0021 assumed twice
				// that and therefore copied only half of the chroma
				// rows: with the right offset the bottom half of the
				// picture would still have stayed neutral grey.
				size_t stride = frame->linesize[1] > 0
				                    ? (size_t)frame->linesize[1]
				                    : (size_t)(2 * n);
				if (stride < (size_t)(2 * n)) n = (int)(stride / 2);
				const size_t row_bytes = (size_t)(2 * n);
				rows_ok = (avail >= row_bytes && stride > 0)
				              ? (int)((avail - row_bytes) / stride) + 1
				              : 0;
			} else if (fmt_chroma == NVD_FMT_I420) {
				const size_t need = (size_t)src_cw * src_ch;
				rows_ok = avail > need
				              ? (int)((avail - need) / (size_t)src_cw)
				              : 0;
			} else {
				const size_t need = (size_t)frame->linesize[1] * src_ch;
				const size_t stride = frame->linesize[1] > 0
				                          ? (size_t)frame->linesize[1]
				                          : (size_t)src_cw;
				rows_ok = avail > need ? (int)((avail - need) / stride) : 0;
			}
			if (rows_ok > hw_h / 2) rows_ok = hw_h / 2;
			if (rows_ok < 0) rows_ok = 0;
			if (rows_ok < hw_h / 2) {
				// The chroma we cannot reach stays neutral (128) instead of
				// stale garbage from the previous frame.
				memset(u, 128, (size_t)cw * ch);
				memset(v, 128, (size_t)cw * ch);
			}
			for (int row = 0; row < rows_ok; row++) {
				uint8_t *du = u + (size_t)row * cw;
				uint8_t *dv = v + (size_t)row * cw;
				const uint8_t *pu;
				const uint8_t *pv;
				int ustep;
				if (fmt_chroma == NVD_FMT_NV12) {
					pu = frame->data[0] + off_chroma +
					     (size_t)row * frame->linesize[1];
					pv = pu + 1;
					ustep = 2;
				} else if (fmt_chroma == NVD_FMT_I420) {
					pu = frame->data[0] + off_chroma +
					     (size_t)row * src_cw;
					pv = pu + (size_t)src_cw * src_ch;
					ustep = 1;
				} else {
					pu = frame->data[0] + off_chroma +
					     (size_t)row * frame->linesize[1];
					pv = pu + (size_t)frame->linesize[1] * src_ch;
					ustep = 1;
				}
				for (int i = 0; i < n; i++) {
					du[i] = pu[i * ustep]; // U
					dv[i] = pv[i * ustep]; // V
				}
				if (n < cw) {
					memset(du + n, 128, (size_t)(cw - n));
					memset(dv + n, 128, (size_t)(cw - n));
				}
			}
		} else {
			// BGRA consumer: let swscale do the colour conversion, but
			// feed it the NV12 surface directly (no intermediate copy).
			m->sws = sws_getCachedContext(m->sws, frame->width, frame->height,
			                              AV_PIX_FMT_NV12, m->width, m->height,
			                              AV_PIX_FMT_BGRA, SWS_BILINEAR, NULL, NULL,
			                              NULL);
			if (!m->sws) {
				set_fatal(m, "failed to create scaler", 0);
				return -1;
			}
			uint8_t *dst[4];
			int dst_stride[4];
			dst[0] = slot->bgra; dst[1] = NULL; dst[2] = NULL; dst[3] = NULL;
			dst_stride[0] = m->width * 4;
			dst_stride[1] = dst_stride[2] = dst_stride[3] = 0;
			sws_scale(m->sws, frame->data, frame->linesize, 0, frame->height,
			          dst, dst_stride);
		}

		m->yuv_cs = derive_yuv_colorspace(frame, m->height);
		slot->pts = pts;
		const uint64_t hw_n =
		    m->hw_frames.fetch_add(1, std::memory_order_relaxed) + 1;
#if NX_NVDEC_DIAG
		if (hw_n == 1) {
			fprintf(stderr,
			        "[nvd] first hardware frame: %dx%d (ring %dx%d) "
			        "linesize=%d/%d chroma_off=%d (model=%d) out_yuv=%d cs=%d\n",
			        frame->width, frame->height, m->width, m->height,
			        frame->linesize[0], frame->linesize[1],
			        m->nvd_chroma_off,
			        (int)(frame->data[1] - frame->data[0]), m->out_yuv ? 1 : 0,
			        m->yuv_cs);
			fflush(stderr);
#if NX_NVDEC_DIAG
			// Plane fingerprint: lets a device run answer "is the decoded
			// pixel data sane, or is the draw broken?" without a debugger.
			// The Y/U/V plane statistics of a real frame are obvious:
			// luma clustered in 16..235, chroma clustered around 128.
			if (m->out_yuv) {
				const int cw = (m->width + 1) / 2, ch = (m->height + 1) / 2;
				const uint8_t *yp = slot->bgra;
				const uint8_t *up = yp + (size_t)m->width * m->height;
				const uint8_t *vp = up + (size_t)cw * ch;
				double ys = 0, us = 0, vs = 0;
				long ymin = 255, ymax = 0, umin = 255, umax = 0, vmin = 255,
				     vmax = 0;
				for (size_t i = 0; i < (size_t)m->width * m->height; i++) {
					const int val = yp[i];
					ys += val;
					if (val < ymin) ymin = val;
					if (val > ymax) ymax = val;
				}
				for (size_t i = 0; i < (size_t)cw * ch; i++) {
					const int uval = up[i], vval = vp[i];
					us += uval;
					vs += vval;
					if (uval < umin) umin = uval;
					if (uval > umax) umax = uval;
					if (vval < vmin) vmin = vval;
					if (vval > vmax) vmax = vval;
				}
				fprintf(stderr,
				        "[nvd] fp Y: mean=%.1f min=%ld max=%ld | "
				        "U: mean=%.1f min=%ld max=%ld | "
				        "V: mean=%.1f min=%ld max=%ld\n",
				        ys / (double)((size_t)m->width * m->height), ymin,
				        ymax, us / (double)((size_t)cw * ch), umin, umax,
				        vs / (double)((size_t)cw * ch), vmin, vmax);
				fprintf(stderr, "[nvd] fp Y[0..15]:");
				for (int i = 0; i < 16 && i < m->width; i++)
					fprintf(stderr, " %02x", yp[i]);
				fprintf(stderr, "\n[nvd] fp U[0..15]:");
				for (int i = 0; i < 16 && i < cw; i++)
					fprintf(stderr, " %02x", up[i]);
				fprintf(stderr, "\n[nvd] fp V[0..15]:");
				for (int i = 0; i < 16 && i < cw; i++)
					fprintf(stderr, " %02x", vp[i]);
				fprintf(stderr,
				        "\n[nvd] fp Y(360,640)=%u Y(719,1279)=%u "
				        "U(179,319)=%u V(179,319)=%u\n",
				        yp[(size_t)360 * m->width + 640],
				        yp[(size_t)(m->height - 1) * m->width + (m->width - 1)],
				        up[(size_t)(ch - 1) * cw + (cw - 1)],
				        vp[(size_t)(ch - 1) * cw + (cw - 1)]);
				fflush(stderr);
			}
#endif
		} else if (hw_n % 120 == 0) {
			fprintf(stderr,
			        "[nvd] hw frames=%llu %dx%d out_yuv=%d\n",
			        (unsigned long long)hw_n, m->width, m->height,
			        m->out_yuv ? 1 : 0);
			fflush(stderr);
		}
#endif
		m->vwrite.store(w + 1, std::memory_order_release);
		return 1;
	}
#endif

	// The software path needs the slot's BGRA / I420 buffer.  It is normally
	// allocated up front in nx_media_open(); on the hardware path it is
	// allocated lazily, because the decoder may have fallen back to software
	// after all.
	if (!slot->bgra) {
		const size_t slot_bytes =
		    m->out_yuv ? nx_media_i420_size(m->width, m->height)
		               : (size_t)m->width * m->height * 4;
		slot->bgra = (uint8_t *)malloc(slot_bytes);
		if (!slot->bgra) {
			set_fatal(m, "out of memory", 0);
			return -1;
		}
	}

	const AVPixelFormat out_fmt =
	    m->out_yuv ? AV_PIX_FMT_YUV420P : AV_PIX_FMT_BGRA;
	m->sws = sws_getCachedContext(m->sws, frame->width, frame->height,
	                              (AVPixelFormat)frame->format, m->width,
	                              m->height, out_fmt, SWS_BILINEAR,
	                              NULL, NULL, NULL);
	if (!m->sws) {
		set_fatal(m, "failed to create scaler", 0);
		return -1;
	}
	uint8_t *dst[4];
	int dst_stride[4];
	if (m->out_yuv) {
		// Planar I420 packed contiguously into the slot: Y (W*H) then U then
		// V (each (W/2)*(H/2)). Matches Skia's SkYUVAPixmaps kY_U_V/k420 tight
		// layout so the GPU upload reads it directly.
		const int cw = (m->width + 1) / 2, ch = (m->height + 1) / 2;
		uint8_t *y = slot->bgra;
		uint8_t *u = y + (size_t)m->width * m->height;
		uint8_t *v = u + (size_t)cw * ch;
		dst[0] = y; dst[1] = u; dst[2] = v; dst[3] = NULL;
		dst_stride[0] = m->width; dst_stride[1] = cw; dst_stride[2] = cw;
		dst_stride[3] = 0;
		// Derive the neutral color-space tag once from the decoded frame's
		// colorimetry (cheap; the value is stable so re-deriving is harmless).
		m->yuv_cs = derive_yuv_colorspace(frame, m->height);
	} else {
		dst[0] = slot->bgra; dst[1] = NULL; dst[2] = NULL; dst[3] = NULL;
		dst_stride[0] = m->width * 4; dst_stride[1] = 0; dst_stride[2] = 0;
		dst_stride[3] = 0;
	}
#if MEDIA_DIAG_112
	auto _sws_t0 = std::chrono::steady_clock::now();
#endif
	sws_scale(m->sws, frame->data, frame->linesize, 0, frame->height, dst,
	          dst_stride);
#if MEDIA_DIAG_112
	double _sws_ms = std::chrono::duration<double, std::milli>(
	                     std::chrono::steady_clock::now() - _sws_t0)
	                     .count();
	static uint64_t _diag_enq_idx = 0;
	uint64_t _diag_idx = _diag_enq_idx++;
	uint64_t _diag_depth = w + 1 - m->vread.load(std::memory_order_relaxed);
	// One line per decoded video frame: sws_ms is the H.264 → BGRA scale
	// cost (I-frames spike vs P-frames), depth is the ring occupancy after
	// this push. Correlate periodic depth==RING_SLOTS just-before-a-drop
	// with big sws_ms values to confirm the GOP-burst hypothesis.
	fprintf(stderr, "[md-diag:enq] fi=%llu pts=%.3f sws_ms=%.2f depth=%llu\n",
	        (unsigned long long)_diag_idx, pts, _sws_ms,
	        (unsigned long long)_diag_depth);
#endif
	slot->pts = pts;
	m->vwrite.store(w + 1, std::memory_order_release);
	return 1;
}

// Blocking wrapper — waits for a ring slot, then enqueues. Used only by the
// EOF drain tail (drain_decoders), where the handful of remaining frames can
// safely block on present; the steady-state loop uses try_enqueue_video.
// Returns false if interrupted (quit/seek) or on fatal.
bool enqueue_video(nx_media *m, AVFrame *frame, double pts) {
	for (;;) {
		int r = try_enqueue_video(m, frame, pts);
		if (r == 1)
			return true;
		if (r < 0)
			return false;
		if (!decode_wait(m))
			return false; // ring full: wait for the presenter to drain
	}
}

// Resample an audio frame to interleaved stereo f32 at the output rate and
// push it into the stream node (blocking on backpressure). Returns false if
// interrupted.
bool enqueue_audio(nx_media *m, AVFrame *frame, double pts) {
	if (!m->swr) {
		AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
		int ret = swr_alloc_set_opts2(
		    &m->swr, &out_layout, AV_SAMPLE_FMT_FLT,
		    (int)m->audio_out_rate, &frame->ch_layout,
		    (AVSampleFormat)frame->format, frame->sample_rate, 0, NULL);
		if (ret < 0 || swr_init(m->swr) < 0) {
			set_fatal(m, "failed to create resampler", ret);
			return false;
		}
	}
	int64_t max_out = av_rescale_rnd(
	    swr_get_delay(m->swr, frame->sample_rate) + frame->nb_samples,
	    (int64_t)m->audio_out_rate, frame->sample_rate, AV_ROUND_UP);
	m->audio_scratch.resize((size_t)max_out * 2);
	uint8_t *out_ptr = (uint8_t *)m->audio_scratch.data();
	int got = swr_convert(m->swr, &out_ptr, (int)max_out,
	                      (const uint8_t **)frame->extended_data,
	                      frame->nb_samples);
	if (got <= 0)
		return true;

	if (!m->audio_clock_valid.load(std::memory_order_relaxed)) {
		// First audio after open/seek: anchor the audio clock at this
		// frame's PTS and the node's current write position.
		m->audio_base.store(
		    m->audio_node->stream_write_pos.load(std::memory_order_relaxed),
		    std::memory_order_relaxed);
		m->audio_pts_base.store(pts, std::memory_order_relaxed);
		m->audio_clock_valid.store(true, std::memory_order_release);
	}

	// Cut #22b Stage 2: tap the resampled audio into the visualizer
	// ring. Interleaved stereo f32 → mono downmix (L+R)/2. Held under a
	// short mutex so the reader can snapshot a consistent window.
	{
		std::lock_guard<std::mutex> tap_lock(m->tap_mutex);
		const float *sp = m->audio_scratch.data();
		uint32_t wp = m->tap_write_pos;
		for (int i = 0; i < got; i++) {
			m->tap_ring[wp] = 0.5f * (sp[i * 2] + sp[i * 2 + 1]);
			wp = (wp + 1) & TAP_MASK;
		}
		m->tap_write_pos = wp;
		m->tap_written += (uint64_t)got;
	}

	const float *src = m->audio_scratch.data();
	uint32_t remaining = (uint32_t)got;
	while (remaining > 0) {
		uint32_t wrote =
		    nx_audio_stream_write(m->audio_node, src, remaining);
		src += (size_t)wrote * 2;
		remaining -= wrote;
		if (remaining > 0 && !decode_wait(m))
			return false;
	}
	return true;
}

// Receive all pending frames from a codec. `seek_drop_until` (media seconds,
// or <0) drops frames decoded while converging on a seek target. Returns
// false if interrupted.
bool receive_frames(nx_media *m, AVCodecContext *ctx, int stream_index,
                    AVFrame *frame, double seek_drop_until) {
	while (true) {
		int ret = avcodec_receive_frame(ctx, frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			return true;
		if (ret < 0) {
			set_fatal(m, "decode error", ret);
			return false;
		}
		double pts = frame_pts_seconds(m, frame, stream_index) +
		             m->loop_offset;
		bool is_video = stream_index == m->vstream;
		// Seek convergence: drop everything strictly before the target —
		// with half a frame of slack for video so the frame *containing*
		// the target is kept.
		double drop_before = is_video
		                         ? seek_drop_until - m->vframe_dur * 0.5
		                         : seek_drop_until - 0.005;
		if (seek_drop_until >= 0 && pts < drop_before) {
			av_frame_unref(frame);
			continue;
		}
		bool ok = is_video ? enqueue_video(m, frame, pts)
		                   : enqueue_audio(m, frame, pts);
		if (is_video && ok && m->seeking.load(std::memory_order_relaxed) &&
		    seek_drop_until >= 0) {
			// First on-target frame after a seek: the seek is complete.
			m->seeking.store(false, std::memory_order_release);
		}
		av_frame_unref(frame);
		if (!ok)
			return false;
	}
}

// Drain both decoders at EOF. Returns false if interrupted.
bool drain_decoders(nx_media *m, AVFrame *frame, double seek_drop_until) {
	if (m->vctx) {
		avcodec_send_packet(m->vctx, NULL);
		if (!receive_frames(m, m->vctx, m->vstream, frame, seek_drop_until))
			return false;
	}
	if (m->actx && m->audio_node) {
		avcodec_send_packet(m->actx, NULL);
		if (!receive_frames(m, m->actx, m->astream, frame, seek_drop_until))
			return false;
	}
	return true;
}

// Flush codec + demuxer state and seek the container. Caller is the decode
// thread. `to_seconds` is in the un-looped media domain.
bool container_seek(nx_media *m, double to_seconds) {
	if (m->vctx)
		avcodec_flush_buffers(m->vctx);
	if (m->actx)
		avcodec_flush_buffers(m->actx);
	if (m->swr) {
		swr_free(&m->swr); // drop resampler delay state
	}
	int64_t ts = (int64_t)(to_seconds * AV_TIME_BASE);
	int ret = av_seek_frame(m->fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
	if (ret < 0) {
		// Fall back to a byte-position rewind for streams without an index.
		ret = av_seek_frame(m->fmt, -1, 0,
		                    AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_BYTE);
	}
	return ret >= 0;
}

// Handle a user-requested seek (decode thread).
void do_seek(nx_media *m, double *seek_drop_until) {
	double target = m->seek_target.load();
	// Discard everything queued: the main thread is guaranteed not to touch
	// the video ring while `seeking` is true.
	m->vread.store(0, std::memory_order_relaxed);
	m->vwrite.store(0, std::memory_order_relaxed);
	if (m->audio_node) {
		nx_audio_stream_flush(m->audio_node);
		m->audio_clock_valid.store(false, std::memory_order_relaxed);
	}
	m->loop_offset = 0;
	m->eof.store(false);
	if (!container_seek(m, target)) {
		set_fatal(m, "seek failed", 0);
		m->seeking.store(false);
		return;
	}
	*seek_drop_until = target;
	m->present_force.store(true);
	// Cut #22c (2026-07-10): reset the audio-stall tracker so the first
	// post-anchor clock_now() call sees `consumed != audio_consumed_last`
	// and re-engages the audio-slave clock immediately (rather than
	// starting one tick in "stalled" mode).
	m->audio_consumed_last = 0;
	// Ledger #115: reset the monotonic gate to the seek target so the
	// first post-seek clock_now can validly return audio-anchored PTS
	// values around the target (which are lower than pre-seek `t`).
	m->last_clock = target;
	if (m->vstream < 0) {
		// No video track: nothing will flip `seeking` in receive_frames.
		m->seeking.store(false);
	}
}

// Demux-ahead queue bounds (2026-09-06). Generous so a transient stall in one
// stream (e.g. a briefly-full video ring) doesn't starve the reader of the
// other stream: while the video ring backs up, the reader keeps pulling
// packets (up to VPKTQ_MAX ~4 s of video) and routing audio into apktq so
// audio never underruns. The audio queue is larger in count than the video
// queue's time-equivalent so it never gates the reader before vpktq does.
constexpr size_t VPKTQ_MAX = 120;
constexpr size_t APKTQ_MAX = 400;

// Free all queued packets + any held video frame. Called on seek (before
// do_seek) and at thread exit so no compressed data or half-decoded frame
// survives a flush.
void flush_pktqs(nx_media *m) {
	while (!m->vpktq.empty()) {
		AVPacket *p = m->vpktq.front();
		m->vpktq.pop_front();
		av_packet_free(&p);
	}
	while (!m->apktq.empty()) {
		AVPacket *p = m->apktq.front();
		m->apktq.pop_front();
		av_packet_free(&p);
	}
	if (m->v_pending_valid) {
		av_frame_unref(m->v_pending);
		m->v_pending_valid = false;
	}
	m->demux_eof = false;
}

// One non-blocking video decode step. `scratch` is the thread's reusable
// AVFrame. Returns true if it made any progress (sent a packet, produced or
// enqueued a frame, dropped a pre-seek frame). Never blocks on the video ring:
// a decoded frame that doesn't fit is stashed in v_pending and retried later.
bool pump_video(nx_media *m, AVFrame *scratch, double *seek_drop_until) {
	if (m->vstream < 0 || !m->vctx)
		return false;
	// Retry a previously-rejected frame first.
	if (m->v_pending_valid) {
		int r = try_enqueue_video(m, m->v_pending, m->v_pending_pts);
		if (r <= 0)
			return false; // ring still full (0) or fatal (-1)
		if (m->seeking.load(std::memory_order_relaxed) &&
		    *seek_drop_until >= 0)
			m->seeking.store(false, std::memory_order_release);
		av_frame_unref(m->v_pending);
		m->v_pending_valid = false;
		return true;
	}
	// Don't decode more while the ring is full (would have nowhere to go).
	if (m->vwrite.load(std::memory_order_relaxed) -
	        m->vread.load(std::memory_order_acquire) >=
	    RING_SLOTS)
		return false;
	int ret = avcodec_receive_frame(m->vctx, scratch);
	if (ret == AVERROR(EAGAIN)) {
		if (m->vpktq.empty())
			return false; // need the reader to supply packets
		AVPacket *pkt = m->vpktq.front();
		m->vpktq.pop_front();
		avcodec_send_packet(m->vctx, pkt); // succeeds: receive just drained
		av_packet_free(&pkt);
		return true;
	}
	if (ret == AVERROR_EOF)
		return false;
	if (ret < 0) {
		set_fatal(m, "decode error", ret);
		return false;
	}
	double pts = frame_pts_seconds(m, scratch, m->vstream) + m->loop_offset;
	if (*seek_drop_until >= 0 && pts < *seek_drop_until - m->vframe_dur * 0.5) {
		av_frame_unref(scratch); // pre-target frame during seek convergence
		return true;
	}
	int r = try_enqueue_video(m, scratch, pts);
	if (r == 1) {
		if (m->seeking.load(std::memory_order_relaxed) &&
		    *seek_drop_until >= 0)
			m->seeking.store(false, std::memory_order_release);
		av_frame_unref(scratch);
	} else if (r == 0) {
		// Ring filled between the check above and now: hold the frame.
		av_frame_move_ref(m->v_pending, scratch);
		m->v_pending_valid = true;
		m->v_pending_pts = pts;
	} else {
		av_frame_unref(scratch); // fatal
	}
	return true;
}

// One audio decode step. Only decodes while the audio ring is under a small
// buffer target, so it keeps audio fed without blocking or running so far
// ahead that it stalls the reader (and thereby video). Returns true on
// progress.
bool pump_audio(nx_media *m, AVFrame *scratch, double *seek_drop_until) {
	if (m->astream < 0 || !m->actx || !m->audio_node)
		return false;
	// Keep ~0.25 s buffered in the audio ring; above that, yield to video/read.
	uint32_t target = (uint32_t)(0.25 * m->audio_out_rate);
	if (nx_audio_stream_pending(m->audio_node) >= target)
		return false;
	int ret = avcodec_receive_frame(m->actx, scratch);
	if (ret == AVERROR(EAGAIN)) {
		if (m->apktq.empty())
			return false;
		AVPacket *pkt = m->apktq.front();
		m->apktq.pop_front();
		avcodec_send_packet(m->actx, pkt);
		av_packet_free(&pkt);
		return true;
	}
	if (ret == AVERROR_EOF)
		return false;
	if (ret < 0) {
		set_fatal(m, "decode error", ret);
		return false;
	}
	double pts = frame_pts_seconds(m, scratch, m->astream) + m->loop_offset;
	if (*seek_drop_until >= 0 && pts < *seek_drop_until - 0.005) {
		av_frame_unref(scratch);
		return true;
	}
	// With ~0.25 s of headroom checked above, enqueue_audio won't block here.
	bool ok = enqueue_audio(m, scratch, pts);
	av_frame_unref(scratch);
	return ok;
}

void decode_thread_main(nx_media *m) {
	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	m->v_pending = av_frame_alloc();
	if (!pkt || !frame || !m->v_pending) {
		set_fatal(m, "out of memory", 0);
		av_packet_free(&pkt);
		av_frame_free(&frame);
		av_frame_free(&m->v_pending);
		return;
	}
	double seek_drop_until = -1;

	while (!m->quit.load()) {
		if (m->seek_requested.exchange(false)) {
			flush_pktqs(m); // drop queued packets + pending frame first
			do_seek(m, &seek_drop_until);
			continue;
		}
		if (m->fatal.load() || m->eof.load()) {
			// Parked: wait for a command (seek/quit).
			std::unique_lock<std::mutex> lock(m->ctl_mutex);
			m->ctl_cv.wait_for(lock, std::chrono::milliseconds(100));
			continue;
		}

		bool progress = false;

		// 1. Refill the packet queues (until one is full or the demuxer ends).
		// Cap the burst so decoding interleaves with reading — otherwise a big
		// refill (startup / post-seek, queues empty) would block the pumps and
		// leave the rings empty longer than necessary.
		int reads = 0;
		while (!m->demux_eof && m->vpktq.size() < VPKTQ_MAX &&
		       m->apktq.size() < APKTQ_MAX && reads++ < 16) {
			int ret = av_read_frame(m->fmt, pkt);
			if (ret == AVERROR_EOF) {
				m->demux_eof = true;
				break;
			}
			if (ret < 0) {
				set_fatal(m, "demux error", ret);
				break;
			}
			if (pkt->stream_index == m->vstream && m->vctx) {
				AVPacket *q = av_packet_alloc();
				av_packet_move_ref(q, pkt);
				m->vpktq.push_back(q);
				progress = true;
			} else if (pkt->stream_index == m->astream && m->actx &&
			           m->audio_node) {
				AVPacket *q = av_packet_alloc();
				av_packet_move_ref(q, pkt);
				m->apktq.push_back(q);
				progress = true;
			} else {
				av_packet_unref(pkt); // other stream (subtitles, etc.)
			}
		}

		// 2. Decode. Audio first (never let it starve), then video.
		if (pump_audio(m, frame, &seek_drop_until))
			progress = true;
		if (pump_video(m, frame, &seek_drop_until))
			progress = true;

		// Seek converged (first on-target frame enqueued, or audio-only): stop
		// dropping.
		if (seek_drop_until >= 0 && !m->seeking.load(std::memory_order_relaxed))
			seek_drop_until = -1;

		// 3. Finalize at end of stream: reader done, queues empty, no held
		// frame — flush the decoders for their last buffered frames, then loop
		// or park.
		if (m->demux_eof && m->vpktq.empty() && m->apktq.empty() &&
		    !m->v_pending_valid) {
			if (!drain_decoders(m, frame, seek_drop_until))
				continue; // interrupted by quit/seek
			seek_drop_until = -1;
			if (m->looping.load()) {
				double advance = m->duration > 0 ? m->duration : 0;
				if (!container_seek(m, 0)) {
					set_fatal(m, "loop seek failed", 0);
					continue;
				}
				m->loop_offset += advance;
				m->demux_eof = false; // resume reading from the top
				continue;
			}
			m->eof.store(true); // top-of-loop park catches it next iteration
			continue;
		}

		// 4. Nothing advanced (rings full + queues full, waiting on the
		// presenter/audio to drain): brief sleep to avoid a busy-spin.
		if (!progress)
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}

	flush_pktqs(m);
	av_frame_free(&m->v_pending);
	av_packet_free(&pkt);
	av_frame_free(&frame);
}

// ---------------------------------------------------------------------------
// Clock (main thread)
// ---------------------------------------------------------------------------

double clock_now(nx_media *m) {
	// Ledger #113 (Fix B, 2026-07-10): video-follows-audio when audio is
	// advancing.
	// Ledger #115 (2026-07-10): + monotonic gate + wall-cap during stall.
	// Diagnostic runs on Citron post-#113 showed periodic audio ring
	// underruns (~1 Hz) — during each ~150 ms underrun, `consumed` was
	// frozen, wall-fallback extrapolated `t` forward, then on resume the
	// fresh `at` was ~150 ms behind the extrapolated `t` → visible
	// frame regression. Two-part cure:
	//   (a) Monotonic gate: `t` is clamped to `>= last_clock`; when audio
	//       resumes below the wall-extrapolated position, video PAUSES on
	//       the last shown frame instead of regressing. Pauses are much
	//       less perceptible than backward jumps.
	//   (b) Wall-cap during stall: extrapolation is bounded by
	//       `ring_newest_pts + vframe_dur`. Present() still drains the
	//       ring (cut #22c deadlock contract preserved), but wall can't
	//       race far past what's actually decoded — so the resume-catch-up
	//       gap stays small.
	// Companion #115 fix: RING_SLOTS 3 → 12 (bigger video buffer prevents
	// the underrun from happening as often in the first place).
	double computed;
	if (m->clock_running && m->audio_node &&
	    m->audio_clock_valid.load(std::memory_order_acquire)) {
		uint64_t consumed = nx_audio_stream_consumed(m->audio_node);
		uint64_t base = m->audio_base.load(std::memory_order_relaxed);
		bool audio_advancing = consumed != m->audio_consumed_last;
		m->audio_consumed_last = consumed;
		if (consumed > base && audio_advancing) {
			double at = m->audio_pts_base.load(std::memory_order_relaxed) +
			            (double)(consumed - base) / m->audio_out_rate;
			m->clock_base = at;
			m->clock_anchor = std::chrono::steady_clock::now();
			computed = at;
		} else {
			// Stall path: wall-extrapolate from last `at` but cap at
			// ring_newest_pts + one frame so we don't race far ahead.
			double t_wall = m->clock_base +
			                std::chrono::duration<double>(
			                    std::chrono::steady_clock::now() -
			                    m->clock_anchor)
			                    .count();
			uint64_t w = m->vwrite.load(std::memory_order_acquire);
			uint64_t r = m->vread.load(std::memory_order_relaxed);
			if (w > r) {
				double newest_pts =
				    m->slots[(w - 1) % RING_SLOTS].pts;
				double cap = newest_pts + m->vframe_dur;
				if (t_wall > cap) t_wall = cap;
			}
			computed = t_wall;
		}
	} else {
		// Pure wall clock: no audio, video-only, or pre-first-audio-frame.
		computed = m->clock_base;
		if (m->clock_running) {
			computed += std::chrono::duration<double>(
			                std::chrono::steady_clock::now() -
			                m->clock_anchor)
			                .count();
		}
	}
	// Monotonic gate: never regress. Even a small backward step causes a
	// visible frame regression; a small forward pause is much less
	// perceptible.
	if (computed < m->last_clock) computed = m->last_clock;
	m->last_clock = computed;
	return computed;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// http(s):// sources are opened by libavformat itself — its http/https
// protocols + hls demuxer fetch the playlist and the rolling media segments
// over TLS (the libnx backend baked into switch-ffmpeg as tls_libnx), so there
// is NO fopen and NO custom AVIO wrap. Every other scheme (sdmc:/, romfs:/,
// disk paths, and in-memory buffers) still goes through fopen/memory +
// avio_read_cb, because FFmpeg's protocol layer doesn't understand those.
// Restored from the QuickJS-era video.c network branch that the V8 migration
// dropped when it adopted upstream nx.js's local-only media pipeline.
static bool nx_is_network_url(const char *path) {
	if (!path) return false;
	return strncmp(path, "http://", 7) == 0 ||
	       strncmp(path, "https://", 8) == 0;
}

// Default User-Agent for network media (2026-09-17).
//
// libavformat otherwise sends `Lavf/<version>`, which a large slice of public
// HLS rejects outright — the origin either 403s or drops the connection. This
// is not a theoretical concern: measured over 70 iptv-org streams that carry
// an `#EXTVLCOPT:http-user-agent` line, the stock UA played 27 and a plain
// browser UA played 44, with zero regressions across a separate 80-stream
// sample that carries no such line (60 -> 63).
//
// Deliberately a plain desktop-Chrome string with NO `Brewser/1.0` token.
// The engine-detection token belongs on the *page* User-Agent (see the
// runtime's UA plumbing); appending an unrecognised token here is exactly
// what draws the 403s this constant exists to avoid. Keep the two separate.
#define NX_MEDIA_DEFAULT_USER_AGENT                                            \
	"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, "    \
	"like Gecko) Chrome/131.0.0.0 Safari/537.36"

// Copy `src` into `dst` with CR and LF removed, NUL-terminated.
//
// `user_agent` and `referer` are written into the request head by
// libavformat's http protocol, so a raw CR/LF in either would terminate the
// header line early and let the remainder be read as additional headers. The
// values reach us from remotely-fetched M3U playlists, i.e. attacker-shaped
// content, so they are sanitised at the boundary rather than trusted.
// Returns false when `src` is null/empty (nothing to set).
static bool nx_sanitize_header_value(const char *src, char *dst,
                                     size_t dst_size) {
	if (!src || !dst || dst_size == 0) return false;
	size_t w = 0;
	for (size_t r = 0; src[r] && w + 1 < dst_size; r++) {
		const char c = src[r];
		if (c == '\r' || c == '\n') continue;
		dst[w++] = c;
	}
	dst[w] = '\0';
	return w > 0;
}

// libavformat interrupt hook. Returning non-zero makes the blocking call in
// progress bail out with AVERROR_EXIT. Called very frequently from inside
// IO, so it must stay trivial — one relaxed atomic load.
static int nx_media_interrupt_cb(void *opaque) {
	nx_media *m = static_cast<nx_media *>(opaque);
	if (!m || !m->abort_flag) return 0;
	return m->abort_flag->load(std::memory_order_relaxed) ? 1 : 0;
}

// Stable JS-facing classification of an open failure. The point is to let a
// caller distinguish "this stream is gone, stop offering it" from "the
// network hiccuped, try again later" — a distinction that matters a lot for
// catalogue apps, which otherwise blacklist a working channel on one blip.
const char *nx_media_fail_kind(int av_err) {
	if (av_err == 0) return "unknown";
	switch (av_err) {
	case AVERROR_HTTP_FORBIDDEN:     return "forbidden";     // 403 — geo/hdr
	case AVERROR_HTTP_NOT_FOUND:     return "not-found";     // 404 — gone
	case AVERROR_HTTP_UNAUTHORIZED:  return "unauthorized";  // 401
	case AVERROR_HTTP_SERVER_ERROR:  return "server-error";  // 5xx — transient
	case AVERROR_HTTP_BAD_REQUEST:   return "bad-request";   // 400
	case AVERROR_HTTP_OTHER_4XX:     return "client-error";
	case AVERROR_INVALIDDATA:        return "format";
	case AVERROR_PROTOCOL_NOT_FOUND: return "format";
	case AVERROR_EXIT:               return "aborted"; // cancelled by us
	default: break;
	}
	// Not compile-time constants on every platform, so tested separately.
	if (av_err == AVERROR(ETIMEDOUT)) return "timeout";
	if (av_err == AVERROR(ENOENT))    return "not-found-local";
	if (av_err == AVERROR(ECONNREFUSED) || av_err == AVERROR(ECONNRESET) ||
	    av_err == AVERROR(EHOSTUNREACH) || av_err == AVERROR(ENETUNREACH) ||
	    av_err == AVERROR(EIO)) {
		return "network";
	}
	return "unknown";
}

nx_media_t *nx_media_open(const char *path, const uint8_t *mem,
                          size_t mem_size, std::shared_ptr<void> keepalive,
                          char *errbuf, size_t errbuf_size, bool want_yuv,
                          const nx_media_net_t *net, int *av_err,
                          std::atomic<bool> *abort_flag, bool want_hw_decode) {
	nx_media *m = new nx_media();
	m->mem_hold = std::move(keepalive);
	m->out_yuv = want_yuv;
	m->abort_flag = abort_flag;
#if defined(NX_ENABLE_NVTEGRA)
	m->want_hw = want_hw_decode;
#endif
	int ret = 0;
	const AVCodec *vcodec = NULL;
	const AVCodec *acodec = NULL;
	unsigned char *avio_buf = NULL;
	AVDictionary *net_opts = NULL; // declared here so the fail: gotos don't
	                               // cross its initialization

	const bool is_network = nx_is_network_url(path);

	if (is_network) {
		// Network (http/https): libavformat owns the IO and, for HLS, its
		// hls demuxer fetches the rolling media segments itself. No fopen,
		// no custom AVIO — pass the URL straight to avformat_open_input.
		m->fmt = avformat_alloc_context();
		if (!m->fmt) {
			snprintf(errbuf, errbuf_size, "out of memory");
			goto fail;
		}
		// Install the abort hook BEFORE the open, so a cancel that arrives
		// while we are still connecting is honoured. rw_timeout below is the
		// backstop for a server that accepts and then stalls; this is the
		// path that makes a user-initiated cancel instant.
		m->fmt->interrupt_callback.callback = nx_media_interrupt_cb;
		m->fmt->interrupt_callback.opaque = m;
		// Bound blocking network reads/writes. The open now runs on a worker
		// thread (video-decoder.cc), and a close() may join it — without a
		// timeout a hung/slow server could block the join (and any read
		// during playback) indefinitely. 30 s (in microseconds) is generous
		// for a LAN transcode while still guaranteeing forward progress.
		av_dict_set(&net_opts, "rw_timeout", "30000000", 0);
		// Protocol allow-list. libavformat will otherwise open ANY compiled-in
		// protocol that a playlist names, and a playlist is attacker-shaped
		// content: an HLS/DASH manifest fetched from a permitted origin can
		// point a segment at `file:`, `concat:` or `subfile:` and have the
		// demuxer read local files on its behalf — the console's own SD card,
		// including other apps' data, all below the runtime's path sandbox
		// because none of it goes through `Switch.readFile`.
		//
		// Restricting to the network transports the HLS/DASH path actually
		// needs turns that into "Protocol not found". `crypto` is required for
		// AES-128 encrypted HLS segments; `hls`/`dash`/`http`/`https`/`tcp`/
		// `tls` are the demuxers and transports themselves.
		//
		// NOT closed by this: a permitted origin can still 30x the decoder to
		// another HOST. libavformat follows redirects inside http.c with no
		// AVOption to veto them (this build exposes neither `follow_redirects`
		// nor `max_redirects`), so a per-host check would need an FFmpeg patch.
		// The whitelist at least keeps a redirect on http/https.
		av_dict_set(&net_opts, "protocol_whitelist",
		            "http,https,tcp,tls,crypto,hls,dash", 0);

		// ---- request shaping (2026-09-17) -------------------------------
		// `user_agent` and `referer` are dedicated http-protocol AVOptions,
		// so libavformat composes the header lines itself — we never build a
		// raw header blob, which keeps the injection surface to the value
		// sanitiser below rather than to string concatenation.
		//
		// These two options MATTER BEYOND THE MANIFEST: libavformat's HLS
		// demuxer harvests a fixed set of options off the parent AVIOContext
		// (ffio_copy_url_options: headers, user_agent, cookies, http_proxy,
		// referer, rw_timeout, icy) and replays them on every segment and
		// key fetch. So a UA/Referer set here follows the whole rolling
		// stream, not just the playlist request. That list is also why the
		// reconnect options below cannot carry the load — see their comment.
		char hdr_buf[1024];
		if (net && nx_sanitize_header_value(net->user_agent, hdr_buf,
		                                    sizeof(hdr_buf))) {
			av_dict_set(&net_opts, "user_agent", hdr_buf, 0);
		} else {
			av_dict_set(&net_opts, "user_agent",
			            NX_MEDIA_DEFAULT_USER_AGENT, 0);
		}
		if (net && nx_sanitize_header_value(net->referer, hdr_buf,
		                                    sizeof(hdr_buf))) {
			av_dict_set(&net_opts, "referer", hdr_buf, 0);
		}

		// ---- live-stream resilience -------------------------------------
		// `seg_max_retry` is the one that counts for HLS. It defaults to 0,
		// meaning a SINGLE failed segment fetch — a blip on a live edge that
		// would recover on the next attempt — tears the whole decode down.
		// Two retries costs nothing on a healthy stream.
		//
		// NOTE the asymmetry with the http `reconnect*` options set after
		// it: those are NOT in ffio_copy_url_options' list, so they apply
		// only to this top-level connection and never reach HLS segment
		// fetches. They are still worth setting — they cover direct http
		// media (mp4/ts/mp3) and the playlist connection itself — but they
		// are NOT a substitute for seg_max_retry, and removing seg_max_retry
		// on the assumption that "reconnect covers it" would silently undo
		// this. `max_reload` covers playlist refreshes that come back short.
		av_dict_set(&net_opts, "seg_max_retry", "2", 0);
		av_dict_set(&net_opts, "max_reload", "5", 0);
		av_dict_set(&net_opts, "reconnect", "1", 0);
		av_dict_set(&net_opts, "reconnect_streamed", "1", 0);
		av_dict_set(&net_opts, "reconnect_on_network_error", "1", 0);
		// Bounded so a dead host still fails inside the rw_timeout budget
		// rather than retrying until the join blocks.
		av_dict_set(&net_opts, "reconnect_delay_max", "4", 0);

		ret = avformat_open_input(&m->fmt, path, NULL, &net_opts);
		av_dict_free(&net_opts);
		if (ret < 0) {
			// Surface the real AVERROR (DNS / TLS handshake / HTTP 4xx /
			// "Protocol not found") — network opens fail many distinct ways.
			av_strerror(ret, errbuf, errbuf_size);
			if (av_err) *av_err = ret;
			goto fail;
		}
	} else {
		if (path) {
			m->file = fopen(path, "rb");
			if (!m->file) {
				snprintf(errbuf, errbuf_size, "failed to open file");
				goto fail;
			}
			fseek(m->file, 0, SEEK_END);
			m->file_size = (int64_t)ftell(m->file);
			fseek(m->file, 0, SEEK_SET);
		} else {
			m->mem = mem;
			m->mem_size = mem_size;
		}

		avio_buf = (unsigned char *)av_malloc(65536);
		if (!avio_buf) {
			snprintf(errbuf, errbuf_size, "out of memory");
			goto fail;
		}
		m->avio = avio_alloc_context(avio_buf, 65536, 0, m, avio_read_cb, NULL,
		                             avio_seek_cb);
		if (!m->avio) {
			av_free(avio_buf);
			snprintf(errbuf, errbuf_size, "out of memory");
			goto fail;
		}
		m->fmt = avformat_alloc_context();
		if (!m->fmt) {
			snprintf(errbuf, errbuf_size, "out of memory");
			goto fail;
		}
		m->fmt->pb = m->avio;

		ret = avformat_open_input(&m->fmt, NULL, NULL, NULL);
		if (ret < 0) {
			av_strerror(ret, errbuf, errbuf_size);
			goto fail;
		}
	}
	ret = avformat_find_stream_info(m->fmt, NULL);
	if (ret < 0) {
		av_strerror(ret, errbuf, errbuf_size);
		goto fail;
	}

	m->vstream = av_find_best_stream(m->fmt, AVMEDIA_TYPE_VIDEO, -1, -1,
	                                 &vcodec, 0);
	m->astream = av_find_best_stream(m->fmt, AVMEDIA_TYPE_AUDIO, -1, -1,
	                                 &acodec, 0);
	if (m->vstream < 0 && m->astream < 0) {
		snprintf(errbuf, errbuf_size, "no playable streams");
		goto fail;
	}

	if (m->vstream >= 0) {
		AVCodecParameters *par = m->fmt->streams[m->vstream]->codecpar;
		m->vctx = avcodec_alloc_context3(vcodec);
		if (!m->vctx ||
		    avcodec_parameters_to_context(m->vctx, par) < 0) {
			snprintf(errbuf, errbuf_size, "failed to set up video decoder");
			goto fail;
		}
#if defined(NX_ENABLE_NVTEGRA)
		if (m->want_hw) {
			// 10-bit streams come out of NVDEC as P010 (16-bit samples),
			// not as the 8-bit NV12 this path de-interleaves.  The
			// bit depth is known from the codec parameters (the demuxer
			// probes it), so we can keep those streams on the software
			// decoder instead of failing them at the first frame.
			if (par->bits_per_raw_sample > 8) {
#if NX_NVDEC_DIAG
				fprintf(stderr,
				        "[nvd] open: %dx%d stream is %d-bit -> keeping the "
				        "software decoder\n",
				        par->width, par->height, par->bits_per_raw_sample);
				fflush(stderr);
#endif
				m->want_hw = false;
			}
		}
		if (m->want_hw) {
			// Try the hardware decoder.  av_hwdevice_ctx_create() failing is
			// the only failure mode here; everything else (unsupported codec,
			// unsupported resolution, ...) is decided by libavcodec later and
			// falls back to the software decoder on its own.
			if (av_hwdevice_ctx_create(&m->hw_device_ref,
			                           AV_HWDEVICE_TYPE_NVTEGRA, NULL, NULL,
			                           0) >= 0) {
				// libavcodec owns the reference we hand it; we keep our
				// own for the lifetime of the media.
				m->vctx->hw_device_ctx = av_buffer_ref(m->hw_device_ref);
				m->vctx->pix_fmt = AV_PIX_FMT_NVTEGRA;
				m->hw_decode = true;
			} else {
				m->hw_device_ref = NULL;
			}
#if NX_NVDEC_DIAG
			fprintf(stderr,
			        "[nvd] open: %dx%d want_hw=1 hw_device=%s out_yuv=%d\n",
			        m->vctx->width, m->vctx->height,
			        m->hw_decode ? "ok" : "unavailable", m->out_yuv ? 1 : 0);
			fflush(stderr);
#endif
		}
#endif
		// Two decode threads: leaves the main JS thread + audio render
		// thread breathing room on the Switch's three usable cores.
		// The hardware decoder submits asynchronously and does not benefit
		// from CPU frame threads, so it gets one.
#if defined(NX_ENABLE_NVTEGRA)
		m->vctx->thread_count = m->hw_decode ? 1 : 2;
#else
		m->vctx->thread_count = 2;
#endif
		ret = avcodec_open2(m->vctx, vcodec, NULL);
#if defined(NX_ENABLE_NVTEGRA)
		if (ret < 0 && m->hw_decode) {
			// The codec/stream does not accept the NVDEC pixel format.
			// Retry with a plain software decoder instead of failing the
			// whole open.
#if NX_NVDEC_DIAG
			fprintf(stderr, "[nvd] avcodec_open2 failed with NVTEGRA "
			                "-> retrying with the software decoder\n");
			fflush(stderr);
#endif
			avcodec_free_context(&m->vctx);
			m->vctx = avcodec_alloc_context3(vcodec);
			if (!m->vctx ||
			    avcodec_parameters_to_context(m->vctx, par) < 0) {
				snprintf(errbuf, errbuf_size,
				         "failed to set up video decoder");
				goto fail;
			}
			m->vctx->thread_count = 2;
			m->hw_decode = false;
			ret = avcodec_open2(m->vctx, vcodec, NULL);
		}
#endif
		if (ret < 0) {
			av_strerror(ret, errbuf, errbuf_size);
			goto fail;
		}
		m->width = par->width;
		m->height = par->height;
		AVRational fr = m->fmt->streams[m->vstream]->avg_frame_rate;
		if (fr.num > 0 && fr.den > 0)
			m->vframe_dur = (double)fr.den / fr.num;
		if (m->width <= 0 || m->height <= 0 || m->width > 4096 ||
		    m->height > 4096) {
			snprintf(errbuf, errbuf_size, "unsupported video dimensions");
			goto fail;
		}
		// Ring slot size depends on the output format: I420 is 1.5 B/px
		// (Y|U|V contiguous), BGRA is 4 B/px. The field is named `bgra` for
		// historical reasons; under out_yuv it holds planar I420.
		//
		// On the hardware path the slot holds NV12 planes instead, and
		// those are allocated lazily on the first hardware frame — so the
		// BGRA/I420 buffer is not allocated up front. It is allocated on
		// demand instead, because the decoder may still have fallen back
		// to software.
#if defined(NX_ENABLE_NVTEGRA)
		if (!m->hw_decode) {
#endif
			const size_t slot_bytes =
			    m->out_yuv ? nx_media_i420_size(m->width, m->height)
			               : (size_t)m->width * m->height * 4;
			for (int i = 0; i < RING_SLOTS; i++) {
				m->slots[i].bgra = (uint8_t *)malloc(slot_bytes);
				if (!m->slots[i].bgra) {
					snprintf(errbuf, errbuf_size, "out of memory");
					goto fail;
				}
			}
#if defined(NX_ENABLE_NVTEGRA)
		}
#endif
	}

	if (m->astream >= 0) {
		AVCodecParameters *par = m->fmt->streams[m->astream]->codecpar;
		m->actx = avcodec_alloc_context3(acodec);
		if (!m->actx ||
		    avcodec_parameters_to_context(m->actx, par) < 0 ||
		    avcodec_open2(m->actx, acodec, NULL) < 0) {
			// Audio is non-fatal: play the video silently.
			if (m->actx)
				avcodec_free_context(&m->actx);
			m->astream = -1;
		}
	}

	if (m->fmt->duration != AV_NOPTS_VALUE && m->fmt->duration > 0) {
		m->duration = (double)m->fmt->duration / AV_TIME_BASE;
		m->duration_known = true;
	}

#if NX_MEDIA_META_DIAG
	// One-line metadata summary (2026-10-22): where the page's
	// videoWidth / videoHeight / duration come from.
	//   dur_known=0 -> the source has NO container duration (a streamed
	//   H.264 feed has no moov) -> the JS layer reports NaN there.
	//   dims        -> the container's track size; for a raw H.264 source
	//   ffmpeg's demuxer parsed them out of the first key frame's SPS.
	fprintf(stderr,
	        "[md-meta] open: %dx%d dur=%.3f dur_known=%d fps=%.3f "
	        "has_audio=%d has_video=%d\n",
	        m->width, m->height, m->duration, m->duration_known ? 1 : 0,
	        m->vframe_dur > 0 ? 1.0 / m->vframe_dur : 0.0,
	        m->astream >= 0 ? 1 : 0, m->vstream >= 0 ? 1 : 0);
	fflush(stderr);
#endif

#if MEDIA_DIAG_112
	// One-time open summary. For the 60fps-feasibility probe: content_fps tells
	// us what the device receives (Source direct-play => ~60), and cross-checked
	// against [md-diag:prs] ring depth it reveals whether SW decode sustains it
	// (depth full ~11-12 + no_frame=0 => decode keeps up at the present's
	// ~60 ring-frames/s consumption; draining => decode-bound => slow motion).
	fprintf(stderr,
	        "[md-diag:open] %dx%d content_fps=%.3f vframe_dur=%.4f dur=%.2f "
	        "out_yuv=%d has_audio=%d\n",
	        m->width, m->height,
	        m->vframe_dur > 0 ? 1.0 / m->vframe_dur : -1.0, m->vframe_dur,
	        m->duration, m->out_yuv ? 1 : 0, m->astream >= 0 ? 1 : 0);
	fflush(stderr);
#endif

	m->thread = std::thread(decode_thread_main, m);
	return m;

fail:
	nx_media_destroy(m);
	return NULL;
}

int nx_media_width(nx_media_t *m) { return m->width; }
int nx_media_height(nx_media_t *m) { return m->height; }
double nx_media_duration(nx_media_t *m) { return m->duration; }
bool nx_media_duration_known(nx_media_t *m) { return m->duration_known; }
// Content frame rate (from the stream's avg_frame_rate; see vframe_dur set at
// open). Drives the shell's swap-interval decision: >~32fps content wants a
// 60 Hz present, not the 30 Hz pacing lock (which would halve it to 30 shown).
double nx_media_content_fps(nx_media_t *m) {
	return m->vframe_dur > 0 ? 1.0 / m->vframe_dur : 0.0;
}
bool nx_media_has_audio(nx_media_t *m) { return m->astream >= 0; }
bool nx_media_has_video(nx_media_t *m) { return m->vstream >= 0; }
bool nx_media_is_yuv(nx_media_t *m) { return m->out_yuv; }
int nx_media_yuv_colorspace(nx_media_t *m) { return m->yuv_cs; }

void nx_media_set_audio_node(nx_media_t *m, nx_audio_node *node,
                             double sample_rate) {
	m->audio_node = node;
	m->audio_out_rate = sample_rate;
}

void nx_media_play(nx_media_t *m) {
	if (m->playing.load())
		return;
	m->clock_anchor = std::chrono::steady_clock::now();
	m->clock_running = true;
	m->playing.store(true);
	if (m->audio_node)
		nx_audio_stream_set_playing(m->audio_node, true);
	m->ctl_cv.notify_all();
}

void nx_media_pause(nx_media_t *m) {
	if (!m->playing.load())
		return;
	m->clock_base = clock_now(m);
	m->clock_running = false;
	m->playing.store(false);
	if (m->audio_node)
		nx_audio_stream_set_playing(m->audio_node, false);
}

void nx_media_seek(nx_media_t *m, double seconds) {
	if (seconds < 0)
		seconds = 0;
	if (m->duration > 0 && seconds > m->duration)
		seconds = m->duration;
	// Ledger #115: reset monotonic gate to the seek target immediately
	// (main thread) so between seek() and do_seek() (decode thread)
	// nx_media_current_time queries return sane values. do_seek also
	// updates last_clock, redundant but idempotent.
	m->last_clock = seconds;
	// Stop presentation/clock reads of the ring first.
	m->seeking.store(true, std::memory_order_release);
	m->clock_base = seconds;
	m->clock_anchor = std::chrono::steady_clock::now();
	m->seek_target.store(seconds);
	m->seek_requested.store(true);
	m->ctl_cv.notify_all();
}

void nx_media_set_loop(nx_media_t *m, bool loop) {
	m->looping.store(loop);
	m->ctl_cv.notify_all();
}

bool nx_media_present(nx_media_t *m, uint8_t **buffer_inout) {
	if (m->seeking.load(std::memory_order_acquire) || m->fatal.load())
		return false;
	double t = clock_now(m);
	uint64_t r = m->vread.load(std::memory_order_relaxed);
	uint64_t w = m->vwrite.load(std::memory_order_acquire);
	int64_t candidate = -1;
	for (uint64_t i = r; i < w; i++) {
		if (m->slots[i % RING_SLOTS].pts <= t + PRESENT_EPSILON)
			candidate = (int64_t)i;
		else
			break;
	}
#if MEDIA_DIAG_112
	// Aggregate over 60 present() calls (~3 s at 20 fps rAF). Extended
	// (ledger #114) with audio-side snapshots to pin the residual jitter
	// after Fix B (#113):
	//   depth_min/max/avg  — video ring occupancy
	//   no_frame / skip    — as before
	//   aur_pending_min/max — audio ring depth (frames of buffered audio)
	//                         over the window
	//   aur_underruns      — count of render-quantum underrun events since
	//                         last window (audio ring emptied → consumer
	//                         emits silence → `consumed` stalls)
	//   t_min/max          — clock_now returns over the window
	//   ring_pts_min/max   — video ring PTS range at present() calls
	static uint32_t _diag_prs_calls = 0;
	static uint64_t _diag_prs_depth_sum = 0;
	static uint32_t _diag_prs_depth_min = 0xFFFFFFFFu;
	static uint32_t _diag_prs_depth_max = 0;
	static uint32_t _diag_prs_no_frame = 0;
	static uint32_t _diag_prs_skip = 0;
	static uint32_t _diag_aur_pending_min = 0xFFFFFFFFu;
	static uint32_t _diag_aur_pending_max = 0;
	static uint64_t _diag_aur_underrun_prev = 0;
	static double _diag_t_min = 1e18;
	static double _diag_t_max = -1e18;
	static double _diag_ring_oldest_min = 1e18;
	static double _diag_ring_newest_max = -1e18;
	uint32_t _diag_depth = (uint32_t)(w - r);
	_diag_prs_calls++;
	_diag_prs_depth_sum += _diag_depth;
	if (_diag_depth < _diag_prs_depth_min) _diag_prs_depth_min = _diag_depth;
	if (_diag_depth > _diag_prs_depth_max) _diag_prs_depth_max = _diag_depth;
	if (candidate < 0) _diag_prs_no_frame++;
	else if (candidate > (int64_t)r) _diag_prs_skip++;
	if (t < _diag_t_min) _diag_t_min = t;
	if (t > _diag_t_max) _diag_t_max = t;
	if (_diag_depth > 0) {
		double _rop = m->slots[r % RING_SLOTS].pts;
		double _rnp = m->slots[(w - 1) % RING_SLOTS].pts;
		if (_rop < _diag_ring_oldest_min) _diag_ring_oldest_min = _rop;
		if (_rnp > _diag_ring_newest_max) _diag_ring_newest_max = _rnp;
	}
	if (m->audio_node) {
		uint32_t _pending = nx_audio_stream_pending(m->audio_node);
		if (_pending < _diag_aur_pending_min) _diag_aur_pending_min = _pending;
		if (_pending > _diag_aur_pending_max) _diag_aur_pending_max = _pending;
	}
	if (_diag_prs_calls >= 60) {
		double _avg = (double)_diag_prs_depth_sum / _diag_prs_calls;
		uint64_t _aur_underrun_now =
		    m->audio_node ? nx_audio_stream_underrun_count(m->audio_node) : 0;
		uint64_t _aur_underrun_delta = _aur_underrun_now - _diag_aur_underrun_prev;
		_diag_aur_underrun_prev = _aur_underrun_now;
		fprintf(stderr,
		        "[md-diag:prs] n=%u depth_min=%u max=%u avg=%.2f "
		        "no_frame=%u skip=%u aur_pending_min=%u max=%u "
		        "aur_underruns=%llu t_min=%.3f t_max=%.3f "
		        "ring_oldest_min=%.3f ring_newest_max=%.3f\n",
		        _diag_prs_calls, _diag_prs_depth_min, _diag_prs_depth_max,
		        _avg, _diag_prs_no_frame, _diag_prs_skip,
		        _diag_aur_pending_min, _diag_aur_pending_max,
		        (unsigned long long)_aur_underrun_delta,
		        _diag_t_min, _diag_t_max,
		        _diag_ring_oldest_min, _diag_ring_newest_max);
		_diag_prs_calls = 0;
		_diag_prs_depth_sum = 0;
		_diag_prs_depth_min = 0xFFFFFFFFu;
		_diag_prs_depth_max = 0;
		_diag_prs_no_frame = 0;
		_diag_prs_skip = 0;
		_diag_aur_pending_min = 0xFFFFFFFFu;
		_diag_aur_pending_max = 0;
		_diag_t_min = 1e18;
		_diag_t_max = -1e18;
		_diag_ring_oldest_min = 1e18;
		_diag_ring_newest_max = -1e18;
	}
	// Per-call verbose trace for the first 100 present() calls (~5s at
	// 20fps rAF). Lets us see the per-tick relationship of t / ring PTS /
	// candidate / audio ring level during startup.
	static uint32_t _diag_vpr_left = 100;
	if (_diag_vpr_left > 0) {
		_diag_vpr_left--;
		double _rop = _diag_depth > 0 ? m->slots[r % RING_SLOTS].pts : -1.0;
		double _rnp = _diag_depth > 0 ? m->slots[(w - 1) % RING_SLOTS].pts
		                              : -1.0;
		uint32_t _apend = m->audio_node
		                      ? nx_audio_stream_pending(m->audio_node)
		                      : 0;
		fprintf(stderr,
		        "[md-diag:vpr] t=%.4f dep=%u rop=%.3f rnp=%.3f cand=%lld "
		        "apend=%u\n",
		        t, _diag_depth, _rop, _rnp, (long long)candidate, _apend);
	}
#endif
	if (candidate < 0 && w > r &&
	    m->present_force.exchange(false, std::memory_order_acq_rel)) {
		// First frame after a seek: show it even if its PTS is just past
		// the paused clock (it is the frame nearest the seek target).
		candidate = (int64_t)r;
	}
	if (candidate < 0)
		return false;
	m->present_force.store(false, std::memory_order_relaxed);
	video_slot *slot = &m->slots[candidate % RING_SLOTS];
	if (m->out_yuv) {
		// The ring holds planar I420 (Y|U|V), which this BGRA
		// pointer-swap API cannot represent: the caller would read the
		// luma plane as BGRA pixels.  Consumers of a YUV media must use
		// nx_media_present_frame(), which reports the format.
		return false;
	}
	uint8_t *prev = *buffer_inout;
	*buffer_inout = slot->bgra;
	slot->bgra = prev;
	m->presented_frames++;
	m->dropped_frames += (uint64_t)candidate - r;
	m->vread.store((uint64_t)candidate + 1, std::memory_order_release);
	return true;
}

// Present the newest due frame and report its pixel format.
//
// Unlike nx_media_present() this does not take ownership of the slot
// buffer: it just points `out` at the frame the ring is showing. The
// pointers stay valid until the next present call on the same media.
bool nx_media_present_frame(nx_media_t *m, nx_media_frame_t *out,
                             uint8_t **buffer_inout) {
	if (!out)
		return false;
	if (m->seeking.load(std::memory_order_acquire) || m->fatal.load())
		return false;
	double t = clock_now(m);
	uint64_t r = m->vread.load(std::memory_order_relaxed);
	uint64_t w = m->vwrite.load(std::memory_order_acquire);
	int64_t candidate = -1;
	for (uint64_t i = r; i < w; i++) {
		if (m->slots[i % RING_SLOTS].pts <= t + PRESENT_EPSILON)
			candidate = (int64_t)i;
		else
			break;
	}
	if (candidate < 0 && w > r &&
	    m->present_force.exchange(false, std::memory_order_acq_rel)) {
		// First frame after a seek: show it even if its PTS is just past
		// the paused clock (it is the frame nearest the seek target).
		candidate = (int64_t)r;
	}
	if (candidate < 0)
		return false;
	m->present_force.store(false, std::memory_order_relaxed);
	video_slot *slot = &m->slots[candidate % RING_SLOTS];
	if (m->out_yuv) {
		// Planar I420 packed contiguously in the slot: Y (W*H) then U then
		// V (each (W/2)*(H/2)). See try_enqueue_video().  The hardware
		// decoder (NVDEC) produces the same layout, so this branch
		// covers both the software and the hardware path.
		const int cw = (m->width + 1) / 2, ch = (m->height + 1) / 2;
		out->format = NX_MEDIA_PIXFMT_I420;
		out->data = slot->bgra;
		out->uv = out->data + (size_t)m->width * m->height;
		out->v = out->uv + (size_t)cw * ch;
		out->pitch = m->width;
		out->uv_pitch = cw;
		out->width = m->width;
		out->height = m->height;
		out->colorspace = m->yuv_cs;
		if (buffer_inout) {
			// Ownership transfer (pointer swap, zero copy): the caller
			// gets the slot's frame, the slot gets the caller's stale
			// buffer.  Without this the decode thread could overwrite
			// the frame the caller is still reading.
			uint8_t *prev = *buffer_inout;
			*buffer_inout = slot->bgra;
			slot->bgra = prev;
			out->data = *buffer_inout;
			out->uv = out->data + (size_t)m->width * m->height;
			out->v = out->uv + (size_t)cw * ch;
		}
	} else {
		out->format = NX_MEDIA_PIXFMT_BGRA;
		out->data = slot->bgra;
		out->uv = nullptr;
		out->v = nullptr;
		out->pitch = m->width * 4;
		out->uv_pitch = 0;
		out->width = m->width;
		out->height = m->height;
		out->colorspace = 0;
		if (buffer_inout) {
			uint8_t *prev = *buffer_inout;
			*buffer_inout = slot->bgra;
			slot->bgra = prev;
			out->data = *buffer_inout;
		}
	}
	m->presented_frames++;
	m->dropped_frames += (uint64_t)candidate - r;
	m->vread.store((uint64_t)candidate + 1, std::memory_order_release);
	return true;
}

// Which decode path this media actually took: "hw" (NVDEC) or "sw".
//
// The distinction matters: a caller may ask for hardware decode and still
// land on the software decoder (no NVDEC device, unsupported codec or
// resolution, avcodec_open2() refusing the hardware pixel format).  Only
// the frames themselves can settle it, and by the time a frame has been
// decoded the hardware path is known for good.
const char *nx_media_decode_mode(nx_media_t *m) {
#if defined(NX_ENABLE_NVTEGRA)
	if (m->hw_frames.load(std::memory_order_relaxed) > 0)
		return "hw";
#endif
	return "sw";
}

uint64_t nx_media_hw_frames(nx_media_t *m) {
#if defined(NX_ENABLE_NVTEGRA)
	return m->hw_frames.load(std::memory_order_relaxed);
#else
	(void)m;
	return 0;
#endif
}

uint64_t nx_media_presented_frames(nx_media_t *m) {
	return m->presented_frames;
}

uint64_t nx_media_dropped_frames(nx_media_t *m) { return m->dropped_frames; }

double nx_media_current_time(nx_media_t *m) {
	if (m->seeking.load())
		return m->seek_target.load();
	double t = clock_now(m);
	if (m->duration > 0) {
		if (m->looping.load())
			t = fmod(t, m->duration);
		else if (t > m->duration)
			t = m->duration;
	}
	return t < 0 ? 0 : t;
}

uint32_t nx_media_buffered_frames(nx_media_t *m) {
	return (uint32_t)(m->vwrite.load(std::memory_order_acquire) -
	                  m->vread.load(std::memory_order_relaxed));
}

bool nx_media_ended(nx_media_t *m) {
	if (!m->eof.load() || m->looping.load())
		return false;
	if (m->vwrite.load(std::memory_order_acquire) !=
	    m->vread.load(std::memory_order_relaxed))
		return false;
	if (m->audio_node && m->audio_clock_valid.load()) {
		uint64_t written =
		    m->audio_node->stream_write_pos.load(std::memory_order_acquire);
		if (nx_audio_stream_consumed(m->audio_node) < written)
			return false;
	}
	return true;
}

bool nx_media_seeking(nx_media_t *m) { return m->seeking.load(); }

const char *nx_media_error(nx_media_t *m) {
	return m->fatal.load() ? m->error_buf : NULL;
}

// ---------------------------------------------------------------------------
// Whole-file audio decode (decodeAudioData / Audio element)
// ---------------------------------------------------------------------------

namespace {

struct mem_reader {
	const uint8_t *data;
	size_t size;
	size_t pos;
};

int mem_read_cb(void *opaque, uint8_t *buf, int n) {
	mem_reader *r = static_cast<mem_reader *>(opaque);
	size_t rem = r->size - r->pos;
	if (rem == 0)
		return AVERROR_EOF;
	size_t count = rem < (size_t)n ? rem : (size_t)n;
	memcpy(buf, r->data + r->pos, count);
	r->pos += count;
	return (int)count;
}

int64_t mem_seek_cb(void *opaque, int64_t offset, int whence) {
	mem_reader *r = static_cast<mem_reader *>(opaque);
	if (whence & AVSEEK_SIZE)
		return (int64_t)r->size;
	whence &= ~AVSEEK_FORCE;
	int64_t base = whence == SEEK_CUR    ? (int64_t)r->pos
	               : whence == SEEK_END ? (int64_t)r->size
	                                    : 0;
	int64_t pos = base + offset;
	if (pos < 0 || pos > (int64_t)r->size)
		return -1;
	r->pos = (size_t)pos;
	return pos;
}

} // namespace

bool nx_media_decode_audio(const uint8_t *data, size_t size,
                           float *channels[NX_MEDIA_MAX_CHANNELS],
                           int *num_channels, uint32_t *length,
                           uint32_t *sample_rate, uint32_t target_rate,
                           char *errbuf, size_t errbuf_size) {
	mem_reader reader = {data, size, 0};
	AVIOContext *avio = NULL;
	AVFormatContext *fmt = NULL;
	AVCodecContext *ctx = NULL;
	SwrContext *swr = NULL;
	AVPacket *pkt = NULL;
	AVFrame *frame = NULL;
	const AVCodec *codec = NULL;
	int stream = -1;
	int ret = 0;
	int nch = 0;
	bool ok = false;
	// Resampler state, hoisted so the post-loop flush can see it (the
	// filter holds a tail of samples that only come out on a NULL-input
	// convert — dropping it would clip the end of every decoded sound).
	int src_rate = 0;  // the file's rate
	int out_rate = 0;  // what we hand back (target_rate, or src_rate)
	int src_nch = 0;   // channels in the decoded frames
	// Planar accumulation buffers (resized as frames arrive).
	std::vector<std::vector<float>> acc;
	std::vector<float> scratch;

	unsigned char *avio_buf = (unsigned char *)av_malloc(65536);
	if (!avio_buf) {
		snprintf(errbuf, errbuf_size, "out of memory");
		goto done;
	}
	avio = avio_alloc_context(avio_buf, 65536, 0, &reader, mem_read_cb, NULL,
	                          mem_seek_cb);
	if (!avio) {
		av_free(avio_buf);
		snprintf(errbuf, errbuf_size, "out of memory");
		goto done;
	}
	fmt = avformat_alloc_context();
	if (!fmt) {
		snprintf(errbuf, errbuf_size, "out of memory");
		goto done;
	}
	fmt->pb = avio;
	ret = avformat_open_input(&fmt, NULL, NULL, NULL);
	if (ret < 0) {
		av_strerror(ret, errbuf, errbuf_size);
		goto done;
	}
	ret = avformat_find_stream_info(fmt, NULL);
	if (ret < 0) {
		av_strerror(ret, errbuf, errbuf_size);
		goto done;
	}
	stream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
	if (stream < 0) {
		snprintf(errbuf, errbuf_size, "no audio stream found");
		goto done;
	}
	ctx = avcodec_alloc_context3(codec);
	if (!ctx ||
	    avcodec_parameters_to_context(ctx, fmt->streams[stream]->codecpar) <
	        0 ||
	    avcodec_open2(ctx, codec, NULL) < 0) {
		snprintf(errbuf, errbuf_size, "failed to open audio decoder");
		goto done;
	}
	pkt = av_packet_alloc();
	frame = av_frame_alloc();
	if (!pkt || !frame) {
		snprintf(errbuf, errbuf_size, "out of memory");
		goto done;
	}

	while (true) {
		ret = av_read_frame(fmt, pkt);
		bool at_eof = ret == AVERROR_EOF;
		if (ret < 0 && !at_eof) {
			av_strerror(ret, errbuf, errbuf_size);
			goto done;
		}
		if (!at_eof && pkt->stream_index != stream) {
			av_packet_unref(pkt);
			continue;
		}
		ret = avcodec_send_packet(ctx, at_eof ? NULL : pkt);
		if (!at_eof)
			av_packet_unref(pkt);
		if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
			av_strerror(ret, errbuf, errbuf_size);
			goto done;
		}
		while (true) {
			ret = avcodec_receive_frame(ctx, frame);
			if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
				break;
			if (ret < 0) {
				av_strerror(ret, errbuf, errbuf_size);
				goto done;
			}
			if (nch == 0) {
				nch = frame->ch_layout.nb_channels;
				if (nch > NX_MEDIA_MAX_CHANNELS)
					nch = NX_MEDIA_MAX_CHANNELS;
				if (nch <= 0) {
					snprintf(errbuf, errbuf_size, "no audio channels");
					goto done;
				}
				src_rate = frame->sample_rate;
				out_rate = target_rate > 0 ? (int)target_rate : src_rate;
				src_nch = frame->ch_layout.nb_channels;
				*sample_rate = (uint32_t)out_rate;
				acc.resize((size_t)nch);
				// Convert to interleaved f32 at `out_rate`, keeping the
				// input channel layout (we deinterleave below). When
				// out_rate != src_rate this is where the actual sample-rate
				// conversion happens — swr's default polyphase resampler,
				// once per file, instead of per-sample linear interpolation
				// on every playback (see the header comment).
				ret = swr_alloc_set_opts2(
				    &swr, &frame->ch_layout, AV_SAMPLE_FMT_FLT, out_rate,
				    &frame->ch_layout, (AVSampleFormat)frame->format,
				    src_rate, 0, NULL);
				if (ret < 0 || swr_init(swr) < 0) {
					snprintf(errbuf, errbuf_size,
					         "failed to create resampler");
					goto done;
				}
			}
			// Output capacity must account for BOTH the rate change and
			// whatever the filter is still holding, or swr silently drops
			// samples it had no room to write.
			int out_cap = (int)av_rescale_rnd(
			    swr_get_delay(swr, src_rate) + frame->nb_samples, out_rate,
			    src_rate, AV_ROUND_UP);
			if (out_cap < 1)
				out_cap = 1;
			scratch.resize((size_t)out_cap * src_nch);
			uint8_t *out_ptr = (uint8_t *)scratch.data();
			int got = swr_convert(swr, &out_ptr, out_cap,
			                      (const uint8_t **)frame->extended_data,
			                      frame->nb_samples);
			if (got > 0) {
				for (int c = 0; c < nch; c++) {
					std::vector<float> &dst = acc[(size_t)c];
					size_t off = dst.size();
					dst.resize(off + (size_t)got);
					for (int i = 0; i < got; i++)
						dst[off + i] = scratch[(size_t)i * src_nch + c];
				}
			}
			av_frame_unref(frame);
		}
		if (at_eof)
			break;
	}

	// Drain the resampler. A polyphase filter runs behind its input by half
	// its kernel, so the last few ms of every sound live inside swr until a
	// NULL-input convert pushes them out. Without this a resampled one-shot
	// loses its tail — quiet, but it turns a decaying sample into a clipped
	// one. No-op when out_rate == src_rate (swr has nothing buffered).
	if (swr && src_nch > 0) {
		while (true) {
			int out_cap = (int)av_rescale_rnd(swr_get_delay(swr, src_rate),
			                                  out_rate, src_rate,
			                                  AV_ROUND_UP);
			if (out_cap <= 0)
				break;
			scratch.resize((size_t)out_cap * src_nch);
			uint8_t *out_ptr = (uint8_t *)scratch.data();
			int got = swr_convert(swr, &out_ptr, out_cap, NULL, 0);
			if (got <= 0)
				break;
			for (int c = 0; c < nch; c++) {
				std::vector<float> &dst = acc[(size_t)c];
				size_t off = dst.size();
				dst.resize(off + (size_t)got);
				for (int i = 0; i < got; i++)
					dst[off + i] = scratch[(size_t)i * src_nch + c];
			}
		}
	}

	if (nch == 0 || acc[0].empty()) {
		snprintf(errbuf, errbuf_size, "audio file contains no audio data");
		goto done;
	}
	*num_channels = nch;
	*length = (uint32_t)acc[0].size();
	for (int c = 0; c < nch; c++) {
		channels[c] = (float *)malloc((size_t)*length * sizeof(float));
		if (!channels[c]) {
			for (int j = 0; j < c; j++) {
				free(channels[j]);
				channels[j] = nullptr;
			}
			snprintf(errbuf, errbuf_size, "out of memory");
			goto done;
		}
		size_t have = acc[(size_t)c].size();
		size_t want = (size_t)*length;
		memcpy(channels[c], acc[(size_t)c].data(),
		       (have < want ? have : want) * sizeof(float));
	}
	ok = true;

done:
	if (pkt)
		av_packet_free(&pkt);
	if (frame)
		av_frame_free(&frame);
	if (swr)
		swr_free(&swr);
	if (ctx)
		avcodec_free_context(&ctx);
	if (fmt)
		avformat_close_input(&fmt);
	if (avio) {
		av_freep(&avio->buffer);
		avio_context_free(&avio);
	}
	return ok;
}

// ---------------------------------------------------------------------------
// Cut #22b Stage 2: visualizer readers (waveform / spectrum / audio-levels)
// ---------------------------------------------------------------------------

namespace {

// Iterative radix-2 Cooley-Tukey FFT on TAP_LEN complex samples in-place.
// Not general-purpose — hard-coded to the tap window size so the inner
// loops can constant-fold. Called at most a few times per second (per
// spectrum read), so no need for split-radix / SIMD; correctness matters.
void fft_tap(float *re, float *im) {
	// Bit-reverse permutation.
	for (uint32_t i = 0; i < TAP_LEN; i++) {
		uint32_t j = 0;
		uint32_t k = i;
		for (int b = 0; b < TAP_LOG2; b++) {
			j = (j << 1) | (k & 1u);
			k >>= 1;
		}
		if (j > i) {
			std::swap(re[i], re[j]);
			std::swap(im[i], im[j]);
		}
	}
	// Cooley-Tukey butterflies (forward FFT — negative-angle twiddles).
	for (uint32_t size = 2; size <= TAP_LEN; size <<= 1) {
		uint32_t half = size >> 1;
		double angle_step = -2.0 * TAP_PI / (double)size;
		for (uint32_t start = 0; start < TAP_LEN; start += size) {
			for (uint32_t k = 0; k < half; k++) {
				double angle = angle_step * (double)k;
				float wr = (float)cos(angle);
				float wi = (float)sin(angle);
				uint32_t i0 = start + k;
				uint32_t i1 = i0 + half;
				float tr = wr * re[i1] - wi * im[i1];
				float ti = wr * im[i1] + wi * re[i1];
				re[i1] = re[i0] - tr;
				im[i1] = im[i0] - ti;
				re[i0] = re[i0] + tr;
				im[i0] = im[i0] + ti;
			}
		}
	}
}

// Snapshot the most-recent TAP_LEN mono samples in chronological order.
// Returns false if the tap hasn't yet accumulated a full window (e.g.
// fresh decoder). Caller-owned buffer must hold TAP_LEN floats.
bool snapshot_tap(nx_media *m, float *out) {
	std::lock_guard<std::mutex> lock(m->tap_mutex);
	if (m->tap_written < TAP_LEN) return false;
	uint32_t wp = m->tap_write_pos; // one past the newest sample
	for (uint32_t i = 0; i < TAP_LEN; i++) {
		// Oldest → newest: sample at logical index i is at ring index
		// (wp + i) mod TAP_LEN — because wp is the next write, i.e.
		// exactly TAP_LEN samples old going forward.
		uint32_t idx = (wp + i) & TAP_MASK;
		out[i] = m->tap_ring[idx];
	}
	return true;
}

} // namespace

bool nx_media_read_waveform(nx_media_t *m, float *out, uint32_t out_len) {
	if (!m || !out || out_len == 0 || out_len > TAP_LEN) return false;
	std::lock_guard<std::mutex> lock(m->tap_mutex);
	if (m->tap_written < out_len) return false;
	uint32_t wp = m->tap_write_pos;
	// The newest sample is at (wp - 1) mod TAP_LEN. Copy out_len samples
	// ending at the newest, in chronological order (oldest first).
	for (uint32_t i = 0; i < out_len; i++) {
		uint32_t idx = (wp + TAP_LEN - out_len + i) & TAP_MASK;
		out[i] = m->tap_ring[idx];
	}
	return true;
}

bool nx_media_read_spectrum(nx_media_t *m, float *out, uint32_t out_len) {
	if (!m || !out || out_len == 0) return false;
	float samples[TAP_LEN];
	if (!snapshot_tap(m, samples)) return false;
	// Hann window (reduces spectral leakage from the rectangular window
	// otherwise implied by a hard buffer boundary).
	for (uint32_t i = 0; i < TAP_LEN; i++) {
		float w = 0.5f * (1.0f - (float)cos(2.0 * TAP_PI * (double)i /
		                                    (double)(TAP_LEN - 1)));
		samples[i] *= w;
	}
	float re[TAP_LEN];
	float im[TAP_LEN] = {};
	memcpy(re, samples, sizeof(re));
	fft_tap(re, im);
	// Useful bins are 0..N/2. Convert to magnitude, normalize by (N/2)
	// so a unit-amplitude tone reads ~1.0 at its bin. Matches the
	// QuickJS-era video.c formulation.
	constexpr uint32_t USABLE = TAP_LEN / 2;
	float mag[USABLE];
	const float norm = 2.0f / (float)TAP_LEN;
	for (uint32_t i = 0; i < USABLE; i++) {
		mag[i] = sqrtf(re[i] * re[i] + im[i] * im[i]) * norm;
	}
	// Linear-scale bin-average USABLE bins into out_len output bins,
	// then flatten the natural 1/f rolloff of music (bass content
	// dominates the raw magnitude spectrum, so without compensation only
	// the first few output bins ever visibly react). The sqrt(1 + bin/K)
	// weighting boosts the tail without warping the bass response —
	// tuned so mid-range bins (~1 kHz) roughly double vs the raw
	// magnitude and treble (~10 kHz) reads ~3× stronger.
	for (uint32_t i = 0; i < out_len; i++) {
		uint32_t lo = (uint32_t)((uint64_t)i * USABLE / out_len);
		uint32_t hi = (uint32_t)((uint64_t)(i + 1) * USABLE / out_len);
		if (hi <= lo) hi = lo + 1;
		if (hi > USABLE) hi = USABLE;
		float sum = 0;
		for (uint32_t k = lo; k < hi; k++) sum += mag[k];
		float avg = sum / (float)(hi - lo);
		// Weight by ~sqrt(centre_bin) — cheap 1/f flatten.
		float centre = 0.5f * (float)(lo + hi);
		float weight = sqrtf(1.0f + centre / 6.0f);
		out[i] = avg * weight;
	}
	return true;
}

uint32_t nx_media_read_audio_levels(nx_media_t *m, float *out,
                                    uint32_t out_max) {
	if (!m || !out || out_max == 0) return 0;
	// Three bands (bass / mid / high). Reuse the spectrum path so the
	// mapping matches the visualizer's frequency reading.
	constexpr uint32_t BANDS = 3;
	const uint32_t n = out_max < BANDS ? out_max : BANDS;
	// Compute a per-band RMS of the tap window in the frequency domain
	// (Parseval). Cheaper: just RMS the raw tap samples split into
	// low-pass / mid / high-pass via a coarse Butterworth-shaped IIR —
	// but for a visualizer the FFT-band approach is more legible.
	float samples[TAP_LEN];
	if (!snapshot_tap(m, samples)) return 0;
	for (uint32_t i = 0; i < TAP_LEN; i++) {
		float w = 0.5f * (1.0f - (float)cos(2.0 * TAP_PI * (double)i /
		                                    (double)(TAP_LEN - 1)));
		samples[i] *= w;
	}
	float re[TAP_LEN];
	float im[TAP_LEN] = {};
	memcpy(re, samples, sizeof(re));
	fft_tap(re, im);
	constexpr uint32_t USABLE = TAP_LEN / 2;
	const float norm = 2.0f / (float)TAP_LEN;
	// Band edges: bass 0..~250 Hz, mid 250 Hz..~4 kHz, high 4 kHz..Nyquist.
	// At 48 kHz sample rate and TAP_LEN=2048, bin size ≈ 23.44 Hz — so
	// 10 bins ≈ 235 Hz, 170 bins ≈ 4 kHz.
	uint32_t edges[BANDS + 1] = {0, 10, 170, USABLE};
	for (uint32_t b = 0; b < n; b++) {
		uint32_t lo = edges[b];
		uint32_t hi = edges[b + 1];
		float sum = 0;
		for (uint32_t k = lo; k < hi; k++) {
			float mag = sqrtf(re[k] * re[k] + im[k] * im[k]) * norm;
			sum += mag * mag;
		}
		float rms = (hi > lo) ? sqrtf(sum / (float)(hi - lo)) : 0.0f;
		out[b] = rms;
	}
	return n;
}

void nx_media_destroy(nx_media_t *m) {
	m->quit.store(true);
	m->ctl_cv.notify_all();
	if (m->thread.joinable())
		m->thread.join();
	if (m->audio_node)
		nx_audio_stream_set_playing(m->audio_node, false);
	if (m->sws)
		sws_freeContext(m->sws);
	if (m->swr)
		swr_free(&m->swr);
	if (m->vctx)
		avcodec_free_context(&m->vctx);
	if (m->actx)
		avcodec_free_context(&m->actx);
	if (m->fmt)
		avformat_close_input(&m->fmt);
	if (m->avio) {
		av_freep(&m->avio->buffer);
		avio_context_free(&m->avio);
	}
	for (int i = 0; i < RING_SLOTS; i++) {
		free(m->slots[i].bgra);
	}
#if defined(NX_ENABLE_NVTEGRA)
	if (m->hw_device_ref)
		av_buffer_unref(&m->hw_device_ref);
#endif
	if (m->file)
		fclose(m->file);
	delete m;
}

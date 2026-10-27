#pragma once
// Portable media (video + audio) decode pipeline over ffmpeg.
//
// Compiled into BOTH the device runtime (switch-ffmpeg 7.x) and the host
// nxjs-test binary (distro ffmpeg 5.x+) — no V8, no libnx. Each open media
// owns a dedicated decode thread which demuxes the container, decodes the
// video stream into a small ring of presentation-ready BGRA frames (PTS
// stamped), and decodes/resamples the audio stream into an audio-graph
// stream-source node (see NX_AUDIO_NODE_STREAM_SOURCE in audio-graph.h).
//
// Threading contract:
//   - All functions below are called from the main (JS loop) thread, except
//     nx_media_open which may run on a libuv worker.
//   - The decode thread is internal; commands (play/pause/seek/loop/quit)
//     are delivered via atomics + a condition variable.
//   - nx_media_present() swaps the newest due frame into the caller's BGRA
//     buffer (pointer swap, zero copy) based on the media clock. The clock
//     is slaved to the audio stream node's consumed-frame counter when an
//     audio track is playing, and to the monotonic wall clock otherwise.
#include <atomic> // nx_media_open's abort_flag
#include <memory>
#include <stddef.h>
#include <stdint.h>

struct nx_audio_node;

typedef struct nx_media nx_media_t;

// Channel cap for whole-file audio decoding (Web Audio allows 32).
#define NX_MEDIA_MAX_CHANNELS 32

// Pixel format of a presented video frame.
//
// The hardware decoder (Tegra X1 NVDEC) and the software decoder feed the
// ring the SAME layout: planar I420 (Y|U|V contiguous) when the media was
// opened with want_yuv, premultiplied BGRA otherwise.  The NVDEC surface is
// NV12 (interleaved chroma) internally; the decoder de-interleaves it on
// the decode thread, so the consumer never sees NV12.
//
// NX_MEDIA_PIXFMT_NV12 is reserved for a future interleaved-chroma path.
typedef enum {
	NX_MEDIA_PIXFMT_BGRA = 0, // premultiplied BGRA, 4 bytes per pixel
	NX_MEDIA_PIXFMT_I420 = 1, // planar I420 (Y|U|V contiguous), 1.5 bytes/px
	NX_MEDIA_PIXFMT_NV12 = 2, // NV12 4:2:0, luma + interleaved chroma planes
} nx_media_pixfmt_t;

// A presented video frame.
//
// `data` points at the BGRA pixels (BGRA) or the luma plane (I420);
// `uv` is the U plane for I420; `v` is the V plane for I420.  Both are
// NULL for BGRA frames.
//
// `colorspace` carries the same neutral tag nx_media_yuv_colorspace()
// reports: 0 = Rec709 limited, 1 = Rec601 limited, 2 = JPEG/full range,
// 3 = Rec709 full.  It is 0 for BGRA frames.
//
// The pointers stay valid until the next present call on the same media,
// unless the caller passed a buffer to nx_media_present_frame() -- then the
// frame is owned by the caller and stays valid until it is presented again.
typedef struct nx_media_frame {
	nx_media_pixfmt_t format;
	const uint8_t *data; // BGRA pixels, or the luma plane
	const uint8_t *uv;   // I420: U plane.  NV12: interleaved U,V chroma plane.
	const uint8_t *v;    // I420 only: V plane (NULL for BGRA / NV12)
	int pitch;           // BGRA: width*4.  I420/NV12: bytes per luma row.
	int uv_pitch;        // I420/NV12: bytes per chroma row.
	int width;
	int height;
	int colorspace;
} nx_media_frame_t;

// Decode an entire audio resource (any ffmpeg-supported container/codec)
// from memory into planar f32 channel buffers. Backs `decodeAudioData()` and
// the `Audio` element.
//
// `target_rate` is the sample rate the decoded buffer should come back at —
// pass the destination AudioContext's rate so the conversion happens ONCE
// here, through libswresample's polyphase filter, instead of every frame in
// the render loop's linear interpolator. 0 keeps the file's native rate.
//
// This matters more than it looks. The Switch's audio graph runs at 48 kHz
// and most game assets ship at 44.1 kHz, so without this the buffer-source
// node reads the buffer at r = 0.91875 with linear interpolation between
// neighbouring samples — which is fine down low but costs -1.5 dB at 10 kHz,
// -3.4 dB at 15 kHz, and folds back roughly -25 dB of imaging junk at 10 kHz
// rising to -15 dB at 15 kHz. Smooth low-passed material (a music bed) hides
// it; short bright percussive one-shots (coin blips, explosions, UI clicks)
// audibly grit and dull. Browsers resample in decodeAudioData for exactly
// this reason, and `r` then lands on 1.0 so the interpolator never engages.
//
// On success fills `channels[0..num_channels)` with malloc'd buffers (caller
// frees), `length` (frames) and `sample_rate` (the rate actually produced),
// and returns true. Blocking — call on a worker thread. On failure fills
// `errbuf` and returns false.
bool nx_media_decode_audio(const uint8_t *data, size_t size,
                           float *channels[NX_MEDIA_MAX_CHANNELS],
                           int *num_channels, uint32_t *length,
                           uint32_t *sample_rate, uint32_t target_rate,
                           char *errbuf, size_t errbuf_size);

// Open a media resource and probe its streams. Exactly one of `path` or
// `mem` must be provided; for `mem`, `keepalive` must own the buffer (e.g. a
// shared_ptr<v8::BackingStore>) — the media holds it until
// nx_media_destroy(), since the decode thread streams from it for the
// media's whole lifetime. Blocking — call off the main thread. Returns NULL
// and fills `errbuf` on failure.
// `want_yuv` (2026-09-06): when true the video ring stores planar I420
// (Y + U + V, contiguous, 1.5 bytes/px) instead of BGRA (4 bytes/px). This
// lets the caller upload the frame to the GPU as YUV planes and do YUV→RGB
// in the shader (Skia's YUVA image path) — ~2.6× less per-frame texture
// upload than BGRA, which is the dominant per-frame cost on the Switch's
// Mesa-nouveau GL (see the Jellyfin video-perf work). Default false keeps the
// documented BGRA contract for the Three.js webgl_materials_video demo and the
// legacy `<video>` element (video.cc).
// Per-open HTTP request shaping for network sources (2026-09-17). Both
// fields are borrowed for the duration of the nx_media_open call only —
// libavformat copies them into its own AVDictionary — so the caller may
// point them at temporaries.
//
// Why this exists: a large slice of public HLS (the iptv-org catalogue in
// particular) rejects libavformat's default `Lavf/<version>` User-Agent with
// 403/connection-reset, and a smaller slice gates on Referer. Measured over
// 70 such streams: 27/70 played with the stock UA, 44/70 with a browser UA.
// Referer additionally unlocks streams that a real browser CANNOT reach at
// all, because `Referer` is a forbidden header name in the Fetch spec.
//
// Only `user_agent` and `referer` are exposed — NOT a raw header blob. Both
// map to dedicated libavformat AVOptions, so we never concatenate into the
// request head ourselves, and both are stripped of CR/LF before use (their
// values routinely originate in remotely-fetched playlists).
struct nx_media_net_t {
	// Override the User-Agent. NULL/empty = the engine's browser-like
	// default (NOT libavformat's `Lavf/...`, which is what gets blocked).
	const char *user_agent;
	// Referer to send. NULL/empty = send none.
	const char *referer;
};

// `av_err` (2026-09-17, optional): on failure receives the raw AVERROR that
// caused the open to fail, so callers can tell a hard rejection (403/404 —
// retrying is pointless) from a transient one (timeout, connection reset —
// worth another attempt) without pattern-matching the human-readable string
// in `errbuf`. Untouched on success. See nx_media_fail_kind().
// `abort_flag` (2026-09-17, optional): caller-owned latch polled by
// libavformat during every blocking IO operation. Set it to make an
// in-flight open or read bail out promptly (AVERROR_EXIT -> fail kind
// "aborted") instead of blocking to the 30s rw_timeout. REQUIRED for any
// caller that can tear a decoder down while its open is still in flight,
// because teardown joins the open thread: without this, closing a decoder
// aimed at a dead host freezes the calling thread for the whole timeout.
// Must outlive the returned media.
// `want_hw_decode` (Tegra X1 NVDEC): when true the video decoder is asked to
// use the Switch's NVDEC hardware decoder.  The ring then holds the same
// planar I420 (Y|U|V) frames as the software path when `want_yuv` is set, or
// BGRA frames otherwise -- the hardware path never changes the contract the
// caller sees.  It is a no-op unless the build defines NX_ENABLE_NVTEGRA
// (the Switch build does, the host build does not).  The decoder falls back
// to software on its own -- if the device cannot be created, if libavcodec
// refuses the hwaccel for this codec, or if avcodec_open2() fails with the
// hardware pixel format.  Use nx_media_present_frame() to read the frames
// back: it reports the actual format; nx_media_present() cannot represent
// the I420 frames and refuses to run on a media opened with want_yuv.
nx_media_t *nx_media_open(const char *path, const uint8_t *mem,
                          size_t mem_size, std::shared_ptr<void> keepalive,
                          char *errbuf, size_t errbuf_size,
                          bool want_yuv = false,
                          const nx_media_net_t *net = nullptr,
                          int *av_err = nullptr,
                          std::atomic<bool> *abort_flag = nullptr,
                          bool want_hw_decode = false);

// Map an AVERROR from nx_media_open's `av_err` to a short stable token for
// JS: "forbidden", "not-found", "unauthorized", "server-error", "timeout",
// "network", "format", "not-found-local", or "unknown". Stable across
// releases — apps branch on these, so treat them as API.
const char *nx_media_fail_kind(int av_err);

// Metadata (valid after a successful open).
int nx_media_width(nx_media_t *m);
int nx_media_height(nx_media_t *m);
double nx_media_duration(nx_media_t *m); // seconds (0 if unknown)
// True when the container carried a duration, i.e. nx_media_duration() is a
// real length. FALSE for a source with no container duration -- a streamed
// H.264 feed has no moov and therefore no total length -- and the JS layer
// must report NaN there instead of 0 (HTML: "If the media resource has no
// known duration, the duration attribute must return NaN").
bool nx_media_duration_known(nx_media_t *m);
double nx_media_content_fps(nx_media_t *m); // stream frame rate (0 if unknown)
bool nx_media_has_audio(nx_media_t *m);
bool nx_media_has_video(nx_media_t *m);

// True if this media's video ring is planar I420 (opened with want_yuv). The
// present buffer is then W*H + 2*((W+1)/2)*((H+1)/2) bytes, not W*H*4.
bool nx_media_is_yuv(nx_media_t *m);
// Neutral color-space tag for the I420 frames, derived from the stream:
//   0 = Rec709 limited, 1 = Rec601 limited, 2 = JPEG/full range, 3 = Rec709 full.
// The GPU draw path maps this to the matching SkYUVColorSpace. Meaningful only
// when nx_media_is_yuv() is true.
int nx_media_yuv_colorspace(nx_media_t *m);
// Size in bytes of one I420 frame for the given luma dimensions.
static inline size_t nx_media_i420_size(int w, int h) {
	size_t cw = (size_t)((w + 1) / 2), ch = (size_t)((h + 1) / 2);
	return (size_t)w * (size_t)h + 2 * cw * ch;
}

// Attach the audio output. `node` must be an NX_AUDIO_NODE_STREAM_SOURCE
// whose graph runs at `sample_rate`; the decoder resamples the audio track
// to interleaved stereo f32 at that rate. The caller owns the node and must
// keep it alive until nx_media_destroy(). Call before nx_media_play().
void nx_media_set_audio_node(nx_media_t *m, nx_audio_node *node,
                             double sample_rate);

// Transport controls (non-blocking; the decode thread reacts).
void nx_media_play(nx_media_t *m);
void nx_media_pause(nx_media_t *m);
void nx_media_seek(nx_media_t *m, double seconds);
void nx_media_set_loop(nx_media_t *m, bool loop);

// Presentation: if a video frame is due at the current media clock, swap it
// into `*buffer_inout` (a caller-owned width*height*4 BGRA buffer; the
// pointer is exchanged with the ring slot's buffer). Returns true if a new
// frame was presented. Call from the main thread (e.g. once per host frame).
//
// NOT usable on a media opened with `want_hw_decode` that ended up on the
// hardware path: the ring then holds NV12 planes, which this BGRA
// pointer-swap API cannot represent. Use nx_media_present_frame() there.
bool nx_media_present(nx_media_t *m, uint8_t **buffer_inout);

// Present the newest due frame and report its pixel format.
//
// This is the format-aware counterpart of nx_media_present(): it points `out`
// at the frame the ring is currently showing. The pointers stay valid until
// the next present call on the same media. Returns true when a new frame was
// presented.
//
// `buffer_inout` (optional, 2026-10-21): when non-NULL and the presented
// frame uses a single buffer (BGRA or planar I420), the ring slot's buffer
// and the caller's buffer are *exchanged* (pointer swap, zero copy) — the
// same ownership transfer nx_media_present() performs. After the call
// `*buffer_inout` points at the frame that was just presented, and the
// decode thread will not touch it until the slot is recycled. Pass NULL to
// keep the observe-only behaviour.
//
// The two APIs are mutually exclusive on a given media: nx_media_present()
// takes ownership of the slot buffer (pointer swap), nx_media_present_frame()
// only observes it unless `buffer_inout` is given.
bool nx_media_present_frame(nx_media_t *m, nx_media_frame_t *out,
                             uint8_t **buffer_inout = nullptr);

// Which decode path this media actually took.
//
// "hw" — the frames come from the Tegra NVDEC hardware decoder (the ring
//        holds planar I420, 1.5 B/px, and the host does no colour maths).
// "sw" — the frames come from the software decoder (the ring holds BGRA, or
//        planar I420 when the media was opened with want_yuv).
//
// The two differ: "hw" is only reported when the hardware decoder really
// produced the frames, so a caller that asked for hardware decode but got
// the software fallback sees "sw" here. See nx_media_hw_frames() for the
// frame count.
const char *nx_media_decode_mode(nx_media_t *m);

// Number of frames the hardware decoder produced so far (0 when the media
// never took the hardware path). Written by the decode thread, read by the
// main thread.
uint64_t nx_media_hw_frames(nx_media_t *m);

// Current playback position in media seconds (wraps when looping).
double nx_media_current_time(nx_media_t *m);

// Number of decoded-and-waiting video frames (readiness heuristic).
uint32_t nx_media_buffered_frames(nx_media_t *m);

// Presentation quality counters (getVideoPlaybackQuality): frames actually
// presented, and frames skipped because a newer frame was already due.
uint64_t nx_media_presented_frames(nx_media_t *m);
uint64_t nx_media_dropped_frames(nx_media_t *m);

// True once playback reached the end of the stream (never true while
// looping) and all buffered frames/audio have been presented/consumed.
bool nx_media_ended(nx_media_t *m);

// True while a seek is in flight (target not yet decoded).
bool nx_media_seeking(nx_media_t *m);

// Sticky fatal decode error message, or NULL.
const char *nx_media_error(nx_media_t *m);

// Cut #22b Stage 2 (2026-07-02): audio-visualizer surface.
//
// The decode thread taps its resampled audio (downmixed to mono) into a
// small rolling ring; these three accessors read from the ring from the
// main thread. They target Switch.VideoDecoder's `getWaveform` /
// `getFrequencyData` / `getAudioLevels` methods, which brewser-runtime
// re-exposes on `<audio>` / `<video>` DOM elements (spectraplay's
// visualizer reads from there).
//
// All three return `false` (or 0 for `read_audio_levels`) when the tap
// hasn't yet accumulated enough samples (fresh decoder / no audio track).

// Fill `out` with the last `out_len` mono samples in [-1, 1]. `out_len`
// should be <= the internal tap window (2048). Returns true iff data was
// filled.
bool nx_media_read_waveform(nx_media_t *m, float *out, uint32_t out_len);

// Compute FFT magnitude over the tap window (Hann-windowed), bin-average
// into `out_len` bins from 0..~Nyquist, normalize to ~[0, 1]. Returns
// true iff data was filled.
bool nx_media_read_spectrum(nx_media_t *m, float *out, uint32_t out_len);

// Fill up to `out_max` per-band RMS values in ~[0, 1] (bass, mid, high).
// Returns the number of bands written (0..3). Backs `getAudioLevels()`.
uint32_t nx_media_read_audio_levels(nx_media_t *m, float *out,
                                    uint32_t out_max);

// Stop the decode thread (joins it) and free everything. The audio node is
// NOT freed (caller-owned).
void nx_media_destroy(nx_media_t *m);

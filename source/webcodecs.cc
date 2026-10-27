// W3C WebCodecs video trio 鈥?native half.
//
// The engine's existing video decoding is `Switch.VideoDecoder`
// (source/video-decoder.cc): the QuickJS-era "hand it a URL and it plays"
// shape. That is NOT the W3C shape, and the W3C shape is what a page that
// drives its own demuxer (e.g. a WebRTC receiver) needs:
//
//   new VideoDecoder({ output, error })
//   decoder.configure({ codec: 'hvc1.1.6.L93.B0', ... })
//   decoder.decode(new EncodedVideoChunk({ type: 'key', timestamp, data }))
//   // -> output(frame)  ->  ctx.drawImage(frame, ...)
//
// Why a decoder of our own instead of nx_media_open(): the nx_media_*
// pipeline is an AVFORMAT pipeline. It needs a CONTAINER 鈥?it demuxes, probes
// the streams and slaves a presentation clock to the container's timestamps.
// WebCodecs hands us bare access units: no container, no clock, timestamps
// come from the page. So this module implements the raw-AVCodec half:
//
//   * configure()  -> avcodec_find_decoder_by_name + avcodec_open2
//                     (+ the Tegra NVDEC hardware device when available)
//   * decode()     -> normalise the chunk to Annex-B, avcodec_send_packet
//   * output       -> the decoded frame, converted to BGRA and wrapped in an
//                     nx_image_t so canvas 2D drawImage() accepts it
//
// The NVDEC part mirrors source/media-decoder.cc exactly (same hwdevice
// setup, same NV12 surface handling, same swscale output format), so both
// pipelines behave the same on the device.
//
// Threading: the main (JS) thread owns the AVCodecContext; a dedicated
// decode thread does the send/receive so a 1080p key frame never stalls the
// page. Chunks go in through a mutex-guarded queue, decoded frames come out
// through another one; the main thread drains the output queue with
// webcodecsVideoDecoderPoll().

#include "webcodecs.h"

#include "error.h"
#include "image.h"
#include "types.h"
#include "util.h"
#include "wrap.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#if defined(NX_ENABLE_NVTEGRA)
// Tegra X1 NVDEC hardware decode. Only the Switch build defines
// NX_ENABLE_NVTEGRA (the portlibs ffmpeg is the nvtegra-patched build); the
// host nxjs-test binary links a stock ffmpeg without it.
extern "C" {
#include <libavutil/hwcontext_nvtegra.h>
#include <libavutil/nvtegra.h>
}
#endif

#include <atomic>
#include <chrono>
#include <ctype.h>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <thread>
#include <vector>

using namespace v8;

namespace {

// Type discriminators. The FIRST member of every wrapped struct carries one,
// because all wrapped objects share a single ObjectTemplate (see wrap.h) 鈥?// nx_get_image() (canvas.cc) therefore has to tell an nx_image_t wrapper from
// any other wrapped object by looking at its first word.
//
//   * a VideoFrame's first member IS an nx_image_t (magic NX_IMAGE_MAGIC) so
//     that canvas 2D drawImage() accepts it 鈥?the same trick nx_video_t uses
//     in source/video.cc;
//   * a VideoDecoder's first member is NX_WC_DECODER_MAGIC, so drawImage()
//     rejects it instead of aliasing the struct.
#define NX_WC_DECODER_MAGIC 0x57434431u // 'WCD1'
#define NX_WC_FRAME_MAGIC 0x57434631u   // 'WCF1'

enum nx_wc_state {
	NX_WC_UNCONFIGURED = 0,
	NX_WC_CONFIGURED = 1,
	NX_WC_CLOSED = 2,
};

// Timestamps are microseconds, like the W3C DOMHighResTimeStamp.
constexpr int64_t NX_WC_TIMEBASE_DEN = 1000000;

// How many frames the decoder may fail on in a row before the decoder is
// declared dead. A single undecodable frame is normal in a live stream.
constexpr uint32_t NX_WC_MAX_CONSECUTIVE_ERRORS = 30;

// An access unit waiting to be decoded (already normalised to Annex-B).
struct nx_wc_chunk_t {
	std::vector<uint8_t> data;
	int64_t timestamp = 0;
	int64_t duration = 0;
	bool key = false;
};

// A decoded frame waiting to be handed to the page.
struct nx_wc_pixels_t {
	// `width*height*4` premultiplied BGRA when `yuv` is false; otherwise the
	// tight planar I420 layout Y|U|V (1.5 B/px) — see convert_frame().
	std::vector<uint8_t> data;
	bool yuv = false;      // I420 (true) vs BGRA (false)
	int yuv_cs = 0;        // 0=Rec709 ltd, 1=Rec601 ltd, 2=JPEG/full, 3=Rec709 full
	int width = 0;
	int height = 0;
	int64_t timestamp = 0;
	int64_t duration = 0;
};

struct nx_wc_decoder_t {
	// MUST be first: the type discriminator (see NX_WC_DECODER_MAGIC).
	uint32_t magic = NX_WC_DECODER_MAGIC;
	int state = NX_WC_UNCONFIGURED;

	std::string codec;
	int coded_width = 0;
	int coded_height = 0;
	bool hardware = false;
	// ★ NEW (2026-10-26): planar I420 output instead of BGRA. Opt-in via the
	//   extra `yuv` argument of webcodecsVideoDecoderConfigure(); default
	//   false ⇒ byte-for-byte the old behaviour.
	bool want_yuv = false;
	// ★ NEW: what the decoder actually produced, for the diagnostics snapshot
	//   (webcodecsVideoDecoderState). Written on the decode thread.
	std::atomic<int> src_pix_fmt{-1};      // AVPixelFormat of the decoded frame
	std::atomic<int> last_yuv_cs{0};       // last color-space tag (0=709 ltd)
	// ★ 0058: 成功用 av_hwframe_transfer_data 反分块的次数。
	std::atomic<uint64_t> sw_transfer_ok{0};
	std::atomic<uint32_t> last_sws_us{0};  // last convert cost, microseconds
	std::atomic<uint32_t> hw_frames{0};    // frames that came from NVDEC
	std::atomic<uint32_t> sw_frames{0};    // frames that came from the SW decoder

	// ---- ffmpeg (owned by the main thread; used by the decode thread) ----
	AVCodecContext *ctx = nullptr;
	AVBufferRef *hw_device_ref = nullptr;
	SwsContext *sws = nullptr;
	AVFrame *frame = nullptr;
	AVPacket *pkt = nullptr;
	// Parameter sets (VPS/SPS/PPS) in Annex-B form, extracted from the
	// config's `description`. They are used twice: as the decoder's
	// extradata AND prepended to the first chunk, because a hardware decoder
	// needs the parameter sets in the bitstream it is fed.
	std::vector<uint8_t> param_sets;
	bool params_injected = false;

	// ---- queues (decode thread <-> main thread) ----
	std::mutex mutex;
	std::condition_variable cv;
	std::deque<nx_wc_chunk_t> in_queue;
	std::deque<nx_wc_pixels_t> out_queue;
	std::thread thread;
	std::atomic<bool> quit{false};

	// flush bookkeeping: flush() resolves once every chunk submitted before
	// the call has been decoded.
	bool flush_pending = false;
	bool flush_done = true;
	uint64_t flush_target = 0;

	// The size of the last decoded frame (0 until the first frame).
	// For a raw H.264 / H.265 feed this is the size ffmpeg parsed out of
	// the parameter sets (the SPS) carried by the first key frame -- a
	// streamed feed has no container that could carry the size.
	int frame_width = 0;
	int frame_height = 0;

	// counters
	uint64_t frames_submitted = 0;
	uint64_t frames_processed = 0;
	uint64_t frames_output = 0;
	uint32_t consecutive_errors = 0;

	// sticky error (set on the decode thread, read on the main thread)
	std::atomic<bool> has_error{false};
	char error_buf[256] = {};
};

// A decoded frame handed to the page. The FIRST member is an nx_image_t so
// that canvas 2D drawImage() accepts it (same trick as nx_video_t).
struct nx_wc_frame_t {
	nx_image_t image; // MUST be first
	uint32_t wc_magic = NX_WC_FRAME_MAGIC;
	int64_t timestamp = 0;
	int64_t duration = 0;
	uint32_t coded_width = 0;
	uint32_t coded_height = 0;
	uint32_t display_width = 0;
	uint32_t display_height = 0;
	bool yuv = false;   // ★ 2026-10-26: pixels are planar I420, not BGRA
	bool closed = false;
};

const char *state_name(int state) {
	switch (state) {
	case NX_WC_CONFIGURED:
		return "configured";
	case NX_WC_CLOSED:
		return "closed";
	default:
		return "unconfigured";
	}
}

// ---------------------------------------------------------------------------
// Bitstream helpers
// ---------------------------------------------------------------------------

// Append one NAL unit with a 4-byte Annex-B start code.
void annexb_append(std::vector<uint8_t> &out, const uint8_t *data, size_t len) {
	static const uint8_t start[4] = {0, 0, 0, 1};
	out.insert(out.end(), start, start + 4);
	out.insert(out.end(), data, data + len);
}

// True when the buffer contains an Annex-B start code anywhere.
bool has_start_code(const uint8_t *d, size_t n) {
	for (size_t i = 0; i + 2 < n; i++) {
		if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1)
			return true;
	}
	return false;
}

// True when the buffer STARTS with an Annex-B start code.
bool starts_with_start_code(const uint8_t *d, size_t n) {
	if (n >= 4 && d[0] == 0 && d[1] == 0 && d[2] == 0 && d[3] == 1)
		return true;
	if (n >= 3 && d[0] == 0 && d[1] == 0 && d[2] == 1)
		return true;
	return false;
}

// Re-frame a length-prefixed (AVCC) buffer as Annex-B NAL units. `len_size`
// is the width of the length prefix in bytes (1, 2 or 4).
bool avcc_to_annexb(const uint8_t *d, size_t n, int len_size,
                     std::vector<uint8_t> &out) {
	size_t off = 0;
	while (off + (size_t)len_size <= n) {
		uint32_t len = 0;
		for (int i = 0; i < len_size; i++)
			len = (len << 8) | d[off + (size_t)i];
		off += (size_t)len_size;
		if (len == 0 || len > n - off)
			return false;
		annexb_append(out, d + off, len);
		off += len;
	}
	return off == n && !out.empty();
}

// Normalise a chunk payload to Annex-B. Returns true when `out` holds the
// converted payload; false when the payload is already Annex-B (or is not
// recognisable) and the caller should pass the bytes through unchanged.
//
// WebRTC delivers H.265 either as Annex-B (with start codes) or as AVCC
// (length-prefixed); which one depends on the browser and the codec, so the
// shape is sniffed per chunk instead of being configured.
bool to_annexb(const uint8_t *d, size_t n, std::vector<uint8_t> &out) {
	if (n == 0)
		return false;
	if (starts_with_start_code(d, n))
		return false; // already Annex-B
	// Try the length-prefixed layouts. The AVCC parse is strict (the whole
	// buffer must be a sequence of valid NALs), so a false positive is
	// unlikely.
	for (int ls = 4; ls >= 1; ls--) {
		std::vector<uint8_t> tmp;
		if (avcc_to_annexb(d, n, ls, tmp)) {
			out.swap(tmp);
			return true;
		}
	}
	return false;
}

// Parse an AVCDecoderConfigurationRecord 鈥?the `description` of an avc1.* /
// avc3.* config 鈥?into Annex-B parameter sets (SPS + PPS).
bool parse_avcc(const uint8_t *d, size_t n, std::vector<uint8_t> &out) {
	if (n < 7 || d[0] != 1)
		return false;
	size_t off = 5;
	unsigned num_sps = d[off] & 0x1f;
	off++;
	for (unsigned i = 0; i < num_sps; i++) {
		if (off + 2 > n)
			return false;
		size_t len = ((size_t)d[off] << 8) | d[off + 1];
		off += 2;
		if (len == 0 || off + len > n)
			return false;
		annexb_append(out, d + off, len);
		off += len;
	}
	if (off >= n)
		return false;
	unsigned num_pps = d[off];
	off++;
	for (unsigned i = 0; i < num_pps; i++) {
		if (off + 2 > n)
			return false;
		size_t len = ((size_t)d[off] << 8) | d[off + 1];
		off += 2;
		if (len == 0 || off + len > n)
			return false;
		annexb_append(out, d + off, len);
		off += len;
	}
	return !out.empty();
}

// Parse an HEVCDecoderConfigurationRecord 鈥?the `description` of an hvc1.* /
// hev1.* config 鈥?into Annex-B parameter sets (VPS + SPS + PPS).
bool parse_hvcc(const uint8_t *d, size_t n, std::vector<uint8_t> &out) {
	if (n < 23 || d[0] != 1)
		return false;
	size_t off = 22;
	unsigned num_arrays = d[off];
	off++;
	for (unsigned i = 0; i < num_arrays; i++) {
		if (off + 3 > n)
			return false;
		unsigned nal_type = d[off] & 0x3f;
		off++;
		unsigned num_nalus = ((unsigned)d[off] << 8) | d[off + 1];
		off += 2;
		for (unsigned j = 0; j < num_nalus; j++) {
			if (off + 2 > n)
				return false;
			size_t len = ((size_t)d[off] << 8) | d[off + 1];
			off += 2;
			if (len == 0 || off + len > n)
				return false;
			// 32 = VPS, 33 = SPS, 34 = PPS 鈥?the ones a decoder needs.
			if (nal_type == 32 || nal_type == 33 || nal_type == 34)
				annexb_append(out, d + off, len);
			off += len;
		}
	}
	return !out.empty();
}

// Extract the parameter sets from a WebCodecs `description`.
//
// Three shapes are accepted:
//   * raw Annex-B parameter sets (what a WebRTC SDP's sprop-parameter-sets
//     looks like once Annex-B framed),
//   * an avcC record (H.264),
//   * an hvcC record (H.265).
std::vector<uint8_t> extract_param_sets(const std::vector<uint8_t> &desc,
                                         const char *dec_name) {
	std::vector<uint8_t> out;
	if (desc.empty())
		return out;
	if (has_start_code(desc.data(), desc.size()))
		return desc; // already Annex-B
	bool ok = false;
	if (strcmp(dec_name, "h264") == 0)
		ok = parse_avcc(desc.data(), desc.size(), out);
	else if (strcmp(dec_name, "hevc") == 0)
		ok = parse_hvcc(desc.data(), desc.size(), out);
	if (!ok)
		out.clear();
	return out;
}

// Map a WebCodecs codec string ("hvc1.1.6.L93.B0") to the ffmpeg decoder
// name. Returns NULL for a codec this build cannot decode.
const char *ffmpeg_decoder_name(const std::string &codec) {
	std::string prefix = codec.substr(0, codec.find('.'));
	for (char &c : prefix)
		c = (char)tolower((unsigned char)c);
	if (prefix == "hvc1" || prefix == "hev1")
		return "hevc";
	if (prefix == "avc1" || prefix == "avc3")
		return "h264";
	if (prefix == "vp8")
		return "vp8";
	if (prefix == "vp09" || prefix == "vp9")
		return "vp9";
	if (prefix == "av01")
		return "av1";
	return nullptr;
}

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------

// Record a fatal decode error. Called on the decode thread.
void set_error(nx_wc_decoder_t *d, const char *what) {
	std::lock_guard<std::mutex> lk(d->mutex);
	if (d->has_error.load(std::memory_order_relaxed))
		return;
	snprintf(d->error_buf, sizeof(d->error_buf), "%s", what);
	d->has_error.store(true, std::memory_order_release);
}

// Neutral color-space tag for a decoded frame — the SAME encoding
// source/media-decoder.cc uses (nx_image_t.yuv_colorspace):
//   0 = Rec709 limited   1 = Rec601 limited   2 = JPEG/full   3 = Rec709 full
// canvas.cc maps the tag to SkYUVColorSpace at draw time
// (kRec709_Limited / kRec601_Limited / kJPEG_Full / kRec709_Full).
//
// ⚠ If the bitstream carries no colorimetry (VUI absent — common for a cloud
//   game stream) this falls back to "height >= 720 ⇒ Rec709 ltd", which is
//   what an HD H.264/HEVC stream means in practice. That guess is why the
//   tag is reported in webcodecsVideoDecoderState: a wrong tag shows up as a
//   **uniform color cast**, never as rainbow garbage (see patches/0023-NOTES §6.2).
int derive_yuv_colorspace(AVFrame *frame, int height) {
	if (frame->color_range == AVCOL_RANGE_JPEG)
		return 2; // JPEG / full range
	if (frame->colorspace == AVCOL_SPC_BT709)
		return 0; // Rec709 limited
	if (frame->colorspace == AVCOL_SPC_BT470BG ||
	    frame->colorspace == AVCOL_SPC_SMPTE170M)
		return 1; // Rec601 limited
	return (height >= 720) ? 0 : 1; // unspecified -> guess by size
}

// Convert a decoded AVFrame into premultiplied BGRA, or (when the page asked
// for it) into tight planar I420.
//
// ★ 2026-10-26 — the I420 arm exists because BGRA is 4 B/px: a 1080p frame is
//   8.3 MB, a 720p frame 3.6 MB, and every one of those bytes is malloc'd,
//   copied (twice: here + poll() + RasterFromPixmapCopy) and then scaled on
//   the draw. I420 is 1.5 B/px (720p = 1.38 MB) and is exactly what
//   canvas.cc's YUVA branch (SkImages::TextureFromYUVAPixmaps) and its CPU
//   arm (expand_yuv_to_bgra_image) both expect.
//
//   Plane layout (identical to source/media-decoder.cc's out_yuv path, so the
//   two decoders cannot drift): ONE contiguous buffer, tight stride,
//      Y  at offset 0                    : w  * h     bytes, stride = w
//      U  at offset w*h                  : cw * ch    bytes, stride = cw
//      V  at offset w*h + cw*ch          : cw * ch    bytes, stride = cw
//   with cw = (w+1)/2, ch = (h+1)/2. That is what SkYUVAPixmaps::FromExternalMemory
//   builds when rowBytes is left null (kY_U_V / k420 / tight), i.e. the
//   `img->data` contract canvas.cc:2668-2676 already relies on.
bool convert_frame(nx_wc_decoder_t *d, AVFrame *src, nx_wc_pixels_t &out) {
	int w = src->width & ~1;
	int h = src->height & ~1;
	if (w <= 0 || h <= 0)
		return false;
	AVPixelFormat src_fmt = (AVPixelFormat)src->format;
	d->src_pix_fmt.store((int)src_fmt, std::memory_order_relaxed);
	const uint8_t *planes[4] = {src->data[0], src->data[1], src->data[2],
	                             src->data[3]};
	int strides[4] = {src->linesize[0], src->linesize[1], src->linesize[2],
	                  src->linesize[3]};
	// ★★ 0058: NVDEC 表面是 GOB 分块(tileFormat=TBL)，线性指针喂 swscale
	//   原理上就不成立 —— 让 ffmpeg 自己把硬件帧搬成线性 NV12。
	AVFrame *sw_tmp = nullptr;
#if defined(NX_ENABLE_NVTEGRA)
	if (src->format == AV_PIX_FMT_NVTEGRA) {
		// The NVDEC surface is pitch-linear NV12 (luma plane + interleaved
		// chroma plane) and the GPU wrote it, so the CPU cache has to be
		// invalidated before the CPU reads it. Same dance as
		// media-decoder.cc's hardware path.
		//
		// ⚠ The chroma plane is at `frame->data[1]`, which for NVTEGRA is
		//   linesize[0] * FFALIGN(h,32) (1280x720 ⇒ 942080), NOT `w*h`
		//   (921600 — those 16 rows are luma padding NVDEC never writes).
		//   Verified byte-for-byte on device: patches/0023-NOTES.md §1.2.
		//   ffmpeg's nvtegra hwcontext also programs NVDEC with that same
		//   offset, so reading data[1] is correct by construction.
		AVNVTegraMap *map = av_nvtegra_frame_get_fbuf_map(src);
		if (!map)
			return false;
		av_nvtegra_map_cache_op(map, NVMAP_CACHE_OP_INV,
		                        av_nvtegra_map_get_addr(map),
		                        av_nvtegra_map_get_size(map));
		src_fmt = AV_PIX_FMT_NV12;
		// ★ 0058: 走 av_hwframe_transfer_data 让 ffmpeg 反分块（内部用 VIC/CPU
		//   按 hwcontext 的 sw_format 搬），搬完 planes/strides 就是线性 NV12。
		sw_tmp = av_frame_alloc();
		if (sw_tmp) {
			sw_tmp->format = AV_PIX_FMT_NV12;
			sw_tmp->width = src->width;
			sw_tmp->height = src->height;
			if (av_hwframe_transfer_data(sw_tmp, src, 0) < 0) {
				av_frame_free(&sw_tmp);
			} else {
				for (int pi = 0; pi < 4; pi++) {
					planes[pi] = sw_tmp->data[pi];
					strides[pi] = sw_tmp->linesize[pi];
				}
				d->sw_transfer_ok.fetch_add(1, std::memory_order_relaxed);
			}
		}
		d->hw_frames.fetch_add(1, std::memory_order_relaxed);
	} else {
		d->sw_frames.fetch_add(1, std::memory_order_relaxed);
	}
#else
	d->sw_frames.fetch_add(1, std::memory_order_relaxed);
#endif
	const AVPixelFormat dst_fmt =
	    d->want_yuv ? AV_PIX_FMT_YUV420P : AV_PIX_FMT_BGRA;
	auto t0 = std::chrono::steady_clock::now();
	d->sws = sws_getCachedContext(d->sws, src->width, src->height, src_fmt, w,
	                              h, dst_fmt, SWS_BILINEAR, NULL, NULL, NULL);
	if (!d->sws)
		return false;
	uint8_t *dst[4] = {nullptr, nullptr, nullptr, nullptr};
	int dst_stride[4] = {0, 0, 0, 0};
	if (d->want_yuv) {
		const int cw = (w + 1) / 2;
		const int ch = (h + 1) / 2;
		out.data.resize((size_t)w * (size_t)h + 2 * (size_t)cw * (size_t)ch);
		uint8_t *y = out.data.data();
		dst[0] = y;
		dst[1] = y + (size_t)w * (size_t)h;
		dst[2] = dst[1] + (size_t)cw * (size_t)ch;
		dst_stride[0] = w;
		dst_stride[1] = cw;
		dst_stride[2] = cw;
	} else {
		out.data.resize((size_t)w * (size_t)h * 4);
		dst[0] = out.data.data();
		dst_stride[0] = w * 4;
	}
	sws_scale(d->sws, planes, strides, 0, src->height, dst, dst_stride);
	if (sw_tmp)
		av_frame_free(&sw_tmp);
	auto t1 = std::chrono::steady_clock::now();
	d->last_sws_us.store((uint32_t)std::chrono::duration_cast<
	                         std::chrono::microseconds>(t1 - t0)
	                         .count(),
	                     std::memory_order_relaxed);
	out.width = w;
	out.height = h;
	out.yuv = d->want_yuv;
	// Neutral color-space tag (same encoding media-decoder.cc uses; canvas.cc
	// switches on it to pick the SkYUVColorSpace / the integer matrix).
	out.yuv_cs = derive_yuv_colorspace(src, h);
	d->last_yuv_cs.store(out.yuv_cs, std::memory_order_relaxed);
	return true;
}

// The timestamp of a decoded frame, in microseconds. The packet PTS is
// carried through the decoder, so this survives B-frame reordering; the
// chunk's own timestamp is only a fallback.
int64_t frame_timestamp(AVFrame *f, int64_t fallback) {
	int64_t ts = f->best_effort_timestamp;
	if (ts == AV_NOPTS_VALUE)
		ts = f->pts;
	if (ts == AV_NOPTS_VALUE)
		return fallback;
	return ts;
}

// Pull every frame the decoder has ready and queue it for the main thread.
//
// A frame the decoder chokes on (a corrupt access unit, or a delta frame
// that arrives after the reference frames were dropped) is skipped rather
// than fatal: only a long run of failures is treated as a dead decoder,
// because a single bad frame must not tear the page's stream down.
int drain_frames(nx_wc_decoder_t *d, int64_t fallback_ts, int64_t duration) {
	for (;;) {
		int ret = avcodec_receive_frame(d->ctx, d->frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			return 0;
		if (ret < 0) {
			d->consecutive_errors++;
			if (d->consecutive_errors >= NX_WC_MAX_CONSECUTIVE_ERRORS) {
				set_error(d, "the decoder failed on too many frames");
				return ret;
			}
			continue;
		}
		d->consecutive_errors = 0;
		nx_wc_pixels_t px;
		if (!convert_frame(d, d->frame, px))
			continue;
		px.timestamp = frame_timestamp(d->frame, fallback_ts);
		px.duration = duration;
		std::lock_guard<std::mutex> lk(d->mutex);
		// Remember the decoded size: for a source with no container this
		// is the only place the dimensions exist (they come from the
		// parameter sets in the bitstream).
		d->frame_width = px.width;
		d->frame_height = px.height;
		d->out_queue.push_back(std::move(px));
	}
}

// Drain the decoder's own reorder buffer (the frames the decoder is still
// holding back) and make it accept packets again.
//
// This mirrors Chrome's ffmpeg-backed VideoDecoder: flush() sends a NULL
// packet to flush the DPB, drains what comes out, then calls
// avcodec_flush_buffers() so the decoder is ready for the next chunk.
void flush_decoder(nx_wc_decoder_t *d) {
	if (!d->ctx)
		return;
	avcodec_send_packet(d->ctx, NULL);
	drain_frames(d, 0, 0);
	avcodec_flush_buffers(d->ctx);
}

// Decode one access unit.
void decode_chunk(nx_wc_decoder_t *d, nx_wc_chunk_t &chunk) {
	if (!d->ctx || !d->pkt)
		return;
	AVPacket *pkt = d->pkt;
	av_packet_unref(pkt);

	// The parameter sets go in-band with the first chunk: the NVDEC path
	// parses the bitstream itself and needs the VPS/SPS/PPS to be there.
	std::vector<uint8_t> tmp;
	const uint8_t *data = chunk.data.data();
	size_t size = chunk.data.size();
	if (!d->params_injected && !d->param_sets.empty()) {
		tmp.reserve(d->param_sets.size() + size);
		tmp.insert(tmp.end(), d->param_sets.begin(), d->param_sets.end());
		tmp.insert(tmp.end(), chunk.data.begin(), chunk.data.end());
		data = tmp.data();
		size = tmp.size();
		d->params_injected = true;
	}

	pkt->data = (uint8_t *)data;
	pkt->size = (int)size;
	pkt->pts = chunk.timestamp;
	pkt->dts = chunk.timestamp;
	pkt->flags = chunk.key ? AV_PKT_FLAG_KEY : 0;

	int ret = avcodec_send_packet(d->ctx, pkt);
	if (ret == AVERROR(EAGAIN)) {
		// The decoder's input queue is full: drain it and retry once.
		if (drain_frames(d, chunk.timestamp, chunk.duration) < 0)
			return;
		ret = avcodec_send_packet(d->ctx, pkt);
	}
	if (ret < 0) {
		// 0045: say WHY the decoder rejected the packet.
		//
		//   "packet rejected by the decoder" told us nothing: the
		//   2026-10-23 device run produced 2261 access units and this
		//   one fixed line for every single one of them.  av_strerror()
		//   gives ffmpeg's own wording ("Invalid data found when
		//   processing input", "Non-existing pps", ...) -- that is the
		//   difference between "the codec is wrong" and "the bitstream
		//   is broken", and the report has to carry it.
		//
		//   NOTE: av_strerror() is declared in <libavutil/error.h>.  This
		//   file does NOT include it on purpose -- but it does not have
		//   to: the ffmpeg headers below sit inside an extern "C" block
		//   and pull it in transitively (libavcodec/avcodec.h ->
		//   libavutil/avutil.h -> libavutil/common.h -> "error.h"), so
		//   the declaration has C linkage and links against libavutil
		//   (engine/Makefile: FFMPEG_LIBS := -lavformat -lavcodec
		//   -lswscale -lswresample -lavutil ...).  Do NOT add another
		//   include: the header set of this translation unit must not
		//   change.
		char eb[128];
		eb[0] = 0;
		av_strerror(ret, eb, sizeof(eb));
		char msg[192];
		snprintf(msg, sizeof(msg), "packet rejected by the decoder: %s (ffmpeg code %d)",
		         (eb[0] ? eb : "unknown error"), ret);
		set_error(d, msg);
		return;
	}
	drain_frames(d, chunk.timestamp, chunk.duration);
}

// The decode thread: pulls chunks off the input queue, decodes them, pushes
// the frames onto the output queue.
void decode_thread_main(nx_wc_decoder_t *d) {
	for (;;) {
		nx_wc_chunk_t chunk;
		bool have_chunk = false;
		bool do_flush = false;
		{
			std::unique_lock<std::mutex> lk(d->mutex);
			d->cv.wait(lk, [d] {
				return d->quit.load(std::memory_order_acquire) ||
				       !d->in_queue.empty() || d->flush_pending;
			});
			if (d->quit.load(std::memory_order_acquire))
				return;
			if (d->in_queue.empty()) {
				if (d->flush_pending) {
					d->flush_pending = false;
					do_flush = true;
				}
			} else {
				chunk = std::move(d->in_queue.front());
				d->in_queue.pop_front();
				have_chunk = true;
			}
		}
		// The flush runs OUTSIDE the lock: it talks to the decoder and
		// queues the frames it produces.
		if (do_flush) {
			flush_decoder(d);
			std::lock_guard<std::mutex> lk(d->mutex);
			d->flush_done = true;
			continue;
		}
		if (!have_chunk)
			continue;
		decode_chunk(d, chunk);
		do_flush = false;
		{
			std::lock_guard<std::mutex> lk(d->mutex);
			d->frames_processed++;
			if (d->flush_pending && d->frames_processed >= d->flush_target) {
				d->flush_pending = false;
				do_flush = true;
			}
		}
		if (do_flush) {
			flush_decoder(d);
			std::lock_guard<std::mutex> lk(d->mutex);
			d->flush_done = true;
		}
	}
}

// Tear the codec down: stop the decode thread, free the ffmpeg resources.
void close_codec(nx_wc_decoder_t *d) {
	if (d->thread.joinable()) {
		d->quit.store(true, std::memory_order_release);
		{
			std::lock_guard<std::mutex> lk(d->mutex);
		}
		d->cv.notify_all();
		d->thread.join();
	}
	if (d->ctx) {
		avcodec_free_context(&d->ctx);
		d->ctx = nullptr;
	}
	if (d->hw_device_ref) {
		av_buffer_unref(&d->hw_device_ref);
		d->hw_device_ref = nullptr;
	}
	if (d->sws) {
		sws_freeContext(d->sws);
		d->sws = nullptr;
	}
	if (d->frame) {
		av_frame_free(&d->frame);
		d->frame = nullptr;
	}
	if (d->pkt) {
		av_packet_free(&d->pkt);
		d->pkt = nullptr;
	}
}

// Open the codec for `codec`. `hardware` asks for the Tegra NVDEC decoder;
// when the hardware path is refused the software decoder is used instead.
bool open_decoder(nx_wc_decoder_t *d, const std::string &codec, int w, int h,
                  const std::vector<uint8_t> &desc, bool hardware,
                  char *errbuf, size_t errbuf_size) {
	const char *dec_name = ffmpeg_decoder_name(codec);
	if (!dec_name) {
		snprintf(errbuf, errbuf_size, "unsupported codec: %s", codec.c_str());
		return false;
	}
	const AVCodec *dec = avcodec_find_decoder_by_name(dec_name);
	if (!dec) {
		snprintf(errbuf, errbuf_size, "no decoder available for %s", dec_name);
		return false;
	}

	d->codec = codec;
	d->coded_width = w;
	d->coded_height = h;
	d->hardware = hardware;
	d->param_sets = extract_param_sets(desc, dec_name);

	d->ctx = avcodec_alloc_context3(dec);
	if (!d->ctx) {
		snprintf(errbuf, errbuf_size, "out of memory");
		return false;
	}
	d->ctx->thread_count = 2;
	// The page's timestamps are microseconds; keep the decoder's time base
	// aligned with them so a decoded frame's PTS is the page's timestamp.
	d->ctx->time_base = AVRational{1, NX_WC_TIMEBASE_DEN};
	if (!d->param_sets.empty()) {
		d->ctx->extradata = (uint8_t *)av_mallocz(d->param_sets.size() +
		                                          AV_INPUT_BUFFER_PADDING_SIZE);
		if (d->ctx->extradata)
			memcpy(d->ctx->extradata, d->param_sets.data(),
			       d->param_sets.size());
	}
#if defined(NX_ENABLE_NVTEGRA)
	if (hardware) {
		// Same setup as media-decoder.cc: create the NVDEC device and let
		// libavcodec build the AVHWFramesContext around it.
		if (av_hwdevice_ctx_create(&d->hw_device_ref, AV_HWDEVICE_TYPE_NVTEGRA,
		                           NULL, NULL, 0) >= 0) {
			d->ctx->hw_device_ctx = av_buffer_ref(d->hw_device_ref);
			d->ctx->pix_fmt = AV_PIX_FMT_NVTEGRA;
		} else {
			d->hw_device_ref = nullptr;
		}
	}
#endif
	int ret = avcodec_open2(d->ctx, dec, NULL);
#if defined(NX_ENABLE_NVTEGRA)
	if (ret < 0 && d->ctx->hw_device_ctx) {
		// The hardware decoder refused this stream 鈥?retry in software
		// rather than failing the configure() outright.
		avcodec_free_context(&d->ctx);
		d->ctx = avcodec_alloc_context3(dec);
		if (!d->ctx) {
			snprintf(errbuf, errbuf_size, "out of memory");
			close_codec(d);
			return false;
		}
		d->ctx->thread_count = 2;
		d->ctx->time_base = AVRational{1, NX_WC_TIMEBASE_DEN};
		if (!d->param_sets.empty()) {
			d->ctx->extradata =
			    (uint8_t *)av_mallocz(d->param_sets.size() +
			                          AV_INPUT_BUFFER_PADDING_SIZE);
			if (d->ctx->extradata)
				memcpy(d->ctx->extradata, d->param_sets.data(),
				       d->param_sets.size());
		}
		ret = avcodec_open2(d->ctx, dec, NULL);
		d->hardware = false;
	}
#endif
	if (ret < 0) {
		av_strerror(ret, errbuf, errbuf_size);
		close_codec(d);
		return false;
	}

	d->frame = av_frame_alloc();
	d->pkt = av_packet_alloc();
	if (!d->frame || !d->pkt) {
		snprintf(errbuf, errbuf_size, "out of memory");
		close_codec(d);
		return false;
	}
	d->quit.store(false, std::memory_order_release);
	d->thread = std::thread(decode_thread_main, d);
	return true;
}

// ---------------------------------------------------------------------------
// Handles
// ---------------------------------------------------------------------------

void free_decoder(nx_wc_decoder_t *d) {
	if (!d)
		return;
	close_codec(d);
	delete d;
}

void free_frame(nx_wc_frame_t *f) {
	if (!f)
		return;
	nx_image_release_cache(&f->image);
	free(f->image.data);
	f->image.data = nullptr;
	f->image.width = 0;
	f->image.height = 0;
	delete f;
}

nx_wc_decoder_t *get_decoder(Isolate *iso, Local<Value> val) {
	nx_wc_decoder_t *d = nx::Unwrap<nx_wc_decoder_t>(val);
	if (!d || d->magic != NX_WC_DECODER_MAGIC) {
		nx_throw(iso, "expected a VideoDecoder handle");
		return nullptr;
	}
	return d;
}

nx_wc_frame_t *get_frame(Isolate *iso, Local<Value> val) {
	nx_wc_frame_t *f = nx::Unwrap<nx_wc_frame_t>(val);
	if (!f || f->wc_magic != NX_WC_FRAME_MAGIC) {
		nx_throw(iso, "expected a VideoFrame handle");
		return nullptr;
	}
	return f;
}

// ---------------------------------------------------------------------------
// Bindings
// ---------------------------------------------------------------------------

// webcodecsVideoDecoderNew() -> handle
void nx_wc_decoder_new(const FunctionCallbackInfo<Value> &info) {
	Isolate *iso = info.GetIsolate();
	nx_wc_decoder_t *d = new nx_wc_decoder_t();
	Local<Object> obj = nx::NewWrapped(iso);
	nx::Wrap<nx_wc_decoder_t>(iso, obj, d, free_decoder);
	info.GetReturnValue().Set(obj);
}

// webcodecsVideoDecoderConfigure(handle, codec, width, height, description,
//                                 hardware) -> "" on success, an error message
// otherwise. Returning the message (instead of throwing) lets the JS side
// pick the right DOMException name (NotSupportedError vs TypeError).
void nx_wc_decoder_configure(const FunctionCallbackInfo<Value> &info) {
	Isolate *iso = info.GetIsolate();
	nx_wc_decoder_t *d = get_decoder(iso, info[0]);
	if (!d)
		return;
	if (d->state == NX_WC_CLOSED) {
		info.GetReturnValue().Set(nx_str(iso, "the decoder is closed"));
		return;
	}
	if (d->state == NX_WC_CONFIGURED) {
		info.GetReturnValue().Set(nx_str(iso, "the decoder is already configured"));
		return;
	}
	String::Utf8Value codec_val(iso, info[1]);
	std::string codec = *codec_val ? *codec_val : "";
	if (codec.empty()) {
		info.GetReturnValue().Set(nx_str(iso, "codec is required"));
		return;
	}
	Local<Context> ctx = iso->GetCurrentContext();
	int w = 0, h = 0;
	if (info[2]->IsNumber()) {
		int32_t v = 0;
		if (info[2]->Int32Value(ctx).To(&v))
			w = v;
	}
	if (info[3]->IsNumber()) {
		int32_t v = 0;
		if (info[3]->Int32Value(ctx).To(&v))
			h = v;
	}
	std::vector<uint8_t> desc;
	if (info[4]->IsObject()) {
		size_t n = 0;
		uint8_t *p = NX_GetBufferSource(iso, &n, info[4]);
		if (p && n)
			desc.assign(p, p + n);
	}
	bool hardware = info[5]->BooleanValue(iso);
	// ★ 2026-10-26: optional 7th argument — planar I420 output. Absent/false
	//   ⇒ BGRA, i.e. exactly the pre-patch behaviour (no silent default flip).
	bool want_yuv = info.Length() > 6 && info[6]->BooleanValue(iso);
	d->want_yuv = want_yuv;

	char err[256] = {};
	if (!open_decoder(d, codec, w, h, desc, hardware, err, sizeof(err))) {
		info.GetReturnValue().Set(nx_str_lossy(iso, err));
		return;
	}
	d->state = NX_WC_CONFIGURED;
	info.GetReturnValue().Set(nx_str(iso, ""));
}

// webcodecsVideoDecoderDecode(handle, bytes, isKey, timestamp, duration)
void nx_wc_decoder_decode(const FunctionCallbackInfo<Value> &info) {
	Isolate *iso = info.GetIsolate();
	nx_wc_decoder_t *d = get_decoder(iso, info[0]);
	if (!d)
		return;
	if (d->state != NX_WC_CONFIGURED) {
		nx_throw(iso, "the decoder is not configured");
		return;
	}
	size_t n = 0;
	uint8_t *p = NX_GetBufferSource(iso, &n, info[1]);
	if (!p) {
		nx_throw(iso, "expected the encoded chunk data");
		return;
	}
	Local<Context> ctx = iso->GetCurrentContext();
	nx_wc_chunk_t chunk;
	chunk.data.assign(p, p + n);
	chunk.key = info[2]->BooleanValue(iso);
	// Timestamps are microseconds 鈥?a double holds them exactly.
	double ts = 0, dur = 0;
	if (info[3]->IsNumber())
		info[3]->NumberValue(ctx).To(&ts);
	if (info[4]->IsNumber())
		info[4]->NumberValue(ctx).To(&dur);
	chunk.timestamp = (int64_t)ts;
	chunk.duration = (int64_t)dur;

	// WebRTC may hand us the access unit length-prefixed (AVCC); the
	// decoder wants Annex-B.
	{
		std::vector<uint8_t> annexb;
		if (to_annexb(chunk.data.data(), chunk.data.size(), annexb))
			chunk.data.swap(annexb);
	}

	{
		std::lock_guard<std::mutex> lk(d->mutex);
		d->in_queue.push_back(std::move(chunk));
		d->frames_submitted++;
	}
	d->cv.notify_one();
}

// webcodecsVideoDecoderFlush(handle) 鈥?ask the decode thread to report back
// once every chunk submitted so far has been decoded.
void nx_wc_decoder_flush(const FunctionCallbackInfo<Value> &info) {
	Isolate *iso = info.GetIsolate();
	nx_wc_decoder_t *d = get_decoder(iso, info[0]);
	if (!d)
		return;
	{
		std::lock_guard<std::mutex> lk(d->mutex);
		d->flush_target = d->frames_submitted;
		d->flush_pending = true;
		d->flush_done = false;
	}
	d->cv.notify_one();
}

// webcodecsVideoDecoderReset(handle)
void nx_wc_decoder_reset(const FunctionCallbackInfo<Value> &info) {
	Isolate *iso = info.GetIsolate();
	nx_wc_decoder_t *d = get_decoder(iso, info[0]);
	if (!d)
		return;
	if (d->state == NX_WC_CLOSED)
		return;
	close_codec(d);
	{
		std::lock_guard<std::mutex> lk(d->mutex);
		d->in_queue.clear();
		d->out_queue.clear();
		d->frames_submitted = 0;
		d->frames_processed = 0;
		d->flush_pending = false;
		d->flush_done = true;
	}
	d->consecutive_errors = 0;
	d->param_sets.clear();
	d->params_injected = false;
	d->has_error.store(false, std::memory_order_release);
	d->error_buf[0] = 0;
	d->state = NX_WC_UNCONFIGURED;
}

// webcodecsVideoDecoderClose(handle)
void nx_wc_decoder_close(const FunctionCallbackInfo<Value> &info) {
	Isolate *iso = info.GetIsolate();
	nx_wc_decoder_t *d = get_decoder(iso, info[0]);
	if (!d)
		return;
	if (d->state == NX_WC_CLOSED)
		return;
	close_codec(d);
	{
		std::lock_guard<std::mutex> lk(d->mutex);
		d->in_queue.clear();
		d->out_queue.clear();
		d->flush_pending = false;
		d->flush_done = true;
	}
	d->state = NX_WC_CLOSED;
}

// webcodecsVideoDecoderPoll(handle) -> a VideoFrame handle, or undefined
// when no frame is ready.
void nx_wc_decoder_poll(const FunctionCallbackInfo<Value> &info) {
	Isolate *iso = info.GetIsolate();
	nx_wc_decoder_t *d = get_decoder(iso, info[0]);
	if (!d)
		return;
	nx_wc_pixels_t px;
	{
		std::lock_guard<std::mutex> lk(d->mutex);
		if (d->out_queue.empty())
			return; // undefined
		px = std::move(d->out_queue.front());
		d->out_queue.pop_front();
	}
	d->frames_output++;

	nx_wc_frame_t *f = new nx_wc_frame_t();
	memset(&f->image, 0, sizeof(f->image));
	f->image.magic = NX_IMAGE_MAGIC;
	f->image.format = FORMAT_UNKNOWN;
	f->image.width = (u32)px.width;
	f->image.height = (u32)px.height;
	f->image.data = (u8 *)malloc(px.data.size());
	if (!f->image.data) {
		delete f;
		nx_throw_oom(iso, px.data.size());
		return;
	}
	memcpy(f->image.data, px.data.data(), px.data.size());
	// ★ 2026-10-26: tell canvas.cc what the bytes are. is_yuv=true makes
	//   drawImage() build a YUVA image (GPU: SkImages::TextureFromYUVAPixmaps;
	//   `[canvas] yuv_convert = cpu`: expand_yuv_to_bgra_image on the CPU)
	//   instead of RasterFromPixmapCopy-ing a 4 B/px buffer.
	//   ⚠ Read this before enabling `yuv`: on a RASTER canvas there is NO
	//   YUVA branch at all (canvas.cc's `else if (img->is_yuv)` needs a
	//   GrDirectContext) — see handoff/analysis/2026-10-26-上屏链路审计.md.
	f->image.is_yuv = px.yuv;
	f->image.yuv_colorspace = px.yuv_cs;
	f->yuv = px.yuv;
	f->wc_magic = NX_WC_FRAME_MAGIC;
	f->timestamp = px.timestamp;
	f->duration = px.duration;
	f->coded_width = (u32)px.width;
	f->coded_height = (u32)px.height;
	f->display_width = (u32)px.width;
	f->display_height = (u32)px.height;

	Local<Object> obj = nx::NewWrapped(iso);
	nx::Wrap<nx_wc_frame_t>(iso, obj, f, free_frame);
	info.GetReturnValue().Set(obj);
}

// webcodecsVideoDecoderState(handle) -> a snapshot of the decoder state.
void nx_wc_decoder_state(const FunctionCallbackInfo<Value> &info) {
	Isolate *iso = info.GetIsolate();
	nx_wc_decoder_t *d = get_decoder(iso, info[0]);
	if (!d)
		return;
	Local<Context> ctx = iso->GetCurrentContext();
	Local<Object> r = Object::New(iso);
	size_t queue_size = 0;
	bool flush_done = true;
	int frame_w = 0, frame_h = 0;
	{
		std::lock_guard<std::mutex> lk(d->mutex);
		queue_size = d->in_queue.size();
		flush_done = d->flush_done;
		frame_w = d->frame_width;
		frame_h = d->frame_height;
	}
	r->Set(ctx, nx_str(iso, "state"), nx_str(iso, state_name(d->state))).Check();
	r->Set(ctx, nx_str(iso, "codec"), nx_str(iso, d->codec.c_str())).Check();
	r->Set(ctx, nx_str(iso, "numberOfFrames"),
	       Number::New(iso, (double)d->frames_submitted)).Check();
	r->Set(ctx, nx_str(iso, "numberOfOutputFrames"),
	       Number::New(iso, (double)d->frames_output)).Check();
	r->Set(ctx, nx_str(iso, "decodeQueueSize"),
	       Integer::NewFromUnsigned(iso, (uint32_t)queue_size)).Check();
	r->Set(ctx, nx_str(iso, "flushDone"), Boolean::New(iso, flush_done)).Check();
	// ★★ 2026-10-26 — "did we actually get NVDEC / what pixels came out /
	//    what did the conversion cost". This is the judgement basis the device
	//    round needs: `hwDecode=false` with `srcPixFmt=yuv420p` means the
	//    hardware arm silently fell back to software, and the app's
	//    `hardwareAcceleration: 'prefer-hardware'` proves nothing on its own.
	{
		int pf = d->src_pix_fmt.load(std::memory_order_relaxed);
		// Deliberately NOT av_get_pix_fmt_name() (that lives in
		// libavutil/pixdesc.h, which this translation unit does not include —
		// keep the include set unchanged). Only three formats can appear here.
		const char *pf_name = "other";
		if (pf < 0)
			pf_name = "none";
		else if ((AVPixelFormat)pf == AV_PIX_FMT_YUV420P)
			pf_name = "yuv420p";
#if defined(NX_ENABLE_NVTEGRA)
		else if ((AVPixelFormat)pf == AV_PIX_FMT_NVTEGRA)
			pf_name = "nvtegra";
#endif
		r->Set(ctx, nx_str(iso, "hwDecode"), Boolean::New(iso, d->hardware)).Check();
		r->Set(ctx, nx_str(iso, "srcPixFmt"), nx_str(iso, pf_name)).Check();
		r->Set(ctx, nx_str(iso, "outFormat"),
		       nx_str(iso, d->want_yuv ? "I420" : "BGRA")).Check();
		r->Set(ctx, nx_str(iso, "yuvColorspace"),
		       Integer::New(iso, d->want_yuv
		                            ? d->last_yuv_cs.load(std::memory_order_relaxed)
		                            : -1)).Check();
		r->Set(ctx, nx_str(iso, "convertUs"),
		       Number::New(iso,
		                   (double)d->last_sws_us.load(std::memory_order_relaxed))).Check();
		r->Set(ctx, nx_str(iso, "hwFrames"),
		       Number::New(iso, (double)d->hw_frames.load(std::memory_order_relaxed))).Check();
		r->Set(ctx, nx_str(iso, "swFrames"),
		       Number::New(iso, (double)d->sw_frames.load(std::memory_order_relaxed))).Check();
		r->Set(ctx, nx_str(iso, "swTransferOk"),
		       Number::New(iso,
		                   (double)d->sw_transfer_ok.load(std::memory_order_relaxed))).Check();
		r->Set(ctx, nx_str(iso, "frameWidth"),
		       Integer::New(iso, frame_w)).Check();
		r->Set(ctx, nx_str(iso, "frameHeight"),
		       Integer::New(iso, frame_h)).Check();
	}
	if (d->has_error.load(std::memory_order_acquire))
		r->Set(ctx, nx_str(iso, "error"), nx_str_lossy(iso, d->error_buf)).Check();
	else
		r->Set(ctx, nx_str(iso, "error"), Null(iso)).Check();
	info.GetReturnValue().Set(r);
}

// webcodecsIsConfigSupported(codec) -> boolean
void nx_wc_is_config_supported(const FunctionCallbackInfo<Value> &info) {
	Isolate *iso = info.GetIsolate();
	String::Utf8Value codec_val(iso, info[0]);
	std::string codec = *codec_val ? *codec_val : "";
	bool supported = false;
	const char *dec_name = ffmpeg_decoder_name(codec);
	if (dec_name)
		supported = avcodec_find_decoder_by_name(dec_name) != nullptr;
	info.GetReturnValue().Set(Boolean::New(iso, supported));
}

// webcodecsVideoFrameClose(handle)
void nx_wc_frame_close(const FunctionCallbackInfo<Value> &info) {
	Isolate *iso = info.GetIsolate();
	nx_wc_frame_t *f = get_frame(iso, info[0]);
	if (!f)
		return;
	if (f->closed)
		return;
	f->closed = true;
	nx_image_release_cache(&f->image);
	free(f->image.data);
	f->image.data = nullptr;
	f->image.width = 0;
	f->image.height = 0;
}

// ---- prototype getters ---------------------------------------------------

void nx_wc_get_state(const FunctionCallbackInfo<Value> &info) {
	nx_wc_decoder_t *d = nx::Unwrap<nx_wc_decoder_t>(info.This());
	info.GetReturnValue().Set(
	    nx_str(info.GetIsolate(), d ? state_name(d->state) : "unconfigured"));
}

// The coded size of the decoded frames, as the decoder learned it from the
// bitstream (the SPS for H.264 / H.265). 0 until the first frame has been
// decoded.
//
// This is the ONLY source of the dimensions for a streamed feed: there is no
// container (no moov), so nothing else could tell the page how big the video
// is. The stream's total duration, on the other hand, is NOT derivable from
// a raw stream at all -- there is no "end of file" to measure -- so the page
// must treat the duration as unknown.
void nx_wc_get_coded_width(const FunctionCallbackInfo<Value> &info) {
	nx_wc_decoder_t *d = nx::Unwrap<nx_wc_decoder_t>(info.This());
	int w = 0;
	if (d) {
		std::lock_guard<std::mutex> lk(d->mutex);
		w = d->frame_width;
	}
	info.GetReturnValue().Set(Integer::New(info.GetIsolate(), w));
}

void nx_wc_get_coded_height(const FunctionCallbackInfo<Value> &info) {
	nx_wc_decoder_t *d = nx::Unwrap<nx_wc_decoder_t>(info.This());
	int h = 0;
	if (d) {
		std::lock_guard<std::mutex> lk(d->mutex);
		h = d->frame_height;
	}
	info.GetReturnValue().Set(Integer::New(info.GetIsolate(), h));
}

void nx_wc_get_codec(const FunctionCallbackInfo<Value> &info) {
	nx_wc_decoder_t *d = nx::Unwrap<nx_wc_decoder_t>(info.This());
	info.GetReturnValue().Set(
	    nx_str_lossy(info.GetIsolate(), d ? d->codec.c_str() : ""));
}

void nx_wc_get_number_of_frames(const FunctionCallbackInfo<Value> &info) {
	nx_wc_decoder_t *d = nx::Unwrap<nx_wc_decoder_t>(info.This());
	info.GetReturnValue().Set(
	    Number::New(info.GetIsolate(), d ? (double)d->frames_submitted : 0));
}

void nx_wc_get_number_of_output_frames(const FunctionCallbackInfo<Value> &info) {
	nx_wc_decoder_t *d = nx::Unwrap<nx_wc_decoder_t>(info.This());
	info.GetReturnValue().Set(
	    Number::New(info.GetIsolate(), d ? (double)d->frames_output : 0));
}

void nx_wc_get_decode_queue_size(const FunctionCallbackInfo<Value> &info) {
	Isolate *iso = info.GetIsolate();
	nx_wc_decoder_t *d = nx::Unwrap<nx_wc_decoder_t>(info.This());
	size_t n = 0;
	if (d) {
		std::lock_guard<std::mutex> lk(d->mutex);
		n = d->in_queue.size();
	}
	info.GetReturnValue().Set(Integer::NewFromUnsigned(iso, (uint32_t)n));
}

void nx_wc_frame_get_coded_width(const FunctionCallbackInfo<Value> &info) {
	nx_wc_frame_t *f = nx::Unwrap<nx_wc_frame_t>(info.This());
	info.GetReturnValue().Set(
	    Integer::NewFromUnsigned(info.GetIsolate(), f ? f->coded_width : 0));
}

void nx_wc_frame_get_coded_height(const FunctionCallbackInfo<Value> &info) {
	nx_wc_frame_t *f = nx::Unwrap<nx_wc_frame_t>(info.This());
	info.GetReturnValue().Set(
	    Integer::NewFromUnsigned(info.GetIsolate(), f ? f->coded_height : 0));
}

void nx_wc_frame_get_display_width(const FunctionCallbackInfo<Value> &info) {
	nx_wc_frame_t *f = nx::Unwrap<nx_wc_frame_t>(info.This());
	info.GetReturnValue().Set(
	    Integer::NewFromUnsigned(info.GetIsolate(), f ? f->display_width : 0));
}

void nx_wc_frame_get_display_height(const FunctionCallbackInfo<Value> &info) {
	nx_wc_frame_t *f = nx::Unwrap<nx_wc_frame_t>(info.This());
	info.GetReturnValue().Set(
	    Integer::NewFromUnsigned(info.GetIsolate(), f ? f->display_height : 0));
}

void nx_wc_frame_get_timestamp(const FunctionCallbackInfo<Value> &info) {
	nx_wc_frame_t *f = nx::Unwrap<nx_wc_frame_t>(info.This());
	info.GetReturnValue().Set(
	    Number::New(info.GetIsolate(), f ? (double)f->timestamp : 0));
}

void nx_wc_frame_get_duration(const FunctionCallbackInfo<Value> &info) {
	nx_wc_frame_t *f = nx::Unwrap<nx_wc_frame_t>(info.This());
	if (!f || f->duration <= 0)
		info.GetReturnValue().SetNull();
	else
		info.GetReturnValue().Set(Number::New(info.GetIsolate(), (double)f->duration));
}

void nx_wc_frame_get_format(const FunctionCallbackInfo<Value> &info) {
	nx_wc_frame_t *f = nx::Unwrap<nx_wc_frame_t>(info.This());
	const char *fmt = (f && f->yuv) ? "I420" : "BGRA";
	info.GetReturnValue().Set(nx_str(info.GetIsolate(), fmt));
}

// ---- class installers ----------------------------------------------------

// webcodecsVideoDecoderInitClass(ClassCtor) 鈥?install the prototype getters.
void nx_wc_decoder_init_class(const FunctionCallbackInfo<Value> &info) {
	Isolate *iso = info.GetIsolate();
	Local<Context> ctx = iso->GetCurrentContext();
	if (info.Length() < 1 || !info[0]->IsObject()) {
		nx_throw(iso, "webcodecsVideoDecoderInitClass(ClassCtor) 鈥?object required");
		return;
	}
	Local<Value> proto_val;
	if (!info[0].As<Object>()->Get(ctx, nx_str(iso, "prototype")).ToLocal(&proto_val))
		return;
	if (!proto_val->IsObject())
		return;
	Local<Object> proto = proto_val.As<Object>();
	NX_DEF_GET(proto, "state", nx_wc_get_state);
	NX_DEF_GET(proto, "codec", nx_wc_get_codec);
	NX_DEF_GET(proto, "codedWidth", nx_wc_get_coded_width);
	NX_DEF_GET(proto, "codedHeight", nx_wc_get_coded_height);
	NX_DEF_GET(proto, "numberOfFrames", nx_wc_get_number_of_frames);
	NX_DEF_GET(proto, "numberOfOutputFrames", nx_wc_get_number_of_output_frames);
	NX_DEF_GET(proto, "decodeQueueSize", nx_wc_get_decode_queue_size);
}

// webcodecsVideoFrameInitClass(ClassCtor) 鈥?install the prototype getters.
void nx_wc_frame_init_class(const FunctionCallbackInfo<Value> &info) {
	Isolate *iso = info.GetIsolate();
	Local<Context> ctx = iso->GetCurrentContext();
	if (info.Length() < 1 || !info[0]->IsObject()) {
		nx_throw(iso, "webcodecsVideoFrameInitClass(ClassCtor) 鈥?object required");
		return;
	}
	Local<Value> proto_val;
	if (!info[0].As<Object>()->Get(ctx, nx_str(iso, "prototype")).ToLocal(&proto_val))
		return;
	if (!proto_val->IsObject())
		return;
	Local<Object> proto = proto_val.As<Object>();
	NX_DEF_GET(proto, "codedWidth", nx_wc_frame_get_coded_width);
	NX_DEF_GET(proto, "codedHeight", nx_wc_frame_get_coded_height);
	NX_DEF_GET(proto, "displayWidth", nx_wc_frame_get_display_width);
	NX_DEF_GET(proto, "displayHeight", nx_wc_frame_get_display_height);
	NX_DEF_GET(proto, "timestamp", nx_wc_frame_get_timestamp);
	NX_DEF_GET(proto, "duration", nx_wc_frame_get_duration);
	NX_DEF_GET(proto, "format", nx_wc_frame_get_format);
}

} // namespace

void nx_init_webcodecs(Isolate *iso, Local<Object> init_obj) {
	NX_SET_FUNC(init_obj, "webcodecsVideoDecoderNew", nx_wc_decoder_new);
	NX_SET_FUNC(init_obj, "webcodecsVideoDecoderInitClass",
	            nx_wc_decoder_init_class);
	NX_SET_FUNC(init_obj, "webcodecsVideoDecoderConfigure",
	            nx_wc_decoder_configure);
	NX_SET_FUNC(init_obj, "webcodecsVideoDecoderDecode", nx_wc_decoder_decode);
	NX_SET_FUNC(init_obj, "webcodecsVideoDecoderFlush", nx_wc_decoder_flush);
	NX_SET_FUNC(init_obj, "webcodecsVideoDecoderReset", nx_wc_decoder_reset);
	NX_SET_FUNC(init_obj, "webcodecsVideoDecoderClose", nx_wc_decoder_close);
	NX_SET_FUNC(init_obj, "webcodecsVideoDecoderPoll", nx_wc_decoder_poll);
	NX_SET_FUNC(init_obj, "webcodecsVideoDecoderState", nx_wc_decoder_state);
	NX_SET_FUNC(init_obj, "webcodecsVideoFrameInitClass", nx_wc_frame_init_class);
	NX_SET_FUNC(init_obj, "webcodecsVideoFrameClose", nx_wc_frame_close);
	NX_SET_FUNC(init_obj, "webcodecsIsConfigSupported", nx_wc_is_config_supported);
}

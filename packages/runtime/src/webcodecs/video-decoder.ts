import { $, type VideoDecoderHandle } from '../$';
import { console } from '../console';
import { DOMException } from '../dom-exception';
import { clearInterval, setInterval } from '../timers';
import { bufferSourceToArrayBuffer, createInternal, proto } from '../utils';
import type { BufferSource } from '../types';
import type { EncodedVideoChunk } from './encoded-video-chunk';
import { VideoFrame } from './video-frame';

/**
 * The state of a {@link VideoDecoder}.
 *
 * - `unconfigured` — the decoder needs a `configure()` call.
 * - `configured` — the decoder is ready to decode.
 * - `closed` — the decoder is closed and cannot be used again.
 */
export type VideoDecoderState = 'unconfigured' | 'configured' | 'closed';

/**
 * The dictionary passed to the {@link VideoDecoder} constructor.
 */
export interface VideoDecoderInit {
	/** Called once per decoded frame. */
	output: (frame: VideoFrame) => void;
	/** Called when the decoder fails. Optional. */
	error?: (error: DOMException) => void;
}

/**
 * The dictionary passed to {@link VideoDecoder.configure}.
 */
export interface VideoDecoderConfig {
	/**
	 * The codec, e.g. `"hvc1.1.6.L93.B0"` (H.265) or `"avc1.42E01E"` (H.264).
	 */
	codec: string;
	/** The width of the coded frames, in pixels. Optional. */
	codedWidth?: number;
	/** The height of the coded frames, in pixels. Optional. */
	codedHeight?: number;
	/**
	 * The codec's parameter sets (SPS/PPS, and VPS for H.265). Optional —
	 * a stream that carries them in-band does not need them here.
	 */
	description?: BufferSource;
	/** Whether to prefer the hardware decoder. Defaults to `no-preference`. */
	hardwareAcceleration?:
		| 'no-preference'
		| 'prefer-hardware'
		| 'prefer-software';
	/** Whether to favour low latency over compression efficiency. */
	optimizeForLatency?: boolean;
	/**
	 * ★ 2026-10-26 (nx.js extension): decode into **planar I420** (1.5 B/px)
	 * instead of premultiplied BGRA (4 B/px) and hand the frame to the canvas
	 * as a YUVA image. Defaults to `false` (BGRA), i.e. the W3C-shaped
	 * behaviour; the extra `VideoFrame.format` value `"I420"` reports which
	 * arm produced the frame.
	 */
	yuv?: boolean;
}

/**
 * The result of {@link VideoDecoder.isConfigSupported}.
 */
export interface VideoDecoderSupport {
	/** Whether the configuration is supported. */
	supported: boolean;
	/** The configuration, when it is supported. */
	config?: VideoDecoderConfig;
}

/**
 * ★ 2026-10-26 (nx.js extension): the decode-path diagnostics of a
 * {@link VideoDecoder}, as returned by {@link VideoDecoder.stats}.
 *
 * This mirrors what `$.webcodecsVideoDecoderState()` reports (the native
 * half is `patches/0055-webcodecs-diag-i420.patch`). Every field is
 * optional so an engine built without that patch still type-checks.
 */
export interface VideoDecoderStats {
	/**
	 * Whether the native decoder took the hardware (NVDEC) arm.
	 *
	 * `hardwareAcceleration: 'prefer-hardware'` only expresses a *wish* —
	 * ffmpeg may still fall back to software, and this is the field that
	 * says whether it did.
	 */
	hwDecode?: boolean;
	/**
	 * The pixel format ffmpeg actually produced: `"nvtegra"` (the hardware
	 * frame), `"yuv420p"`, `"none"` (nothing decoded yet) or `"other"`.
	 *
	 * `hwDecode` + `srcPixFmt` together are the honest hardware verdict:
	 * `true` / `"yuv420p"` means the hardware arm silently fell back.
	 */
	srcPixFmt?: string | null;
	/** The format the frames are handed over as. */
	outFormat?: 'BGRA' | 'I420';
	/**
	 * The YUV matrix of the last I420 frame (`AVColorSpace`). `-1` in BGRA
	 * mode, where no colour conversion matrix is chosen.
	 */
	yuvColorspace?: number;
	/** Microseconds spent last time on the planar/BGRA conversion. */
	convertUs?: number;
	/** How many frames came off the hardware decoder. */
	hwFrames?: number;
	/** How many frames came off the software decoder. */
	swFrames?: number;
	/** Width of the decoded frames, in pixels (0 until the first one). */
	frameWidth?: number;
	/** Height of the decoded frames, in pixels (0 until the first one). */
	frameHeight?: number;
}

interface VideoDecoderInternal {
	handle: VideoDecoderHandle;
	output: (frame: VideoFrame) => void;
	error: (error: DOMException) => void;
	closed: boolean;
	errorReported: boolean;
	pump: number | null;
	flushes: Array<{ resolve: () => void; reject: (error: Error) => void }>;
}

const _ = createInternal<VideoDecoder, VideoDecoderInternal>();

function stopPump(i: VideoDecoderInternal) {
	if (i.pump !== null) {
		clearInterval(i.pump);
		i.pump = null;
	}
}

function rejectFlushes(i: VideoDecoderInternal, error: Error) {
	const flushes = i.flushes;
	i.flushes = [];
	for (const f of flushes) f.reject(error);
}

// Hand every decoded frame to the page. Returns how many frames were
// delivered.
function drainFrames(i: VideoDecoderInternal): number {
	let delivered = 0;
	for (;;) {
		const raw = $.webcodecsVideoDecoderPoll(i.handle);
		if (!raw) break;
		delivered++;
		const frame = proto(raw, VideoFrame) as unknown as VideoFrame;
		try {
			i.output(frame);
		} catch (err) {
			console.error('VideoDecoder: the output callback threw', err);
		}
	}
	return delivered;
}

// The pump runs on the runtime's frame tick: the native decoder hands over
// the frames it decoded on its own thread, and this is where they cross into
// JS.
function pump(decoder: VideoDecoder) {
	const i = _(decoder);
	if (i.closed) return;

	drainFrames(i);

	const state = $.webcodecsVideoDecoderState(i.handle);
	if (state.error && !i.errorReported) {
		// A decoder that failed is closed, and the page is told through the
		// error callback.
		i.errorReported = true;
		i.closed = true;
		stopPump(i);
		const error = new DOMException(state.error, 'InvalidStateError');
		$.webcodecsVideoDecoderClose(i.handle);
		rejectFlushes(i, error);
		try {
			i.error(error);
		} catch (err) {
			console.error('VideoDecoder: the error callback threw', err);
		}
		return;
	}

	if (i.flushes.length > 0 && state.flushDone) {
		// The decode thread reports "flushed" only after it has pushed the
		// last frame, but that frame may have landed after the drain above —
		// so drain once more and only resolve when nothing is left.
		if (drainFrames(i) === 0) {
			const flushes = i.flushes;
			i.flushes = [];
			for (const f of flushes) f.resolve();
		}
	}
}

/**
 * A W3C WebCodecs video decoder.
 *
 * The decoder turns compressed chunks into {@link VideoFrame}s. It is the
 * shape a page expects when it drives its own demuxer — a WebRTC receiver,
 * for instance — rather than handing a URL to the engine.
 *
 * ```typescript
 * const decoder = new VideoDecoder({
 * 	output: (frame) => {
 * 		ctx.drawImage(frame, 0, 0, canvas.width, canvas.height);
 * 		frame.close();
 * 	},
 * 	error: (err) => console.error(err),
 * });
 *
 * decoder.configure({ codec: 'hvc1.1.6.L93.B0', codedWidth: 1920, codedHeight: 1080 });
 * decoder.decode(new EncodedVideoChunk({ type: 'key', timestamp: 0, data: bytes }));
 * ```
 *
 * The chunks may be Annex-B (with start codes) or length-prefixed (AVCC);
 * the shape is detected per chunk, so both work.
 *
 * A raw stream carries no container, so two things follow from it:
 *
 * - the frame size is only knowable from the bitstream -- it shows up on
 *   {@link codedWidth} / {@link codedHeight} once the first key frame has
 *   been decoded (the SPS in it carries the size);
 * - the stream has no total duration at all. There is no "end of stream"
 *   to measure, so a page that wants one has to count the frames it gets
 *   (or ask the peer). Nothing here ever invents a duration.
 */
export class VideoDecoder {
	/** The decoder's current state. */
	declare readonly state: VideoDecoderState;
	/** The codec the decoder was configured with (`""` when unconfigured). */
	declare readonly codec: string;
	/**
	 * The coded width of the decoded frames, in pixels.
	 *
	 * 0 until the first frame has been decoded. A streamed feed (a WebRTC
	 * receiver, say) has no container to carry the size, so this is where
	 * the dimensions come from: the decoder parses them out of the
	 * parameter sets (the SPS) in the first key frame.
	 */
	declare readonly codedWidth: number;
	/**
	 * The coded height of the decoded frames, in pixels.
	 *
	 * @see {@link codedWidth}
	 */
	declare readonly codedHeight: number;
	/** The number of chunks handed to {@link VideoDecoder.decode}. */
	declare readonly numberOfFrames: number;
	/** The number of frames handed to the `output` callback. */
	declare readonly numberOfOutputFrames: number;
	/** The number of chunks waiting to be decoded. */
	declare readonly decodeQueueSize: number;

	constructor(init: VideoDecoderInit) {
		if (!init || typeof init.output !== 'function') {
			throw new TypeError('VideoDecoder: an output callback is required');
		}
		const decoder = proto($.webcodecsVideoDecoderNew(), VideoDecoder);
		_.set(decoder, {
			handle: decoder as unknown as VideoDecoderHandle,
			output: init.output,
			error:
				typeof init.error === 'function' ? init.error : () => undefined,
			closed: false,
			errorReported: false,
			pump: null,
			flushes: [],
		});
		const i = _(decoder);
		i.pump = setInterval(() => pump(decoder), 1);
		return decoder;
	}

	/**
	 * Configure the decoder. Must be called before the first
	 * {@link VideoDecoder.decode}.
	 *
	 * @throws a `DOMException` with the name `NotSupportedError` when the
	 * codec is not supported.
	 */
	configure(config: VideoDecoderConfig): void {
		const i = _(this);
		if (i.closed) {
			throw new DOMException(
				'The VideoDecoder is closed',
				'InvalidStateError',
			);
		}
		if ($.webcodecsVideoDecoderState(i.handle).state === 'configured') {
			throw new DOMException(
				'The VideoDecoder is already configured',
				'InvalidStateError',
			);
		}
		if (
			!config ||
			typeof config !== 'object' ||
			typeof config.codec !== 'string' ||
			config.codec.length === 0
		) {
			throw new TypeError("VideoDecoder.configure: 'codec' is required");
		}
		const description = config.description
			? bufferSourceToArrayBuffer(config.description)
			: null;
		const error = $.webcodecsVideoDecoderConfigure(
			i.handle,
			config.codec,
			config.codedWidth ?? 0,
			config.codedHeight ?? 0,
			description,
			config.hardwareAcceleration !== 'prefer-software',
			// ★ 2026-10-26: 7th argument — planar I420 output. Omitted/false
			//   keeps the old BGRA behaviour byte-for-byte.
			config.yuv === true,
		);
		if (error) {
			throw new DOMException(error, 'NotSupportedError');
		}
	}

	/**
	 * Queue a chunk for decoding. The frame it produces is handed to the
	 * `output` callback.
	 */
	decode(chunk: EncodedVideoChunk): void {
		const i = _(this);
		if (i.closed) {
			throw new DOMException(
				'The VideoDecoder is closed',
				'InvalidStateError',
			);
		}
		if ($.webcodecsVideoDecoderState(i.handle).state !== 'configured') {
			throw new DOMException(
				'The VideoDecoder is not configured',
				'InvalidStateError',
			);
		}
		const bytes = new Uint8Array(
			chunk.buffer,
			chunk.byteOffset,
			chunk.byteLength,
		);
		$.webcodecsVideoDecoderDecode(
			i.handle,
			bytes,
			chunk.type === 'key',
			chunk.timestamp,
			chunk.duration ?? 0,
		);
	}

	/**
	 * Wait until every chunk queued so far has been decoded and its frames
	 * handed to the `output` callback.
	 */
	flush(): Promise<void> {
		const i = _(this);
		if (i.closed) {
			return Promise.reject(
				new DOMException('The VideoDecoder is closed', 'InvalidStateError'),
			);
		}
		return new Promise<void>((resolve, reject) => {
			i.flushes.push({ resolve, reject });
			$.webcodecsVideoDecoderFlush(i.handle);
		});
	}

	/**
	 * Drop everything the decoder holds and return it to the `unconfigured`
	 * state.
	 */
	reset(): void {
		const i = _(this);
		if (i.closed) return;
		$.webcodecsVideoDecoderReset(i.handle);
		rejectFlushes(
			i,
			new DOMException('The VideoDecoder was reset', 'AbortError'),
		);
	}

	/**
	 * Close the decoder. The decoder cannot be used afterwards.
	 */
	close(): void {
		const i = _(this);
		if (i.closed) return;
		i.closed = true;
		stopPump(i);
		$.webcodecsVideoDecoderClose(i.handle);
		rejectFlushes(
			i,
			new DOMException('The VideoDecoder was closed', 'AbortError'),
		);
	}

	/**
	 * ★ 2026-10-26 (nx.js extension, not in W3C WebCodecs): read the
	 * decode-path diagnostics — whether the hardware arm was really used,
	 * which pixel format came out, the conversion cost and the decoded frame
	 * size. See {@link VideoDecoderStats}.
	 *
	 * This only *reads* native state; it never changes what the decoder does.
	 */
	stats(): VideoDecoderStats {
		const i = _(this);
		return $.webcodecsVideoDecoderState(i.handle);
	}

	/**
	 * Check whether a configuration is supported.
	 *
	 * This is how a page detects that the platform can decode a codec — the
	 * answer is `true` for every codec the engine's ffmpeg build carries,
	 * including H.265 (`hvc1.*` / `hev1.*`).
	 */
	static async isConfigSupported(
		config: VideoDecoderConfig,
	): Promise<VideoDecoderSupport> {
		if (
			!config ||
			typeof config !== 'object' ||
			typeof config.codec !== 'string' ||
			config.codec.length === 0
		) {
			throw new TypeError(
				"VideoDecoder.isConfigSupported: 'codec' is required",
			);
		}
		if ($.webcodecsIsConfigSupported(config.codec)) {
			return { supported: true, config };
		}
		return { supported: false };
	}
}

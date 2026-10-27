import { $, type VideoFrameHandle } from '../$';
import type { DOMHighResTimeStamp } from '../types';

/**
 * A decoded video frame.
 *
 * A frame is produced by a {@link VideoDecoder} and handed to its `output`
 * callback. It owns the decoded pixels and must be released with
 * {@link VideoFrame.close} once the page is done with it — until then the
 * pixels stay alive.
 *
 * A frame is drawable: it can be passed straight to a canvas 2D context.
 *
 * ```typescript
 * const decoder = new VideoDecoder({
 * 	output: (frame) => {
 * 		ctx.drawImage(frame, 0, 0, canvas.width, canvas.height);
 * 		frame.close();
 * 	},
 * });
 * ```
 *
 * Frames are created by the decoder, not by the page: the public constructor
 * is not supported.
 */
export class VideoFrame {
	/** The width of the frame's pixels, in pixels. */
	declare readonly codedWidth: number;
	/** The height of the frame's pixels, in pixels. */
	declare readonly codedHeight: number;
	/** The width the frame should be rendered at, in pixels. */
	declare readonly displayWidth: number;
	/** The height the frame should be rendered at, in pixels. */
	declare readonly displayHeight: number;
	/** Presentation timestamp, in microseconds. */
	declare readonly timestamp: DOMHighResTimeStamp;
	/** Duration in microseconds, or `null` when the frame carries none. */
	declare readonly duration: DOMHighResTimeStamp | null;
	/**
	 * The pixel format of the frame: `"BGRA"`, or `"I420"` when the decoder
	 * was configured with `yuv: true` (nx.js extension, 2026-10-26).
	 * Planar I420 frames are laid out tight as Y|U|V (1.5 B/px) and are
	 * accepted by `ctx.drawImage()` exactly like a BGRA frame.
	 */
	declare readonly format: string;

	constructor() {
		throw new TypeError(
			'VideoFrame: frames are created by a VideoDecoder, not directly',
		);
	}

	/**
	 * Release the frame's pixels. Calling it twice is harmless.
	 */
	close(): void {
		$.webcodecsVideoFrameClose(this as unknown as VideoFrameHandle);
	}
}

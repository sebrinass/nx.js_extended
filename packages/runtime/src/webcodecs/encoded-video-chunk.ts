import type { BufferSource } from '../types';

/**
 * The kind of frame an encoded chunk carries.
 *
 * - `key` — a key frame: the chunk can be decoded on its own.
 * - `delta` — a delta frame: it depends on the frames before it.
 */
export type EncodedVideoChunkType = 'key' | 'delta';

/**
 * The dictionary passed to the {@link EncodedVideoChunk} constructor.
 */
export interface EncodedVideoChunkInit {
	/** Whether the chunk is a key frame or a delta frame. */
	type: EncodedVideoChunkType;
	/** Presentation timestamp, in microseconds. */
	timestamp: number;
	/** Duration, in microseconds. Optional. */
	duration?: number | null;
	/** The encoded bytes. */
	data: BufferSource;
}

/**
 * A chunk of encoded video.
 *
 * This is the W3C WebCodecs shape: a chunk is an opaque blob of compressed
 * video plus the metadata the decoder needs to place it on a timeline. It
 * carries no codec knowledge — the codec is chosen when the
 * {@link VideoDecoder} is configured.
 *
 * ```typescript
 * const chunk = new EncodedVideoChunk({
 * 	type: 'key',
 * 	timestamp: 0,
 * 	data: bytes,
 * });
 * decoder.decode(chunk);
 * ```
 *
 * The bytes are kept by reference (no copy), so a chunk built from a view
 * into a larger buffer stays a view: {@link EncodedVideoChunk.buffer},
 * {@link EncodedVideoChunk.byteOffset} and
 * {@link EncodedVideoChunk.byteLength} describe where the payload sits.
 */
export class EncodedVideoChunk {
	/** Whether the chunk is a key frame or a delta frame. */
	readonly type: EncodedVideoChunkType;
	/** Presentation timestamp, in microseconds. */
	readonly timestamp: number;
	/** Duration in microseconds, or `null` when the chunk carries none. */
	readonly duration: number | null;
	/** The buffer holding the chunk's bytes. */
	readonly buffer: ArrayBuffer;
	/** The offset of the chunk's bytes inside {@link EncodedVideoChunk.buffer}. */
	readonly byteOffset: number;
	/** The length of the chunk's bytes. */
	readonly byteLength: number;

	constructor(init: EncodedVideoChunkInit) {
		if (!init || typeof init !== 'object') {
			throw new TypeError('EncodedVideoChunk: an init dictionary is required');
		}
		if (init.type !== 'key' && init.type !== 'delta') {
			throw new TypeError(
				"EncodedVideoChunk: 'type' must be either 'key' or 'delta'",
			);
		}
		if (typeof init.timestamp !== 'number' || !Number.isFinite(init.timestamp)) {
			throw new TypeError("EncodedVideoChunk: 'timestamp' must be a number");
		}
		const data = init.data as
			| ArrayBuffer
			| { buffer: ArrayBuffer; byteOffset: number; byteLength: number };
		if (data instanceof ArrayBuffer) {
			this.buffer = data;
			this.byteOffset = 0;
			this.byteLength = data.byteLength;
		} else if (
			data &&
			typeof data === 'object' &&
			data.buffer instanceof ArrayBuffer &&
			typeof data.byteOffset === 'number' &&
			typeof data.byteLength === 'number'
		) {
			this.buffer = data.buffer;
			this.byteOffset = data.byteOffset;
			this.byteLength = data.byteLength;
		} else {
			throw new TypeError(
				"EncodedVideoChunk: 'data' must be an ArrayBuffer or a TypedArray",
			);
		}
		this.type = init.type;
		this.timestamp = init.timestamp;
		this.duration =
			typeof init.duration === 'number' && Number.isFinite(init.duration)
				? init.duration
				: null;
	}

	/**
	 * The chunk's bytes, as a view over {@link EncodedVideoChunk.buffer}.
	 */
	get data(): Uint8Array {
		return new Uint8Array(this.buffer, this.byteOffset, this.byteLength);
	}

	/**
	 * Copy the chunk's bytes into `destination`.
	 *
	 * `destination` may be an `ArrayBuffer` or a view into one; at most
	 * `destination.byteLength` bytes are written.
	 */
	copyTo(destination: BufferSource): void {
		const src = this.data;
		if (destination instanceof ArrayBuffer) {
			const dst = new Uint8Array(destination);
			dst.set(src.subarray(0, Math.min(src.length, dst.length)));
			return;
		}
		const view = destination as unknown as {
			buffer: ArrayBuffer;
			byteOffset: number;
			byteLength: number;
		};
		const dst = new Uint8Array(
			view.buffer,
			view.byteOffset,
			view.byteLength,
		);
		dst.set(src.subarray(0, Math.min(src.length, dst.length)));
	}
}

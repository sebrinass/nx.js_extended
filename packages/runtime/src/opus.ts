import { $ } from './$';
import { getSharedAudioContext } from './audio';
import { ctxInternal, nodeInternal } from './audio/internal';
import { def } from './utils';

/**
 * A live Opus track (RFC 6716) fed with raw Opus packets.
 *
 * ## Why this exists
 *
 * The mihoyo cloud sends its game audio as an Opus track over RTP.  RFC 7587
 * (RTP payload format for Opus) says the RTP payload of an audio packet is
 * *exactly one* Opus packet, so a receiver can hand those bytes straight to
 * a decoder -- there is no container, no demuxer and no depacketizer.
 *
 * The engine could already decode Opus, but only when it sat inside a
 * container (`nx_media_decode_audio` handles MP3/WAV/OGG files).  A WebRTC
 * audio track is not a container, so this class feeds the packets to
 * ffmpeg's native Opus decoder directly and pushes the resulting PCM into
 * the engine's audio graph, from where it reaches the speakers.
 *
 * ## Example
 *
 * ```typescript
 * const track = new OpusTrack({ channels: 2 });
 * track.play();                       // route it to the speakers
 * track.push(opusPacketBytes);        // one RTP payload per call
 * console.log(track.stats.rms);       // how loud the stream is
 * ```
 */
export class OpusTrack {
	#handle: $.OpusTrackHandle | null = null;
	#channels: number;
	#node: $.AudioNodeHandle | null = null;
	#gain: unknown = null;
	#closed = false;

	/**
	 * Parse the TOC byte of one Opus packet (RFC 6716 section 3.2.1).
	 *
	 * @returns the packet's structure, or `null` when the bytes are not a
	 * valid Opus packet.
	 */
	static packetInfo(packet: Uint8Array | ArrayBuffer): OpusPacketInfo | null {
		return $.opusPacketInfo(packet);
	}

	constructor(options: { channels?: number } = {}) {
		this.#channels = options.channels ?? 2;
		const ctx = getSharedAudioContext();
		this.#handle = $.opusTrackNew(
			ctxInternal(ctx).handle,
			this.#channels,
		);
	}

	/**
	 * Feed one Opus packet (an RTP payload, RFC 7587).
	 *
	 * @returns the number of PCM frames (samples per channel) the packet
	 * decoded to.
	 */
	push(packet: Uint8Array | ArrayBuffer): number {
		if (this.#closed || !this.#handle) {
			throw new Error('the Opus track is closed');
		}
		return $.opusTrackPush(this.#handle, packet);
	}

	/**
	 * Route the decoded audio to the speakers.
	 *
	 * The PCM goes into a stream-source node of the shared AudioContext,
	 * through a gain node, into the destination.  Calling this twice is
	 * harmless.
	 */
	play(): void {
		if (this.#closed || !this.#handle) {
			throw new Error('the Opus track is closed');
		}
		if (this.#node) {
			$.opusTrackSetPlaying(this.#handle, true);
			return;
		}
		const ctx = getSharedAudioContext();
		const node = $.opusTrackNode(this.#handle);
		if (!node) {
			throw new Error('the Opus track has no audio node');
		}
		this.#node = node;
		const gain = ctx.createGain();
		gain.connect(ctx.destination);
		this.#gain = gain;
		$.audioNodeConnect(node, nodeInternal(gain).handle);
		$.opusTrackSetPlaying(this.#handle, true);
	}

	/** Stop the output (the track keeps decoding, nothing is played). */
	pause(): void {
		if (this.#closed || !this.#handle) return;
		$.opusTrackSetPlaying(this.#handle, false);
	}

	/**
	 * Copy the newest decoded PCM out of the track.
	 *
	 * `out` is filled with interleaved stereo float32 samples (the newest
	 * ones) and the number of stereo frames written is returned.  This is
	 * what a diagnostic page draws its waveform from.
	 */
	read(out: Float32Array): number {
		if (this.#closed || !this.#handle) {
			throw new Error('the Opus track is closed');
		}
		return $.opusTrackRead(this.#handle, out);
	}

	/**
	 * Whether the decoded audio is connected to the graph's destination.
	 */
	get connected(): boolean {
		return this.#node !== null;
	}

	/** Counters of the decoded stream. */
	get stats(): OpusTrackStats {
		if (this.#closed || !this.#handle) {
			return {
				packets: 0,
				frames: 0,
				rms: 0,
				peak: 0,
				errors: 0,
				sampleRate: 0,
				channels: this.#channels,
				graphRate: 0,
			};
		}
		return $.opusTrackStats(this.#handle);
	}

	/** Release the decoder and the graph node. */
	close(): void {
		if (this.#closed) return;
		this.#closed = true;
		this.#handle = null;
		this.#node = null;
		this.#gain = null;
	}
}

/** Counters of a decoded Opus stream. */
export interface OpusTrackStats {
	/** Opus packets fed to the decoder. */
	packets: number;
	/** PCM frames (samples per channel) produced. */
	frames: number;
	/** RMS of the decoded PCM (0..1). */
	rms: number;
	/** Peak of the decoded PCM (0..1+). */
	peak: number;
	/** Packets that could not be decoded. */
	errors: number;
	/** The decoder's sample rate -- always 48000 for Opus. */
	sampleRate: number;
	/** Channel count. */
	channels: number;
	/** The audio graph's rate (what the PCM is resampled to). */
	graphRate: number;
}

/**
 * The structure of one Opus packet (RFC 6716 section 3.2.1, the TOC byte).
 */
export interface OpusPacketInfo {
	/** TOC bits 7..3 -- the codec configuration. */
	config: number;
	/** TOC bits 2..0 -- the frame count code. */
	frameCode: number;
	/** How many Opus frames the packet carries. */
	frameCount: number;
	/** The duration of one frame, in tenths of a millisecond. */
	frameTenthsMs: number;
	/** The duration of the whole packet, in tenths of a millisecond. */
	totalTenthsMs: number;
	/** The samples the packet decodes to at 48 kHz. */
	samples: number;
	/** False for CELT, whose frame length (not the TOC) sets the duration. */
	exact: boolean;
}

/**
 * Parse the TOC byte of one Opus packet.
 *
 * @returns the packet's structure, or `null` when the bytes are not a
 * valid Opus packet.
 */
export function opusPacketInfo(packet: Uint8Array | ArrayBuffer):
	| OpusPacketInfo
	| null {
	return $.opusPacketInfo(packet);
}

def(OpusTrack);

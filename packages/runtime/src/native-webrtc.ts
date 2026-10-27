/**
 * Native WebRTC glue.
 *
 * Bridges the native libpeer binding (`$.webrtc*`, see source/webrtc.cc) to
 * the two surfaces the facade understands:
 *
 *  1. `globalThis.__nx_webrtc_peer_factory` — a factory returning an object
 *     with the facade's `NativePeer` shape (facade/src/native.ts). This is
 *     the PREFERRED surface: it hands DataChannel payloads over as real
 *     `Uint8Array`s, so the binary input packets survive the trip.
 *
 *  2. `globalThis.__nx_webrtc` — the `EngineWebrtcBinding` shape from
 *     docs/08 §6.2 (`onEvent(peerId, kind, payloadJson)`). Kept for
 *     compatibility with the documented design, but note that its JSON
 *     channel CANNOT carry binary: DataChannel payloads are base64-encoded
 *     here. The facade's JSON decoder has to base64-decode them.
 *
 * The facade's `loadNativeFactoryFromGlobal()` prefers (1) over (2), so the
 * base64 path is only taken if the peer factory is unavailable.
 *
 * Threading: `$.webrtcTick()` drives the libpeer state machine and MUST run
 * on the loop thread. It is called from a timer here, which the engine
 * implements on top of a libuv timer => always the loop thread.
 *
 * See docs/17-native绑定设计.md for the full design.
 */

import { $ } from './$';
import { clearInterval, setInterval } from './timers';

// ---------------------------------------------------------------------------
// Types (mirrored from facade/src/native.ts — keep them structurally equal)
// ---------------------------------------------------------------------------

export type NativeIceConnectionState =
	| 'new'
	| 'checking'
	| 'connected'
	| 'completed'
	| 'disconnected'
	| 'failed'
	| 'closed';

export type NativePeerConnectionState =
	| 'new'
	| 'connecting'
	| 'connected'
	| 'disconnected'
	| 'failed'
	| 'closed';

export interface NativeStats {
	rttMs?: number;
	packetsLost?: number;
	framesReceived?: number;
}

export type NativeEventKind =
	| 'icecandidate'
	| 'datachannel'
	| 'datachannelopen'
	| 'datachannelclose'
	| 'datachannelmessage'
	| 'track'
	| 'icestate'
	| 'connstate'
	| 'error';

type Payload = Record<string, unknown>;

interface PeerSink {
	onDataChannel?: (sid: number, label: string) => void;
	onDataChannelOpen?: (sid: number) => void;
	onDataChannelMessage?: (sid: number, data: Uint8Array) => void;
	onDataChannelClose?: (sid: number) => void;
	/**
	 * Media track callback.  `data` carries one access unit (video:
	 * H.264 Annex-B; audio: a raw Opus packet) on every media event.
	 */
	onTrack?: (
		kind: 'video' | 'audio',
		mediaType: string,
		sdpMid: string,
		data?: Uint8Array,
	) => void;
	onIceCandidate?: (
		candidate: string,
		sdpMid: string | null,
		sdpMLineIndex: number | null,
	) => void;
	onIceState?: (state: NativeIceConnectionState) => void;
	onConnState?: (state: NativePeerConnectionState) => void;
	onError?: (message: string) => void;
}

// ---------------------------------------------------------------------------
// Event dispatch
// ---------------------------------------------------------------------------

const sinks = new Map<number, PeerSink>();

function num(value: unknown): number {
	return typeof value === 'number' ? value : 0;
}

function str(value: unknown): string {
	return typeof value === 'string' ? value : '';
}

function dispatch(peerId: number, kind: string, payload: Payload): void {
	const sink = sinks.get(peerId);
	if (!sink) return;
	switch (kind as NativeEventKind) {
		case 'icecandidate':
			sink.onIceCandidate?.(
				str(payload['candidate']),
				typeof payload['sdpMid'] === 'string' ? payload['sdpMid'] : null,
				typeof payload['sdpMLineIndex'] === 'number'
					? payload['sdpMLineIndex']
					: null,
			);
			break;
		case 'datachannel':
			sink.onDataChannel?.(num(payload['sid']), str(payload['label']));
			break;
		case 'datachannelopen':
			sink.onDataChannelOpen?.(num(payload['sid']));
			break;
		case 'datachannelclose':
			sink.onDataChannelClose?.(num(payload['sid']));
			break;
		case 'datachannelmessage': {
			const data = payload['data'];
			const bytes =
				data instanceof ArrayBuffer
					? new Uint8Array(data)
					: data instanceof Uint8Array
						? data
						: new Uint8Array(0);
			sink.onDataChannelMessage?.(num(payload['sid']), bytes);
			break;
		}
		case 'track': {
			const media = payload['data'];
			const bytes =
				media instanceof ArrayBuffer
					? new Uint8Array(media)
					: media instanceof Uint8Array
						? media
						: undefined;
			sink.onTrack?.(
				payload['kind'] === 'audio' ? 'audio' : 'video',
				str(payload['mediaType']) || 'video',
				str(payload['sdpMid']) || '0',
				bytes,
			);
			break;
		}
		case 'icestate':
			sink.onIceState?.(str(payload['state']) as NativeIceConnectionState);
			break;
		case 'connstate':
			sink.onConnState?.(
				str(payload['state']) as NativePeerConnectionState,
			);
			break;
		case 'error':
			sink.onError?.(str(payload['message']) || 'native error');
			break;
		default:
			break;
	}
}

// ---------------------------------------------------------------------------
// Ticker
// ---------------------------------------------------------------------------

// The libpeer state machine is poll-based: somebody has to call
// `$.webrtcTick()` at a reasonable rate. 16 ms matches a 60 Hz frame.
let ticker: ReturnType<typeof setInterval> | null = null;

function ensureTicker(): void {
	if (ticker !== null) return;
	ticker = setInterval(() => {
		$.webrtcTick();
	}, 16);
}

function stopTickerIfIdle(): void {
	if (ticker === null) return;
	if (sinks.size > 0) return;
	clearInterval(ticker);
	ticker = null;
}

// ---------------------------------------------------------------------------
// NativePeer implementation (the facade's shape)
// ---------------------------------------------------------------------------

class NativePeerImpl {
	readonly peerId: number;
	#sink: PeerSink = {};
	#closed = false;

	constructor(peerId: number) {
		this.peerId = peerId;
		sinks.set(peerId, this.#sink);
		ensureTicker();
	}

	/** No-op: the native session is created eagerly by the factory. */
	create(): void {}

	setRemoteDescription(sdp: string, type: 'offer' | 'answer'): void {
		$.webrtcSetRemoteDescription(this.peerId, sdp, type === 'offer');
	}

	createAnswer(): string {
		return $.webrtcCreateAnswer(this.peerId);
	}

	/**
	 * 交一条 ICE 候选。
	 *
	 * ★ 2026-07-22 真机实测：**云端有时会推一条 TCP 候选**
	 *   （`a=candidate:... 1 tcp ... typ host tcptype active`），
	 *   而 libpeer **只支持 UDP**（`extern/libpeer/src/ice.c`：
	 *   "Only UDP transport is supported"）⇒ 原先这里**直接抛**。
	 *
	 * ⇒ 抛出来的后果**不是"这一条候选没加上"**，而是：
	 *   ① 异常冒成 rejected promise ⇒ SDK 不 catch ⇒ **unhandledrejection**；
	 *   ② native peer 报错 ⇒ ICE 从 `connected` 退回 `checking` → `failed`
	 *      ⇒ **整条连接挂掉** —— 而那时 **UDP 候选其实已经连上了**。
	 * ⇒ 所以这里改成：**不支持的传输直接跳过并记日志**，不抛。
	 *   （真错误仍然要抛 —— 静默吞掉会掩盖别的问题。）
	 *
	 * ⚠️ 官方 SDK 也会 addIceCandidate 那条 TCP 候选 ⇒ 官方页不做同样的容错
	 *   就会**以完全一样的方式死掉** ⇒ 这不是自研客户端独有的坑。
	 */
	addIceCandidate(candidate: string): void {
		// 候选的传输在第 3 个字段：`a=candidate:<foundation> <component> <transport> …`
		const transport = /^\s*a=candidate:[^\s]+\s+\d+\s+([A-Za-z]+)/.exec(candidate)?.[1]?.toLowerCase();
		if (transport !== undefined && transport !== 'udp') {
			// 跳过：不支持的传输（libpeer 只有 UDP）—— 记一行，别让整条连接陪葬。
			console.warn(
				`[webrtc] addIceCandidate: 跳过不支持的传输 "${transport}"（libpeer 只支持 UDP）`,
			);
			return;
		}
		try {
			$.webrtcAddIceCandidate(this.peerId, candidate);
		} catch (error) {
			// native 侧也可能因为传输不支持而抛（ice.c 的那句）⇒ 同样跳过。
			const message = String((error as { message?: unknown })?.message ?? error);
			if (/only udp/i.test(message)) {
				console.warn(
					`[webrtc] addIceCandidate: 跳过 native 拒绝的候选（${message}）`,
				);
				return;
			}
			throw error;
		}
	}

	close(): void {
		if (this.#closed) return;
		this.#closed = true;
		sinks.delete(this.peerId);
		$.webrtcClosePeer(this.peerId);
		stopTickerIfIdle();
	}

	/** The SDP the page SDK munged (docs/08 §4.4 risk 3). */
	setLocalDescription(sdp: string, _type: 'offer' | 'answer'): void {
		$.webrtcSetLocalDescription(this.peerId, sdp);
	}

	/**
	 * Tell the backend that the ANSWER went out to the cloud (patch 0037).
	 *
	 * Until this is called no ICE connectivity check leaves: the cloud cannot
	 * answer a check that arrives before it has processed our answer (it does
	 * not know our ICE credentials yet) and drops it.  The first check used
	 * to leave one frame after createAnswer() -- before the answer had been
	 * sent at all -- and the next one only 100 frames (~3.3 s) later.  That
	 * is the "sometimes it connects after 3 seconds" number.
	 *
	 * The page (through the facade) calls this right after it has sent the
	 * answer over the signalling channel.  If it never does, the backend
	 * starts the check anyway after a short fallback, so the connection still
	 * comes up.
	 */
	notifyAnswerSent(): void {
		$.webrtcNotifyAnswerSent(this.peerId);
	}

	onDataChannel(cb: (sid: number, label: string) => void): void {
		this.#sink.onDataChannel = cb;
	}

	sendDataChannel(sid: number, data: Uint8Array): void {
		// Copy into a fresh ArrayBuffer: the view may be a window into a
		// larger buffer, and the native side wants the exact bytes.
		const copy = new Uint8Array(data.byteLength);
		copy.set(data);
		$.webrtcDcSend(this.peerId, sid, copy.buffer);
	}

	onDataChannelOpen(cb: (sid: number) => void): void {
		this.#sink.onDataChannelOpen = cb;
	}

	onDataChannelMessage(cb: (sid: number, data: Uint8Array) => void): void {
		this.#sink.onDataChannelMessage = cb;
	}

	onDataChannelClose(cb: (sid: number) => void): void {
		this.#sink.onDataChannelClose = cb;
	}

	onTrack(
		cb: (kind: 'video' | 'audio', mediaType: string, sdpMid: string) => void,
	): void {
		this.#sink.onTrack = cb;
	}

	onIceCandidate(
		cb: (
			candidate: string,
			sdpMid: string | null,
			sdpMLineIndex: number | null,
		) => void,
	): void {
		this.#sink.onIceCandidate = cb;
	}

	onIceStateChange(cb: (state: NativeIceConnectionState) => void): void {
		this.#sink.onIceState = cb;
	}

	onConnectionStateChange(cb: (state: NativePeerConnectionState) => void): void {
		this.#sink.onConnState = cb;
	}

	onError(cb: (message: string) => void): void {
		this.#sink.onError = cb;
	}

	/** libpeer only knows H264 / G.711; the SDK's codec preferences are a
	 * facade-level concern and are not forwarded. */
	setCodecPreferences(_kind: 'video' | 'audio', _mimeTypes: string[]): void {}

	getStats(): NativeStats {
		try {
			return JSON.parse($.webrtcGetStats(this.peerId)) as NativeStats;
		} catch {
			return {};
		}
	}
}

// ---------------------------------------------------------------------------
// docs/08 §6.2 compatible JSON binding
// ---------------------------------------------------------------------------

function base64(bytes: Uint8Array): string {
	let binary = '';
	for (let i = 0; i < bytes.length; i++) {
		binary += String.fromCharCode(bytes[i] as number);
	}
	return btoa(binary);
}

function toJsonPayload(kind: string, payload: Payload): string {
	const out: Record<string, unknown> = { ...payload };
	if (kind === 'datachannelmessage' || kind === 'track') {
		const data = payload['data'];
		if (data instanceof ArrayBuffer) {
			out['data'] = base64(new Uint8Array(data));
			out['dataEncoding'] = 'base64';
		}
	}
	try {
		return JSON.stringify(out);
	} catch {
		return '{}';
	}
}

/**
 * The docs/08 §6.2 surface. Note the JSON channel cannot carry binary
 * without base64 (see the module comment).
 */
const jsonBinding = {
	createPeer(configJson: string): number {
		let urls: string[] = [];
		let usernames: string[] = [];
		let credentials: string[] = [];
		try {
			const cfg = JSON.parse(configJson || '{}') as {
				iceServers?: { urls?: string | string[]; username?: string; credential?: string }[];
			};
			for (const server of cfg.iceServers ?? []) {
				const list = Array.isArray(server.urls)
					? server.urls
					: server.urls
						? [server.urls]
						: [];
				for (const url of list) {
					urls.push(url);
					usernames.push(server.username ?? '');
					credentials.push(server.credential ?? '');
				}
			}
		} catch {
			// An unparsable config just means "no ICE servers".
		}
		return $.webrtcCreatePeer(urls, usernames, credentials);
	},
	destroyPeer(peerId: number): void {
		$.webrtcDestroyPeer(peerId);
	},
	setRemoteDescription(peerId: number, sdpJson: string): void {
		const { sdp, type } = JSON.parse(sdpJson || '{}') as {
			sdp?: string;
			type?: string;
		};
		$.webrtcSetRemoteDescription(peerId, sdp ?? '', type === 'offer');
	},
	createAnswer(peerId: number): string {
		return $.webrtcCreateAnswer(peerId);
	},
	setLocalDescription(peerId: number, sdp: string): void {
		$.webrtcSetLocalDescription(peerId, sdp);
	},
	/**
	 * Tell the backend that the ANSWER went out to the cloud (patch 0037).
	 *
	 * The facade calls this right after it has sent the answer over the
	 * signalling channel.  Until then no ICE connectivity check leaves: the
	 * cloud drops a check that arrives before it has processed our answer.
	 */
	notifyAnswerSent(peerId: number): void {
		$.webrtcNotifyAnswerSent(peerId);
	},
	addIceCandidate(peerId: number, candidateJson: string): void {
		const { candidate } = JSON.parse(candidateJson || '{}') as {
			candidate?: string;
		};
		$.webrtcAddIceCandidate(peerId, candidate ?? '');
	},
	closePeer(peerId: number): void {
		$.webrtcClosePeer(peerId);
	},
	getStats(peerId: number): string {
		return $.webrtcGetStats(peerId);
	},
	dcSend(peerId: number, dcId: number, data: ArrayBuffer): void {
		$.webrtcDcSend(peerId, dcId, data);
	},
	dcClose(peerId: number, dcId: number): void {
		$.webrtcDcClose(peerId, dcId);
	},
	onEvent(
		cb: (peerId: number, kind: string, payloadJson: string) => void,
	): void {
		$.webrtcOnEvent((peerId, kind, payload) => {
			cb(peerId, kind, toJsonPayload(kind, payload));
		});
	},
};

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

/** Install the native WebRTC surfaces onto `globalThis`. */
export function installNativeWebrtc(): void {
	const g = globalThis as unknown as Record<string, unknown>;

	// docs/08 §6.2 surface (JSON events).
	g['__nx_webrtc'] = jsonBinding;

	// Preferred surface: binary-safe, matches facade/src/native.ts.
	g['__nx_webrtc_peer_factory'] = (config?: {
		iceServers?: unknown[];
		sdpSemantics?: string;
	}): NativePeerImpl => {
		const urls: string[] = [];
		const usernames: string[] = [];
		const credentials: string[] = [];
		for (const server of (config?.iceServers ?? []) as {
			urls?: string | string[];
			username?: string;
			credential?: string;
		}[]) {
			const list = Array.isArray(server.urls)
				? server.urls
				: server.urls
					? [server.urls]
					: [];
			for (const url of list) {
				urls.push(url);
				usernames.push(server.username ?? '');
				credentials.push(server.credential ?? '');
			}
		}
		const peerId = $.webrtcCreatePeer(urls, usernames, credentials);
		return new NativePeerImpl(peerId);
	};

	// The single native -> JS event hook.
	$.webrtcOnEvent((peerId, kind, payload) => {
		dispatch(peerId, kind, payload);
	});
}

installNativeWebrtc();

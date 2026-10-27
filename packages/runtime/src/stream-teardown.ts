/**
 * Guarded teardown for `ReadableStream` / `WritableStream`.
 *
 * ## Why this module exists (engine patch 0036)
 *
 * `ReadableStream.cancel()` and `WritableStream.abort()` do **not** throw when
 * the stream is locked — they return a **rejected promise**:
 *
 * ```
 * cancel()  ->  Promise.reject(new TypeError("Cannot cancel a stream that already has a reader"))
 * abort()   ->  Promise.reject(new TypeError("Cannot abort a stream that already has a writer"))
 * ```
 *
 * That is the behaviour of `web-streams-polyfill@4.2.0` (the implementation the
 * engine ships — see `packages/runtime/src/polyfills/streams.ts`) and it matches
 * the WHATWG spec ("If stream.[[locked]] is true, return a promise rejected with
 * a TypeError").
 *
 * A caller that drops that promise on the floor turns an ordinary `abort()` into
 * a **global `unhandledrejection`**, which the engine's error guard records as a
 * crash. Device capture (2026-10-23 13:58, see `patches/0036-NOTES.md`):
 *
 * ```
 * Uncaught (in promise) TypeError: Cannot cancel a stream that already has a reader
 *     at cancel (node_modules/.pnpm/web-streams-polyfill@4.2.0/.../ponyfill.mjs:8:49322)
 *     at req.signal.addEventListener.once (nxjs:src/fetch/fetch.ts:209:20)
 * ```
 *
 * The helpers below therefore:
 *
 *   1. **skip** the call when the stream is `locked` — the reader/writer owns
 *      the stream, and there is nothing to cancel or abort; and
 *   2. **attach a rejection handler** to the returned promise, so a late
 *      failure (e.g. the stream is already errored) can never become an
 *      unhandled rejection.
 *
 * Failures are reported at **debug** level (the engine's debug log file,
 * `sdmc:/switch/nxjs-debug.log`) and never at error level: aborting a request
 * is a normal path, and a failed teardown must not become an error storm of its
 * own.
 */

/** A readable stream that can be cancelled. */
export interface CancellableStream {
	readonly locked: boolean;
	cancel(reason?: any): Promise<void>;
}

/** A writable stream that can be aborted. */
export interface AbortableStream {
	readonly locked: boolean;
	abort(reason?: any): Promise<void>;
}

/** The part of a socket that an abort has to tear down. */
export interface AbortableSocket {
	readonly readable: CancellableStream;
	readonly writable: AbortableStream;
	close(reason?: any): unknown;
}

/** Human-readable form of a thrown / rejected value. */
function describeError(err: unknown): string {
	if (err instanceof Error) return `${err.name}: ${err.message}`;
	if (typeof err === 'string') return err;
	try {
		return String(err);
	} catch {
		return '<unprintable error>';
	}
}

/**
 * Write one line to the engine's debug log.
 *
 * `console.debug` goes to the debug log file (stderr), **not** to the on-screen
 * console, so a failed teardown never becomes visible noise. Logging must never
 * break the teardown itself, hence the blanket try/catch.
 */
function debugLog(message: string): void {
	try {
		if (typeof console !== 'undefined' && typeof console.debug === 'function') {
			console.debug(message);
		}
	} catch {
		/* ignore */
	}
}

/**
 * Cancel a readable stream, if it can be cancelled.
 *
 * A locked stream is skipped: its reader owns it, and cancelling it would only
 * produce a rejected promise (see the module docs).
 *
 * @param stream The readable to cancel.
 * @param reason The abort reason handed to the underlying source.
 * @param label Log prefix, e.g. `"[fetch] abort"`.
 */
export function cancelReadable(
	stream: CancellableStream,
	reason: any,
	label: string,
): void {
	if (stream.locked) return;
	try {
		const p = stream.cancel(reason);
		// `cancel()` rejects instead of throwing when it cannot cancel — never
		// leave that promise unhandled.
		Promise.resolve(p).catch((err) => {
			debugLog(`${label}: readable.cancel failed: ${describeError(err)}`);
		});
	} catch (err) {
		debugLog(`${label}: readable.cancel failed: ${describeError(err)}`);
	}
}

/**
 * Abort a writable stream, if it can be aborted.
 *
 * A locked stream is skipped: its writer owns it, and aborting it would only
 * produce a rejected promise (see the module docs).
 *
 * @param stream The writable to abort.
 * @param reason The abort reason handed to the underlying sink.
 * @param label Log prefix, e.g. `"[fetch] abort"`.
 */
export function abortWritable(
	stream: AbortableStream,
	reason: any,
	label: string,
): void {
	if (stream.locked) return;
	try {
		const p = stream.abort(reason);
		// `abort()` rejects instead of throwing when it cannot abort — never
		// leave that promise unhandled.
		Promise.resolve(p).catch((err) => {
			debugLog(`${label}: writable.abort failed: ${describeError(err)}`);
		});
	} catch (err) {
		debugLog(`${label}: writable.abort failed: ${describeError(err)}`);
	}
}

/**
 * Tear down a socket after an abort.
 *
 * This is the teardown `fetch()` runs when its `AbortSignal` fires. It must
 * never throw and never leave a rejected promise behind: the abort happens on a
 * timer callback, so anything escaping here turns into a global error.
 *
 * @param socket The socket to tear down.
 * @param reason The abort reason.
 * @param label Log prefix, e.g. `"[fetch] abort"`.
 */
export function abortSocket(
	socket: AbortableSocket,
	reason: any,
	label = '[fetch] abort',
): void {
	cancelReadable(socket.readable, reason, label);
	abortWritable(socket.writable, reason, label);
	try {
		socket.close();
	} catch (err) {
		debugLog(`${label}: socket.close failed: ${describeError(err)}`);
	}
}

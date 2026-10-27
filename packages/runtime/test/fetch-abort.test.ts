/**
 * Regression test — engine patch 0036.
 *
 * `fetch()`'s abort handler used to call `socket.readable.cancel(err)` and
 * `socket.writable.abort(err)` unconditionally. Once the response is being
 * read, `socket.readable` is locked (the body reader owns it), and cancelling
 * a locked stream does **not** throw — it returns a **rejected promise**:
 *
 * ```
 * TypeError: Cannot cancel a stream that already has a reader
 * ```
 *
 * Dropping that promise turned every aborted request into a global
 * `unhandledrejection`, which the engine's error guard records as a crash
 * (device capture 2026-10-23 13:58 — see `patches/0036-NOTES.md`).
 *
 * These tests exercise the real teardown helper (`src/stream-teardown.ts`)
 * against the real stream implementation the engine ships:
 * `web-streams-polyfill`, the same package `src/polyfills/streams.ts` installs
 * as the global `ReadableStream` / `WritableStream`.
 */

import { ReadableStream, WritableStream } from 'web-streams-polyfill';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { abortSocket, type AbortableSocket } from '../src/stream-teardown';

/** Unhandled rejections raised while the current test runs. */
const unhandled: unknown[] = [];
const onUnhandled = (reason: unknown) => {
	unhandled.push(reason);
};

beforeEach(() => {
	unhandled.length = 0;
	process.on('unhandledRejection', onUnhandled);
});

afterEach(() => {
	process.off('unhandledRejection', onUnhandled);
	vi.restoreAllMocks();
});

/** Give the microtask queue (and Node's rejection reporter) a turn. */
const settle = () => new Promise((resolve) => setTimeout(resolve, 10));

/** A writable that never fails. */
const okWritable = () => ({
	locked: false,
	abort: () => Promise.resolve(),
});

describe('fetch abort teardown (patch 0036)', () => {
	it('does not throw and does not leak an unhandled rejection when the readable is locked', async () => {
		const readable = new ReadableStream({
			start() {},
		});
		// The body is being read => the readable is locked. This is the state
		// the engine is in when the request times out.
		readable.getReader();
		const writable = new WritableStream({
			write() {},
		});
		// The request writer may still be held when the abort lands.
		writable.getWriter();

		expect(readable.locked).toBe(true);
		expect(writable.locked).toBe(true);

		const close = vi.fn();
		const socket: AbortableSocket = {
			readable,
			writable,
			close,
		};

		expect(() => abortSocket(socket, new Error('aborted'))).not.toThrow();

		await settle();
		expect(unhandled).toEqual([]);
		expect(close).toHaveBeenCalledTimes(1);
	});

	it('reproduces the pre-fix failure: cancel() on a locked stream rejects', async () => {
		const readable = new ReadableStream({
			start() {},
		});
		readable.getReader();

		// This is what the old code did: the returned promise was dropped.
		await expect(readable.cancel(new Error('aborted'))).rejects.toThrow(
			/already has a reader/,
		);

		await settle();
		expect(unhandled).toEqual([]);
	});

	it('never calls cancel()/abort() on a locked stream', async () => {
		const readable = new ReadableStream({
			start() {},
		});
		readable.getReader();
		const writable = new WritableStream({
			write() {},
		});
		writable.getWriter();

		const cancel = vi.spyOn(readable, 'cancel');
		const abort = vi.spyOn(writable, 'abort');

		abortSocket(
			{
				readable,
				writable,
				close: () => undefined,
			},
			new Error('aborted'),
		);

		expect(cancel).not.toHaveBeenCalled();
		expect(abort).not.toHaveBeenCalled();

		await settle();
		expect(unhandled).toEqual([]);
	});

	it('cancels an unlocked readable with the abort reason', async () => {
		const readable = new ReadableStream({
			start() {},
		});
		const cancel = vi.spyOn(readable, 'cancel');
		const reason = new Error('aborted');

		abortSocket(
			{
				readable,
				writable: okWritable(),
				close: () => undefined,
			},
			reason,
		);

		expect(cancel).toHaveBeenCalledWith(reason);

		await settle();
		expect(unhandled).toEqual([]);
	});

	it('swallows a rejected cancel() and reports it at debug level', async () => {
		const debug = vi.spyOn(console, 'debug').mockImplementation(() => {});
		const socket: AbortableSocket = {
			readable: {
				locked: false,
				cancel: () => Promise.reject(new TypeError('already errored')),
			},
			writable: okWritable(),
			close: () => undefined,
		};

		expect(() => abortSocket(socket, new Error('aborted'))).not.toThrow();

		await settle();
		expect(unhandled).toEqual([]);
		expect(debug).toHaveBeenCalledTimes(1);
		expect(debug.mock.calls[0]?.[0]).toContain('readable.cancel failed');
	});

	it('swallows a synchronous throw from cancel()', async () => {
		const debug = vi.spyOn(console, 'debug').mockImplementation(() => {});
		const socket: AbortableSocket = {
			readable: {
				locked: false,
				cancel: () => {
					throw new TypeError('sync boom');
				},
			},
			writable: okWritable(),
			close: () => undefined,
		};

		expect(() => abortSocket(socket, new Error('aborted'))).not.toThrow();

		await settle();
		expect(unhandled).toEqual([]);
		expect(debug.mock.calls[0]?.[0]).toContain('readable.cancel failed');
	});

	it('swallows a rejected abort() on the writable', async () => {
		const debug = vi.spyOn(console, 'debug').mockImplementation(() => {});
		const socket: AbortableSocket = {
			readable: {
				locked: false,
				cancel: () => Promise.resolve(),
			},
			writable: {
				locked: false,
				abort: () => Promise.reject(new TypeError('already errored')),
			},
			close: () => undefined,
		};

		expect(() => abortSocket(socket, new Error('aborted'))).not.toThrow();

		await settle();
		expect(unhandled).toEqual([]);
		expect(debug.mock.calls[0]?.[0]).toContain('writable.abort failed');
	});

	it('survives the full engine chain: AbortSignal -> listener -> teardown', async () => {
		const readable = new ReadableStream({
			start() {},
		});
		readable.getReader();
		const writable = new WritableStream({
			write() {},
		});
		writable.getWriter();

		const close = vi.fn();
		const socket: AbortableSocket = {
			readable,
			writable,
			close,
		};

		// Same shape as the engine: the shell's XHR polyfill calls
		// `AbortController.abort()`, which dispatches an "abort" event on the
		// signal; the listener registered by `fetch()` tears the socket down.
		const controller = new AbortController();
		controller.signal.addEventListener(
			'abort',
			() => {
				abortSocket(socket, controller.signal.reason);
			},
			{ once: true },
		);

		expect(() => controller.abort()).not.toThrow();

		await settle();
		expect(unhandled).toEqual([]);
		expect(close).toHaveBeenCalledTimes(1);
	});
});

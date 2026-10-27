/**
 * Keep-alive connection pool for `fetch()`.
 *
 * ## Why a pool exists (device evidence)
 *
 * `fetch()` used to open a brand-new TCP connection for every request and
 * throw it away afterwards (body-less requests even asked the server to close
 * with `Connection: close`). On the Switch the bsdsocket sysmodule draws
 * socket buffers from a fixed shared pool, and roughly 90-100 rapid
 * connect/close cycles exhaust it: subsequent requests fail with ENOBUFS
 * ("No buffer space available") while unrelated long-lived sockets keep
 * working (device report 2026-10-24 19:58, first failure at poll 87; a full
 * browser survives the same workload because it reuses connections).
 *
 * The pool keeps HTTP/1.1 keep-alive connections alive, keyed by
 * `(protocol, hostname, port)`:
 *
 *  - a request whose response body framing completed hands the connection
 *    back (`release()`) instead of closing it;
 *  - the next request to the same target borrows it (`acquire()`) instead of
 *    opening a new socket, including TLS connections (no re-handshake);
 *  - idle connections run a small watchdog read that detects a peer which
 *    closed or sent unsolicited (framing-breaking) bytes, so a dead
 *    connection never poisons a later borrow silently;
 *  - a borrowed connection that fails before producing a single response byte
 *    is retried once on a fresh connection, transparently to the caller.
 *
 * Reuse rules: only responses whose body length is exactly known and fully
 * consumed (Content-Length, or chunked up to the terminal chunk with no
 * residual bytes) make a connection reusable. A `Connection: close` response
 * header, a body cancelled before its framing completed, an unknown body
 * length, or leftover bytes after a chunked terminal chunk all destroy the
 * connection instead.
 *
 * All native I/O goes through an injectable {@link FetchPoolDeps} facade so
 * host tests can drive the pool with a fake transport.
 */

import { $ } from '../$';
import { connect as tcpConnect, type TlsContextOpaque } from '../tcp';
import { toPromise } from '../utils';

/** Physical I/O primitives the pool needs. Defaults bind to the native `$`. */
export interface FetchPoolDeps {
	/** Open a TCP connection, returning its fd. */
	connect(address: { hostname: string; port: number }): Promise<number>;
	tlsHandshake(
		fd: number,
		hostname: string,
		rejectUnauthorized: boolean,
	): Promise<TlsContextOpaque>;
	tlsRead(ctx: TlsContextOpaque, buffer: ArrayBuffer): Promise<number>;
	tlsWrite(ctx: TlsContextOpaque, data: ArrayBuffer): Promise<number>;
	tlsClose(ctx: TlsContextOpaque): void;
	read(fd: number, buffer: ArrayBuffer): Promise<number>;
	write(fd: number, data: ArrayBuffer): Promise<number>;
	closeFd(fd: number): void;
	/** Native tcp_rx_buf_size; sizes the per-connection read scratch buffer. */
	readonly tcpRxBufSize: number;
	/** Wall clock, injectable so tests can drive idle timers deterministically. */
	now(): number;
}

/** Default deps bind to the engine's native socket/TLS facade. */
export function defaultFetchPoolDeps(): FetchPoolDeps {
	return {
		connect: (address) => tcpConnect(address),
		tlsHandshake: (fd, hostname, rejectUnauthorized) =>
			toPromise($.tlsHandshake, fd, hostname, rejectUnauthorized),
		tlsRead: (ctx, buffer) => toPromise($.tlsRead, ctx, buffer),
		tlsWrite: (ctx, data) => toPromise($.tlsWrite, ctx, data),
		tlsClose: (ctx) => $.tlsClose(ctx),
		read: (fd, buffer) => toPromise($.read, fd, buffer),
		write: (fd, data) => toPromise($.write, fd, data),
		closeFd: (fd) => $.close(fd),
		// Read lazily: the native value is only known after socketInitialize().
		get tcpRxBufSize() {
			return $.tcpRxBufSize || 1024 * 1024;
		},
		now: () => Date.now(),
	};
}

/** Pool tuning. The per-host cap bounds live (active + idle) connections. */
export interface ConnectionPoolOptions {
	maxPerHost?: number;
	maxTotal?: number;
	idleTimeoutMs?: number;
	activeStaleMs?: number;
	deps?: FetchPoolDeps;
}

/** Defaults: 4 per host, 16 total, 15s idle timeout, 60s abandoned-body reap. */
export const FETCH_POOL_MAX_PER_HOST = 4;
export const FETCH_POOL_MAX_TOTAL = 16;
export const FETCH_POOL_IDLE_TIMEOUT_MS = 15_000;
export const FETCH_POOL_ACTIVE_STALE_MS = 60_000;
/** Interval of the diagnostics summary line + inactive-connection sweep. */
export const FETCH_POOL_STATS_INTERVAL_MS = 60_000;
/** Watchdog read buffer for idle connections (EOF / stray-byte detector). */
const WATCHDOG_READ_BYTES = 4096;

export type PoolKey = string;

export function poolKeyFor(
	protocol: string,
	hostname: string,
	port: number,
): PoolKey {
	return `${protocol}//${hostname}:${port}`;
}

export interface PoolTarget {
	hostname: string;
	port: number;
	secure: boolean;
	rejectUnauthorized?: boolean;
}

export interface FetchPoolStats {
	/** Physical connections opened (pool misses). */
	created: number;
	/** Borrows served by an existing connection (pool hits). */
	reused: number;
	/** Physical teardowns, any reason. */
	closed: number;
	/** Closed because the idle timeout elapsed. */
	expiredIdle: number;
	/** Closed because the peer closed or sent stray bytes while idle. */
	stalePeer: number;
	/** Requests retried once on a fresh connection after a dead borrow. */
	retriedStale: number;
	/** Live connections right now (connecting + active + idle). */
	live: number;
	/** Idle (pooled) connections right now. */
	idle: number;
	/** Acquires waiting for a free slot right now. */
	queued: number;
}

export type ConnectionState = 'connecting' | 'active' | 'idle' | 'closed';

interface PoolHooks {
	onClosed(conn: PooledConnection): void;
}

/**
 * One physical TCP (optionally TLS) connection.
 *
 * Owns its fd / TLS context directly (it deliberately does NOT use the
 * `Socket` class, whose streams are single-use and which always closes its
 * fd on teardown - both incompatible with pooling). Exactly one read is
 * outstanding at a time: the same read slot serves the idle watchdog and,
 * via adoption, the first foreground read after a borrow.
 */
export class PooledConnection {
	readonly key: PoolKey;
	readonly hostname: string;
	readonly port: number;
	readonly secure: boolean;

	state: ConnectionState = 'connecting';
	destroyed = false;
	/** Set for the current borrow: this connection came from the pool. */
	wasReused = false;
	/** Response bytes read during the current borrow (stale-retry gate). */
	borrowBytes = 0;
	/** Response body framing fully consumed (set by the fetch framing hooks). */
	framingDone = false;
	/** Protocol + headers allow reuse (set by the fetch framing hooks). */
	reusable = false;
	/** No stray bytes after the body end (chunked trailers poison this). */
	residualClean = true;
	/** Last request/response activity, for the inactive-connection sweep. */
	lastActivity = 0;
	/** Why the connection failed, surfaced to readers raced against it. */
	failReason: unknown = null;

	/** Errors the current borrow's body stream with the failure reason. */
	onRequestError?: (err: unknown) => void;

	/* internal bookkeeping */
	fd = -1;
	tls?: TlsContextOpaque;
	/** False once the fd is handed to the native TLS context (it owns it). */
	fdOwned = false;
	idleTimer?: ReturnType<typeof setTimeout>;
	/** Borrow generation; bumped by the pool on every activate(). */
	generation = 0;
	/** The single allowed in-flight read, tagged with its generation. */
	pendingRead: Promise<number> | null = null;
	pendingReadGen = -1;
	pendingBuffer: ArrayBuffer | null = null;
	readBuffer?: ArrayBuffer;
	/**
	 * Bytes a stale-generation reader deposited for the current one. A read
	 * started before a re-borrow resolves into whichever generation owns the
	 * connection NOW; this carries its bytes across the handoff.
	 */
	pendingData: Uint8Array[] = [];
	/** Consumers parked because a foreign-generation read is in flight. */
	readQueue: Array<PromiseWithResolvers<Uint8Array | null>> = [];
	failed: PromiseWithResolvers<never>;

	constructor(
		key: PoolKey,
		target: PoolTarget,
		private readonly deps: FetchPoolDeps,
		private readonly hooks: PoolHooks,
	) {
		this.key = key;
		this.hostname = target.hostname;
		this.port = target.port;
		this.secure = target.secure;
		this.failed = Promise.withResolvers<never>();
		// A rejection with no `Promise.race` attached (no reader at that
		// moment) must never surface as an unhandled rejection.
		this.failed.promise.catch(() => {});
	}

	/** Start the single allowed outstanding read into `buf`. */
	startRead(buf: ArrayBuffer): Promise<number> {
		const p = this.tls
			? this.deps.tlsRead(this.tls, buf)
			: this.deps.read(this.fd, buf);
		this.pendingRead = p;
		this.pendingReadGen = this.generation;
		this.pendingBuffer = buf;
		return p;
	}

	/**
	 * Read the next chunk from the connection. Returns `null` on EOF.
	 *
	 * The connection allows exactly ONE physical read in flight, and a read
	 * may outlive the borrow that started it (the response pipe pre-pulls a
	 * chunk ahead of the consumer, and the connection can be released and
	 * re-borrowed while that read is still parked). The rules:
	 *
	 *  - bytes deposited by a stale generation are consumed first;
	 *  - a read belonging to the CURRENT generation is adopted;
	 *  - a foreign-generation read parks the caller on `readQueue`; whoever
	 *    consumes the physical read deposits its bytes and wakes the queue.
	 */
	async readChunk(): Promise<Uint8Array | null> {
		if (this.destroyed) {
			throw this.failReason ?? new Error('[fetch-pool] connection closed');
		}
		if (this.pendingData.length > 0) {
			const chunk = this.pendingData.shift()!;
			this.borrowBytes += chunk.byteLength;
			this.lastActivity = this.deps.now();
			return chunk;
		}
		const myGen = this.generation;
		if (this.pendingRead) {
			if (this.pendingReadGen === myGen) {
				// Same generation: adopt the in-flight read.
				const buf = this.pendingBuffer!;
				const n = await Promise.race([
					this.pendingRead,
					this.failed.promise,
				]);
				this.clearReadSlot();
				return this.afterRead(n, buf);
			}
			// Foreign generation owns the in-flight read; wait for its bytes.
			const waiter = Promise.withResolvers<Uint8Array | null>();
			this.readQueue.push(waiter);
			const chunk = await Promise.race([waiter.promise, this.failed.promise]);
			if (chunk) {
				this.borrowBytes += chunk.byteLength;
				this.lastActivity = this.deps.now();
			}
			if (this.destroyed && chunk !== null) {
				throw this.failReason ?? new Error('[fetch-pool] connection closed');
			}
			return chunk;
		}
		if (!this.readBuffer) {
			// Same sizing rule as the Socket class: match the native
			// tcp_rx_buf_size so applet mode does not over-allocate.
			this.readBuffer = new ArrayBuffer(this.deps.tcpRxBufSize);
		}
		const buf = this.readBuffer;
		const readPromise = this.startRead(buf);
		const n = await Promise.race([readPromise, this.failed.promise]);
		this.clearReadSlot();
		const chunk = this.afterRead(n, buf);
		if (myGen !== this.generation || this.state !== 'active') {
			// This borrow no longer owns the connection: hand the bytes to
			// whoever owns it now instead of consuming them here.
			if (chunk) {
				this.pendingData.push(chunk);
				this.flushReadQueue();
			}
			return chunk; // the stale caller discards it
		}
		return chunk;
	}

	private afterRead(n: number, buf: ArrayBuffer): Uint8Array | null {
		if (n === 0) {
			this.readBuffer = undefined; // EOF: release the large buffer promptly
			return null;
		}
		this.borrowBytes += n;
		this.lastActivity = this.deps.now();
		return new Uint8Array(buf.slice(0, n));
	}

	private clearReadSlot(): void {
		this.pendingRead = null;
		this.pendingReadGen = -1;
		this.pendingBuffer = null;
	}

	/** Hand deposited bytes to a parked consumer (FIFO). */
	private flushReadQueue(): void {
		const waiter = this.readQueue.shift();
		if (waiter) {
			waiter.resolve(this.pendingData.shift() ?? null);
		}
	}

	/** Write everything, looping over short writes (non-blocking send). */
	async writeBytes(data: Uint8Array): Promise<void> {
		if (this.destroyed) {
			throw this.failReason ?? new Error('[fetch-pool] connection closed');
		}
		let offset = 0;
		while (offset < data.byteLength) {
			const view = data.subarray(offset);
			const ab =
				view.byteOffset === 0 && view.byteLength === view.buffer.byteLength
					? view.buffer
					: view.slice().buffer;
			const op = this.tls
				? this.deps.tlsWrite(this.tls, ab)
				: this.deps.write(this.fd, ab);
			const n = await Promise.race([op, this.failed.promise]);
			if (this.destroyed) {
				throw this.failReason ?? new Error('[fetch-pool] connection closed');
			}
			if (!(n > 0)) {
				throw new Error('[fetch-pool] write made no progress');
			}
			offset += n;
			this.lastActivity = this.deps.now();
		}
	}

	/** Fail the connection: destroy it and unblock any in-flight read/write. */
	fail(err: unknown): void {
		if (this.destroyed) return;
		this.failReason = err;
		// Reject BEFORE destroy so a pending read's Promise.race sees the real
		// reason (tlsClose would otherwise settle it with a bare EOF).
		this.failed.reject(err);
		this.destroy();
		const hook = this.onRequestError;
		if (hook) {
			try {
				hook(err);
			} catch {
				// The body stream may already be closed/errored - benign.
			}
		}
	}

	/** Tear the physical connection down (idempotent). */
	destroy(): void {
		if (this.destroyed) return;
		this.destroyed = true;
		this.state = 'closed';
		if (this.idleTimer) {
			clearTimeout(this.idleTimer);
			this.idleTimer = undefined;
		}
		if (this.tls) {
			// The native TLS context owns the fd; it also settles any pending
			// read/write with 0 so awaiting promises never hang.
			this.deps.tlsClose(this.tls);
			this.tls = undefined;
		} else if (this.fdOwned && this.fd >= 0) {
			this.deps.closeFd(this.fd);
		}
		this.fd = -1;
		this.pendingRead = null;
		this.pendingReadGen = -1;
		this.pendingBuffer = null;
		this.readBuffer = undefined;
		// Wake parked consumers with EOF so their body streams close instead
		// of hanging on a dead connection.
		const waiters = this.readQueue.splice(0);
		for (const waiter of waiters) waiter.resolve(null);
		this.pendingData = [];
		this.hooks.onClosed(this);
	}
}

interface Waiter {
	key: PoolKey;
	target: PoolTarget;
	resolve(conn: PooledConnection, reused: boolean): void;
	fail(err: unknown): void;
	/** Create started for this waiter (dispatch) - needed for cancel. */
	pendingConn: PooledConnection | null;
}

export interface ConnectionAcquire {
	promise: Promise<PooledConnection>;
	/**
	 * Cancel a queued acquire or an in-flight connect; after the acquire
	 * resolved, fails the handed-out connection. Used for AbortSignal while
	 * the connection is still exclusively owned by the borrower.
	 */
	cancel(reason: unknown): void;
	/**
	 * Cancel ONLY the not-yet-settled acquire (queued / connecting). After
	 * the acquire resolved this is a no-op: the handed-out connection may
	 * already serve another request, and a late abort must not kill it.
	 */
	cancelIfPending(reason: unknown): void;
}

export class ConnectionPool {
	readonly maxPerHost: number;
	readonly maxTotal: number;
	readonly idleTimeoutMs: number;
	readonly activeStaleMs: number;
	private readonly deps: FetchPoolDeps;

	private idle = new Map<PoolKey, PooledConnection[]>();
	private waiters: Waiter[] = [];
	private activeSet = new Set<PooledConnection>();
	private livePerHost = new Map<PoolKey, number>();
	private liveTotal = 0;
	private disposed = false;
	private statsTimer: ReturnType<typeof setTimeout> | null = null;

	private counts: FetchPoolStats = {
		created: 0,
		reused: 0,
		closed: 0,
		expiredIdle: 0,
		stalePeer: 0,
		retriedStale: 0,
		live: 0,
		idle: 0,
		queued: 0,
	};

	constructor(options: ConnectionPoolOptions = {}) {
		this.maxPerHost = options.maxPerHost ?? FETCH_POOL_MAX_PER_HOST;
		this.maxTotal = options.maxTotal ?? FETCH_POOL_MAX_TOTAL;
		this.idleTimeoutMs = options.idleTimeoutMs ?? FETCH_POOL_IDLE_TIMEOUT_MS;
		this.activeStaleMs = options.activeStaleMs ?? FETCH_POOL_ACTIVE_STALE_MS;
		this.deps = options.deps ?? defaultFetchPoolDeps();
	}

	stats(): FetchPoolStats {
		let idle = 0;
		for (const q of this.idle.values()) idle += q.length;
		return {
			created: this.counts.created,
			reused: this.counts.reused,
			closed: this.counts.closed,
			expiredIdle: this.counts.expiredIdle,
			stalePeer: this.counts.stalePeer,
			retriedStale: this.counts.retriedStale,
			live: this.liveTotal,
			idle,
			queued: this.waiters.length,
		};
	}

	/** Incremented by the fetch layer when a dead borrow was retried. */
	countStaleRetry(): void {
		this.counts.retriedStale++;
	}

	acquire(key: PoolKey, target: PoolTarget): ConnectionAcquire {
		if (this.disposed) {
			return {
				promise: Promise.reject(new Error('[fetch-pool] pool disposed')),
				cancel: () => {},
				cancelIfPending: () => {},
			};
		}
		let settled = false;
		let handedConn: PooledConnection | null = null;
		let resolveFn!: (conn: PooledConnection) => void;
		let rejectFn!: (err: unknown) => void;
		const promise = new Promise<PooledConnection>((res, rej) => {
			resolveFn = res;
			rejectFn = rej;
		});
		const settleWith = (conn: PooledConnection, reused: boolean) => {
			if (settled) return;
			settled = true;
			handedConn = conn;
			this.activate(conn, reused);
			resolveFn(conn);
		};
		const failWith = (err: unknown) => {
			if (settled) return;
			settled = true;
			rejectFn(err);
		};
		const waiter: Waiter = {
			key,
			target,
			resolve: (conn, reused) => settleWith(conn, reused),
			fail: failWith,
			pendingConn: null,
		};

		// 1. Borrow an idle connection (LIFO: freshest first).
		const queue = this.idle.get(key);
		if (queue && queue.length > 0) {
			const conn = queue.pop()!;
			if (queue.length === 0) this.idle.delete(key);
			if (conn.idleTimer) {
				clearTimeout(conn.idleTimer);
				conn.idleTimer = undefined;
			}
			settleWith(conn, true);
		} else if (
			(this.livePerHost.get(key) ?? 0) < this.maxPerHost &&
			this.liveTotal < this.maxTotal
		) {
			// 2. Capacity for a new physical connection.
			waiter.pendingConn = this.beginCreate(
				key,
				target,
				(conn) => settleWith(conn, false),
				failWith,
			);
		} else {
			// 3. Queue until a release/close frees a slot.
			this.waiters.push(waiter);
		}

		const cancelPendingOnly = (reason: unknown) => {
			if (settled) return;
			const wi = this.waiters.indexOf(waiter);
			if (wi >= 0) this.waiters.splice(wi, 1);
			const pending = waiter.pendingConn;
			if (pending && !pending.destroyed) pending.fail(reason);
			failWith(reason);
		};

		return {
			promise,
			cancel: (reason: unknown) => {
				if (settled) {
					if (handedConn && !handedConn.destroyed) handedConn.fail(reason);
					return;
				}
				cancelPendingOnly(reason);
			},
			cancelIfPending: cancelPendingOnly,
		};
	}

	/**
	 * Hand an active connection back to the pool. Only valid when the response
	 * body framing completed and the connection is protocol-clean; anything
	 * else destroys the connection.
	 */
	release(conn: PooledConnection): void {
		if (conn.destroyed) return;
		if (conn.state !== 'active') {
			conn.destroy();
			return;
		}
		this.activeSet.delete(conn);
		if (!conn.framingDone || !conn.reusable || !conn.residualClean) {
			conn.destroy();
			return;
		}
		// Direct handoff to a same-key waiter skips the idle dwell entirely.
		const wi = this.waiters.findIndex((w) => w.key === conn.key);
		if (wi >= 0) {
			const waiter = this.waiters[wi];
			this.waiters.splice(wi, 1);
			waiter.resolve(conn, true);
			return;
		}
		conn.state = 'idle';
		conn.lastActivity = this.deps.now();
		const queue = this.idle.get(conn.key);
		if (queue) queue.push(conn);
		else this.idle.set(conn.key, [conn]);
		this.armIdleWatchdog(conn);
		conn.idleTimer = setTimeout(() => {
			conn.idleTimer = undefined;
			if (conn.state === 'idle') {
				this.counts.expiredIdle++;
				conn.destroy();
			}
		}, this.idleTimeoutMs);
		// A queued acquire for another host may now be serviceable by evicting
		// an idle connection when only the global cap was blocking it.
		this.dispatch();
	}

	/** Close every connection and stop timers. Mainly for tests. */
	dispose(): void {
		this.disposed = true;
		if (this.statsTimer) {
			clearTimeout(this.statsTimer);
			this.statsTimer = null;
		}
		for (const conn of [...this.activeSet]) conn.destroy();
		for (const queue of [...this.idle.values()]) {
			for (const conn of [...queue]) conn.destroy();
		}
		this.idle.clear();
		this.waiters = [];
	}

	/* ------------------------------------------------------------------ */

	private activate(conn: PooledConnection, reused: boolean): void {
		conn.state = 'active';
		conn.generation++;
		conn.wasReused = reused;
		conn.borrowBytes = 0;
		conn.framingDone = false;
		conn.reusable = false;
		conn.residualClean = true;
		// The previous borrow's error hook belongs to a finished request
		// stream; the new borrower installs its own.
		conn.onRequestError = undefined;
		// NOTE: pendingData is intentionally kept - bytes deposited by a
		// stale generation belong to this borrow (e.g. a response that
		// arrived across the release/borrow handoff).
		conn.lastActivity = this.deps.now();
		if (reused) this.counts.reused++;
		this.activeSet.add(conn);
	}

	private beginCreate(
		key: PoolKey,
		target: PoolTarget,
		settle: (conn: PooledConnection) => void,
		fail: (err: unknown) => void,
	): PooledConnection {
		const conn = new PooledConnection(key, target, this.deps, {
			onClosed: (c) => this.onConnClosed(c),
		});
		// Track it from birth so dispose() reaches in-flight connects too.
		this.activeSet.add(conn);
		// Reserve capacity synchronously so concurrent acquires cannot overshoot.
		this.liveTotal++;
		this.livePerHost.set(key, (this.livePerHost.get(key) ?? 0) + 1);
		this.counts.created++;
		this.ensureStatsTimer();
		void (async () => {
			try {
				const fd = await this.deps.connect({
					hostname: target.hostname,
					port: target.port,
				});
				if (conn.destroyed) {
					// Cancelled while connecting: the fresh fd is ours, close it.
					this.deps.closeFd(fd);
					return;
				}
				conn.fd = fd;
				conn.fdOwned = true;
				if (target.secure) {
					// The native TLS context takes ownership of the fd (and its
					// poll handle); it must then be closed via tlsClose only.
					conn.fdOwned = false;
					const tls = await this.deps.tlsHandshake(
						fd,
						target.hostname,
						target.rejectUnauthorized ?? true,
					);
					if (conn.destroyed) {
						this.deps.tlsClose(tls);
						return;
					}
					conn.tls = tls;
				}
				conn.state = 'active';
				conn.lastActivity = this.deps.now();
				settle(conn);
			} catch (err) {
				conn.destroy();
				fail(err);
				this.dispatch();
			}
		})();
		return conn;
	}

	private onConnClosed(conn: PooledConnection): void {
		this.liveTotal--;
		const n = (this.livePerHost.get(conn.key) ?? 1) - 1;
		if (n <= 0) this.livePerHost.delete(conn.key);
		else this.livePerHost.set(conn.key, n);
		this.activeSet.delete(conn);
		const queue = this.idle.get(conn.key);
		if (queue) {
			const i = queue.indexOf(conn);
			if (i >= 0) {
				queue.splice(i, 1);
				if (queue.length === 0) this.idle.delete(conn.key);
			}
		}
		this.counts.closed++;
		this.dispatch();
	}

	/** Free slots -> start creates for queued acquires (global FIFO). */
	private dispatching = false;
	private dispatch(): void {
		if (this.disposed || this.dispatching || this.waiters.length === 0) return;
		this.dispatching = true;
		try {
			for (let i = 0; i < this.waiters.length; ) {
				const waiter = this.waiters[i];
				if ((this.livePerHost.get(waiter.key) ?? 0) >= this.maxPerHost) {
					i++;
					continue;
				}
				if (this.liveTotal >= this.maxTotal) {
					// The global cap is full. Idle connections of OTHER hosts
					// are of no use to this waiter: evict the least-recently-
					// used idle connection to make room (browser behaviour).
					const victim = this.findIdleVictim();
					if (!victim) {
						i++;
						continue;
					}
					victim.destroy(); // onConnClosed -> dispatch() no-ops (guard)
					continue; // re-evaluate this waiter with the freed slot
				}
				this.waiters.splice(i, 1);
				waiter.pendingConn = this.beginCreate(
					waiter.key,
					waiter.target,
					(conn) => waiter.resolve(conn, false),
					waiter.fail,
				);
			}
		} finally {
			this.dispatching = false;
		}
	}

	/** Least-recently-used idle connection across all hosts, if any. */
	private findIdleVictim(): PooledConnection | null {
		let victim: PooledConnection | null = null;
		let oldest = Infinity;
		for (const queue of this.idle.values()) {
			for (const conn of queue) {
				if (conn.lastActivity < oldest) {
					oldest = conn.lastActivity;
					victim = conn;
				}
			}
		}
		return victim;
	}

	/**
	 * While idle, keep exactly one small read parked on the connection: a
	 * resolution means the peer closed (EOF) or sent bytes we did not ask for
	 * (framing desync, e.g. trailers the chunked parser did not consume) -
	 * either way the connection must never be handed out again.
	 *
	 * A foreground read may still be parked when the connection is released
	 * (the response pipe pre-pulls one chunk ahead of the consumer). In that
	 * case the watchdog must NOT start a second read (that would clobber the
	 * single read slot and orphan the parked one); it attaches to the parked
	 * read instead, which then doubles as the watchdog. Its bytes flow to the
	 * next borrower through the deposit queue.
	 */
	private armIdleWatchdog(conn: PooledConnection): void {
		const onSettle = () => {
			if (conn.state !== 'idle' || conn.destroyed) return;
			this.counts.stalePeer++;
			conn.destroy();
		};
		if (conn.pendingRead) {
			void conn.pendingRead.then(onSettle, onSettle);
			return;
		}
		const buf = new ArrayBuffer(WATCHDOG_READ_BYTES);
		conn.startRead(buf).then(onSettle, onSettle);
	}

	/**
	 * One self-rescheduling timer serving two duties every 60s: emit the
	 * pool statistics line (console.error is the only channel that reaches
	 * the engine's debug log on device), and reap active connections whose
	 * last I/O is older than `activeStaleMs`. This covers a response body
	 * that was never consumed (no cancel, no read - its read may sit parked
	 * on the socket): without the reap such connections would hold their
	 * per-host slots forever and eventually deadlock new acquires. A 60s
	 * stretch without a single byte is treated as a dead response; a
	 * consumer attached to the body learns about it through the stream
	 * erroring.
	 */
	private ensureStatsTimer(): void {
		if (this.statsTimer || this.disposed) return;
		const tick = () => {
			this.statsTimer = null;
			if (this.disposed) return;
			const now = this.deps.now();
			for (const conn of [...this.activeSet]) {
				if (
					conn.state === 'active' &&
					now - conn.lastActivity > this.activeStaleMs
				) {
					conn.fail(
						new Error('[fetch-pool] active connection reaped after inactivity'),
					);
				}
			}
			const s = this.stats();
			try {
				console.error(
					`[fetch-pool] live=${s.live} idle=${s.idle} queued=${s.queued} ` +
						`created=${s.created} reused=${s.reused} closed=${s.closed} ` +
						`expiredIdle=${s.expiredIdle} stalePeer=${s.stalePeer} ` +
						`retriedStale=${s.retriedStale}`,
				);
			} catch {
				// Never let diagnostics break the pool.
			}
			this.statsTimer = setTimeout(tick, FETCH_POOL_STATS_INTERVAL_MS);
		};
		this.statsTimer = setTimeout(tick, FETCH_POOL_STATS_INTERVAL_MS);
	}
}

export function createConnectionPool(
	options: ConnectionPoolOptions = {},
): ConnectionPool {
	return new ConnectionPool(options);
}

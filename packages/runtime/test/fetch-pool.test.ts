/**
 * Connection-pool tests for `fetch()`.
 *
 * The pool (`src/fetch/pool.ts`) exists to kill the device ENOBUFS wall:
 * ~90-100 rapid connect/close cycles used to exhaust the Switch bsdsocket
 * buffer pool (device report 2026-10-24 19:58, first failure at poll 87).
 * These tests pin the pool contract on the host with a fake transport
 * injected through `FetchPoolDeps`, so no real network is touched:
 *
 *  - reuse: same-target requests borrow the same physical connection;
 *  - caps: per-host (4) and total (16) live-connection limits queue instead
 *    of opening more sockets;
 *  - idle expiry + watchdog: pooled connections that time out or whose peer
 *    closed are discarded;
 *  - stale-borrow retry: a borrowed connection that dies before its first
 *    response byte is retried once on a fresh connection, transparently;
 *  - framing: only fully-delimited bodies (Content-Length, chunked terminal
 *    chunk, no-body statuses) are reusable; `Connection: close`, early body
 *    cancellation and overruns destroy the connection;
 *  - concurrency: borrows / returns / cancels interleaved stay consistent.
 *
 * Mutations (see delivery notes): disabling reuse or the caps turns the
 * corresponding tests red.
 */

// The engine reads its native facade from `globalThis.$` at module-eval time
// (tcp.ts calls `$.tcpServerInit` at import). Install a magic stub BEFORE the
// dynamic imports below; the pool under test never uses it (tests inject
// FetchPoolDeps), but the module graph needs it to evaluate.
const magic: any = new Proxy(function () {}, {
	get(_t: any, prop: string | symbol) {
		if (prop === Symbol.toPrimitive) return () => 0;
		if (prop === 'toString') return () => '';
		if (prop === Symbol.iterator) return function* () {};
		return magic;
	},
	apply() {
		return magic;
	},
	construct() {
		return magic;
	},
});
/**
 * `$.urlNew` must return an object with WORKING href/protocol/... accessors
 * that survive the engine's `proto(obj, URL)` re-prototyping, so back it with
 * Node's WHATWG URL copied into own properties. Node's classes are captured
 * BEFORE the engine import below, because the engine installs its own
 * polyfills over the globals (`def(URL)`).
 */
const NodeURL = globalThis.URL;
const NodeURLSearchParams = globalThis.URLSearchParams;
const NodeTextDecoder = globalThis.TextDecoder;

// The engine's device globals (navigator.userAgent -> Application.self ->
// addEventListener('unload', ...)) do not exist under Node.
(globalThis as any).addEventListener ??= () => {};
(globalThis as any).removeEventListener ??= () => {};

function fakeUrlNew(input: string | URL, base?: string | URL): object {
	let u: URL;
	try {
		const baseStr =
			base === undefined || base === null ? undefined : String(base);
		u = baseStr
			? new NodeURL(String(input), baseStr)
			: new NodeURL(String(input));
	} catch {
		// A garbage base (magic stubs stringify to '0') must not break an
		// absolute input.
		u = new NodeURL(String(input));
	}
	const obj: any = {};
	for (const key of [
		'hash',
		'host',
		'hostname',
		'href',
		'password',
		'pathname',
		'port',
		'protocol',
		'search',
		'username',
		'origin',
	]) {
		Object.defineProperty(obj, key, {
			enumerable: true,
			get() {
				return (u as any)[key];
			},
			set(v: string) {
				(u as any)[key] = v;
			},
		});
	}
	return obj;
}

(globalThis as any).$ = new Proxy(
	{
		tcpRxBufSize: 16384,
		urlNew: fakeUrlNew,
		urlSearchNew: (input?: any) => new NodeURLSearchParams(input ?? ''),
		// The engine's TextDecoder delegates to this native fast path; back it
		// with Node so header / chunk-size parsing produces real strings.
		textDecode: (bytes: Uint8Array, encoding: string) =>
			new NodeTextDecoder(encoding, { fatal: false }).decode(bytes),
	},
	{
		get(target: any, prop: string) {
			if (prop in target) return target[prop];
			return () => magic;
		},
	},
);

const { fetch, setFetchConnectionPool, getFetchPoolStats } = await import(
	'../src/fetch/fetch'
);
const { createConnectionPool } = await import('../src/fetch/pool');
type ConnectionPool = import('../src/fetch/pool').ConnectionPool;

import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

/* ------------------------------------------------------------------------ */
/* Fake transport                                                            */
/* ------------------------------------------------------------------------ */

const encoder = new TextEncoder();

function textBytes(s: string): Uint8Array {
	return encoder.encode(s);
}

function concatAll(parts: Uint8Array[]): Uint8Array {
	const total = parts.reduce((n, p) => n + p.byteLength, 0);
	const out = new Uint8Array(total);
	let offset = 0;
	for (const p of parts) {
		out.set(p, offset);
		offset += p.byteLength;
	}
	return out;
}

interface HttpResponseOptions {
	status?: string;
	/** Text body; sends a Content-Length header. */
	body?: string;
	/** Chunked body pieces, auto-framed up to the terminal chunk. */
	chunkedBody?: string[];
	/** Raw extra header lines, e.g. ['connection: close']. */
	headers?: string[];
}

function httpResponse(opts: HttpResponseOptions = {}): Uint8Array {
	const parts: Uint8Array[] = [];
	// Header block first: status, Content-Length (plain bodies), extra
	// headers, then the blank line that ends the headers.
	parts.push(textBytes(`${opts.status ?? 'HTTP/1.1 200 OK'}\r\n`));
	if (opts.chunkedBody) {
		parts.push(textBytes('transfer-encoding: chunked\r\n'));
	} else if (opts.body !== undefined) {
		const b = textBytes(opts.body);
		parts.push(textBytes(`content-length: ${b.byteLength}\r\n`));
	}
	for (const h of opts.headers ?? []) parts.push(textBytes(`${h}\r\n`));
	parts.push(textBytes('\r\n'));
	// Body block after the headers.
	if (opts.chunkedBody) {
		for (const piece of opts.chunkedBody) {
			const b = textBytes(piece);
			parts.push(textBytes(`${b.byteLength.toString(16)}\r\n`));
			parts.push(b);
			parts.push(textBytes('\r\n'));
		}
		parts.push(textBytes('0\r\n\r\n'));
	} else if (opts.body !== undefined) {
		parts.push(textBytes(opts.body));
	}
	return concatAll(parts);
}

interface FakeFd {
	id: number;
	secure: boolean;
	hostname: string;
	port: number;
	/** Bytes the "server" has queued for the next JS reads. */
	chunks: Uint8Array[];
	readWaiters: Array<() => void>;
	eof: boolean;
	closedByJs: boolean;
	/** Bytes JS wrote (whole requests, for assertions). */
	received: Uint8Array[];
	tlsCtx?: { fd: number };
	staleOnNextWrite: boolean;
}

interface RouteCtx {
	fd: number;
	secure: boolean;
	hostname: string;
	port: number;
}

type RouteHandler = (ctx: RouteCtx) => void;

class FakeNet {
	deps: any;
	fds = new Map<number, FakeFd>();
	nextFd = 1;
	connectCalls = 0;
	tlsHandshakeCalls = 0;
	closedFds: number[] = [];
	routes = new Map<string, RouteHandler>();
	/** Every JS write, decoded, in order. */
	written: string[] = [];
	/** Hosts whose connect() rejects (DNS / connect failure path). */
	failHosts = new Set<string>();
	currentTime = 1_000_000;

	constructor() {
		const self = this;
		this.deps = {
			async connect({ hostname, port }: { hostname: string; port: number }) {
				if (self.failHosts.has(hostname)) {
					throw new Error(`[fake-net] connect to ${hostname} refused`);
				}
				self.connectCalls++;
				const fd = self.nextFd++;
				self.fds.set(fd, {
					id: fd,
					secure: false,
					hostname,
					port,
					chunks: [],
					readWaiters: [],
					eof: false,
					closedByJs: false,
					received: [],
					staleOnNextWrite: false,
				});
				return fd;
			},
			async tlsHandshake(fd: number, _hostname: string, _ru: boolean) {
				self.tlsHandshakeCalls++;
				const f = self.mustFd(fd);
				f.secure = true;
				f.tlsCtx = { fd };
				return f.tlsCtx;
			},
			read(fd: number, buf: ArrayBuffer) {
				return self.readFd(self.mustFd(fd), buf);
			},
			async write(fd: number, data: ArrayBuffer) {
				const f = self.mustFd(fd);
				f.received.push(new Uint8Array(data.slice(0)));
				self.written.push(new TextDecoder().decode(data));
				await self.dispatchRequest(f);
				return data.byteLength;
			},
			tlsRead(ctx: { fd: number }, buf: ArrayBuffer) {
				return self.readFd(self.mustFd(ctx.fd), buf);
			},
			async tlsWrite(ctx: { fd: number }, data: ArrayBuffer) {
				const f = self.mustFd(ctx.fd);
				f.received.push(new Uint8Array(data.slice(0)));
				self.written.push(new TextDecoder().decode(data));
				await self.dispatchRequest(f);
				return data.byteLength;
			},
			tlsClose(ctx: { fd: number }) {
				const f = self.fds.get(ctx.fd);
				if (f) {
					f.closedByJs = true;
					// TLS parity: nx_tls_close settles pending reads with 0.
					self.flushWaiters(f);
				}
			},
			closeFd(fd: number) {
				const f = self.fds.get(fd);
				if (f) f.closedByJs = true;
				// TCP parity: nx_tcp_close drops pending ops, whose promises
				// then never settle. Nothing else to do here.
				self.closedFds.push(fd);
			},
			tcpRxBufSize: 16384,
			now: () => self.currentTime,
		};
	}

	private mustFd(fd: number): FakeFd {
		const f = this.fds.get(fd);
		if (!f) throw new Error(`[fake-net] unknown fd ${fd}`);
		return f;
	}

	private readFd(f: FakeFd, buf: ArrayBuffer): Promise<number> {
		const tryRead = (): number | null => {
			if (f.chunks.length > 0) {
				const chunk = f.chunks[0];
				const n = Math.min(chunk.byteLength, buf.byteLength);
				new Uint8Array(buf).set(chunk.subarray(0, n));
				if (n < chunk.byteLength) f.chunks[0] = chunk.subarray(n);
				else f.chunks.shift();
				return n;
			}
			if (f.eof) return 0;
			return null;
		};
		const immediate = tryRead();
		if (immediate !== null) return Promise.resolve(immediate);
		return new Promise((resolve) => {
			f.readWaiters.push(() => {
				const n = tryRead();
				resolve(n === null ? 0 : n);
			});
		});
	}

	private flushWaiters(f: FakeFd): void {
		const waiters = f.readWaiters.splice(0);
		for (const w of waiters) w(0);
	}

	route(origin: string, handler: RouteHandler): void {
		this.routes.set(origin, handler);
	}

	/** Server pushes response bytes to a fd. */
	respond(fd: number, bytes: Uint8Array): void {
		const f = this.mustFd(fd);
		f.chunks.push(bytes);
		this.flushWaiters(f);
	}

	/** Server closes the connection (FIN) - parked reads see EOF. */
	peerClose(fd: number): void {
		const f = this.mustFd(fd);
		f.eof = true;
		this.flushWaiters(f);
	}

	/** Next request written on this fd is answered with a close instead. */
	staleOnNextWrite(fd: number): void {
		this.mustFd(fd).staleOnNextWrite = true;
	}

	private async dispatchRequest(f: FakeFd): Promise<void> {
		if (f.staleOnNextWrite) {
			f.staleOnNextWrite = false;
			this.peerClose(f.id);
			return;
		}
		const origin = `${f.secure ? 'https' : 'http'}://${f.hostname}:${f.port}`;
		const handler = this.routes.get(origin);
		if (!handler) return;
		handler({ fd: f.id, secure: f.secure, hostname: f.hostname, port: f.port });
	}
}

/* ------------------------------------------------------------------------ */
/* Harness                                                                   */
/* ------------------------------------------------------------------------ */

let currentNet: FakeNet | null = null;
let currentPool: ConnectionPool | null = null;

function setup(): FakeNet {
	const net = new FakeNet();
	const pool = createConnectionPool({ deps: net.deps });
	setFetchConnectionPool(pool);
	currentNet = net;
	currentPool = pool;
	return net;
}

/** Let acquired connections, pipes and pool bookkeeping settle. */
const settle = () => new Promise((resolve) => setTimeout(resolve, 25));

const unhandled: unknown[] = [];
const onUnhandled = (reason: unknown) => unhandled.push(reason);

beforeEach(() => {
	unhandled.length = 0;
	process.on('unhandledRejection', onUnhandled);
});

afterEach(async () => {
	currentPool?.dispose();
	currentPool = null;
	currentNet = null;
	// Restore a pristine default pool for the next test.
	setFetchConnectionPool(createConnectionPool());
	vi.useRealTimers();
	vi.restoreAllMocks();
	process.off('unhandledRejection', onUnhandled);
	await settle();
	expect(unhandled).toEqual([]);
});

/** Standard keep-alive route that answers every request identically. */
function autoRoute(
	net: FakeNet,
	origin: string,
	response: Uint8Array = httpResponse({ body: 'ok' }),
): void {
	net.route(origin, ({ fd }) => net.respond(fd, response));
}

/** Route that holds every request fd until the test answers it. */
function holdRoute(net: FakeNet, origin: string): { held: number[] } {
	const held: number[] = [];
	net.route(origin, ({ fd }) => held.push(fd));
	return { held };
}

/* ------------------------------------------------------------------------ */
/* Tests                                                                     */
/* ------------------------------------------------------------------------ */

describe('fetch connection pool', () => {
	it('reuses one connection for sequential same-origin requests', async () => {
		const net = setup();
		let served = 0;
		net.route('http://x.test:80', ({ fd }) => {
			served++;
			net.respond(fd, httpResponse({ body: served === 1 ? 'first' : 'ok' }));
		});

		const res1 = await fetch('http://x.test/a');
		expect(await res1.text()).toBe('first');
		await settle();

		const res2 = await fetch('http://x.test/b');
		expect(await res2.text()).toBe('ok');
		await settle();

		expect(net.connectCalls).toBe(1);
		const s = getFetchPoolStats();
		expect(s.created).toBe(1);
		expect(s.reused).toBe(1);
		// Both requests travelled over the same physical connection.
		expect(net.written.length).toBe(2);
		// Every request asks the server to keep the connection alive.
		expect(net.written[0]).toContain('connection: keep-alive');
		expect(net.written[1]).toContain('connection: keep-alive');
	});

	it('queues beyond the per-host cap instead of opening more sockets', async () => {
		const net = setup();
		const { held } = holdRoute(net, 'http://cap.test:80');

		const responses = Promise.all(
			[0, 1, 2, 3, 4, 5].map((i) => fetch(`http://cap.test/${i}`)),
		);
		await settle();
		console.error(`T0 connects=${net.connectCalls} held=${held.length} written=${net.written.length}`);

		// Only 4 physical connections exist while all 6 requests are pending.
		expect(net.connectCalls).toBe(4);
		expect(getFetchPoolStats().queued).toBe(2);

		// Round 1: answer the first 4 requests; their connections are handed
		// straight to the two queued acquires.
		for (const fd of held.splice(0, 4)) {
			net.respond(fd, httpResponse({ body: 'r1' }));
		}
		await settle();
		console.error(`T1 held=${held.length} written=${net.written.length} stats=${JSON.stringify(getFetchPoolStats())}`);

		// Round 2: the two handed-off connections sent new requests.
		expect(held.length).toBe(2);
		for (const fd of held.splice(0, 2)) {
			net.respond(fd, httpResponse({ body: 'r2' }));
		}
		const results = await responses;
		expect(await results[0].text()).toBe('r1');
		expect(await results[1].text()).toBe('r1');
		expect(await results[2].text()).toBe('r1');
		expect(await results[3].text()).toBe('r1');
		expect(await results[4].text()).toBe('r2');
		expect(await results[5].text()).toBe('r2');
		await settle();

		expect(net.connectCalls).toBe(4);
		const s = getFetchPoolStats();
		expect(s.created).toBe(4);
		expect(s.reused).toBe(2);
	}, 15000);

	it('enforces the total cap across hosts', async () => {
		const net = setup();
		const names = ['a', 'b', 'c', 'd', 'e'];
		const routes = names.map((n) => holdRoute(net, `http://${n}.test:80`));

		const responses = Promise.all(
			names.flatMap((n) =>
				[0, 1, 2, 3].map((i) => fetch(`http://${n}.test/${i}`)),
			),
		);
		await settle();

		// 5 hosts x 4 = 20 requested; only 16 sockets may exist.
		expect(net.connectCalls).toBe(16);
		expect(getFetchPoolStats().queued).toBe(4);

		// Answer everything, in rounds, until no request is left holding.
		for (let round = 0; round < 5; round++) {
			let answered = 0;
			for (const route of routes) {
				while (route.held.length > 0) {
					net.respond(route.held.shift()!, httpResponse({ body: 'x' }));
					answered++;
				}
			}
			if (answered === 0) break;
			await settle();
		}

		const results = await responses;
		expect(results).toHaveLength(20);
		for (const r of results) {
			expect(r.status).toBe(200);
			await r.text();
		}
		await settle();
		// The 4 queued requests were served after idle connections of other
		// hosts were evicted to make room under the global cap (browser
		// behaviour), so up to 4 extra connects appear; 16 must exist at the
		// moment of saturation, which the earlier assertion pinned.
		expect(net.connectCalls).toBeLessThanOrEqual(20);
		const s = getFetchPoolStats();
		expect(s.queued).toBe(0);
		expect(s.closed).toBeGreaterThanOrEqual(4);
	}, 15000);

	it('closes a pooled connection after the idle timeout', async () => {
		vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] });
		const net = setup();
		autoRoute(net, 'http://idle.test:80', httpResponse({ body: 'idle' }));

		const res = await fetch('http://idle.test/');
		await res.text();
		await vi.advanceTimersByTimeAsync(50);
		expect(getFetchPoolStats().live).toBe(1);
		expect(getFetchPoolStats().idle).toBe(1);

		await vi.advanceTimersByTimeAsync(15_000);
		const s = getFetchPoolStats();
		expect(s.expiredIdle).toBe(1);
		expect(s.live).toBe(0);
		expect(net.closedFds.length).toBe(1);

		// The next request must open a fresh connection.
		const res2 = await fetch('http://idle.test/');
		expect(await res2.text()).toBe('idle');
		await vi.advanceTimersByTimeAsync(50);
		expect(net.connectCalls).toBe(2);
	});

	it('destroys the connection when the server answers Connection: close', async () => {
		const net = setup();
		autoRoute(
			net,
			'http://close.test:80',
			httpResponse({ body: 'bye', headers: ['connection: close'] }),
		);

		const res = await fetch('http://close.test/');
		expect(await res.text()).toBe('bye');
		await settle();
		expect(getFetchPoolStats().live).toBe(0);

		// A fresh connection is opened for the next request.
		const res2 = await fetch('http://close.test/');
		expect(await res2.text()).toBe('bye');
		await settle();
		expect(net.connectCalls).toBe(2);
	});

	it('detects a peer-closed idle connection through the watchdog', async () => {
		const net = setup();
		autoRoute(net, 'http://watch.test:80', httpResponse({ body: 'watch' }));

		const res = await fetch('http://watch.test/');
		await res.text();
		await settle();
		const fd = [...net.fds.keys()][0];
		expect(getFetchPoolStats().idle).toBe(1);

		// Server closes while the connection sits idle: the parked watchdog
		// read sees EOF and the pool discards the connection.
		net.peerClose(fd);
		await settle();
		const s = getFetchPoolStats();
		expect(s.stalePeer).toBe(1);
		expect(s.live).toBe(0);

		const res2 = await fetch('http://watch.test/');
		expect(await res2.text()).toBe('watch');
		await settle();
		expect(net.connectCalls).toBe(2);
	});

	it('retries once on a stale borrowed connection, transparently', async () => {
		const net = setup();
		autoRoute(net, 'http://stale.test:80', httpResponse({ body: 'fresh' }));

		const res1 = await fetch('http://stale.test/first');
		await res1.text();
		await settle();
		expect(getFetchPoolStats().idle).toBe(1);

		// The pooled connection dies right after the next borrow sends its
		// request: the server answers with a FIN instead of a response.
		const fd0 = [...net.fds.keys()][0];
		net.staleOnNextWrite(fd0);

		const res2 = await fetch('http://stale.test/second');
		// The retry is invisible to the caller.
		expect(await res2.text()).toBe('fresh');
		await settle();

		expect(net.connectCalls).toBe(2);
		const s = getFetchPoolStats();
		expect(s.retriedStale).toBe(1);
		expect(s.created).toBe(2);
		expect(s.reused).toBe(1);
	});

	it('does not retry a real failure on a fresh connection', async () => {
		const net = setup();
		net.failHosts.add('dead.test');

		await expect(fetch('http://dead.test/x')).rejects.toThrow(
			/\[fake-net\] connect to dead\.test refused/,
		);
		await settle();
		const s = getFetchPoolStats();
		expect(s.retriedStale).toBe(0);
		expect(s.created).toBe(1);
		expect(s.live).toBe(0);
	});

	it('destroys the connection when the body is cancelled before framing completes', async () => {
		const net = setup();
		// A Content-Length of 100 with only 10 body bytes sent: the framing
		// never completes, so whatever the consumer does, the connection
		// cannot be reused.
		let partial = true;
		net.route('http://cancel.test:80', ({ fd }) => {
			if (partial) {
				net.respond(
					fd,
					concatAll([
						textBytes('HTTP/1.1 200 OK\r\ncontent-length: 100\r\n\r\n'),
						textBytes('0123456789'),
					]),
				);
			} else {
				net.respond(fd, httpResponse({ body: 'done' }));
			}
		});

		const res = await fetch('http://cancel.test/');
		const reader = res.body!.getReader();
		await reader.read();
		await reader.cancel();
		await settle();

		expect(getFetchPoolStats().live).toBe(0);
		expect(net.closedFds.length).toBe(1);

		// Next request opens a fresh connection and works normally.
		partial = false;
		const res2 = await fetch('http://cancel.test/');
		expect(await res2.text()).toBe('done');
		await settle();
		expect(net.connectCalls).toBe(2);
	});

	it('terminates chunked bodies at the terminal chunk and reuses the connection', async () => {
		const net = setup();
		autoRoute(
			net,
			'http://chunked.test:80',
			httpResponse({ chunkedBody: ['hello ', 'world'] }),
		);

		const res = await fetch('http://chunked.test/');
		const body = await res.text();
		expect(body).toBe('hello world');
		await settle();

		// The terminal chunk ended the body without EOF: the connection is
		// reusable and the next request borrows it.
		const res2 = await fetch('http://chunked.test/');
		expect(await res2.text()).toBe('hello world');
		await settle();

		expect(net.connectCalls).toBe(1);
		expect(getFetchPoolStats().reused).toBe(1);
	});

	it('returns an empty body for 204 and reuses the connection', async () => {
		const net = setup();
		autoRoute(
			net,
			'http://nocontent.test:80',
			httpResponse({ status: 'HTTP/1.1 204 No Content' }),
		);

		const res = await fetch('http://nocontent.test/');
		expect(res.status).toBe(204);
		expect(await res.text()).toBe('');
		await settle();

		const res2 = await fetch('http://nocontent.test/');
		expect(res2.status).toBe(204);
		expect(await res2.text()).toBe('');
		await settle();

		expect(net.connectCalls).toBe(1);
		expect(getFetchPoolStats().reused).toBe(1);
	});

	it('returns an empty body for HEAD and reuses the connection', async () => {
		const net = setup();
		// HEAD: headers claim a body that is never sent.
		net.route('http://head.test:80', ({ fd }) => {
			net.respond(
				fd,
				textBytes('HTTP/1.1 200 OK\r\ncontent-length: 123\r\n\r\n'),
			);
		});

		const res = await fetch('http://head.test/', { method: 'HEAD' });
		expect(res.status).toBe(200);
		expect(await res.text()).toBe('');
		await settle();

		const res2 = await fetch('http://head.test/', { method: 'HEAD' });
		expect(await res2.text()).toBe('');
		await settle();

		expect(net.connectCalls).toBe(1);
		expect(getFetchPoolStats().reused).toBe(1);
	});

	it('pools TLS connections without re-handshake', async () => {
		const net = setup();
		autoRoute(net, 'https://tls.test:443', httpResponse({ body: 'secure' }));

		const res1 = await fetch('https://tls.test/a');
		expect(await res1.text()).toBe('secure');
		await settle();

		const res2 = await fetch('https://tls.test/b');
		expect(await res2.text()).toBe('secure');
		await settle();

		expect(net.connectCalls).toBe(1);
		expect(net.tlsHandshakeCalls).toBe(1);
		expect(getFetchPoolStats().reused).toBe(1);
	});

	it('stays consistent under concurrent borrows, returns and aborts', async () => {
		const net = setup();
		const holdA = holdRoute(net, 'http://ca.test:80');
		const holdB = holdRoute(net, 'http://cb.test:80');

		const controllers = Array.from({ length: 8 }, () => new AbortController());
		const settled = Promise.allSettled(
			controllers.map((c, i) =>
				fetch(`http://${i < 4 ? 'ca' : 'cb'}.test/${i}`, { signal: c.signal }),
			),
		);
		await settle();
		expect(net.connectCalls).toBe(8);

		// Abort three of the held requests mid-flight.
		controllers[0].abort();
		controllers[5].abort();
		controllers[6].abort();
		await settle();

		// Answer everything that is still holding, in rounds.
		for (let round = 0; round < 3; round++) {
			const holding = [...holdA.held.splice(0), ...holdB.held.splice(0)];
			if (holding.length === 0) break;
			for (const fd of holding) {
				net.respond(fd, httpResponse({ body: 'concurrent' }));
			}
			await settle();
		}

		const results = await settled;
		const rejected = results.filter((r) => r.status === 'rejected');
		const fulfilled = results.filter(
			(r) => r.status === 'fulfilled',
		) as PromiseFulfilledResult<Response>[];
		expect(rejected).toHaveLength(3);
		expect(fulfilled).toHaveLength(5);
		for (const r of fulfilled) {
			expect(await r.value.text()).toBe('concurrent');
		}

		// Aborted requests destroyed their connections; fulfilled ones pooled.
		const s = getFetchPoolStats();
		expect(s.queued).toBe(0);
		expect(s.live).toBe(5);

		// The pool still works afterwards.
		autoRoute(net, 'http://ca.test:80', httpResponse({ body: 'after' }));
		const after = await fetch('http://ca.test/after');
		expect(await after.text()).toBe('after');
	}, 15000);

	it('rejects a queued acquire when its signal aborts, without leaking the slot', async () => {
		const net = setup();
		const { held } = holdRoute(net, 'http://queue.test:80');

		const first4 = Promise.allSettled([
			fetch('http://queue.test/1'),
			fetch('http://queue.test/2'),
			fetch('http://queue.test/3'),
			fetch('http://queue.test/4'),
		]);
		await settle();
		expect(net.connectCalls).toBe(4);

		const controller = new AbortController();
		const queued = fetch('http://queue.test/5', { signal: controller.signal });
		await settle();
		expect(getFetchPoolStats().queued).toBe(1);

		controller.abort();
		await expect(queued).rejects.toThrow();
		await settle();

		// The four held requests still complete on their connections.
		for (const fd of held.splice(0)) {
			net.respond(fd, httpResponse({ body: 'queued' }));
		}
		const results = await first4;
		expect(results.every((r) => r.status === 'fulfilled')).toBe(true);
		for (const r of results as PromiseFulfilledResult<Response>[]) {
			await r.value.text();
		}
		await settle();
		expect(getFetchPoolStats().queued).toBe(0);
		expect(getFetchPoolStats().live).toBe(4);

		// The pool keeps serving.
		const next = fetch('http://queue.test/6');
		await settle();
		net.respond(held[0], httpResponse({ body: 'queued' }));
		expect(await (await next).text()).toBe('queued');
	}, 15000);

	it('reaps active connections whose body was never consumed', async () => {
		vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] });
		const net = setup();
		// Full-length header, partial body: framing never completes, so the
		// connection stays checked out until the sweep reaps it.
		let partial = true;
		net.route('http://sweep.test:80', ({ fd }) => {
			if (partial) {
				net.respond(
					fd,
					concatAll([
						textBytes('HTTP/1.1 200 OK\r\ncontent-length: 1000\r\n\r\n'),
						textBytes('0123456789'),
					]),
				);
			}
		});

		const responses = await Promise.all([
			fetch('http://sweep.test/1'),
			fetch('http://sweep.test/2'),
			fetch('http://sweep.test/3'),
			fetch('http://sweep.test/4'),
		]);
		await vi.advanceTimersByTimeAsync(100);
		expect(getFetchPoolStats().live).toBe(4);

		// 60s pass without any read: the sweep must free all four slots.
		net.currentTime += 60_001;
		await vi.advanceTimersByTimeAsync(60_000);
		let s = getFetchPoolStats();
		expect(s.live).toBe(0);
		expect(s.closed).toBe(4);

		// No deadlock: a new request goes through on a fresh connection.
		net.route('http://sweep.test:80', ({ fd }) => {
			net.respond(fd, httpResponse({ body: 'recovered' }));
		});
		const res = await fetch('http://sweep.test/5');
		expect(await res.text()).toBe('recovered');
		s = getFetchPoolStats();
		expect(s.created).toBe(5);
		expect(s.queued).toBe(0);
	});

	it('opens a second connection when the same host is hit concurrently', async () => {
		const net = setup();
		const { held } = holdRoute(net, 'http://par.test:80');

		const p1 = fetch('http://par.test/1');
		await settle();
		const p2 = fetch('http://par.test/2');
		await settle();

		// The first request is still holding its connection: the second must
		// not wait forever, it opens a second connection.
		expect(net.connectCalls).toBe(2);
		expect(held.length).toBe(2);

		net.respond(held[1], httpResponse({ body: 'two' }));
		net.respond(held[0], httpResponse({ body: 'one' }));
		expect(await (await p1).text()).toBe('one');
		expect(await (await p2).text()).toBe('two');
		await settle();
	});
});

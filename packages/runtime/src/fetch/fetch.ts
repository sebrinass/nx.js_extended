import { dataUriToBuffer } from 'data-uri-to-buffer';
import {
	type CompressionFormat,
	DecompressionStream,
} from '../compression-streams';
import { DOMException } from '../dom-exception';
import { readFile } from '../fs';
import { navigator } from '../navigator';
import { decoder } from '../polyfills/text-decoder';
import { encoder } from '../polyfills/text-encoder';
import { objectUrls, URL } from '../polyfills/url';
import { def } from '../utils';
import { Headers } from './headers';
import {
	ConnectionPool,
	createConnectionPool,
	poolKeyFor,
	type ConnectionPoolOptions,
	type FetchPoolStats,
	type PooledConnection,
} from './pool';
import { Request, type RequestInit } from './request';
import { Response } from './response';

/**
 * Optional veto consulted before a redirect is followed.
 *
 * Redirects are followed inside this module, so a caller that gated the URL it
 * asked for has no say in where it actually ends up: a server it was permitted
 * to reach can answer with a 30x to one it was not. An embedder that enforces
 * a per-origin policy (brewser gates `http(s)` on the app manifest's declared
 * `allowed_origins`) installs a guard here and re-checks every hop.
 *
 * Return false to refuse the hop; `fetch` then rejects with a TypeError rather
 * than silently continuing. Unset by default, so stock nx.js behaviour is
 * unchanged.
 *
 * @param url The absolute redirect target.
 * @param from The URL that issued the redirect.
 */
export type RedirectGuard = (url: string, from: string) => boolean;

let redirectGuard: RedirectGuard | null = null;

/** Install (or clear, with `null`) the redirect guard. */
export function setFetchRedirectGuard(guard: RedirectGuard | null): void {
	redirectGuard = guard;
}

/**
 * The keep-alive connection pool behind every `http(s)` fetch. See
 * `./pool.ts` for why it exists (device ENOBUFS after ~90 rapid
 * connect/close cycles) and how reuse is decided.
 */
let fetchPool: ConnectionPool = createConnectionPool();

/**
 * Replace the connection pool (tests install one backed by a fake transport;
 * pass a fresh `createConnectionPool()` to restore native behaviour).
 */
export function setFetchConnectionPool(pool: ConnectionPool): void {
	fetchPool = pool;
}

/** Snapshot of the pool counters (hits / misses / closes, waiters, idle). */
export function getFetchPoolStats(): FetchPoolStats {
	return fetchPool.stats();
}

export type { FetchPoolStats, ConnectionPoolOptions };
export { createConnectionPool, poolKeyFor };

function indexOfEol(arr: ArrayLike<number>, offset: number): number {
	for (let i = offset; i < arr.length - 1; i++) {
		if (arr[i] === 13 && arr[i + 1] === 10) {
			return i;
		}
	}
	return -1;
}

function concat(a: Uint8Array, b: Uint8Array) {
	const c = new Uint8Array(a.length + b.length);
	c.set(a, 0);
	c.set(b, a.length);
	return c;
}

async function* headersIterator(
	reader: ReadableStreamDefaultReader<Uint8Array>,
): AsyncGenerator<{ line: string } | { line: null; leftover: Uint8Array }> {
	let leftover: Uint8Array | null = null;
	while (true) {
		const next = await reader.read();
		if (next.done) throw new Error('Stream closed before end of headers');
		const chunk: Uint8Array = leftover
			? concat(leftover, next.value)
			: next.value;
		let pos = 0;
		while (true) {
			const eol = indexOfEol(chunk, pos);
			if (eol === -1) {
				leftover = chunk.slice(pos);
				break;
			}
			const line = decoder.decode(chunk.slice(pos, eol));
			pos = eol + 2;
			if (line) {
				yield { line };
			} else {
				// end of headers
				leftover = chunk.slice(pos);
				yield { line: null, leftover };
				reader.releaseLock();
				return;
			}
		}
	}
}

/**
 * Parse a chunked response body.
 *
 * For connection reuse the stream must terminate at the terminal (zero-size)
 * chunk rather than waiting for EOF - a keep-alive server keeps the socket
 * open after the terminal chunk. `onComplete(cleanEnd)` fires there: with a
 * keep-alive server the terminal chunk is followed by exactly one CRLF; any
 * other residual bytes (trailers, or data split across pulls) poison the
 * connection, which the pool then destroys instead of reusing.
 */
function createChunkedParseStream(hooks?: FramingHooks) {
	let dataSize = -1;
	let buffer: Uint8Array | null = null;

	return new TransformStream<Uint8Array, Uint8Array>({
		transform(chunk, controller) {
			buffer = buffer ? concat(buffer, chunk) : chunk;

			if (dataSize !== -1) {
				if (buffer.length >= dataSize + 2) {
					const chunkData = buffer.slice(0, dataSize);
					buffer = buffer.slice(dataSize + 2);
					controller.enqueue(chunkData);
					dataSize = -1;
				} else {
					return; // not enough data, wait for more
				}
			}

			let pos;
			while ((pos = indexOfEol(buffer, 0)) >= 0) {
				// while we can find a chunk boundary
				if (pos === 0) {
					// skip empty chunks
					buffer = buffer.slice(2);
					continue;
				}

				const size = parseInt(decoder.decode(buffer.slice(0, pos)), 16);
				if (isNaN(size)) {
					throw new Error('Invalid chunk size');
				}
				buffer = buffer.slice(pos + 2);

				if (size === 0) {
					// Terminal chunk. Consume the final CRLF that follows it.
					if (buffer.length >= 2 && buffer[0] === 13 && buffer[1] === 10) {
						buffer = buffer.slice(2);
					}
					if (hooks?.onComplete) hooks.onComplete(buffer.length === 0);
					controller.terminate();
					return;
				}

				if (buffer.length >= size + 2) {
					// we got a whole chunk
					const chunkData = buffer.slice(0, size);
					buffer = buffer.slice(size + 2);
					controller.enqueue(chunkData);
				} else {
					dataSize = size;
					break; // not enough data, wait for more
				}
			}
		},

		flush(controller) {
			// in case there's unprocessed data left in the buffer when the stream is closed
			if (buffer && buffer.length > 0) {
				controller.enqueue(buffer);
				buffer = null;
			}
		},
	});
}

/** Hooks the fetch layer uses to drive connection-pool framing decisions. */
interface FramingHooks {
	/**
	 * Called once the body is fully delimited. `cleanEnd` is false when
	 * unexpected bytes followed the body end (overrun / chunked trailers),
	 * which poisons the connection against reuse.
	 */
	onComplete?: (cleanEnd: boolean) => void;
	/** Called when the body carried bytes beyond the declared length. */
	onOverrun?: () => void;
}

function createContentLengthStream(
	contentLength: number,
	hooks?: FramingHooks,
) {
	let bytesRead = 0;
	return new TransformStream<Uint8Array, Uint8Array>({
		transform(chunk, controller) {
			if (bytesRead + chunk.byteLength < contentLength) {
				controller.enqueue(chunk);
				bytesRead += chunk.byteLength;
			} else {
				const end = contentLength - bytesRead;
				if (end > 0) {
					controller.enqueue(chunk.subarray(0, end));
				}
				// Bytes beyond the declared length mean the server violated its
				// own framing (or pipelined); the connection must not be reused.
				if (end < chunk.byteLength && hooks?.onOverrun) {
					hooks.onOverrun();
				}
				if (hooks?.onComplete) hooks.onComplete(end >= 0);
				controller.terminate();
			}
		},
	});
}

// List of supported content encodings.
// These values must be accepted by the `DecompressionStream` class.
const ACCEPT_ENCODINGS = new Set(['zstd', 'gzip', 'deflate']);
const ACCEPT_ENCODING_HEADER = [...ACCEPT_ENCODINGS].join(', ');

const MAX_REDIRECTS = 20;

function isSameOrigin(a: URL, b: URL): boolean {
	return (
		a.protocol === b.protocol && a.hostname === b.hostname && a.port === b.port
	);
}

async function fetchHttp(
	req: Request,
	url: URL,
	redirectCount = 0,
	bodyBytes?: Uint8Array | null,
): Promise<Response> {
	if (redirectCount > MAX_REDIRECTS) {
		throw new TypeError('Failed to fetch: too many redirects');
	}

	const isHttps = url.protocol === 'https:';
	const { hostname } = url;
	const port = +url.port || (isHttps ? 443 : 80);
	const hasContentLength = req.headers.has('content-length');

	if (req.signal && req.signal.aborted) {
		throw new DOMException('The operation was aborted.', 'AbortError');
	}

	if (!req.headers.has('host')) {
		req.headers.set('host', url.host);
	}
	if (!req.headers.has('user-agent')) {
		req.headers.set('user-agent', navigator.userAgent);
	}
	if (!req.headers.has('accept')) {
		req.headers.set('accept', '*/*');
	}

	// Enable response compression by default.
	// To opt-out, set `accept-encoding` to "identity".
	// https://developer.mozilla.org/docs/Web/HTTP/Headers/Accept-Encoding
	if (!req.headers.has('accept-encoding')) {
		req.headers.set('accept-encoding', ACCEPT_ENCODING_HEADER);
	}

	// Every request asks for keep-alive by default: reusing connections is the
	// whole point of the pool. A server that answers `Connection: close` is
	// honoured on the response side - such a connection is destroyed and
	// never re-pooled. Callers may set an explicit `Connection: close` to
	// force a fresh connection (e.g. to work around keep-alive reuse bugs),
	// so only set the default when the header is absent.
	if (!req.headers.has('connection')) {
		req.headers.set('connection', 'keep-alive');
	}
	if (req.body && !hasContentLength) {
		req.headers.set('transfer-encoding', 'chunked');
	}

	const headerParts = [`${req.method} ${url.pathname}${url.search} HTTP/1.1`];
	for (const [name, value] of req.headers) {
		headerParts.push(`${name}: ${value}`);
	}
	headerParts.push('', '');
	const header = headerParts.join('\r\n');

	// Buffer the request body on the first call so it can be replayed on 307/308 redirects.
	// On subsequent redirects, `bodyBytes` is passed in directly.
	if (req.body && bodyBytes === undefined) {
		const chunks: Uint8Array[] = [];
		for await (const chunk of req.body) {
			chunks.push(chunk);
		}
		const totalLength = chunks.reduce((sum, c) => sum + c.byteLength, 0);
		bodyBytes = new Uint8Array(totalLength);
		let offset = 0;
		for (const c of chunks) {
			bodyBytes.set(c, offset);
			offset += c.byteLength;
		}
	}

	const poolKey = poolKeyFor(url.protocol, hostname, port);
	const maxAttempts = 2;
	for (let attempt = 0; attempt < maxAttempts; attempt++) {
		const acquire = fetchPool.acquire(poolKey, {
			hostname,
			port,
			secure: isHttps,
		});
		let conn: PooledConnection | null = null;
		// True while `conn` is exclusively ours: only then may the abort
		// listener tear it down. Once the connection is back in the pool (or
		// was destroyed after a redirect hop) a late abort must NOT kill it -
		// it may already serve another request.
		let ownsConn = false;
		let bodyCtrl: ReadableStreamDefaultController<Uint8Array> | null = null;
		const onAbort = () => {
			const err =
				req.signal && req.signal.reason
					? req.signal.reason
					: new DOMException('The operation was aborted.', 'AbortError');
			if (ownsConn) {
				// The connection is handed out and still exclusively ours.
				ownsConn = false;
				acquire.cancel(err);
				return;
			}
			// No connection in our hands. Two cases: the acquire is still
			// queued / connecting (cancel it), or the connection was already
			// released or destroyed (a late abort must NOT touch it - it may
			// serve another request). cancelIfPending does exactly that.
			acquire.cancelIfPending(err);
		};
		if (req.signal) {
			req.signal.addEventListener('abort', onAbort, { once: true });
		}

		try {
			conn = await acquire.promise;
			ownsConn = true;

			await conn.writeBytes(encoder.encode(header));

			// Flush the request body
			if (bodyBytes && bodyBytes.byteLength > 0) {
				if (hasContentLength) {
					await conn.writeBytes(bodyBytes);
				} else {
					await conn.writeBytes(
						encoder.encode(`${bodyBytes.byteLength.toString(16)}\r\n`),
					);
					await conn.writeBytes(bodyBytes);
					await conn.writeBytes(encoder.encode('\r\n0\r\n\r\n'));
				}
			}

			// Per-request response stream over the pooled connection. Its
			// `cancel` runs both when the body framing completes (the framing
			// transform terminates -> the pipe cancels this source) and when
			// the consumer abandons the body early; only the first case
			// returns the connection to the pool.
			const bodySource = new ReadableStream<Uint8Array>({
				async pull(controller) {
					bodyCtrl ??= controller;
					const c = conn;
					if (!c) {
						controller.close();
						return;
					}
					try {
						const chunk = await c.readChunk();
						// The connection may have been cancelled (and released
						// or destroyed) while this read was in flight.
						if (conn !== c) return;
						if (!chunk) {
							// EOF from the server: for an identity-framed body
							// this is the normal end; either way the physical
							// connection is gone.
							if (!c.framingDone) c.destroy();
							controller.close();
							return;
						}
						controller.enqueue(chunk);
					} catch (err) {
						try {
							controller.error(err);
						} catch {
							// Stream already closed/errored - benign abort race.
						}
					}
				},
				cancel() {
					const c = conn;
					conn = null;
					ownsConn = false;
					if (!c || c.destroyed) return;
					if (c.framingDone && c.reusable && c.residualClean) {
						fetchPool.release(c);
					} else {
						// fail() (not just destroy()) so an in-flight read is
						// unblocked instead of dangling forever.
						c.fail(
							new Error(
								'[fetch-pool] response body cancelled before framing completed',
							),
						);
					}
				},
			});

			// Abort / failure of the connection errors this request's body
			// stream, so a consumer blocked on `res.body` wakes up with the
			// real reason instead of hanging on a dead socket.
			conn.onRequestError = (err) => {
				if (bodyCtrl) {
					try {
						bodyCtrl.error(err);
					} catch {
						// Stream already closed/errored - benign abort race.
					}
				}
			};

			const resHeaders = new Headers();
			const r = bodySource.getReader();

			const hi = headersIterator(r);

			// Parse response status line — use indexOf to preserve multi-word status text
			const firstLine = await hi.next();
			if (firstLine.done || !firstLine.value.line) {
				throw new Error('Failed to read response header');
			}
			const statusLine = firstLine.value.line;
			const firstSpace = statusLine.indexOf(' ');
			if (firstSpace === -1) {
				throw new Error(`Invalid HTTP response status line: ${statusLine}`);
			}
			const secondSpace = statusLine.indexOf(' ', firstSpace + 1);
			const statusStr =
				secondSpace === -1
					? statusLine.slice(firstSpace + 1)
					: statusLine.slice(firstSpace + 1, secondSpace);
			const statusText =
				secondSpace === -1 ? '' : statusLine.slice(secondSpace + 1);
			const status = +statusStr;
			if (isNaN(status)) {
				throw new Error(`Invalid HTTP status code: ${statusStr}`);
			}

			// Parse response headers — use append() to support multi-value headers (e.g. Set-Cookie)
			let leftover: Uint8Array | undefined;
			for await (const v of hi) {
				if (typeof v.line === 'string') {
					const col = v.line.indexOf(':');
					const name = v.line.slice(0, col);
					const value = v.line.slice(col + 1).trim();
					resHeaders.append(name, value);
				} else {
					leftover = v.leftover;
				}
			}

			// Redirect
			if (((status / 100) | 0) === 3) {
				// The redirect response body is never read, so the connection's
				// framing is incomplete: destroy it. The next hop opens its own
				// connection, as before.
				if (conn && !conn.destroyed) conn.destroy();
				conn = null;
				ownsConn = false;

				if (req.redirect === 'error') {
					throw new TypeError(
						`URI requested responds with a redirect, redirect mode is set to error: ${url}`,
					);
				}

				if (req.redirect === 'follow') {
					const loc = resHeaders.get('location');
					if (!loc) {
						throw new Error(
							`No "Location" header in ${status} redirect from "${url}"`,
						);
					}
					const redirectUrl = new URL(loc, req.url);

					// Re-gate the hop. The embedder's policy is about which origins may
					// be reached, not about which URL was typed first.
					if (
						redirectGuard &&
						!redirectGuard(redirectUrl.href, String(url))
					) {
						throw new TypeError(
							`Redirect from "${url}" to "${redirectUrl.href}" was refused`,
						);
					}

					let method: RequestInit['method'] = 'GET';
					let redirectBody: Uint8Array | null = null;
					if (status === 307 || status === 308) {
						method = req.method;
						redirectBody = bodyBytes ?? null;
					}

					// Forward headers, but strip Authorization on cross-origin redirects
					const redirectHeaders = new Headers(req.headers);
					redirectHeaders.delete('host');
					if (!isSameOrigin(url, redirectUrl)) {
						redirectHeaders.delete('authorization');
					}

					const redirect = new Request(redirectUrl, {
						method,
						body: redirectBody,
						headers: redirectHeaders,
						redirect: req.redirect,
						signal: req.signal,
					});
					const res = await fetchHttp(
						redirect,
						redirectUrl,
						redirectCount + 1,
						redirectBody,
					);
					res.redirected = true;
					return res;
				}

				// For "manual", just continue with the regular logic
			}

			const c = conn!;
			// Can the connection be reused after this response? HTTP/1.1
			// defaults to keep-alive; the server vetoes via `Connection:
			// close`. HTTP/1.0 needs an explicit `Connection: keep-alive`.
			const connectionHeader = (
				resHeaders.get('connection') ?? ''
			).toLowerCase();
			const connTokens = connectionHeader
				.split(',')
				.map((t) => t.trim());
			c.reusable =
				!connTokens.includes('close') &&
				!(
					statusLine.startsWith('HTTP/1.0') &&
					!connTokens.includes('keep-alive')
				);

			/**
			 * The body framing just completed: the physical connection is no
			 * longer needed by this response, so hand it back (or destroy it)
			 * IMMEDIATELY instead of waiting for the consumer to finish
			 * draining the JS-side streams. Returning early is what lets a
			 * queued acquire reuse the connection even when the response body
			 * is never read; whatever bytes are still queued in JS continue
			 * to flow to this consumer with no fd involvement.
			 */
			const finishFraming = () => {
				if (!conn || conn.destroyed) return;
				conn.framingDone = true;
				if (conn.reusable && conn.residualClean) {
					fetchPool.release(conn);
				} else {
					conn.destroy();
				}
				conn = null;
				ownsConn = false;
			};

			// Responses with no body by spec: framing completes with the
			// headers, and nothing may be read afterwards.
			const noBody =
				status < 200 ||
				status === 204 ||
				status === 304 ||
				req.method.toUpperCase() === 'HEAD';

			const resContentLength = resHeaders.get('content-length');
			const parsedContentLength =
				typeof resContentLength === 'string'
					? Number(resContentLength)
					: NaN;
			let resStream: TransformStream<Uint8Array, Uint8Array> | null = null;
			if (noBody) {
				finishFraming();
			} else if (
				Number.isFinite(parsedContentLength) &&
				parsedContentLength >= 0
			) {
				resStream = createContentLengthStream(parsedContentLength, {
					onOverrun: () => {
						c.residualClean = false;
					},
					onComplete: () => finishFraming(),
				});
			} else if (resHeaders.get('transfer-encoding') === 'chunked') {
				resStream = createChunkedParseStream({
					onComplete: (cleanEnd) => {
						c.residualClean = cleanEnd;
						finishFraming();
					},
				});
			} else {
				// Body delimited only by EOF: reuse would hang the next request
				// (a keep-alive server never sends EOF) and the connection
				// cannot survive this response anyway.
				resStream = new TransformStream<Uint8Array, Uint8Array>();
				c.reusable = false;
			}

			let resBody: ReadableStream<Uint8Array>;
			if (noBody) {
				resBody = new ReadableStream<Uint8Array>({
					start(controller) {
						controller.close();
					},
				});
				finishFraming();
			} else {
				// Web-streams transforms only run when the body is READ, so a
				// Content-Length body that arrived COMPLETELY with the headers
				// (all bytes already off the socket, sitting in `leftover`)
				// must be recognized here: the fd is framing-clean right now
				// and the connection can be handed back immediately. Otherwise
				// an unread body would hold the connection until the sweep.
				const leftoverCoversBody =
					resStream !== null &&
					Number.isFinite(parsedContentLength) &&
					parsedContentLength >= 0 &&
					leftover !== undefined &&
					leftover.byteLength >= parsedContentLength;
				const leftoverLength = leftover ? leftover.byteLength : 0;
				if (leftover) {
					const w = resStream!.writable.getWriter();
					w.write(leftover);
					w.releaseLock();
					leftover = undefined;
				}
				if (leftoverCoversBody) {
					if (leftoverLength > parsedContentLength) {
						// Bytes beyond the declared length: server violated its
						// own framing - never reuse this connection.
						c.residualClean = false;
					}
					finishFraming();
				}
				resBody = bodySource.pipeThrough(resStream!);

				// Decompress the response if the "content-encoding"
				// header is set to a supported decompression format
				const contentEncoding = resHeaders.get('content-encoding');
				if (
					typeof contentEncoding === 'string' &&
					ACCEPT_ENCODINGS.has(contentEncoding)
				) {
					resBody = resBody.pipeThrough(
						new DecompressionStream(contentEncoding as CompressionFormat),
					);
				}
			}

			const res = new Response(resBody, {
				status,
				statusText,
				headers: resHeaders,
			});
			res.url = url.href;
			return res;
		} catch (err) {
			const c = conn;
			conn = null;
			ownsConn = false;
			if (c && !c.destroyed) c.fail(err);
			// A borrowed keep-alive connection that died before producing a
			// single response byte (peer closed it while it sat idle) is
			// retried once on a fresh connection, transparent to the caller.
			// Everything else is a real error and propagates.
			const staleBorrow =
				!!c &&
				c.wasReused &&
				c.borrowBytes === 0 &&
				!(req.signal && req.signal.aborted);
			if (!staleBorrow || attempt === maxAttempts - 1) {
				throw err;
			}
			fetchPool.countStaleRetry();
			continue;
		}
	}
	// Unreachable: each iteration either returns, throws, or continues, and
	// the last iteration always throws.
	throw new Error('Failed to fetch: exhausted connection attempts');
}

async function fetchBlob(req: Request, url: URL) {
	if (req.method !== 'GET') {
		throw new Error(
			`GET method must be used when fetching "${url.protocol}" protocol (got "${req.method}")`,
		);
	}
	const data = objectUrls.get(req.url);
	if (!data) {
		throw new Error(`Object URL "${req.url}" does not exist`);
	}
	return new Response(data, {
		headers: {
			'content-length': String(data.size),
		},
	});
}

async function fetchData(req: Request, url: URL) {
	if (req.method !== 'GET') {
		throw new Error(
			`GET method must be used when fetching "${url.protocol}" protocol (got "${req.method}")`,
		);
	}
	const parsed = dataUriToBuffer(url);
	return new Response(parsed.buffer, {
		headers: {
			'content-length': String(parsed.buffer.byteLength),
			'content-type': parsed.typeFull,
		},
	});
}

async function fetchFile(req: Request, url: URL) {
	if (req.method !== 'GET') {
		throw new Error(
			`GET method must be used when fetching "${url.protocol}" protocol (got "${req.method}")`,
		);
	}
	const path = url.protocol === 'file:' ? `sdmc:${url.pathname}` : url.href;
	// TODO: Use streaming FS interface
	const data = await readFile(path);
	const headers = new Headers();
	let status = 200;
	if (data) {
		headers.set('content-length', String(data.byteLength));
	} else {
		status = 404;
	}
	return new Response(data, { status, headers });
}

const fetchers = new Map<string, (req: Request, url: URL) => Promise<Response>>(
	[
		['http:', fetchHttp],
		['https:', fetchHttp],
		['blob:', fetchBlob],
		['data:', fetchData],
		['file:', fetchFile],
		['sdmc:', fetchFile],
		['romfs:', fetchFile],
	],
);

/**
 * The global `fetch()` method starts the process of fetching a resource from the network, returning a promise which is fulfilled once the response is available.
 *
 * ### Supported Protocols
 *
 * | Protocol | Description                                                                 |
 * |----------|-----------------------------------------------------------------------------|
 * | `http:`  | Fetch data from the network using the HTTP protocol                         |
 * | `https:` | Fetch data from the network using the HTTPS protocol                        |
 * | `blob:`  | Fetch data from a URL constructed by {@link URL.createObjectURL | `URL.createObjectURL()`}                |
 * | `data:`  | Fetch data from a [Data URI](https://developer.mozilla.org/docs/Web/HTTP/Basics_of_HTTP/Data_URLs) (possibly base64-encoded)                        |
 * | `sdmc:`  | Fetch data from a local file on the SD card                                 |
 * | `romfs:` | Fetch data from the RomFS partition of the nx.js application                |
 * | `file:`  | Same as `sdmc:`                                                             |
 *
 * @example
 *
 * ```typescript
 * fetch('http://jsonip.com')
 *   .then(res => res.json())
 *   .then(data => {
 *     console.log(`Current IP address: ${data.ip}`);
 *   });
 * ```
 *
 * @see https://developer.mozilla.org/docs/Web/API/fetch
 */
// NOTE: Intentionally not an async function so that `Object.prototype.toString` returns "[object Function]"
export function fetch(
	input: string | URL | Request,
	init?: RequestInit,
): Promise<Response> {
	const req = new Request(input, init);
	const url = new URL(req.url);
	const fetcher = fetchers.get(url.protocol);
	if (!fetcher) {
		throw new Error(`scheme '${url.protocol.slice(0, -1)}' not supported`);
	}
	return fetcher(req, url).then((res) => {
		if (!res.url) res.url = url.href;
		return res;
	});
}
def(fetch);

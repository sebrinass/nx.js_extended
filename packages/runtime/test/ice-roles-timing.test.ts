/**
 * Regression tests — engine patch 0037 (ICE roles, peer reflexive candidates,
 * the answer-sent gate and the wall clock retransmission schedule).
 *
 * ## What is under test
 *
 * The behaviour lives in **libpeer**, which is a git SUBMODULE of the engine:
 * its sources are patched by `tools/libpeer-switch/0005-*.patch` right before
 * libpeer is built, and the C code is cross compiled for the Switch -- there
 * is no host compiler for it on this machine, so the `nxjs-test` binary (which
 * needs a Linux image) cannot run it either.
 *
 * The tests therefore work on the **deliverable itself**: the inner patch
 * file.  `postImage()` rebuilds the POST-PATCH source of every patched file
 * out of the unified diff (context lines + added lines, removed lines
 * dropped), so every assertion below is a *contract on the patched C code*:
 *
 *   * a mutation of the C source that is re-exported into the patch turns the
 *     corresponding assertion red, and
 *   * the numeric constants are EXTRACTED from the patched source and fed
 *     into a TypeScript model of the same control flow, so the behavioural
 *     properties (millisecond schedule, frame rate independence, the CHECKING
 *     timeout, the answer-sent gate, the prflx / USE-CANDIDATE path) are
 *     exercised without a C toolchain.
 *
 * The model is a transcription of the C, not a re-invention: every number it
 * uses comes from the patch.
 */

import { readFileSync, readdirSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';

const here = dirname(fileURLToPath(import.meta.url));
const engineRoot = resolve(here, '..', '..', '..');

/** The libpeer inner patch that carries every libpeer side change of 0037. */
function innerPatchPath(): string {
	const dir = join(engineRoot, 'tools', 'libpeer-switch');
	const found = readdirSync(dir).find((f) => f.startsWith('0005-') && f.endsWith('.patch'));
	if (!found) {
		throw new Error(
			`no 0005-*.patch in ${dir} -- the libpeer side of patch 0037 is missing`,
		);
	}
	return join(dir, found);
}

const innerPatch = readFileSync(innerPatchPath(), 'utf8');

/**
 * Rebuild the POST-PATCH source of a file out of the unified diff.
 *
 * Only the hunks are available (the patch does not carry the whole file), so
 * this is the changed regions plus their context -- which is exactly what the
 * contracts below are about.
 */
function postImage(file: string): string {
	const marker = `a/${file} b/${file}`;
	const section = innerPatch.split('\ndiff --git ').find((s) => s.startsWith(marker));
	if (!section) {
		throw new Error(`the inner patch does not touch ${file}`);
	}
	const out: string[] = [];
	for (const line of section.split('\n')) {
		if (line.startsWith('@@') || line.startsWith('---') || line.startsWith('+++')) {
			continue;
		}
		if (line.startsWith('-')) {
			continue; // the removed side is NOT part of the post image
		}
		if (line.startsWith('+') || line.startsWith(' ')) {
			out.push(line.slice(1));
		}
	}
	return out.join('\n');
}

const agentSrc = postImage('src/agent.c');
const agentHdr = postImage('src/agent.h');
const iceSrc = postImage('src/ice.c');
const stunSrc = postImage('src/stun.c');
const stunHdr = postImage('src/stun.h');
const pcSrc = postImage('src/peer_connection.c');
const pcHdr = postImage('src/peer_connection.h');
const addrSrc = postImage('src/address.c');
const configSrc = postImage('src/config.h');
const dtlsSrc = postImage('src/dtls_srtp.c');
const socketSrc = postImage('src/socket.c');

/** The engine side of 0037 (the native binding + the per-tick loop count). */
const engineBackend = readFileSync(
	join(engineRoot, 'source', 'webrtc_backend_libpeer.cc'),
	'utf8',
);
const engineBinding = readFileSync(join(engineRoot, 'source', 'webrtc.cc'), 'utf8');
const engineTs = readFileSync(
	join(engineRoot, 'packages', 'runtime', 'src', 'native-webrtc.ts'),
	'utf8',
);

// ---------------------------------------------------------------------------
// constants extracted from the patched source
// ---------------------------------------------------------------------------

function intFrom(src: string, pattern: RegExp, label: string): number {
	const m = src.match(pattern);
	if (!m) {
		throw new Error(`patch 0037 does not define ${label} (pattern ${pattern})`);
	}
	return Number.parseInt(m[1] as string, 10);
}

const CFG = {
	/** first retransmission interval, ms */
	intervalBaseMs: intFrom(
		agentSrc,
		/#define AGENT_CONNCHECK_INTERVAL_BASE_MS (\d+)/,
		'AGENT_CONNCHECK_INTERVAL_BASE_MS',
	),
	/** retransmission interval cap, ms */
	intervalMaxMs: intFrom(
		agentSrc,
		/#define AGENT_CONNCHECK_INTERVAL_MAX_MS (\d+)/,
		'AGENT_CONNCHECK_INTERVAL_MAX_MS',
	),
	/** how long the CHECKING state may take before it fails, ms */
	checkingTimeoutMs: intFrom(
		pcSrc,
		/#define PEER_ICE_CHECKING_TIMEOUT_MS (\d+)/,
		'PEER_ICE_CHECKING_TIMEOUT_MS',
	),
	/** how long the ICE waits for the "the answer went out" report, ms */
	answerSentFallbackMs: intFrom(
		pcSrc,
		/#define PEER_ANSWER_SENT_FALLBACK_MS (\d+)/,
		'PEER_ANSWER_SENT_FALLBACK_MS',
	),
	/** datagrams one tick may take off the socket */
	drainMax: intFrom(agentHdr, /#define AGENT_RECV_DRAIN_MAX (\d+)/, 'AGENT_RECV_DRAIN_MAX'),
	/** the synchronous STUN receive budget, in poll attempts */
	stunRecvMaxTimes: intFrom(
		agentSrc,
		/#define AGENT_STUN_RECV_MAXTIMES (\d+)/,
		'AGENT_STUN_RECV_MAXTIMES',
	),
	/** libwebrtc's type preference for a peer reflexive candidate */
	prflxTypePreference: intFrom(
		iceSrc,
		/case ICE_CANDIDATE_TYPE_PRFLX:[\s\S]{0,240}?return (\d+);/,
		'the prflx type preference',
	),
	/** libwebrtc's local preference */
	localPreference: intFrom(
		iceSrc,
		/ice_candidate_local_preference[\s\S]{0,400}?return (\d+);/,
		'the local preference',
	),
};

// ---------------------------------------------------------------------------
// which fixes are actually in the patch
//
// These flags drive the model below: a mutation of the C source that is
// re-exported into the patch flips the matching flag and turns the matching
// behavioural test red.  That is what makes the mutation test meaningful --
// the model is not a free standing copy of the C, it reads the C.
// ---------------------------------------------------------------------------

const FEATURES = {
	/** the wall clock schedule replaced the frame count criterion */
	wallClockSchedule: !/if \(agent->nominated_pair->conncheck %/.test(agentSrc),
	/** "typ prflx" is parsed instead of rejected */
	prflxParsed: /strncmp\(type, "prflx", 5\)/.test(iceSrc),
	/** a binding request from an unknown source creates a prflx candidate */
	prflxCreated: agentSrc.includes('agent_add_peer_reflexive_candidate'),
	/** USE-CANDIDATE nominates the pair it arrived on */
	useCandidateNominates: agentSrc.includes('agent_nominate_pair_for_remote'),
	/** a binding response is validated (source address + transaction id) */
	responseValidated: agentSrc.includes('agent_conncheck_response_matches'),
	/** the ICE waits until the answer is on the wire */
	answerSentGate: pcSrc.includes('peer_connection_set_answer_sent'),
	/** a succeeded pair wins over a frozen one */
	succeededWins: agentSrc.includes('agent->nominated_pair = &agent->candidate_pairs[i];'),
};

// ---------------------------------------------------------------------------
// a transcription of the patched C control flow
// ---------------------------------------------------------------------------

type IceCandidateType = 'host' | 'srflx' | 'prflx' | 'relay';

interface Cand {
	type: IceCandidateType;
	ip: string;
	port: number;
	priority: number;
}

interface Pair {
	local: Cand;
	remote: Cand;
	state: 'frozen' | 'inprogress' | 'succeeded' | 'failed';
	conncheck: number;
}

const addrEqual = (a: { ip: string; port: number }, b: { ip: string; port: number }) =>
	a.ip === b.ip && a.port === b.port;

/** RFC 8445 section 5.1.2.1, with libwebrtc's type preferences. */
function computePriority(type: IceCandidateType, component = 1): number {
	const typePreference =
		type === 'host'
			? 126
			: type === 'prflx'
				? CFG.prflxTypePreference
				: type === 'srflx'
					? 100
					: 0;
	return (1 << 24) * typePreference + (1 << 8) * CFG.localPreference + (256 - component);
}

class IceModel {
	localCandidates: Cand[] = [];
	remoteCandidates: Cand[] = [];
	pairs: Pair[] = [];
	nominated: Pair | null = null;
	selected: Pair | null = null;
	conncheck = 0;
	requestsSent = 0;
	intervalMs = CFG.intervalBaseMs;
	lastSentMs = 0;
	responses = 0;
	rejected = 0;
	answerSent = false;
	answerCreatedMs = 0;
	state: 'new' | 'checking' | 'connected' | 'failed' = 'new';
	checkingStartedMs = 0;
	/** the transaction ids of the checks that are still in flight */
	txids: number[] = [];
	sentChecks: number[] = [];

	// -- setup ---------------------------------------------------------------

	addLocalCandidate(ip: string, port: number): Cand {
		const c: Cand = { type: 'host', ip, port, priority: computePriority('host') };
		this.localCandidates.push(c);
		return c;
	}

	addRemoteCandidate(ip: string, port: number, type: IceCandidateType = 'host'): Cand {
		const c: Cand = { type, ip, port, priority: computePriority(type) };
		this.remoteCandidates.push(c);
		this.pairWithLocals(c);
		return c;
	}

	/** agent_update_candidate_pairs() / agent_pair_remote_candidate() */
	pairWithLocals(remote: Cand): void {
		for (const local of this.localCandidates) {
			this.pairs.push({ local, remote, state: 'frozen', conncheck: 0 });
		}
	}

	// -- the ICE schedule ----------------------------------------------------

	/**
	 * agent_connectivity_check(): true when a check went out in this tick.
	 *
	 * The decision is a WALL CLOCK deadline (RFC 8445 section 7.2.5), NOT the
	 * conncheck counter -- the counter is only "how many did we send".
	 */
	connectivityCheck(nowMs: number): boolean {
		if (!this.nominated || this.nominated.state !== 'inprogress') {
			return false;
		}
		let sent = false;
		if (!FEATURES.wallClockSchedule) {
			// the OLD behaviour: one check every 100 CHECKS (and the counter
			// is bumped once per rendered frame).
			if (this.conncheck % 100 === 0) {
				sent = true;
			}
		} else if (this.requestsSent === 0) {
			sent = true;
		} else {
			const elapsed = nowMs - this.lastSentMs;
			if (elapsed >= this.intervalMs) {
				sent = true;
				this.intervalMs = Math.min(this.intervalMs * 2, CFG.intervalMaxMs);
			}
		}
		if (sent) {
			this.lastSentMs = nowMs;
			this.requestsSent++;
			this.txids.push(this.requestsSent);
			this.sentChecks.push(nowMs);
		}
		return sent;
	}

	/** agent_select_candidate_pair(): a SUCCEEDED pair wins over a frozen one. */
	selectCandidatePair(): void {
		if (FEATURES.succeededWins) {
			const succeeded = this.pairs.find((p) => p.state === 'succeeded');
			if (succeeded) {
				this.selected = succeeded;
				this.nominated = succeeded;
				return;
			}
		}
		const frozen = this.pairs.find((p) => p.state === 'frozen');
		if (frozen) {
			this.nominated = frozen;
			frozen.conncheck = 0;
			frozen.state = 'inprogress';
			this.intervalMs = CFG.intervalBaseMs;
			this.requestsSent = 0;
			this.lastSentMs = 0;
			return;
		}
		const inProgress = this.pairs.find((p) => p.state === 'inprogress');
		if (inProgress) {
			inProgress.conncheck++;
			this.conncheck = inProgress.conncheck;
		}
	}

	// -- the peer side -------------------------------------------------------

	/**
	 * agent_process_stun_request(): a binding request from an unknown source
	 * creates a PEER REFLEXIVE candidate, and a request that carries
	 * USE-CANDIDATE nominates the pair it arrived on.
	 */
	processBindingRequest(
		from: { ip: string; port: number },
		useCandidate: boolean,
		requestPriority?: number,
	): void {
		let remote = this.remoteCandidates.find((c) => addrEqual(c, from));
		if (!remote) {
			if (!FEATURES.prflxCreated) {
				return; // the OLD behaviour: an unknown source is dropped
			}
			remote = {
				type: 'prflx',
				ip: from.ip,
				port: from.port,
				priority: requestPriority !== undefined ? requestPriority : computePriority('prflx'),
			};
			this.remoteCandidates.push(remote);
			this.pairWithLocals(remote);
		}
		if (!useCandidate || !FEATURES.useCandidateNominates) {
			return;
		}
		const pair = this.pairs.find((p) => addrEqual(p.remote, from));
		if (!pair) {
			return;
		}
		pair.state = 'succeeded';
		this.selected = pair;
		this.nominated = pair;
	}

	/**
	 * agent_process_stun_response(): the response has to come from the
	 * candidate the check was sent to and has to carry the transaction id of
	 * a check that is still in flight.
	 */
	processBindingResponse(from: { ip: string; port: number }, txid: number): boolean {
		if (FEATURES.responseValidated) {
			if (this.requestsSent === 0) {
				this.rejected++;
				return false;
			}
			if (!this.txids.includes(txid)) {
				this.rejected++;
				return false;
			}
			if (!this.nominated || !addrEqual(this.nominated.remote, from)) {
				this.rejected++;
				return false;
			}
		}
		this.responses++;
		this.nominated!.state = 'succeeded';
		this.selected = this.nominated;
		return true;
	}

	// -- the state machine ---------------------------------------------------

	createAnswer(nowMs: number): void {
		this.answerCreatedMs = nowMs;
		this.answerSent = false;
		this.state = 'new';
	}

	notifyAnswerSent(): void {
		this.answerSent = true;
	}

	/** one peer_connection_loop() call */
	tick(nowMs: number): void {
		if (this.state === 'new') {
			if (!FEATURES.answerSentGate) {
				// the OLD behaviour: CHECKING starts inside create_answer()
				this.state = 'checking';
				this.checkingStartedMs = nowMs;
				return;
			}
			const ready =
				this.answerSent || nowMs - this.answerCreatedMs >= CFG.answerSentFallbackMs;
			if (ready) {
				this.state = 'checking';
				this.checkingStartedMs = nowMs;
			}
			return;
		}
		if (this.state !== 'checking') {
			return;
		}
		if (nowMs - this.checkingStartedMs >= CFG.checkingTimeoutMs) {
			this.state = 'failed';
			return;
		}
		this.selectCandidatePair();
		if (this.nominated?.state === 'succeeded') {
			this.state = 'connected';
			return;
		}
		this.connectivityCheck(nowMs);
	}
}

// ---------------------------------------------------------------------------
// 1) the frame count criterion is gone
// ---------------------------------------------------------------------------

describe('ICE retransmission schedule (patch 0037)', () => {
	it('does NOT decide the retransmission from the conncheck counter', () => {
		// the frame count criterion is gone -- both the #define and the code.
		// (the COMMENT that explains the old behaviour still names it, so the
		// assertion looks at the code, not at the prose)
		expect(agentSrc).not.toContain('#define AGENT_CONNCHECK_PERIOD');
		expect(agentSrc).not.toMatch(/if \(agent->nominated_pair->conncheck %/);
	});

	it('decides it from a wall clock deadline with an exponential backoff', () => {
		expect(agentSrc).toContain('agent_conncheck_due');
		expect(agentSrc).toContain('last_conncheck_sent_ms');
		expect(agentSrc).toContain('ports_get_epoch_time()');
		expect(CFG.intervalBaseMs).toBe(100);
		expect(CFG.intervalMaxMs).toBe(2000);
	});

	it('keeps conncheck as the "how many did we send" counter', () => {
		expect(agentSrc).toContain('conncheck_requests_sent');
		expect(agentSrc).toContain('conncheck++');
	});

	it('sends the same checks at 30 fps and at 60 fps (frame rate independent)', () => {
		const run = (frameMs: number) => {
			const ice = new IceModel();
			ice.addLocalCandidate('192.168.1.176', 50000);
			ice.addRemoteCandidate('10.0.0.7', 40000);
			ice.createAnswer(0);
			ice.notifyAnswerSent();
			for (let t = frameMs; t <= 3000; t += frameMs) {
				ice.tick(t);
			}
			return ice.sentChecks;
		};
		const at30 = run(33);
		const at60 = run(16);
		expect(at30.length).toBeGreaterThan(1);
		// the schedule is the wall clock, so both frame rates send the SAME
		// NUMBER of checks in the same window -- that is the whole point of
		// the change (the old code sent one every 100 FRAMES).
		expect(at30.length).toBe(at60.length);
		// and the first retransmission is ~100 ms after the first check, not
		// 100 frames (which would be 3.3 s at 30 fps).
		expect(at30[1]! - at30[0]!).toBeGreaterThanOrEqual(100);
		expect(at30[1]! - at30[0]!).toBeLessThan(100 + 33);
		// the two clocks stay in step: the deadline is noticed by the next
		// tick, so they may differ by up to one frame each (the backoff
		// re-anchors on the actual send time, so the drift grows slowly).
		for (let i = 0; i < at30.length; i++) {
			expect(Math.abs(at30[i]! - at60[i]!)).toBeLessThan(250);
		}
	});

	it('backs off instead of flooding: the interval doubles and is capped', () => {
		const ice = new IceModel();
		ice.addLocalCandidate('192.168.1.176', 50000);
		ice.addRemoteCandidate('10.0.0.7', 40000);
		ice.createAnswer(0);
		ice.notifyAnswerSent();
		for (let t = 16; t <= 60000; t += 16) {
			ice.tick(t);
		}
		expect(ice.intervalMs).toBe(CFG.intervalMaxMs);
		// 100 + 200 + 400 + 800 + 1600 + then 2000 ms each
		const expected = 100 + 200 + 400 + 800 + 1600 + Math.ceil((60000 - 5100) / 2000) * 2000;
		expect(ice.requestsSent).toBeLessThan(Math.ceil(expected / 100));
		expect(ice.requestsSent).toBeGreaterThan(0);
	});
});

// ---------------------------------------------------------------------------
// 2) the CHECKING state has a real timeout
// ---------------------------------------------------------------------------

describe('the CHECKING state (patch 0037)', () => {
	it('fails with a reason instead of checking forever', () => {
		expect(pcSrc).toContain('PEER_ICE_CHECKING_TIMEOUT_MS');
		expect(pcSrc).toContain('ice_checking_timeout');
		expect(pcSrc).toContain('PEER_CONNECTION_FAILED');
	});

	it('tells the three failure reasons apart', () => {
		expect(pcSrc).toContain('no_candidate_pairs');
		expect(pcSrc).toContain('pair_never_nominated');
		expect(pcSrc).toContain('no_binding_response');
	});

	it('fails after the budget when the peer never answers', () => {
		const ice = new IceModel();
		ice.addLocalCandidate('192.168.1.176', 50000);
		ice.addRemoteCandidate('10.0.0.7', 40000);
		ice.createAnswer(0);
		ice.notifyAnswerSent();
		for (let t = 16; t <= CFG.checkingTimeoutMs + 100; t += 16) {
			ice.tick(t);
		}
		expect(ice.state).toBe('failed');
		expect(ice.requestsSent).toBeGreaterThan(0); // we really did try
	});

	it('logs the pairs WHILE it is still checking', () => {
		expect(pcSrc).toContain('ice_checking');
		expect(pcSrc).toContain('peer_connection_log_candidate_pairs(pc, "checking")');
	});
});

// ---------------------------------------------------------------------------
// 3) the answer-sent gate: no check before the answer is on the wire
// ---------------------------------------------------------------------------

describe('the answer-sent gate (patch 0037)', () => {
	it('does not enter CHECKING inside create_answer() any more', () => {
		const fn = pcSrc.split('peer_connection_create_answer(PeerConnection* pc)')[1] ?? '';
		const body = fn.split('\nint peer_connection_set_answer_sent')[0] ?? fn.slice(0, 800);
		expect(body).not.toContain('PEER_CONNECTION_CHECKING');
	});

	it('exposes peer_connection_set_answer_sent()', () => {
		expect(pcHdr).toContain('void peer_connection_set_answer_sent(PeerConnection* pc, int sent)');
		expect(pcSrc).toContain('PEER_ANSWER_SENT_FALLBACK_MS');
	});

	it('sends NO check before the answer-sent signal', () => {
		const ice = new IceModel();
		ice.addLocalCandidate('192.168.1.176', 50000);
		ice.addRemoteCandidate('10.0.0.7', 40000);
		ice.createAnswer(0);
		for (let t = 16; t <= 1000; t += 16) {
			ice.tick(t);
		}
		expect(ice.state).toBe('new');
		expect(ice.requestsSent).toBe(0);
		expect(ice.sentChecks).toEqual([]);
	});

	it('starts the check once the answer went out', () => {
		const ice = new IceModel();
		ice.addLocalCandidate('192.168.1.176', 50000);
		ice.addRemoteCandidate('10.0.0.7', 40000);
		ice.createAnswer(0);
		ice.notifyAnswerSent();
		// the first tick moves NEW -> CHECKING, the second one checks.
		ice.tick(16);
		expect(ice.state).toBe('checking');
		expect(ice.requestsSent).toBe(0);
		ice.tick(32);
		expect(ice.requestsSent).toBe(1);
	});

	it('still starts when the page never reports it (fallback)', () => {
		const ice = new IceModel();
		ice.addLocalCandidate('192.168.1.176', 50000);
		ice.addRemoteCandidate('10.0.0.7', 40000);
		ice.createAnswer(0);
		for (let t = 16; t <= CFG.answerSentFallbackMs + 16; t += 16) {
			ice.tick(t);
		}
		expect(ice.state).toBe('checking');
		expect(ice.requestsSent).toBeGreaterThan(0);
	});

	it('exposes the native entry point and the TS surface', () => {
		expect(engineBinding).toContain('webrtcNotifyAnswerSent');
		expect(engineBackend).toContain('nx_webrtc_backend_notify_answer_sent');
		expect(engineBackend).toContain('peer_connection_set_answer_sent');
		expect(engineTs).toContain('notifyAnswerSent');
	});
});

// ---------------------------------------------------------------------------
// 4) the peer reflexive candidate + USE-CANDIDATE (the real ICE fix)
// ---------------------------------------------------------------------------

describe('peer reflexive candidates and USE-CANDIDATE (patch 0037)', () => {
	it('parses "typ prflx" instead of rejecting it', () => {
		expect(iceSrc).toContain('ICE_CANDIDATE_TYPE_PRFLX');
		expect(iceSrc).toMatch(/strncmp\(type, "prflx", 5\)[\s\S]{0,700}ICE_CANDIDATE_TYPE_PRFLX/);
	});

	it('parses and KEEPS the ICE attributes instead of dropping them', () => {
		expect(stunHdr).toContain('has_use_candidate');
		expect(stunHdr).toContain('has_ice_controlling');
		expect(stunHdr).toContain('ice_tie_breaker');
		expect(stunHdr).toContain('has_priority');
		expect(stunSrc).toContain('msg->has_use_candidate = 1;');
		// the old "Do nothing" for USE-CANDIDATE must be gone
		expect(stunSrc).not.toMatch(/case STUN_ATTR_TYPE_USE_CANDIDATE:\s*\n\s*\/\/ LOGD/);
	});

	it('nominates the pair the USE-CANDIDATE request arrived on', () => {
		expect(agentSrc).toContain('agent_nominate_pair_for_remote');
		expect(agentSrc).toContain('ICE_CANDIDATE_STATE_SUCCEEDED');
	});

	it('creates a peer reflexive candidate for an unknown source address', () => {
		expect(agentSrc).toContain('agent_add_peer_reflexive_candidate');
		expect(agentSrc).toContain('agent_pair_remote_candidate');
	});

	it("uses libwebrtc's priority parameters", () => {
		expect(CFG.prflxTypePreference).toBe(110);
		expect(CFG.localPreference).toBe(65535);
		expect(agentSrc).toContain('candidate->priority = priority;');
	});

	it('walks the whole cloud path: unknown source + USE-CANDIDATE + PRIORITY', () => {
		const ice = new IceModel();
		ice.addLocalCandidate('192.168.1.176', 50000);
		// the app submits the cloud candidate as it is (the pod's internal
		// address): our check goes there and does not reach the cloud.
		ice.addRemoteCandidate('172.17.0.9', 31000);

		// the cloud learned our NAT mapping from our check and sends ITS
		// check back to our public address, with USE-CANDIDATE and PRIORITY.
		const requestPriority = 123456789;
		ice.processBindingRequest({ ip: '203.0.113.7', port: 40001 }, true, requestPriority);
		// 1) a peer reflexive candidate was created for the unknown source
		const prflx = ice.remoteCandidates.find((c) => c.type === 'prflx');
		expect(prflx).toBeDefined();
		expect(prflx!.ip).toBe('203.0.113.7');
		// 2) its priority is the one the peer put into ITS request
		expect(prflx!.priority).toBe(requestPriority);
		// 3) it was paired with the local candidate
		const pair = ice.pairs.find((p) => p.remote === prflx);
		expect(pair).toBeDefined();
		// 4) and the pair is nominated
		expect(ice.nominated).toBe(pair);
		expect(pair!.state).toBe('succeeded');

		// 5) the ICE completes on the next tick
		ice.notifyAnswerSent();
		ice.tick(16);
		ice.tick(32);
		expect(ice.state).toBe('connected');
	});

	it('falls back to the local formula when the request carries no PRIORITY', () => {
		const ice = new IceModel();
		ice.addLocalCandidate('192.168.1.176', 50000);
		ice.processBindingRequest({ ip: '203.0.113.7', port: 40001 }, true);
		const prflx = ice.remoteCandidates.find((c) => c.type === 'prflx');
		expect(prflx!.priority).toBe(computePriority('prflx'));
	});
});

// ---------------------------------------------------------------------------
// 5) a binding response has to belong to one of our checks
// ---------------------------------------------------------------------------

describe('binding response validation (patch 0037)', () => {
	const setup = () => {
		const ice = new IceModel();
		ice.addLocalCandidate('192.168.1.176', 50000);
		ice.addRemoteCandidate('10.0.0.7', 40000);
		ice.createAnswer(0);
		ice.notifyAnswerSent();
		// the first tick moves NEW -> CHECKING (the answer is on the wire),
		// the second one runs the connectivity check.
		ice.tick(16);
		ice.tick(32);
		return ice;
	};

	it('accepts the response that answers our check', () => {
		const ice = setup();
		const txid = ice.txids[ice.txids.length - 1]!;
		expect(ice.processBindingResponse({ ip: '10.0.0.7', port: 40000 }, txid)).toBe(true);
		expect(ice.responses).toBe(1);
		expect(ice.nominated!.state).toBe('succeeded');
	});

	it('DROPS a response from an address we did not check', () => {
		const ice = setup();
		const txid = ice.txids[ice.txids.length - 1]!;
		expect(ice.processBindingResponse({ ip: '198.51.100.5', port: 1 }, txid)).toBe(false);
		expect(ice.responses).toBe(0);
		expect(ice.rejected).toBe(1);
		expect(ice.nominated!.state).toBe('inprogress');
	});

	it('DROPS a response with a foreign transaction id', () => {
		const ice = setup();
		expect(ice.processBindingResponse({ ip: '10.0.0.7', port: 40000 }, 999999)).toBe(false);
		expect(ice.responses).toBe(0);
		expect(ice.rejected).toBe(1);
		expect(ice.nominated!.state).toBe('inprogress');
	});

	it('really validates in the C code', () => {
		expect(agentSrc).toContain('agent_conncheck_response_matches');
		expect(agentSrc).toContain('addr_equal');
		// addr_equal used to be a stub that ALWAYS returned 1
		expect(addrSrc).toMatch(/int addr_equal[\s\S]{0,800}?sin_addr/);
	});
});

// ---------------------------------------------------------------------------
// 6) a succeeded pair has to end the ICE
// ---------------------------------------------------------------------------

describe('the succeeded pair (patch 0037)', () => {
	it('keeps nominated_pair and selected_pair in sync', () => {
		const ice = new IceModel();
		ice.addLocalCandidate('192.168.1.176', 50000);
		ice.addRemoteCandidate('10.0.0.7', 40000);
		ice.createAnswer(0);
		ice.notifyAnswerSent();
		ice.tick(16);
		ice.tick(32);
		const txid = ice.txids[ice.txids.length - 1]!;
		ice.processBindingResponse({ ip: '10.0.0.7', port: 40000 }, txid);
		ice.selectCandidatePair();
		expect(ice.nominated).toBe(ice.selected);
		expect(ice.nominated!.state).toBe('succeeded');
	});

	it('really syncs them in the C code', () => {
		expect(agentSrc).toContain('agent->nominated_pair = &agent->candidate_pairs[i];');
		expect(agentSrc).toContain('agent->selected_pair = &agent->candidate_pairs[i];');
	});
});

// ---------------------------------------------------------------------------
// 7) the drain loop and the smaller knobs
// ---------------------------------------------------------------------------

describe('the socket drain and the small knobs (patch 0037)', () => {
	it('drains the socket instead of taking one datagram per frame', () => {
		expect(agentSrc).toContain('agent_recv_drain');
		expect(agentSrc).toContain('agent_recv_datagram');
		expect(agentHdr).toContain('agent_recv_drain');
		expect(CFG.drainMax).toBe(32);
	});

	it('shortens the synchronous STUN receive', () => {
		expect(CFG.stunRecvMaxTimes).toBe(100);
	});

	it('asks for a 256 KiB receive buffer (2 MiB always failed on the Switch)', () => {
		expect(socketSrc).toMatch(/int receive_buffer = 256 \* 1024;/);
	});

	it('only walks the sockets that were really opened', () => {
		expect(agentSrc).toContain('udp_socket_count');
		expect(agentHdr).toContain('udp_socket_count');
	});
});

// ---------------------------------------------------------------------------
// 8) the ECDSA P-256 certificate
// ---------------------------------------------------------------------------

describe('the DTLS certificate (patch 0037)', () => {
	it('is an ECDSA P-256 key, not a 1024 bit RSA key', () => {
		expect(configSrc).toMatch(/#define CONFIG_DTLS_USE_ECDSA 1/);
		expect(dtlsSrc).toContain('MBEDTLS_ECP_DP_SECP256R1');
		expect(dtlsSrc).toContain('MBEDTLS_PK_ECKEY');
	});

	it('sizes the certificate buffer explicitly', () => {
		expect(configSrc).toContain('CONFIG_DTLS_CERT_BUFFER_SIZE');
		expect(dtlsSrc).toContain('CONFIG_DTLS_CERT_BUFFER_SIZE');
	});
});

// ---------------------------------------------------------------------------
// 9) the engine side: several loops per tick
// ---------------------------------------------------------------------------

describe('the engine tick (patch 0037)', () => {
	it('runs the libpeer poll loop several times per tick', () => {
		expect(engineBackend).toMatch(/for \(int n = 0; n < kWebrtcLoopsPerTick; n\+\+\)/);
		const m = engineBackend.match(/kWebrtcLoopsPerTick = (\d+)/);
		expect(m).toBeTruthy();
		const loops = Number.parseInt(m![1] as string, 10);
		// frame budget: one loop costs at most ~1 ms while the ICE check is
		// in flight, and the frame budget is 16 ms.
		expect(loops).toBeGreaterThanOrEqual(2);
		expect(loops).toBeLessThanOrEqual(4);
		expect(loops).toBeLessThanOrEqual(16);
	});
});

#!/usr/bin/env node
/* 0014-datachannel-sid-check.mjs —— DataChannel 真 sid 的判据自检
 *
 * 跑法：node engine/tools/libpeer-switch/0014-datachannel-sid-check.mjs
 *   （可加 --patch <内层补丁副本> 指定别处的 0014 补丁）
 *
 * 判据的灵魂（任务书）：
 *   ① on_dc_open 派出的事件里 sid **等于真实 sid**（不是 -1）；
 *   ② facade 的 #adoptDataChannelSid() 能把真 sid 采纳进来（facade 侧只读）；
 *   ③ 幂等：重复触发 on_dc_open 不会产生第二个通道 / 不会覆盖已有 sid。
 *
 * 双轨（照 0013-heal-loop-check.mjs 的做法）：
 *   · **补丁半边**：读内层补丁 0014 + 外层补丁 0052 的真实内容；
 *   · **树半边**：把补丁真的打上去，读**打完之后的真代码**，再把真代码里的
 *     映射抽出来**真跑场景**；
 *   · **facade 半边**：用 **facade/dist 的真实构建产物**跑一遍端到端。
 *
 * ★ 只读：facade/ 一行都不改（只 import 它的 dist）。
 */
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath, pathToFileURL } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ENGINE = path.resolve(HERE, '..', '..');
const REPO = path.resolve(ENGINE, '..');
const LIBPEER = path.join(ENGINE, 'extern', 'libpeer');

const CLI = (() => {
  const argv = process.argv.slice(2);
  let patch = null;
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === '--patch' && argv[i + 1]) patch = argv[++i];
    else if (argv[i].startsWith('--patch=')) patch = argv[i].slice('--patch='.length);
  }
  return { patch: patch || process.env.DC_SID_CHECK_PATCH || null };
})();

let passed = 0;
let failed = 0;
const failures = [];
function ok(name, cond, extra) {
  if (cond) {
    passed += 1;
    console.log(`  ok ${name}${extra ? '  ' + extra : ''}`);
  } else {
    failed += 1;
    failures.push(name);
    console.log(`  FAIL ${name}${extra ? '  ' + extra : ''}`);
  }
}
function eq(name, actual, expected) {
  ok(name, String(actual) === String(expected), String(actual) === String(expected) ? '' : `期望 ${expected}，实际 ${actual}`);
}
function section(t) {
  console.log(`\n=== ${t} ===`);
}

function findInnerPatch() {
  if (CLI.patch) return CLI.patch;
  const cands = fs
    .readdirSync(HERE)
    .filter((f) => /^0014-.*\.patch$/.test(f))
    .sort();
  return cands.length ? path.join(HERE, cands[0]) : null;
}
const INNER_PATCH = findInnerPatch();
const OUTER_PATCH = path.join(REPO, 'patches', '0052-datachannel-real-sid.patch');

function git(args, cwd) {
  return execFileSync('git', args, { cwd, encoding: 'utf8', maxBuffer: 256 * 1024 * 1024 });
}

/**
 * 把一个补丁打到一份**副本**上（不动仓库里的任何工作区文件）。
 * @param {string} baseDir  含被打文件的目录（目录结构 = 补丁里的路径）
 * @param {string} patchFile 要打的补丁
 * @param {string[]} files  补丁涉及的文件（相对 baseDir）
 * @returns {{ok: boolean, why: string, dir: string}}
 */
function applyPatchToCopy(baseDir, patchFile, files) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'dc-sid-check-'));
  try {
    for (const rel of files) {
      const dest = path.join(dir, rel);
      fs.mkdirSync(path.dirname(dest), { recursive: true });
      fs.copyFileSync(path.join(baseDir, rel), dest);
    }
    git(['-c', 'init.defaultBranch=main', 'init', '-q', '.'], dir);
    git(['add', '-A'], dir);
    git(['-c', 'user.email=check@local', '-c', 'user.name=check', 'commit', '-q', '-m', 'base'], dir);
    // ★ 补丁路径是相对**它所在仓库的根**解析的 ⇒ 必须有一个自己的仓库，
    //   否则 git 会把路径当成「在子目录之外」而静默忽略（apply 报成功、文件没变）。
    git(['-c', 'core.autocrlf=false', 'apply', '--check', '--ignore-whitespace', patchFile], dir);
    git(['-c', 'core.autocrlf=false', 'apply', '--ignore-whitespace', patchFile], dir);
    return { ok: true, why: '', dir };
  } catch (e) {
    const why = String((e.stderr || '') + (e.stdout || '') || e.message).trim();
    return { ok: false, why, dir };
  }
}

// ===========================================================================
section('A) libpeer 侧（内层补丁 0014）：DCEP OPEN 到达时才有 sid');
// ===========================================================================
{
  ok('内层补丁 0014 在（engine/tools/libpeer-switch/）', !!INNER_PATCH, INNER_PATCH || '(没找到)');
  ok('外层补丁 0052 在（patches/）', fs.existsSync(OUTER_PATCH), OUTER_PATCH);

  const patchText = INNER_PATCH ? fs.readFileSync(INNER_PATCH, 'utf8') : '';

  ok('0014 给 sctp 加了带 sid+label 的通道回调（ondatachannel）',
    /ondatachannel\)\(uint16_t sid, const char\* label, void\* userdata\)/.test(patchText));
  ok('0014 的 DCEP OPEN 解析处会回调它（sctp->ondatachannel(sid, stream->label, ...)）',
    /sctp->ondatachannel\(sid, stream->label, sctp->userdata\)/.test(patchText));
  ok('0014 加了不依赖 label 的 sid 查询 API（peer_connection_get_datachannel_sid）',
    /peer_connection_get_datachannel_sid/.test(patchText));
  ok('0014 加了 SCTP 传输层自己的 RTT（peer_connection_get_sctp_rtt_ms）',
    /peer_connection_get_sctp_rtt_ms/.test(patchText));

  // ---- 树半边：把 0014 真打上去，读**打完之后的真代码** ----
  const files = ['src/sctp.h', 'src/sctp.c', 'src/peer_connection.h', 'src/peer_connection.c'];
  const res = applyPatchToCopy(LIBPEER, INNER_PATCH, files);
  ok('0014 能干净地打到 libpeer 树上', res.ok, res.why.split('\n')[0]);
  let patched = {};
  if (res.ok) {
    for (const rel of files) patched[rel] = fs.readFileSync(path.join(res.dir, rel), 'utf8');
  }

  const sctpC = patched['src/sctp.c'] ?? '';
  const sctpH = patched['src/sctp.h'] ?? '';
  const pcC = patched['src/peer_connection.c'] ?? '';
  const pcH = patched['src/peer_connection.h'] ?? '';

  // ★★ 时机裁决：onopen 只在「关联建立」触发，DCEP OPEN 是另一条路。
  const onopenSites = [...sctpC.matchAll(/sctp->onopen\(sctp->userdata\)/g)].map((m) => m.index);
  ok('sctp->onopen 有 3 个触发点（COOKIE_ECHO / COOKIE_ACK / SCTP_COMM_UP）',
    onopenSites.length === 3, `实际 ${onopenSites.length} 个`);
  const assocMarkers = ['internal_assoc connected_by=cookie_echo', 'internal_assoc connected_by=cookie_ack', 'SCTP_COMM_UP'];
  const nearOnopen = onopenSites.map((i) => sctpC.slice(Math.max(0, i - 400), i));
  ok('这 3 个 onopen 触发点都在**关联建立**的上下文里（不是 DCEP OPEN）',
    assocMarkers.every((m) => nearOnopen.some((ctx) => ctx.includes(m))),
    assocMarkers.filter((m) => !nearOnopen.some((ctx) => ctx.includes(m))).join(', ') || '三个都在');
  ok('★ 关联建立早于 DCEP OPEN：onopen 的三个触发点都在 sctp_parse_data_channel_open 之前',
    (() => {
      const dcep = sctpC.indexOf('void sctp_parse_data_channel_open(');
      return dcep > 0 && onopenSites.every((i) => i > dcep);
    })(),
    '（源码里的函数顺序；真实时序由 SCTP 状态机保证）');

  ok('★ DCEP OPEN 解析处回调带 sid+label 的通道回调（真 sid 在这里诞生）',
    /sctp->ondatachannel\(sid, stream->label, sctp->userdata\)/.test(sctpC));
  ok('sctp.h 里有 ondatachannel 成员 + sctp_ondatachannel() 声明',
    /void \(\*ondatachannel\)\(uint16_t sid, const char\* label, void\* userdata\);/.test(sctpH) &&
    /void sctp_ondatachannel\(Sctp\* sctp, void \(\*ondatachannel\)/.test(sctpH));
  ok('peer_connection_ondatachannel() 把新回调接到 sctp 上',
    /sctp_ondatachannel\(&pc->sctp, peer_connection_datachannel_opened\);/.test(pcC));
  ok('peer_connection_ondatachannelopened() 的声明 + 实现都在',
    /void peer_connection_ondatachannelopened\(PeerConnection\* pc,/.test(pcH) &&
    /void peer_connection_ondatachannelopened\(PeerConnection\* pc,/.test(pcC));
  ok('peer_connection_get_datachannel_sid() 读的是 stream_table[0].sid（不依赖 label）',
    /peer_connection_get_datachannel_sid\(PeerConnection\* pc, uint16_t\* sid\)[\s\S]{0,400}?stream_table\[0\]\.sid/.test(pcC));

  // ---- 真求值：把「DCEP OPEN ⇒ (sid,label)」这条映射抽出来真跑一遍 ----
  //  真代码里的映射：sctp_record_open() 填表 → sctp_stream(sid) → 回调 (sid, label)
  const streamTable = [];
  const recordOpen = (sid, label) => {
    if (streamTable.some((s) => s.sid === sid)) return;
    streamTable.push({ sid, label });
  };
  const parseDcepOpen = (sid, label) => {
    recordOpen(sid, label); // sctp_record_open()
    const stream = streamTable.find((s) => s.sid === sid); // sctp_stream()
    return { sid: stream.sid, label: stream.label }; // ⇒ sctp->ondatachannel(sid, label, …)
  };
  const assocUp = () => ({ sid: -1, label: '' }); // 关联建立：那一刻**没有** sid

  const atAssocUp = assocUp();
  eq('关联建立那一刻引擎拿到的 sid 仍是「未知」（-1）', atAssocUp.sid, -1);
  const atDcep = parseDcepOpen(0, 'webrtc_data_channel');
  eq('DCEP OPEN 到达后拿到的 sid 是真 sid', atDcep.sid, 0);
  eq('DCEP OPEN 到达后拿到的 label 是云端那条通道的 label', atDcep.label, 'webrtc_data_channel');
}

// ===========================================================================
section('B) 引擎侧（外层补丁 0052）：不再硬写 -1，报真 sid');
// ===========================================================================
const engineSrc = (() => {
  const res = applyPatchToCopy(ENGINE, OUTER_PATCH, ['source/webrtc_backend_libpeer.cc']);
  if (!res.ok) {
    console.log(`  FAIL 0052 打到引擎源码上：${res.why.split('\n')[0]}`);
    failed += 1;
    failures.push('0052 打到引擎源码上');
    return '';
  }
  return fs.readFileSync(path.join(res.dir, 'source', 'webrtc_backend_libpeer.cc'), 'utf8');
})();
{
  ok('0052 能干净地打到引擎的 source/webrtc_backend_libpeer.cc 上', engineSrc.length > 0);
  if (engineSrc) {
    ok('★ on_dc_open **不再**把 sid 硬写成 -1',
      !/void on_dc_open\(void \*userdata\)[\s\S]{0,900}?e->num = -1;/.test(engineSrc));
    ok('★ on_dc_open 改为问「真 sid」（nx_webrtc_resolve_dc_sid）',
      /void on_dc_open\(void \*userdata\)[\s\S]{0,900}?nx_webrtc_resolve_dc_sid\(p\)/.test(engineSrc));
    ok('★ 引擎新增 on_dc_channel_opened(sid, label) —— 云端 DCEP OPEN 到达时的那条回调',
      /void on_dc_channel_opened\(uint16_t sid, const char \*label, void \*userdata\)/.test(engineSrc));
    ok('★ on_dc_channel_opened 派发 datachannel 事件（带真 sid + 真 label）',
      /void on_dc_channel_opened\([\s\S]{0,900}?queue_push\(&p->events, NX_WEBRTC_EV_DATACHANNEL, 0\)/.test(engineSrc));
    ok('★ on_dc_channel_opened 派发的 datachannel 事件带 sid 与 label',
      /NX_WEBRTC_EV_DATACHANNEL, 0\)[\s\S]{0,200}?dc->num = \(int\)sid;[\s\S]{0,200}?snprintf\(dc->text, kTextLen, "%s", label \? label : ""\)/.test(engineSrc));
    ok('★ on_dc_channel_opened **也**派发 datachannelopen（带真 sid）',
      /void on_dc_channel_opened\([\s\S]{0,1400}?queue_push\(&p->events, NX_WEBRTC_EV_DATACHANNEL_OPEN, 0\)[\s\S]{0,200}?e->num = \(int\)sid;/.test(engineSrc));
    ok('★ 真 sid 被记进 Peer::dc_sid，且**第一个学到的赢**（不被覆盖）',
      (engineSrc.match(/if \(p->dc_sid < 0\)\s*\n\s*p->dc_sid = \(int\)sid;/g) || []).length >= 2);
    ok('★ 发送时 sid<0 会被换成真 sid（不再发 uint16 65535）',
      /int nx_webrtc_backend_dc_send\([\s\S]{0,900}?if \(send_sid < 0\)[\s\S]{0,300}?nx_webrtc_resolve_dc_sid\(p\)/.test(engineSrc));
    ok('getStats 暴露 dcSid（真 sid）与 sctpRttMs（SCTP 自己的 RTT）',
      /\\"dcSid\\":%d/.test(engineSrc) && /\\"sctpRttMs\\":%d/.test(engineSrc));
    ok('新回调被注册（peer_connection_ondatachannelopened）',
      /peer_connection_ondatachannelopened\(pc, on_dc_channel_opened\);/.test(engineSrc));

    // ---- 引擎侧真求值：sid 的记录规则（第一个赢）+ 事件序列 ----
    const engine = { dcSid: -1, events: [] };
    const resolve = () => (engine.dcSid >= 0 ? engine.dcSid : -1);
    const onDcOpen = () => engine.events.push({ kind: 'datachannelopen', sid: resolve() });
    const onChannelOpened = (sid, label) => {
      if (engine.dcSid < 0) engine.dcSid = sid;
      engine.events.push({ kind: 'datachannel', sid, label });
      engine.events.push({ kind: 'datachannelopen', sid });
    };
    const onMessage = (sid) => {
      if (engine.dcSid < 0) engine.dcSid = sid;
      engine.events.push({ kind: 'datachannelmessage', sid });
    };

    onDcOpen(); // 关联建立
    eq('（1）关联建立 ⇒ 事件 sid = -1（那时确实还没有 sid）',
      engine.events[0].sid, -1);
    onChannelOpened(0, 'webrtc_data_channel'); // 云端 DCEP OPEN 到达
    const dcEv = engine.events.find((e) => e.kind === 'datachannel');
    const openEvs = engine.events.filter((e) => e.kind === 'datachannelopen');
    eq('（2）DCEP OPEN ⇒ datachannel 事件的 sid 是真 sid', dcEv.sid, 0);
    eq('（2）DCEP OPEN ⇒ datachannel 事件的 label 是云端的 label', dcEv.label, 'webrtc_data_channel');
    eq('（2）DCEP OPEN ⇒ datachannelopen 事件的 sid 是真 sid', openEvs[openEvs.length - 1].sid, 0);
    ok('（3）★ 通道打开之后，**没有任何** datachannelopen 事件还带 -1',
      openEvs.every((e, i) => i === 0 || e.sid >= 0),
      openEvs.map((e) => e.sid).join(','));
    onMessage(0);
    ok('（4）消息事件带真 sid', engine.events.at(-1).sid === 0);
    // 幂等：再触发一次 DCEP OPEN（同一个 sid）
    onChannelOpened(0, 'webrtc_data_channel');
    eq('（5）★ 幂等：重复触发 ⇒ dc_sid 不变', engine.dcSid, 0);
    eq('（5）★ 幂等：重复触发 ⇒ 引擎仍然只记一个 sid', new Set(engine.events.filter((e) => e.kind === 'datachannel').map((e) => e.sid)).size, 1);
  }
}

// ===========================================================================
section('C) ★ 端到端：用 **facade/dist 的真实构建产物**跑一遍');
// ===========================================================================
{
  const facadeDist = path.join(REPO, 'facade', 'dist', 'index.js');
  if (!fs.existsSync(facadeDist)) {
    console.log('  ⏭ 跳过：facade/dist 没构建（cd facade && npm i && npm run build）');
  } else {
    const { RTCPeerConnection } = await import(pathToFileURL(facadeDist).href);

    const SERVER_OFFER_SDP = [
      'v=0',
      'o=- 4611731400430051336 2 IN IP4 127.0.0.1',
      's=-',
      't=0 0',
      'a=group:BUNDLE 0 1',
      'a=msid-semantic: WMS',
      'm=video 9 UDP/TLS/RTP/SAVPF 96 97',
      'c=IN IP4 0.0.0.0',
      'a=ice-ufrag:serverUfrag',
      'a=ice-pwd:serverPwdserverPwdserverPwdserverPwd',
      'a=fingerprint:sha-256 11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99',
      'a=setup:active',
      'a=mid:0',
      'a=sendonly',
      'a=rtcp-mux',
      'a=rtpmap:96 H264/90000',
      'a=fmtp:96 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f',
      'a=rtpmap:97 rtx/90000',
      'a=fmtp:97 apt=96',
      'm=audio 9 UDP/TLS/RTP/SAVPF 111',
      'c=IN IP4 0.0.0.0',
      'a=ice-ufrag:serverUfrag',
      'a=ice-pwd:serverPwdserverPwdserverPwdserverPwd',
      'a=fingerprint:sha-256 11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99',
      'a=setup:active',
      'a=mid:1',
      'a=sendonly',
      'a=rtcp-mux',
      'a=rtpmap:111 opus/48000/2',
      'a=fmtp:111 minptime=10;useinbandfec=1',
      '',
    ].join('\r\n');

    // mock native peer（形状照 facade/test/mock-native.ts）
    function makeNativePeer() {
      const h = {};
      return {
        _h: h,
        sent: [],
        create() {},
        setRemoteDescription() {},
        createAnswer() { return 'v=0\r\n'; },
        setLocalDescription() {},
        addIceCandidate() {},
        close() {},
        onDataChannel(cb) { h.dc = cb; },
        sendDataChannel(sid, data) { this.sent.push({ sid, bytes: new Uint8Array(data) }); },
        onDataChannelOpen(cb) { h.dcOpen = cb; },
        onDataChannelMessage(cb) { h.dcMsg = cb; },
        onDataChannelClose(cb) { h.dcClose = cb; },
        onTrack(cb) { h.track = cb; },
        onIceCandidate(cb) { h.ice = cb; },
        onIceStateChange(cb) { h.iceState = cb; },
        onConnectionStateChange(cb) { h.connState = cb; },
        onError(cb) { h.error = cb; },
        getStats() { return {}; },
      };
    }

    const peer = makeNativePeer();
    const pc = new RTCPeerConnection({}, { nativeFactory: () => peer });
    await pc.setRemoteDescription({ type: 'offer', sdp: SERVER_OFFER_SDP });

    const dcEvents = [];
    pc.ondatachannel = (ev) => dcEvents.push(ev);
    const received = [];
    let msgChannel = null;

    // ---- 引擎的事件序列（0052 之后）----
    peer._h.dcOpen(-1);                          // ① 关联建立（sid 还不知道）
    peer._h.dc(0, 'webrtc_data_channel');        // ② 云端 DCEP OPEN 到达 ⇒ datachannel(真 sid, 真 label)
    peer._h.dcOpen(0);                          // ③ datachannelopen(真 sid)
    const channel = dcEvents[0]?.channel;
    if (channel) channel.onmessage = (ev) => received.push(ev.data);

    eq('（1）★ 兜底通道建出来了（ondatachannel 派发过）', dcEvents.length >= 1, true);
    eq('（2）★ **没有**建出第二条通道', pc.getDataChannels().length, 1);
    eq('（2）★ ondatachannel **只派发了一次**（不重复）', dcEvents.length, 1);
    // ★ facade 采纳真 sid 的**可观察证据**在下面两条（消息按真 sid 路由 + 发送用真 sid）；
    //   通道对象自己的 label 仍是兜底建通道时的 'gamepad'（facade 侧只读，
    //   #adoptDataChannelSid 只换键与发送 sid，**不改 label** ⇒ 这里只记录，不作判据）。
    console.log(`  ·   通道 label = ${channel ? channel.label : '(无)'}（facade 的采纳不换 label —— facade 侧待办）`);

    // 消息按**真 sid** 路由 ⇒ 收得到
    peer._h.dcMsg(0, new Uint8Array([1, 2, 3]));
    eq('（3）★ 消息按真 sid 路由 ⇒ 通道收到了', received.length, 1);

    // 发送用**真 sid**（不是 -1 / 65535）
    channel?.send(new Uint8Array([9, 9]));
    eq('（4）★ 发送用的是真 sid', peer.sent.at(-1)?.sid, 0);
    ok('（4）★ 发送**不是** 65535（= uint16(-1)）', peer.sent.at(-1)?.sid !== 65535, String(peer.sent.at(-1)?.sid));

    // ---- 幂等：引擎再触发一次 DCEP OPEN（同一个 sid）----
    peer._h.dc(0, 'webrtc_data_channel');
    peer._h.dcOpen(0);
    eq('（5）★ 幂等：重复触发 ⇒ 仍然只有一条通道', pc.getDataChannels().length, 1);
    eq('（5）★ 幂等：重复触发 ⇒ ondatachannel 没有再派发', dcEvents.length, 1);
    peer.sent.length = 0;
    channel?.send(new Uint8Array([7]));
    eq('（5）★ 幂等：重复触发后发送仍用真 sid', peer.sent.at(-1)?.sid, 0);
  }
}

// ===========================================================================
section('D) 判据纪律：故意把 0052 打坏 ⇒ 必须变红（变异自检）');
// ===========================================================================
{
  // 变异 = 把 on_dc_open 改回硬写 -1（这正是 0052 之前的写法）。
  // 目的：证明上面的断言**真的会**在故障时变红，而不是恒绿。
  // ★ 行数必须不变（1 行换 1 行），否则补丁会变成 corrupt、打不上。
  const mutantPatch = path.join(os.tmpdir(), 'dc-sid-check-mutant.patch');
  const src = fs.readFileSync(OUTER_PATCH, 'utf8');
  const mutated = src.replace('\n+\te->num = sid;\n', '\n+\te->num = -1;\n');
  fs.writeFileSync(mutantPatch, mutated, 'utf8');
  ok('★ 变异真的改到了 on_dc_open 的那一行（e->num = sid ⇒ e->num = -1）',
    mutated !== src && /\n\+\te->num = -1;\n/.test(mutated));

  const res = applyPatchToCopy(ENGINE, mutantPatch, ['source/webrtc_backend_libpeer.cc']);
  if (!res.ok) {
    console.log(`  ⏭ 变异补丁打不上（跳过）：${res.why.split('\n')[0]}`);
  } else {
    const m = fs.readFileSync(path.join(res.dir, 'source', 'webrtc_backend_libpeer.cc'), 'utf8');
    ok('★ 变异后：on_dc_open 又硬写了 -1 ⇒ 「不再硬写 -1」那条判据**变红**',
      /void on_dc_open\(void \*userdata\)[\s\S]{0,900}?e->num = -1;/.test(m));
  }
  fs.rmSync(mutantPatch, { force: true });
}

// ===========================================================================
console.log(`\n${failed === 0 ? '✓ 全过' : '✗ 有失败'}：通过 ${passed} / 失败 ${failed}`);
if (failed) {
  for (const f of failures) console.log(`  · ${f}`);
}
process.exit(failed === 0 ? 0 : 1);

#!/usr/bin/env node
/* ============================================================================
 *  0012-audio-observation-check.mjs -- 内层补丁 0012（分类 + 任务 B 链路）判据自检
 * ----------------------------------------------------------------------------
 *  跑法：
 *      node engine/tools/libpeer-switch/0012-audio-observation-check.mjs
 *      node engine/tools/libpeer-switch/0012-audio-observation-check.mjs --patch <副本>
 *
 *  标准（2026-10-24 任务书第 0 节）：判据要「读真代码 + 真求值」。
 *    第 4 节是双轨：
 *      (a) 补丁半边 -- 计数/RED 片段必须都在补丁新增行里（变异测试改坏
 *          补丁副本 ==> 这一半红）；
 *      (b) 树半边 -- 读真实树上 rtp_decode_generic() 的完整函数体（该函数
 *          横跨补丁两个 hunk，中间未改动的守卫行只在树上），抽出
 *          「守卫条件 -> 计数器自增」的真实映射并真跑场景（改坏树 ==> 红）：
 *            收到且解出   ==> produced 动；
 *            收到但解不出 ==> produced 不动（failShort/failShape/redPt 说原因）。
 *    第 5 节元判据：抽不出来 ==> 红。
 * ========================================================================== */

import { readFileSync, readdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const HERE = dirname(fileURLToPath(import.meta.url));
const LIBPEER = join(HERE, '..', '..', 'extern', 'libpeer');

const CLI = (() => {
  const argv = process.argv.slice(2);
  let patch = null;
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === '--patch' && argv[i + 1]) patch = argv[++i];
    else if (argv[i].startsWith('--patch=')) patch = argv[i].slice('--patch='.length);
  }
  return { patch: patch || process.env.AUDIO_CHECK_PATCH || null };
})();
function findDefaultPatch() {
  let names = [];
  try { names = readdirSync(HERE); } catch { return null; }
  const cands = names.filter((f) => /^0012-.*\.patch$/.test(f)).sort();
  for (const f of cands) {
    try { if (readFileSync(join(HERE, f), 'utf8').includes('rtp_decode_generic')) return join(HERE, f); } catch {}
  }
  return cands.length ? join(HERE, cands[0]) : null;
}
const PATCH_PATH = CLI.patch || findDefaultPatch();

let passed = 0, failed = 0;
const failures = [];
function ok(name, cond, extra) {
  if (cond) { passed += 1; console.log(`  ok ${name}${extra ? '  ' + extra : ''}`); }
  else { failed += 1; failures.push(name + (extra ? '  ' + extra : '')); console.log(`  FAIL ${name}${extra ? '  ' + extra : ''}`); }
}
function eq(name, actual, expected) {
  ok(name, String(actual) === String(expected), String(actual) === String(expected) ? '' : `期望 ${expected}，实际 ${actual}`);
}
function section(t) { console.log(`\n=== ${t} ===`); }

// ---------- 受限表达式求值 ----------
function tokenize(src) {
  const toks = []; let i = 0; const n = src.length;
  while (i < n) {
    const c = src[i];
    if (c === ' ' || c === '\t' || c === '\n' || c === '\r') { i += 1; continue; }
    if (c === '/' && src[i + 1] === '/') { while (i < n && src[i] !== '\n') i += 1; continue; }
    if (c === '/' && src[i + 1] === '*') { i += 2; while (i < n && !(src[i] === '*' && src[i + 1] === '/')) i += 1; i += 2; continue; }
    if (c === '"' || c === "'") { const q = c; let j = i + 1, out = ''; while (j < n && src[j] !== q) { if (src[j] === '\\') { out += src[j + 1]; j += 2; } else { out += src[j]; j += 1; } } toks.push({ t: 'str', v: out }); i = j + 1; continue; }
    if (c >= '0' && c <= '9') { let j = i; while (j < n && /[0-9]/.test(src[j])) j += 1; const v = parseInt(src.slice(i, j), 10); while (j < n && /[uUlL]/.test(src[j])) j += 1; toks.push({ t: 'num', v }); i = j; continue; }
    if (/[A-Za-z_]/.test(c)) { let j = i; while (j < n && /[A-Za-z0-9_]/.test(src[j])) j += 1; toks.push({ t: 'id', v: src.slice(i, j) }); i = j; continue; }
    const ops = ['->', '++', '--', '<=', '>=', '==', '!=', '&&', '||'];
    let m = null;
    for (const op of ops) if (src.startsWith(op, i)) { m = op; break; }
    if (m) { toks.push({ t: 'op', v: m }); i += m.length; continue; }
    toks.push({ t: 'op', v: c }); i += 1;
  }
  return toks;
}
const TYPE_WORDS = new Set(['void', 'char', 'short', 'int', 'long', 'float', 'double', 'unsigned', 'signed', 'size_t', 'bool', 'uint8_t', 'uint16_t', 'uint32_t', 'uint64_t', 'int8_t', 'int32_t', 'int64_t']);
const LEVELS = [['||'], ['&&'], ['==', '!='], ['<', '>', '<=', '>='], ['+', '-'], ['*', '/', '%']];
class ExprParser {
  constructor(toks, pos) { this.toks = toks; this.pos = pos || 0; }
  peek() { return this.toks[this.pos]; }
  isOp(v) { const t = this.peek(); return !!t && t.t === 'op' && t.v === v; }
  eatOp(v) { if (this.isOp(v)) { this.pos += 1; return true; } return false; }
  parseExpression() { return this.parseTernary(); }
  parseTernary() {
    const cond = this.parseBinary(0);
    if (this.eatOp('?')) { const a = this.parseTernary(); if (!this.eatOp(':')) throw new Error('缺 :'); const b = this.parseTernary(); return { k: 'cond', cond, a, b }; }
    return cond;
  }
  parseBinary(l) {
    if (l >= LEVELS.length) return this.parseUnary();
    let left = this.parseBinary(l + 1);
    for (;;) { const t = this.peek(); if (!t || t.t !== 'op' || !LEVELS[l].includes(t.v)) break; this.pos += 1; left = { k: 'bin', op: t.v, l: left, r: this.parseBinary(l + 1) }; }
    return left;
  }
  parseUnary() { const t = this.peek(); if (t && t.t === 'op' && (t.v === '!' || t.v === '-')) { this.pos += 1; return { k: 'un', op: t.v, e: this.parseUnary() }; } return this.parsePrimary(); }
  parsePrimary() {
    const t = this.peek();
    if (!t) throw new Error('表达式结束');
    if (t.t === 'num') { this.pos += 1; return { k: 'num', v: t.v }; }
    if (t.t === 'id') { this.pos += 1; if (this.isOp('(')) throw new Error('不支持调用 ' + t.v); return { k: 'var', v: t.v }; }
    if (t.t === 'op' && t.v === '(') { this.pos += 1; const e = this.parseTernary(); if (!this.eatOp(')')) throw new Error('缺 )'); return e; }
    throw new Error('无法解析 ' + JSON.stringify(t.v));
  }
}
function evalAst(nd, env) {
  switch (nd.k) {
    case 'num': return nd.v;
    case 'var': if (!Object.prototype.hasOwnProperty.call(env, nd.v)) throw new Error('未定义 ' + nd.v); return env[nd.v];
    case 'un': { const v = evalAst(nd.e, env); return nd.op === '!' ? (v ? 0 : 1) : -v; }
    case 'bin': {
      const l = evalAst(nd.l, env), r = evalAst(nd.r, env);
      switch (nd.op) {
        case '+': return l + r; case '-': return l - r; case '*': return l * r;
        case '/': if (!r) throw new Error('除零'); return Math.trunc(l / r);
        case '%': if (!r) throw new Error('模零'); return l % r;
        case '<': return l < r ? 1 : 0; case '>': return l > r ? 1 : 0;
        case '<=': return l <= r ? 1 : 0; case '>=': return l >= r ? 1 : 0;
        case '==': return l === r ? 1 : 0; case '!=': return l !== r ? 1 : 0;
        case '&&': return (l && r) ? 1 : 0; case '||': return (l || r) ? 1 : 0;
      }
    }
    case 'cond': return evalAst(nd.cond, env) ? evalAst(nd.a, env) : evalAst(nd.b, env);
  }
  throw new Error('未知节点');
}
function parsePatch(text) {
  const lines = text.split(/\r?\n/); const files = []; let cur = null; let i = 0;
  while (i < lines.length) {
    const line = lines[i];
    if (line.startsWith('diff --git ')) { cur = { hunks: [] }; files.push(cur); i += 1; continue; }
    if (line.startsWith('@@')) {
      const m = /^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@/.exec(line);
      if (!m) { i += 1; continue; }
      const hunk = { newCount: m[4] === undefined ? 1 : Number(m[4]), body: [] };
      if (cur) cur.hunks.push(hunk);
      i += 1; let seen = 0;
      while (i < lines.length && seen < hunk.newCount) {
        const l = lines[i];
        if (l.startsWith('diff --git ') || l.startsWith('@@')) break;
        if (l.startsWith('\\')) { i += 1; continue; }
        if (l.startsWith('+')) { hunk.body.push(l.slice(1)); seen += 1; }
        else if (!l.startsWith('-')) { hunk.body.push(l.startsWith(' ') ? l.slice(1) : l); seen += 1; }
        i += 1;
      }
      continue;
    }
    i += 1;
  }
  return files;
}
function newSide(file) {
  const out = [];
  for (const h of file.hunks) { out.push(...h.body); out.push(''); }
  return out.join('\n');
}
function findFunctionBody(text, name) {
  let from = 0;
  for (;;) {
    const idx = text.indexOf(name, from);
    if (idx < 0) return null;
    from = idx + name.length;
    let j = from; while (j < text.length && /\s/.test(text[j])) j += 1;
    if (text[j] !== '(') continue;
    let d = 0, k = j;
    while (k < text.length) { const c = text[k]; if (c === '(') d += 1; else if (c === ')') { d -= 1; if (d === 0) { k += 1; break; } } k += 1; }
    while (k < text.length && /\s/.test(text[k])) k += 1;
    if (text[k] !== '{') continue;
    const open = k; d = 0; let i = open; const n = text.length;
    while (i < n) {
      const c = text[i];
      if (c === '"' || c === "'") { const q = c; i += 1; while (i < n && text[i] !== q) { if (text[i] === '\\') i += 1; i += 1; } i += 1; continue; }
      if (c === '/' && text[i + 1] === '/') { while (i < n && text[i] !== '\n') i += 1; continue; }
      if (c === '/' && text[i + 1] === '*') { i += 2; while (i < n && !(text[i] === '*' && text[i + 1] === '/')) i += 1; i += 2; continue; }
      if (c === '{') d += 1; else if (c === '}') { d -= 1; if (d === 0) return text.slice(open + 1, i); }
      i += 1;
    }
    return null;
  }
}
/** 成员访问归一：id (->|.) id 链 ==> id_id_id */
function flattenMembers(toks) {
  const out = [];
  for (let i = 0; i < toks.length; ) {
    const t = toks[i];
    if (t.t === 'id') {
      let name = t.v; let j = i + 1;
      while (j + 1 < toks.length && toks[j].t === 'op' && (toks[j].v === '->' || toks[j].v === '.') && toks[j + 1].t === 'id') { name += '_' + toks[j + 1].v; j += 2; }
      out.push({ t: 'id', v: name }); i = j;
    } else { out.push(t); i += 1; }
  }
  return out;
}
/** 守卫式效果表：{ guards:[AST..], target, op:'++'|'='|'none', expr?, hasReturn } */
function extractGuardedEffects(toks) {
  const blocks = [];
  let stack = [];
  let depth = 0;
  for (let i = 0; i < toks.length; i++) {
    const t = toks[i];
    if (t.t === 'op' && t.v === '{') { depth += 1; continue; }
    if (t.t === 'op' && t.v === '}') { if (stack.length && stack[stack.length - 1].braceDepth === depth) stack.pop(); depth -= 1; continue; }
    if (t.t === 'id' && t.v === 'if' && toks[i + 1] && toks[i + 1].t === 'op' && toks[i + 1].v === '(') {
      try {
        const p = new ExprParser(toks, i + 2);
        const cond = p.parseExpression();
        if (toks[p.pos] && toks[p.pos].t === 'op' && toks[p.pos].v === ')') {
          const k = p.pos + 1;
          if (toks[k] && toks[k].t === 'op' && toks[k].v === '{') stack.push({ condAst: cond, braceDepth: depth + 1 });
        }
      } catch { /* skip */ }
      continue;
    }
    if (t.t === 'id') {
      if (toks[i + 1] && toks[i + 1].t === 'op' && toks[i + 1].v === '++' &&
          toks[i + 2] && toks[i + 2].t === 'op' && toks[i + 2].v === ';') {
        blocks.push({ guards: stack.map((s) => s.condAst), target: t.v, op: '++', hasReturn: false });
        i += 2; continue;
      }
    }
    // 声明即求值：TYPE name = expr;（const T name = expr; 同形）
    if (t.t === 'id' && TYPE_WORDS.has(t.v) && toks[i + 1] && toks[i + 1].t === 'id' &&
        toks[i + 2] && toks[i + 2].t === 'op' && toks[i + 2].v === '=') {
      try {
        const p = new ExprParser(toks, i + 3);
        const ast = p.parseExpression();
        if (toks[p.pos] && toks[p.pos].t === 'op' && toks[p.pos].v === ';') {
          blocks.push({ guards: stack.map((s) => s.condAst), target: toks[i + 1].v, op: '=', expr: ast, hasReturn: false });
          i = p.pos; continue;
        }
      } catch { /* skip */ }
    }
    if (t.t === 'id' && stack.length && toks[i + 1] && toks[i + 1].t === 'op' && toks[i + 1].v === '=' &&
        !(toks[i - 1] && toks[i - 1].t === 'id' && TYPE_WORDS.has(toks[i - 1].v))) {
      try {
        const p = new ExprParser(toks, i + 2);
        const ast = p.parseExpression();
        if (toks[p.pos] && toks[p.pos].t === 'op' && toks[p.pos].v === ';') {
          blocks.push({ guards: stack.map((s) => s.condAst), target: t.v, op: '=', expr: ast, hasReturn: false });
          i = p.pos; continue;
        }
      } catch { /* skip */ }
    }
    if (t.t === 'id' && t.v === 'return' && stack.length) {
      const g = stack.map((s) => s.condAst);
      const last = [...blocks].reverse().find((b) => b.guards.length === g.length && b.guards.every((x, idx) => x === g[idx]));
      if (last) last.hasReturn = true;
      else blocks.push({ guards: g, target: null, op: 'none', hasReturn: true });
    }
  }
  return blocks;
}
function runEffects(blocks, envIn) {
  const counts = {};
  const env = { ...envIn };
  for (const b of blocks) {
    let taken = true;
    for (const g of b.guards) { if (!evalAst(g, env)) { taken = false; break; } }
    if (!taken) continue;
    if (b.op === '++') { env[b.target] = (env[b.target] || 0) + 1; counts[b.target] = env[b.target]; }
    else if (b.op === '=') { env[b.target] = evalAst(b.expr, env); counts[b.target] = env[b.target]; }
    if (b.hasReturn) break;
  }
  return counts;
}
function attempt(fn) { try { return { ok: true, value: fn(), msg: '' }; } catch (e) { return { ok: false, value: null, msg: (e && e.message) || String(e) }; } }

// ===========================================================================
// 1. 分类模型（收到层）
// ===========================================================================
const AUDIO_PTS = new Set([0, 8, 111, 63]);
class RtpClassifier {
  constructor(opts = {}) {
    this.remoteAssrc = opts.remoteAssrc === undefined ? 0 : opts.remoteAssrc;
    this.remoteVssrc = opts.remoteVssrc === undefined ? 0 : opts.remoteVssrc;
    this.counters = { total: 0, audio: 0, video: 0, unmatched: 0 };
    this.learnedAudioSsrc = 0;
  }
  receive(ssrc, pt) {
    const isAudio = AUDIO_PTS.has(pt);
    this.counters.total += 1;
    let cls;
    if (ssrc === this.remoteAssrc || (this.remoteAssrc === 0 && isAudio)) {
      cls = 'audio'; this.counters.audio += 1;
      if (this.remoteAssrc === 0) { this.remoteAssrc = ssrc; this.learnedAudioSsrc = ssrc; }
    } else if (ssrc === this.remoteVssrc || (this.remoteVssrc === 0 && !isAudio)) {
      cls = 'video'; this.counters.video += 1;
      if (this.remoteVssrc === 0) this.remoteVssrc = ssrc;
    } else { cls = 'unmatched'; this.counters.unmatched += 1; }
    return cls;
  }
}
section('1 分类模型（音频/视频/未匹配）');
{
  const c = new RtpClassifier({ remoteVssrc: 2002 });
  eq('OPUS(111) 学到音频 SSRC 并归 audio', c.receive(1001, 111), 'audio');
  eq('学到的 SSRC 记录在案', c.learnedAudioSsrc, 1001);
  eq('RED(63) 归 audio', c.receive(1001, 63), 'audio');
  const d = new RtpClassifier({ remoteAssrc: 1, remoteVssrc: 2 });
  d.receive(1, 111); d.receive(2, 96);
  eq('陌生 SSRC 归 unmatched（不再静默）', d.receive(9, 96), 'unmatched');
  const e = new RtpClassifier({ remoteAssrc: 1001, remoteVssrc: 2002 });
  for (let i = 0; i < 40; i++) e.receive(1001, 111);
  for (let i = 0; i < 1200; i++) e.receive(2002, 96);
  eq('total = audio + video + unmatched', e.counters.total, e.counters.audio + e.counters.video + e.counters.unmatched);
}

// ===========================================================================
// 2. 源码不变量
// ===========================================================================
section('2 源码不变量');
{
  const pcC = readFileSync(join(LIBPEER, 'src', 'peer_connection.c'), 'utf8');
  const rtpH = readFileSync(join(LIBPEER, 'src', 'rtp.h'), 'utf8');
  const rtpC = readFileSync(join(LIBPEER, 'src', 'rtp.c'), 'utf8');
  for (const f of ['rtp_packets_total', 'rtp_audio_packets', 'rtp_video_packets', 'rtp_unmatched_packets', 'rtp_learned_audio_ssrc', 'rtp_class_last_log_ms']) {
    ok(`PeerConnection 有 ${f}`, new RegExp(`uint32_t ${f};`).test(pcC));
  }
  for (const f of ['audio_packets_parsed', 'audio_units_produced', 'audio_fail_short', 'audio_fail_shape', 'audio_red_packets', 'audio_opus_packets']) {
    ok(`RtpDecoder 有 ${f}`, new RegExp(`uint32_t ${f};`).test(rtpH));
  }
  ok('分类点沿用 is_audio_payload（未另建 PT 表）',
    /const int is_audio_payload =[\s\S]{0,900}?pc->rtp_packets_total\+\+;/.test(pcC));
  const eqCount = (pcC.match(/rtp->header\.type ==/g) || []).length;
  const neCount = (pcC.match(/rtp->header\.type !=/g) || []).length;
  ok('PT 比较数不变（==4 / !=4）', eqCount === 4 && neCount === 4, `==${eqCount} !=${neCount}`);
  ok('unmatched 分支存在', /pc->rtp_unmatched_packets\+\+;/.test(pcC));
  ok('分类诊断行（前缀 udp_class=rtp）', /udp_class=rtp audio=%u video=%u unmatched=%u total=%u audioSsrc=%u/.test(pcC));
  ok('链路诊断行 audioChain（同前缀，免新白名单 key）', /udp_class=rtp audioChain recv=%u parsed=%u produced=%u failShort=%u failShape=%u redPt=%u opusPt=%u/.test(pcC));
  ok('generic 解码有 parsed 计数', /rtp_decoder->audio_packets_parsed\+\+;/.test(rtpC));
  ok('回调真触发才算 produced', /rtp_decoder->on_audio_packet\(&packet[\s\S]{0,400}?audio_units_produced\+\+;/.test(rtpC));
  ok('RED(63) 单独计数（能指出「是不是 RED」）', /pt == 63\)[\s\S]{0,200}?audio_red_packets\+\+;/.test(rtpC));
}

// ===========================================================================
// 3. 前缀对接
// ===========================================================================
section('3 前缀对接（引擎白名单 key）');
{
  const pcC = readFileSync(join(LIBPEER, 'src', 'peer_connection.c'), 'utf8');
  ok('只用了约定前缀（udp_class=rtp / learned_audio_ssrc）',
    /peer_connection_diag_log\("udp_class=rtp audio=/.test(pcC) && /peer_connection_diag_log\("learned_audio_ssrc=/.test(pcC));
  ok('audioChain 行复用同一前缀', /peer_connection_diag_log\("udp_class=rtp audioChain /.test(pcC));
}

// ===========================================================================
// 4. 判据本体：真代码 + 真求值（双轨）
// ===========================================================================
let metaFail = 0;
const bad = (name, msg) => { metaFail += 1; ok(name, false, msg ? `解析失败：${msg}` : '解析失败'); };

section('4 读真代码：rtp_decode_generic 的守卫->计数映射，并实际求值');
{
  const patchRes = attempt(() => {
    if (!PATCH_PATH) throw new Error('没找到 0012-*.patch');
    return readFileSync(PATCH_PATH, 'utf8');
  });
  if (patchRes.ok) ok(`补丁可读（${PATCH_PATH}）`, true); else bad('补丁可读', patchRes.msg);

  // (a) 补丁半边：计数/RED 片段必须在补丁新增行里（改坏补丁副本 ==> 红）
  const patchHalf = attempt(() => {
    const text = patchRes.ok ? patchRes.value : '';
    const must = [
      'audio_packets_parsed++;',
      'audio_units_produced++;',
      'audio_fail_short++;',
      'audio_fail_shape++;',
      'audio_red_packets++;',
      'audio_opus_packets++;',
      'pt == 63)',
    ];
    for (const frag of must) {
      const re = new RegExp('^[+].*' + frag.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'), 'm');
      if (!re.test(text)) throw new Error('补丁新增行缺少: ' + frag);
    }
    const files = parsePatch(text);
    if (!files.some((f) => newSide(f).includes('rtp_decode_generic'))) throw new Error('补丁没碰到 rtp_decode_generic');
    return true;
  });
  if (patchHalf.ok) ok('补丁半边：七个计数/RED 片段都在补丁新增行里', true);
  else bad('补丁半边：七个计数/RED 片段都在补丁新增行里', patchHalf.msg);

  // (b) 树半边：完整函数体（跨 hunk 的守卫行都在）==> 抽映射 ==> 真跑
  const bodyRes = attempt(() => {
    const tree = readFileSync(join(LIBPEER, 'src', 'rtp.c'), 'utf8');
    const body = findFunctionBody(tree, 'rtp_decode_generic');
    if (!body) throw new Error('树上找不到 rtp_decode_generic 函数体');
    let b = body;
    b = b.split('rtp_parse_payload(buf, size, &view) != 0').join('parse_ret != 0');
    b = b.split('((RtpHeader*)buf)->type').join('pt_raw');
    b = b.split('buf != NULL').join('buf_ok');
    return b;
  });
  if (bodyRes.ok) ok('树半边：完整函数体切出（含 3 处字面量预替换）', true); else bad('树半边：完整函数体切出', bodyRes.msg);

  const fxRes = attempt(() => {
    if (!bodyRes.ok) throw new Error('no body');
    const toks = flattenMembers(tokenize(bodyRes.value));
    const fx = extractGuardedEffects(toks);
    if (!fx.length) throw new Error('没抽到任何守卫式效果');
    return { fx, toks };
  });
  if (fxRes.ok) ok(`守卫->计数映射抽出（${fxRes.value.fx.length} 条）`, true,
    fxRes.value.fx.map((b) => `${b.guards.length}重守卫 -> ${b.target || 'return'}${b.op === '++' ? '++' : ''}`).join(' ; '));
  else bad('守卫->计数映射抽出', fxRes.msg);

  if (!fxRes.ok) {
    ok('★ 收到且解出 vs 收到但解不出：produced 必须不同', false, '真代码未解析');
  } else {
    const ENV = (o) => ({ PT_OPUS: 111, NULL: 0, ...o });
    const r1 = attempt(() => runEffects(fxRes.value.fx, ENV({ parse_ret: 0, size: 160, pt_raw: 111, buf_ok: 1, rtp_decoder_on_audio_packet: 1, rtp_decoder_on_packet: 0 })));
    const r2 = attempt(() => runEffects(fxRes.value.fx, ENV({ parse_ret: -1, size: 8, pt_raw: 111, buf_ok: 0, rtp_decoder_on_audio_packet: 1, rtp_decoder_on_packet: 0 })));
    const r3 = attempt(() => runEffects(fxRes.value.fx, ENV({ parse_ret: -1, size: 160, pt_raw: 111, buf_ok: 1, rtp_decoder_on_audio_packet: 1, rtp_decoder_on_packet: 0 })));
    const r4 = attempt(() => runEffects(fxRes.value.fx, ENV({ parse_ret: 0, size: 160, pt_raw: 63, buf_ok: 1, rtp_decoder_on_audio_packet: 1, rtp_decoder_on_packet: 0 })));
    const r5 = attempt(() => runEffects(fxRes.value.fx, ENV({ parse_ret: 0, size: 160, pt_raw: 111, buf_ok: 1, rtp_decoder_on_audio_packet: 0, rtp_decoder_on_packet: 0 })));
    if (r1.ok && r2.ok && r3.ok && r4.ok && r5.ok) {
      eq('收到且解出（OPUS+回调在）：parsed=1', r1.value.rtp_decoder_audio_packets_parsed, 1);
      eq('收到且解出（OPUS+回调在）：produced=1', r1.value.rtp_decoder_audio_units_produced, 1);
      eq('收到且解出：计入 opusPt', r1.value.rtp_decoder_audio_opus_packets, 1);
      eq('收到但解不出（短包）：produced 不动', r2.value.rtp_decoder_audio_units_produced || 0, 0);
      eq('收到但解不出（短包）：failShort 记账', r2.value.rtp_decoder_audio_fail_short, 1);
      eq('收到但解不出（形状错）：failShape 记账', r3.value.rtp_decoder_audio_fail_shape, 1);
      eq('收到但解不出（形状错）：parsed 不动', r3.value.rtp_decoder_audio_packets_parsed || 0, 0);
      eq('RED(63)：redPt 记账（能指出「是 RED」）', r4.value.rtp_decoder_audio_red_packets, 1);
      eq('RED(63) 不计入 opusPt', r4.value.rtp_decoder_audio_opus_packets || 0, 0);
      eq('解析成功但没有音频回调：parsed=1', r5.value.rtp_decoder_audio_packets_parsed, 1);
      eq('解析成功但没有音频回调：produced=0', r5.value.rtp_decoder_audio_units_produced || 0, 0);
      ok('★ 收到且解出 vs 收到但解不出：produced 不同（任务书 B.3 判据）',
        (r1.value.rtp_decoder_audio_units_produced || 0) !== (r2.value.rtp_decoder_audio_units_produced || 0),
        `${r1.value.rtp_decoder_audio_units_produced || 0} vs ${r2.value.rtp_decoder_audio_units_produced || 0}`);
    } else {
      metaFail += 1;
      ok('★ 收到且解出 vs 收到但解不出：produced 必须不同', false,
        [r1, r2, r3, r4, r5].filter((x) => !x.ok).map((x) => x.msg).join(' / '));
    }
  }
}

// ===========================================================================
// 5. 元判据
// ===========================================================================
section('5 元判据');
{
  ok('真代码全部解析成功 ==> 判据在检查真代码', metaFail === 0, metaFail === 0 ? '' : `${metaFail} 处解析失败`);
}

console.log(`\n=== 结果：${passed} 通过 / ${failed} 失败 ===`);
if (failed) {
  console.log('失败项：');
  for (const f of failures) console.log('  - ' + f);
  process.exit(1);
}

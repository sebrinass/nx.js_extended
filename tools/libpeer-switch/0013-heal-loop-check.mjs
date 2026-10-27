#!/usr/bin/env node
/* 0013-heal-loop-check.mjs -- 内层补丁 0013（heal 闭环观测）判据自检
 * 跑法：node engine/tools/libpeer-switch/0013-heal-loop-check.mjs [--patch 副本]
 * 标准：读真代码 + 真求值（双轨：补丁半边查片段；树半边抽出
 *       「守卫 -> 计数」真实映射并真跑场景）。
 * 判据灵魂（任务书 C）：构造「请求发出但无应答」的输入，
 * 断言对应那一环的计数能反映出来（pliAnswered 不动、idrDelivered 不动）。
 */
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
  return { patch: patch || process.env.HEAL_CHECK_PATCH || null };
})();
function findDefaultPatch() {
  let names = [];
  try { names = readdirSync(HERE); } catch { return null; }
  const cands = names.filter((f) => /^0013-.*\.patch$/.test(f)).sort();
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
// ===========================================================================
// 1. 行为模型（旁证）
// ===========================================================================
const ANSWER_WINDOW_MS = 3000;
section('1 行为模型（应答归因）');
{
  function model(state, now) {
    const out = { answered: 0, seen: state.seen, healed: false };
    if (state.kf !== state.seen) {
      const fresh = state.pliMs !== 0 && now - state.pliMs <= ANSWER_WINDOW_MS;
      if (fresh) out.answered = 1;
      out.seen = state.kf;
    }
    if (state.kfd !== state.seenDelivered) { out.healed = true; }
    return out;
  }
  eq('PLI 开着 + 关键帧到 ==> 记一次应答', model({ kf: 5, seen: 3, pliMs: 9000, kfd: 1, seenDelivered: 1 }, 10000).answered, 1);
  eq('没有 PLI + 关键帧到 ==> 不记应答（那一环断了）', model({ kf: 5, seen: 3, pliMs: 0, kfd: 1, seenDelivered: 1 }, 10000).answered, 0);
  eq('PLI 太老 + 关键帧到 ==> 不记应答', model({ kf: 5, seen: 3, pliMs: 4000, kfd: 1, seenDelivered: 1 }, 10000).answered, 0);
}

// ===========================================================================
// 2. 源码不变量
// ===========================================================================
section('2 源码不变量（0013 还在树上）');
{
  const pcC = readFileSync(join(LIBPEER, 'src', 'peer_connection.c'), 'utf8');
  const rtpC = readFileSync(join(LIBPEER, 'src', 'rtp.c'), 'utf8');
  const rtpH = readFileSync(join(LIBPEER, 'src', 'rtp.h'), 'utf8');
  ok('RtpDecoder 有 keyframes_delivered / au_had_idr',
    /uint32_t keyframes_delivered;/.test(rtpH) && /int au_had_idr;/.test(rtpH));
  ok('H.264 组帧处置 au_had_idr', /if \(type == 5\) \{[\s\S]{0,900}?au_had_idr = 1;/.test(rtpC));
  ok('H.265 IRAP 置 au_had_idr', /H265_NAL_IRAP_FIRST[\s\S]{0,900}?au_had_idr = 1;/.test(rtpC));
  ok('AU 复位清 au_had_idr', /rtp_decoder_reset_access_unit[\s\S]{0,700}?au_had_idr = 0;/.test(rtpC));
  ok('成功交付分支里 delivered 只认 au_had_idr',
    /access_units_completed\+\+;[\s\S]{0,400}?if \(rtp_decoder->au_had_idr\) \{[\s\S]{0,120}?keyframes_delivered\+\+;/.test(rtpC));
  for (const f of ['heal_seen_keyframes', 'heal_pli_answered', 'heal_seen_delivered', 'heal_healed_ms', 'heal_diag_ms']) {
    ok(`PeerConnection 有 ${f}`, new RegExp(`uint32_t ${f};`).test(pcC));
  }
  ok('应答窗口常量 3000ms', /#define PEER_HEAL_ANSWER_WINDOW_MS 3000/.test(pcC));
  ok('heal 诊断行（前缀 heal_state）',
    /heal_state pliSent=%u pliAnswered=%u idrSeen=%u idrDelivered=%u nackReq=%u nackPkts=%u recovered=%u auOk=%u auDrop=%u healedAgoMs=%u/.test(pcC));
  ok('记账函数每 tick 被调', /peer_connection_account_heal_keyframe\(pc, ports_get_epoch_time\(\)\);/.test(pcC));
}

// ===========================================================================
// 3. 前缀对接（新 key：heal_state）
// ===========================================================================
section('3 前缀对接');
{
  const pcC = readFileSync(join(LIBPEER, 'src', 'peer_connection.c'), 'utf8');
  ok('heal_state 是本补丁唯一的新前缀（需主代理加白名单 key "heal_state"）',
    /peer_connection_diag_log\("heal_state /.test(pcC));
}

// ===========================================================================
// 4. 判据本体：真代码 + 真求值（双轨）
// ===========================================================================
let metaFail = 0;
const bad = (name, msg) => { metaFail += 1; ok(name, false, msg ? `解析失败：${msg}` : '解析失败'); };

section('4 读真代码：heal 记账与 AU 交付的守卫->计数映射，并实际求值');
{
  const patchRes = attempt(() => {
    if (!PATCH_PATH) throw new Error('没找到 0013-*.patch');
    return readFileSync(PATCH_PATH, 'utf8');
  });
  if (patchRes.ok) ok(`补丁可读（${PATCH_PATH}）`, true); else bad('补丁可读', patchRes.msg);

  // (a) 补丁半边
  const patchHalf = attempt(() => {
    const text = patchRes.ok ? patchRes.value : '';
    const must = [
      'uint32_t keyframes_delivered;',
      'int au_had_idr;',
      'au_had_idr = 1;',
      'au_had_idr = 0;',
      'keyframes_delivered++;',
      'heal_pli_answered++;',
      'heal_state pliSent=',
      'PEER_HEAL_ANSWER_WINDOW_MS 3000',
    ];
    for (const frag of must) {
      const re = new RegExp('^[+].*' + frag.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'), 'm');
      if (!re.test(text)) throw new Error('补丁新增行缺少: ' + frag);
    }
    return true;
  });
  if (patchHalf.ok) ok('补丁半边：八个关键片段都在补丁新增行里', true);
  else bad('补丁半边：八个关键片段都在补丁新增行里', patchHalf.msg);

  // (b) 树半边之一：account_heal_keyframe
  const accRes = attempt(() => {
    const tree = readFileSync(join(LIBPEER, 'src', 'peer_connection.c'), 'utf8');
    const body = findFunctionBody(tree, 'peer_connection_account_heal_keyframe');
    if (!body) throw new Error('树上找不到 account_heal_keyframe');
    return body;
  });
  if (accRes.ok) ok('树半边：account_heal_keyframe 函数体切出', true); else bad('树半边：account_heal_keyframe 函数体切出', accRes.msg);

  const accFx = attempt(() => {
    if (!accRes.ok) throw new Error('no body');
    const fx = extractGuardedEffects(flattenMembers(tokenize(accRes.value)));
    if (!fx.some((b) => b.target === 'pc_heal_pli_answered')) throw new Error('没抽到 pli_answered 效果');
    return fx;
  });
  if (accFx.ok) ok(`记账映射抽出（${accFx.value.length} 条）`, true,
    accFx.value.map((b) => `${b.guards.length}重 -> ${b.target}${b.op === '++' ? '++' : '='}`).join(' ; '));
  else bad('记账映射抽出', accFx.msg);

  if (accFx.ok) {
    const tree = readFileSync(join(LIBPEER, 'src', 'peer_connection.c'), 'utf8');
    const win = Number((/#define PEER_HEAL_ANSWER_WINDOW_MS (\d+)/.exec(tree) || [])[1] || 3000);
    const ENV = (o) => ({ PEER_HEAL_ANSWER_WINDOW_MS: win, NULL: 0, ...o });
    const s1 = attempt(() => runEffects(accFx.value, ENV({ now_ms: 10000, pc_vrtp_decoder_keyframes_received: 5, pc_heal_seen_keyframes: 3, pc_vrtp_decoder_pli_last_sent_ms: 9000, pc_vrtp_decoder_keyframes_delivered: 1, pc_heal_seen_delivered: 1 })));
    const s2 = attempt(() => runEffects(accFx.value, ENV({ now_ms: 10000, pc_vrtp_decoder_keyframes_received: 5, pc_heal_seen_keyframes: 3, pc_vrtp_decoder_pli_last_sent_ms: 0, pc_vrtp_decoder_keyframes_delivered: 1, pc_heal_seen_delivered: 1 })));
    const s3 = attempt(() => runEffects(accFx.value, ENV({ now_ms: 10000, pc_vrtp_decoder_keyframes_received: 5, pc_heal_seen_keyframes: 5, pc_vrtp_decoder_pli_last_sent_ms: 9000, pc_vrtp_decoder_keyframes_delivered: 2, pc_heal_seen_delivered: 1 })));
    if (s1.ok && s2.ok && s3.ok) {
      eq('PLI 开着 + 关键帧到（真代码）：pli_answered=1', s1.value.pc_heal_pli_answered, 1);
      eq('★ 请求没发（pliMs=0）+ 关键帧到（真代码）：pli_answered 不动（那一环断了）', s2.value.pc_heal_pli_answered || 0, 0);
      eq('关键帧没来（kf 不变）（真代码）：pli_answered 不动', s3.value.pc_heal_pli_answered || 0, 0);
      eq('两种输入下 seen 都推进（记账不丢）', s1.value.pc_heal_seen_keyframes, 5);
      eq('交付关键帧到来 ==> healed_ms=now（真代码）', s3.value.pc_heal_healed_ms, 10000);
      ok('★ 「请求发出且被应答」vs「请求没发」：pli_answered 不同（任务书 C 判据）',
        (s1.value.pc_heal_pli_answered || 0) !== (s2.value.pc_heal_pli_answered || 0));
    } else {
      metaFail += 1;
      ok('★ 任务书 C 判据', false, [s1, s2, s3].filter((x) => !x.ok).map((x) => x.msg).join(' / '));
    }
  }

  // (c) 树半边之二：flush_access_unit（delivered 只在成功 + au_had_idr 时动）
  const flushRes = attempt(() => {
    const tree = readFileSync(join(LIBPEER, 'src', 'rtp.c'), 'utf8');
    const body = findFunctionBody(tree, 'rtp_decoder_flush_access_unit');
    if (!body) throw new Error('找不到 flush_access_unit');
    return body;
  });
  if (flushRes.ok) ok('树半边：flush_access_unit 函数体切出', true); else bad('树半边：flush_access_unit 函数体切出', flushRes.msg);
  const flushFx = attempt(() => {
    if (!flushRes.ok) throw new Error('no body');
    const fx = extractGuardedEffects(flattenMembers(tokenize(flushRes.value)));
    if (!fx.some((b) => b.target === 'rtp_decoder_keyframes_delivered')) throw new Error('没抽到 keyframes_delivered 效果');
    return fx;
  });
  if (flushFx.ok) ok(`AU 交付映射抽出（${flushFx.value.length} 条）`, true); else bad('AU 交付映射抽出', flushFx.msg);
  if (flushFx.ok) {
    const ENV = (o) => ({ NULL: 0, rtp_decoder_au_offset: 100, ...o });
    const f1 = attempt(() => runEffects(flushFx.value, ENV({ rtp_decoder_au_damaged: 0, rtp_decoder_au_has_vcl: 1, rtp_decoder_au_had_idr: 1, rtp_decoder_au_started: 1 })));
    const f2 = attempt(() => runEffects(flushFx.value, ENV({ rtp_decoder_au_damaged: 0, rtp_decoder_au_has_vcl: 1, rtp_decoder_au_had_idr: 0, rtp_decoder_au_started: 1 })));
    const f3 = attempt(() => runEffects(flushFx.value, ENV({ rtp_decoder_au_damaged: 1, rtp_decoder_au_has_vcl: 1, rtp_decoder_au_had_idr: 1, rtp_decoder_au_started: 1 })));
    if (f1.ok && f2.ok && f3.ok) {
      eq('成功 AU 且带 IDR（真代码）：delivered=1', f1.value.rtp_decoder_keyframes_delivered, 1);
      eq('成功 AU 不带 IDR（真代码）：delivered 不动', f2.value.rtp_decoder_keyframes_delivered || 0, 0);
      eq('损坏 AU（真代码）：dropped 记账、delivered 不动', (f3.value.rtp_decoder_access_units_dropped || 0) === 1 && !(f3.value.rtp_decoder_keyframes_delivered > 0), true);
      ok('★ idrSeen 与 idrDelivered 的差 = 我们丢的（能区分「云端没给」与「我们丢了」）',
        (f1.value.rtp_decoder_keyframes_delivered || 0) !== (f3.value.rtp_decoder_keyframes_delivered || 0));
    } else {
      metaFail += 1;
      ok('★ AU 交付判据', false, [f1, f2, f3].filter((x) => !x.ok).map((x) => x.msg).join(' / '));
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

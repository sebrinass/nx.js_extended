#!/usr/bin/env node
/* ============================================================================
 *  0008-remb-semantics-check.mjs -- 内层补丁 0008（R3：REMB AIMD 收敛）判据自检
 * ----------------------------------------------------------------------------
 *  跑法：
 *      node engine/tools/libpeer-switch/0008-remb-semantics-check.mjs
 *      node engine/tools/libpeer-switch/0008-remb-semantics-check.mjs --patch <副本>
 *
 *  判据历史（为什么这个脚本长这样）：
 *    v1（B1 轮）第 1/2 节是「JS 复刻 + 正则」，C 侧改坏也会报绿（假绿），
 *    被主代理升级为「读真代码 + 真求值」。R2 轮契约从「max(实测, FLOOR)
 *    常量声明」改为「AIMD 估计器」，本脚本随之重写。R3 轮（2026-10-24
 *    审稿三项改动）再加：
 *      T1 压力源拆分：net_stress（gaps+late+decfail）是唯一乘性减驱动；
 *         self_stress（forced+audrop）只进诊断行（selfStress=），不再降。
 *      T2 上限收紧：CEIL 20M -> 8M；审稿建议的「与实测中位数取 min」
 *         钳制经分析不采纳（情形 A 下反向压制恢复），本脚本挡两道：
 *         结构上夹取必须恰好 floor/ceil 两个；行为上低实测 quiet 窗口
 *         必须继续爬升且声明值 >= FLOOR。
 *      T3 暖机：前 PEER_REMB_WARMUP_WINDOWS 个窗口只加不减。
 *    0018/修改4（2026-10-27）：REMB 估计器换代，AIMD 爬升整个删除：
 *      declared = 1~2 s 滑窗实测 bps x 11/10，clamp[FLOOR=500k, CEIL=8M]；
 *      无测量（span_bytes==0）= START=200k；net_stress 仍 x7/10 回退（出
 *      暖机后），self_stress 仍不触发；PEER_REMB_AI_STEP_BPS 删除。
 *      判据读**树上** peer_connection.c（--patch 仍可指定补丁副本做变异
 *      测试）；决策不再是单三元而是 if/else 链，求值加了语句级执行
 *      （extractAssigns），标识符支持 pc->member（滑窗状态在结构体里）。
 *    并沿用真代码求值标准：
 *      ④ 从真代码抠出 stress / congested / declared 的真实表达式与报文
 *         实参，用受限求值器真算一遍；
 *      ⑤ 元判据：解析不出 ==> 红，绝不悄悄不检查。
 *
 *  本轮判据的灵魂：
 *    「实测低」有三种原因，行为必须分得开：
 *      A 云端限速（quiet 窗口）        ==> 声明值 = 实测 x 1.1（跟踪实测，
 *         不再爬升）；
 *      B 网络问题（net_stress 窗口）   ==> 同估计 x 7/10（back OFF）；
 *      C 自己处理不过来（self_stress）==> 与 A 逐位相同（降码率治不了
 *         自己丢自己的帧）。
 *    同样的实测、三组不同的丢包特征 ==> A==C，B = A x 7/10（下限 FLOOR）。
 * ========================================================================== */

import { readFileSync, readdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const HERE = dirname(fileURLToPath(import.meta.url));
const LIBPEER = join(HERE, '..', '..', 'extern', 'libpeer');
const ENGINE = join(HERE, '..', '..');

const CLI = (() => {
  const argv = process.argv.slice(2);
  let patch = null;
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === '--patch' && argv[i + 1]) patch = argv[++i];
    else if (argv[i].startsWith('--patch=')) patch = argv[i].slice('--patch='.length);
  }
  return { patch: patch || process.env.REMB_CHECK_PATCH || null };
})();

// 0018：默认读**树上** peer_connection.c（构建出来的就是它）；--patch <副本>
// 仍指向补丁做变异测试。旧默认是自动找 0008-*.patch —— 内层补丁尚未随
// 修改4 同步（inner-sync）前，那里面还是旧 AIMD 代码。
function findDefaultPatch() {
  let names = [];
  try { names = readdirSync(HERE); } catch { return null; }
  const cands = names.filter((f) => /^0008-.*\.patch$/.test(f)).sort();
  for (const f of cands) {
    try {
      if (readFileSync(join(HERE, f), 'utf8').includes('rtcp_get_remb')) return join(HERE, f);
    } catch { /* ignore */ }
  }
  return cands.length ? join(HERE, cands[0]) : null;
}
const TREE_PATH = join(LIBPEER, 'src', 'peer_connection.c');
const SOURCE_PATH = CLI.patch || TREE_PATH;
const SOURCE_KIND = CLI.patch ? '补丁' : '树上源码';
const AUTO_PATCH = CLI.patch ? null : findDefaultPatch();

let passed = 0;
let failed = 0;
const failures = [];
function ok(name, cond, extra) {
  if (cond) { passed += 1; console.log(`  ok ${name}${extra ? '  ' + extra : ''}`); }
  else { failed += 1; failures.push(name + (extra ? '  ' + extra : '')); console.log(`  FAIL ${name}${extra ? '  ' + extra : ''}`); }
}
function eq(name, actual, expected) {
  ok(name, String(actual) === String(expected),
    String(actual) === String(expected) ? '' : `期望 ${expected}，实际 ${actual}`);
}
function section(t) { console.log(`\n=== ${t} ===`); }

// ===========================================================================
// 受限求值器（分词 + 优先级爬升 + 求值；不用 eval；补丁文本是不可信输入）
// ===========================================================================
function tokenize(src) {
  const toks = [];
  let i = 0;
  const n = src.length;
  while (i < n) {
    const c = src[i];
    if (c === ' ' || c === '\t' || c === '\n' || c === '\r') { i += 1; continue; }
    if (c === '"' || c === "'") {
      const q = c;
      let j = i + 1;
      let out = '';
      while (j < n && src[j] !== q) {
        if (src[j] === '\\') { out += src[j + 1]; j += 2; } else { out += src[j]; j += 1; }
      }
      toks.push({ t: 'str', v: out });
      i = j + 1;
      continue;
    }
    if (c === '/' && src[i + 1] === '/') { while (i < n && src[i] !== '\n') i += 1; continue; }
    if (c === '/' && src[i + 1] === '*') { i += 2; while (i < n && !(src[i] === '*' && src[i + 1] === '/')) i += 1; i += 2; continue; }
    if (c >= '0' && c <= '9') {
      // 0018: 十六进制字面量（0xffffffffull 等）也要能进求值器。
      if (c === '0' && (src[i + 1] === 'x' || src[i + 1] === 'X')) {
        let j = i + 2;
        while (j < n && /[0-9a-fA-F]/.test(src[j])) j += 1;
        const hv = parseInt(src.slice(i + 2, j), 16);
        while (j < n && /[uUlL]/.test(src[j])) j += 1;
        toks.push({ t: 'num', v: hv });
        i = j;
        continue;
      }
      let j = i;
      while (j < n && /[0-9]/.test(src[j])) j += 1;
      const value = parseInt(src.slice(i, j), 10);
      while (j < n && /[uUlL]/.test(src[j])) j += 1;
      toks.push({ t: 'num', v: value });
      i = j;
      continue;
    }
    if (/[A-Za-z_]/.test(c)) {
      let j = i;
      while (j < n && /[A-Za-z0-9_]/.test(src[j])) j += 1;
      toks.push({ t: 'id', v: src.slice(i, j) });
      i = j;
      continue;
    }
    const ops = ['->', '++', '--', '<<', '>>', '<=', '>=', '==', '!=', '&&', '||', '+=', '-=', '*=', '/=', '%='];
    let matched = null;
    for (const op of ops) if (src.startsWith(op, i)) { matched = op; break; }
    if (matched) { toks.push({ t: 'op', v: matched }); i += matched.length; continue; }
    toks.push({ t: 'op', v: c });
    i += 1;
  }
  return toks;
}
const TYPE_WORDS = new Set(['void', 'char', 'short', 'int', 'long', 'float', 'double',
  'signed', 'unsigned', 'size_t', 'bool', 'int8_t', 'int16_t', 'int32_t', 'int64_t',
  'uint8_t', 'uint16_t', 'uint32_t', 'uint64_t']);
const BINARY_LEVELS = [['||'], ['&&'], ['==', '!='], ['<', '>', '<=', '>='], ['+', '-'], ['*', '/', '%']];
class ExprParser {
  constructor(tokens, pos) { this.toks = tokens; this.pos = pos || 0; }
  peek() { return this.toks[this.pos]; }
  isOp(v) { const t = this.peek(); return !!t && t.t === 'op' && t.v === v; }
  eatOp(v) { if (this.isOp(v)) { this.pos += 1; return true; } return false; }
  parseExpression() { return this.parseTernary(); }
  parseTernary() {
    const cond = this.parseBinary(0);
    if (this.eatOp('?')) {
      const a = this.parseTernary();
      if (!this.eatOp(':')) throw new Error('三元缺 :');
      const b = this.parseTernary();
      return { k: 'cond', cond, a, b };
    }
    return cond;
  }
  parseBinary(level) {
    if (level >= BINARY_LEVELS.length) return this.parseUnary();
    let left = this.parseBinary(level + 1);
    for (;;) {
      const t = this.peek();
      if (!t || t.t !== 'op' || !BINARY_LEVELS[level].includes(t.v)) break;
      this.pos += 1;
      const right = this.parseBinary(level + 1);
      left = { k: 'bin', op: t.v, l: left, r: right };
    }
    return left;
  }
  parseUnary() {
    const t = this.peek();
    if (t && t.t === 'op' && (t.v === '!' || t.v === '-' || t.v === '+' || t.v === '~')) {
      this.pos += 1;
      return { k: 'un', op: t.v, e: this.parseUnary() };
    }
    return this.parseCast();
  }
  parseCast() {
    if (this.isOp('(')) {
      const save = this.pos;
      this.pos += 1;
      let words = 0;
      while (words < 4) {
        const t = this.peek();
        if (t && t.t === 'id' && TYPE_WORDS.has(t.v)) { this.pos += 1; words += 1; } else break;
      }
      if (words > 0 && (this.isOp('*') || this.isOp(')'))) {
        while (!this.isOp(')')) { if (this.pos > this.toks.length) { this.pos = save; return this.parsePrimary(); } this.pos += 1; }
        this.pos += 1;
        return { k: 'cast', e: this.parseUnary() };
      }
      this.pos = save;
    }
    return this.parsePrimary();
  }
  parsePrimary() {
    const t = this.peek();
    if (!t) throw new Error('表达式意外结束');
    if (t.t === 'num') { this.pos += 1; return { k: 'num', v: t.v }; }
    if (t.t === 'id') {
      this.pos += 1;
      if (this.isOp('(')) throw new Error('表达式里不支持函数调用: ' + t.v);
      // 0018: 支持 pc->member 成员访问（REMB 新代码把滑窗状态放结构体里，
      // 表达式里出现 pc->remb_prev_window_bytes 等）。整链合成一个标识符，
      // 场景环境用 'pc->xxx' 键喂值。
      let name = t.v;
      while (this.isOp('->')) {
        this.pos += 1;
        const f = this.peek();
        if (!f || f.t !== 'id') throw new Error('-> 后缺成员名: ' + name);
        this.pos += 1;
        name += '->' + f.v;
      }
      return { k: 'var', v: name };
    }
    if (t.t === 'op' && t.v === '(') {
      this.pos += 1;
      const e = this.parseTernary();
      if (!this.eatOp(')')) throw new Error('缺 )');
      return e;
    }
    throw new Error('无法解析: ' + JSON.stringify(t.v));
  }
}
function evalAst(node, env) {
  switch (node.k) {
    case 'num': return node.v;
    case 'var': {
      if (!Object.prototype.hasOwnProperty.call(env, node.v)) throw new Error('未定义标识符: ' + node.v);
      return env[node.v];
    }
    case 'cast': return Math.trunc(evalAst(node.e, env));
    case 'un': {
      const v = evalAst(node.e, env);
      if (node.op === '!') return v ? 0 : 1;
      if (node.op === '-') return -v;
      if (node.op === '+') return v;
      if (node.op === '~') return ~v;
      throw new Error('未知一元 ' + node.op);
    }
    case 'bin': {
      const l = evalAst(node.l, env);
      const r = evalAst(node.r, env);
      switch (node.op) {
        case '+': return l + r;
        case '-': return l - r;
        case '*': return l * r;
        case '/': if (r === 0) throw new Error('除零'); return Math.trunc(l / r);
        case '%': if (r === 0) throw new Error('模零'); return l % r;
        case '<': return l < r ? 1 : 0;
        case '>': return l > r ? 1 : 0;
        case '<=': return l <= r ? 1 : 0;
        case '>=': return l >= r ? 1 : 0;
        case '==': return l === r ? 1 : 0;
        case '!=': return l !== r ? 1 : 0;
        case '&&': return (l && r) ? 1 : 0;
        case '||': return (l || r) ? 1 : 0;
        default: throw new Error('未知二元 ' + node.op);
      }
    }
    case 'cond': return evalAst(node.cond, env) ? evalAst(node.a, env) : evalAst(node.b, env);
    default: throw new Error('未知节点 ' + node.k);
  }
}
function srcOf(toks, from, to) {
  return toks.slice(from, to).map((t) => t.v).join(' ');
}

// ===========================================================================
// 补丁解析（unified diff -> 每文件新侧文本）+ 函数体/声明/赋值/夹取提取
// ===========================================================================
function parsePatch(text) {
  const lines = text.split(/\r?\n/);
  const files = [];
  let cur = null;
  let i = 0;
  while (i < lines.length) {
    const line = lines[i];
    if (line.startsWith('diff --git ')) { cur = { path: '', hunks: [] }; files.push(cur); i += 1; continue; }
    if (line.startsWith('+++ ')) {
      let p = line.slice(4).trim();
      if (p.startsWith('b/')) p = p.slice(2);
      if (cur) cur.path = p;
      i += 1; continue;
    }
    if (line.startsWith('@@')) {
      const m = /^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@/.exec(line);
      if (!m) { i += 1; continue; }
      const hunk = { newCount: m[4] === undefined ? 1 : Number(m[4]), body: [] };
      if (cur) cur.hunks.push(hunk);
      i += 1;
      let newSeen = 0;
      while (i < lines.length && newSeen < hunk.newCount) {
        const l = lines[i];
        if (l.startsWith('diff --git ') || l.startsWith('@@')) break;
        if (l.startsWith('\\')) { i += 1; continue; }
        if (l.startsWith('+')) { hunk.body.push({ kind: 'add', text: l.slice(1) }); newSeen += 1; }
        else if (l.startsWith('-')) { hunk.body.push({ kind: 'del', text: l.slice(1) }); }
        else { hunk.body.push({ kind: 'ctx', text: l.startsWith(' ') ? l.slice(1) : l }); newSeen += 1; }
        i += 1;
      }
      continue;
    }
    i += 1;
  }
  return files;
}
function newSideText(file) {
  const out = [];
  for (const h of file.hunks) {
    for (const b of h.body) if (b.kind !== 'del') out.push(b.text);
    out.push('');
  }
  return out.join('\n');
}
function findFunctionBody(text, name) {
  let from = 0;
  for (;;) {
    const idx = text.indexOf(name, from);
    if (idx < 0) return null;
    from = idx + name.length;
    let j = from;
    while (j < text.length && /\s/.test(text[j])) j += 1;
    if (text[j] !== '(') continue;
    let depth = 0;
    let k = j;
    while (k < text.length) {
      const c = text[k];
      if (c === '(') depth += 1;
      else if (c === ')') { depth -= 1; if (depth === 0) { k += 1; break; } }
      k += 1;
    }
    while (k < text.length && /\s/.test(text[k])) k += 1;
    if (text[k] !== '{') continue;
    const open = k;
    depth = 0;
    let i = open;
    const n = text.length;
    while (i < n) {
      const c = text[i];
      if (c === '"' || c === "'") { const q = c; i += 1; while (i < n && text[i] !== q) { if (text[i] === '\\') i += 1; i += 1; } i += 1; continue; }
      if (c === '/' && text[i + 1] === '/') { while (i < n && text[i] !== '\n') i += 1; continue; }
      if (c === '/' && text[i + 1] === '*') { i += 2; while (i < n && !(text[i] === '*' && text[i + 1] === '/')) i += 1; i += 2; continue; }
      if (c === '{') depth += 1;
      else if (c === '}') { depth -= 1; if (depth === 0) return text.slice(open + 1, i); }
      i += 1;
    }
    return null;
  }
}
function findDeclInit(toks, name) {
  for (let i = 1; i < toks.length; i++) {
    const t = toks[i];
    if (t.t !== 'id' || t.v !== name) continue;
    const prev = toks[i - 1];
    if (!prev || prev.t !== 'id' || !TYPE_WORDS.has(prev.v)) continue;
    if (!(toks[i + 1] && toks[i + 1].t === 'op' && toks[i + 1].v === '=')) continue;
    const p = new ExprParser(toks, i + 2);
    const ast = p.parseExpression();
    return { ast, src: srcOf(toks, i - 1, p.pos + 1) };
  }
  return null;
}
function findAssignments(toks, name) {
  const out = [];
  for (let i = 0; i < toks.length; i++) {
    const t = toks[i];
    if (t.t !== 'id' || t.v !== name) continue;
    const prev = toks[i - 1];
    if (prev && prev.t === 'id' && TYPE_WORDS.has(prev.v)) continue;
    if (!(toks[i + 1] && toks[i + 1].t === 'op' && toks[i + 1].v === '=')) continue;
    try {
      const p = new ExprParser(toks, i + 2);
      const ast = p.parseExpression();
      out.push({ ast, src: srcOf(toks, i, p.pos + 1) });
    } catch { /* skip */ }
  }
  return out;
}
function findClamps(toks, name) {
  const out = [];
  for (let i = 0; i < toks.length; i++) {
    const t = toks[i];
    if (t.t !== 'id' || t.v !== 'if') continue;
    if (!(toks[i + 1] && toks[i + 1].t === 'op' && toks[i + 1].v === '(')) continue;
    const cp = new ExprParser(toks, i + 2);
    let cond;
    try { cond = cp.parseExpression(); } catch { continue; }
    if (!(toks[cp.pos] && toks[cp.pos].t === 'op' && toks[cp.pos].v === ')')) continue;
    const brace = cp.pos + 1;
    if (!(toks[brace] && toks[brace].t === 'op' && toks[brace].v === '{')) continue;
    let depth = 0;
    for (let k = brace; k < toks.length; k++) {
      const tk = toks[k];
      if (tk.t === 'op' && tk.v === '{') { depth += 1; continue; }
      if (tk.t === 'op' && tk.v === '}') { depth -= 1; if (depth === 0) break; continue; }
      if (depth === 1 && tk.t === 'id' && tk.v === name && toks[k + 1] && toks[k + 1].t === 'op' && toks[k + 1].v === '=') {
        const ap = new ExprParser(toks, k + 2);
        const ast = ap.parseExpression();
        out.push({ cond, assign: ast, src: srcOf(toks, i, ap.pos + 1) });
        break;
      }
    }
  }
  return out;
}
// 0018: if/else 决策块提取。REMB 新决策是
//   if (span_bytes == 0) { declared = START; }
//   else { declared = <估计>; if (back_off) {...} if (<FLOOR) {...} if (>CEIL) {...} }
// 单三元提取器表达不了这个结构，这里按语句级抽出 then/else 两支里所有
// `declared = expr;` 与 `if (cond) { declared = expr; }`，求值器按序执行。
function takeBrace(toks, open) {
  let depth = 0;
  for (let k = open; k < toks.length; k++) {
    if (toks[k].t === 'op' && toks[k].v === '{') depth += 1;
    else if (toks[k].t === 'op' && toks[k].v === '}') {
      depth -= 1;
      if (depth === 0) return { start: open, end: k };
    }
  }
  return null;
}
function extractAssigns(toks, start, end, name) {
  const out = [];
  let depth = 0;
  for (let k = start; k <= end; k++) {
    const t = toks[k];
    if (t.t === 'op' && (t.v === '{' || t.v === '(')) { depth += 1; continue; }
    if (t.t === 'op' && (t.v === '}' || t.v === ')')) { depth -= 1; continue; }
    if (depth !== 0) continue;
    if (t.t === 'id' && t.v === name && toks[k + 1] && toks[k + 1].t === 'op' && toks[k + 1].v === '='
        && !(toks[k - 1] && toks[k - 1].t === 'id' && TYPE_WORDS.has(toks[k - 1].v))) {
      const ap = new ExprParser(toks, k + 2);
      const ast = ap.parseExpression();
      out.push({ kind: 'assign', ast, src: srcOf(toks, k, ap.pos + 1) });
      k = ap.pos;
      continue;
    }
    if (t.t === 'id' && t.v === 'if' && toks[k + 1] && toks[k + 1].t === 'op' && toks[k + 1].v === '(') {
      const cp = new ExprParser(toks, k + 2);
      let cond;
      try { cond = cp.parseExpression(); } catch { continue; }
      if (!(toks[cp.pos] && toks[cp.pos].t === 'op' && toks[cp.pos].v === ')')) continue;
      const br = cp.pos + 1;
      if (!(toks[br] && toks[br].t === 'op' && toks[br].v === '{')) continue;
      const blk = takeBrace(toks, br);
      if (!blk) continue;
      const inner = extractAssigns(toks, blk.start + 1, blk.end - 1, name);
      if (inner.length === 1 && inner[0].kind === 'assign') {
        out.push({ kind: 'if', cond, ast: inner[0].ast, src: srcOf(toks, k, blk.end + 1) });
        k = blk.end;
      }
      continue;
    }
  }
  return out;
}
function findIfElseAssign(toks, name) {
  for (let i = 0; i < toks.length; i++) {
    const t = toks[i];
    if (t.t !== 'id' || t.v !== 'if') continue;
    if (!(toks[i + 1] && toks[i + 1].t === 'op' && toks[i + 1].v === '(')) continue;
    const cp = new ExprParser(toks, i + 2);
    let cond;
    try { cond = cp.parseExpression(); } catch { continue; }
    if (!(toks[cp.pos] && toks[cp.pos].t === 'op' && toks[cp.pos].v === ')')) continue;
    let q = cp.pos + 1;
    if (!(toks[q] && toks[q].t === 'op' && toks[q].v === '{')) continue;
    const thenBlk = takeBrace(toks, q);
    if (!thenBlk) continue;
    q = thenBlk.end + 1;
    if (!(toks[q] && toks[q].t === 'id' && toks[q].v === 'else')) continue;
    q += 1;
    if (!(toks[q] && toks[q].t === 'op' && toks[q].v === '{')) continue;
    const elseBlk = takeBrace(toks, q);
    if (!elseBlk) continue;
    const thenStmts = extractAssigns(toks, thenBlk.start + 1, thenBlk.end - 1, name);
    const elseStmts = extractAssigns(toks, elseBlk.start + 1, elseBlk.end - 1, name);
    if (!thenStmts.length && !elseStmts.length) continue;
    return { cond, then: thenStmts, else: elseStmts, src: srcOf(toks, i, elseBlk.end + 1) };
  }
  return null;
}
function splitArgs(toks, openIdx) {
  const args = [];
  let depth = 0;
  let start = openIdx + 1;
  for (let i = openIdx; i < toks.length; i++) {
    const t = toks[i];
    if (t.t === 'op' && (t.v === '(' || t.v === '[')) { depth += 1; continue; }
    if (t.t === 'op' && (t.v === ')' || t.v === ']')) {
      depth -= 1;
      if (depth === 0) { args.push(toks.slice(start, i)); return { args, end: i }; }
      continue;
    }
    if (t.t === 'op' && t.v === ',' && depth === 1) { args.push(toks.slice(start, i)); start = i + 1; }
  }
  return { args: null, end: -1 };
}
function findCall(toks, fname) {
  for (let i = 0; i < toks.length; i++) {
    const t = toks[i];
    if (t.t !== 'id' || t.v !== fname) continue;
    const nxt = toks[i + 1];
    if (!nxt || nxt.t !== 'op' || nxt.v !== '(') continue;
    const r = splitArgs(toks, i + 1);
    if (!r.args) continue;
    return { args: r.args };
  }
  return null;
}
function formatSpecs(fmt) {
  const specs = [];
  const re = /%([-+ #0]*\d*(?:\.\d+)?)([a-zA-Z%])/g;
  let m;
  while ((m = re.exec(fmt)) !== null) {
    if (m[2] === '%') continue;
    specs.push({ conv: m[2], prefix: fmt.slice(0, m.index) });
  }
  return specs;
}
function attempt(fn) {
  try { return { ok: true, value: fn(), msg: '' }; }
  catch (e) { return { ok: false, value: null, msg: (e && e.message) || String(e) }; }
}

// ===========================================================================
// 1/1b. 行为模型（旁证；判据本体在第 4 节）
// ===========================================================================
const C = {
  START: 200000, FLOOR: 500000, CEIL: 8000000,
  MEAS_NUM: 11, MEAS_DEN: 10, MD_NUM: 7, MD_DEN: 10, LOSS_PCT: 2, WARMUP: 5,
};

// 0018/修改4：估计器模型（旁证）。declared = 滑窗实测 x 11/10，
// clamp[FLOOR, CEIL]；无测量 = START；net_stress（出暖机）x7/10；
// self_stress 不动作；没有爬升——声明值只跟实测走。
class RembEstimator {
  constructor() { this.declared = 0; this.prevBytes = 0; this.prevMs = 0; }
  window(bytes, elapsedMs, net, self, pkts, windowIndex) {
    const congested = net > 0 && (pkts === 0 || Math.trunc((net * 100) / pkts) >= C.LOSS_PCT);
    const warmup = windowIndex < C.WARMUP;
    const backOff = congested && !warmup;
    const spanBytes = bytes + this.prevBytes;
    const spanMs = elapsedMs + this.prevMs;
    this.prevBytes = bytes;
    this.prevMs = elapsedMs;
    const measured = spanMs ? Math.trunc((spanBytes * 8 * 1000) / spanMs) : 0;
    let declared;
    if (spanBytes === 0) declared = C.START;
    else {
      declared = Math.trunc((measured * C.MEAS_NUM) / C.MEAS_DEN);
      if (backOff) declared = Math.trunc((declared * C.MD_NUM) / C.MD_DEN);
      if (declared < C.FLOOR) declared = C.FLOOR;
      if (declared > C.CEIL) declared = C.CEIL;
    }
    this.declared = declared;
    return { declared, congested, backOff, measured };
  }
}

section('1 行为模型（估计器：A/B/C 三态必须分得开）');
{
  const r0 = new RembEstimator().window(0, 1000, 0, 0, 370, 0);
  eq('无测量（span=0）==> START 200 k', r0.declared, C.START);
  const a = new RembEstimator();
  a.window(250000, 1000, 0, 0, 370, 0);
  const rA = a.window(250000, 1000, 0, 0, 370, 9);      // A：quiet，滑窗实测 2 Mbit/s
  eq('A（quiet，实测 2 M）==> 实测 x 11/10 = 2.2 M', rA.declared, 2200000);
  const bSelf = new RembEstimator();
  bSelf.window(250000, 1000, 0, 0, 370, 0);
  const rC = bSelf.window(250000, 1000, 0, 20, 370, 9); // C：self_stress=20/370，net=0
  eq('C（self_stress）==> 与 quiet 同（不降）', rC.declared, 2200000);
  eq('C 与 A 的声明值相同（self stress 不改行为）', rC.declared, rA.declared);
  const bNet = new RembEstimator();
  bNet.window(250000, 1000, 0, 0, 370, 0);
  const rB = bNet.window(250000, 1000, 20, 0, 370, 9);  // B：net_stress=20/370 ~= 5.4%
  eq('B（net_stress）==> 估计 x 7/10 = 1.54 M', rB.declared, Math.trunc(2200000 * C.MD_NUM / C.MD_DEN));
  ok('B 与 A/C 的声明值不同（模型层）', rA.declared > rB.declared, `${rA.declared} > ${rB.declared}`);
}

section('1b 暖机 + 夹取 + 跟踪实测（模型层旁证）');
{
  const warm = new RembEstimator();
  warm.window(250000, 1000, 0, 0, 500, 0);
  const seq = [];
  for (let k = 1; k <= 5; k++) seq.push(warm.window(250000, 1000, 50, 0, 500, k).declared);
  ok('暖机窗 1..4 net_stress 不降（恒 2.2 M）', seq.slice(0, 4).every((v) => v === 2200000), seq.join(','));
  eq('第 6 窗（序号 5）起 net_stress 触发 md', seq[4], Math.trunc(2200000 * C.MD_NUM / C.MD_DEN));
  const t = new RembEstimator();
  t.window(250000, 1000, 0, 0, 500, 0);
  const flat = [];
  for (let i = 1; i <= 6; i++) flat.push(t.window(250000, 1000, 0, 0, 500, i).declared);
  ok('无爬升：同实测多窗声明值恒定（不随窗号涨）', flat.every((v) => v === 2200000), flat.join(','));
  const lo = new RembEstimator();
  lo.window(60, 1000, 0, 0, 500, 0);
  eq('低实测（480 bps）夹到 FLOOR 500 k', lo.window(60, 1000, 0, 0, 500, 9).declared, C.FLOOR);
  const hi = new RembEstimator();
  hi.window(2000000, 1000, 0, 0, 500, 0);
  eq('高实测（16 M）夹到 CEIL 8 M', hi.window(2000000, 1000, 0, 0, 500, 9).declared, C.CEIL);
  const selfOnly = new RembEstimator();
  selfOnly.window(250000, 1000, 0, 0, 500, 0);
  eq('持续 self_stress 永不 md（仍 2.2 M）', selfOnly.window(250000, 1000, 0, 50, 500, 9).declared, 2200000);
}

// ===========================================================================
// 2. 源码不变量（树上）
// ===========================================================================
section('2 源码不变量（0018 估计器在树上）');
{
  const pcC = readFileSync(join(LIBPEER, 'src', 'peer_connection.c'), 'utf8');
  for (const [name, val] of [['PEER_REMB_START_BPS', 200000], ['PEER_REMB_FLOOR_BPS', 500000],
    ['PEER_REMB_CEIL_BPS', 8000000], ['PEER_REMB_MEASURED_NUM', 11], ['PEER_REMB_MEASURED_DEN', 10],
    ['PEER_REMB_LOSS_PCT', 2], ['PEER_REMB_WARMUP_WINDOWS', 5]]) {
    ok(`${name} = ${val}`, new RegExp(`#define ${name} ${val}u`).test(pcC));
  }
  ok('PEER_REMB_AI_STEP_BPS（+500k 爬升）已从树上删除', !/#define PEER_REMB_AI_STEP_BPS/.test(pcC));
  ok('MD 因子 7/10 保留', /#define PEER_REMB_MD_NUM 7u/.test(pcC) && /#define PEER_REMB_MD_DEN 10u/.test(pcC));
  ok('滑窗状态字段在树上（remb_prev_window_bytes / remb_prev_window_ms）',
    /remb_prev_window_bytes/.test(pcC) && /remb_prev_window_ms/.test(pcC));
  ok('估计式 declared = 实测 x 11/10 在树上',
    /measured_bps \* PEER_REMB_MEASURED_NUM\) \/ PEER_REMB_MEASURED_DEN/.test(pcC));
  ok('无测量冷启动 = PEER_REMB_START_BPS（span_bytes == 0 分支）',
    /span_bytes == 0/.test(pcC) && /declared = PEER_REMB_START_BPS;/.test(pcC));
  ok('旧 AIMD 爬升（declared + PEER_REMB_AI_STEP_BPS）已从树上消失',
    !/declared \+ PEER_REMB_AI_STEP_BPS/.test(pcC));
  ok('旧 AIMD 三元骨架（declared == 0 ? START : back_off ? ...）已从树上消失',
    !/declared = declared == 0\s*\n\s*\? PEER_REMB_START_BPS/.test(pcC));
  ok('congested 判据在树上（net_stress 百分比门限）',
    /const int congested = net_stress > 0 &&[\s\S]{0,160}?PEER_REMB_LOSS_PCT/.test(pcC));
  ok('T1：net_stress = gaps + late + decfail（树上）',
    /const uint32_t net_stress = gap_ev \+ late_ev \+ decfail_ev;/.test(pcC));
  ok('T1：self_stress = forced + audrop（树上）',
    /const uint32_t self_stress = forced_ev \+ audrop_ev;/.test(pcC));
  ok('T1：旧五路合线（stress = gap+late+forced+audrop+decfail）已从树上消失',
    !/const uint32_t stress = gap_ev \+ late_ev \+ forced_ev \+ audrop_ev \+ decfail_ev;/.test(pcC));
  ok('T3：warmup 表达式在树上',
    /const int warmup = window_index < PEER_REMB_WARMUP_WINDOWS;/.test(pcC));
  ok('T3：back_off = congested && !warmup 是唯一 md 触发',
    /const int back_off = congested && !warmup;/.test(pcC));
  ok('T2：审稿的 min 钳制未被加回（树上没有 recent_measured 环形数组）',
    !/recent_measured/.test(pcC));
  ok('旧 v1（max(measured, FLOOR) 常量声明）已从树上消失',
    !/measured_bps < \(uint64_t\)PEER_REMB_FLOOR_BPS/.test(pcC));
  ok('报文实参走声明值（remb_declared_bps 驱动）',
    /pc->remb_bitrate_bps = declared;/.test(pcC));
  ok('发送诊断行带 netStress / selfStress / warmup / dir',
    /rtcp_remb sent=1 bitrateBps=%u measuredBps=%llu stressEv=%u netStress=%u selfStress=%u warmup=%u windowPkts=%u dir=%s/.test(pcC));
  ok('dir 标签含 est（实测跟踪模式）', /"est"/.test(pcC));
  ok('任务 D 时间线行 remb_state 存在（不变）',
    /remb_state declared=%u measuredLast=%u stressTotal=%u keyframeAgoMs=%u/.test(pcC));
  ok('时间线只是观测（不改声明值）：log_remb_state 不写 remb_declared_bps',
    !/peer_connection_log_remb_state[\s\S]{0,900}?remb_declared_bps\s*=/.test(pcC));
}

// ===========================================================================
// 3. 真机判据出口
// ===========================================================================
section('3 真机判据出口（引擎侧）');
{
  const backend = readFileSync(join(ENGINE, 'source', 'webrtc_backend_libpeer.cc'), 'utf8');
  ok('getStats 里有 rembSent', backend.includes('\\"rembSent\\"'));
  ok('getStats 里有 rembBitrateBps', backend.includes('\\"rembBitrateBps\\"'));
  ok('诊断结构体带 remb_sent / remb_bitrate_bps', /diag\.remb_sent/.test(backend) && /diag\.remb_bitrate_bps/.test(backend));
}

// ===========================================================================
// 4. 判据本体：读补丁真代码，真求值（A/B/C 必须分得开）
// ===========================================================================
let metaFail = 0;
const bad = (name, msg) => { metaFail += 1; ok(name, false, msg ? `解析失败：${msg}` : '解析失败'); };

// 补丁同步状态（只提示，不进判定）：内层补丁是树的派生物，修改4 之后要
// inner-sync 重新生成；同步前 --patch 模式会对新断言红（诚实红）。
if (AUTO_PATCH) {
  try {
    const ptxt = readFileSync(AUTO_PATCH, 'utf8');
    if (ptxt.includes('PEER_REMB_AI_STEP_BPS') || !ptxt.includes('PEER_REMB_MEASURED_NUM')) {
      console.log(`\nWARN: 内层补丁 ${AUTO_PATCH} 仍是旧 AIMD 语义（未随 0018/修改4 同步）；`);
      console.log('      本次判据读树上源码。inner-sync 重新生成补丁前，--patch 模式会对新断言红。');
    }
  } catch { /* ignore */ }
}

section(`4 读真代码（${SOURCE_KIND}）：抽出估计器决策并实际求值`);
{
  const srcRes = attempt(() => {
    if (!SOURCE_PATH) throw new Error('没找到真代码（--patch 可指定补丁副本）');
    return readFileSync(SOURCE_PATH, 'utf8');
  });
  if (srcRes.ok) ok(`真代码可读（${SOURCE_PATH}）`, true);
  else bad('真代码可读', srcRes.msg);

  // 补丁是 unified diff，抽新侧；树上源码本身就是文本
  const isPatch = /\.patch$/.test(SOURCE_PATH || '');
  const sidesRes = attempt(() => (isPatch
    ? parsePatch(srcRes.ok ? srcRes.value : '').map(newSideText)
    : [srcRes.ok ? srcRes.value : '']));
  if (sidesRes.ok) ok(isPatch ? '补丁能解析成文件/hunk 结构' : '树上源码直接作为真代码', true);
  else bad('真代码文本可解析', sidesRes.msg);
  const newSides = sidesRes.ok ? sidesRes.value : [];
  const side = newSides.find((s) => s.includes('peer_connection_maybe_send_remb')) || '';

  const bodyRes = attempt(() => {
    const b = findFunctionBody(side, 'peer_connection_maybe_send_remb');
    if (b === null) throw new Error('找不到函数体');
    return b;
  });
  if (bodyRes.ok) ok('能切出 maybe_send_remb 函数体', true);
  else bad('能切出 maybe_send_remb 函数体', bodyRes.msg);
  const bodyToks = bodyRes.ok ? tokenize(bodyRes.value) : [];

  // 常量：从真代码读，不写死
  const consts = {};
  for (const [name, expect] of [['PEER_REMB_START_BPS', C.START], ['PEER_REMB_FLOOR_BPS', C.FLOOR],
    ['PEER_REMB_CEIL_BPS', C.CEIL], ['PEER_REMB_MEASURED_NUM', C.MEAS_NUM],
    ['PEER_REMB_MEASURED_DEN', C.MEAS_DEN], ['PEER_REMB_MD_NUM', C.MD_NUM],
    ['PEER_REMB_MD_DEN', C.MD_DEN], ['PEER_REMB_LOSS_PCT', C.LOSS_PCT],
    ['PEER_REMB_WARMUP_WINDOWS', C.WARMUP]]) {
    const r = attempt(() => {
      for (const s of newSides) {
        const m = new RegExp(`^[ \\t]*#define[ \\t]+${name}[ \\t]+(\\d+)(?:[uUlL]*)`, 'm').exec(s);
        if (m) return parseInt(m[1], 10);
      }
      throw new Error('找不到 #define ' + name);
    });
    if (r.ok) { consts[name] = r.value; ok(`从真代码读出 ${name} = ${expect}`, r.value === expect, `实际 ${r.value}`); }
    else { consts[name] = expect; bad(`从真代码读出 ${name} = ${expect}`, r.msg); }
  }
  const aiRes = attempt(() => {
    for (const s of newSides) {
      if (/^[ \t]*#define[ \t]+PEER_REMB_AI_STEP_BPS/m.test(s))
        throw new Error('PEER_REMB_AI_STEP_BPS 还在（爬升应已删除）');
    }
    return true;
  });
  if (aiRes.ok) ok('PEER_REMB_AI_STEP_BPS 已从真代码删除', true);
  else bad('PEER_REMB_AI_STEP_BPS 已从真代码删除', aiRes.msg);

  // 表达式链：*_ev -> net/self -> stress -> congested -> warmup -> back_off
  //          span_bytes/span_ms -> measured_bps -> estimate -> declared
  const DECL_NAMES = ['gap_ev', 'late_ev', 'forced_ev', 'audrop_ev', 'decfail_ev', 'pkts',
    'net_stress', 'self_stress', 'stress', 'congested', 'warmup', 'back_off',
    'span_bytes', 'span_ms', 'measured_bps', 'estimate', 'declared', 'bitrate'];
  const decls = {};
  for (const nm of DECL_NAMES) {
    const r = attempt(() => {
      const d = findDeclInit(bodyToks, nm);
      if (!d) throw new Error('找不到声明 ' + nm);
      return d;
    });
    if (r.ok) { decls[nm] = r.value; ok(`真代码：${nm} 表达式解析成功`, true, r.value.src); }
    else bad(`真代码：${nm} 表达式解析成功`, r.msg);
  }
  // 0018：决策是 if/else 链，不再是一个三元。
  const decRes = attempt(() => {
    const d = findIfElseAssign(bodyToks, 'declared');
    if (!d) throw new Error('找不到 declared 的 if/else 决策块');
    return d;
  });
  if (decRes.ok) ok('真代码：declared 的 if/else 决策块解析成功', true, decRes.value.src.slice(0, 160));
  else bad('真代码：declared 的 if/else 决策块解析成功', decRes.msg);
  const shapeRes = attempt(() => {
    const d = decRes.value;
    if (d.then.length !== 1 || d.then[0].kind !== 'assign')
      throw new Error('then 分支应恰好 1 个赋值（冷启动 START），实际 ' + d.then.length);
    if (d.else.length !== 4 || d.else[0].kind !== 'assign'
        || d.else[1].kind !== 'if' || d.else[2].kind !== 'if' || d.else[3].kind !== 'if')
      throw new Error('else 分支应为 [估计赋值, if(md), if(floor), if(ceil)]，实际 '
        + d.else.map((s) => s.kind).join(','));
    if (!/PEER_REMB_START_BPS/.test(d.then[0].src))
      throw new Error('then 赋值应是 PEER_REMB_START_BPS：' + d.then[0].src);
    if (!/estimate/.test(d.else[0].src))
      throw new Error('else 基准应是实测估计（estimate 变量）：' + d.else[0].src);
    if (!/PEER_REMB_MD_NUM/.test(d.else[1].src))
      throw new Error('md 触发应带 MD 因子：' + d.else[1].src);
    if (!/PEER_REMB_FLOOR_BPS/.test(d.else[2].src) || !/PEER_REMB_CEIL_BPS/.test(d.else[3].src))
      throw new Error('后两个 if 应是 floor/ceil 夹取');
    return true;
  });
  if (shapeRes.ok) ok('真代码：决策结构 = 冷启动 / 实测估计 + md + 双夹取（无第三钳制）', true);
  else bad('真代码：决策结构 = 冷启动 / 实测估计 + md + 双夹取（无第三钳制）', shapeRes.msg);
  const argRes = attempt(() => {
    const call = findCall(bodyToks, 'rtcp_get_remb');
    if (!call) throw new Error('找不到 rtcp_get_remb 调用');
    const argToks = call.args[call.args.length - 1];
    const p = new ExprParser(argToks, 0);
    const ast = p.parseExpression();
    if (p.pos !== argToks.length) throw new Error('位率实参解析不完');
    return { ast, src: srcOf(argToks, 0, argToks.length) };
  });
  if (argRes.ok) ok('真代码：进报文的位率实参解析成功', true, argRes.value.src);
  else bad('真代码：进报文的位率实参解析成功', argRes.msg);
  const diagRes = attempt(() => {
    for (const callName of ['peer_connection_diag_log']) {
      const call = findCall(bodyToks, callName);
      if (!call) continue;
      const fmt = call.args[0][0]?.v;
      if (typeof fmt !== 'string') continue;
      const specs = formatSpecs(fmt);
      for (let i = 0; i < specs.length; i++) {
        if (!specs[i].prefix.endsWith('bitrateBps=')) continue;
        const argToks = call.args[i + 1];
        const p = new ExprParser(argToks, 0);
        return { ast: p.parseExpression(), src: srcOf(argToks, 0, argToks.length) };
      }
    }
    throw new Error('诊断行里没有 bitrateBps= 实参');
  });
  if (diagRes.ok) ok('真代码：诊断行 bitrateBps 实参解析成功', true, diagRes.value.src);
  else bad('真代码：诊断行 bitrateBps 实参解析成功', diagRes.msg);

  const allOk = DECL_NAMES.every((nm) => decls[nm]) && decRes.ok && shapeRes.ok && argRes.ok && diagRes.ok;

  /**
   * 用真代码跑一个窗口：输入 = 滑窗两窗的字节/时长（pc->remb_prev_window_*
   * 与 bytes/elapsed_ms）+ 上一声明值 + 五路计数的窗口增量 + 包数 + 窗口序号
   * （暖机时钟）。window_index 场景直接给。
   */
  function runWindow(env) {
    const e = { ...env, ...consts };
    for (const nm of ['gap_ev', 'late_ev', 'forced_ev', 'audrop_ev', 'decfail_ev', 'pkts',
      'net_stress', 'self_stress', 'stress', 'congested', 'warmup', 'back_off',
      'span_bytes', 'span_ms', 'measured_bps', 'estimate']) {
      e[nm] = evalAst(decls[nm].ast, e);
    }
    e.declared = evalAst(decls.declared.ast, e);
    const d = decRes.value;
    const branch = evalAst(d.cond, e) ? d.then : d.else;
    for (const s of branch) {
      if (s.kind === 'assign') e.declared = evalAst(s.ast, e);
      else if (evalAst(s.cond, e)) e.declared = evalAst(s.ast, e);
    }
    e.bitrate = evalAst(decls.bitrate.ast, e);
    const inPacket = evalAst(argRes.value.ast, { ...e, declared: e.declared });
    const inDiag = evalAst(diagRes.value.ast, { ...e, declared: e.declared });
    return {
      declared: e.declared, inPacket, inDiag,
      congested: e.congested, warmup: e.warmup, back_off: e.back_off,
      net: e.net_stress, self: e.self_stress, measured: e.measured_bps,
    };
  }

  // 场景底座：滑窗两窗各 250 kB / 1000 ms（稳态实测 2 Mbit/s，估计 2.2 M）；
  // 窗口序号 9 = 已出暖机期
  const mkEnv = (o) => ({
    bytes: 250000, elapsed_ms: 1000,
    'pc->remb_prev_window_bytes': 250000, 'pc->remb_prev_window_ms': 1000,
    'pc->remb_declared_bps': 0, window_index: 9,
    gaps_now: 0, gaps_prev: 0, late_now: 0, late_prev: 0,
    forced_now: 0, forced_prev: 0, audrop_now: 0, audrop_prev: 0,
    decfail_now: 0, decfail_prev: 0, pkts_now: 370, pkts_prev: 0, ...o });

  // 判据的灵魂：同样的实测、三组不同的丢包特征
  const scenA = mkEnv({});                                              // A：quiet
  const scenSelf = mkEnv({ forced_now: 14, audrop_now: 6 });            // C：self=20，net=0
  const scenNet = mkEnv({ gaps_now: 14 });                              // B：net=14/370 ≈ 3.8%

  if (!allOk) {
    ok(' A/B/C 三态行为分得开（真代码）', false, '真代码未全部解析，无法验证');
  } else {
    const outA = attempt(() => runWindow(scenA));
    const outSelf = attempt(() => runWindow(scenSelf));
    const outNet = attempt(() => runWindow(scenNet));
    if (outA.ok && outSelf.ok && outNet.ok) {
      const a = outA.value;
      const s = outSelf.value;
      const n = outNet.value;
      eq('A（quiet，实测 2 M）声明值 = 实测 x 11/10 = 2.2 M', a.declared, 2200000);
      eq(' C（self_stress 20/370）不改行为 = 与 A 相同', s.declared, 2200000);
      eq(' C 与 A 逐位相同（self stress 不改行为，只进诊断）', s.declared, a.declared);
      eq(' C 的 congested 判据为 0（net 视角静默）', s.congested, 0);
      eq('B（net_stress 3.8%）声明值 = 估计 x 7/10 = 1.54 M', n.declared, Math.trunc(2200000 * C.MD_NUM / C.MD_DEN));
      ok(' B 与 A/C 声明值不同（真代码判据本体）', a.declared > n.declared, `A ${a.declared} / C ${s.declared} / B ${n.declared}`);
      ok(' 三个窗口的实测值确实相同（控制变量成立）', a.measured === s.measured && s.measured === n.measured, `${a.measured}`);
      ok(' B 的 md 触发链：congested=1 且 back_off=1', n.congested === 1 && n.back_off === 1);
      ok(' C 的 md 触发链：back_off=0（不触发）', s.back_off === 0);
      ok(' 报文里装的就是声明值（A/C/B 场景）',
        a.inPacket === a.declared && s.inPacket === s.declared && n.inPacket === n.declared);
      ok(' 诊断行记录的也是声明值（A/C/B）',
        a.inDiag === a.declared && s.inDiag === s.declared && n.inDiag === n.declared);
      ok(' 声明值都在 [FLOOR, CEIL] 内',
        a.declared >= C.FLOOR && a.declared <= C.CEIL && s.declared >= C.FLOOR && s.declared <= C.CEIL
        && n.declared >= C.FLOOR && n.declared <= C.CEIL);
    } else {
      metaFail += 1;
      ok(' A/B/C 三态行为分得开（真代码）', false,
        (outA.ok ? '' : outA.msg + ' ') + (outSelf.ok ? '' : outSelf.msg + ' ') + (outNet.ok ? '' : outNet.msg));
    }

    // 冷启动 + 夹取 + 跟踪（真代码）
    const cold = attempt(() => runWindow(mkEnv({
      bytes: 0, 'pc->remb_prev_window_bytes': 0, gaps_now: 50, pkts_now: 500 })));
    if (cold.ok) {
      eq('无测量（span_bytes==0）==> START 200 k（哪怕 net_stress 在场）', cold.value.declared, C.START);
    } else { metaFail += 1; ok('冷启动场景可求值', false, cold.msg); }

    const clampGuard = attempt(() => ({
      low: runWindow(mkEnv({ bytes: 60, 'pc->remb_prev_window_bytes': 60, 'pc->remb_declared_bps': 4000000 })),
      lowNet: runWindow(mkEnv({ bytes: 60, 'pc->remb_prev_window_bytes': 60, gaps_now: 50, pkts_now: 500 })),
      hi: runWindow(mkEnv({ bytes: 2000000, 'pc->remb_prev_window_bytes': 2000000 })),
    }));
    if (clampGuard.ok) {
      const g = clampGuard.value;
      eq('低实测（480 bps）quiet 夹到 FLOOR 500 k（跟踪实测，不爬升）', g.low.declared, C.FLOOR);
      eq('低实测 + net_stress 也不跌破 FLOOR', g.lowNet.declared, C.FLOOR);
      eq('高实测（16 M）夹到 CEIL 8 M', g.hi.declared, C.CEIL);
      ok('无爬升：声明值与上一窗无关（上一声明 4M 会被拉回实测）',
        g.low.declared < 4000000, `${g.low.declared} < 4000000`);
    } else {
      metaFail += 1;
      ok('夹取场景可求值', false, clampGuard.msg);
    }

    // T3 暖机（真代码）：前 5 窗（序号 0..4）net_stress 高也不扣；第 6 窗起扣
    const warmRes = attempt(() => {
      const base = mkEnv({ gaps_now: 50, pkts_now: 500, pkts_prev: 0 }); // net=50/500 = 10%
      const outs = [];
      let prev = 0;
      for (let k = 0; k <= 5; k++) {
        const r = runWindow({ ...base, window_index: k, 'pc->remb_declared_bps': prev });
        outs.push(r);
        prev = r.declared;
      }
      return outs;
    });
    if (warmRes.ok) {
      const w = warmRes.value;
      ok('T3：窗 0..4（暖机）net_stress 10% 不降 = 估计 2.2 M',
        w.slice(0, 5).every((r) => r.declared === 2200000),
        w.map((r) => r.declared).join(','));
      ok('T3：窗 0..4 的 warmup 标志 = 1（只记不动作）',
        w.slice(0, 5).every((r) => r.warmup === 1));
      ok('T3：窗 0..4 的 back_off = 0（暖机压住 md）',
        w.slice(0, 5).every((r) => r.back_off === 0));
      eq('T3：第 6 窗（序号 5）起 md 生效 = 1.54 M', w[5].declared,
        Math.trunc(2200000 * C.MD_NUM / C.MD_DEN));
      ok('T3：第 6 窗 warmup=0 且 back_off=1', w[5].warmup === 0 && w[5].back_off === 1);
      ok('T3：第 6 窗确实低于第 5 窗', w[5].declared < w[4].declared, `${w[4].declared} -> ${w[5].declared}`);
    } else {
      metaFail += 1;
      ok('T3：暖机序列可求值', false, warmRes.msg);
    }

    // 无爬升 + self_stress（真代码）
    const trackRes = attempt(() => {
      const flat = [];
      let prev = 0;
      for (let i = 0; i < 6; i++) {
        const r = runWindow(mkEnv({ window_index: i + 3, 'pc->remb_declared_bps': prev }));
        flat.push(r.declared);
        prev = r.declared;
      }
      const selfStress = runWindow(mkEnv({ forced_now: 50, audrop_now: 50, window_index: 9 }));
      return { flat, selfStress };
    });
    if (trackRes.ok) {
      ok('无爬升（真代码）：同实测 6 窗声明值恒定 2.2 M',
        trackRes.value.flat.every((v) => v === 2200000), trackRes.value.flat.join(','));
      eq('持续 self_stress 永不 md（真代码）', trackRes.value.selfStress.declared, 2200000);
      eq(' self_stress 窗 back_off = 0', trackRes.value.selfStress.back_off, 0);
    } else {
      metaFail += 1;
      ok('无爬升/self_stress 场景可求值', false, trackRes.msg);
    }

    // T2 的 min 钳制挡板：声明值只跟实测走，任何"与实测中位数取 min"的
    // 复活都会被「上一声明 4M 被拉回 500k」这组场景打红（上面 clampGuard）。
    ok('0018 行为契约都有对应断言（跟踪实测 / net_stress 回退 / self_stress 与暖机不动作 / 双夹取 / 冷启动）', true);
  }
}

// ===========================================================================
// 5. 元判据
// ===========================================================================
section('5 元判据（解析失败必须红）');
{
  ok('真代码全部解析成功 ==> 判据确实在检查真代码', metaFail === 0, metaFail === 0 ? '' : `${metaFail} 处解析失败`);
  ok('判据读的是 C 真代码（默认树上 peer_connection.c；--patch 指向补丁副本做变异测试）',
    SOURCE_PATH !== null && metaFail === 0,
    SOURCE_PATH ? `真代码：${SOURCE_PATH}` : '没找到真代码');
}

console.log(`\n=== 结果：${passed} 通过 / ${failed} 失败 ===`);
if (failed) {
  console.log('失败项：');
  for (const f of failures) console.log('  - ' + f);
  process.exit(1);
}

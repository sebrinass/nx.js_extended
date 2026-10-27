#!/usr/bin/env node
/* ============================================================================
 *  0010-hole-attribution-check.mjs —— 内层补丁 0010 的**判据自检**（host 上跑，零编译）
 * ----------------------------------------------------------------------------
 *  跑法：
 *      node engine/tools/libpeer-switch/0010-hole-attribution-check.mjs
 *
 *  ★ 这一版对应的是**瘦身后**的 0010（只保留「一个丢包只赔一帧」+ 两侧对称，
 *    **不再**动重排窗口 / 等待时间）。
 *
 *  ★ 为什么需要它（AGENTS.md §6.2 第 8 条：判据在故障时必须真的会变）
 *
 *    ★★ 先说清楚：**这一条在 2026-10-23 那次故障里从未被触发过**。
 *       真机报告的 packetsLost = 0，而 packetsLost 就是
 *       vstats.sequence_gaps（引擎 webrtc_backend_libpeer.cc）⇒ 那一轮
 *       **一个序号空洞都没有** ⇒ 「缺口 ⇒ 整帧被丢」这条链根本没发生。
 *       ⇒ 0010 **不是**本次的修复，只是消除 H.264/H.265 的不对称。
 *
 *    这个 host 自检能保证的是：
 *    ① 0010 想修的那条规则本身是对的（下面第 1 节：一个**行为模型**，
 *       同一条有缺口的流，旧规则赔两帧、新规则赔一帧 ⇒ 判据真的会变）；
 *    ② 源码里那些**会被无声改回去**的东西还在（下面第 2 节：源码不变量），
 *       特别是「**没有**把重排窗口改大」这条**反向**不变量 —— 一次只改一个
 *       变量，这一轮**不许**动窗口。
 *
 *  ★ 边界：这是**模型 + 源码不变量**检查，**不是**对 C 代码的编译/单测。
 * ========================================================================== */

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const HERE = dirname(fileURLToPath(import.meta.url));
const LIBPEER = join(HERE, '..', '..', 'extern', 'libpeer');
const ENGINE = join(HERE, '..', '..');

let passed = 0;
let failed = 0;
const failures = [];

function ok(name, cond, extra) {
  if (cond) {
    passed += 1;
    console.log(`  ✔ ${name}${extra ? '  ' + extra : ''}`);
  } else {
    failed += 1;
    failures.push(name + (extra ? '  ' + extra : ''));
    console.log(`  ✘ ${name}${extra ? '  ' + extra : ''}`);
  }
}

function eq(name, actual, expected) {
  ok(name, String(actual) === String(expected),
    String(actual) === String(expected) ? '' : `期望 ${expected}，实际 ${actual}`);
}

function section(t) {
  console.log(`\n=== ${t} ===`);
}

// ===========================================================================
// 1. 行为模型：一个丢包赔几帧（0010 的核心规则）
// ===========================================================================
//
// 模型严格照抄 src/rtp.c 的结构：
//   · 「损坏」是**解码器级**的标志（au_damaged），不是某个 AU 的标志；
//   · 缺口被检测到时，标志打在**当时开着的那一帧**上；
//   · rtp_decoder_begin_access_unit() 在时间戳变化时交出上一帧，
//     交出（flush_access_unit → reset_access_unit）会把标志**清掉**
//     ⇒ 下一帧是干净的；
//   · ★ 0010 删掉的正是「交出上一帧之后**再打一次**标志」那一句 ——
////     那一句打在**下一帧**身上，而下一帧其实是完整的。
// ===========================================================================

/** oldRule=true ⇒ 模拟修复前的行为（缺口连坐下一帧）。 */
class Reassembler {
  constructor(oldRule = false) {
    this.oldRule = oldRule;
    this.au = null;
    this.damaged = false;     // ★ 解码器级的「损坏」标志
    this.lastSeq = null;
    this.delivered = [];
    this.dropped = 0;
  }

  /** 收到一个包（seq 连续或跳号）。 */
  push(seq, timestamp) {
    const gap = this.lastSeq !== null && this.lastSeq + 1 !== seq;
    // ★ 缺口打在**当时开着的那一帧**上（第一句，修复前后都一样）。
    if (gap) this.damaged = true;
    this.lastSeq = seq;

    // 新的一帧 ⇒ 交出上一帧（交出会清掉 damaged）。
    if (this.au && this.au.timestamp !== timestamp) this.flush();
    if (!this.au) this.au = { timestamp, packets: 0 };
    this.au.packets += 1;

    // ★ 修复前还有这一句：交出上一帧之后**再打一次**标志
    //   ⇒ 下一帧（完整的）被连坐丢掉。
    if (this.oldRule && gap) this.damaged = true;
  }

  /** 帧尾（marker）⇒ 交出这一帧。 */
  flush() {
    if (!this.au) return;
    if (!this.damaged && this.au.packets > 0) this.delivered.push(this.au);
    else this.dropped += 1;
    this.au = null;
    this.damaged = false;     // reset_access_unit 清标志
  }
}

section('① 行为模型：同一个缺口赔几帧（0010 的规则）');
{
  // 场景 1：缺口在帧**内部**（第二个包丢了）⇒ 这一帧不完整 ⇒ 丢（两侧一样）。
  {
    const r = new Reassembler();
    r.push(10, 1000);   // 帧 A，包 1
    r.push(12, 1000);   // 包 3 到了（包 2 丢了）⇒ 帧 A 内部有缺口
    r.flush();
    eq('帧内缺口 ⇒ 该帧被丢', r.delivered.length, 0);
    eq('帧内缺口 ⇒ dropped=1', r.dropped, 1);
  }

  // 场景 2：缺口是**上一帧的尾巴**（帧 A 的最后一个包丢了）⇒ 帧 B 完整 ⇒ 交付。
  //   ★ 这是 0010 修掉的「连坐」：旧规则会把帧 B 一起丢掉。
  {
    const r = new Reassembler();
    r.push(10, 1000);   // 帧 A
    r.push(11, 1000);
    r.push(13, 2000);   // 帧 B 的第一个包（包 12 丢了 ⇒ 那是帧 A 的尾巴）
    r.push(14, 2000);
    r.flush();
    eq('★ 上一帧的尾巴丢了 ⇒ 这一帧照样交付', r.delivered.length, 1);
    eq('★ 交付的是完整的那一帧', r.delivered[0] && r.delivered[0].timestamp, 2000);
    eq('★ 缺包的那一帧被丢掉', r.dropped, 1);
  }

  // 场景 2b：★ 同一条流，**旧规则**赔两帧、**新规则**赔一帧 ⇒ 判据真的会变。
  {
    const old = new Reassembler(true);
    old.push(10, 1000);
    old.push(11, 1000);
    old.push(13, 2000);
    old.push(14, 2000);
    old.flush();
    const neu = new Reassembler();
    neu.push(10, 1000);
    neu.push(11, 1000);
    neu.push(13, 2000);
    neu.push(14, 2000);
    neu.flush();
    eq('★ 旧规则：赔两帧（交付 0 · 丢 2）', old.delivered.length + '/' + old.dropped, '0/2');
    eq('★ 新规则：赔一帧（交付 1 · 丢 1）', neu.delivered.length + '/' + neu.dropped, '1/1');
    ok('★ 判据真的会变（同一条流，两条规则的 dropped 不一样）',
      old.dropped !== neu.dropped,
      `旧 ${old.dropped} / 新 ${neu.dropped}`);
  }

  // 场景 3：缺口是**这一帧的第一个包**（上一帧已被 marker 交出）⇒ 这一帧不完整 ⇒ 丢。
  {
    const r = new Reassembler();
    r.push(10, 1000);
    r.flush();          // 帧 A 交出（marker）
    r.push(12, 2000);   // 帧 B 的第一个包（11 丢了）⇒ 帧 B 少了一个包
    r.push(13, 2000);
    r.flush();
    eq('本帧的第一个包丢了 ⇒ 丢', r.delivered.length, 1);   // 只有帧 A
    eq('本帧的第一个包丢了 ⇒ dropped=1', r.dropped, 1);
  }

  // 场景 4：没有缺口 ⇒ 全部交付（★ 真机那一轮就是这个形状：packetsLost = 0）。
  {
    const r = new Reassembler();
    for (let i = 0; i < 5; i++) r.push(20 + i, 3000);
    r.flush();
    eq('没有缺口 ⇒ 交付', r.delivered.length, 1);
    eq('没有缺口 ⇒ dropped=0', r.dropped, 0);
  }
}

// ===========================================================================
// 2. 源码不变量：0010 改了的东西不许被无声改回去
// ===========================================================================
section('② 源码不变量（0010 还在树上）');
{
  const rtpC = readFileSync(join(LIBPEER, 'src', 'rtp.c'), 'utf8');
  const rtpH = readFileSync(join(LIBPEER, 'src', 'rtp.h'), 'utf8');
  const pcC = readFileSync(join(LIBPEER, 'src', 'peer_connection.c'), 'utf8');

  // ---- ① 「连坐下一帧」那一句必须已经不在了（两侧都没有）----
  const unconditional = /au_ssrc = view\.ssrc;\s*\n\s*if \(sequence_gap\)\s*\n\s*rtp_decoder->au_damaged = 1;/.test(rtpC);
  ok('★ 旧的「缺口连坐下一帧」已经删掉（H.264 + H.265 两处）', !unconditional);

  // ---- ② 第一句仍然在「if (sequence_gap)」里面（缺口算在当前帧头上）----
  /** 取出一个函数里 "if (sequence_gap) { ... }" 这一整块（按括号配对）。 */
  function gapBlock(src, fnName) {
    const i = src.indexOf(fnName);
    if (i < 0) return '';
    const start = src.indexOf('if (sequence_gap) {', i);
    if (start < 0) return '';
    let depth = 0;
    for (let j = start; j < src.length; j++) {
      if (src[j] === '{') depth++;
      else if (src[j] === '}') {
        depth--;
        if (depth === 0) return src.slice(start, j + 1);
      }
    }
    return '';
  }
  const hb = gapBlock(rtpC, 'rtp_decode_h264');
  const hg = gapBlock(rtpC, 'rtp_decode_h265');
  ok('★ 两侧的缺口处理块都能定位到', !!(hb && hg));
  ok('★ 缺口仍然算在**当时开着的那一帧**上',
    /sequence_gaps\+\+;[\s\S]*?au_damaged = 1;/.test(hb || ''));
  // 每个函数里只应剩下一处 "if (sequence_gap)"（第二句无条件的已经删掉）
  const h264Fn = rtpC.slice(rtpC.indexOf('static int rtp_decode_h264'), rtpC.indexOf('static int rtp_decode_h264') + 6000);
  const h265Fn = rtpC.slice(rtpC.indexOf('static int rtp_decode_h265'), rtpC.indexOf('static int rtp_decode_h265') + 6000);
  eq('★ H.264 里只剩一处 "if (sequence_gap)"', (h264Fn.match(/if \(sequence_gap\)/g) || []).length, 1);
  eq('★ H.265 里只剩一处 "if (sequence_gap)"', (h265Fn.match(/if \(sequence_gap\)/g) || []).length, 1);

  // ---- ③ 两侧（H.264 / H.265）文本对称 ----
  // 0010 注：先剥 \r 再比对 —— 补丁栈在这个树里留下的插入行是 LF、
  // 原文件是 CRLF，同一函数里两种结尾并存是常态（apply 用
  // --ignore-whitespace）；「对称」的意图是内容对称，不是字节对称。
  const norm = (s) => s.replace(/\r/g, '').replace(/H26[45]/g, 'H26x');
  ok('★ H.264 与 H.265 的缺口处理**文本对称**（除了 codec 名）',
    norm(hb) === norm(hg),
    '两侧注释/结构一致');

  // ---- ④ ★ 反向不变量：**这一轮不许**动重排窗口 ----
  const win = /#define RTP_REORDER_WINDOW (\d+)/.exec(rtpH);
  const holdPackets = /#define RTP_REORDER_MAX_HOLD_PACKETS (\d+)/.exec(rtpH);
  ok('rtp.h 里有 RTP_REORDER_WINDOW', !!win);
  ok('rtp.h 里有 RTP_REORDER_MAX_HOLD_PACKETS', !!holdPackets);
  if (win && holdPackets) {
    eq('★ 窗口**没有**被改大（一次只改一个变量）', Number(win[1]), 128);
    eq('★ 等待上限**没有**被改大', Number(holdPackets[1]), 64);
  }
  ok('★ 没有留下 RTP_REORDER_HOLD_MS_MIN/MAX（等待时间仍然是常量 30 ms）',
    !/RTP_REORDER_HOLD_MS_MIN/.test(rtpH) && !/RTP_REORDER_HOLD_MS_MAX/.test(rtpH));
  ok('★ RtpDecoder 里没有 reorder_hold_ms 字段（结构体布局没动）',
    !/reorder_hold_ms/.test(rtpH));
  ok('★ peer_connection.c 里没有 RTT 驱动的等待时间（peer_connection_update_video_reorder_hold）',
    !/peer_connection_update_video_reorder_hold/.test(pcC));
  ok('★ rtp.c 里没有 rtp_decoder_hold_ms()（缺口超时仍然是常量）',
    !/rtp_decoder_hold_ms\(/.test(rtpC));
}

// ===========================================================================
// 3. 真机判据的出口：getStats 里真的有那些字段
// ===========================================================================
section('③ 真机判据的出口（外层补丁 0042 / 0044）');
{
  const backend = readFileSync(join(ENGINE, 'source', 'webrtc_backend_libpeer.cc'), 'utf8');
  const fields = [
    'videoRtpPackets', 'videoRtpSequenceGaps', 'videoRtpReordered',
    'videoRtpLateDropped', 'videoRtpForcedSkips', 'videoRtpBuffered',
    'videoAuCompleted', 'videoAuDropped', 'videoNackRequests', 'videoNackPackets',
  ];
  for (const f of fields) {
    ok(`getStats 里有 ${f}`, backend.includes(`\\"${f}\\"`));
  }
  // facade 是原样透传 ⇒ 字段到得了页面
  const facade = readFileSync(join(ENGINE, '..', 'facade', 'src', 'rtc-peer-connection.ts'), 'utf8');
  ok('facade 的 getStats 是原样透传（没有白名单）',
    /getStats\([^)]*\)[^{]*\{[\s\S]{0,300}return \[resolved\]/.test(facade));
}

console.log(`\n=== 结果：${passed} 通过 / ${failed} 失败 ===`);
if (failed) {
  console.log('失败项：');
  for (const f of failures) console.log('  - ' + f);
  process.exit(1);
}

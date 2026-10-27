#!/usr/bin/env node
/* ============================================================================
 *  0011-pli-trigger-check.mjs —— 内层补丁 0011（B2：PLI 契约）的**判据自检**
 * ----------------------------------------------------------------------------
 *  跑法：
 *      node engine/tools/libpeer-switch/0011-pli-trigger-check.mjs
 *
 *  ★ 为什么需要它（AGENTS.md §6.2 第 8 条：判据在故障时必须真的会变）
 *
 *    这一轮的判据是「画面损坏后能不能自己长好」。真机上的判据是
 *    **真机报告**里的这几个计数器（外层补丁 0044 把它们放进 getStats）：
 *
 *      故障存在（从来不发 PLI）：
 *          videoKeyframesReceived = 0 / videoPliSent = 0
 *      契约 v1（拿到首个关键帧就永久停发）：
 *          videoPliSent 停止增长，而 videoLastKeyframeMs 无限增大
 *          —— 之后的每次丢包都让画面永远损坏（黄→黑→黄循环）
 *      契约 v2（B2，本次：关键帧过期就续问）：
 *          videoPliSent 持续缓增（健康时约每 3 s 一条），
 *          videoLastKeyframeMs 保持小值 —— 丢包后 ≤ 3 s 自愈
 *      云端不应答：
 *          videoPliSent > 0 而 videoKeyframesReceived = 0 ★ 该找云端
 *
 *    这个 host 自检保证的是：
 *    ① 0011/B2 想修的那条规则本身是对的（第 1 节：行为模型照抄 C 的
 *       时序，逐条场景断言）；
 *    ② v1 vs v2 **判据真的会变**（第 1b 节：同一条丢包时间线，v1 永不
 *       自愈、v2 在 3 s 内拿到新 IDR ⇒ 死字段不算判据）；
 *    ③ 源码里那些**会被无声改回去**的东西还在（第 2 节：源码不变量）；
 *    ④ 真机判据的出口真的接上了（第 3 节）。
 *
 *  ★ 边界：这是**模型 + 源码不变量**检查，不是对 C 代码的编译/单测。
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
// 1. 行为模型：PLI 定时器（照抄 peer_connection.c 的 B2 时序）
// ===========================================================================
//
// 模型与 src/peer_connection.c 的规则一一对应：
//   · 只有 COMPLETED 才发；
//   · media SSRC 未知就不发（ssrc == 0 ⇒ 发不出去）；
//   · 首次延迟之前不发（默认 500 ms）；
//   · ★ B2：最后一个关键帧还「新鲜」（< STALE_MS，默认 3000 ms）就不发；
//     过期（或从来没有）就发 —— 这取代了 v1 的「拿到首个关键帧就永久停发」；
//   · 两次之间至少隔一个间隔（默认 300 ms）。
// ===========================================================================

function clamp(v, lo, hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

class PliTimer {
  constructor(opts = {}) {
    this.interval = clamp(opts.interval === undefined ? 300 : opts.interval, 100, 5000);
    this.firstDelay = clamp(opts.firstDelay === undefined ? 500 : opts.firstDelay, 0, 5000);
    this.staleMs = clamp(opts.staleMs === undefined ? 3000 : opts.staleMs, 1000, 30000);
    this.state = 'checking';
    this.anchorMs = null;
    this.remoteVssrc = opts.remoteVssrc === undefined ? 0 : opts.remoteVssrc;
    this.keyframesReceived = 0;
    this.lastKeyframeMs = 0;
    this.pliSent = 0;
    this.pliLastSentMs = 0;
  }

  /** 传输建立（= PEER_CONNECTION_COMPLETED）⇒ 锚点。 */
  complete(nowMs) {
    this.state = 'completed';
    this.anchorMs = nowMs;
  }

  /** 从第一个视频 RTP 包学到 media SSRC。 */
  learnSsrc(ssrc) {
    this.remoteVssrc = ssrc;
  }

  /** 收到一个关键帧。 */
  receiveKeyframe(nowMs) {
    this.keyframesReceived += 1;
    this.lastKeyframeMs = nowMs;
  }

  /** peer_connection_loop() 的一个周期。 */
  tick(nowMs) {
    if (this.state !== 'completed') return;
    if (!this.remoteVssrc) return;
    const anchor = this.anchorMs === null ? nowMs : this.anchorMs;
    if ((nowMs - anchor) < this.firstDelay) return;
    if (this.lastKeyframeMs !== 0 && (nowMs - this.lastKeyframeMs) < this.staleMs) return;
    if (this.pliLastSentMs !== 0 && (nowMs - this.pliLastSentMs) < this.interval) return;
    this.pliSent += 1;
    this.pliLastSentMs = nowMs;
  }
}

section('① 行为模型：PLI 什么时候发（0011/B2 的规则）');
{
  // 场景 1：首次延迟之前不发。
  {
    const t = new PliTimer({ remoteVssrc: 2331692711 });
    t.complete(1000);
    t.tick(1100);   // +100 ms
    t.tick(1400);   // +400 ms
    eq('首次延迟之前不发（+400 ms < 500 ms）', t.pliSent, 0);
    t.tick(1500);   // +500 ms ⇒ 到点了
    eq('到了首次延迟就发', t.pliSent, 1);
  }

  // 场景 2：两次之间至少隔一个间隔。
  {
    const t = new PliTimer({ remoteVssrc: 2331692711 });
    t.complete(0);
    t.tick(500);
    eq('第一个 PLI 发出', t.pliSent, 1);
    t.tick(600);    // +100 ms
    eq('间隔没到不发', t.pliSent, 1);
    t.tick(799);
    eq('间隔没到不发（差 1 ms）', t.pliSent, 1);
    t.tick(800);    // +300 ms
    eq('间隔到了就发', t.pliSent, 2);
    eq('pli_last_sent_ms 记的是最后一次', t.pliLastSentMs, 800);
  }

  // 场景 3（★ B2 核心）：关键帧新鲜时不发，过期后续问。
  {
    const t = new PliTimer({ remoteVssrc: 2331692711 });
    t.complete(0);
    t.tick(500);
    t.receiveKeyframe(600);      // 首个 IDR
    t.tick(1000);                // 400 ms 新鲜
    t.tick(2000);                // 1400 ms 新鲜
    t.tick(3500);                // 2900 ms 新鲜（< 3000）
    eq('关键帧新鲜（2900 ms < 3000 ms）⇒ 不发', t.pliSent, 1);
    t.tick(3601);                // 3001 ms 过期 ⇒ 续问
    eq('★ 关键帧过期（3001 ms）⇒ 续问', t.pliSent, 2);
    t.receiveKeyframe(3800);     // 新 IDR 到了
    t.tick(5000);
    t.tick(6000);                // 2200 ms 新鲜
    eq('新关键帧又把请求压住了', t.pliSent, 2);
  }

  // 场景 4：media SSRC 未知 ⇒ 不发（发出去也是 -1）。
  {
    const t = new PliTimer();
    t.complete(0);
    t.tick(500);
    t.tick(1000);
    eq('remote_vssrc == 0 ⇒ 不发', t.pliSent, 0);
    t.learnSsrc(2331692711);
    t.tick(1100);
    eq('★ 学到 SSRC 之后就能发了', t.pliSent, 1);
  }

  // 场景 5：传输没完成 ⇒ 不发。
  {
    const t = new PliTimer({ remoteVssrc: 2331692711 });
    t.tick(500);
    t.tick(5000);
    eq('状态不是 COMPLETED ⇒ 不发', t.pliSent, 0);
  }

  // 场景 6：间隔 / 首次延迟 / 过期窗口的编译期覆盖被夹在合理范围内。
  {
    eq('间隔夹到下限', new PliTimer({ interval: 1 }).interval, 100);
    eq('间隔夹到上限', new PliTimer({ interval: 999999 }).interval, 5000);
    eq('首次延迟夹到上限', new PliTimer({ firstDelay: 999999 }).firstDelay, 5000);
    eq('过期窗口夹到下限', new PliTimer({ staleMs: 1 }).staleMs, 1000);
    eq('过期窗口夹到上限', new PliTimer({ staleMs: 999999 }).staleMs, 30000);
    eq('默认间隔是 300 ms', new PliTimer().interval, 300);
    eq('默认首次延迟是 500 ms', new PliTimer().firstDelay, 500);
    eq('默认过期窗口是 3000 ms', new PliTimer().staleMs, 3000);
  }
}

// ===========================================================================
// 1b. 契约 v1 vs 契约 v2：判据真的会变吗？
// ===========================================================================
//
//  v1 = 「拿到首个关键帧就永久停发」（0011 的第一版）。
//  v2 = 「关键帧过期（3 s）就续问」（B2，当前）。
//  云端模型 = 只在被问到时才推 IDR（收到 PLI 后 100 ms 送一个）；
//            丢包 = t = 2000 ms 起画面永久损坏，直到下一个关键帧到达。
//  ⇒ v1：丢包后永远没有新 IDR（我们不再问），画面永不自愈；
//    v2：丢包后 ≤ 3.3 s 内拿到新 IDR，画面自愈。
// ===========================================================================
section('①b 契约 v1 vs 契约 v2（判据真的会变吗）');
{
  function run(contract) {
    const t = new PliTimer({ remoteVssrc: 2331692711 });
    const cloud = { pendingKeyframeAt: null, idrDelivered: 0 };
    let pictureWhole = true;
    let wholeAgainAt = null;
    // 传输在 1000 ms 建好；之后每 50 ms 一个 loop 周期；t=2000 ms 丢包损坏画面。
    for (let now = 1000; now <= 15000; now += 50) {
      if (now === 1000) t.complete(now);
      if (contract === 'v2') {
        const before = t.pliSent;
        t.tick(now);
        if (t.pliSent > before && cloud.pendingKeyframeAt === null) {
          cloud.pendingKeyframeAt = now + 100;
        }
      } else {
        // v1：同一条时间线，但拿到首个关键帧后就不再问
        const before = t.pliSent;
        if (t.keyframesReceived === 0) t.tick(now);
        if (t.pliSent > before && cloud.pendingKeyframeAt === null) {
          cloud.pendingKeyframeAt = now + 100;
        }
      }
      if (cloud.pendingKeyframeAt !== null && now >= cloud.pendingKeyframeAt) {
        t.receiveKeyframe(now);
        cloud.idrDelivered += 1;
        cloud.pendingKeyframeAt = null;
        if (!pictureWhole) { pictureWhole = true; wholeAgainAt = now; }
      }
      if (now === 2000) pictureWhole = false; // ★ 丢包：画面损坏
    }
    return { pliSent: t.pliSent, keyframes: t.keyframesReceived,
             healed: pictureWhole, healedAt: wholeAgainAt };
  }

  const v1 = run('v1');
  const v2 = run('v2');
  eq('★ v1：丢包后画面永不自愈', v1.healed, false);
  eq('★ v2：丢包后画面自愈', v2.healed, true);
  ok('★ v2：自愈发生在丢包后 ~3.3 s 内', v2.healedAt >= 2000 && v2.healedAt <= 5500,
    `丢包 @2000ms，自愈 @${v2.healedAt}ms`);
  ok('★ v1 的关键帧停在首个（不再问 ⇒ 云端不再给）', v1.keyframes >= 1, `共 ${v1.keyframes} 个`);
  ok('★★ 判据真的会变（同一丢包时间线，v2 的关键帧数 > v1）',
    v2.keyframes > v1.keyframes, `v1 ${v1.keyframes} 个 / v2 ${v2.keyframes} 个`);
}

// ===========================================================================
// 2. 源码不变量：B2 改了的东西不许被无声改回去
// ===========================================================================
section('② 源码不变量（0011/B2 还在树上）');
{
  const rtpC = readFileSync(join(LIBPEER, 'src', 'rtp.c'), 'utf8');
  const rtpH = readFileSync(join(LIBPEER, 'src', 'rtp.h'), 'utf8');
  const pcC = readFileSync(join(LIBPEER, 'src', 'peer_connection.c'), 'utf8');
  const pcH = readFileSync(join(LIBPEER, 'src', 'peer_connection.h'), 'utf8');

  // 抽出 maybe_send_pli 的函数体（含其前的规则注释块）
  const fnMatch = pcC.match(
    /\/\/ 0011\/B2: ask the cloud[\s\S]*?static void peer_connection_maybe_send_pli\(PeerConnection\* pc\) \{[\s\S]*?\n\}/);
  ok('peer_connection_maybe_send_pli 找得到（带 B2 注释头）', !!fnMatch);
  const fn = fnMatch ? fnMatch[0] : '';

  // ---- ① 定时器在 peer_connection_loop() 里 ----
  const loop = /int peer_connection_loop\(PeerConnection\* pc\) \{[\s\S]*?\n\}/.exec(pcC);
  ok('peer_connection_loop 找得到', !!loop);
  if (loop) {
    ok('★ PLI 定时器在 peer_connection_loop 里',
      /peer_connection_maybe_send_pli\(pc\);/.test(loop[0]));
  }

  // ---- ② ★ B2：v1 的终止条件必须不存在 ----
  if (fn) {
    ok('★ v1 终止条件（keyframes_received > 0 就 return）已删除',
      !/keyframes_received > 0/.test(fn));
    ok('★ B2 的新鲜度闸门存在（last_keyframe_ms != 0 && < stale）',
      /last_keyframe_ms != 0 &&[\s\S]{0,120}?pli_stale_ms\(\)/.test(fn));
    ok('★ 间隔闸门保留', /pli_interval_ms\(\)/.test(fn));
    ok('★ 首次延迟闸门保留', /pli_first_delay_ms\(\)/.test(fn));
    ok('★ SSRC 闸门保留', /remote_vssrc/.test(fn));
    ok('★ 状态闸门保留', /PEER_CONNECTION_COMPLETED/.test(fn));
  }

  // ---- ③ 常量可以编译期覆盖，而且有上下限 ----
  ok('★ LIBPEER_PLI_STALE_MS 可以编译期覆盖',
    /#ifndef LIBPEER_PLI_STALE_MS/.test(pcC));
  ok('★ LIBPEER_PLI_STALE_MS 默认 3000 ms',
    /#define LIBPEER_PLI_STALE_MS 3000/.test(pcC));
  ok('★ 过期窗口夹在 1000..30000 ms',
    /LIBPEER_PLI_STALE_MS_MIN 1000/.test(pcC) &&
    /LIBPEER_PLI_STALE_MS_MAX 30000/.test(pcC));
  ok('★ LIBPEER_PLI_INTERVAL_MS / FIRST_DELAY_MS 仍可覆盖',
    /#ifndef LIBPEER_PLI_INTERVAL_MS/.test(pcC) &&
    /#ifndef LIBPEER_PLI_FIRST_DELAY_MS/.test(pcC));
  ok('★ 间隔仍夹在 100..5000 ms',
    /LIBPEER_PLI_INTERVAL_MS_MIN 100/.test(pcC) &&
    /LIBPEER_PLI_INTERVAL_MS_MAX 5000/.test(pcC));

  // ---- ④ 关键帧在**组帧**路径被加 1（不变，沿自 v1） ----
  const h264Count = /rtp_decoder_append_access_unit_nalu[\s\S]{0,4000}?keyframes_received\+\+/.test(rtpC);
  const h265Count = /rtp_decoder_append_h265_nalu_parts[\s\S]{0,4000}?keyframes_received\+\+/.test(rtpC);
  ok('★ H.264 的关键帧在组帧路径被加 1', h264Count);
  ok('★ H.265 的关键帧在组帧路径被加 1', h265Count);
  ok('★ H.264 只认 NAL type 5（IDR）',
    /if \(type == 5\) \{[\s\S]{0,600}?keyframes_received\+\+/.test(rtpC));
  ok('★ H.265 认 IRAP（16..23）',
    /H265_NAL_IRAP_FIRST && type <= H265_NAL_IRAP_LAST\) \{[\s\S]{0,300}?keyframes_received\+\+/.test(rtpC));

  // ---- ⑤ 计数器的位置（不变，沿自 v1） ----
  ok('★ RtpDecoder 里有 keyframes_received / last_keyframe_ms / pli_sent / pli_last_sent_ms',
    /uint32_t keyframes_received;/.test(rtpH) &&
    /uint32_t last_keyframe_ms;/.test(rtpH) &&
    /uint32_t pli_sent;/.test(rtpH) &&
    /uint32_t pli_last_sent_ms;/.test(rtpH));
  ok('★ PeerVideoRtpStats 里有同样四个字段',
    /uint32_t keyframes_received;/.test(pcH) &&
    /uint32_t pli_sent;/.test(pcH) &&
    /uint32_t pli_last_sent_ms;/.test(pcH) &&
    /uint32_t last_keyframe_ms;/.test(pcH));
  ok('★ getter 把四个字段填上了',
    /stats->keyframes_received =/.test(pcC) &&
    /stats->pli_sent =/.test(pcC) &&
    /stats->pli_last_sent_ms =/.test(pcC) &&
    /stats->last_keyframe_ms =/.test(pcC));

  // ---- ⑥ 诊断日志里能看到 PLI / 关键帧（不变，沿自 v1） ----
  ok('★ 诊断日志有 pli_state 那一行',
    /pli_state sent=%u keyframes=%u/.test(pcC));
  ok('★ rtp_h264 那条日志带上了 keyframes / pliSent',
    /auDrop=%u keyframes=%u pliSent=%u/.test(pcC));
}

// ===========================================================================
// 3. 真机判据的出口：getStats 里真的有那四个字段
// ===========================================================================
section('③ 真机判据的出口（外层补丁 0044）');
{
  const backend = readFileSync(join(ENGINE, 'source', 'webrtc_backend_libpeer.cc'), 'utf8');
  for (const f of ['videoKeyframesReceived', 'videoPliSent', 'videoPliLastMs', 'videoLastKeyframeMs']) {
    ok(`getStats 里有 ${f}`, backend.includes(`\\"${f}\\"`));
  }
  // 0042 的十个计数器也必须还在（判据要能一起看）
  for (const f of ['videoAuCompleted', 'videoAuDropped', 'videoRtpForcedSkips', 'videoNackRequests']) {
    ok(`getStats 里还有 0042 的 ${f}`, backend.includes(`\\"${f}\\"`));
  }
}

console.log(`\n=== 结果：${passed} 通过 / ${failed} 失败 ===`);
if (failed) {
  console.log('失败项：');
  for (const f of failures) console.log('  - ' + f);
  process.exit(1);
}

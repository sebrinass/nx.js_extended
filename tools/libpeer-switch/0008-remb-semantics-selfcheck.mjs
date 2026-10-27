#!/usr/bin/env node
/* ============================================================================
 *  0008-remb-semantics-selfcheck.mjs —— 判据（0008-remb-semantics-check.mjs）
 *  的**自检**：用「变异测试」证明这条判据在故障时**真的会变**。
 * ----------------------------------------------------------------------------
 *  跑法：
 *      node engine/tools/libpeer-switch/0008-remb-semantics-selfcheck.mjs
 *
 *  为什么需要它（AGENTS.md §6.2 第 8 条：判据在故障时必须真的会变）
 *
 *    判据本体自己也可能变成「假判据」。最典型的两种：
 *      · 判据是**纯正则**：只要补丁里那几句字样还在就绿，改坏语义也不红；
 *      · 判据是**一份 JS 复刻**：它自己算一遍，不读 C 代码，C 那边分支
 *        写反、实参传错，它照样绿。
 *    （0008 的判据原来就栽在这第二条上：把 FLOOR 分支写反、或把报文里
 *      的实参换成 measured_bps，判据都报全绿。）
 *
 *  本脚本做的事：**把补丁副本改坏，看判据会不会变红**。
 *
 *      M0  对照：什么都不改              ==> 判据必须绿（exit 0）
 *      M1  整段回退到旧实现（报实测码率）==> 判据必须红（exit 非 0）
 *      M2a congested 恒假（A/B 分不开）  ==> 判据必须红（exit 非 0）
 *      M2b 报文实参换成 measured_bps     ==> 判据必须红（exit 非 0）
 *      M3  T1 拆分拆掉（net_stress 合并回五路）==> 判据必须红
 *          （self_stress 高的窗口会被误判成 congested，C 场景红）
 *      M4  T3 暖机拆除（back_off 不再看 warmup）==> 判据必须红
 *          （暖机期 net_stress 窗口会降，暖机序列红）
 *      M5  T2 上限改回 20M               ==> 判据必须红
 *          （补丁读出的 CEIL 常量与 8M 不符、8M 收敛断言红）
 *      M6  T2 已否决的 min 钳制被加回    ==> 判据必须红
 *          （第三个夹取让「恰好 floor/ceil 两个」红；低实测场景的
 *            爬升断言也红）
 *
 *    每个变异都打印「改坏后 exit=? / 改回后 exit=?」，并断言
 *      改坏 ==> 红（exit 非 0）、改回 ==> 绿（exit = 0）。
 *
 *  安全：**绝不动仓库里的任何文件**。所有变异只发生在 %TEMP% 的补丁
 *    副本上，判据用 `--patch <副本>` 去读那个副本。
 * ========================================================================== */

import { readFileSync, writeFileSync, readdirSync, mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { spawnSync } from 'node:child_process';

const HERE = dirname(fileURLToPath(import.meta.url));
const CHECK = join(HERE, '0008-remb-semantics-check.mjs');

let passed = 0;
let failed = 0;
const failures = [];

function ok(name, cond, extra) {
  if (cond) {
    passed += 1;
    console.log(`  ok ${name}${extra ? '  ' + extra : ''}`);
  } else {
    failed += 1;
    failures.push(name + (extra ? '  ' + extra : ''));
    console.log(`  FAIL ${name}${extra ? '  ' + extra : ''}`);
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
// 0. 找到「被审对象」：0008 补丁 + 判据脚本
// ===========================================================================
section('0. 被审对象');
{
  let names = [];
  try { names = readdirSync(HERE); } catch { /* 忽略 */ }
  const cands = names.filter((f) => /^0008-.*\.patch$/.test(f)).sort();
  let patchPath = null;
  for (const f of cands) {
    try {
      if (readFileSync(join(HERE, f), 'utf8').includes('rtcp_get_remb')) {
        patchPath = join(HERE, f);
        break;
      }
    } catch { /* 忽略读不了的 */ }
  }
  if (!patchPath && cands.length) patchPath = join(HERE, cands[0]);
  ok('找到 0008 补丁', patchPath !== null, patchPath || '没找到 0008-*.patch');
  ok('找到判据脚本', (() => {
    try { readFileSync(CHECK, 'utf8'); return true; } catch { return false; }
  })(), CHECK);

  if (!patchPath) {
    console.log('\n=== 结果：无法继续 ===');
    process.exit(1);
  }

  var PATCH = patchPath;
  var PATCH_TEXT = readFileSync(patchPath, 'utf8');

  // ---- 在 %TEMP% 里造一个补丁副本：所有变异只发生在这里 ----
  var TMP_DIR = mkdtempSync(join(tmpdir(), 'remb-selfcheck-'));
  var TMP_PATCH = join(TMP_DIR, '0008-mutant.patch');

  /** 跑判据（指向指定补丁副本），返回退出码与失败项。 */
  function runCheck(patchFile) {
    const r = spawnSync(process.execPath, [CHECK, '--patch', patchFile], {
      encoding: 'utf8',
      cwd: HERE,
    });
    const stdout = r.stdout || '';
    const failedLines = stdout.split(/\r?\n/).filter((l) => l.includes('FAIL'));
    return {
      status: typeof r.status === 'number' ? r.status : -1,
      stdout,
      failedLines,
    };
  }

  /** 写副本 -> 跑 -> 还原 -> 跑。返回 {broken, restored}。 */
  function runMutation(mutatedText) {
    writeFileSync(TMP_PATCH, mutatedText, 'utf8');
    const broken = runCheck(TMP_PATCH);
    writeFileSync(TMP_PATCH, PATCH_TEXT, 'utf8');
    const restored = runCheck(TMP_PATCH);
    return { broken, restored };
  }

  function showBroken(label, broken) {
    console.log(`    · ${label} 判据的红项（前 3 条）：`);
    if (!broken.failedLines.length) {
      console.log('      （没有红项 —— 这就是「假绿」）');
      return;
    }
    for (const l of broken.failedLines.slice(0, 3)) console.log('      ' + l.trim());
  }

  // =========================================================================
  // M0 对照：什么都不改，判据必须绿
  // =========================================================================
  section('M0 对照（不改任何东西）');
  {
    const r = runCheck(PATCH);
    eq('M0：未改动的补丁上判据必须绿', r.status, 0);
    ok('M0：判据的断言全过（没有红项）', r.failedLines.length === 0,
      r.failedLines.length ? `有 ${r.failedLines.length} 个红项` : '');
  }

  // =========================================================================
  // M1：整段回退到旧实现（报实测码率 + 无数据就不发 + 0 就退）
  // =========================================================================
  section('M1 变异（整段回退到旧实现）');
  {
    const OLD_IMPL = [
      '+static void peer_connection_maybe_send_remb(PeerConnection* pc) {',
      '+  const uint32_t now_ms = ports_get_monotonic_time();',
      '+  if (!pc || pc->state != PEER_CONNECTION_COMPLETED || pc->remote_vssrc == 0) {',
      '+    return;',
      '+  }',
      '+  const uint64_t bytes = pc->video_bytes_since_remb;',
      '+  uint32_t elapsed_ms = PEER_REMB_INTERVAL_MS;',
      '+  pc->video_bytes_since_remb = 0;',
      '+  if (bytes == 0 || elapsed_ms == 0) {',
      '+    return;',
      '+  }',
      '+  const uint64_t measured_bps = (bytes * 8u * 1000u) / elapsed_ms;',
      '+  uint64_t bitrate = measured_bps;',
      '+  if (bitrate == 0 || bitrate > 0xFFFFFFFFull) {',
      '+    return;',
      '+  }',
      '+  uint8_t packet[64];',
      '+  int size = rtcp_get_remb(packet, sizeof(packet), pc->vrtp_encoder.ssrc,',
      '+                           pc->remote_vssrc, (uint32_t)bitrate);',
      '+  if (size <= 0) {',
      '+    return;',
      '+  }',
      '+  dtls_srtp_encrypt_rctp_packet(&pc->dtls_srtp, packet, &size);',
      '+  const int ret = agent_send(&pc->agent, packet, size);',
      '+  if (ret == size) {',
      '+    pc->remb_sent++;',
      '+    pc->remb_bitrate_bps = (uint32_t)bitrate;',
      '+  }',
      '+}',
    ].join('\n');

    const re = /^\+static void peer_connection_maybe_send_remb\(PeerConnection\* pc\) \{[\s\S]*?^\+\}$/m;
    const matched = re.test(PATCH_TEXT);
    ok('M1：能在补丁里定位到要改坏的 REMB 函数（变异真的生效了）', matched,
      matched ? '' : '没匹配到，变异没生效，这个变异测试是空的');
    const mutated = matched ? PATCH_TEXT.replace(re, OLD_IMPL) : PATCH_TEXT;
    ok('M1：补丁副本确实被改动了（变异不是空操作）', mutated !== PATCH_TEXT);

    const { broken, restored } = runMutation(mutated);
    console.log(`  > M1 改坏后 exit=${broken.status} / 改回后 exit=${restored.status}`);
    ok('M1：回退到旧实现后判据必须红', broken.status !== 0, `exit=${broken.status}`);
    ok('M1：改回之后判据必须绿', restored.status === 0, `exit=${restored.status}`);
    showBroken('M1', broken);
  }

  // =========================================================================
  // M2a：congested 恒假 -- A/B/C 三态分不开（判据的灵魂）
  // =========================================================================
  section('M2a 变异（congested 恒假）');
  {
    const re = /(const int congested = )net_stress > 0 &&/;
    const matched = re.test(PATCH_TEXT);
    ok('M2a：能定位到 congested 判据（变异真的生效了）', matched,
      matched ? '' : '没匹配到，变异没生效，这个变异测试是空的');
    const mutated = matched
      ? PATCH_TEXT.replace(re, '$1' + '0 &&')
      : PATCH_TEXT;
    ok('M2a：补丁副本确实被改动了（变异不是空操作）', mutated !== PATCH_TEXT);
    ok('M2a：改坏后的 congested 确实恒假', matched &&
      /const int congested = 0 &&/.test(mutated));

    const { broken, restored } = runMutation(mutated);
    console.log(`  > M2a 改坏后 exit=${broken.status} / 改回后 exit=${restored.status}`);
    ok('M2a：congested 恒假后判据必须红', broken.status !== 0, `exit=${broken.status}`);
    ok('M2a：改回之后判据必须绿', restored.status === 0, `exit=${restored.status}`);
    showBroken('M2a', broken);
  }

  // =========================================================================
  // M2b：进报文的实参换成 measured_bps（声明值算对了，却报错了东西）
  // =========================================================================
  section('M2b 变异（报文实参换成 measured_bps）');
  {
    const re = /(pc->remote_vssrc, )\(uint32_t\)bitrate\)/;
    const matched = re.test(PATCH_TEXT);
    ok('M2b：能定位到 rtcp_get_remb 的位率实参（变异真的生效了）', matched,
      matched ? '' : '没匹配到，变异没生效，这个变异测试是空的');
    const mutated = matched
      ? PATCH_TEXT.replace(re, '$1(uint32_t)measured_bps)')
      : PATCH_TEXT;
    ok('M2b：补丁副本确实被改动了（变异不是空操作）', mutated !== PATCH_TEXT);
    ok('M2b：改坏后进报文的确实是裸实测值', matched &&
      /pc->remote_vssrc, \(uint32_t\)measured_bps\)/.test(mutated));

    const { broken, restored } = runMutation(mutated);
    console.log(`  > M2b 改坏后 exit=${broken.status} / 改回后 exit=${restored.status}`);
    ok('M2b：报文实参换成实测值后判据必须红', broken.status !== 0, `exit=${broken.status}`);
    ok('M2b：改回之后判据必须绿', restored.status === 0, `exit=${restored.status}`);
    showBroken('M2b', broken);
  }

  // =========================================================================
  // M3：T1 拆分拆掉 -- net_stress 合并回五路（self_stress 又能触发降）
  // =========================================================================
  section('M3 变异（T1 拆分拆掉：net_stress 合并回五路）');
  {
    const re = /(const uint32_t net_stress = gap_ev \+ late_ev \+ decfail)_ev;/;
    const matched = re.test(PATCH_TEXT);
    ok('M3：能定位到 net_stress 拆分（变异真的生效了）', matched,
      matched ? '' : '没匹配到，变异没生效，这个变异测试是空的');
    const mutated = matched
      ? PATCH_TEXT.replace(re, '$1_ev + forced_ev + audrop_ev;')
      : PATCH_TEXT;
    ok('M3：补丁副本确实被改动了（变异不是空操作）', mutated !== PATCH_TEXT);
    ok('M3：改坏后 net_stress 又包含 forced/audrop', matched &&
      /const uint32_t net_stress = gap_ev \+ late_ev \+ decfail_ev \+ forced_ev \+ audrop_ev;/.test(mutated));

    const { broken, restored } = runMutation(mutated);
    console.log(`  > M3 改坏后 exit=${broken.status} / 改回后 exit=${restored.status}`);
    ok('M3：拆分拆掉后判据必须红（self_stress 场景被误降）', broken.status !== 0, `exit=${broken.status}`);
    ok('M3：改回之后判据必须绿', restored.status === 0, `exit=${restored.status}`);
    showBroken('M3', broken);
  }

  // =========================================================================
  // M4：T3 暖机拆除 -- back_off 不再看 warmup（暖机期也会降）
  // =========================================================================
  section('M4 变异（T3 暖机拆除）');
  {
    const re = /(const int back_off = congested) && !warmup;/;
    const matched = re.test(PATCH_TEXT);
    ok('M4：能定位到 back_off 的暖机门（变异真的生效了）', matched,
      matched ? '' : '没匹配到，变异没生效，这个变异测试是空的');
    const mutated = matched
      ? PATCH_TEXT.replace(re, '$1;')
      : PATCH_TEXT;
    ok('M4：补丁副本确实被改动了（变异不是空操作）', mutated !== PATCH_TEXT);
    ok('M4：改坏后 back_off 不再看暖机', matched &&
      /const int back_off = congested;/.test(mutated) &&
      !/const int back_off = congested && !warmup;/.test(mutated));

    const { broken, restored } = runMutation(mutated);
    console.log(`  > M4 改坏后 exit=${broken.status} / 改回后 exit=${restored.status}`);
    ok('M4：暖机拆除后判据必须红（暖机序列被降）', broken.status !== 0, `exit=${broken.status}`);
    ok('M4：改回之后判据必须绿', restored.status === 0, `exit=${restored.status}`);
    showBroken('M4', broken);
  }

  // =========================================================================
  // M5：T2 上限改回 20M（审稿前的旧上限）
  // =========================================================================
  section('M5 变异（CEIL 改回 20M）');
  {
    const re = /(#define PEER_REMB_CEIL_BPS )8000000u/;
    const matched = re.test(PATCH_TEXT);
    ok('M5：能定位到 CEIL 常量（变异真的生效了）', matched,
      matched ? '' : '没匹配到，变异没生效，这个变异测试是空的');
    const mutated = matched
      ? PATCH_TEXT.replace(re, '$120000000u')
      : PATCH_TEXT;
    ok('M5：补丁副本确实被改动了（变异不是空操作）', mutated !== PATCH_TEXT);
    ok('M5：改坏后 CEIL 回到 20000000', matched &&
      /#define PEER_REMB_CEIL_BPS 20000000u/.test(mutated));

    const { broken, restored } = runMutation(mutated);
    console.log(`  > M5 改坏后 exit=${broken.status} / 改回后 exit=${restored.status}`);
    ok('M5：CEIL 改回 20M 后判据必须红', broken.status !== 0, `exit=${broken.status}`);
    ok('M5：改回之后判据必须绿', restored.status === 0, `exit=${restored.status}`);
    showBroken('M5', broken);
  }

  // =========================================================================
  // M6：T2 已否决的 min 钳制被加回（在 ceil 夹取后插入
  //     declared = min(declared, max(measured, FLOOR)) 的等价形态）
  // =========================================================================
  section('M6 变异（被否决的 min 钳制加回）');
  {
    const anchor = '+  if (declared > PEER_REMB_CEIL_BPS) {\n'
      + '+    declared = PEER_REMB_CEIL_BPS;\n'
      + '+  }\n';
    const extraClamp = '+  if (declared > PEER_REMB_FLOOR_BPS && measured_bps < declared) {\n'
      + '+    declared = measured_bps < PEER_REMB_FLOOR_BPS ? PEER_REMB_FLOOR_BPS : measured_bps;\n'
      + '+  }\n';
    const matched = PATCH_TEXT.includes(anchor);
    ok('M6：能定位到 ceil 夹取（变异真的生效了）', matched,
      matched ? '' : '没匹配到，变异没生效，这个变异测试是空的');
    const mutated = matched
      ? PATCH_TEXT.replace(anchor, anchor + extraClamp)
      : PATCH_TEXT;
    ok('M6：补丁副本确实被改动了（变异不是空操作）', mutated !== PATCH_TEXT);
    ok('M6：改坏后多出第三个夹取', matched &&
      /declared = measured_bps < PEER_REMB_FLOOR_BPS \? PEER_REMB_FLOOR_BPS : measured_bps;/.test(mutated));

    const { broken, restored } = runMutation(mutated);
    console.log(`  > M6 改坏后 exit=${broken.status} / 改回后 exit=${restored.status}`);
    ok('M6：min 钳制加回后判据必须红（夹取数与低实测爬升）', broken.status !== 0, `exit=${broken.status}`);
    ok('M6：改回之后判据必须绿', restored.status === 0, `exit=${restored.status}`);
    showBroken('M6', broken);
  }

  // =========================================================================
  // 收尾：清掉 %TEMP% 的副本
  // =========================================================================
  try { rmSync(TMP_DIR, { recursive: true, force: true }); } catch { /* 忽略 */ }
}

console.log(`\n=== 结果：${passed} 通过 / ${failed} 失败 ===`);
if (failed) {
  console.log('失败项：');
  for (const f of failures) console.log('  - ' + f);
  process.exit(1);
}

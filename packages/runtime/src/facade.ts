/**
 * facade 落地点（patches/0009-facade-injection.patch）。
 *
 * ## 为什么落在这里
 *
 * 引擎的 runtime bundle（`packages/runtime/runtime.js`）是**所有宿主共用的那一份**：
 * 独立 NRO、slim NRO、Brewser（壳）跑的都是它。
 * 而它在**任何页面脚本之前**求值：
 *
 * ```
 * engine/source/main.cc:2264   run_script(runtime.js)            ← facade 在这里被装上
 * engine/source/main.cc:2294   nx_run_entry_module(user_code)    ← 页面脚本在这之后
 * ```
 *
 * → 把 facade 挂在这里 = `installFacade()` **必然**早于米哈游 SDK（`cg-sdk.js`）加载
 *   → SDK 的 `CompatibilityChecker` 探测得到 `RTCPeerConnection` → 门禁能过。
 *
 * ⚠️ 反过来必坏：SDK 先加载、再装 facade → `CompatibilityChecker` 判定「浏览器不支持」
 *   → 直接黑屏，连登录页都出不来（docs/08 §4.5 S2）。
 *
 * ## facade 本体在哪
 *
 * 实现体在**本仓库**的 `facade/`（不在引擎里 —— 引擎只以 patch 形式改，见
 * `patches/README.md` 的铁律）：
 *
 * - `scripts/windows/build-engine.ps1` 先在 `facade/` 跑 `npm run build`
 *   → 产出 `facade/dist/`（tsc，ESM 多文件）；
 * - 再把 `facade/dist/` 整目录拷到 `packages/runtime/facade-dist/`
 *   （**构建产物**，已由本 patch 加进 `.gitignore`，不入库）；
 * - 这里只做一次**副作用导入**，把入口拉进 runtime bundle。
 *
 * 入口文件 `facade-dist/engine-entry.js` 在模块加载时就调 `installEngineFacade()`
 * （与 patch 0003 的 `native-webrtc.ts` 同一形状）。
 *
 * ## 与 native 半边的契约
 *
 * | 契约 | 对方 | 形状 |
 * |---|---|---|
 * | `globalThis.__nx_webrtc_peer_factory` | patch 0003 的 `native-webrtc.ts` | `(config) => NativePeer` |
 * | `globalThis.Switch` | 引擎 runtime（`packages/runtime/src/switch.ts`） | fs（`appendFileSync` / `writeFileSync` / …） |
 *
 * 两个契约都只按 **globalThis 上的字符串键**约定，facade 与引擎之间
 * **没有编译期依赖**（docs/21 §3.6）。
 */

// ⚠️ 这里**不能**用副作用导入（`import '../facade-dist/engine-entry.js';`）——
//
// 引擎 `packages/runtime/src/index.ts` 里的 `import './facade'` 是**静态 import**，
// ESM 规范规定静态 import 会被**提升到整个模块体之前**求值 → 本文件与
// `facade-dist/engine-entry.js` 的顶层代码会在 `index.ts` 的
// `def(setTimeout, 'setTimeout')` / `def(Switch, 'Switch')` **之前**执行
// → 那时 `globalThis.setTimeout` 还不存在 → 引擎启动即抛
// `ReferenceError: setTimeout is not defined`（真机实测，2026-10-21）。
//
// → 改成 re-export，由 `index.ts` 的**模块体末尾**显式调用 `installEngineFacade()`。
//   时序仍满足「facade 早于任何页面脚本」（docs/08 §4.5 S2）。
export { installEngineFacade } from '../facade-dist/engine-entry.js';

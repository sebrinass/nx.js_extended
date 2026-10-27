import parseCssFont, { type IFont } from 'parse-css-font';
import { $ } from '../$';
import { readFileSync } from '../fs';
import { INTERNAL_SYMBOL } from '../internal';
import { EventTarget } from '../polyfills/event-target';
import type { screen } from '../screen';
import type { FontFaceSetLoadStatus } from '../types';
import { assertInternalConstructor, def } from '../utils';
import { FontFace, isFontFaceUsable, nativeFontFace } from './font-face';

/**
 * Manages the loading of font-faces and querying of their download status.
 *
 * @see https://developer.mozilla.org/docs/Web/API/FontFaceSet
 */
export class FontFaceSet extends EventTarget {
	#set = new Set<FontFace>();

	/**
	 * @ignore
	 */
	constructor() {
		assertInternalConstructor(arguments);
		super();
		this.ready = Promise.resolve(this);
		this.status = 'loaded';
	}

	onloading: ((this: FontFaceSet, ev: Event) => any) | null = null;
	onloadingdone: ((this: FontFaceSet, ev: Event) => any) | null = null;
	onloadingerror: ((this: FontFaceSet, ev: Event) => any) | null = null;
	ready: Promise<this>;
	status: FontFaceSetLoadStatus;
	/**
	 * Whether every font needed to render `text` in `font` is available.
	 *
	 * Every face in this set is fully decoded at `add()` time (there is no
	 * pending/loading state on this platform), so "available" reduces to
	 * "a face with a matching family is registered" — plus `system-ui` /
	 * `sans-serif`, which `addSystemFont()` materialises from the Switch's
	 * shared font on demand and therefore always resolve.
	 *
	 * Matching is by family name only, case-insensitively. `findFont()`
	 * additionally requires an exact weight/style/stretch match because it
	 * has to pick ONE face to rasterise with; `check()` is asking a broader
	 * question ("would text in this family render?"), and browsers answer
	 * yes there by synthesising the missing variant.
	 *
	 * Deviation from spec: an unparseable `font` shorthand returns false
	 * rather than throwing a SyntaxError. Callers reach `check()` through
	 * feature-detection paths where a throw is far more damaging than a
	 * conservative false.
	 */
	check(font: string, _text?: string | undefined): boolean {
		const families = familiesOf(font);
		if (families.length === 0) return false;
		for (const family of families) {
			if (isFamilyAvailable(this, family)) return true;
		}
		return false;
	}

	/**
	 * Resolve the faces matching `font`. Nothing is fetched here: a
	 * {@link FontFace} owns decoded bytes from the moment it is constructed,
	 * so by the time a face is in this set it is loaded. Resolves with the
	 * matching faces (empty when none match) and never rejects — same
	 * reasoning as `check()` above.
	 */
	load(font: string, _text?: string | undefined): Promise<FontFace[]> {
		const families = familiesOf(font);
		const out: FontFace[] = [];
		for (const family of families) {
			const lower = family.toLowerCase();
			for (const face of this.#set) {
				if (
					face.family.toLowerCase() === lower &&
					isFontFaceUsable(face) &&
					!out.includes(face)
				) {
					out.push(face);
				}
			}
		}
		return Promise.resolve(out);
	}

	// Set interface
	get size() {
		return this.#set.size;
	}
	add(font: FontFace) {
		this.#set.add(font);
		return this;
	}
	clear(): void {
		this.#set.clear();
	}
	delete(font: FontFace): boolean {
		return this.#set.delete(font);
	}
	has(font: FontFace): boolean {
		return this.#set.has(font);
	}
	keys(): IterableIterator<FontFace> {
		return this.#set.keys();
	}
	values(): IterableIterator<FontFace> {
		return this.#set.values();
	}
	entries(): IterableIterator<[FontFace, FontFace]> {
		return this.#set.entries();
	}
	forEach(
		callbackfn: (value: FontFace, key: FontFace, parent: FontFaceSet) => void,
		thisArg: any = this,
	): void {
		for (const font of this.#set) {
			callbackfn.call(thisArg, font, font, this);
		}
	}
	[Symbol.iterator](): IterableIterator<FontFace> {
		return this.#set[Symbol.iterator]();
	}
}
/** Families the runtime can always produce, whether or not a face for them
 * is in the set yet — `addSystemFont()` registers both from the Switch's
 * shared font the first time an unmatched family is requested. */
const ALWAYS_AVAILABLE_FAMILIES = new Set(['system-ui', 'sans-serif']);

/** Family list from a CSS `font` shorthand, or from a bare family name.
 * Returns [] when the input can't be understood. */
function familiesOf(font: string): string[] {
	if (typeof font !== 'string' || font.trim() === '') return [];
	try {
		const parsed = parseCssFont(font);
		const family = (parsed as IFont).family;
		if (family && family.length > 0) return family;
	} catch {
		// Not a full shorthand. Callers (Cocos's loader, for one) pass a bare
		// family name to load()/check(), which parse-css-font rejects since
		// the shorthand requires a size. Fall through and treat the input as
		// a comma-separated family list.
	}
	return font
		.split(',')
		.map((f) => f.trim().replace(/^['"]|['"]$/g, ''))
		.filter((f) => f.length > 0);
}

function isFamilyAvailable(set: FontFaceSet, family: string): boolean {
	const lower = family.toLowerCase();
	if (ALWAYS_AVAILABLE_FAMILIES.has(lower)) return true;
	for (const face of set) {
		// A url()-sourced face that has not finished `load()` yet has no
		// glyphs, so reporting it as available would make a page skip its
		// own wait and render in the fallback.
		if (face.family.toLowerCase() === lower && isFontFaceUsable(face)) {
			return true;
		}
	}
	return false;
}

def(FontFaceSet);

/**
 * Contains the available fonts for use on the {@link screen | `screen`} Canvas context.
 *
 * There are two built-in fonts available:
 *
 *  - `"system-ui"` is the system font provided by the Switch operating system.
 *  - `"system-icons"` contains the icons used by the Switch operating system.
 *
 * Custom fonts can be added to the set using the {@link FontFaceSet.add | `add()`} method.
 *
 * @see https://nxjs.n8.io/runtime/concepts/fonts
 */
// @ts-expect-error Internal constructor
export var fonts = new FontFaceSet(INTERNAL_SYMBOL);
def(fonts, 'fonts');

export function findFont(
	fontFaceSet: FontFaceSet,
	desired: IFont,
): FontFace | null {
	if (!desired.family) {
		throw new Error('No `font-family` was specified');
	}
	// ── 阶段 1：四个描述符全等（原有行为，优先级不变） ──────────────────
	for (const family of desired.family) {
		for (const fontFace of fontFaceSet) {
			if (
				family === fontFace.family &&
				desired.stretch === fontFace.stretch &&
				desired.style === fontFace.style &&
				desired.weight === fontFace.weight
			) {
				// url()-sourced faces are a JS shell around a buffer-backed
				// FontFace; native code can only unwrap the latter. `null`
				// means "still loading" — keep looking.
				const native = nativeFontFace(fontFace);
				if (native) return native;
			}
		}
	}
	// ── 阶段 2：CSS Fonts 4 §5.2 的字体匹配 ────────────────────────────────
	// 阶段 1 对真实页面太严：`@font-face` 几乎总是带**字重区间**声明
	// （`font-weight: 100 900`），而请求过来的是一个具体字重（`600`、
	// `bold`）。要求字符串全等就永远匹配不上，调用方只能回退到系统字体 ——
	// Switch 上没有 CJK 字形 → 整页豆腐块。
	//
	// 这里照 CSS Fonts 4 §5.2 “Matching font styles” 做：
	//   ① font-width（stretch）先筛  ② font-style 再筛  ③ font-weight 最后筛
	// 每一筛都是「先看有没有**覆盖**目标值的候选，有就只留这些；没有就按规范
	// 给出的**方向性**次序挑一个」。
	//
	// 家族列表仍然按顺序走，**第一个有可用脸的家族获胜** —— 引擎不做逐字回退
	// （见 §5.4 Character handling issues），所以一个能解析出来的家族会挡住
	// 它后面的家族，这跟 findFont 一直以来的行为一致。
	const wantStretch = desired.stretch || 'normal';
	const wantStyle = desired.style || 'normal';
	const wantWeight = desired.weight || 'normal';
	for (const family of desired.family) {
		const candidates: FontFace[] = [];
		for (const fontFace of fontFaceSet) {
			if (family !== fontFace.family) continue;
			// 还没 load() 完的 url()-sourced face 没有 native 脸，跳过。
			if (!nativeFontFace(fontFace)) continue;
			candidates.push(fontFace);
		}
		if (candidates.length === 0) continue;
		const pool = narrowByWeight(
			narrowByStyle(narrowByStretch(candidates, wantStretch), wantStyle),
			wantWeight,
		);
		if (pool.length > 0) {
			const native = nativeFontFace(pool[0]);
			if (native) return native;
		}
	}
	return null;
}

/** `font-stretch` 关键字 → 百分比（CSS Fonts 4 §7.1）。 */
const STRETCH_PERCENT: Record<string, number> = {
	'ultra-condensed': 50,
	'extra-condensed': 62.5,
	condensed: 75,
	'semi-condensed': 87.5,
	normal: 100,
	'semi-expanded': 112.5,
	expanded: 125,
	'extra-expanded': 150,
	'ultra-expanded': 200,
};

function stretchPercent(stretch: string): number {
	return STRETCH_PERCENT[stretch] ?? 100;
}

/**
 * CSS Fonts 4 §5.2：font-width 先筛。
 *
 * 「若候选里有覆盖目标宽度的，就只留这些；否则按方向性次序挑一个」——
 * 目标 ≤100%：先看小于目标（降序），再看大于目标（升序）；
 * 目标 >100%：先看大于目标（升序），再看小于目标（降序）。
 */
function narrowByStretch(faces: FontFace[], want: string): FontFace[] {
	const target = stretchPercent(want);
	const exact = faces.filter((f) => stretchPercent(f.stretch) === target);
	if (exact.length > 0) return exact;
	const below = faces
		.filter((f) => stretchPercent(f.stretch) < target)
		.sort((a, b) => stretchPercent(b.stretch) - stretchPercent(a.stretch));
	const above = faces
		.filter((f) => stretchPercent(f.stretch) > target)
		.sort((a, b) => stretchPercent(a.stretch) - stretchPercent(b.stretch));
	const order = target <= 100 ? [...below, ...above] : [...above, ...below];
	return order.length > 0 ? [order[0]] : faces;
}

/**
 * CSS Fonts 4 §5.2：font-style 再筛。
 *
 * 引擎里的 style 只有 normal / italic / oblique 三种，没有角度值，按规范
 * 「italic=1 与 oblique 11deg 等价」折算成一条轴：
 * normal=0、oblique=0.5、italic=1。请求 italic 时规范优先 oblique 再 normal；
 * 请求 oblique 时规范先看比目标小的角度（直到 0），所以 normal 优先于 italic。
 */
function narrowByStyle(faces: FontFace[], want: string): FontFace[] {
	const exact = faces.filter((f) => f.style === want);
	if (exact.length > 0) return exact;
	const order: Record<string, string[]> = {
		normal: ['oblique', 'italic'],
		italic: ['oblique', 'normal'],
		oblique: ['normal', 'italic'],
	};
	for (const style of order[want] ?? order.normal) {
		const hit = faces.filter((f) => f.style === style);
		if (hit.length > 0) return hit;
	}
	return faces;
}

/**
 * CSS Fonts 4 §5.2：font-weight 最后筛。
 *
 * 「若候选里有字重区间覆盖目标值的，就只留这些；否则按方向性次序挑一个」：
 *   · 目标 < 400：≤目标（降序）→ >目标（升序）
 *   · 目标 ∈ [400,500]：≥目标（升序，到 500 为止）→ <目标（降序）→ >500
 *   · 目标 > 500：≥目标（升序）→ <目标（降序）
 */
function narrowByWeight(faces: FontFace[], want: string): FontFace[] {
	const target = weightValue(want);
	const covering = faces.filter((f) => {
		const span = weightSpan(f.weight);
		return target >= span.min && target <= span.max;
	});
	if (covering.length > 0) return covering;
	const order: number[] = [];
	if (target < 400) {
		for (let w = target; w >= 1; w--) order.push(w);
		for (let w = target + 1; w <= 900; w++) order.push(w);
	} else if (target <= 500) {
		for (let w = target; w <= 500; w++) order.push(w);
		for (let w = target - 1; w >= 1; w--) order.push(w);
		for (let w = 501; w <= 900; w++) order.push(w);
	} else {
		for (let w = target; w <= 900; w++) order.push(w);
		for (let w = target - 1; w >= 1; w--) order.push(w);
	}
	for (const w of order) {
		const hit = faces.filter((f) => {
			const span = weightSpan(f.weight);
			return w >= span.min && w <= span.max;
		});
		if (hit.length > 0) return hit;
	}
	return faces;
}

/**
 * CSS `font-weight` 的数值。`normal` → 400，`bold` → 700，
 * `100`…`900` → 本身。
 *
 * 相对关键字 `bolder` / `lighter` 依赖继承来的字重，规范要求按继承值算
 * （CSS Fonts 4 §7.1）。画布的 font 简写里没有继承链，引擎也不追踪它，
 * 因此这里按**继承值 = 400（normal）** 近似，用的表直接抄 Blink 的
 * `FontDescription::BolderWeight` / `FontDescription::LighterWeight`：
 *   lighter：<100 不变；<550 → 100；<750 → 400；否则 700
 *   bolder ：<350 → 400；<550 → 700；<900 → 900；否则不变
 */
function weightValue(weight: string): number {
	switch (weight) {
		case 'bold':
			return 700;
		case 'normal':
			return 400;
		case 'bolder':
			return bolderWeight(400);
		case 'lighter':
			return lighterWeight(400);
		default: {
			const n = parseInt(weight, 10);
			return Number.isFinite(n) ? n : 400;
		}
	}
}

/** Blink `FontDescription::LighterWeight`（继承值取 400）。 */
function lighterWeight(weight: number): number {
	if (weight < 100) return weight;
	if (weight < 550) return 100;
	if (weight < 750) return 400;
	return 700;
}

/** Blink `FontDescription::BolderWeight`（继承值取 400）。 */
function bolderWeight(weight: number): number {
	if (weight < 350) return 400;
	if (weight < 550) return 700;
	if (weight < 900) return 900;
	return weight;
}

/** 一张脸覆盖的字重区间。描述符可以是单值（`400`）也可以是区间
 * （`100 900`）；区间覆盖区间内的所有字重。 */
function weightSpan(weight: string): { min: number; max: number } {
	const parts = String(weight).trim().split(/\s+/).filter(Boolean);
	if (parts.length >= 2) {
		const a = weightValue(parts[0]);
		const b = weightValue(parts[1]);
		return { min: Math.min(a, b), max: Math.max(a, b) };
	}
	const v = weightValue(parts[0] ?? 'normal');
	return { min: v, max: v };
}

export function addSystemFont(fonts: FontFaceSet): FontFace {
	// Idempotent: the generic-family fallback in the 2D `font` setter now
	// calls this on every unmatched font-set (weight variants included, since
	// findFont requires an exact weight match), so re-copying the multi-MB
	// shared font and appending duplicate FontFaces each time would bloat the
	// set. Reuse the already-registered system-ui face if present.
	for (const f of fonts) {
		if (f.family === 'system-ui') return f;
	}
	// Prefer the bundled CJK font over the Switch's shared font: the latter
	// has no CJK glyphs, so using it as the system font leaves every Chinese
	// glyph as a tofu box.
	const bundled = addBundledSystemFont(fonts);
	if (bundled) return bundled;
	const data = $.getSystemFont(0 /* PlSharedFontType_Standard */);
	const f = new FontFace('system-ui', data);
	fonts.add(f);
	fonts.add(new FontFace('sans-serif', data));
	return f;
}

export function addIconFont(fonts: FontFaceSet): FontFace {
	const data = $.getSystemFont(5 /* PlSharedFontType_NintendoExt */);
	const f = new FontFace('system-icons', data);
	fonts.add(f);
	return f;
}

/**
 * The CJK-capable font bundled in the NRO's own RomFS (mounted as `nxjs:` at
 * runtime; the engine's Makefile copies it in at build time).
 *
 * The Switch's shared system font (`PlSharedFontType_Standard`) is a pure
 * Latin face — it carries no CJK glyphs at all — so every page that leans on
 * `system-ui` / `sans-serif` (the Brewser shell's own directory page
 * included) renders Chinese as tofu boxes. The engine therefore ships its own
 * subset of Noto Sans SC (SIL OFL 1.1) and registers it as the system font.
 */
const BUNDLED_SYSTEM_FONT_PATH = 'nxjs:/NotoSansSC-subset.ttf';

/**
 * The family the bundled font is registered under. `sans-serif` is NOT
 * registered separately: `addSystemFont()` hands the `system-ui` face to
 * every generic-family fallback, so a `sans-serif` request lands on the very
 * same face — a second registration would only duplicate the font's bytes
 * in the heap.
 */
const BUNDLED_SYSTEM_FONT_FAMILY = 'system-ui';

/**
 * Register the bundled CJK font as the system font.
 *
 * Returns `null` when the RomFS does not carry the font (the host test
 * binary has no `nxjs:` mount) or when the bytes cannot be decoded — the
 * caller then falls back to the Switch's own shared font.
 */
function addBundledSystemFont(fonts: FontFaceSet): FontFace | null {
	let data: ArrayBuffer | null = null;
	try {
		data = readFileSync(BUNDLED_SYSTEM_FONT_PATH);
	} catch {
		return null;
	}
	if (!data) return null;
	let face: FontFace;
	try {
		face = new FontFace(BUNDLED_SYSTEM_FONT_FAMILY, data);
	} catch {
		return null;
	}
	fonts.add(face);
	// ONE native face only. The engine picks ONE face per text run (no
	// per-glyph fallback), and `sans-serif` requests fall back to the
	// `system-ui` face through addSystemFont() anyway — so a second copy of
	// the same bytes under `sans-serif` would only double the font's heap
	// footprint (the bytes are copied into the native face). On a memory-
	// constrained applet that extra megabyte is not free: the video
	// pipeline's 720p frame buffer allocation sits right at the edge.
	return face;
}

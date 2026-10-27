/**
 * Mapping of the button indices on a `Gamepad` to the
 * corresponding name of the button on the controller.
 *
 * The order is the **standard Web Gamepad API order** — `A=0, B=1, X=2, Y=3`,
 * then the shoulder buttons, the triggers, `View`/`Menu`, the stick clicks and
 * the D-Pad. Always use this enum (or named members) to look up buttons in
 * `Gamepad.buttons[]` rather than hard-coding indices.
 *
 * > [!NOTE]
 * > This used to follow the Switch's HID service order (`B=0, A=1, Y=2, X=3`)
 * > instead of the standard mapping. Only the four face buttons were
 * > affected — `L`, `R`, `ZL`, `ZR`, `Minus`, `Plus`, `StickL`, `StickR` and
 * > the D-Pad keep their positions.
 *
 * @example
 *
 * ```typescript
 * import { Button } from '@nx.js/constants';
 *
 * const pad = navigator.getGamepads()[0];
 * if (pad.buttons[Button.A].pressed) {
 *   console.log('Button A is pressed');
 * }
 * ```
 */
export enum Button {
	/** A button (right-side face button — _right_ position). */
	A,
	/** B button (right-side face button on Pro/Joy-Con grip — _down_ position). */
	B,
	/** X button (left-side face button — _up_ position). */
	X,
	/** Y button (left-side face button — _left_ position). */
	Y,
	/** L shoulder button. */
	L,
	/** R shoulder button. */
	R,
	/** ZL trigger. */
	ZL,
	/** ZR trigger. */
	ZR,
	/** Minus (–) button. */
	Minus,
	/** Plus (+) button. */
	Plus,
	/** Click of the Left Stick. */
	StickL,
	/** Click of the Right Stick. */
	StickR,
	/** D-Pad Up. */
	Up,
	/** D-Pad Down. */
	Down,
	/** D-Pad Left. */
	Left,
	/** D-Pad Right. */
	Right,
}

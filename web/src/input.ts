/**
 * Keyboard and mouse input for the engine: DOM events → Windows virtual keys / DirectInput scan
 * codes (keys.ts) and window-relative mouse messages, with pointer lock for the game's own cursor.
 */
import { KEYS } from "./keys.ts";

export interface InputEngine {
  _web_key(kind: number, vk: number, dik: number, ch: number): void;
  _web_mouse_move(x: number, y: number, dx: number, dy: number): void;
  _web_mouse_button(button: number, down: number, x: number, y: number): void;
  _web_mouse_wheel(dy: number): void;
  _web_focus(focused: number): void;
  /** The game's client area as width << 16 | height (its chosen resolution). */
  _web_client_size(): number;
}

export interface InputOptions {
  /** KeyboardEvent.code that also acts as Escape (browsers reserve Esc in fullscreen/pointer lock). */
  pauseAlias: string;
  /** Lock the pointer on click: the game hides the system cursor and moves its own from raw DirectInput motion. */
  mouseCapture: boolean;
  /** F1: show or hide the controls list. */
  onHelp?: () => void;
}

export class Input {
  options: InputOptions;
  private readonly engine: InputEngine;
  private readonly canvas: HTMLCanvasElement;
  /** Buttons whose press only captured the pointer (their release is not sent either). */
  private readonly swallowed = new Set<number>();
  /** While suspended (an overlay is open) the game gets no input and is told it lost focus. */
  private suspended = false;
  /** Last pointer position in client space; integrated from motion while the pointer is locked. */
  private locked: [number, number] = [400, 300];
  /** Game pixels per CSS pixel (x, y), and the motion's unsent fractions. */
  private scale: [number, number] = [1, 1];
  private frac: [number, number] = [0, 0];

  constructor(engine: InputEngine, canvas: HTMLCanvasElement, options: InputOptions) {
    this.engine = engine;
    this.canvas = canvas;
    this.options = options;
    window.addEventListener("keydown", (e) => this.key(e, true));
    window.addEventListener("keyup", (e) => this.key(e, false));
    canvas.addEventListener("mousemove", (e) => {
      const [x, y] = this.pos(e);
      // The game moves its cursor one game pixel per count: scale the (OS-accelerated) motion so
      // it travels as far on screen as the system cursor would, at any resolution or zoom.
      const fx = this.frac[0] + e.movementX * this.scale[0], fy = this.frac[1] + e.movementY * this.scale[1];
      const dx = Math.trunc(fx), dy = Math.trunc(fy);
      this.frac = [fx - dx, fy - dy];
      engine._web_mouse_move(x, y, dx, dy);
    });
    canvas.addEventListener("mousedown", (e) => {
      if (this.suspended) return;
      // The click that captures the pointer is not a game click (mouse buttons may be bound to
      // actions such as Attack).
      if (this.options.mouseCapture && document.pointerLockElement !== canvas) {
        this.swallowed.add(e.button);
        this.lockPointer();
        return;
      }
      const [x, y] = this.pos(e);
      engine._web_mouse_button(button(e.button), 1, x, y);
    });
    canvas.addEventListener("mouseup", (e) => {
      if (this.swallowed.delete(e.button)) return;
      const [x, y] = this.pos(e);
      engine._web_mouse_button(button(e.button), 0, x, y);
    });
    canvas.addEventListener("wheel", (e) => engine._web_mouse_wheel(e.deltaY < 0 ? 1 : -1), { passive: true });
    canvas.addEventListener("contextmenu", (e) => e.preventDefault());
    window.addEventListener("focus", () => {
      if (!this.suspended) engine._web_focus(1);
    });
    window.addEventListener("blur", () => engine._web_focus(0));
  }

  /** Pause the game (as when the window loses focus, which also releases held keys) or resume it. */
  suspend(on: boolean): void {
    if (on === this.suspended) return;
    this.suspended = on;
    this.engine._web_focus(on ? 0 : 1);
  }

  private key(e: KeyboardEvent, down: boolean): void {
    if (e.code === "F1") {
      e.preventDefault();
      if (down && !e.repeat) this.options.onHelp?.();
      return;
    }
    if (this.suspended) return;
    const code = e.code === this.options.pauseAlias ? "Escape" : e.code;
    const key = KEYS[code];
    if (!key) return;
    // Let the browser keep its own shortcuts for keys the game never uses (e.g. F5, F11, F12).
    e.preventDefault();
    if (down && e.repeat) return;
    this.engine._web_key(down ? 0 : 1, key[0], key[1], 0);
    if (down && e.key.length === 1 && code === e.code) this.engine._web_key(2, 0, 0, e.key.charCodeAt(0));
  }

  /** Window-relative coordinates in the game's client space (its resolution), inside the letterbox. */
  private pos(e: MouseEvent): [number, number] {
    const packed = this.engine._web_client_size();
    const cw = packed >> 16 || 800, ch = packed & 0xffff || 600;
    const r = this.canvas.getBoundingClientRect();
    const iw = Math.min(r.width, (r.height * cw) / ch), ih = Math.min(r.height, (r.width * ch) / cw);
    const ix = r.left + (r.width - iw) / 2, iy = r.top + (r.height - ih) / 2;
    this.scale = [cw / iw, ch / ih];
    // Under pointer lock clientX/Y stay frozen: integrate the motion, clamped to the window, so
    // the game's re-capture after its cursor leaves an edge lands where the motion went.
    if (this.pointerLocked) {
      this.locked[0] = Math.min(cw - 1, Math.max(0, this.locked[0] + (e.movementX / iw) * cw));
      this.locked[1] = Math.min(ch - 1, Math.max(0, this.locked[1] + (e.movementY / ih) * ch));
    } else {
      this.locked = [((e.clientX - ix) / iw) * cw, ((e.clientY - iy) / ih) * ch];
    }
    return [Math.round(this.locked[0]), Math.round(this.locked[1])];
  }

  get pointerLocked(): boolean {
    return document.pointerLockElement === this.canvas;
  }

  lockPointer(): void {
    try {
      // Keep the system's pointer acceleration (no unadjustedMovement): the cursor feels like the desktop's.
      const p = this.canvas.requestPointerLock() as Promise<void> | void;
      if (p && typeof p.catch === "function") p.catch(() => {});
    } catch {
      // Not supported: the unlocked pointer still drives the game.
    }
  }
}

function button(b: number): number {
  return b === 0 ? 0 : b === 2 ? 1 : 2;
}

/**
 * Game settings. Bella Sara reads its options from an XML file in the install, OPFS
 * `bella/Bella Sara/settings.dat` (the game creates it on first run and rewrites it at start-up,
 * preserving values). This module changes only the values the page offers (resolution and the
 * ADV_* graphics options) and leaves every other element and the file's line endings alone.
 */
import { INSTALL_DIR } from "./cache.ts";
import { KEYS } from "./keys.ts";
import { readOpfsFile, writeOpfsFile } from "./opfs-write.ts";

export const GAME_SUBDIR = "Bella Sara";
export const SETTINGS_PATH = [INSTALL_DIR, GAME_SUBDIR, "settings.dat"];

// ---- Resolution --------------------------------------------------------------------------------

export const AUTO_RESOLUTION = "auto";

export const RESOLUTIONS: { group: string; sizes: [number, number][] }[] = [
  { group: "4:3", sizes: [[800, 600], [1024, 768], [1280, 960], [1600, 1200]] },
  { group: "16:9", sizes: [[1280, 720], [1600, 900], [1920, 1080], [2560, 1440]] },
  { group: "16:10", sizes: [[1280, 800], [1680, 1050], [1920, 1200]] },
];

/** The screen in device pixels, rounded to even numbers and capped at 2560 wide (aspect kept). */
export function matchScreen(): [number, number] {
  const dpr = window.devicePixelRatio || 1;
  let w = window.screen.width * dpr;
  let h = window.screen.height * dpr;
  if (w > 2560) {
    h = (h * 2560) / w;
    w = 2560;
  }
  const even = (n: number) => Math.max(2, Math.round(n / 2) * 2);
  return [even(w), even(h)];
}

// ---- Quality -----------------------------------------------------------------------------------

export type Adv = Record<string, number>;

/** The ADV_* options in the file, with how the advanced panel shows them. */
export const ADV_OPTIONS: { key: string; label: string; choices?: string[] }[] = [
  { key: "ADV_TEXTUREFILTERMODE", label: "Texture filtering", choices: ["Basic", "Standard", "High"] },
  { key: "ADV_CULLINGDISTANCE", label: "Draw distance", choices: ["Near", "Medium", "Far"] },
  { key: "ADV_LOWRESTEXTURES", label: "Low-resolution textures" },
  { key: "ADV_SIMPLEWATER", label: "Simple water" },
  { key: "ADV_HORSESHADER", label: "Horse shader" },
  { key: "ADV_WORLDEFFECTS", label: "World effects" },
  { key: "ADV_FULLSCREENEFFECTS", label: "Fullscreen effects" },
  { key: "ADV_FULLSCREENBLOOM", label: "Bloom" },
  { key: "ADV_PLAYERSHADOWS", label: "Player shadows" },
  { key: "ADV_WORLDSHADOWS", label: "World shadows" },
  { key: "ADV_TAILLIGHTING", label: "Tail lighting" },
  { key: "ADV_TREEANIMATION", label: "Tree animation" },
  { key: "ADV_PLANTSCAPE", label: "Plants and grass" },
  { key: "ADV_CLONECROWD", label: "Crowds" },
  { key: "ADV_HORSEHOOFEFFECTS", label: "Hoof effects" },
];

/** Medium is the game's own default set. */
const MEDIUM: Adv = {
  ADV_TEXTUREFILTERMODE: 1, ADV_HORSESHADER: 1, ADV_SIMPLEWATER: 0, ADV_CULLINGDISTANCE: 1, ADV_WORLDEFFECTS: 1,
  ADV_FULLSCREENEFFECTS: 1, ADV_FULLSCREENBLOOM: 0, ADV_PLAYERSHADOWS: 1, ADV_WORLDSHADOWS: 1, ADV_TAILLIGHTING: 1,
  ADV_LOWRESTEXTURES: 0, ADV_TREEANIMATION: 1, ADV_PLANTSCAPE: 1, ADV_CLONECROWD: 1, ADV_HORSEHOOFEFFECTS: 1,
};
export const PRESETS: Record<string, Adv> = {
  Low: {
    ...MEDIUM, ADV_TEXTUREFILTERMODE: 0, ADV_CULLINGDISTANCE: 0, ADV_LOWRESTEXTURES: 1, ADV_SIMPLEWATER: 1,
    ADV_PLAYERSHADOWS: 0, ADV_WORLDSHADOWS: 0, ADV_PLANTSCAPE: 0, ADV_TREEANIMATION: 0, ADV_HORSEHOOFEFFECTS: 0,
  },
  Medium: MEDIUM,
  High: { ...MEDIUM, ADV_TEXTUREFILTERMODE: 2, ADV_CULLINGDISTANCE: 2, ADV_FULLSCREENBLOOM: 1 },
};
export const DEFAULT_QUALITY = "High";

/** The preset whose values all match, or undefined (custom). */
export function presetOf(adv: Adv): string | undefined {
  return Object.keys(PRESETS).find((p) => ADV_OPTIONS.every((o) => PRESETS[p][o.key] === adv[o.key]));
}

// ---- The XML file ------------------------------------------------------------------------------

export function defaultXml(width: number, height: number, adv: Adv): string {
  const lines = [
    "<settings>",
    '    <FirstTime value="1" />',
    "    <VideoMode>",
    `        <VM_WIDTH value="${width}" />`,
    `        <VM_HEIGHT value="${height}" />`,
    '        <VM_DEPTH value="32" />',
    '        <VM_REFRESH value="0" />',
    '        <VM_MULTISAMPLELEVEL value="1" />',
    '        <VM_WINDOW value="0" />',
    "    </VideoMode>",
    "    <GraphicsSettings>",
    ...ADV_OPTIONS.map((o) => `        <${o.key} value="${adv[o.key]}" />`),
    "    </GraphicsSettings>",
    "</settings>",
    "",
  ];
  return lines.join("\r\n");
}

export function getValue(xml: string, name: string): number | undefined {
  const m = new RegExp(`<${name}\\s+value\\s*=\\s*"(\\d+)"`).exec(xml);
  return m ? Number(m[1]) : undefined;
}

/** Set one value in place; a missing element is added at the end of its section. */
export function setValue(xml: string, name: string, value: number): string {
  const re = new RegExp(`(<${name}\\s+value\\s*=\\s*")[^"]*(")`);
  if (re.test(xml)) return xml.replace(re, (_, a: string, b: string) => `${a}${value}${b}`);
  const eol = xml.includes("\r\n") ? "\r\n" : "\n";
  const section = name.startsWith("VM_") ? "VideoMode" : "GraphicsSettings";
  const close = new RegExp(`([ \\t]*)</${section}>`);
  if (close.test(xml)) return xml.replace(close, (_, ind: string) => `${ind}    <${name} value="${value}" />${eol}${ind}</${section}>`);
  return xml.replace(/<\/settings>/, `    <${section}>${eol}        <${name} value="${value}" />${eol}    </${section}>${eol}</settings>`);
}

export interface Graphics {
  width: number;
  height: number;
  adv: Adv;
}

async function readXml(): Promise<string | null> {
  const data = await readOpfsFile(SETTINGS_PATH);
  return data ? new TextDecoder().decode(data) : null;
}

/** The file's graphics values, filling anything missing from the default (High) set. */
export async function loadGraphics(): Promise<Graphics> {
  const xml = await readXml();
  const [mw, mh] = matchScreen();
  const adv: Adv = {};
  const get = (name: string) => (xml === null ? undefined : getValue(xml, name));
  for (const o of ADV_OPTIONS) adv[o.key] = get(o.key) ?? PRESETS[DEFAULT_QUALITY][o.key];
  return { width: get("VM_WIDTH") || mw, height: get("VM_HEIGHT") || mh, adv };
}

/**
 * Write graphics choices into settings.dat: `resolution` is "auto" (match the screen now) or
 * "WxH"; `adv` replaces the ADV_* values. Omitted parts are left as the file has them. With no
 * file yet, the full default file is written (FirstTime 1).
 */
export async function applyGraphics(resolution?: string, adv?: Adv): Promise<void> {
  let xml = await readXml();
  const [w, h] = resolution === undefined ? [undefined, undefined] : resolution === AUTO_RESOLUTION ? matchScreen() : resolution.split("x").map(Number);
  if (xml === null) {
    const [mw, mh] = matchScreen();
    xml = defaultXml(w ?? mw, h ?? mh, adv ?? PRESETS[DEFAULT_QUALITY]);
  } else {
    if (w && h) xml = setValue(setValue(xml, "VM_WIDTH", w), "VM_HEIGHT", h);
    if (adv) for (const o of ADV_OPTIONS) xml = setValue(xml, o.key, adv[o.key]);
  }
  await writeOpfsFile(SETTINGS_PATH, new TextEncoder().encode(xml));
}

// ---- Key labels (for the browser's substitute-Esc key) ------------------------------------------

export function keyLabel(code: string): string {
  const named: Record<string, string> = {
    ArrowUp: "↑", ArrowDown: "↓", ArrowLeft: "←", ArrowRight: "→", Escape: "Esc", Space: "Space", Enter: "Enter",
    NumpadEnter: "Num Enter", ShiftLeft: "Left Shift", ShiftRight: "Right Shift", ControlLeft: "Left Ctrl",
    ControlRight: "Right Ctrl", AltLeft: "Left Alt", AltRight: "Right Alt", Backquote: "`", Minus: "-", Equal: "=",
    BracketLeft: "[", BracketRight: "]", Backslash: "\\", Semicolon: ";", Quote: "'", Comma: ",", Period: ".", Slash: "/",
    NumpadMultiply: "Num *", NumpadSubtract: "Num -", NumpadAdd: "Num +", NumpadDecimal: "Num .", NumpadDivide: "Num /",
    PageUp: "Page Up", PageDown: "Page Down", CapsLock: "Caps Lock", NumLock: "Num Lock", ScrollLock: "Scroll Lock",
  };
  if (named[code]) return named[code];
  if (/^Key[A-Z]$/.test(code)) return code.slice(3);
  if (/^Digit\d$/.test(code)) return code.slice(5);
  if (/^Numpad\d$/.test(code)) return `Num ${code.slice(6)}`;
  return code;
}

/** KeyboardEvent.code is one the game can see (it has a scan code). */
export const isGameKey = (code: string) => code in KEYS;

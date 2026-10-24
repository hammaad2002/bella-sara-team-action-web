#!/usr/bin/env node
// Headless check of controller play, with a simulated controller in place of
// navigator.getGamepads() (local development only):
//   play     — starts a new game using only the controller (A through the menus, left stick up to
//              walk, Start to pause), captures frames to work/web-frames-pad
//   node tools/web-pad-test.mjs [--port 5174] [--only play] [--seconds 260] [--browser chrome|firefox|webkit]
import { mkdirSync, rmSync, writeFileSync } from "node:fs";
import { browserArgs, launch as launchBrowser } from "./browsers.mjs";

const args = process.argv.slice(2);
const opt = (name, dflt) => {
  const i = args.indexOf(`--${name}`);
  return i >= 0 ? args[i + 1] : dflt;
};
const port = opt("port", "5174");
const only = opt("only", "");
const base = `http://localhost:${port}/`;
let failures = 0;
const check = (ok, what, detail = "") => {
  console.log(`${ok ? "PASS" : "FAIL"} ${what}${detail ? ` — ${detail}` : ""}`);
  if (!ok) failures++;
};

// Simulated standard-mapping controller. Its state follows a script keyed by presented frames
// (window.__bella.frames) or, before the game runs, window.__padHold (button indices / axes).
const fakePad = (script) => {
  const pad = {
    id: "Test Controller (STANDARD GAMEPAD Vendor: 045e Product: 028e)",
    index: 0,
    connected: true,
    mapping: "standard",
    timestamp: 0,
    axes: [0, 0, 0, 0],
    buttons: Array.from({ length: 17 }, () => ({ pressed: false, touched: false, value: 0 })),
    vibrationActuator: {
      playEffect: (type, p) => (window.__rumble.push({ type, ...p }), Promise.resolve("complete")),
      reset: () => (window.__rumble.push({ type: "reset" }), Promise.resolve("complete")),
    },
  };
  window.__rumble = [];
  window.__padHold = { buttons: [], axes: [0, 0, 0, 0] };
  const names = { a: 0, b: 1, x: 2, y: 3, lb: 4, rb: 5, lt: 6, rt: 7, back: 8, start: 9, ls: 10, rs: 11, up: 12, down: 13, left: 14, right: 15 };
  const axes = { lx: 0, ly: 1, rx: 2, ry: 3 };
  navigator.getGamepads = () => {
    const f = window.__bella?.frames ?? 0;
    const held = new Set(window.__padHold.buttons);
    const ax = [...window.__padHold.axes];
    for (const e of script) {
      if (f < e.frame || f >= e.frame + e.hold) continue;
      if (e.control in names) held.add(names[e.control]);
      else if (e.control in axes) ax[axes[e.control]] = e.value;
    }
    pad.buttons.forEach((b, i) => {
      b.pressed = held.has(i);
      b.value = b.pressed ? 1 : 0;
    });
    pad.axes = ax;
    pad.timestamp = performance.now();
    return [pad, null, null, null];
  };
};
const parseScript = (s) =>
  s.split(",").filter(Boolean).map((item) => {
    const [frame, rest] = item.split(":");
    const [ctl, hold] = rest.replace(/^p0\./, "").split("+");
    const [control, value] = ctl.split("=");
    return { frame: Number(frame), control, value: value === undefined ? 1 : Number(value), hold: Number(hold ?? 4) };
  });

const launch = async (script) => {
  const ctx = await launchBrowser({ ...browserArgs(args), port });
  await ctx.addInitScript(fakePad, parseScript(script));
  const page = ctx.pages()[0] ?? (await ctx.newPage());
  page.on("pageerror", (e) => console.log(`pageerror: ${e.message}`));
  return { ctx, page };
};

// The settings screen is covered by tools/web-settings-test.mjs (Bella's settings.dat Graphics panel).

if (!only || only === "play") {
  // The same menu path as the native test (tools: BELLA_PAD), on a fresh save.
  const script = "100:a,180:a,250:ly=-1+60,320:start";
  const captures = [120, 240, 360];
  const { ctx, page } = await launch(script);
  await page.goto(`${base}?autoplay&resetSave&env=BELLA_TRACE=input&capture=${captures.join(",")}`);
  const outDir = "work/web-frames-pad";
  rmSync(outDir, { recursive: true, force: true });
  mkdirSync(outDir, { recursive: true });
  const deadline = Date.now() + Number(opt("seconds", "260")) * 1000;
  while (Date.now() < deadline) {
    await page.waitForTimeout(3000);
    const s = await page.evaluate(() => ({ state: window.__bella?.state ?? "loading", frames: window.__bella?.frames ?? 0, caps: Object.keys(window.__bella?.captures ?? {}).length }));
    if (s.state === "error" || s.caps >= captures.length) break;
  }
  for (const [frame, url] of Object.entries(await page.evaluate(() => window.__bella?.captures ?? {})))
    writeFileSync(`${outDir}/frame_${String(frame).padStart(5, "0")}.png`, Buffer.from(url.split(",")[1], "base64"));
  const log = await page.evaluate(() => window.__bella?.log ?? []);
  writeFileSync(`${outDir}/engine.log`, log.join("\n"));
  check(log.some((l) => l.includes("input: virtual gamepad 0 connected")), "the game sees player 1's controller");
  const [sw, sh] = await page.evaluate(() => [window.screen.width, window.screen.height].map((n) => Math.round((n * (devicePixelRatio || 1)) / 2) * 2));
  const [vw, vh] = await page.evaluate(() => [window.innerWidth, window.innerHeight].map((n) => Math.round((n * (devicePixelRatio || 1)) / 2) * 2));
  const resMatch = log.some((l) => l.includes("CreateDevice") && (l.includes(`${sw}x${sh}`) || l.includes(`${vw}x${vh}`)));
  check(resMatch, `game started at the default Match screen (${sw} × ${sh})`);
  check((await page.evaluate(() => Object.keys(window.__bella?.captures ?? {}).length)) === captures.length, "reached gameplay with the controller", `captures in ${outDir}`);
  await ctx.close();
}

console.log(failures ? `${failures} check(s) failed` : "all checks passed");
process.exit(failures ? 1 : 0);

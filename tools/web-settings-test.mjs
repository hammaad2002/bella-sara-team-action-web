#!/usr/bin/env node
// Headless check of the Graphics settings panel against settings.dat in OPFS (local development only).
//   node tools/web-settings-test.mjs [--port 5575] [--url http://localhost:5575] [--browser chrome|firefox|webkit] [--game]
// Uses its own browser profile (work/<browser>-profile-<port>); installs from the dev ISO when empty.
// With --game it also starts the game and checks the logged CreateDevice resolution.
import assert from "node:assert/strict";
import { browserArgs, launch } from "./browsers.mjs";

const args = process.argv.slice(2);
const opt = (n, d) => (args.includes(`--${n}`) ? args[args.indexOf(`--${n}`) + 1] : d);
const port = opt("port", "5575");
const base = opt("url", `http://localhost:${port}`);
const ctx = await launch({ ...browserArgs(args), port });
const page = ctx.pages()[0] ?? (await ctx.newPage());
page.on("pageerror", (e) => console.log("pageerror:", e.message));

const readDat = () =>
  page.evaluate(async () => {
    try {
      const root = await navigator.storage.getDirectory();
      const dir = await (await root.getDirectoryHandle("bella")).getDirectoryHandle("Bella Sara");
      return await (await (await dir.getFileHandle("settings.dat")).getFile()).text();
    } catch {
      return null;
    }
  });
const waitState = (s, t = 180_000) => page.waitForFunction((s) => window.__bella?.state === s, s, { timeout: t });
const saved = () => page.waitForFunction(() => /^Saved/.test(document.getElementById("set-status").textContent));
const val = (xml, k) => new RegExp(`<${k} value="(\\d+)"`).exec(xml)?.[1];

await page.goto(`${base}/?devIso`);
await waitState("ready");
console.log("installed; settings.dat before:", JSON.stringify(await readDat()));

// A game-written file with an unknown extra element and a changed value: must survive edits.
const original = [
  "<settings>", '    <FirstTime value="0" />', '    <Extra value="7" />', "    <VideoMode>", '        <VM_WIDTH value="800" />', '        <VM_HEIGHT value="600" />',
  '        <VM_DEPTH value="32" />', '        <VM_REFRESH value="0" />', '        <VM_MULTISAMPLELEVEL value="1" />', '        <VM_WINDOW value="0" />', "    </VideoMode>",
  "    <GraphicsSettings>", '        <ADV_TEXTUREFILTERMODE value="1" />', '        <ADV_HORSESHADER value="1" />', '        <ADV_SIMPLEWATER value="0" />',
  '        <ADV_CULLINGDISTANCE value="1" />', "    </GraphicsSettings>", "</settings>", "",
].join("\r\n");
await page.evaluate(async (text) => {
  const dir = await (await (await navigator.storage.getDirectory()).getDirectoryHandle("bella")).getDirectoryHandle("Bella Sara", { create: true });
  const w = await (await dir.getFileHandle("settings.dat", { create: true })).createWritable();
  await w.write(text);
  await w.close();
}, original);

await page.evaluate(() => localStorage.clear()); // a previous run left "Match screen" off
await page.reload();
await waitState("ready");
await page.click("#settings");
await page.locator("#settings-panel").waitFor({ state: "visible" });
assert.equal(await page.locator("#set-resolution option:checked").innerText().then((t) => t.startsWith("Match screen")), true, "default resolution is Match screen");
assert.equal(await page.inputValue("#set-quality"), "Custom", "partial file is not a preset");
await page.selectOption("#set-resolution", "1280x960");
await page.selectOption("#set-quality", "Low");
await saved();
let xml = await readDat();
console.log("after 1280x960 + Low:\n" + xml);
assert.equal(val(xml, "VM_WIDTH"), "1280");
assert.equal(val(xml, "VM_HEIGHT"), "960");
assert.equal(val(xml, "ADV_LOWRESTEXTURES"), "1");
assert.equal(val(xml, "ADV_SIMPLEWATER"), "1");
assert.equal(val(xml, "ADV_CULLINGDISTANCE"), "0");
assert.equal(val(xml, "ADV_FULLSCREENBLOOM"), "0");
assert.ok(xml.includes('<Extra value="7" />') && xml.includes('<FirstTime value="0" />') && xml.includes("VM_MULTISAMPLELEVEL"), "other elements kept");
assert.ok(xml.includes("\r\n") && !/[^\r]\n/.test(xml), "CRLF kept");
await page.selectOption("#set-quality", "High");
await page.locator("summary", { hasText: "Advanced" }).click();
await page.check("#set-adv-ADV_PLAYERSHADOWS"); // already on in High; toggle off then check preset flips to Custom
await page.uncheck("#set-adv-ADV_PLAYERSHADOWS");
assert.equal(await page.inputValue("#set-quality"), "Custom");
await page.check("#set-adv-ADV_PLAYERSHADOWS");
assert.equal(await page.inputValue("#set-quality"), "High");
await saved();
xml = await readDat();
assert.equal(val(xml, "ADV_TEXTUREFILTERMODE"), "2");
assert.equal(val(xml, "ADV_FULLSCREENBLOOM"), "1");
assert.equal(val(xml, "ADV_LOWRESTEXTURES"), "0");
await page.screenshot({ path: "work/settings-panel.png", fullPage: true });
await page.click("#settings-close");
await page.locator("#play-panel").waitFor({ state: "visible" });
console.log("settings panel checks passed");

// No file at all: the full default file (FirstTime 1) is written on Play / Save.
await page.evaluate(async () => {
  const dir = await (await (await navigator.storage.getDirectory()).getDirectoryHandle("bella")).getDirectoryHandle("Bella Sara", { create: true });
  await dir.removeEntry("settings.dat").catch(() => {});
});
await page.reload();
await waitState("ready");
await page.click("#settings");
await page.selectOption("#set-resolution", "1280x960");
await saved();
xml = await readDat();
assert.ok(xml.startsWith("<settings>\r\n    <FirstTime value=\"1\" />") && val(xml, "VM_WIDTH") === "1280" && val(xml, "ADV_HORSEHOOFEFFECTS") === "1", "default file written");
console.log("default file written when absent");

// Restore default Match screen so subsequent tests on this profile start at default resolution.
await page.selectOption("#set-resolution", "auto");
await saved();
await page.evaluate(() => localStorage.clear());

if (args.includes("--game")) {
  await page.selectOption("#set-resolution", "1280x960");
  await saved();
  await page.goto(`${base}/?devIso&autoplay`);
  await waitState("running", 300_000);
  await page.waitForFunction(() => window.__bella.log.some((l) => /CreateDevice/.test(l)), null, { timeout: 120_000 });
  const line = await page.evaluate(() => window.__bella.log.find((l) => /CreateDevice/.test(l)));
  console.log(line);
  assert.match(line, /1280x960/);
  await page.waitForFunction(() => window.__bella.frames > 120, null, { timeout: 120_000 }); // High has bloom on: no crash
  console.log("frames:", await page.evaluate(() => window.__bella.frames), "state:", await page.evaluate(() => window.__bella.state));
}
await ctx.close();

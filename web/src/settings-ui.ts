/**
 * The settings panel: graphics options (written to the game's settings.dat as they are changed,
 * applied the next time the game starts) and the browser's substitute-Esc key.
 */
import { DEFAULT_PREFS, savePrefs, type Prefs } from "./prefs.ts";
import {
  ADV_OPTIONS, AUTO_RESOLUTION, DEFAULT_QUALITY, PRESETS, RESOLUTIONS, applyGraphics, isGameKey, keyLabel, loadGraphics, matchScreen,
  presetOf, type Adv,
} from "./settings.ts";

const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;
const CUSTOM = "Custom";

export class SettingsPanel {
  private adv: Adv = { ...PRESETS[DEFAULT_QUALITY] };
  private resolution = AUTO_RESOLUTION;
  private capturing = false;
  private saveTimer = 0;
  private saving: Promise<void> = Promise.resolve();
  private readonly prefs: Prefs;

  constructor(prefs: Prefs) {
    this.prefs = prefs;
    this.build();
  }

  async open(): Promise<void> {
    const g = await loadGraphics();
    this.adv = g.adv;
    this.resolution = this.prefs.autoResolution ? AUTO_RESOLUTION : `${g.width}x${g.height}`;
    this.render();
    $("settings-panel").hidden = false;
    $("play-panel").hidden = true;
  }

  async close(): Promise<void> {
    this.stopCapture();
    if (this.saveTimer) await this.save();
    await this.saving;
    $("settings-panel").hidden = true;
    $("play-panel").hidden = false;
  }

  private build(): void {
    const res = $<HTMLSelectElement>("set-resolution");
    const [mw, mh] = matchScreen();
    res.add(new Option(`Match screen (recommended) — ${mw} × ${mh}`, AUTO_RESOLUTION));
    for (const { group, sizes } of RESOLUTIONS) {
      const og = document.createElement("optgroup");
      og.label = group;
      for (const [w, h] of sizes) og.append(new Option(`${w} × ${h}`, `${w}x${h}`));
      res.append(og);
    }
    const quality = $<HTMLSelectElement>("set-quality");
    for (const name of [...Object.keys(PRESETS), CUSTOM]) quality.add(new Option(name, name));
    const box = $("set-adv");
    for (const o of ADV_OPTIONS) {
      const label = document.createElement("label");
      const input = o.choices ? document.createElement("select") : document.createElement("input");
      input.id = `set-adv-${o.key}`;
      if (input instanceof HTMLSelectElement) {
        o.choices!.forEach((c, i) => input.add(new Option(c, String(i))));
        label.append(`${o.label} `, input);
      } else {
        input.type = "checkbox";
        label.append(input, ` ${o.label}`);
      }
      input.addEventListener("input", () => {
        this.adv[o.key] = input instanceof HTMLInputElement ? Number(input.checked) : Number(input.value);
        this.changed();
      });
      box.append(label);
    }
    res.addEventListener("input", () => {
      this.resolution = res.value;
      this.prefs.autoResolution = res.value === AUTO_RESOLUTION;
      savePrefs(this.prefs);
      this.changed();
    });
    quality.addEventListener("input", () => {
      if (PRESETS[quality.value]) this.adv = { ...PRESETS[quality.value] };
      this.changed();
    });
    $("settings-close").addEventListener("click", () => void this.close());
    $("set-alias").addEventListener("click", () => this.startCapture());
    $("set-revert").addEventListener("click", () => ($("set-revert-confirm").hidden = false));
    $("set-revert-no").addEventListener("click", () => ($("set-revert-confirm").hidden = true));
    $("set-revert-yes").addEventListener("click", () => {
      $("set-revert-confirm").hidden = true;
      this.adv = { ...PRESETS[DEFAULT_QUALITY] };
      this.resolution = AUTO_RESOLUTION;
      Object.assign(this.prefs, DEFAULT_PREFS);
      savePrefs(this.prefs);
      this.changed();
    });
    window.addEventListener("keydown", (e) => this.onKey(e), true);
    document.addEventListener("pointerdown", (e) => {
      if (this.capturing && e.target !== $("set-alias")) this.stopCapture();
    });
  }

  private changed(): void {
    this.render();
    clearTimeout(this.saveTimer);
    $("set-status").textContent = "Saving…";
    this.saveTimer = window.setTimeout(() => void this.save(), 150);
  }

  /** Write the panel's choices into settings.dat; saves run one after another. */
  private save(): Promise<void> {
    clearTimeout(this.saveTimer);
    this.saveTimer = 0;
    const [res, adv] = [this.resolution, { ...this.adv }];
    this.saving = this.saving.then(async () => {
      try {
        await applyGraphics(res, adv);
        $("set-status").textContent = "Saved. Changes apply the next time you press Play.";
      } catch (err) {
        $("set-status").textContent = `Could not save: ${err instanceof Error ? err.message : String(err)}`;
      }
    });
    return this.saving;
  }

  private render(): void {
    const res = $<HTMLSelectElement>("set-resolution");
    // A size saved in the file but not in the list (e.g. restored from a backup) gets its own entry.
    if (![...res.options].some((o) => o.value === this.resolution)) res.add(new Option(this.resolution.replace("x", " × "), this.resolution));
    res.value = this.resolution;
    $<HTMLSelectElement>("set-quality").value = presetOf(this.adv) ?? CUSTOM;
    for (const o of ADV_OPTIONS) {
      const el = $<HTMLInputElement & HTMLSelectElement>(`set-adv-${o.key}`);
      if (o.choices) el.value = String(this.adv[o.key]);
      else el.checked = !!this.adv[o.key];
    }
    $("set-alias").textContent = keyLabel(this.prefs.pauseAlias);
  }

  private startCapture(): void {
    this.capturing = true;
    $("set-alias").classList.add("capturing");
    $("set-alias").textContent = "Press a key…";
  }

  private stopCapture(): void {
    if (!this.capturing) return;
    this.capturing = false;
    $("set-alias").classList.remove("capturing");
    this.render();
  }

  private onKey(e: KeyboardEvent): void {
    if (!this.capturing) return;
    e.preventDefault();
    e.stopPropagation();
    if (e.code === "Escape") return this.stopCapture();
    if (!isGameKey(e.code)) return; // a key the game cannot see; keep waiting
    this.prefs.pauseAlias = e.code;
    savePrefs(this.prefs);
    this.stopCapture();
  }
}

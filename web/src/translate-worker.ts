/// <reference lib="webworker" />
/**
 * Translates the installed `BellaSara.exe` (from OPFS) into a WebAssembly module on the player's own
 * machine and caches it in OPFS as `bella/cache/<name>`. Messages out: progress / done / error.
 */
import { initIced } from "iced-x86";
import icedWasmUrl from "../../node_modules/iced-x86/iced_x86_bg.wasm?url";
import { Program } from "../../recomp/src/cfg.ts";
import { WasmModuleWriter } from "../../recomp/src/emit-wasm.ts";
import { liftFunction } from "../../recomp/src/lift.ts";
import { PeImage } from "../../recomp/src/pe.ts";
import type { ExeMetadata } from "../../recomp/src/translate.ts";
import ghidraHints from "../../research/ghidra-functions.json" with { type: "json" };
import { cacheName, sha256 } from "./cache.ts";

// Address-level metadata for each supported executable, keyed by SHA-256.
const METADATA = import.meta.glob("../../metadata/*.json", { query: "?url", import: "default", eager: true }) as Record<string, string>;

/** Offline metadata omits some Ghidra functions that gen-c still lifts via --hints; merge before translate. */
function metadataWithHints(meta: ExeMetadata): ExeMetadata {
  const entries = new Set(meta.entries);
  for (const f of ghidraHints.functions) entries.add(f.entry);
  if (entries.size === meta.entries.length) return meta;
  return { ...meta, entries: [...entries].sort((a, b) => a - b) };
}

/** Like `translateExe` but `discover({ speculate: true })` so coverage matches gen-c / native. */
function translateInBrowser(
  exe: Uint8Array,
  meta: ExeMetadata,
  progress?: (phase: "discover" | "translate" | "encode", done: number, total: number) => void,
) {
  progress?.("discover", 0, 1);
  const pe = new PeImage(exe);
  const prog = new Program(pe, {
    entries: meta.entries,
    noreturn: meta.noreturn,
    jumpTargets: new Map(meta.jumpTargets),
  });
  prog.discover({ speculate: true });
  const importIndex = new Map(pe.imports.map((im, i) => [im.iatSlot, i]));
  const entries = [...prog.funcs.keys()].sort((a, b) => a - b);
  const exec = pe.sections.filter((s) => s.executable);
  const textLo = Math.min(...exec.map((s) => s.va));
  const textHi = Math.max(...exec.map((s) => s.va + Math.max(s.vsize, s.rawSize)));
  const writer = new WasmModuleWriter(entries, { textLo, textHi });
  for (let i = 0; i < entries.length; i++) {
    writer.add(liftFunction(prog, prog.funcs.get(entries[i])!, (slot) => importIndex.get(slot)!));
    if (i % 250 === 0) progress?.("translate", i, entries.length);
  }
  progress?.("encode", 0, 1);
  const out = writer.finish();
  return { wasm: out.bytes, functions: entries.length, irreducible: out.irreducible };
}

self.onmessage = async () => {
  try {
    const opfs = await navigator.storage.getDirectory();
    const root = await opfs.getDirectoryHandle("bella");
    const exe = new Uint8Array(await (await (await root.getFileHandle("BellaSara.exe")).getFile()).arrayBuffer());
    const sha = await sha256(exe);
    const metaUrl = METADATA[`../../metadata/${sha}.json`];
    if (!metaUrl) throw new Error(`This version of BellaSara.exe is not supported yet (SHA-256 ${sha}).`);
    const meta = metadataWithHints((await (await fetch(metaUrl)).json()) as ExeMetadata);
    initIced(new Uint8Array(await (await fetch(icedWasmUrl)).arrayBuffer()));
    const t0 = performance.now();
    let last = 0;
    const out = translateInBrowser(exe, meta, (phase, done, total) => {
      const now = performance.now();
      if (now - last > 100) {
        last = now;
        postMessage({ type: "progress", phase, done, total });
      }
    });
    const cache = await root.getDirectoryHandle("cache", { create: true });
    const fh = await cache.getFileHandle(cacheName(sha), { create: true });
    const h = await (fh as unknown as { createSyncAccessHandle(): Promise<FileSystemSyncAccessHandle> }).createSyncAccessHandle();
    h.truncate(0);
    h.write(out.wasm, { at: 0 });
    h.flush();
    h.close();
    postMessage({
      type: "done",
      name: cacheName(sha),
      seconds: (performance.now() - t0) / 1000,
      bytes: out.wasm.length,
      functions: out.functions,
      irreducible: out.irreducible,
    });
  } catch (err) {
    postMessage({ type: "error", message: err instanceof Error ? `${err.message}\n${err.stack ?? ""}` : String(err) });
  }
};

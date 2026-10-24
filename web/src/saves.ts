/**
 * Save backups. The game keeps its save (EQSAVE.DAT) and settings.dat in the "Bella Sara" folder
 * of the install; exporting is the only backup. (A ZIP lists the saves under Save/ and
 * settings.dat at its top level; restoring puts both back in "Bella Sara".)
 */
import { INSTALL_DIR } from "./cache.ts";
import { writeOpfsFile } from "./opfs-write.ts";
import { GAME_SUBDIR } from "./settings.ts";
import { makeZip, readZip } from "./zip.ts";

const SAVE_SLOT = /^save\.bin(_2|_3)?$/i;
const MAX_SAVE_BYTES = 4 << 20;

function isSaveFile(name: string): boolean {
  return SAVE_SLOT.test(name) || /\.(dat|sav)$/i.test(name);
}

async function installRoot(): Promise<FileSystemDirectoryHandle> {
  return (await navigator.storage.getDirectory()).getDirectoryHandle(INSTALL_DIR);
}

async function saveDir(create: boolean): Promise<FileSystemDirectoryHandle> {
  return (await installRoot()).getDirectoryHandle(GAME_SUBDIR, { create });
}

async function readSettingsDat(): Promise<{ data: Uint8Array<ArrayBuffer>; date: Date } | null> {
  try {
    const f = await (await saveDir(false)).getFileHandle("settings.dat");
    const file = await f.getFile();
    return { data: new Uint8Array(await file.arrayBuffer()), date: new Date(file.lastModified) };
  } catch {
    return null;
  }
}

/** The save files (EQSAVE.DAT) currently stored in the game folder, by canonical name. */
export async function listSaves(): Promise<{ name: string; data: Uint8Array<ArrayBuffer>; date: Date }[]> {
  let dir: FileSystemDirectoryHandle;
  try {
    dir = await saveDir(false);
  } catch {
    return [];
  }
  const out: { name: string; data: Uint8Array<ArrayBuffer>; date: Date }[] = [];
  for await (const [name, handle] of (dir as unknown as { entries(): AsyncIterable<[string, FileSystemHandle]> }).entries()) {
    if (handle.kind !== "file" || !isSaveFile(name) || name.toLowerCase() === "settings.dat") continue;
    try {
      const f = await (handle as FileSystemFileHandle).getFile();
      out.push({ name: name.toLowerCase(), data: new Uint8Array(await f.arrayBuffer()), date: new Date(f.lastModified) });
    } catch {
      // Removed while listing.
    }
  }
  return out.sort((a, b) => a.name.localeCompare(b.name));
}

/** A ZIP of all save slots (and settings.dat when present), or null when there is nothing to back up. */
export async function exportSaves(): Promise<Blob | null> {
  const saves = await listSaves();
  const settings = await readSettingsDat();
  if (!saves.length && !settings) return null;
  const entries = saves.map((s) => ({ name: `Save/${s.name}`, data: s.data, date: s.date }));
  if (settings) entries.push({ name: "settings.dat", data: settings.data, date: settings.date });
  return new Blob([makeZip(entries)], { type: "application/zip" });
}

export interface ImportPlan {
  files: { name: string; data: Uint8Array<ArrayBuffer> }[];
  settings?: Uint8Array<ArrayBuffer>;
  overwrites: string[];
}

/** Validate chosen files (a backup ZIP or individual save files) before writing anything. */
export async function planImport(files: File[]): Promise<ImportPlan> {
  const found = new Map<string, Uint8Array<ArrayBuffer>>();
  let settings: Uint8Array<ArrayBuffer> | undefined;
  for (const f of files) {
    if (/\.zip$/i.test(f.name)) {
      for (const e of await readZip(new Uint8Array(await f.arrayBuffer()))) {
        const base = e.name.split("/").pop() ?? "";
        if (base.toLowerCase() === "settings.dat") settings = e.data;
        else if (isSaveFile(base)) found.set(base.toLowerCase(), e.data);
      }
    } else if (f.name.toLowerCase() === "settings.dat") {
      settings = new Uint8Array(await f.arrayBuffer());
    } else if (isSaveFile(f.name)) {
      found.set(f.name.toLowerCase(), new Uint8Array(await f.arrayBuffer()));
    } else {
      throw new Error(
        `${f.name} is not a Bella Sara save (expected EQSAVE.DAT, settings.dat, or a backup .zip)`,
      );
    }
  }
  if (!found.size && !settings) throw new Error("no Bella Sara save files found in the selection");
  for (const [name, data] of found) {
    if (data.length === 0 || data.length > MAX_SAVE_BYTES) throw new Error(`${name} has an unexpected size (${data.length} bytes)`);
  }
  if (settings && (settings.length === 0 || settings.length > MAX_SAVE_BYTES)) {
    throw new Error(`settings.dat has an unexpected size (${settings.length} bytes)`);
  }
  const existing = new Set((await listSaves()).map((s) => s.name));
  const overwrites = [...found.keys()].filter((n) => existing.has(n));
  if (settings) {
    try {
      await (await saveDir(false)).getFileHandle("settings.dat");
      overwrites.push("settings.dat");
    } catch {
      // New file.
    }
  }
  return {
    files: [...found].map(([name, data]) => ({ name, data })),
    settings,
    overwrites,
  };
}

export async function applyImport(plan: ImportPlan): Promise<void> {
  // The game's files are upper case (EQSAVE.DAT) and its lookups ignore case: write over the existing name.
  const existing = new Map<string, string>();
  const dir = await saveDir(false).catch(() => null);
  if (dir) for await (const name of (dir as unknown as { keys(): AsyncIterable<string> }).keys()) existing.set(name.toLowerCase(), name);
  for (const f of plan.files) await writeOpfsFile([INSTALL_DIR, GAME_SUBDIR, existing.get(f.name) ?? f.name], f.data);
  if (plan.settings) await writeOpfsFile([INSTALL_DIR, GAME_SUBDIR, "settings.dat"], plan.settings);
}

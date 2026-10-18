/**
 * Installs Bella Sara from the player's own disc image: reads the ISO, the MSI database and
 * the MSZIP cabinet, and writes the INSTALLDIR tree (e.g. `Common/FrontEnd/...`) to a sink.
 */
import { Cabinet } from "./cab.ts";
import { IsoImage } from "./iso9660.ts";
import type { ByteSource, FileSink } from "./io.ts";
import { md5, hex } from "./md5.ts";
import { MsiDatabase, readLayout, type MsiFile, type MsiLayout } from "./msi.ts";

export const PRODUCT_NAME = "Bella Sara";

/**
 * Translation-input rule (docs/PLAN.md §0): the MSI's BellaSara.exe is the SecuROM-wrapped
 * build and cannot be translated. The port's only supported translation input is the
 * unprotected executable shipped loose on the disc at `DINO_R_0WNZ/BellaSara.exe` (8.3
 * uppercase, `;1`-versioned in the ISO; lookup is case-insensitive). It is installed in place
 * of the MSI copy after hash verification, so the installed tree holds no executable copy of
 * our own — it comes entirely from the player's disc.
 */
const TRANSLATION_INPUT_ISO_PATH = "DINO_R_0WNZ/BELLASARA.EXE";
const TRANSLATION_INPUT_SHA256 = "0ca1762db13129fbfa7dee2f3a770dafdfc4468520008b4c95068190545ced05";
const TRANSLATION_INPUT_SIZE = 3496569;
const TRANSLATION_INPUT_INSTALL_PATH = "BellaSara.exe";

/**
 * Files replaced on the disc after the MSI was built (they are stored uncompressed, newer
 * timestamps), so their MsiFileHash describes an older build. MD5s of known retail copies.
 */
const LATE_MASTERED_MD5: Record<string, string[]> = {
  "Game/Shrek 2.exe": ["c3bdca1f047e722e72afaa899841ed5c"], // EN retail, PE timestamp 2004-10-07
};

export interface InstallProgress {
  phase: "scan" | "extract" | "copy" | "done";
  done: number;
  total: number;
  path?: string;
}

export interface InstallOptions {
  /** Return false to skip a file (paths relative to INSTALLDIR). Default: everything. */
  include?: (installPath: string) => boolean;
  verify?: boolean;
  onProgress?: (p: InstallProgress) => void;
}

export interface InstallReport {
  volumeId: string;
  written: number;
  bytes: number;
  skipped: number;
  verified: number;
  mismatches: string[];
  registry: MsiLayout["registry"];
}

export function openDisc(iso: ByteSource): { image: IsoImage; db: MsiDatabase; layout: MsiLayout } {
  const image = new IsoImage(iso);
  for (const e of image.list("")) {
    if (e.isDir || !e.name.toLowerCase().endsWith(".msi")) continue;
    const db = new MsiDatabase(image.open(e.name).read(0, e.size));
    const layout = readLayout(db);
    if (layout.productName === PRODUCT_NAME) return { image, db, layout };
  }
  throw new Error(`no ${PRODUCT_NAME} installer database found on this disc (volume ${image.volumeId})`);
}

export async function install(iso: ByteSource, sink: FileSink, opts: InstallOptions = {}): Promise<InstallReport> {
  const { image, layout } = openDisc(iso);
  const include = opts.include ?? (() => true);
  const verify = opts.verify ?? true;
  const report: InstallReport = { volumeId: image.volumeId, written: 0, bytes: 0, skipped: 0, verified: 0, mismatches: [], registry: layout.registry };
  const wanted = layout.files.filter((f) => f.installPath !== null && include(f.installPath));
  const total = wanted.length;
  const bySeq = new Map(layout.files.map((f) => [f.sequence, f]));

  const emit = async (f: MsiFile, data: Uint8Array) => {
    if (data.length !== f.size) report.mismatches.push(`${f.installPath}: size ${data.length} != ${f.size}`);
    else if (verify && f.md5 && !(f.md5.every((b) => b === 0))) {
      // Note: an all-zero MsiFileHash means "no hash recorded" (e.g. the empty
      // Beach/CrossCountry/Main/Fillmewithdata.txt), not the hash of zero data.
      const got = hex(md5(data));
      if (got === hex(f.md5) || LATE_MASTERED_MD5[f.installPath!]?.includes(got)) report.verified++;
      else report.mismatches.push(`${f.installPath}: MD5 mismatch (${got})`);
    }
    await sink.writeFile(f.installPath!, data);
    report.written++;
    report.bytes += data.length;
    opts.onProgress?.({ phase: f.compressed ? "extract" : "copy", done: report.written, total, path: f.installPath! });
  };

  // Compressed files: one cabinet per media entry; CAB file names are MSI File keys, and
  // the i-th file of the cabinet has sequence (previous media's LastSequence + i + 1).
  let prevLast = 0;
  for (const m of [...layout.media].sort((a, b) => a.diskId - b.diskId)) {
    if (m.cabinet) {
      const cabName = m.cabinet.replace(/^#/, "");
      const cab = new Cabinet(image.open(cabName));
      const byKey = new Map(layout.files.map((f) => [f.key, f]));
      const files = cab.files.map((cf, i) => ({ cf, f: byKey.get(cf.name) ?? bySeq.get(prevLast + i + 1) }));
      for (const folder of cab.folders) {
        // The MSI's BellaSara.exe is the SecuROM-wrapped build: it is never installed. The
        // disc's unprotected executable is written once, below (PLAN §0).
        const members = files.filter((x) => x.cf.folder === folder.index && x.f && x.f.installPath !== null && include(x.f.installPath) && x.f.installPath !== TRANSLATION_INPUT_INSTALL_PATH);
        if (members.length === 0) continue;
        const data = folder.index >= 0 ? cab.extractFolder(folder.index) : new Uint8Array(0);
        for (const { cf, f } of members) await emit(f!, data.subarray(cf.offsetInFolder, cf.offsetInFolder + cf.size));
      }
    }
    prevLast = m.lastSequence;
  }

  // Uncompressed files are stored loose on the disc at their MSI source path.
  for (const f of wanted.filter((x) => !x.compressed)) {
    const e = image.lookup(f.sourcePath);
    if (!e || e.isDir) {
      report.mismatches.push(`${f.installPath}: source not found on disc at ${f.sourcePath}`);
      continue;
    }
    await emit(f, image.open(f.sourcePath).read(0, e.size));
  }

  // Translation-input rule (§0): write the disc's unprotected executable over the MSI's
  // SecuROM-wrapped BellaSara.exe, which cannot be translated and is not installed.
  if (include(TRANSLATION_INPUT_INSTALL_PATH)) {
    const e = image.lookup(TRANSLATION_INPUT_ISO_PATH);
    if (!e || e.isDir) {
      throw new Error(`translation input not found on disc: ${TRANSLATION_INPUT_ISO_PATH} (the port supports only the unprotected executable; see docs/PLAN.md §0)`);
    }
    const data = image.open(TRANSLATION_INPUT_ISO_PATH).read(0, e.size);
    if (data.length !== TRANSLATION_INPUT_SIZE || sha256(data) !== TRANSLATION_INPUT_SHA256) {
      throw new Error(`translation input on disc does not match the known retail build: ${TRANSLATION_INPUT_ISO_PATH} (sha256 ${sha256(data)})`);
    }
    await emit({
      key: "bellasara.exe",
      installPath: TRANSLATION_INPUT_INSTALL_PATH,
      sourcePath: TRANSLATION_INPUT_ISO_PATH,
      size: TRANSLATION_INPUT_SIZE,
      sequence: 0,
      compressed: false,
      md5: null,
    }, data);
  }

  report.skipped = layout.files.length - report.written;
  opts.onProgress?.({
    phase: "done",
    done: report.written,
    total,
    path: total === report.written ? "BellaSara.exe replaced with the disc's unprotected executable (translation input; PLAN §0)" : undefined,
  });
  return report;
}

/**
 * SHA-256 (FIPS 180-4) over a Uint8Array, lowercase hex. Used to verify the disc's
 * unprotected translation-input executable; pure TypeScript so install() stays browser-safe.
 */
function sha256(data: Uint8Array): string {
  const K = new Uint32Array([
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
  ]);
  const h = new Uint32Array([0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]);
  const w = new Uint32Array(64);
  const padded = new Uint8Array(((data.length + 9 + 63) >> 6) << 6);
  padded.set(data);
  padded[data.length] = 0x80;
  const dv = new DataView(padded.buffer);
  dv.setUint32(padded.length - 8, Math.floor((data.length * 8) / 2 ** 32));
  dv.setUint32(padded.length - 4, (data.length * 8) >>> 0);
  for (let off = 0; off < padded.length; off += 64) {
    for (let i = 0; i < 16; i++) w[i] = dv.getUint32(off + i * 4);
    for (let i = 16; i < 64; i++) {
      const s0 = ((w[i - 15] >>> 7) | (w[i - 15] << 25)) ^ ((w[i - 15] >>> 18) | (w[i - 15] << 14)) ^ (w[i - 15] >>> 3);
      const s1 = ((w[i - 2] >>> 17) | (w[i - 2] << 15)) ^ ((w[i - 2] >>> 19) | (w[i - 2] << 13)) ^ (w[i - 2] >>> 10);
      w[i] = (w[i - 16] + s0 + w[i - 7] + s1) >>> 0;
    }
    let [a, b, c, d, e, f, g, hh] = [h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]];
    for (let i = 0; i < 64; i++) {
      const S1 = ((e >>> 6) | (e << 26)) ^ ((e >>> 11) | (e << 21)) ^ ((e >>> 25) | (e << 7));
      const ch = (e & f) ^ (~e & g);
      const t1 = (hh + S1 + ch + K[i] + w[i]) >>> 0;
      const S0 = ((a >>> 2) | (a << 30)) ^ ((a >>> 13) | (a << 19)) ^ ((a >>> 22) | (a << 10));
      const maj = (a & b) ^ (a & c) ^ (b & c);
      const t2 = (S0 + maj) >>> 0;
      hh = g; g = f; f = e; e = (d + t1) >>> 0; d = c; c = b; b = a; a = (t1 + t2) >>> 0;
    }
    h[0] = (h[0] + a) >>> 0; h[1] = (h[1] + b) >>> 0; h[2] = (h[2] + c) >>> 0; h[3] = (h[3] + d) >>> 0;
    h[4] = (h[4] + e) >>> 0; h[5] = (h[5] + f) >>> 0; h[6] = (h[6] + g) >>> 0; h[7] = (h[7] + hh) >>> 0;
  }
  let out = "";
  for (let i = 0; i < 8; i++) {
    const word = h[i];
    for (let j = 0; j < 4; j++) out += ((word >>> (24 - j * 8)) & 0xff).toString(16).padStart(2, "0");
  }
  return out;
}

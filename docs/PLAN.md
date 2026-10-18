# Bella Sara (PC, 2008) — web port plan

Play your own copy of *Bella Sara* in a desktop browser. The repository ships **no game files and
no game code**: each player picks their own disc image, the browser unpacks it into private
storage and translates the game's x86 program to WebAssembly on that machine, and the translated
code runs on a re-implementation of the Windows, Direct3D 8, DInput8 and FMOD interfaces the game
uses.

The framework (`installer/`, `recomp/`, `runtime/`, `web/`, `tools/`) is forked from the author's
own MIT-licensed `shrek2-team-action-web` port. Everything below is the delta that Bella Sara
needs. `docs/JOURNAL.md` records facts and gotchas as they are established.

## 0. Target facts (established 2026-10-17, see JOURNAL)

| Item | Value |
|---|---|
| Disc | `Bella Sara.iso`, volume `BELLASARA`, ISO 9660 + Joliet, NERO-authored, 389,394,432 B |
| Installer | `Bella Sara.msi` (1,494,528 B) + `Data1.cab` (293,958,603 B, **MSZIP**, 147 folders, 1,101 files) |
| Product | `Bella Sara` (MSI `ProductName`), media `DISK1`, `LastSequence` 1101 |
| Installed tree | 1,101 files / 537.7 MB, 1,098 with `MsiFileHash` MD5s; `Win32/…`, `Common/…`, `fmod.dll`, `BellaSara.exe` |
| Engine | Codemasters **PilotLight** ("Darwin" core), RenderWare-derived assets (`.dff/.txd/.anm/.rws/.col`), own `.dwa`/`.ffs`/`.met` formats |
| Graphics | **Direct3D 8** (`Direct3DCreate8` is the only d3d8 import); D3DX8 statically linked; effects in `Win32/Graphics/Shaders/*.fx` with inline `vs.1.1` / `ps.1.1` |
| Audio | **FMOD 3** (`fmod.dll`, 53 `FSOUND_*`/`FMUSIC_*` stdcall imports); banks `Win32/Sound/*.ffs` |
| Other imports | `dinput8`(1), `winmm`(5), `ole32`(6), `advapi32`(5), `user32`(45), `kernel32`(110), `gdi32`(1), `shell32`(1) — 228 total |
| Executable | `DINO_R_0wnZ/BellaSara.exe` on the disc: 3,496,569 B, sha256 `0ca1762d…`, entry RVA `0x299e9b`, 15,091 functions discovered, 822 k instructions |

### Decision: which executable is translated

The exe the MSI installs (`BellaSara.exe`, 9,367,552 B, sha256 `e1417ede…`) is wrapped in
**SecuROM v7** (`Stext`/`Sitext`/`Srdata`/`Sdata`/`Sidata`/`.securom` sections) and its `.text` is
encrypted at rest (97 % of bytes differ from the unprotected image), so it cannot be statically
translated. The disc also carries the unprotected build at `DINO_R_0wnZ/BellaSara.exe`, whose
`.rsrc`/`.data` match the retail image and whose entry point is ordinary MSVC CRT startup.

**The port supports that unprotected executable and nothing else.** It is copied from the player's
own disc; no DRM is bypassed, no decryption is implemented, and no copy of any executable is
shipped. If a disc without that file is used, the installer stops with a clear message.

## 1. Architecture (unchanged from the Shrek port)

1. `installer/` — ISO 9660/Joliet → CFB → MSI tables → CAB decoder; writes the installed tree with
   MD5 verification, into OPFS in the browser (`FileReaderSync` + sync access handles) or a
   directory in Node.
2. `recomp/` — iced-x86 → CFG → IR → WebAssembly (and C for debugging/native). One wasm function
   per guest function, `dispatch(c, addr)` by table index, sharing the runtime's linear memory.
   Offline `metadata/<sha256>.json` holds **addresses only**.
3. `runtime/` — C++/Emscripten HLE: Win32, D3D8 → GLES 3/WebGL 2 command stream, DInput8, FMOD.
4. `web/` — the page: install, translate, play, settings, saves, controls; engine contains no game
   code, translated module cached in OPFS.
5. `tools/` — builds, Ghidra export, headless browser tests, release scan.

## 2. Milestones

Each milestone lists its deliverables, the interfaces it fixes, and the command that decides it.

### M0 — Recon (this document + `docs/JOURNAL.md`)
Established: disc layout, MSI/CAB facts, exe choice and hashes, import surface, engine identity,
shader model, audio stack, lifter gap (2,317 of 15,091 functions trap, dominated by SSE1).
**Exit:** JOURNAL holds the facts with evidence (hashes, sizes, counts).

### M1 — Installer (`installer/`)
Deliverables: MSZIP support in `installer/src/cab.ts` (folder compression 1) + the inflater it
needs; `PRODUCT_NAME = "Bella Sara"`; the exe rule from §0; a Node CLI that installs the tree.
Interfaces: `Cabinet.extractFolder(i): Uint8Array` unchanged; `install(iso, sink, opts): InstallReport`
unchanged; the module stays browser-safe (no Node-only imports outside `node.ts`).
**Exit:** `node installer/src/cli.ts "Bella Sara.iso" work/install` writes 1,101 files / 537.7 MB,
MD5-verifies 1,098, reports 0 mismatches, and a sample of files is byte-identical to `cabextract`.

### M2 — Recompiler (`recomp/`)
Deliverables: SIMD lifting — SSE1/SSE2 scalar+packed, MMX, and `fcomip`/`fucomip`/`pushf`/`popf` —
in `lift.ts`/`ir.ts`/`x86.ts`; `metadata/<sha256>.json` for the supported exe (from Ghidra hints);
conformance harness extended to snippets containing SIMD forms.
Interfaces: `translateExe(exe, meta): { wasm, functions }` unchanged; the wasm module's imports stay
the existing 28 runtime helpers plus the HLE index.
**Exit:** `node /tmp/bella-swarm/tools/probe-traps.ts work/ref/BellaSara.exe` reports no remaining
SSE1/SSE2/MMX trap outside 3DNow!-only code paths, and the snippet differential run
(`tests/conformance/snippets.sh` + `run.py`) passes for every snippet.

### M3 — Native runtime boot (`runtime/`)
Deliverables: HLE for the 228 imports the game uses (kernel32/user32/gdi32/advapi32/ole32/shell32/
winmm/dinput8 by name), PE loader, guest threads, case-insensitive VFS over the installed tree,
virtual registry (the game only reads `Software\Microsoft\Direct3D`), settings/save paths.
**Exit:** the translated game boots natively to the first `Direct3DCreate8` call and its own
`WinMain` message loop runs with a window.

### M4 — D3D8 + GLES (`runtime/src/d3d8/`, `runtime/src/gles/`)
Deliverables: `IDirect3D8`/`IDirect3DDevice8`/textures/vertex+index buffers/surfaces/swap chain/
state blocks as COM vtables that record the existing `cmd::` stream; vs_1_1 (reuse
`gles/sm_vs.cpp`) and **ps_1.1** translators; D3DX8 effects arrive as device shader calls because
D3DX8 is statically linked inside the guest.
**Exit:** intros and the front-end render at the engine's own frame rate in a window, verified by
frame capture.

### M5 — Audio and input (`runtime/src/hle/fmod.cpp`, `dinput8.cpp`)
Deliverables: FMOD 3 HLE (init/output, streams, samples, songs, volumes, 3D attributes) with the
`.ffs` bank and its inner sample/stream formats decoded through the FFmpeg build the runtime
already links; mixer reuse from `audio/mixer.cpp`; keyboard/mouse/controller through DInput8.
**Exit:** music and effects play with correct mixing; the game is playable with keyboard and pad.

### M6 — Gameplay completeness
Saves (`Save/*.dat`), settings, save-slot UI, level loading from the installed tree, tutorials.
**Exit:** a full session (front end → hub → a level → save → reload) runs uninterrupted.

### M7 — Web build (`web/`)
Deliverables: OPFS installer worker over the player's ISO, in-browser translation worker with the
metadata cache, play panel, settings, save backup/restore, controls list, fullscreen, pointer lock,
worker-threaded renderer with one presented frame per animation frame.
**Exit:** headless Chrome installs from the ISO, translates the exe, and reaches gameplay.

### M8 — Browsers and release
Firefox and WebKit runs, quota/private-window handling, `tools/scan-dist.mjs` proof that no game
bytes ship, licences page, README, deploy.
**Exit:** all three browsers pass the release rehearsal from a fresh profile.

## 3. Parallel waves (swarm)

| Wave | Workers (disjoint file sets) |
|---|---|
| 1 | W1 installer/MSZIP · W2 Ghidra export + metadata · W3 SIMD lifter + conformance · W4 runtime scaffold + D3D8/FMOD stubs |
| 2 | FMOD HLE + `.ffs` decode · D3D8 device semantics + ps_1.1 · Win32/HLE completion to first light · web shell rebrand |
| 3 | gameplay/saves · browser tests · release guards |

Wave 1 starts from the bootstrapped tree (framework copied, dependencies installed, `work/ref/`
holding the supported exe). Every worker edits only the files its brief names; verification is run
by the reviewer, not taken from worker summaries.

## 4. Risks and open questions

- **SSE breadth.** ~2.3 k functions need SIMD semantics; correctness must come from the Unicorn
  differential harness, not from spot checks. 3DNow!/MMX-only D3DX paths may stay disabled through
  CPUID reporting, as in the Shrek port, but any MMX used by the engine itself must be lifted.
- **D3D8 vs D3D9.** The renderer's command stream and fixed-function GLSL generator carry over, but
  the D3D8 vtables, `FVF`/shader-handle model, state blocks and `ps.1.1` are new work.
- **FMOD 3.** No open reimplementation exists; the HLE is driven by the game's actual call pattern
  and the `.ffs` bank layout, both to be established in M5.
- **Installer memory.** MSZIP folders can be tens of MB uncompressed; extraction must stream folder
  by folder and never hold the cabinet.
- **Storage.** ~540 MB install; the page must require ~900 MB free and explain private-window
  limits (Firefox has no OPFS in private windows, WebKit older builds have no sync access handles).
- **Legal.** No game data, no translated game code, no executable in the repo or the site; the exe
  choice in §0 keeps the port away from DRM circumvention entirely.

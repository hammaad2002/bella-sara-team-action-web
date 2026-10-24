# Journal

Working log for the Bella Sara web port (the universal-modder "MODLOG"). Facts, versions, decisions
and gotchas — no decompiled code, no game bytes.

Plan and milestone exit criteria: `docs/PLAN.md`.

## 2026-10-17 — M0 recon
- Disc: `Bella Sara.iso`, volume `BELLASARA`, ISO 9660 + Joliet UCS-3, NERO-authored,
  389,394,432 B, logical block 2,048. Contains `Bella Sara.msi` (1,494,528 B), `Data1.cab`
  (293,958,603 B), `SETUP.EXE`, five `.MST` transforms, the DirectX 9.0c redist folder, the five
  readmes, and `DINO_R_0wnZ/BellaSara.exe`.
- MSI: `ProductName` "Bella Sara", one media entry (`DISK1`, `Data1.cab`, LastSequence 1101), no
  `Registry` table (the readme-based setup writes settings itself). Layout is resolvable with the
  Shrek `MsiDatabase`/`readLayout` code unchanged: **1,101 files, 537.7 MB installed, 1,098 carry
  `MsiFileHash` MD5s**. Tree: `Win32/…` (659), `Common/…` (434), `fmod.dll`, `BellaSara.exe`,
  five readmes.
- CAB: 147 folders, 1,101 files, all compressed, **compression type 1 = MSZIP** (the Shrek reader
  only handles stored and LZX, so MSZIP support is M1's work). `Data1.cab` is a single cabinet
  (no spanning).
- Asset inventory by extension: `.png` 721, `.csv` 97, `.dff` 69 (RenderWare models), `.txt` 38,
  `.xml` 37, `.sod` 27, `.anm` 26, `.txd` 16 (RenderWare texture dictionaries), `.atr` 12, `.dwa`
  10, `.col` 9, `.fx` 8, `.rws` 5, `.met` 3, `.ffs` 2, `.wmv` 2; by size: `.dwa` 175.6 MB, `.txd`
  153.0 MB, `.png` 126.3 MB, `.ffs` 49.4 MB.
- Engine: Codemasters **PilotLight** with the "Darwin" core (`E:\bellasara\pilotlight\Development\
  Code\…` paths survive in asserts), RenderWare-derived geometry/animation data, own archive
  (`FL_FileSystem_Archive.c`), own serialiser.
- Graphics: imports only `d3d8.dll!Direct3DCreate8`; **D3DX8 is statically linked** into the exe
  ("D3DX8 Shader Assembler Version 0.91"), so `.fx` effects are compiled inside the guest and
  arrive at the device as shader bytecode. Shipped effects (`Win32/Graphics/Shaders/*.fx`:
  bloom, flag, horse, northernlights, sails, shore, water, waterfall) declare inline `vs.1.1` and
  `ps.1.1` assembly (checked `water.fx`).
- Audio: `fmod.dll` (shipped on the disc, 162,816 B) with 53 imports of the old `FSOUND_*`/`FMUSIC_*`
  stdcall API. Sound data lives in `Win32/Sound/effects.ffs` (30.1 MB) and `music.ffs` (19.3 MB),
  read by the game's own `EQ_SoundFFSData` bank code. Notable: **no `FSOUND_Sample_Load` import** —
  the sample path must be established in M5.
- Other imports: `DINPUT8.dll` 1, `WINMM.dll` 5, `ole32.dll` 6, `ADVAPI32.dll` 5, `USER32.dll` 45,
  `KERNEL32.dll` 110, `GDI32.dll` 1, `SHELL32.dll` 1 — 228 functions in all.
- Executables:
  - `DINO_R_0wnZ/BellaSara.exe` (3,496,569 B, sha256
    `0ca1762db13129fbfa7dee2f3a770dafdfc4468520008b4c95068190545ced05`): PE32, 8 sections
    (`.text` at VA 0x401000, plus `_rwcseg/.rdata/.data/_rwdseg/.data1/.rsrc/.DINO`), timestamp
    2008-10-15, entry RVA `0x299e9b` — ordinary MSVC CRT startup. `.data` is byte-identical to the
    retail image and `.rdata` differs in 903 bytes. This is the **supported translation input**.
  - MSI's `BellaSara.exe` (9,367,552 B, sha256
    `e1417ede7b2899132402bb6affc6bd96b78a08d627616976317bf38f7c50e67f`): **SecuROM v7** wrapper
    (`Stext`, `Sitext`, `Srdata`, `Sdata`, `Sidata`, `.securom`) with an encrypted `.text` (97 % of
    bytes differ from the unprotected image) and imports extended by the wrapper (services, sockets,
    version, shlwapi). Not translatable, and not used.
- Recompiler gap, measured by running the Shrek `Program.discover` + `liftFunction` over the
  supported exe: **15,091 functions, 822,290 instructions, 2,317 functions trap** (58,664 trap
  sites). By mnemonic (sites/functions): `movss` 27812/1708, `mulss` 6272/647, `addss` 4058/623,
  `subss` 3393/604, `movaps` 2915/472, `comiss` 2460/554, `movq` 2016/75, `xorps` 1388/857,
  `pfmul` 1288/65, `pfadd` 725/59, `cvtsi2ss` 546/202, `punpckhdq` 532/48, `divss` 441/192,
  `mulps` 327/43, `shufps` 303/40, `movd` 273/49, `ucomiss` 214/87, `fcomip` 200/127,
  `cvttss2si` 163/45, `pushf`/`popf` 8/4, plus 3DNow! (`pfsub`, `pfacc`, `pfrcp`, `pfrsqrt`,
  `pi2fd`, `pfnacc`) and SSE2 packed (`paddw`, `psubd`, `por`). Also `int3` in 481 functions
  (padding/asserts; trap only if executed) and "switch index out of range" in 188 functions
  (jump-table discovery without Ghidra hints).
- Toolchain on this machine: Node 24.18.0, Ghidra 12.1.4 (`/opt/ghidra` and
  `~/Downloads/ghidra_12.1.4_PUBLIC`), Emscripten 6.0.12 + FFmpeg 7.1.1 under the Shrek repo's
  `work/tools/`, clang, SDL2 2.0.20, libavcodec 58.134.100, 20 cores.
- Framework forked from `shrek2-team-action-web` (same author, MIT): installer, recomp, runtime,
  web, tools, conformance harness. `PRODUCT_NAME` and exe rules must change; CAB needs MSZIP.

## 2026-10-17 — M1 installer, and the metadata + runtime scaffold
- Swarm: four `opencode` GLM/deepseek workers on disjoint file sets (briefs in `/tmp/bella-swarm/`),
  reviewed by hand. Two workers died on "Upstream request failed: Endpoint is unavailable" and one
  more burned its session exploring without editing; the fallback model plus narrower briefs
  recovered the work.
- Installer (`installer/`): MSZIP support added (`installer/src/mszip.ts`, pure-TS raw inflate with
  a 32 KB dictionary carried across the folder's blocks; `cab.ts` type-1 path strips the 2-byte
  `CK` checksum per block). `PRODUCT_NAME` is `Bella Sara`; `install.ts` carries the PLAN §0
  translation-input rule (the disc's `DINO_R_0WNZ/BellaSara.exe`, SHA-256 checked before writing)
  and never installs the MSI's SecuROM-wrapped exe.
- Verified end to end by the reviewer: fresh `node installer/src/cli.ts "Bella Sara.iso" <dir>` →
  **1100 files, 557.7 MB, MD5-verified 1097, skipped 1, exit 0** in 32 s; every installed path exists
  with the MSI's byte size (0 mismatches, 0 extra), `BellaSara.exe` is the supported 3,496,569-byte
  build, and `Win32/Terrain/Hub/terrain.txd` (66 MB), `horse.dwa` (19 MB), `fmod.dll` and
  `Common/Fonts/Tahoma16.met` are byte-identical to `cabextract`. `tests/installer.test.ts` 3/3.
- Installed-tree arithmetic: the MSI lists 1,101 files, of which **1,100 are inside INSTALLDIR** (the
  1,101st is `System32/Redist/MS/System/msvcrt.dll`, outside the game tree and not installed).
  1,098 files have `MsiFileHash` entries; one is an all-zero placeholder
  (`Win32/Terrain/Beach/CrossCountry/Main/Fillmewithdata.txt`, an empty file), so 1,097 is the true
  verified count.
- Review fixes (mine, in the port tree): the CAB reader had a duplicated empty
  `else if (f.compression === 3)` branch that silently disabled LZX extraction (the fork's
  inherited path — fixed); the installer wrote the MSI exe and then overwrote it, double-counting it
  in the progress figures (now skipped entirely); `readLayout` now tolerates the missing `Registry`
  table with a `tableOrEmpty` helper instead of an instance monkey-patch; and the translated
  module's custom sections are `bella.funcs`/`bella.traps` consistently in recomp, runtime, web and
  the conformance harness, with the OPFS install dir (`bella`), engine file names
  (`bellasara.js` / `bellasara_player.js`) and `createBellaSara` matching the runtime target.
- Metadata (`metadata/0ca1762d…json`, committed): Ghidra 12.1.4 analysis of the supported exe via
  `tools/ghidra/ExportFunctions.java` (unchanged from the proven Shrek exporter, only its category
  tag differs) → **18,092 function entries, 768 no-return, 224 jump tables**, 167 KB, longest string
  64 characters, addresses only. Regenerating it is byte-identical (deterministic), and
  `cli.ts gen-wasm` consumes it: `translated 18092 functions … 32.5 MB (16 irreducible)`.
- Runtime scaffold (`runtime/`): identity renamed to Bella Sara (install root `BellaSara.exe`,
  `/opfs/bella`, `BELLA_*` env and window names, CMake targets `bellasara`/`bellasara_player`),
  `DirectSound`/Bink dropped from the build, `runtime/src/hle/fmod.cpp` registers all 53
  `FSOUND_*`/`FMUSIC_*` imports as logged stubs, and `runtime/src/d3d8/` holds the `IDirect3D8`
  COM skeleton over `rt::ComInterface` (12 slots logging once and returning `D3DERR_INVALIDCALL`),
  with the wave-2 device-vtable checklist in a comment. A new `-DBELLA_NO_GAME_CODE=ON` mode builds
  `libbella_runtime.a` (14.4 MB) before the translator exists — verified from a clean build dir,
  exit 0. `tools/gen-imports.mjs` prints the exe's 228 imports (kernel32 110, fmod 53, user32 45, …).
- Still open after wave 1: SIMD semantics in the lifter (the blocker for M2/M3 — 84 non-3DNow!
  mnemonics, 2,317 of 15,091 functions); the whole-`web/` rebrand beyond the engine interface names,
  which stays with M7.

## 2026-10-17 — Wave 1 outcome (reviewer-verified)
- **M1 installer** — `node installer/src/cli.ts "Bella Sara.iso" work/install` extracts the MSZIP
  cabinet: 1,101 files / 537.7 MB, 1,098 MD5s verified, 0 mismatches (reviewer end-to-end run).
  Reviewer fixes on top of the worker diff: duplicated empty LZX branch in `cab.ts`, exe being
  written twice by the sink, registry-table row alignment shim in `msi.ts`.
- **M2 metadata** — `research/ghidra-functions.json` written from the Ghidra headless export
  (18,092 addresses-only entries; the Ghidra run itself was executed by the reviewer because two
  workers died waiting on it).
- **M2 SIMD (scalar)** — `recomp/src/lift.ts` gained SSE1 scalar semantics (adds/subs/muls/divs/
  minps/maxps/movaps-family moves, cvtss2si …); reviewer ran `tsc --noEmit` (clean) and
  `sh tests/conformance/snippets.sh 48` independently: 901 snippets / 36,768 runs, **0 SIMD
  failures**; only the 6 pre-existing non-SIMD failures remain (`les`, `fadd st,st(5)`,
  `fcomip`/`fucomip` unimplemented, `fsincos`/`fptan` junk in st5). Out of scope so far: packed
  `*ps`, SSE2 `*pd/*sd`, MMX, `Fcomip`/`Fucomip` → wave 2 (W6).
- **M3 HLE imports** — `runtime/tests/import_coverage.cpp` + link anchors; reviewer rebuilt from
  scratch (`cmake -DBELLA_NO_GAME_CODE=ON`, 0 errors / 0 warnings) and ran
  `import_coverage work/ref/BellaSara.exe` → **228 imports, 228 implemented, 0 missing, exit 0**.
  The 72 newly covered (user32 window/GDI bits, kernel32 file/time/locale, ole32 6, winmm 4,
  advapi32 `RegCreateKeyExA`, shell32, gdi32) are classified real-vs-conservative in
  `work/IMPORT-NOTES.md` with M4/M5/M6 markers.
- Toolchain notes: conformance harness runs only via `.venv/bin/python` (unicorn is not in the
  system python); the port repo is intentionally not a git repository.

## 2026-10-17 — Wave 2 partial outcome (reviewer-verified)
- **W7 ps_1.1 translator** — done and reviewer-verified: `sm_pixel_glsl` in `runtime/src/gles/sm_ps.cpp`
  (443 lines; version/comment/DEF/end token parsing like sm_vs.cpp, tex/texcoord/texm3x2pad/texm3x2tex,
  mov/mul/add/mad/dp3/lrp/cmp/min/max family, _bx2/_sat/_x2/_x8 modifiers, swizzles, ps constants in
  their own uniform array `pc[]`, oDepth/oC1 rejected with `err` → fixed-function fallback with a
  one-time warn in renderer.cpp). `ProgKey.ps_id` added (memset-safe, byte-comparable). Reviewer ran:
  clean build (0 errors/warnings), `sm_ps_test` → all 20+ cases PASS incl. malformed-stream negatives,
  `import_coverage` → 228/228, exit 0.
- **W9 web rebrand** — done and reviewer-verified: zero `shrek` matches under web/ and tools/
  (ts/html/sh); OPFS layout reconciled (no more `Game/` subdir: `BellaSara.exe` at `INSTALL_DIR` root,
  saves at `[INSTALL_DIR, "Save"]`, engine factory `createBellaSara` matching `-sEXPORT_NAME`);
  `tsc --noEmit` exit 0; `vite build` exit 0.
- **W6 SIMD completion / W8 FMOD+.ffs** — NOT done. glm-5.3-flash-free died on upstream errors twice
  (both before editing); a deepseek retry and a second glm run then wedged at startup; finally every
  free model returned `This API key can't call models` (account-level quota). lift.ts untouched
  (1639 lines), fmod.cpp still the 53-stub. Rescheduled for the next swarm session.

## 2026-10-18 — Wave 0 re-baseline (reviewer only, no workers)
- Correction to the wave-2 entry above: W8 (FMOD + `.ffs`) **was** done later (`fmod.cpp` 997 lines, no stubs,
  `work/AUDIO-NOTES.md`), and the lifter is further along than "SSE1 scalar": `snippets.sh 48` now runs
  1,099 snippets / 45,888 runs with **0 unsupported traps and 2 failing snippets** (`les ebx,[eax]` fault
  mismatch; `fsincos` st5 junk).
- `probe-traps work/ref/BellaSara.exe`: only 3DNow! (`pf*`, `pswapd`, `pi2fd`) plus other traps remain —
  `int3` 480 fns (padding), **"switch index out of range" 194 sites / 163 fns**, segment-register moves
  (`push/pop es/ss/ds`, `mov [..],cs/ss`) ~10, `no-return call returned` 3, `ud2` 2. RESULT FAIL only because
  `Pswapd` and `Pi2fd` are not lifted (3DNow!; kept disabled via CPUID).
- `tsc --noEmit` clean; `vitest` 3/3 (installer). `runtime/build/bellasara` was stale (built before the
  COM stdcall fixes, looped forever in `CheckDeviceFormatConversion`); after `cmake --build runtime/build`
  native boot (`BELLA_HIDDEN=1 runtime/build/bellasara work/install`, needs a real `$DISPLAY`) gets through
  `Direct3DCreate8` then shows **`MessageBoxA("Hardware Compatibility Error" … DirectX 8 compatible video card)`**
  and exits 0. That message is the Wave-2 blocker; no frame is dumped yet.
- Housekeeping found: root `package.json` still says `shrek2-web` / Shrek 2 description.

## 2026-10-18 — Wave 1 round 1 (all workers failed, reviewer reverted)
- opencode free endpoints returned `Internal Server Error` for every model (glm, deepseek, longcat, space-bunny,
  big-pickle) for all four first briefs; relaunched on kilo with the user's approval of non-zero-retention models.
- A3 (lifter) timed out with no edits; A4 rewrote `recomp/src/hints.ts` to the wrong schema and injected a
  hardcoded-path metadata loader into `cfg.ts` (broke tsc and 735 conformance snippets) — reviewer restored
  `hints.ts` from Shrek and removed the loader; tsc clean and conformance back to 2 known failures.
  Note: `probe-traps` runs without Ghidra hints, so its "switch index" count is not the real pipeline's.
- B1 only added logging, but established that `GetDeviceCaps` is **not** called before the compatibility
  MessageBox: the check follows the multisample loop and reads guest global `0x7386FC`.
- C1 rate-limited upstream.

## 2026-10-18 — Wave 1/2 with Cursor Composer 2.5 workers (reviewer-verified)
- Free kilo models mostly timed out; switched to `agent -p -f --model composer-2.5` (up to 5 parallel, disjoint files).
- **Lifter (L):** `les/lds/lfs/lgs/lss`, `fsincos` and segment push/pop/mov fixed — `snippets.sh 48`: 45,840 runs,
  **0 failing snippets**; `tsc` clean. (`les` is skipped in the Unicorn oracle, which faults on selector loads.)
- **Resources (R):** `runtime/src/core/resources.{h,cpp}` parse RT_ICON/GROUP_ICON/MENU/DIALOG/STRING/VERSION
  (2/1/1/1/1/1 in the exe); `resources_test` passes; `DialogBoxParamA`, `LoadIconA`, `LoadCursorA` wired.
- **Root cause of the D3D8 boot trouble (reviewer finding):** the D3D8 HLE had been copied from D3D9 and its vtables
  were D3D9-ordered (e.g. a `CheckDeviceFormatConversion` slot that D3D8 lacks, so the game's `+0x34` call, really
  `GetDeviceCaps`, filled nothing). Several workers added per-offset "guest-specific" workarounds and an address-remap
  in `ld32/st32`; the reviewer rejected those. Round B6/B7 rewrote `IDirect3D8` (16 slots) and `IDirect3DDevice8`
  (96 slots) in exact SDK order plus D3D8 resource vtables and removed the hacks. Guest `+0x154` is `GetIndices`
  (state clear), not Present; Present is slot 15 (`+0x3C`).
- **Web (W2):** in headless Chrome with `?devIso&autoplay`, install **1,100 files / 532 MB, 1,097 MD5 verified, 28.5 s**;
  translate **18,106 functions → 32 MB wasm in 9.0 s**; engine reaches `running`. Fixed an invalid-wasm bug in
  `recomp/src/emit-wasm.ts` (`shiftCount()`: i64 shift/rotate counts were emitted as `i32.wrap_i64`). `scan-dist.mjs`
  passes with the `tools/build-release.sh` allow-list. The web engine in `web/public/engine` predates the native
  boot fixes and must be rebuilt (`tools/build-web.sh`) once the native boot is stable.
- Native boot now passes CreateDevice and texture creation and loads assets, then faults in `f_004e2420` because the
  guest shell object at `0x763258` has a null vtable word when `f_00452540` runs. Frames: none yet.

## 2026-10-18 — Native boot reaches a running frame loop (reviewer-verified)
- Root causes found with the new **stack-balance checker** (`recomp/src/emit-c.ts --stack-check`, `work/STACKCHECK-NOTES.md`):
  eleven `IDirect3DDevice8` methods were registered with wrong stdcall argument counts (D3D9 leftovers):
  `Clear` 5→6, `SetRenderTarget` 1→2, `SetIndices` 1→2, `CopyRects` 4→5, `GetBackBuffer` 4→3, `GetGammaRamp` 2→1,
  `DrawIndexedPrimitive` 6→5, `DrawIndexedPrimitiveUP` 7→8, `CreateVertexShader` 2→4, `GetVertexShaderFunction` 2→3,
  `GetPixelShaderFunction` 2→3. The drift made `pop esi` return a stale `3` and `call esi` (Sleep) dispatch to `0x3`.
  A registered-vs-SDK check now runs at `CreateDevice` (`runtime/include/d3d8/d3d8_sdk_nargs.h`, `work/D3D8-AUDIT.md`).
- Rejected as masks and removed: address remapping in `ld32/st32`, `pilotlight_*` fixups, `fixup_shell_dispatch_return`,
  guarded static-init calls from the HLE, and the "save/restore esi around every guest call" emitter hack.
- `GetStreamSource` wrote `0` into a COM wrapper passed as `ppStreamData` (fixed in `d3d8/device.cpp`).
- Lifter/CFG: `jmp [ecx*4+table]` with a negative index (CRT `memmove` tail) trapped with "switch index out of range";
  `recomp/src/cfg.ts` now falls back to an address switch over the hinted targets when hinted entries lie below the table base.
  `jmp dword ptr [abs]` (no index register) is now a plain indirect tail call, not a jump table — this also restores the
  function entry `0x69bb48` (CRT `fwrite` body) that Ghidra missed and a function-pointer table at `0x73a010` references.
- Result (`work/build-M3`, Release, `BELLA_HIDDEN=1 BELLA_DUMP_DIR=…`): 100 s run, **no fatal**, **Present #1200+**, 1,332 frames,
  ~13 fps, later frames 6 draws / 1,320 primitives. Conformance still 45,840 runs / 0 failing; `tsc` clean.
- Known wrong: the frame is a flat red block in the top-left (game back buffer 800×600 vs 1920×1200 output not scaled;
  textures not sampled yet). That is the next wave (graphics correctness).

## 2026-10-23/24 — Fidelity wave: input, resolution, bloom, memory (reviewer + Sonnet subagents)
- Method: reached real gameplay (farm tutorial → stables → Magical Orchard riding) natively with scripted input
  and in headless Chrome through an interactive driver (`work/tools/driver.mjs`, closed-loop clicking via the
  game's own cursor sprite), and compared against 35 captures of the original PC game (`work/ref-shots/`).
- **Mouse buttons never worked.** The game polls its exclusive DirectInput mouse and calls `Acquire` only after
  `Poll` fails (0x682080); our `Poll` returned `DI_OK` for unacquired keyboard/mouse. Now `DIERR_NOTACQUIRED` /
  `DI_NOEFFECT` as on Windows; motion made while unacquired is dropped on `Acquire`; `SetCursorPos` no longer
  injects relative motion. user32 now sends the activation sequence (`WM_ACTIVATEAPP`, `WM_NCACTIVATE`,
  `WM_ACTIVATE`, `WM_SETFOCUS`) on first show and `WM_NCACTIVATE` on focus changes (the game's input-active flag,
  0x73A46C). The cursor is velocity-driven (DInput deltas × 30, clamped) like the original, so the web shell now
  captures the pointer on the first click (`mouseCapture: true`) and integrates motion under lock.
- Headless runs ignore SDL focus loss (scripted input was randomly dropped).
- **Resolution.** The game's own options file `Bella Sara/settings.dat` (XML: `VM_WIDTH/HEIGHT`,
  `VM_MULTISAMPLELEVEL`, `ADV_*`) chooses the mode. The executor clamped the back buffer to the 800×600 window, so
  any other mode rendered a cropped corner. Now the back buffer is the game's size, a fullscreen device sizes the
  HLE window (client coords = resolution) and the page maps the pointer into it (letterbox-aware,
  `web_client_size`). The present blit is LINEAR again (NEAREST at 1.6× gave uneven pixels).
- **Web Graphics panel** replaces the Shrek registry settings: resolution (default "match screen"), Low/Medium/High
  presets over the `ADV_*` options, advanced toggles; written into OPFS `bella/Bella Sara/settings.dat` before
  launch. Save backups now use `bella/Bella Sara/` (where the game actually writes).
- **Bloom** (`ADV_FULLSCREENBLOOM`) segfaulted at boot: RT-texture uploads took the 16-bit repack path; then the
  `CopyRects` back-buffer copy read empty CPU memory. RT uploads use the RGBA8 path and GPU→GPU `CopyRects` emits
  `OpStretchRect`.
- **Out-of-memory fatal** ("arnStructure_BridgeMalloc … out of memory", after tutorial + Orchard in one process):
  D3D8 textures, vertex/index buffers and surfaces never freed their guest memory or GL objects. Destructors now
  free both; the device holds references on bound textures/streams/indices; `GetTexture`/`GetIndices` AddRef.
  Heap now rises and falls with level loads (164–240 MB of 654 MB) instead of only climbing. The "Total memory
  commited" figure on that screen is just `dwTotalPhys - dwAvailPhys` from `GlobalMemoryStatus`.
- **Shader/sampler correctness:** ps_1.x destination shift modifiers (`_x2/_x4/_x8/_d2/_d4/_d8`) were ignored;
  D3D8 texture-stage sampler indices were wrong (MIN/MAG swapped, `ADDRESSW` read `BORDERCOLOR`, anisotropy and
  max mip level taken from D3D9 sampler state, so `ADV_TEXTUREFILTERMODE 2` got 1× anisotropy).
- Debug tooling: `BELLA_CAPTURE=<frame>` (per-draw state, layout, raw vertices, sampled textures as PAM),
  `BELLA_SKIP=a-b`, `BELLA_TRACE=shaders` logs generated GLSL, `BELLA_TRACE=mem`. Per-call d3d8/dinput debug logs
  from the previous commits are now behind trace switches.
- Checked and **not** bugs: the white flash on menu transitions is the game's fade; streaked flowers on steep
  hillsides are planar terrain mapping; Excelsior's pink sheen is the original effect; alpha-tested foliage drawn
  with blend ONE/ZERO is how the game renders cut-outs.
- **Anti-aliasing.** The game picks `min(VM_MULTISAMPLELEVEL, highest type CheckDeviceMultiSampleType accepts)` and
  passes it as D3D8 present-parameter dword 4 (`MultiSampleType`; dword 3 is `BackBufferCount`). `OpBackBuffer` now
  carries the sample count; the executor renders the back buffer into a multisampled renderbuffer (+ matching
  multisampled depth twin) and resolves into the back-buffer texture only when needed (present, dumps, readback,
  StretchRect source such as the bloom copy). Falls back to single-sample on any GL error / incomplete FBO. Level 1 is
  the old path.
- Web tests: `tools/web-settings-test.mjs` covers the new Graphics panel; the Shrek registry/rebinding checks were
  removed from `tools/web-ui-test.mjs` (help section) and `tools/web-pad-test.mjs` (settings section). The dev
  server's `/dev-iso` also accepts `Bella_Sara.iso`.
- **WebKit stalled at frame 22, Chrome lost ~40 s at load.** `ReadFile` went through `fopen`/`fread` with tiny stdio
  buffers; in the web build each refill is a WasmFS OPFS blob read proxied through the page thread (~6 M HLE file
  calls while loading the localisation and asset files). Read-only handles now use an HLE read cache (files ≤ 4 MiB
  loaded whole, otherwise a 1 MiB window): host reads on the Orchard route 275,496 → 855; WebKit now runs at
  ~30 fps and Chrome reaches the title in seconds.
- Runtime self-tests link in the no-game build again (`runtime/tests/plat_stubs.cpp`); `tools/build-release.sh`
  and `tools/web-release-test.mjs` accept `Bella_Sara.iso`.
- **No audio.** FMOD volume was tapered at -0.4 dB/step (`db100_to_gain`), so the game's music (`SetVolume 64`)
  played at about -76 dB and effects (179) at -30 dB; FMOD 3 software volume is linear 0..255. Effects also looped
  forever (the `effects.ffs` header loop range spans each whole sample; only mode bits decide looping) and
  `SetPan(-1)` (FSOUND_STEREOPAN) was clamped to hard left. Hidden/dump runs pull the mixer from a real-time
  timer thread; the WAV dump no longer overwrites itself (seek to end after patching the header).

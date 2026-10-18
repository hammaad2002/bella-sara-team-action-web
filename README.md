# Bella Sara — in your browser (bring your own disc)

Play your own copy of *Bella Sara* (PC, 2008, Codemasters) in a desktop browser.

This repository and the site built from it contain **no game files and no game code**. Each player
chooses their own disc image (`Bella Sara.iso`); the browser unpacks it into its private storage and
translates the game's x86 program to WebAssembly on the player's own machine. The translated code
runs on a re-implementation of the Windows, Direct3D 8, DirectInput 8 and FMOD interfaces the game
uses.

See `docs/PLAN.md` for the architecture and milestone criteria, and `docs/JOURNAL.md` for the
port's engineering log.

Not affiliated with or endorsed by Codemasters or Hidden City Games.

## The disc

The retail disc is copy-protected: the executable the installer writes (`BellaSara.exe`) is wrapped
in SecuROM and cannot be statically translated. The same disc also carries the game's unprotected
executable at `DINO_R_0wnZ/BellaSara.exe`, and that is the file this port runs. It is read from the
player's own disc; no protection is bypassed or removed and no executable is redistributed. A disc
without that file is refused with an explanation.

## How it will work
1. `installer/` reads the ISO 9660 image, the MSI database and the MSZIP cabinet and writes the
   installed tree into the Origin Private File System, checking every file against the MSI's MD5
   hashes.
2. `recomp/` translates that executable (identified by SHA-256; `metadata/` holds address-only
   analysis hints for the supported build) into one WebAssembly module, cached in OPFS.
3. `runtime/` (C++, Emscripten) provides the Windows API, Direct3D 8 on WebGL 2 / OpenGL ES 3,
   DirectInput and FMOD. The game runs on worker threads; the page renders and handles input.
4. `web/` is the page: install, settings, saves, controls, fullscreen.

`work/` and `research/` are git-ignored: game files, extracted data and anything derived from them
never enter the repository or the site.

## Building
Requirements: Node.js 24, Emscripten 6.0.12, FFmpeg 7.1.1 sources, and for the native build SDL2,
FFmpeg and OpenGL ES headers.

```sh
npm install
tools/build-web.sh                       # engine -> web/public/engine
cd web && npx vite                       # development server on http://localhost:5173
```

Installer (development, from a disc image):

```sh
node installer/src/cli.ts "Bella Sara.iso" work/install
```

## Licenses
The code in this repository is under the MIT license (`LICENSE`). The engine statically links
FFmpeg (LGPL 2.1 or later) and uses iced-x86 (MIT), Emscripten (MIT), dlmalloc (MIT-0) and
Khronos headers (MIT). The framework is forked from the author's MIT-licensed
`shrek2-team-action-web`.

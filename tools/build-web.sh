#!/bin/bash
# Build the web runtimes and stage the deployable player engine for the shell.
#   tools/build-web.sh          (run from anywhere)
# - work/build-web-W/bellasara_player.{js,wasm}: runtime without game code -> web/public/engine/
# - work/build-web-W/bellasara.{js,wasm}: development engine with C-backend game code (stays in work/)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Emscripten + FFmpeg live in the sibling Shrek port tree (not copied into this repo).
TOOLCHAIN="${BELLA_WEB_TOOLCHAIN:-$ROOT/../shrek2-team-action-web}"
BUILD="${BELLA_BUILD_WEB:-$ROOT/work/build-web-W}"
FFMPEG_WEB="${BELLA_FFMPEG_WEB:-$TOOLCHAIN/work/tools/ffmpeg-web}"
source "$TOOLCHAIN/work/tools/emsdk/emsdk_env.sh" > /dev/null 2>&1
[ -f "$FFMPEG_WEB/lib/libavcodec.a" ] || BELLA_WEB_TOOLCHAIN="$TOOLCHAIN" BELLA_FFMPEG_WEB="$FFMPEG_WEB" "$ROOT/tools/build-ffmpeg-web.sh"
emcmake cmake -S "$ROOT/runtime" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DGEN_OPT=-O2 -DFFMPEG_WEB="$FFMPEG_WEB" > /dev/null
ninja -C "$BUILD" bellasara_player ${WITH_DEV_ENGINE:+bellasara}
mkdir -p "$ROOT/web/public/engine"
cp "$BUILD/bellasara_player.js" "$BUILD/bellasara_player.wasm" "$ROOT/web/public/engine/"
echo "player engine staged in web/public/engine"

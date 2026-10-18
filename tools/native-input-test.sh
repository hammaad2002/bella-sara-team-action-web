#!/usr/bin/env bash
# Proves BELLA_INPUT scripted keys reach DirectInput keyboard state during a native boot.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BELLA_BUILD_DIR:-$ROOT/work/build-A}"
INSTALL="${BELLA_INSTALL:-$ROOT/work/install}"
BIN="$BUILD/bellasara"
if [[ ! -x "$BIN" ]]; then
  echo "missing executable: $BIN (cmake -S runtime -B work/build-A …)" >&2
  exit 1
fi
LOG="$(mktemp -t bella-input-test.XXXXXX.log)"
trap 'rm -f "$LOG"' EXIT
export BELLA_HIDDEN=1
export BELLA_TRACE=input
export BELLA_INPUT='60:Return+8,180:Down+6'
timeout 30 "$BIN" "$INSTALL" >"$LOG" 2>&1 || true
if ! rg -q 'scripted input: Return down' "$LOG"; then
  echo "FAIL: scripted Return not seen in trace" >&2
  rg 'scripted input|BELLA_INPUT' "$LOG" || true
  exit 1
fi
if ! rg -q 'SetDataFormat size=256 .keyboard.' "$LOG"; then
  echo "FAIL: keyboard SetDataFormat (c_dfDIKeyboard 256 bytes) not traced" >&2
  exit 1
fi
if ! rg -q 'SetCooperativeLevel.*keyboard' "$LOG"; then
  echo "FAIL: keyboard SetCooperativeLevel not traced" >&2
  exit 1
fi
echo "PASS native-input-test (log: $LOG)"
cp "$LOG" "$ROOT/work/trace-A/native-input-test.log" 2>/dev/null || true

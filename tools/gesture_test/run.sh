#!/usr/bin/env bash
# Host test for the touch gesture recogniser (AGENTS.md §9a).
#
# Compiles the REAL main/touch_gesture.c plus this directory's main.c, runs the
# synthetic touch cases and exits non-zero if any of them produced the wrong
# event. Works from any cwd: the repo root comes from this script's location.
set -euo pipefail

CC=${CC:-cc}
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd -- "$HERE/../.." && pwd)
TEST="$HERE/gesture_test"

cd "$ROOT"
"$CC" -std=gnu99 -Wall -Wextra -Werror -O1 -I"$ROOT/main" \
      "$ROOT/main/touch_gesture.c" "$HERE/main.c" -o "$TEST"

exec "$TEST"

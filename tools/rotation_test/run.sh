#!/usr/bin/env bash
# Host test for the turn detector (AGENTS.md §9a).
#
# Compiles the REAL main/rotation_logic.c plus this directory's main.c, runs the
# synthetic-sample cases and exits non-zero if any assertion failed. Works from
# any cwd: the repo root comes from this script's location.
set -euo pipefail

CC=${CC:-cc}
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd -- "$HERE/../.." && pwd)
TEST="$HERE/rotation_test"

cd "$ROOT"
"$CC" -std=gnu99 -Wall -Wextra -Werror -O1 -I"$ROOT/main" \
      "$ROOT/main/rotation_logic.c" "$HERE/main.c" -o "$TEST" -lm

exec "$TEST"

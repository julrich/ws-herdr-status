#!/usr/bin/env bash
# Host render harness for the companion UI (AGENTS.md §9a).
#
# Builds a host LVGL 8.4 together with main/ui_companion.c *unmodified* and
# tools/ui_host_test/host_main.c, runs the scenario assertions, writes
# tools/ui_host_test/frame_<scenario>.ppm and exits non-zero if any assertion
# failed. Works from any cwd: the repo root comes from this script's location.
set -euo pipefail

CC=${CC:-cc}
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd -- "$HERE/../.." && pwd)
OUT="$HERE/build"
HARNESS="$HERE/harness"

CFLAGS="-DLV_CONF_INCLUDE_SIMPLE -DCONFIG_HERDR_UI_MAX_AGENTS=4 -I$HERE -I$ROOT/managed_components/lvgl__lvgl -I$ROOT/main -std=gnu99 -O1 -w -c"

cd "$ROOT"

mapfile -t SRCS < <(find "$ROOT/managed_components/lvgl__lvgl/src" -name '*.c' -print | sort)
SRCS+=("$ROOT/main/ui_companion.c" "$HERE/host_main.c")

rm -rf "$OUT"
mkdir -p "$OUT"

export CC CFLAGS OUT
printf '%s\n' "${SRCS[@]}" | xargs -d '\n' -n1 -P"$(nproc)" bash -c '
    obj="$OUT/$(printf "%s" "$0" | tr "/" "_").o"
    "$CC" $CFLAGS "$0" -o "$obj"
'

"$CC" -o "$HARNESS" "$OUT"/*.o -lm

set +e
"$HARNESS"
rc=$?
set -e
exit "$rc"
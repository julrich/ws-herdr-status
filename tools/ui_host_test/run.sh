#!/usr/bin/env bash
# Host render harness for the companion UI (AGENTS.md §9a).
#
# Builds a host LVGL 9 together with main/ui_companion.c *unmodified* and
# tools/ui_host_test/host_main.c, runs the scenario assertions, writes
# tools/ui_host_test/frame_<scenario>.ppm and exits non-zero if any assertion
# failed. Works from any cwd: the repo root comes from this script's location.
set -euo pipefail

CC=${CC:-cc}
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd -- "$HERE/../.." && pwd)
OUT="$HERE/build"
HARNESS="$HERE/harness"

CFLAGS="-DLV_CONF_INCLUDE_SIMPLE -DCONFIG_HERDR_UI_MAX_AGENTS=4 -I$HERE -I$HERE/esp_shim -I$ROOT/main -I$ROOT/components/lvgl_kawaii_face/include -I$ROOT/managed_components/lvgl__lvgl -std=gnu99 -O1 -w -c"

cd "$ROOT"

mapfile -t SRCS < <(find "$ROOT/managed_components/lvgl__lvgl/src" -name '*.c' -print | sort)
SRCS+=("$ROOT/main/ui_companion.c" "$ROOT/main/mood_face.c" "$HERE/host_main.c")
# The mood face is compiled for real, as the firmware does: the shims above are
# the only difference (AGENTS.md §9a).
SRCS+=("$ROOT/components/lvgl_kawaii_face/lvgl_kawaii_face.c")

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
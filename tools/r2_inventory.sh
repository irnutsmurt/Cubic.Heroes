#!/bin/sh
# R2 chain inventory run: a scenario with tools/r1_dev_lift.sh's development
# executable and DOTHEROES_R2_INVENTORY only -- no capture, no observers --
# so every frame's PPFX chain structure is logged at the cost of a hash per
# draw (rsx_draw_engine.c). Summarise with tools/r2_inventory.py.
#   tools/r2_inventory.sh <tag> <scenario> [secs]   (extra settings from the environment;
#   R2_PROGRAMS=<lo>-<hi> another fragment-program range, 0-FFFFFFFF every draw)
# Into build/r1/inventory/<tag>/: inventory.txt, run/. Game data stays local.
cd "$(dirname "$0")/.." || exit 1
tag=$1; scen=$2; secs=$3
[ -n "$tag" ] && [ -n "$scen" ] || { echo "usage: $0 <tag> <scenario> [secs]"; exit 2; }
case $scen in river_fight) c=build/shader-cache-river;; *) c=build/shader-cache-field;; esac
ROOT=$(pwd)
D="$ROOT/build/r1/inventory/$tag"
rm -rf "build/r1/inventory/$tag"; mkdir -p "build/r1/inventory/$tag"
PY=build/venv/Scripts/python.exe
[ -x "$PY" ] || PY=python3
KEEP_RUNS=60 DOTHEROES_DEV_CANT_DIE=1 FIGHTS=1 AUDIO=1 DOTHEROES_CPU_PLACE=normal DIAG_VBLANK_MS=0.5 \
DOTHEROES_R2_INVENTORY="$D/inventory.txt" DOTHEROES_R2_PROGRAMS=${R2_PROGRAMS:-018F0000-01900000} \
SCENARIO=$scen PS3RECOMP_SPIRV_CACHE=$c BIN="$ROOT/build/r1/bld/dotheroes.exe" RUN_TAG=r2i-$tag \
  "$PY" tests/boot_smoke.py $secs > "build/r1/inventory/$tag.out" 2>&1
echo "$tag exit $?"
mv build/test/boot-*-r2i-$tag "build/r1/inventory/$tag/run" 2>/dev/null
"$PY" tools/r2_inventory.py "$D/inventory.txt"

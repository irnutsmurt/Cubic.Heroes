#!/bin/sh
# R1 capture run (docs/NATIVE_PORT.md section 5): with tools/r1_dev_lift.sh's
# development executable, PPFX passes 3 and 48 of one frame -- the
# consumption capture (rsx_draw_engine.c, rsx_vulkan_backend.c) and the
# boundary log (port/native/render/r1_observe.cpp) together.
#   tools/r1_capture.sh <tag> <scenario> <at seconds> [secs]
# e.g. tools/r1_capture.sh river1 river_fight 100
#      tools/r1_capture.sh field1 field_after_dungeon 120 160
# Into build/r1/captures/<tag>/: pass3/, pass48/, boundary.txt, run/ (the
# test's run directory). Game data: everything there stays local.
# Then, for each pass: build/r1/bld/r1_replay.exe <dir> --validate [--poison],
# tools/r1_compare.py <dir> --png, tools/r1_boundary_check.py <tag dir> pass3 pass48.
cd "$(dirname "$0")/.." || exit 1
tag=$1; scen=$2; at=$3; secs=$4
[ -n "$tag" ] && [ -n "$scen" ] && [ -n "$at" ] || { echo "usage: $0 <tag> <scenario> <at seconds> [secs]"; exit 2; }
case $scen in river_fight) c=build/shader-cache-river;; *) c=build/shader-cache-field;; esac
ROOT=$(pwd)
D="$ROOT/build/r1/captures/$tag"
rm -rf "build/r1/captures/$tag"; mkdir -p "build/r1/captures/$tag"
PY=build/venv/Scripts/python.exe
[ -x "$PY" ] || PY=python3
KEEP_RUNS=60 DOTHEROES_DEV_CANT_DIE=1 FIGHTS=1 AUDIO=1 DOTHEROES_CPU_PLACE=normal DIAG_VBLANK_MS=0.5 \
DOTHEROES_R1_DIR="$D" DOTHEROES_R1_AT=$at DOTHEROES_R1_DRAWS="pass3=02DD0000/018F0380/1,pass48=*/018F0FC0/1" \
SCENARIO=$scen PS3RECOMP_SPIRV_CACHE=$c BIN="$ROOT/build/r1/bld/dotheroes.exe" RUN_TAG=r1-$tag \
  "$PY" tests/boot_smoke.py $secs > "build/r1/captures/$tag.out" 2>&1
echo "$tag exit $?"
mv build/test/boot-*-r1-$tag "build/r1/captures/$tag/run" 2>/dev/null
grep -a "^\[r1\]" "build/r1/captures/$tag/run/stderr.log"

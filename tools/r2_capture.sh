#!/bin/sh
# R2 chain capture run (docs/NATIVE_R2.md): with tools/r1_dev_lift.sh's
# development executable, every PPFX draw of the frame for <frames>
# consecutive frames -- from pass 1 (fragment program 0x018F0000 into the
# scene target 0x01AE0000, drawn before asp renders the scene) to the
# frame's end (the final composite's program varies by area), only draws
# whose fragment program lies in PPFX's program area (asp's scene draws in
# between are recorded as work outside the chain) -- the consumption
# capture's chain mode (rsx_draw_engine.c, rsx_vulkan_backend.c), and the
# boundary log (port/native/render/r1_observe.cpp) over them.
#   tools/r2_capture.sh <tag> <scenario> <at seconds>[,<at>...] <frames> [secs]
# Into build/r1/captures/<tag>/: f<frame>/d<draw>/, f<frame>/ops.jsonl and
# input_r<id>[_g<n>].bin, boundary.txt, run/, and inventory.txt: the chain
# structure of every frame of the whole run (rsx_draw_engine.c,
# DOTHEROES_R2_INVENTORY; tools/r2_inventory.py). With several times, one
# batch each into <tag>/b<k>/ (frames and boundary log), the inventory and
# run/ shared. Then tools/r2_chain.py on each.
# Game data: everything stays local.
cd "$(dirname "$0")/.." || exit 1
tag=$1; scen=$2; at=$3; frames=$4; secs=$5
[ -n "$tag" ] && [ -n "$scen" ] && [ -n "$at" ] && [ -n "$frames" ] || { echo "usage: $0 <tag> <scenario> <at> <frames> [secs]"; exit 2; }
case $scen in river_fight) c=build/shader-cache-river;; *) c=build/shader-cache-field;; esac
ROOT=$(pwd)
D="$ROOT/build/r1/captures/$tag"
rm -rf "build/r1/captures/$tag"; mkdir -p "build/r1/captures/$tag"
PY=build/venv/Scripts/python.exe
[ -x "$PY" ] || PY=python3
KEEP_RUNS=60 DOTHEROES_DEV_CANT_DIE=1 FIGHTS=1 AUDIO=1 DOTHEROES_CPU_PLACE=normal DIAG_VBLANK_MS=0.5 \
DOTHEROES_R1_DIR="$D" DOTHEROES_R1_AT=$at DOTHEROES_R2_FRAMES=$frames DOTHEROES_R2_INVENTORY="$D/inventory.txt" \
DOTHEROES_R2_CHAIN="01AE0000/018F0000/1,end" DOTHEROES_R2_PROGRAMS=018F0000-01900000 \
SCENARIO=$scen PS3RECOMP_SPIRV_CACHE=$c BIN="$ROOT/build/r1/bld/dotheroes.exe" RUN_TAG=r2-$tag \
  "$PY" tests/boot_smoke.py $secs > "build/r1/captures/$tag.out" 2>&1
echo "$tag exit $?"
mv build/test/boot-*-r2-$tag "build/r1/captures/$tag/run" 2>/dev/null
grep -a "^\[r2\]\|^\[r1\]" "build/r1/captures/$tag/run/stderr.log" | head -20

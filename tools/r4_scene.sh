#!/bin/sh
# R4's runs (docs/NATIVE_R4.md) of one scenario, into build/r4/<tag>/: the
# report (report.txt), the first differing outputs (dump/) and the run
# directory (run/, kept out of the test runs' pruning). As tools/r3_scene.sh;
# the R4 switches go after the length, e.g. DOTHEROES_PPFX_BYPASS=1 (the
# bypass), =verify, =alt:<n>.
#   tools/r4_scene.sh <tag> <scenario> <seconds> [NAME=value ...]
# NAME=value: more environment, e.g. DOTHEROES_PPFX_NATIVE=check,
# DOTHEROES_PPFX_COMPARE=0, RUNS=3. The game is R4_BIN (default
# build/r4/b1/dotheroes.exe, a copy of the development lift, so it can be
# rebuilt while a run goes on).
cd "$(dirname "$0")/.." || exit 1
tag=$1; scen=$2; secs=$3
[ -n "$tag" ] && [ -n "$scen" ] && [ -n "$secs" ] || { echo "usage: $0 <tag> <scenario> <seconds> [NAME=value ...]"; exit 2; }
shift 3
case $scen in river_fight) c=build/shader-cache-river;; *) c=build/shader-cache-field;; esac
ROOT=$(pwd)
D="$ROOT/build/r4/$tag"
mkdir -p "$D/dump"
PY=build/venv/Scripts/python.exe
[ -x "$PY" ] || PY=python3
env KEEP_RUNS=60 DOTHEROES_DEV_CANT_DIE=1 FIGHTS=1 AUDIO=1 DOTHEROES_CPU_PLACE=normal \
  DOTHEROES_PPFX_NATIVE=1 DOTHEROES_PPFX_REPORT="$D/report.txt" DOTHEROES_PPFX_DUMP="$D/dump" \
  SCENARIO="$scen" PS3RECOMP_SPIRV_CACHE=$c BIN="$ROOT/${R4_BIN:-build/r4/b1/dotheroes.exe}" RUN_TAG="r4-$tag" "$@" \
  timeout $((secs + 300)) "$PY" tests/boot_smoke.py "$secs" > "$D/out.txt" 2>&1
echo "$tag exit $?" > "$D/exit.txt"
R=$(ls -td build/test/boot-*-r4-"$tag" 2>/dev/null | head -1)
[ -n "$R" ] && mv "$R" "$D/run"
cat "$D/exit.txt"

#!/bin/sh
# R8 runs (docs/NATIVE_R8.md) of one scenario, into build/r8/<tag>/: as
# tools/r7_scene.sh (report.txt, dump/, run/). The game is R8_BIN (default
# build/r8/base/dotheroes.exe: the R7 release, rel5, kept unchanged as
# R8's baseline).
#   tools/r8_scene.sh <tag> <scenario> <seconds> [NAME=value ...]
# Later NAME=value pairs win: DOTHEROES_DEV_CANT_DIE=0 for normal damage.
cd "$(dirname "$0")/.." || exit 1
tag=$1; scen=$2; secs=$3
[ -n "$tag" ] && [ -n "$scen" ] && [ -n "$secs" ] || { echo "usage: $0 <tag> <scenario> <seconds> [NAME=value ...]"; exit 2; }
shift 3
case $scen in river_fight) c=build/shader-cache-river;; *) c=build/shader-cache-field;; esac
ROOT=$(pwd)
D="$ROOT/build/r8/$tag"
mkdir -p "$D/dump"
PY=build/venv/Scripts/python.exe
[ -x "$PY" ] || PY=python3
env KEEP_RUNS=60 DOTHEROES_DEV_CANT_DIE=1 FIGHTS=1 AUDIO=1 DOTHEROES_CPU_PLACE=normal \
  DOTHEROES_PPFX_NATIVE=1 DOTHEROES_PPFX_REPORT="$D/report.txt" DOTHEROES_PPFX_DUMP="$D/dump" \
  SCENARIO="$scen" PS3RECOMP_SPIRV_CACHE=$c BIN="$ROOT/${R8_BIN:-build/r8/base/dotheroes.exe}" RUN_TAG="r8-$tag" "$@" \
  timeout $((secs + 300)) "$PY" tests/boot_smoke.py "$secs" > "$D/out.txt" 2>&1
echo "$tag exit $?" > "$D/exit.txt"
R=$(ls -td build/test/boot-*-r8-"$tag" 2>/dev/null | head -1)
[ -n "$R" ] && mv "$R" "$D/run"
cat "$D/exit.txt"

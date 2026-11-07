#!/bin/sh
# R8's lift (docs/NATIVE_R8.md): R7's native lift (tools/r7_native_lift.sh: the
# production lift with config/r7_native_hooks.json, no store probes) made with
# R8's lifter (the indirect calls' call-site cache, PPU_ICALL) into build/r8/gen,
# leaving R7's lift (build/r7/native/gen) as it is. Build a release from it with
#   tools/r8_release_build.sh <dir> -DRECOMP_DIR="$(pwd)/build/r8/gen" [...]
set -e
cd "$(dirname "$0")/.."
O=${R8_GEN:-build/r8/gen}
PY=build/venv/Scripts/python.exe
[ -x "$PY" ] || PY=python3
mkdir -p "$O"
start=$(date +%s)
"$PY" build/ref/ps3recomp/tools/ppu_lifter.py build/EBOOT.elf \
  --functions build/gen/out/EBOOT.functions.fixed.json --hle-stubs build/gen/out/EBOOT.imports.json \
  --sjlj config/sjlj.json --insn-patches config/insn_patches.json \
  --entry-observers config/r7_native_hooks.json --chunks 48 -o "$O/" > "$O.lift.log" 2>&1
grep -c "entry observer" "$O"/*.cpp | awk -F: '{n+=$2} END{print n " observed functions in the lift"}'
grep -o "PPU_ICALL(ctx)" "$O"/*.cpp | wc -l | awk '{print $1 " call-site caches (PPU_ICALL)"}'
echo "lifted into $O in $(( $(date +%s) - start )) s"

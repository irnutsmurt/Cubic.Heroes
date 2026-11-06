#!/bin/sh
# R6 native-play build (docs/NATIVE_R6.md section 4): the production lift
# plus entry observers at the same API boundaries as the development lift --
# the libgcm entry points, asp's methods, the game's draws, the flip, memcpy
# (config/r6_native_observers.json) -- without the development lift's store
# probes (PPU_OBS_ST on every store of an observed body), into
# build/r6/native/gen, built in build/r6/native/bld with the main build's
# toolchain and options. The production lift (build/gen/recompiled), the
# development lift (build/r1) and config/insn_patches.json are left as they
# are. R4's verify mode needs the development lift.
#   tools/r6_native_lift.sh          lift, configure and build
#   tools/r6_native_lift.sh build    build only (after runtime or port changes)
set -e
cd "$(dirname "$0")/.."
R=build/r6/native
PY=build/venv/Scripts/python.exe
[ -x "$PY" ] || PY=python3
DEPS="$(pwd)/build/deps/windows"
if [ "$1" != build ]; then
  mkdir -p $R/gen
  "$PY" build/ref/ps3recomp/tools/ppu_lifter.py build/EBOOT.elf \
    --functions build/gen/out/EBOOT.functions.fixed.json --hle-stubs build/gen/out/EBOOT.imports.json \
    --sjlj config/sjlj.json --insn-patches config/insn_patches.json \
    --entry-observers config/r6_native_observers.json --chunks 48 -o $R/gen/ > $R/lift.log 2>&1
  grep -c "entry observer" $R/gen/*.cpp | awk -F: '{n+=$2} END{print n " observed functions in the lift"}'
  grep -c "(PPU_OBS_ST(" $R/gen/*.cpp | awk -F: '{n+=$2} END{print n " store probes (0 for this lift)"}'
  cmake -S port -B $R/bld -G Ninja -DCMAKE_TOOLCHAIN_FILE="$(pwd)/build/toolchain/cmake-windows/toolchain.cmake" \
    -DDOTHEROES_DEV_DIAGNOSTICS=ON -DPS3RECOMP_RSX_VULKAN=ON -DPS3RECOMP_VULKAN_INCLUDE_DIR="$DEPS/include" \
    -DCMAKE_FIND_ROOT_PATH="$DEPS" -DCMAKE_PREFIX_PATH="$DEPS" -DRECOMP_DIR="$(pwd)/$R/gen" > $R/configure.log 2>&1
fi
cmake --build $R/bld --target dotheroes > $R/build.log 2>&1
cp build/port-win/SDL2.dll $R/bld/ 2>/dev/null || true
echo "built $R/bld/dotheroes.exe"

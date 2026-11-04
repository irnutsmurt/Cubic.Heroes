#!/bin/sh
# R1 development build (docs/NATIVE_PORT.md section 5): the production lift
# plus entry observers on the libgcm calls PPFX makes
# (config/dev_capture_observers.json, port/native/render/r1_observe.cpp), into
# build/r1/gen, built in build/r1/bld with the main build's toolchain and
# options. The production lift (build/gen/recompiled) and
# config/insn_patches.json are left as they are.
#   tools/r1_dev_lift.sh          lift, configure and build
#   tools/r1_dev_lift.sh build    build only (after runtime or port changes)
set -e
cd "$(dirname "$0")/.."
R=build/r1
PY=build/venv/Scripts/python.exe
[ -x "$PY" ] || PY=python3
DEPS="$(pwd)/build/deps/windows"
if [ "$1" != build ]; then
  mkdir -p $R/gen
  "$PY" build/ref/ps3recomp/tools/ppu_lifter.py build/EBOOT.elf \
    --functions build/gen/out/EBOOT.functions.fixed.json --hle-stubs build/gen/out/EBOOT.imports.json \
    --sjlj config/sjlj.json --insn-patches config/insn_patches.json \
    --entry-observers config/dev_capture_observers.json --chunks 48 -o $R/gen/ > $R/lift.log 2>&1
  grep -c "entry observer" $R/gen/*.cpp | awk -F: '{n+=$2} END{print n " observed functions in the lift"}'
  cmake -S port -B $R/bld -G Ninja -DCMAKE_TOOLCHAIN_FILE="$(pwd)/build/toolchain/cmake-windows/toolchain.cmake" \
    -DDOTHEROES_DEV_DIAGNOSTICS=ON -DPS3RECOMP_RSX_VULKAN=ON -DPS3RECOMP_VULKAN_INCLUDE_DIR="$DEPS/include" \
    -DCMAKE_FIND_ROOT_PATH="$DEPS" -DCMAKE_PREFIX_PATH="$DEPS" -DRECOMP_DIR="$(pwd)/$R/gen" > $R/configure.log 2>&1
fi
cmake --build $R/bld --target dotheroes > $R/build.log 2>&1
cp build/port-win/SDL2.dll $R/bld/ 2>/dev/null || true
echo "built $R/bld/dotheroes.exe"

#!/bin/sh
# R7's native release build (docs/NATIVE_R7.md): R7's lift (build/r7/native/gen,
# from tools/r7_native_lift.sh: the entry hooks, no store probes) built with
# DOTHEROES_NATIVE_RELEASE=ON -- the runtime without the RSX command path
# (PS3RECOMP_NATIVE_GRAPHICS: no FIFO walker, method decoder, register file,
# draw engine, live draw or backend drawing) and the port without R4's
# bridge, its handbacks and comparisons -- into build/r7/release/bld.
# Development diagnostics stay on (crash reports, a PDB: the link evidence,
# tools/r7_symbols.py, reads it). The development builds are left as they are.
#   tools/r7_release_build.sh          configure and build
#   tools/r7_release_build.sh build    build only
set -e
cd "$(dirname "$0")/.."
R=build/r7/release
DEPS="$(pwd)/build/deps/windows"
mkdir -p $R
if [ "$1" != build ]; then
  cmake -S port -B $R/bld -G Ninja -DCMAKE_TOOLCHAIN_FILE="$(pwd)/build/toolchain/cmake-windows/toolchain.cmake" \
    -DDOTHEROES_DEV_DIAGNOSTICS=ON -DPS3RECOMP_RSX_VULKAN=ON -DDOTHEROES_NATIVE_RELEASE=ON \
    -DPS3RECOMP_VULKAN_INCLUDE_DIR="$DEPS/include" -DCMAKE_FIND_ROOT_PATH="$DEPS" -DCMAKE_PREFIX_PATH="$DEPS" \
    -DRECOMP_DIR="$(pwd)/build/r7/native/gen" > $R/configure.log 2>&1
  grep -i "native only\|Graphics:" $R/configure.log || true
fi
cmake --build $R/bld --target dotheroes > $R/build.log 2>&1
cp build/port-win/SDL2.dll $R/bld/ 2>/dev/null || true
echo "built $R/bld/dotheroes.exe"

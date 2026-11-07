#!/bin/sh
# R8's native release builds (docs/NATIVE_R8.md): R7's release configuration
# (tools/r7_release_build.sh: R7's lift in build/r7/native/gen, no RSX command
# path, no bridge) in a build directory of its own, with extra CMake options,
# so variants (an optimisation level, say) build side by side.
#   tools/r8_release_build.sh <dir> [-DNAME=value ...]    configure and build into <dir>/bld
#   BUILD_ONLY=1 tools/r8_release_build.sh <dir>          build only
# e.g. tools/r8_release_build.sh build/r8/o2 -DDOTHEROES_PPU_OPT=-O2
set -e
cd "$(dirname "$0")/.."
R=$1; shift
[ -n "$R" ] || { echo "usage: $0 <dir> [-DNAME=value ...]"; exit 2; }
DEPS="$(pwd)/build/deps/windows"
mkdir -p "$R"
if [ -z "$BUILD_ONLY" ]; then
  cmake -S port -B "$R/bld" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$(pwd)/build/toolchain/cmake-windows/toolchain.cmake" \
    -DDOTHEROES_DEV_DIAGNOSTICS=ON -DPS3RECOMP_RSX_VULKAN=ON -DDOTHEROES_NATIVE_RELEASE=ON \
    -DPS3RECOMP_VULKAN_INCLUDE_DIR="$DEPS/include" -DCMAKE_FIND_ROOT_PATH="$DEPS" -DCMAKE_PREFIX_PATH="$DEPS" \
    -DRECOMP_DIR="$(pwd)/build/r7/native/gen" "$@" > "$R/configure.log" 2>&1
  grep -i "native only\|Graphics:\|Lifted PPU code" "$R/configure.log" || true
fi
start=$(date +%s)
cmake --build "$R/bld" --target dotheroes > "$R/build.log" 2>&1
cp build/port-win/SDL2.dll "$R/bld/" 2>/dev/null || true
echo "built $R/bld/dotheroes.exe in $(( $(date +%s) - start )) s"

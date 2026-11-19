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
# the platform: Windows (the default, as before) or, on a Linux host, Linux (M11.4); R8_PLATFORM=windows on a Linux
# host cross-builds Windows' program (a compile check: it runs on Windows only)
case "$(uname -s)" in Linux) PLAT=linux ;; *) PLAT=windows ;; esac
HOST_PLAT=$PLAT
PLAT=${R8_PLATFORM:-$PLAT}
DEPS="$(pwd)/build/deps/$PLAT"
PY=build/venv/Scripts/python.exe
[ -x "$PY" ] || PY=python3
TC="$(pwd)/build/toolchain/cmake-$PLAT/toolchain.cmake"
[ "$PLAT" = windows ] && [ "$HOST_PLAT" = windows ] || TC=$("$PY" -c "import sys; sys.path.insert(0, 'tools'); import zig_build; print(zig_build.cmake_toolchain('$PLAT'))")
mkdir -p "$R"
if [ -z "$BUILD_ONLY" ]; then
  cmake -S port -B "$R/bld" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$TC" \
    -DDOTHEROES_DEV_DIAGNOSTICS=ON -DPS3RECOMP_RSX_VULKAN=ON -DDOTHEROES_NATIVE_RELEASE=ON \
    -DPS3RECOMP_VULKAN_INCLUDE_DIR="$DEPS/include" -DCMAKE_FIND_ROOT_PATH="$DEPS" -DCMAKE_PREFIX_PATH="$DEPS" \
    -DRECOMP_DIR="$(pwd)/build/r7/native/gen" "$@" > "$R/configure.log" 2>&1
  grep -i "native only\|Graphics:\|Lifted PPU code" "$R/configure.log" || true
fi
start=$(date +%s)
cmake --build "$R/bld" --target dotheroes > "$R/build.log" 2>&1
if [ "$PLAT" = linux ]; then   # the game loads its libraries from <its folder>/lib (rpath $ORIGIN/lib, dlopen)
  mkdir -p "$R/bld/lib"
  for l in libSDL2-2.0.so.0 libopenal.so.1 libavutil.so.61 libavcodec.so.63 libavformat.so.63; do
    cp -L "$DEPS/lib/$l" "$R/bld/lib/"
  done
  echo "built $R/bld/dotheroes in $(( $(date +%s) - start )) s"
  exit 0
fi
cp build/port-win/SDL2.dll "$R/bld/" 2>/dev/null || true
cp build/deps/windows/bin/OpenAL32.dll "$R/bld/" 2>/dev/null || true
cp build/deps/windows/bin/avutil-61.dll build/deps/windows/bin/avcodec-63.dll build/deps/windows/bin/avformat-63.dll \
   "$R/bld/" 2>/dev/null || true   # FFmpeg (M8): the movies, loaded at run time from the game's folder
echo "built $R/bld/dotheroes.exe in $(( $(date +%s) - start )) s"

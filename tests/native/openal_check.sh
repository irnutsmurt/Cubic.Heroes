#!/bin/sh
# M2's standalone tests of the OpenAL Soft layer (port/native/audio; tests/native/openal_check.cpp):
#   tests/native/openal_check.sh <the player's sound folder> [hour]
# builds the checker with the bundled Zig against build/deps/<platform> (tools/build_deps.py), then runs: the
# made-up cases (loop forms, crossfade, positional stereo and attenuation, reverb, the command queue, the
# lifecycle and leaks); every sound file loaded; every looped file played and compared sample by sample; a
# minute streamed in real time with a device loss half way. With `hour`: an hour streamed in real time.
# Real-time runs use OpenAL Soft's wave writer into the null device (DEVICE_TEST=real: the default output).
set -e
cd "$(dirname "$0")/../.."
SND=${1:?usage: $0 <sound folder> [hour]}
OUT=${OUT:-build/m2}
mkdir -p "$OUT"
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) P=windows; Z=build/toolchain/zig-x86_64-windows-0.17.0/zig.exe; T=x86_64-windows-gnu
       EXE=$OUT/openal_check.exe; NUL=NUL
       cmp -s build/deps/windows/bin/OpenAL32.dll "$OUT/OpenAL32.dll" || cp build/deps/windows/bin/OpenAL32.dll "$OUT/" ;;
  *)   P=linux; Z=$(ls -d build/toolchain/zig-x86_64-linux-*/zig | head -1); T=x86_64-linux-gnu.2.31
       EXE=$OUT/openal_check; mkdir -p "$OUT/lib"; cp build/deps/linux/lib/libopenal.so.1 build/deps/linux/lib/libSDL2-2.0.so.0 "$OUT/lib/"
       NUL=/dev/null ;;
esac
"$Z" c++ -target "$T" -O2 -std=c++17 -Wall -Wextra -I port/native/audio -I "build/deps/$P/include" \
  port/native/audio/wav_pcm.cpp port/native/audio/al_api.cpp port/native/audio/al_device.cpp \
  port/native/audio/al_stream.cpp port/native/audio/voice_pool.cpp tests/native/openal_check.cpp -o "$EXE"
printf '[general]\ndrivers = wave\n\n[wave]\nfile = %s\n' "$NUL" > "$OUT/wave_null.ini"
[ "$DEVICE_TEST" = real ] || export ALSOFT_CONF="$OUT/wave_null.ini"
set +e
rc=0
run() {   # <lines to show> <args...>: the full output in $OUT/<first arg>.out
  n=$1; shift
  "$EXE" "$@" > "$OUT/$1.out" 2>&1 || rc=1
  tail -n "$n" "$OUT/$1.out"
}
run 3 synth
run 4 files "$SND"
run 1 loops "$SND"
run 5 device "$SND/bgm_cave.wav" 60 reopen
[ "$2" = hour ] && run 5 device "$SND/bgm_field07.wav" 3600 reopen
echo "openal_check: $([ $rc = 0 ] && echo passed || echo FAILED)"
exit $rc

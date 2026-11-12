#!/bin/sh
# The standalone tests of the game's audio on OpenAL Soft (port/native/audio), no game running:
#   tests/native/openal_check.sh <the player's sound folder> [hour]
# builds, with the bundled Zig against build/deps/<platform> (tools/build_deps.py):
#   openal_check (tests/native/openal_check.cpp, M2): the made-up cases (loop forms, crossfade, positional stereo
#     and attenuation, reverb, the command queue, the lifecycle and leaks, the voice pool); every sound file
#     loaded; every looped file played and compared sample by sample; a minute streamed in real time with a
#     device loss half way; with `hour`, an hour streamed in real time;
#   sound_manager_check (tests/native/sound_manager_check.cpp, M3): the native sound manager's behaviour on
#     made-up sounds (records, fades, ducks, music, pause).
# Real-time runs use OpenAL Soft's wave writer into the null device (DEVICE_TEST=real: the default output).
# M3's replay against rel27's traces is tests/native/sound_replay.sh (it needs the developer's own captures).
set -e
cd "$(dirname "$0")/../.."
SND=${1:?usage: $0 <sound folder> [hour]}
OUT=${OUT:-build/m2}
mkdir -p "$OUT"
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) P=windows; Z=build/toolchain/zig-x86_64-windows-0.17.0/zig.exe; T=x86_64-windows-gnu
       X=.exe; NUL=NUL
       cmp -s build/deps/windows/bin/OpenAL32.dll "$OUT/OpenAL32.dll" || cp build/deps/windows/bin/OpenAL32.dll "$OUT/" ;;
  *)   P=linux; Z=$(ls -d build/toolchain/zig-x86_64-linux-*/zig | head -1); T=x86_64-linux-gnu.2.31
       X=; mkdir -p "$OUT/lib"; cp build/deps/linux/lib/libopenal.so.1 build/deps/linux/lib/libSDL2-2.0.so.0 "$OUT/lib/"
       NUL=/dev/null ;;
esac
A=port/native/audio
LAYER="$A/wav_pcm.cpp $A/al_api.cpp $A/al_device.cpp $A/al_stream.cpp $A/voice_pool.cpp"
"$Z" c++ -target "$T" -O2 -std=c++17 -Wall -Wextra -I $A -I "build/deps/$P/include" \
  $LAYER tests/native/openal_check.cpp -o "$OUT/openal_check$X"
"$Z" c++ -target "$T" -O2 -std=c++17 -Wall -Wextra -Wno-unused-result -I $A -I "build/deps/$P/include" \
  $LAYER $A/sound_list.cpp $A/sound_manager.cpp tests/native/sound_manager_check.cpp -o "$OUT/sound_manager_check$X"
printf '[general]\ndrivers = wave\n\n[wave]\nfile = %s\n' "$NUL" > "$OUT/wave_null.ini"
[ "$DEVICE_TEST" = real ] || export ALSOFT_CONF="$OUT/wave_null.ini"
set +e
rc=0
run() {   # <lines to show> <args...>: the full output in $OUT/<first arg>.out
  n=$1; shift
  "$OUT/openal_check$X" "$@" > "$OUT/$1.out" 2>&1 || rc=1
  tail -n "$n" "$OUT/$1.out"
}
run 3 synth
run 4 files "$SND"
run 1 loops "$SND"
run 5 device "$SND/bgm_cave.wav" 60 reopen
[ "$2" = hour ] && run 5 device "$SND/bgm_field07.wav" 3600 reopen
"$OUT/sound_manager_check$X" > "$OUT/sound_manager_check.out" 2>&1 || rc=1
tail -n 1 "$OUT/sound_manager_check.out"
echo "openal_check: $([ $rc = 0 ] && echo passed || echo FAILED)"
exit $rc

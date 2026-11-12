#!/bin/sh
# M3's replay gate (tests/native/sound_replay.cpp): every audio trace in a folder -- the developer's own M0
# captures of rel27 (DOTHEROES_AUDIO_TRACE; kept locally, never in the repository) -- given to the native sound
# manager and compared with rel27's answers, volumes and music starts.
#   tests/native/sound_replay.sh <trace folder> <the player's sound folder>
set -e
cd "$(dirname "$0")/../.."
TR=${1:?usage: $0 <trace folder> <sound folder>}
SND=${2:?usage: $0 <trace folder> <sound folder>}
OUT=${OUT:-build/m3}
mkdir -p "$OUT"
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) P=windows; Z=build/toolchain/zig-x86_64-windows-0.17.0/zig.exe; T=x86_64-windows-gnu
       EXE=$OUT/sound_replay.exe
       cmp -s build/deps/windows/bin/OpenAL32.dll "$OUT/OpenAL32.dll" || cp build/deps/windows/bin/OpenAL32.dll "$OUT/" ;;
  *)   P=linux; Z=$(ls -d build/toolchain/zig-x86_64-linux-*/zig | head -1); T=x86_64-linux-gnu.2.31
       EXE=$OUT/sound_replay; mkdir -p "$OUT/lib"; cp build/deps/linux/lib/libopenal.so.1 build/deps/linux/lib/libSDL2-2.0.so.0 "$OUT/lib/" ;;
esac
A=port/native/audio
"$Z" c++ -target "$T" -O2 -std=c++17 -Wall -Wextra -I $A -I tests/native -I "build/deps/$P/include" \
  $A/wav_pcm.cpp $A/al_api.cpp $A/al_device.cpp $A/al_stream.cpp $A/voice_pool.cpp $A/sound_list.cpp \
  $A/sound_manager.cpp tests/native/sound_replay.cpp -o "$EXE"
set +e
rc=0
for t in "$TR"/*.trace; do
  b=$(basename "$t" .trace)
  "$EXE" "$t" "$SND" --report "$OUT/$b.diff" > "$OUT/$b.out" 2>/dev/null || rc=1
  head -2 "$OUT/$b.out" | cut -c1-260
done
echo "sound_replay: $([ $rc = 0 ] && echo passed || echo DIFFERENCES)"
exit $rc

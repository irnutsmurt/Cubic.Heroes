#!/bin/sh
# M8.1's standalone test (docs/FFMPEG_MIGRATION_PLAN.md section 6), no game running:
#   tests/native/ffmpeg_decode_check.sh <the player's movie folder>
# builds tests/native/ffmpeg_decode_check.cpp with port/native/movie/ff_api.cpp (FFmpeg loaded at run time by full
# path, as the game will) with the bundled Zig against build/deps/<platform> (tools/build_deps.py), puts FFmpeg's
# libraries where the game has them (beside the program; Linux: its lib/), and decodes every frame and every
# sample of the five movies; then (M8.2) tests/native/movie_player_check.cpp checks the native movie player's
# decoding against those frames (in order, every third frame shown, a restart) and its sound's total. The movies
# are the player's own (their disc's data/movie); nothing is written from them but the frames' hashes and the
# summary, in $OUT (build/m8).
set -e
cd "$(dirname "$0")/../.."
MOV=${1:?usage: $0 <movie folder>}
OUT=${OUT:-build/m8}
mkdir -p "$OUT"
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) P=windows; Z=build/toolchain/zig-x86_64-windows-0.17.0/zig.exe; T=x86_64-windows-gnu
       X=.exe; LIB="$OUT"
       for f in avutil-61.dll avcodec-63.dll avformat-63.dll; do
         cmp -s "build/deps/windows/bin/$f" "$LIB/$f" || cp "build/deps/windows/bin/$f" "$LIB/"
       done ;;
  *)   P=linux; Z=$(ls -d build/toolchain/zig-x86_64-linux-*/zig | head -1); T=x86_64-linux-gnu.2.31
       X=; LIB="$OUT/lib"; mkdir -p "$LIB"
       cp build/deps/linux/lib/libavutil.so.61 build/deps/linux/lib/libavcodec.so.63 \
          build/deps/linux/lib/libavformat.so.63 "$LIB/" ;;
esac
M=port/native/movie
"$Z" c++ -target "$T" -O2 -std=c++17 -Wall -Wextra -I $M -I "build/deps/$P/include" \
  $M/ff_api.cpp tests/native/ffmpeg_decode_check.cpp -o "$OUT/ffmpeg_decode_check$X"
# M8.2: the native player's decoding against the frames' hashes the plain decode writes
"$Z" c++ -target "$T" -O2 -std=c++17 -Wall -Wextra -I $M -I port/native/audio -I "build/deps/$P/include" \
  $M/ff_api.cpp $M/movie_player.cpp tests/native/movie_player_check.cpp -o "$OUT/movie_player_check$X"
rc=0
"$OUT/ffmpeg_decode_check$X" "$MOV" "$OUT" || rc=1
"$OUT/movie_player_check$X" "$MOV" "$OUT" || rc=1
exit $rc

#!/bin/sh
# Step A3's checker batch (docs/AUDIO.md): the mixer's traces (TRACES, default F:/3d dot game heroes/scratch/a3trace:
# SPU_TRACE_IMAGE=6 captures, not in the repository) replayed by the lifted mixer and the port, at both -mcpu
# levels, and synthesized cases. Results in OUT/batch_*.txt, block counts in OUT/cov_*.txt
# (tools/fmod_mixer_coverage.py merges them). Build first: tools/fmod_mixer_check.sh [x86_64_v2].
cd "$(dirname "$0")/.." || exit 1
OUT=${OUT:-"F:/3d dot game heroes/scratch/mixer/out"}
TR=${TRACES:-"F:/3d dot game heroes/scratch/a3trace"}
run() {   # name exe trace args...
    name=$1; exe=$2; trace=$3; shift 3
    MIXER_COVERAGE_FILE="$OUT/cov_$name.txt" "$OUT/$exe" "$TR/$trace" "$@" > "$OUT/batch_$name.txt" 2>&1
    echo "$name: exit $? ($(grep -c '^\[PASS\]' "$OUT/batch_$name.txt") passed, $(grep -c '^\[FAIL\]' "$OUT/batch_$name.txt") failed)"
}
for t in ${TRACE_NAMES:-river dungeon}; do
    run trace_$t fmod_mixer_check_v3.exe mixer_$t.bin
    run ${t}_v3 fmod_mixer_check_v3.exe mixer_$t.bin --fuzz 20000 --seed 7
    run ${t}_v2 fmod_mixer_check_v2.exe mixer_$t.bin --fuzz 20000 --seed 3
done
# the long traces (8.6 GB each: the dungeon run lifted, and the native mixer's own run), replayed at both levels
for t in ${LONG_TRACE_NAMES-dungeonlong native_dungeon}; do
    run trace_${t}_v3 fmod_mixer_check_v3.exe mixer_$t.bin
    run trace_${t}_v2 fmod_mixer_check_v2.exe mixer_$t.bin
done
echo "batch done"

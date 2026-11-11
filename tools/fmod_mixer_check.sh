#!/bin/sh
# Step A3's off-line check (docs/AUDIO.md): fmod_mixer.cpp regenerated from the lift, then the checker built at
# one -mcpu level (default x86_64_v3). OUT (default F:/3d dot game heroes/scratch/mixer/out) takes the objects.
#   sh tools/fmod_mixer_check.sh [x86_64_v3|x86_64_v2]
set -e
cd "$(dirname "$0")/.."
CPU=${1:-x86_64_v3}
SFX=$(echo "$CPU" | sed 's/x86_64_//')
OUT=${OUT:-"F:/3d dot game heroes/scratch/mixer/out"}
ZIG=build/toolchain/zig-x86_64-windows-0.17.0/zig.exe
LIFT=build/gen/spu/spu_0005_at_00F86A80/spu_recomp.c
ELF=build/gen/spu/images/spu_0005_at_00F86A80.elf
PY=build/venv/Scripts/python.exe
mkdir -p "$OUT"
"$PY" tools/gen_spu_native.py tools/fmod_mixer_config.py "$LIFT" "$ELF" tools/fmod_mixer.cpp.in \
    port/native/fmod_mixer.cpp --report "$OUT/gen_report.txt"
if [ ! -f "$OUT/mixer_lift_$SFX.o" ]; then
    "$ZIG" cc -target x86_64-windows-gnu -mcpu=$CPU -O2 -g -c -I build/ref/ps3recomp/runtime/spu \
        -I build/ref/ps3recomp/include "$LIFT" -o "$OUT/mixer_lift_$SFX.o"
fi
# COVERAGE=0: the port without its block counters (as the game builds it), for --profile: fmod_mixer_prof_*.exe
if [ "${COVERAGE:-1}" = 0 ]; then DEF=""; EXE="$OUT/fmod_mixer_prof_$SFX.exe"; else DEF="-DFMOD_MIXER_COVERAGE"; EXE="$OUT/fmod_mixer_check_$SFX.exe"; fi
"$ZIG" c++ -target x86_64-windows-gnu -mcpu=$CPU -O2 -g -std=c++17 $DEF \
    -I port/native -I build/ref/ps3recomp/runtime/spu -I build/ref/ps3recomp/include \
    tests/native/fmod_mixer_check.cpp port/native/fmod_mixer.cpp port/native/fmod_codec_raw.cpp \
    port/native/fmod_sfxreverb.cpp "$OUT/mixer_lift_$SFX.o" -Wl,--stack,67108864 -o "$EXE"
echo "built $EXE"

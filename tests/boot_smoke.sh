#!/usr/bin/env bash
# Boot smoke test: run the port headless, isolated from the desktop, in strict
# mode, and summarize how far it got.
#
#   tests/boot_smoke.sh [seconds]        (default 90)
#
# Environment
#   SHOT_EVERY=<sec>   save a screenshot of the last presented frame every <sec>
#                      seconds to <run>/shots/NNNN.png (Vulkan builds: the
#                      backend's PS3RECOMP_VK_DUMP frame). Default 10; 0 = off.
#   PAD_SCRIPT=...     scripted input (libs/input/cellPad.c); passed through.
#   RUN_TAG=<name>     suffix for the run directory.
#   KEEP_RUNS=<n>      run directories kept under build/test (default 5; the
#                      oldest are deleted when a run starts). The live frame
#                      dump (frame.ppm) is deleted when a run ends.
#   PS3RECOMP_VK_DUMP_EVERY=<n>  refresh frame.ppm every n-th present (default
#                      15; dumping every frame costs several fps).
#   DRIVE=<script>     run tests/drive.py with this script against the game
#                      (closed-loop input; see tests/scripts/).
#
# Input: <run>/pad.txt is the game's PAD_FILE. Writing a button mask there
# presses it (e.g. `echo 0x0008 > pad.txt` = START), so a driver outside the
# process can look at the screenshots and decide the next button.
#
# Isolation: its own cwd/save root under build/test/, SDL dummy audio (nothing
# reaches the speakers), no display (DISPLAY/WAYLAND_DISPLAY unset, rendering
# offscreen), and only the PID it started is ever signalled.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SECS="${1:-90}"
BIN="$ROOT/build/port/dotheroes"
ELF="$ROOT/build/disc/PS3_GAME/USRDIR/EBOOT.elf"
RUN="$ROOT/build/test/boot-$(date +%Y%m%d-%H%M%S)${RUN_TAG:+-$RUN_TAG}"
# Retention: test runs (logs, screenshots) must not pile up on disk.
KEEP_RUNS="${KEEP_RUNS:-5}"
# (newline-delimited: the project path may contain spaces)
ls -d "$ROOT"/build/test/boot-* 2>/dev/null | sort | head -n -"$((KEEP_RUNS > 1 ? KEEP_RUNS - 1 : 0))" | xargs -r -d '\n' rm -rf --
SHOT_EVERY="${SHOT_EVERY:-10}"
mkdir -p "$RUN/dev_flash" "$RUN/shots"
: > "$RUN/pad.txt"
[ -x "$BIN" ] || { echo "no $BIN (build port/ first)"; exit 2; }
DRIVE_ABS=; [ -n "${DRIVE:-}" ] && DRIVE_ABS="$(realpath "$DRIVE")"

cd "$RUN"
# Our crash reports carry the context; a kernel core of a 4 GB guest address
# space takes systemd-coredump tens of seconds. DIAG_CORE=1 keeps it.
[ "${DIAG_CORE:-0}" = 1 ] || ulimit -c 0
env -u DISPLAY -u WAYLAND_DISPLAY \
    PS3_STRICT="${PS3_STRICT:-1}" \
    PS3_GAMEDATA_ROOT="$RUN/gamedata" \
    PS3_HDD0_ROOT="$RUN/gamedata/dev_hdd0" \
    PS3_HDD1_ROOT="$RUN/gamedata/dev_hdd1" \
    PS3_DEV_FLASH="$RUN/dev_flash" \
    PS3RECOMP_VK_GUEST_PROGRAMS="${PS3RECOMP_VK_GUEST_PROGRAMS:-1}" \
    PS3RECOMP_VK_DUMP="$RUN/frame.ppm" \
    PS3RECOMP_VK_DUMP_EVERY="${PS3RECOMP_VK_DUMP_EVERY:-15}" \
    PAD_FILE="$RUN/pad.txt" \
    SDL_AUDIODRIVER=dummy \
    DIAG_CRASH_DIR="$RUN/diag" DIAG_HANG_EXIT=1 DIAG_BOOT_SECS="${DIAG_BOOT_SECS:-60}" \
    "$BIN" "$ELF" > "$RUN/stdout.log" 2> "$RUN/stderr.log" &
PID=$!
( sleep "$SECS"; kill -TERM "$PID" 2>/dev/null; sleep 5; kill -KILL "$PID" 2>/dev/null ) >/dev/null 2>&1 &
WD=$!
SH=
if [ "$SHOT_EVERY" != 0 ]; then
    ( n=0; while kill -0 "$PID" 2>/dev/null; do
          sleep "$SHOT_EVERY"; n=$((n + SHOT_EVERY))
          # Copy first: the backend overwrites the dump on every present.
          [ -s frame.ppm ] && cp frame.ppm shots/.tmp.ppm 2>/dev/null &&
              timeout 10 pnmtopng shots/.tmp.ppm > "shots/$(printf %04d $n).png" 2>/dev/null
      done; rm -f shots/.tmp.ppm ) >/dev/null 2>&1 &
    SH=$!
fi
DR=
if [ -n "${DRIVE:-}" ]; then
    python3 "$ROOT/tests/drive.py" "$RUN" "$DRIVE_ABS" > "$RUN/drive.log" 2>&1 &
    DR=$!
fi
wait "$PID"; RC=$?
touch "$RUN/.done"
rm -f "$RUN/frame.ppm" "$RUN/shots/.tmp.ppm"
[ -n "$DR" ] && { sleep 1; kill "$DR" 2>/dev/null; }
pkill -P "$WD" 2>/dev/null; kill "$WD" 2>/dev/null
[ -n "$SH" ] && { pkill -P "$SH" 2>/dev/null; kill "$SH" 2>/dev/null; }

echo "run dir : ${RUN#$ROOT/}"
echo "exit    : $RC$( [ $RC -gt 128 ] && echo " (signal $((RC-128)))")"
echo "strict  : $(grep -m1 '^\[strict\] FATAL' stderr.log || echo none)"
echo "hle     : $(grep -c 'UNRESOLVED NID' stderr.log) unresolved NID(s) reported"
echo "flips   : $(grep -oE 'frames presented \(guest flips\): [0-9]+' stderr.log diag/* 2>/dev/null | tail -1)"
echo "reports : $(ls diag 2>/dev/null | tr '\n' ' ')"
echo "shots   : $(ls shots 2>/dev/null | wc -l)"
[ -n "$DR" ] && echo "drive   : $(tail -1 drive.log)"
echo "--- last stderr lines"
tail -25 stderr.log

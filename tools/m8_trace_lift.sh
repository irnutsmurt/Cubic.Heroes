#!/bin/sh
# M8.0's trace lift (docs/FFMPEG_MIGRATION_PLAN.md section 2): R8's release lift (tools/r8_lift.sh) with the movie
# trace's observers (config/m8_movie_trace.json) beside the native renderer's (config/r7_native_hooks.json), merged
# into one file for the lifter, and the credits on demand (config/m8_dev_credits.json: an entry hook, inert unless
# DOTHEROES_DEV_CREDITS), into build/r8/gen15m (M8_GEN). A development lift only: build a release from it with
#   tools/r8_release_build.sh <dir> -DRECOMP_DIR="$(pwd)/build/r8/gen15m" [...]
# and run it with DOTHEROES_MOVIE_TRACE=<file> (port/native/movie/movie_trace.cpp).
set -e
cd "$(dirname "$0")/.."
O=${M8_GEN:-build/r8/gen15m}
PY=build/venv/Scripts/python.exe
[ -x "$PY" ] || PY=python3
mkdir -p "$O" build/m8
OBS=build/m8/trace_observers.json
"$PY" - config/r7_native_hooks.json config/m8_movie_trace.json "$OBS" <<'EOF'
import json, sys
base = json.load(open(sys.argv[1]))
movie = json.load(open(sys.argv[2]))
seen = {int(o["addr"], 0) for o in base["observers"]}
extra = [o for o in movie["observers"] if int(o["addr"], 0) not in seen]
out = {"_comment": "merged by tools/m8_trace_lift.sh", "store_probes": base.get("store_probes", True),
       "observers": base["observers"] + extra}
json.dump(out, open(sys.argv[3], "w"), indent=1)
print("%d observers (%d of the movie trace's)" % (len(out["observers"]), len(extra)))
EOF
start=$(date +%s)
"$PY" build/ref/ps3recomp/tools/ppu_lifter.py build/EBOOT.elf \
  --functions build/gen/out/EBOOT.functions.fixed.json --hle-stubs build/gen/out/EBOOT.imports.json \
  --sjlj config/sjlj.json --insn-patches config/insn_patches.json --insn-patches config/r8_hooks.json --insn-patches config/audio_hooks.json \
  --insn-patches config/m8_dev_credits.json \
  --entry-observers "$OBS" --traps config/excluded_code.json --chunks 48 --promote-leaf \
  -o "$O/" > "$O.lift.log" 2>&1
grep -c "entry observer" "$O"/*.cpp | awk -F: '{n+=$2} END{print n " observed functions in the lift"}'
echo "lifted into $O in $(( $(date +%s) - start )) s"

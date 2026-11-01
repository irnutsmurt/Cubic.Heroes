#!/bin/bash
# The runtime's GCM FIFO unit tests (build/ref/ps3recomp/libs/video/tests/test_gcm_*.c) on Windows,
# with the bundled Zig. Each test #includes cellGcmSys.c and stubs the rest; two things keep them from
# building or running here as written, worked around in build/native-test/gcm (not in the runtime):
#  - they allocate guest memory up to the inject base + 64 KB, but cellGcmInit now clears the RSX report
#    area 8 MB past it (a write past their allocation on any platform): vm_shim.h gives that one
#    allocation the whole 16 MB inject window;
#  - the COFF linker resolves every symbol before it drops unused code: two display-buffer stubs.
# Not built here: test_gcm_flip_status and test_gcm_user_command (stubs they lack), and
# test_gcm_legacy_state_filter (POSIX setenv). test_gcm_walker_prof is a profiling tool, not a test.
#   tests/native/run_gcm_tests.sh [test name ...]
cd "$(dirname "$0")/../.." || exit 2
ROOT=$(pwd)
Z="$ROOT/build/toolchain/zig-x86_64-windows-0.17.0/zig.exe"
O="$ROOT/build/native-test/gcm"
mkdir -p "$O"
cat > "$O/vm_shim.h" <<'H'
#include <stdlib.h>
static inline void* vm_shim_calloc(size_t n, size_t s)
{
    size_t t = n * s;
    if (t >= 0x20000000u && t < 0x21000000u) t = 0x21000000u;   /* the guest space: the whole inject window */
    return calloc(1, t);
}
#define calloc(n, s) vm_shim_calloc((n), (s))
H
cat > "$O/stub_draw.c" <<'C'
#include <stdint.h>
void rsx_draw_engine_set_display_buffer(uint32_t i, uint32_t l, uint32_t o, uint32_t p, uint32_t w, uint32_t h)
{ (void)i; (void)l; (void)o; (void)p; (void)w; (void)h; }
C
cat > "$O/stub_live.c" <<'C'
#include <stdint.h>
void rsx_live_draw_set_display_buffer(uint32_t i, uint32_t l, uint32_t o, uint32_t p, uint32_t w, uint32_t h)
{ (void)i; (void)l; (void)o; (void)p; (void)w; (void)h; }
C
cd build/ref/ps3recomp || exit 2
tests=("$@")
[ ${#tests[@]} -eq 0 ] && tests=(test_gcm_fifo_user_command test_gcm_flip_stream test_gcm_flip_wrap test_gcm_get_on_jump
  test_gcm_get_on_release test_gcm_nop_runs test_gcm_park_reuse test_gcm_resync_labels test_gcm_wrap_lap)
fail=0
for t in "${tests[@]}"; do
  src=libs/video/tests/$t.c
  stubs=()   # a test that defines a stub itself must not get ours too
  grep -q "rsx_draw_engine_set_display_buffer" "$src" || stubs+=("$O/stub_draw.c")
  grep -q "rsx_live_draw_set_display_buffer" "$src" || stubs+=("$O/stub_live.c")
  rm -f "$O/$t.exe"
  "$Z" cc -std=gnu17 -target x86_64-windows-gnu -I include -I libs/video -w -include "$O/vm_shim.h" \
      "$src" "${stubs[@]}" -o "$O/$t.exe" > "$O/$t.build.txt" 2>&1
  if [ ! -f "$O/$t.exe" ]; then echo "$t: BUILD FAILED (see $O/$t.build.txt)"; fail=1; continue; fi
  out=$(timeout 120 "$O/$t.exe" 2>&1); rc=$?
  echo "$t: $(echo "$out" | tail -1) (exit $rc)"
  [ $rc -ne 0 ] && fail=1
done
exit $fail

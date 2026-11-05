#!/usr/bin/env python3
"""R4's audit of an attributed frame trace (docs/NATIVE_R4.md): which code
wrote the command stream, with PPFX's calls apart.

A trace made with PS3_ATTRIB_SHADOW=1 (and PS3_ATTRIB_ST over the command
buffer) names, for every word, the guest function that stored it and that
function's caller (lr). For each writer: its R4 class (the recorder's
r4_class: bypassed, kept, or a function that writes no command of PPFX's),
the words it stored when PPFX called it (lr in PPFX's code), when another
libgcm function did (a wrapper; whose call it was the trace cannot tell) and
when other code did -- and the stream markers. With R4 on, a bypassed
writer's words from PPFX's calls are 0, and PPFX's draws are gone: the
markers stand where they were.

  tools/r4_trace_audit.py <frametrace.txt>
"""
import collections
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0] if "/" in __file__ else ".")
from r4_callsites import BYPASSED  # noqa: E402

KEPT = {0x0069F944, 0x006A674C, 0x006A5DA0, 0x006A5E30, 0x006A39EC, 0x006A4E50, 0x006A8020, 0x006A817C,
        0x006A820C, 0x006A829C, 0x006A1D1C}
PPFX = (0x007497E0, 0x007D3E38)
GCM = (0x0069C000, 0x006B0000)
MAGIC = "52344D4B"


def main():
    path = sys.argv[1]
    words = collections.Counter()
    frames = 0
    markers = 0
    draws = collections.Counter()
    for ln in open(path, encoding="utf-8", errors="replace"):
        if ln.startswith("#"):
            continue
        f = ln.split()
        if len(f) < 3:
            continue
        if f[2] == "FLIP":
            frames += 1
            continue
        if len(f) < 5:
            continue
        try:
            fn = int(f[2], 16)
        except ValueError:
            continue
        lr = int(f[4][3:], 16) if f[4].startswith("lr=") else 0
        who = "ppfx" if PPFX[0] <= lr < PPFX[1] else "libgcm" if GCM[0] <= lr < GCM[1] else "other"
        n = 1
        if len(f) >= 9 and f[5] == "M":
            n = 1 + int(f[8])
            if f[7] == "0100" and len(f) > 9 and f[9] == MAGIC:
                markers += 1
            if f[7] in ("1808", "1814", "1818", "181C"):   # BEGIN_END, DRAW_ARRAYS, DRAW_INDEX, INDEX ARRAY
                draws[(fn, who)] += 1
        words[(fn, who)] += n
    fns = sorted({k[0] for k in words})
    print("# r4_trace_audit %s: %d frames; command words by writer and by whose call (PPFX's: lr in PPFX's code)"
          % (path, frames))
    print("# writer   class      words_ppfx_calls  words_libgcm_calls  words_other  draw_words(ppfx/other)")
    tot = collections.Counter()
    for fn in fns:
        cls = "bypassed" if fn in BYPASSED else "kept" if fn in KEPT else ("libgcm" if GCM[0] <= fn < GCM[1] else
                                                                           "ppfx" if PPFX[0] <= fn < PPFX[1] else "-")
        a, b, c = words[(fn, "ppfx")], words[(fn, "libgcm")], words[(fn, "other")]
        tot[(cls, "ppfx")] += a
        tot[(cls, "libgcm")] += b
        tot[(cls, "other")] += c
        if a + b + c:
            print("%08X  %-9s  %10d  %10d  %10d  %d/%d" % (fn, cls, a, b, c, draws[(fn, "ppfx")],
                                                         draws[(fn, "libgcm")] + draws[(fn, "other")]))
    print("# totals by class (words from PPFX's calls / libgcm's / other code's):")
    for cls in ("bypassed", "kept", "libgcm", "ppfx", "-"):
        print("#   %-9s %d / %d / %d" % (cls, tot[(cls, "ppfx")], tot[(cls, "libgcm")], tot[(cls, "other")]))
    print("# stream markers: %d (%.1f a frame)" % (markers, markers / max(frames, 1)))


if __name__ == "__main__":
    main()

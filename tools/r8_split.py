#!/usr/bin/env python3
"""R8: the main PPU thread's profile in two sets of frames compared, in ms a
frame: the frames inside the given spans (e.g. heavy debris) against the
rest of the profiled window -- which functions (inclusive) grow.

  tools/r8_split.py <profile.raw> <spans file: T0-T1,...  (us, the frame trace's clock)> <ftrace.txt> [--top N]

Needs a PS3_FTRACE run (profile.times beside profile.raw; the flips, kind 9,
give each set's frame rate).
"""
import bisect, collections, os, sys
sys.argv, args = sys.argv[:1], sys.argv[1:]
raw, spans_f, trace = args[0], args[1], args[2]
top = int(args[args.index("--top") + 1]) if "--top" in args else 30
sys.argv = [sys.argv[0], raw]
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import importlib.util
spec = importlib.util.spec_from_file_location("r8t", os.path.join(here, "r8_threads.py"))
src = open(os.path.join(here, "r8_threads.py"), encoding="utf-8").read()
src = src.split("names, per = {}, collections.defaultdict(list)")[0]   # the symbolizer only
g = {"__name__": "r8t", "__file__": os.path.join(here, "r8_threads.py")}
exec(compile(src, "r8_threads.py", "exec"), g)
name = g["name"]
spans = sorted(tuple(float(x) for x in s.split("-")) for s in open(spans_f).read().split(","))
starts = [a for a, b in spans]
def inside(t):
    i = bisect.bisect_right(starts, t) - 1
    return i >= 0 and t <= spans[i][1]
when = [float(x) for x in open(os.path.join(os.path.dirname(os.path.abspath(raw)), "profile.times"))]
lines = open(raw, encoding="utf-8", errors="replace").read().splitlines()
names, main = {}, None
for l in lines[1:]:
    f = l.split()
    if f and f[0] == "thread":
        names[int(f[1])] = " ".join(f[2:])
main = next(t for t, n in names.items() if n == "ppu main")
sets = {True: collections.Counter(), False: collections.Counter()}
cnt = {True: 0, False: 0}
tmin, tmax, i = None, None, 0
for l in lines[1:]:
    f = l.split()
    if not f or f[0] in ("thread", "module"):
        continue
    t = when[i]; i += 1
    if int(f[0]) != main:
        continue
    tmin = t if tmin is None else min(tmin, t); tmax = t if tmax is None else max(tmax, t)
    k = inside(t)
    cnt[k] += 1
    for n in set(name(int(x, 16)) for x in f[1:]):
        sets[k][n] += 1
flips = []
for l in open(trace, encoding="utf-8", errors="replace"):
    x = l.split()
    if len(x) == 6 and x[0] == "9":
        flips.append(float(x[4]))
fl = [x for x in flips if tmin <= x <= tmax]
fh = sum(1 for x in fl if inside(x)); fo = len(fl) - fh
dur_h = sum(min(b, tmax) - max(a, tmin) for a, b in spans if b > tmin and a < tmax) / 1e6
dur_o = (tmax - tmin) / 1e6 - dur_h
fps_h, fps_o = fh / dur_h, fo / dur_o
print(f"in the spans: {dur_h:.2f} s, {fh} frames ({fps_h:.1f}/s, {1000 / fps_h:.2f} ms), {cnt[True]} samples; "
      f"outside: {dur_o:.2f} s, {fo} frames ({fps_o:.1f}/s, {1000 / fps_o:.2f} ms), {cnt[False]} samples")
def ms(k, n):   # the function's share of the set's wall time, in ms a frame of that set
    return n / cnt[k] * (1000 / (fps_h if k else fps_o))
rows = []
for n in set(sets[True]) | set(sets[False]):
    a, b = ms(True, sets[True][n]), ms(False, sets[False][n])
    rows.append((a - b, a, b, n))
print(f"{'grows':>7} {'in':>7} {'out':>7}  inclusive, ms a frame")
for d, a, b, n in sorted(rows, reverse=True)[:top]:
    print(f"{d:7.2f} {a:7.2f} {b:7.2f}  {n[:110]}")

#!/usr/bin/env python3
"""Symbolize a DIAG_PROF profile (port/diag): per thread, the functions most
samples were in (self) and under (inclusive).

  tests/prof_report.py <profile.raw> [--top N] [--min-share 2]
"""
import argparse, bisect, collections, subprocess

ap = argparse.ArgumentParser()
ap.add_argument("raw"); ap.add_argument("--top", type=int, default=15)
ap.add_argument("--min-share", type=float, default=2.0, help="skip threads under this %% of samples")
a = ap.parse_args()
lines = open(a.raw).read().splitlines()
_, base, binary = lines[0].split(None, 2); base = int(base, 16)
syms = []
for l in subprocess.run(["nm", "-C", "--defined-only", "-n", binary], capture_output=True, text=True).stdout.splitlines():
    p = l.split(" ", 2)
    if len(p) == 3 and p[1] in "tTwW":
        syms.append((int(p[0], 16), p[2]))
addrs = [s[0] for s in syms]
cache = {}
def name(pc):
    if pc in cache: return cache[pc]
    off = pc - base - 1
    i = bisect.bisect_right(addrs, off) - 1
    r = syms[i][1] if 0 <= i and off - addrs[i] < 0x200000 else "[lib %x]" % pc
    cache[pc] = r; return r
per = collections.defaultdict(list)
for l in lines[1:]:
    f = l.split()
    per[int(f[0])].append([int(x, 16) for x in f[1:]])
total = sum(len(v) for v in per.values())
print(f"{total} samples, {len(per)} threads")
for tid, samples in sorted(per.items(), key=lambda kv: -len(kv[1])):
    share = 100.0 * len(samples) / total
    if share < a.min_share: continue
    self_c, incl_c = collections.Counter(), collections.Counter()
    for st in samples:
        fr = [name(pc) for pc in st[2:]]          # skip the handler and the signal trampoline
        if not fr: continue
        self_c[fr[0]] += 1
        for n in set(fr): incl_c[n] += 1
    print(f"\n== thread {tid}: {len(samples)} samples ({share:.1f}%)")
    print("  self:")
    for n, c in self_c.most_common(a.top): print(f"    {100.0*c/len(samples):5.1f}%  {n[:110]}")
    print("  inclusive:")
    for n, c in incl_c.most_common(a.top): print(f"    {100.0*c/len(samples):5.1f}%  {n[:110]}")

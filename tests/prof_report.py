#!/usr/bin/env python3
"""Symbolize a DIAG_PROF profile (port/diag): per thread, the functions most
samples were in (self) and under (inclusive).

  tests/prof_report.py <profile.raw> [--top N] [--min-share 2] [--lines FUNCTION] [--at T0-T1[,T0-T1...]]
--lines (Windows): the source lines that FUNCTION's own samples were on.
--at: only the samples taken in these spans, in us on the frame trace's clock
(a PS3_FTRACE run writes profile.times next to profile.raw), e.g. one slow
frame's flip request to the next (tests/slow_frames.py lists them).

Linux: the binary's symbol table through nm. Windows: the PDB next to the
.exe through DbgHelp (a Windows development build writes one, see
port/CMakeLists.txt), with the thread names the profile recorded.
"""
import argparse, bisect, collections, subprocess

ap = argparse.ArgumentParser()
ap.add_argument("raw"); ap.add_argument("--top", type=int, default=15)
ap.add_argument("--min-share", type=float, default=2.0, help="skip threads under this %% of samples")
ap.add_argument("--lines", help="also list the source lines FUNCTION's own samples were on (Windows)")
ap.add_argument("--at", help="T0-T1[,T0-T1...]: only samples in these spans (us, the frame trace's clock)")
a = ap.parse_args()
lines = open(a.raw, encoding="utf-8", errors="replace").read().splitlines()
if a.at:   # profile.times: one time per sample line, in profile.raw's order
    import os
    spans = [tuple(float(x) for x in s.split("-")) for s in a.at.split(",")]
    when = [float(x) for x in open(os.path.join(os.path.dirname(os.path.abspath(a.raw)), "profile.times"))]
    kept, i = [lines[0]], 0
    for l in lines[1:]:
        if l.startswith(("thread ", "module ")):
            kept.append(l); continue
        if any(t0 <= when[i] <= t1 for t0, t1 in spans):
            kept.append(l)
        i += 1
    lines = kept
_, base, binary = lines[0].split(None, 2); base = int(base, 16)
windows = binary.lower().endswith(".exe")
cache = {}

if windows:
    import ctypes, ctypes.wintypes as wt

    class SYMBOL_INFO(ctypes.Structure):
        _fields_ = [("SizeOfStruct", wt.ULONG), ("TypeIndex", wt.ULONG), ("Reserved", ctypes.c_uint64 * 2),
                    ("Index", wt.ULONG), ("Size", wt.ULONG), ("ModBase", ctypes.c_uint64), ("Flags", wt.ULONG),
                    ("Value", ctypes.c_uint64), ("Address", ctypes.c_uint64), ("Register", wt.ULONG),
                    ("Scope", wt.ULONG), ("Tag", wt.ULONG), ("NameLen", wt.ULONG), ("MaxNameLen", wt.ULONG),
                    ("Name", ctypes.c_char * 512)]
    dbg = ctypes.WinDLL("dbghelp")
    dbg.SymLoadModuleExW.restype = ctypes.c_uint64
    dbg.SymLoadModuleExW.argtypes = [ctypes.c_void_p, wt.HANDLE, wt.LPCWSTR, wt.LPCWSTR, ctypes.c_uint64,
                                     wt.DWORD, ctypes.c_void_p, wt.DWORD]
    dbg.SymFromAddr.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), ctypes.c_void_p]
    proc = ctypes.c_void_p(0x50524F46)       # a pseudo handle: no live process needed
    dbg.SymSetOptions(0x2)                   # SYMOPT_UNDNAME
    dbg.SymInitializeW(proc, None, False)
    dbg.SymLoadModuleExW(proc, None, binary, None, base, 0, None, 0)
    mods = {}                                 # other modules: their exports name ntdll's waits
    for l in lines[1:]:
        f = l.split(None, 3)
        if f and f[0] == "module" and int(f[1], 16) != base:
            mods[int(f[1], 16)] = f[3].rsplit("\\", 1)[-1]
            dbg.SymLoadModuleExW(proc, None, f[3], None, int(f[1], 16), int(f[2]), None, 0)
    si = SYMBOL_INFO(); si.SizeOfStruct = 88; si.MaxNameLen = 500
    disp = ctypes.c_uint64()

    def name(pc):
        if pc in cache: return cache[pc]
        si.MaxNameLen = 500
        ok = dbg.SymFromAddr(proc, pc - 1, ctypes.byref(disp), ctypes.byref(si))
        if ok and si.ModBase == base: r = si.Name.decode(errors="replace")
        elif ok: r = f"{mods.get(si.ModBase, '?')}!{si.Name.decode(errors='replace')}"
        else: r = "[ext %x]" % pc
        cache[pc] = r; return r
    skip = 0                                  # frame 0 is the thread's own instruction pointer
else:
    syms = []
    for l in subprocess.run(["nm", "-C", "--defined-only", "-n", binary], capture_output=True, text=True).stdout.splitlines():
        p = l.split(" ", 2)
        if len(p) == 3 and p[1] in "tTwW":
            syms.append((int(p[0], 16), p[2]))
    addrs = [s[0] for s in syms]

    def name(pc):
        if pc in cache: return cache[pc]
        off = pc - base - 1
        i = bisect.bisect_right(addrs, off) - 1
        r = syms[i][1] if 0 <= i and off - addrs[i] < 0x200000 else "[lib %x]" % pc
        cache[pc] = r; return r
    skip = 2                                  # the signal handler and the trampoline

names = {}
per = collections.defaultdict(list)
for l in lines[1:]:
    f = l.split()
    if f and f[0] == "thread":
        names[int(f[1])] = " ".join(f[2:]); continue
    if f and f[0] == "module": continue
    per[int(f[0])].append([int(x, 16) for x in f[1:]])
total = sum(len(v) for v in per.values())
print(f"{total} samples, {len(per)} threads")
for tid, samples in sorted(per.items(), key=lambda kv: -len(kv[1])):
    share = 100.0 * len(samples) / total
    if share < a.min_share: continue
    self_c, incl_c = collections.Counter(), collections.Counter()
    for st in samples:
        fr = [name(pc) for pc in st[skip:]]
        if not fr: continue
        self_c[fr[0]] += 1
        for n in set(fr): incl_c[n] += 1
    print(f"\n== thread {tid}{' (' + names[tid] + ')' if tid in names else ''}: {len(samples)} samples ({share:.1f}%)")
    print("  self:")
    for n, c in self_c.most_common(a.top): print(f"    {100.0*c/len(samples):5.1f}%  {n[:110]}")
    print("  inclusive:")
    for n, c in incl_c.most_common(a.top): print(f"    {100.0*c/len(samples):5.1f}%  {n[:110]}")
    if a.lines and windows and self_c[a.lines]:
        class LINE(ctypes.Structure):
            _fields_ = [("SizeOfStruct", wt.DWORD), ("Key", ctypes.c_void_p), ("LineNumber", wt.DWORD),
                        ("FileName", ctypes.c_char_p), ("Address", ctypes.c_uint64)]
        dbg.SymGetLineFromAddr64.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.POINTER(wt.DWORD),
                                             ctypes.POINTER(LINE)]
        lc = collections.Counter()
        for st in samples:
            if len(st) > skip and name(st[skip]) == a.lines:
                ln, d = LINE(), wt.DWORD()
                ln.SizeOfStruct = ctypes.sizeof(LINE)
                if dbg.SymGetLineFromAddr64(proc, st[skip] - 1, ctypes.byref(d), ctypes.byref(ln)):
                    lc[f"{ln.FileName.decode(errors='replace').replace(chr(92), '/').rsplit('/', 1)[-1]}:{ln.LineNumber}"] += 1
                else:
                    lc["?"] += 1
        print(f"  {a.lines}'s own samples by line ({self_c[a.lines]}):")
        for n, c in lc.most_common(a.top): print(f"    {100.0*c/self_c[a.lines]:5.1f}%  {n}")

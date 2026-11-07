#!/usr/bin/env python3
"""R8's inventory of a DIAG_PROF profile (port/diag, Windows): every thread's
work against its waits, what each thread works on by category (the game's
lifted code, the recorder, SPU images, native physics, SPURS, audio, the
renderer, the runtime), what each waits in, and for the main PPU thread the
same split in ms a frame.

  tools/r8_threads.py <profile.raw> [--fps F] [--top N] [--thread NAME]

--fps: the run's frame rate over the profile's window, to turn the main
thread's shares into ms a frame. --thread: also the top functions of the
threads whose name contains NAME. The profiler samples every thread about
once a millisecond each, blocked or not (port/diag/diag.cpp), so a thread's
samples are its wall time; a sample whose innermost frame is a kernel wait,
a sleep or a yield is a wait.
"""
import argparse, collections, ctypes, ctypes.wintypes as wt, os, re

ap = argparse.ArgumentParser()
ap.add_argument("raw"); ap.add_argument("--fps", type=float); ap.add_argument("--top", type=int, default=12)
ap.add_argument("--thread", action="append", default=[])
ap.add_argument("--under", help="the main thread's samples with FUNCTION on the stack: their share and self functions")
a = ap.parse_args()
lines = open(a.raw, encoding="utf-8", errors="replace").read().splitlines()
_, base, binary = lines[0].split(None, 2); base = int(base, 16)


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
proc = ctypes.c_void_p(0x52385448)
dbg.SymSetOptions(0x2)
dbg.SymInitializeW(proc, None, False)
dbg.SymLoadModuleExW(proc, None, binary, None, base, 0, None, 0)
mods = {}
for l in lines[1:]:
    f = l.split(None, 3)
    if f and f[0] == "module" and int(f[1], 16) != base:
        mods[int(f[1], 16)] = f[3].rsplit("\\", 1)[-1]
        dbg.SymLoadModuleExW(proc, None, f[3], None, int(f[1], 16), int(f[2]), None, 0)
si = SYMBOL_INFO(); si.SizeOfStruct = 88; si.MaxNameLen = 500
disp = ctypes.c_uint64()
cache = {}


def name(pc):
    if pc in cache: return cache[pc]
    ok = dbg.SymFromAddr(proc, pc - 1, ctypes.byref(disp), ctypes.byref(si))
    if ok and si.ModBase == base: r = si.Name.decode(errors="replace")
    elif ok: r = f"{mods.get(si.ModBase, '?')}!{si.Name.decode(errors='replace')}"
    else: r = "[ext %x]" % pc
    cache[pc] = r; return r


WAIT = re.compile(r"!(Zw|Nt)(WaitFor|Delay|RemoveIoCompletion|YieldExecution|ReplyWaitReceive|SignalAndWait)|"
                  r"!SwitchToThread|!Sleep(Ex)?$|!ZwAlertThreadByThreadId|!NtUser(MsgWaitForMultipleObjectsEx|GetMessage)")
SYS = re.compile(r"^(ntdll|KERNEL32|KERNELBASE|win32u|USER32|ucrtbase|VCRUNTIME140|msvcp140|nvoglv64|nvwgf2umx|"
                 r"vulkan-1|nvoglv|nvgpucomp64|SDL2|GDI32|combase|ole32|dxgi|d3d11|nvd3dumx|nvldumdx|nvlddmkm|"
                 r"AUDIOSES|MMDevAPI|winmm|XInput1_4|ws2_32|nvapi64)\.(dll|DLL)!|^\?!|^\[ext")
SPU_IMG = {"0000": "spu: water", "0001": "spu: waver", "0002": "spu: edge zlib", "0003": "spu: BulletCD (lifted)",
           "0004": "spu: BulletSolver (lifted)"}


def category(fr):
    """the innermost frame's category, from the outermost meaningful frame inward"""
    f0 = fr[0]
    m = re.match(r"spu_(\d{4})_at_", f0)
    if m:
        return SPU_IMG.get(m.group(1), "spu: FMOD (audio)")
    if re.search(r"bt[A-Z]|bullet|Bullet|native_cd|native_solver|cd_task|solver_task", f0):
        return "physics (native)"
    if f0.startswith(("func_", "ppu_recomp")):
        return "game code (lifted)"
    if f0 in ("ps3_indirect_call",) or f0.startswith(("ppu_", "vm_", "ps3_")):
        return "runtime: " + f0 if f0 in ("ps3_indirect_call",) else "runtime (ppu/vm)"
    if re.search(r"ppfx|asp_|vp_blob|blob_of|take_vconst|shadow_page|r6_|r7_|R6|R7", f0):
        return "recorder"
    if re.search(r"spurs|spu_|Spurs|SPU", f0):
        return "spu/spurs runtime"
    if re.search(r"fmod|FMOD|audio|Audio|mix", f0):
        return "audio"
    if re.search(r"vk|Vk|vulkan|render|present|rsx", f0):
        return "renderer/present"
    if re.search(r"memmove|memcpy|memset", f0):
        return "memcpy/memset (" + next((x for x in fr[1:] if not SYS.search(x) and "mem" not in x), "?")[:40] + ")"
    if SYS.search(f0):
        inner = next((x for x in fr[1:] if not SYS.search(x)), None)
        return "system (" + (inner[:40] if inner else "?") + ")"
    return "other: " + f0[:50]


def wait_site(fr):
    """the first frame of ours above the wait, and the guest function above that, if any"""
    ours = [x for x in fr if not SYS.search(x)]
    if not ours: return "?"
    g = next((x for x in ours if x.startswith("func_")), None)
    return ours[0][:48] + (f"  [guest {g}]" if g and g != ours[0] else "")


names, per = {}, collections.defaultdict(list)
for l in lines[1:]:
    f = l.split()
    if not f: continue
    if f[0] == "thread":
        names[int(f[1])] = " ".join(f[2:]); continue
    if f[0] == "module": continue
    per[int(f[0])].append([int(x, 16) for x in f[1:]])

rows = []
for tid, samples in per.items():
    work, waits, cats, fn = 0, collections.Counter(), collections.Counter(), collections.Counter()
    for st in samples:
        fr = [name(pc) for pc in st]
        if not fr: continue
        if WAIT.search(fr[0]):
            waits[wait_site(fr)] += 1
        else:
            work += 1
            cats[category(fr)] += 1
            fn[fr[0]] += 1
    rows.append((tid, names.get(tid, ""), len(samples), work, cats, waits, fn))

print(f"{os.path.abspath(a.raw)}: {sum(r[2] for r in rows)} samples, {len(rows)} threads")
print("\nthread                                    work%   top work categories (% of the thread's samples)")
for tid, nm, n, work, cats, waits, fn in sorted(rows, key=lambda r: -r[3] / max(r[2], 1)):
    if work / n < 0.005: continue
    top = ", ".join(f"{c} {100 * k / n:.1f}" for c, k in cats.most_common(4))
    print(f"  {tid:6d} {nm[:32]:32s} {100 * work / n:5.1f}   {top}")
idle = [f"{nm or tid}" for tid, nm, n, work, *_ in rows if work / n < 0.005]
print(f"\nidle (under 0.5% work): {len(idle)} threads: " + ", ".join(sorted(idle)))
print("\nwork, all threads, by category (thread-ms per second of the window):")
allc = collections.Counter()
for tid, nm, n, work, cats, *_ in rows:
    for c, k in cats.items(): allc[c] += 1000.0 * k / n
for c, v in allc.most_common(20): print(f"  {v:7.1f}  {c}")

for tid, nm, n, work, cats, waits, fn in rows:
    if not (nm == "ppu main" or any(s in nm for s in a.thread)): continue
    ms = (lambda k: f"  {k / n * 1000.0 / a.fps:5.2f} ms/frame" if a.fps else "")
    print(f"\n== {nm} ({tid}): {n} samples, work {100 * work / n:.1f}%")
    print("  work by category:")
    for c, k in cats.most_common(14): print(f"    {100 * k / n:5.1f}%{ms(k)}  {c}")
    print("  waits by site:")
    for c, k in waits.most_common(8): print(f"    {100 * k / n:5.1f}%{ms(k)}  {c}")
    print("  top functions (self):")
    for c, k in fn.most_common(a.top): print(f"    {100 * k / n:5.1f}%{ms(k)}  {c[:100]}")
    if a.under and nm == "ppu main":
        sub, tot = collections.Counter(), 0
        for st in per[tid]:
            fr = [name(pc) for pc in st]
            if fr and any(x == a.under for x in fr):
                tot += 1
                sub[fr[0]] += 1
        print(f"  under {a.under}: {100 * tot / n:.1f}%{ms(tot)}, self:")
        for c, k in sub.most_common(a.top): print(f"    {100 * k / n:5.1f}%{ms(k)}  {c[:100]}")

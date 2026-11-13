#!/usr/bin/env python3
"""thread_cpu.py: each thread's CPU time in a running game, from the system's own accounting (Windows: the kernel's
per-thread user and kernel time, GetThreadTimes; the thread's name from GetThreadDescription, which the runtime
sets: ps3emu/thread_name.h). Nothing is injected into the game and nothing in it changes; the sampler's own cost is
one snapshot of the thread list and one query a thread per interval.

  thread_cpu.py [--pid PID | --name dotheroes.exe] [--interval S] [--duration S] [--after S]
                [--csv FILE] [--top N] [--wait S]

--after: start measuring S seconds after the process is found (a scene's set-up left out); --wait: wait up to S
seconds for the process to appear. Prints, for the window measured: the process's CPU in cores, then each thread
(a name shared by several threads is summed under it, its count shown): its mean share of one core, the highest
one interval reached, its CPU seconds; with --csv, every interval of every thread (time, tid, name, % of a core).
Threads that start or end inside the window are counted for the time they lived.
"""
import argparse
import collections
import ctypes
import ctypes.wintypes as wt
import sys
import time

if sys.platform != "win32":
    sys.exit("thread_cpu.py: Windows only (Linux: /proc/<pid>/task/*/stat holds the same)")

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
TH32CS_SNAPPROCESS, TH32CS_SNAPTHREAD = 0x2, 0x4
THREAD_QUERY_LIMITED_INFORMATION = 0x0800
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("th32DefaultHeapID", ctypes.c_void_p), ("th32ModuleID", wt.DWORD), ("cntThreads", wt.DWORD),
                ("th32ParentProcessID", wt.DWORD), ("pcPriClassBase", ctypes.c_long), ("dwFlags", wt.DWORD),
                ("szExeFile", ctypes.c_wchar * 260)]


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD),
                ("th32OwnerProcessID", wt.DWORD), ("tpBasePri", ctypes.c_long), ("tpDeltaPri", ctypes.c_long),
                ("dwFlags", wt.DWORD)]


k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
k32.CreateToolhelp32Snapshot.argtypes = [wt.DWORD, wt.DWORD]
k32.Process32FirstW.argtypes = k32.Process32NextW.argtypes = [wt.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
k32.Thread32First.argtypes = k32.Thread32Next.argtypes = [wt.HANDLE, ctypes.POINTER(THREADENTRY32)]
k32.OpenThread.restype = wt.HANDLE
k32.OpenThread.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.GetThreadTimes.argtypes = [wt.HANDLE] + [ctypes.POINTER(wt.FILETIME)] * 4
k32.CloseHandle.argtypes = [wt.HANDLE]
GetThreadDescription = getattr(k32, "GetThreadDescription", None)
if GetThreadDescription:
    GetThreadDescription.argtypes = [wt.HANDLE, ctypes.POINTER(ctypes.c_wchar_p)]
k32.LocalFree.argtypes = [ctypes.c_void_p]
INVALID = wt.HANDLE(-1).value


def find_pid(name):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    e = PROCESSENTRY32W()
    e.dwSize = ctypes.sizeof(e)
    found = None
    ok = k32.Process32FirstW(snap, ctypes.byref(e))
    while ok:
        if e.szExeFile.lower() == name.lower():
            found = e.th32ProcessID
        ok = k32.Process32NextW(snap, ctypes.byref(e))
    k32.CloseHandle(snap)
    return found


def threads_of(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    e = THREADENTRY32()
    e.dwSize = ctypes.sizeof(e)
    out = []
    ok = k32.Thread32First(snap, ctypes.byref(e))
    while ok:
        if e.th32OwnerProcessID == pid:
            out.append(e.th32ThreadID)
        ok = k32.Thread32Next(snap, ctypes.byref(e))
    k32.CloseHandle(snap)
    return out


def ft(f):
    return (f.dwHighDateTime << 32 | f.dwLowDateTime) / 1e7   # 100 ns units -> s


class Thread:
    def __init__(self, tid):
        self.tid = tid
        self.h = k32.OpenThread(THREAD_QUERY_LIMITED_INFORMATION, False, tid)
        self.name = ""
        if self.h and GetThreadDescription:
            p = ctypes.c_wchar_p()
            if GetThreadDescription(self.h, ctypes.byref(p)) >= 0 and p.value:
                self.name = p.value
                k32.LocalFree(ctypes.cast(p, ctypes.c_void_p))
        if not self.name:
            self.name = "(unnamed)"

    def cpu(self):
        if not self.h:
            return None
        c, x, kt, ut = wt.FILETIME(), wt.FILETIME(), wt.FILETIME(), wt.FILETIME()
        if not k32.GetThreadTimes(self.h, ctypes.byref(c), ctypes.byref(x), ctypes.byref(kt), ctypes.byref(ut)):
            return None
        return ft(kt) + ft(ut)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pid", type=int)
    ap.add_argument("--name", default="dotheroes.exe")
    ap.add_argument("--interval", type=float, default=1.0)
    ap.add_argument("--duration", type=float, default=60.0)
    ap.add_argument("--after", type=float, default=0.0)
    ap.add_argument("--wait", type=float, default=0.0)
    ap.add_argument("--csv")
    ap.add_argument("--top", type=int, default=25)
    a = ap.parse_args()
    pid = a.pid
    t_end_wait = time.monotonic() + a.wait
    while not pid:
        pid = find_pid(a.name)
        if pid or time.monotonic() >= t_end_wait:
            break
        time.sleep(0.5)
    if not pid:
        sys.exit(f"thread_cpu.py: no process {a.name}")
    if a.after:
        time.sleep(a.after)
    live = {}
    total = collections.defaultdict(float)     # tid -> CPU seconds in the window
    peak = collections.defaultdict(float)      # tid -> highest interval share
    names = {}
    rows = []
    t0 = last = time.monotonic()
    prev = {}
    while True:
        tids = threads_of(pid)
        if not tids:
            break
        now = time.monotonic()
        for tid in tids:
            if tid not in live:
                live[tid] = Thread(tid)
                names[tid] = live[tid].name
        dt = now - last
        for tid in list(live):
            c = live[tid].cpu()
            if c is None:
                continue
            if tid in prev and dt > 0:
                d = max(0.0, c - prev[tid])
                total[tid] += d
                share = 100.0 * d / dt
                peak[tid] = max(peak[tid], share)
                if a.csv:
                    rows.append((round(now - t0, 3), tid, names[tid], round(share, 2)))
            prev[tid] = c
        last = now
        if now - t0 >= a.duration:
            break
        time.sleep(max(0.0, a.interval - (time.monotonic() - now)))
    span = last - t0
    if span <= 0:
        sys.exit("thread_cpu.py: nothing measured")
    groups = collections.OrderedDict()
    for tid, s in total.items():
        g = groups.setdefault(names[tid], [0.0, 0.0, 0])
        g[0] += s
        g[1] = max(g[1], peak[tid])
        g[2] += 1
    print(f"pid {pid}: {span:.1f} s measured, {len(total)} threads; the process {sum(total.values()) / span:.2f} cores")
    print(f"{'thread':40s} {'n':>3s} {'mean % of a core':>17s} {'peak %':>7s} {'CPU s':>7s}")
    for name, (s, pk, n) in sorted(groups.items(), key=lambda kv: -kv[1][0])[:a.top]:
        print(f"{name[:40]:40s} {n:3d} {100.0 * s / span:17.1f} {pk:7.1f} {s:7.2f}")
    if a.csv:
        with open(a.csv, "w", encoding="utf-8", newline="\n") as f:
            f.write("t,tid,name,percent_of_a_core\n")
            for r in rows:
                f.write(f"{r[0]},{r[1]},\"{r[2]}\",{r[3]}\n")


if __name__ == "__main__":
    main()

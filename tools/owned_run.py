#!/usr/bin/env python3
"""Run a batch so that stopping it stops everything it started, and nothing else.

    build/venv/Scripts/python.exe tools/owned_run.py --name NAME [--no-watch] -- COMMAND [ARGS...]
    build/venv/Scripts/python.exe tools/owned_run.py --cancel NAME
    build/venv/Scripts/python.exe tools/owned_run.py --list

The command and every process it starts (its shells, tests/boot_smoke.py,
tests/drive.py, `timeout`, the game) run inside a Windows job object of the
batch's own, so ownership is the operating system's bookkeeping rather than
a search by name or command line. The batch ends, with all of them, when:
  - `--cancel NAME` is run (any shell; the job is named), or
  - the launching shell goes away: every shell or Python process this launcher
    was started under, up to the first other process, is watched. Stopping a
    background task ends only the outermost Git Bash on Windows; its children
    used to run on (docs/NATIVE_R8.md section 9.2), or
  - the launcher itself is killed: the job is set to kill its processes when
    the last handle to it -- the launcher's -- closes.
(--no-watch: not the shell; for a batch meant to outlive it.)
When the command ends by itself, anything it left running in the job is
stopped and named, and the launcher exits with the command's exit code.

A process outside the job -- the owner's game, another batch, anything started
some other way -- is never touched. Records: build/owned/NAME.json while it
runs. A graceful stop of test runs (each game writing its logs) is
tests/stop_tests.py; this is the stop for the batch as a whole.

Elsewhere than Windows the command runs in a session of its own, and --cancel
signals that process group (no ancestor watch)."""
import json
import os
import signal
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RECORDS = os.path.join(ROOT, "build", "owned")
# the processes a batch is launched through: watched, up to the first other one
WRAPPERS = {"bash.exe", "sh.exe", "dash.exe", "python.exe", "pythonw.exe", "py.exe", "timeout.exe", "env.exe",
            "cmd.exe", "powershell.exe", "pwsh.exe", "conhost.exe"}
MAX_ANCESTORS = 8

if os.name == "nt":
    import ctypes
    import ctypes.wintypes as w

    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    # every signature declared: a handle is pointer-sized (the current process's pseudo-handle
    # is -1, which does not fit ctypes' default int)
    for fn, res, args in (
            ("CreateJobObjectW", w.HANDLE, (ctypes.c_void_p, w.LPCWSTR)),
            ("OpenJobObjectW", w.HANDLE, (w.DWORD, w.BOOL, w.LPCWSTR)),
            ("SetInformationJobObject", w.BOOL, (w.HANDLE, ctypes.c_int, ctypes.c_void_p, w.DWORD)),
            ("QueryInformationJobObject", w.BOOL, (w.HANDLE, ctypes.c_int, ctypes.c_void_p, w.DWORD, ctypes.c_void_p)),
            ("AssignProcessToJobObject", w.BOOL, (w.HANDLE, w.HANDLE)),
            ("TerminateJobObject", w.BOOL, (w.HANDLE, w.UINT)),
            ("OpenProcess", w.HANDLE, (w.DWORD, w.BOOL, w.DWORD)),
            ("GetCurrentProcess", w.HANDLE, ()),
            ("CloseHandle", w.BOOL, (w.HANDLE,)),
            ("GetProcessTimes", w.BOOL, (w.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)),
            ("WaitForMultipleObjects", w.DWORD, (w.DWORD, ctypes.c_void_p, w.BOOL, w.DWORD)),
            ("CreateToolhelp32Snapshot", w.HANDLE, (w.DWORD, w.DWORD)),
            ("Process32FirstW", w.BOOL, (w.HANDLE, ctypes.c_void_p)),
            ("Process32NextW", w.BOOL, (w.HANDLE, ctypes.c_void_p))):
        getattr(k32, fn).restype = res
        getattr(k32, fn).argtypes = args
    SYNCHRONIZE, PROCESS_QUERY_LIMITED = 0x00100000, 0x1000
    JOB_TERMINATE, JOB_QUERY = 0x0008, 0x0004
    KILL_ON_JOB_CLOSE = 0x2000
    WAIT_OBJECT_0, WAIT_TIMEOUT = 0, 0x102
    ERROR_ALREADY_EXISTS = 183

    class IO_COUNTERS(ctypes.Structure):
        _fields_ = [(n, ctypes.c_ulonglong) for n in ("r", "w", "o", "rb", "wb", "ob")]

    class BASIC_LIMIT(ctypes.Structure):
        _fields_ = [("PerProcessUserTimeLimit", ctypes.c_longlong), ("PerJobUserTimeLimit", ctypes.c_longlong),
                    ("LimitFlags", w.DWORD), ("MinimumWorkingSetSize", ctypes.c_size_t),
                    ("MaximumWorkingSetSize", ctypes.c_size_t), ("ActiveProcessLimit", w.DWORD),
                    ("Affinity", ctypes.c_size_t), ("PriorityClass", w.DWORD), ("SchedulingClass", w.DWORD)]

    class EXTENDED_LIMIT(ctypes.Structure):
        _fields_ = [("Basic", BASIC_LIMIT), ("Io", IO_COUNTERS), ("ProcessMemoryLimit", ctypes.c_size_t),
                    ("JobMemoryLimit", ctypes.c_size_t), ("PeakProcessMemoryUsed", ctypes.c_size_t),
                    ("PeakJobMemoryUsed", ctypes.c_size_t)]

    class PROCESSENTRY32W(ctypes.Structure):
        _fields_ = [("dwSize", w.DWORD), ("cntUsage", w.DWORD), ("th32ProcessID", w.DWORD),
                    ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", w.DWORD), ("cntThreads", w.DWORD),
                    ("th32ParentProcessID", w.DWORD), ("pcPriClassBase", ctypes.c_long), ("dwFlags", w.DWORD),
                    ("szExeFile", ctypes.c_wchar * 260)]

    def job_name(name):
        return "Local\\dotheroes-owned-" + name

    def processes():
        """{pid: (parent pid, image name)}"""
        snap = k32.CreateToolhelp32Snapshot(2, 0)
        out, pe = {}, PROCESSENTRY32W()
        pe.dwSize = ctypes.sizeof(pe)
        ok = k32.Process32FirstW(snap, ctypes.byref(pe))
        while ok:
            out[pe.th32ProcessID] = (pe.th32ParentProcessID, pe.szExeFile)
            ok = k32.Process32NextW(snap, ctypes.byref(pe))
        k32.CloseHandle(snap)
        return out

    def creation_time(h):
        c, e, kk, u = (w.FILETIME() for _ in range(4))
        if not k32.GetProcessTimes(h, ctypes.byref(c), ctypes.byref(e), ctypes.byref(kk), ctypes.byref(u)):
            return None
        return (c.dwHighDateTime << 32) | c.dwLowDateTime

    def ancestors():
        """handles to the shells and Pythons this process runs under, nearest first: (handle, pid, name)"""
        procs, me = processes(), os.getpid()
        own = k32.OpenProcess(PROCESS_QUERY_LIMITED, False, me)
        mine = creation_time(own)
        k32.CloseHandle(own)
        out, pid = [], procs.get(me, (0, ""))[0]
        while pid and pid in procs and len(out) < MAX_ANCESTORS:
            name = procs[pid][1]
            if name.lower() not in WRAPPERS:
                break
            h = k32.OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED, False, pid)
            if not h:
                break
            born = creation_time(h)
            if born is None or mine is None or born > mine:   # a reused pid: not our ancestor
                k32.CloseHandle(h)
                break
            out.append((h, pid, name))
            pid = procs[pid][0]
        return out

    def job_pids(job):
        """the processes in the job"""
        n = 1024

        class LIST(ctypes.Structure):
            _fields_ = [("assigned", w.DWORD), ("listed", w.DWORD), ("ids", ctypes.c_size_t * n)]
        lst = LIST()
        if not k32.QueryInformationJobObject(job, 3, ctypes.byref(lst), ctypes.sizeof(lst), None):
            return []
        return [lst.ids[i] for i in range(lst.listed)]

    def describe(pids):
        procs = processes()
        return ", ".join(f"{procs.get(p, (0, '?'))[1]} {p}" for p in pids)


def record_path(name):
    return os.path.join(RECORDS, name + ".json")


def launch(name, cmd, watch=True):
    os.makedirs(RECORDS, exist_ok=True)
    rec = {"name": name, "pid": os.getpid(), "cmd": cmd, "started": time.strftime("%Y-%m-%d %H:%M:%S")}
    if os.name != "nt":
        p = subprocess.Popen(cmd, start_new_session=True)
        rec["pgid"] = p.pid
        json.dump(rec, open(record_path(name), "w"), indent=1)
        try:
            return p.wait()
        finally:
            try:
                os.killpg(p.pid, signal.SIGTERM)
            except OSError:
                pass
            os.remove(record_path(name))
    job = k32.CreateJobObjectW(None, job_name(name))
    if not job:
        sys.exit(f"owned_run: no job object ({ctypes.get_last_error()})")
    if ctypes.get_last_error() == ERROR_ALREADY_EXISTS:
        sys.exit(f"owned_run: a batch named {name} is running (tools/owned_run.py --list)")
    lim = EXTENDED_LIMIT()
    lim.Basic.LimitFlags = KILL_ON_JOB_CLOSE
    if not k32.SetInformationJobObject(job, 9, ctypes.byref(lim), ctypes.sizeof(lim)):
        sys.exit(f"owned_run: the job's kill-on-close not set ({ctypes.get_last_error()})")
    watched = ancestors() if watch else []
    # the launcher joins the job first, so every process started from here on is in it
    # from its first instruction: nothing can start a child before it is assigned
    if not k32.AssignProcessToJobObject(job, k32.GetCurrentProcess()):
        sys.exit(f"owned_run: could not join the job ({ctypes.get_last_error()})")
    rec["job"] = job_name(name)
    rec["watched"] = [f"{n} {p}" for _, p, n in watched]
    json.dump(rec, open(record_path(name), "w"), indent=1)
    print(f"[owned_run] {name}: {' '.join(cmd)} (job {job_name(name)}; ends with "
          f"{', '.join(rec['watched']) or 'the launcher'})", flush=True)
    try:
        p = subprocess.Popen(cmd)
    except OSError as e:
        os.remove(record_path(name))
        sys.exit(f"owned_run: {e}")
    handles = (w.HANDLE * (1 + len(watched)))(int(p._handle), *[h for h, _, _ in watched])
    why = None
    while why is None:
        r = k32.WaitForMultipleObjects(len(handles), handles, False, 1000)
        if r == WAIT_OBJECT_0:
            why = "done"
        elif WAIT_OBJECT_0 < r < WAIT_OBJECT_0 + len(handles):
            _, pid, nm = watched[r - WAIT_OBJECT_0 - 1]
            why = f"its launching {nm} {pid} ended"
        elif r != WAIT_TIMEOUT:
            why = f"wait failed ({ctypes.get_last_error()})"
    code = p.poll() if why == "done" else 1
    left = [x for x in job_pids(job) if x != os.getpid()]
    if why == "done":
        msg = f"[owned_run] {name}: exit {code}" + (f"; stopping what it left running: {describe(left)}" if left else "")
    else:
        msg = f"[owned_run] {name}: {why}: stopping {describe(left) or 'nothing (all ended)'}"
    try:
        os.remove(record_path(name))
    except OSError:
        pass
    print(msg, flush=True)
    sys.stdout.flush()
    sys.stderr.flush()
    # every process in the job, this one with them, with the command's code
    k32.TerminateJobObject(job, code & 0xFFFFFFFF if code is not None else 1)
    return code   # not reached: the launcher is in the job


def cancel(name):
    if os.name != "nt":
        try:
            os.killpg(json.load(open(record_path(name)))["pgid"], signal.SIGTERM)
        except (OSError, ValueError, KeyError) as e:
            sys.exit(f"owned_run: {name}: {e}")
        print(f"[owned_run] {name}: cancelled")
        return 0
    job = k32.OpenJobObjectW(JOB_TERMINATE | JOB_QUERY, False, job_name(name))
    if not job:
        print(f"[owned_run] {name}: not running (no job {job_name(name)})")
        try:
            os.remove(record_path(name))   # the record of a launcher that was killed
        except OSError:
            pass
        return 1
    pids = job_pids(job)
    print(f"[owned_run] {name}: cancelling {describe(pids) or 'an empty job'}", flush=True)
    ok = k32.TerminateJobObject(job, 1)
    for _ in range(50):   # until every process has gone
        if not job_pids(job):
            break
        time.sleep(0.1)
    left = job_pids(job)
    k32.CloseHandle(job)
    try:
        os.remove(record_path(name))
    except OSError:
        pass
    if not ok or left:
        print(f"[owned_run] {name}: still running: {describe(left)}")
        return 1
    print(f"[owned_run] {name}: all {len(pids)} stopped")
    return 0


def list_batches():
    os.makedirs(RECORDS, exist_ok=True)
    for f in sorted(os.listdir(RECORDS)):
        if not f.endswith(".json"):
            continue
        name = f[:-5]
        rec = json.load(open(os.path.join(RECORDS, f)))
        state = "?"
        if os.name == "nt":
            job = k32.OpenJobObjectW(JOB_QUERY, False, job_name(name))
            state = describe(job_pids(job)) if job else "gone (its launcher was killed with it): record removed"
            if job:
                k32.CloseHandle(job)
            else:
                os.remove(os.path.join(RECORDS, f))
        print(f"{name}: since {rec['started']}: {' '.join(rec['cmd'])}\n  {state}")
    return 0


def main():
    a = sys.argv[1:]
    if a[:1] == ["--list"]:
        return list_batches()
    if len(a) == 2 and a[0] == "--cancel":
        return cancel(a[1])
    if len(a) >= 4 and a[0] == "--name" and "--" in a and a.index("--") + 1 < len(a):
        opts, cmd = a[2:a.index("--")], a[a.index("--") + 1:]
        name, watch = a[1], "--no-watch" not in opts
        if set(opts) - {"--no-watch"}:
            sys.exit(__doc__)
        if not name.replace("-", "").replace("_", "").isalnum():
            sys.exit("owned_run: NAME: letters, digits, - and _")
        return launch(name, cmd, watch)
    sys.exit(__doc__)


if __name__ == "__main__":
    sys.exit(main())

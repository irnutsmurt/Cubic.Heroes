#!/usr/bin/env python3
"""The pipeline worker: the launcher's headless half (docs/M9_WORKER_PROTOCOL.md).

    worker.py <command> [options]          one JSON object per line on stdout, nothing else

Commands:
    version                                 the worker's version and protocol
    probe-disc  --disc PATH                 what the disc image (or disc folder) is; supported or why not
    probe-keys  --disc PATH --keys PATH     whether the keys file (or an EBOOT.elf) decrypts this disc's program
    space       --install DIR [--for install|update]   free space against what the operation needs
    info        --install DIR               what an install holds (install.json)
    install     --disc PATH --keys PATH --install DIR  the player's disc -> the native game
    update      --install DIR [--full]      redo what changed (an update of the tools or the configuration)
    migrate-root --from OLD --install NEW [--dry-run]   an existing installation copied into a (portable) root
    find-legacy [--install DIR]             the old launcher's pointers to an install and the old default folders
    (the rest: docs/M9_WORKER_PROTOCOL.md section 1)

Options for every command: --control (the launcher: "cancel" lines on stdin, and the end of stdin, cancel the
operation), --log FILE (every event appended as it is sent, for the launcher's logs).

In a source checkout: `python tools/worker.py ...`; packaged, the setup's frozen executable runs it as
`<app> --worker ...` (setup_app.py main). Keys: read only by the decryption step and the key probe, never
written, logged or put into an event.

The portable root: everything the worker writes for an install is inside it -- the install's own items, its
downloads, Zig's caches (data/cache/zig-*), its temporary files and its children's (data/tmp/worker-<pid>: TMP, TEMP,
TMPDIR), the launcher's update (downloads/launcher-*). The one exception: remove-launcher's helper, which must run
from outside the folder it removes, copies itself into the operating system's temporary folder.
"""
import argparse
import errno
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import traceback
import hostenv  # noqa: E402 -- the system's programs without the frozen worker's library path

ROOT = (os.path.dirname(sys.executable) if getattr(sys, "frozen", False)
        else os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import game_settings  # noqa: E402
import install_layout as layout  # noqa: E402
import pipeline_manifest  # noqa: E402
import root_migration  # noqa: E402
import setup_pipeline as pipeline  # noqa: E402
import updates  # noqa: E402

PROTOCOL = 1
# Exit codes: the result event says the same
EXIT_OK, EXIT_FAILED, EXIT_CANCELLED, EXIT_USAGE = 0, 1, 2, 64
# A cancelled operation unwinds at its next progress or log callback; one blocked longer than this (a compile
# in flight) is ended by the worker's exit, which ends its children with it (the job object below)
CANCEL_GRACE_S = 5.0
# Each step's typical length on the reference PC (seconds; M9.0's measurement, docs/M9_WORKER_PROTOCOL.md
# section 6): the weights of the overall progress, and the remaining time before a step's own rate is known
STAGE_SECONDS = {"verify": 15.0, "extract": 25.0, "decrypt": 1.0, "recomp": 40.0, "spu": 12.0, "build": 275.0,
                 "finish": 1.0, "validate": 5.0, "activate": 1.0, "prepare": 3.0,
                 "copy": 60.0, "check-copy": 20.0, "place": 1.0}   # migrate-root's (estimates, not measured)
CPU_STAGES = {"recomp", "spu", "build"}   # bound by the processor (the others by the drive)
# What an operation needs on the install's drive (bytes; measured, section 6)
NEEDED = {"install": 6 << 30, "update": 3 << 29}
DISK_FULL_ERRNO = {28}          # ENOSPC
DISK_FULL_WINERROR = {39, 112}  # ERROR_HANDLE_DISK_FULL, ERROR_DISK_FULL


LOG_LEVELS = {"error": 0, "warning": 1, "info": 2, "debug": 3, "trace": 4}
LOG_KEEP = 4 << 20   # a worker log past this keeps its last quarter (its newest jobs): no clean-up ever needed


def _trim_log(path):
    try:
        size = os.path.getsize(path)
        if size <= LOG_KEEP:
            return
        with open(path, "rb") as f:
            f.seek(size - LOG_KEEP // 4)
            tail = f.read()
        tail = tail[tail.find(b"\n") + 1:]
        with open(path, "wb") as f:
            f.write(tail)
    except OSError:
        pass


class Events:
    """The event stream: one JSON object per line, flushed, from any thread."""

    def __init__(self, stream, log_path=None):
        self.out = stream
        self.lock = threading.Lock()
        self.t0 = time.monotonic()
        self.log = None
        if log_path:
            os.makedirs(os.path.dirname(os.path.abspath(log_path)), exist_ok=True)
            _trim_log(log_path)
            self.log = open(log_path, "a", encoding="utf-8", newline="\n")
        self.closed = False
        # the log file's detail (the settings' DOTHEROES_LOG_LEVEL, which the launcher passes on): "log" events above
        # it are left out of the file (the launcher still sees them); errors and the other events always written
        self.level = LOG_LEVELS.get(os.environ.get("DOTHEROES_LOG_LEVEL", "info"), 2)

    def emit(self, ev, **kw):
        obj = {"ev": ev, "t": round(time.monotonic() - self.t0, 3)}
        obj.update(kw)
        line = json.dumps(obj, ensure_ascii=False, separators=(",", ":"))
        with self.lock:
            if self.closed:
                return
            try:
                self.out.write(line + "\n")
                self.out.flush()
            except (OSError, ValueError):   # the launcher went away: carry on to the end
                pass
            if self.log and (ev != "log" or LOG_LEVELS.get(kw.get("level"), 2) <= self.level):
                self.log.write(line + "\n")
                self.log.flush()

    def close(self):
        with self.lock:
            self.closed = True
            if self.log:
                self.log.close()


class Control:
    """--control: the launcher's commands on stdin. "cancel" cancels; the end of stdin (the launcher gone)
    cancels too. Without --control nothing is read and Ctrl+C cancels."""

    def __init__(self, events):
        self.events = events
        self.cancel = threading.Event()
        self.finished = threading.Event()
        # M12: a PS3 save key the player typed, sent as a "key <32 hex digits>" line -- never on the command line,
        # never echoed, logged or written by the worker (characters --action import uses it)
        self.key_text = None
        self.key_ready = threading.Event()

    def listen(self):
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in sys.stdin:
            if line.startswith("key "):
                self.key_text = line[4:].strip()
                self.key_ready.set()
                continue
            if line.strip() == "nokey":
                self.key_ready.set()
                continue
            if line.strip() == "cancel":
                return self.request("the player")
        self.key_ready.set()
        self.request("the launcher closed")

    def request(self, by):
        if self.cancel.is_set() or self.finished.is_set():
            return
        self.cancel.set()
        self.events.emit("cancelling", by=by)
        threading.Thread(target=self._grace, daemon=True).start()

    def _grace(self):
        if self.finished.wait(CANCEL_GRACE_S):
            return
        # still blocked (a compile or a lift in flight): end here; the job ends the children
        self.events.emit("result", ok=False, code="cancelled", message="Cancelled.", data={"forced": True})
        self.events.close()
        hard_exit(EXIT_CANCELLED)

    def check(self):
        if self.cancel.is_set():
            raise pipeline.Cancelled()


def own_children():
    """Every process this worker starts ends with it: on Windows a job object that kills its processes when
    the worker's handle closes (at any exit, a crash included); elsewhere a process group of its own."""
    if os.name == "nt":
        import ctypes
        import ctypes.wintypes as w
        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        k32.CreateJobObjectW.restype = w.HANDLE
        k32.CreateJobObjectW.argtypes = (ctypes.c_void_p, w.LPCWSTR)
        k32.SetInformationJobObject.argtypes = (w.HANDLE, ctypes.c_int, ctypes.c_void_p, w.DWORD)
        k32.AssignProcessToJobObject.argtypes = (w.HANDLE, w.HANDLE)
        k32.GetCurrentProcess.restype = w.HANDLE

        class Basic(ctypes.Structure):
            _fields_ = [("a", ctypes.c_longlong), ("b", ctypes.c_longlong), ("LimitFlags", w.DWORD),
                        ("c", ctypes.c_size_t), ("d", ctypes.c_size_t), ("e", w.DWORD), ("f", ctypes.c_size_t),
                        ("g", w.DWORD), ("h", w.DWORD)]

        class Extended(ctypes.Structure):
            _fields_ = [("Basic", Basic), ("Io", ctypes.c_ulonglong * 6), ("i", ctypes.c_size_t),
                        ("j", ctypes.c_size_t), ("k", ctypes.c_size_t), ("l", ctypes.c_size_t)]
        job = k32.CreateJobObjectW(None, None)
        lim = Extended()
        # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, and BREAKAWAY_OK: only a process started with CREATE_BREAKAWAY_FROM_JOB
        # leaves it (remove-launcher's helper, which must outlive this worker)
        lim.Basic.LimitFlags = 0x2000 | 0x0800
        if job and k32.SetInformationJobObject(job, 9, ctypes.byref(lim), ctypes.sizeof(lim)):
            return bool(k32.AssignProcessToJobObject(job, k32.GetCurrentProcess()))
        return False
    try:
        os.setpgid(0, 0)
        return True
    except OSError:
        return False


def hard_exit(code):
    """End now with `code`, from any thread. Elsewhere than Windows, the processes of this worker's own group (its
    tools: a compile, a lift) are ended first -- each but this one, so no signal handler is needed (one can be set
    only on the main thread, and the forced cancel calls this from its grace thread)."""
    if os.name != "nt":
        import signal
        try:
            me = os.getpid()
            if os.getpgid(0) == me:
                for d in os.listdir("/proc"):
                    if d.isdigit() and int(d) != me:
                        try:
                            if os.getpgid(int(d)) == me:
                                os.kill(int(d), signal.SIGTERM)
                        except OSError:
                            pass
        except OSError:
            pass
    os._exit(code)   # Windows: the job's last handle closes with the process; its children end


def step_name(s):
    return pipeline.STEP_NAMES.get(s) or STEP_NAMES_M9.get(s, s)


class Callbacks:
    """the operations (op_install, op_update, op_migrate) driven in-process, without the event stream: the setup
    app until the launcher replaces it. cancel: a threading.Event, set to cancel."""

    def __init__(self, progress=None, log=None, skip=None, cancel=None):
        self._progress, self._log, self._skip = progress, log, skip
        self.control = self
        self.cancel = cancel or threading.Event()

    def check(self):
        if self.cancel.is_set():
            raise pipeline.Cancelled()

    def progress(self, step, frac, message):
        self.check()
        if self._progress:
            self._progress(step, frac, message)

    def log(self, text):
        self.check()
        if self._log:
            self._log(text)

    def skip(self, step):
        if self._skip:
            self._skip(step)


def installed_game(install):
    """the game a shortcut starts: game/ in an install with build slots, the setup app's beside its data"""
    lay = layout.Layout(install)
    if lay.kind() == "slots":
        lay.repair_game_link()
        return os.path.join(lay.game_link, layout.exe_name())
    return os.path.join(lay.root, layout.exe_name())


class Progress:
    """The pipeline's progress, log and skip callbacks -> stage and progress events: each step's fraction,
    the overall fraction (STAGE_SECONDS as weights, skipped steps leaving the total), elapsed time and, once
    measurable, the remaining time. Progress events at most every 0.1 s per step unless the step ends."""

    def __init__(self, events, control, stages, clock=time.monotonic):
        self.ev, self.control = events, control
        self.now = clock
        self.stages = list(stages)
        self.weight = {s: STAGE_SECONDS.get(s, 10.0) for s in self.stages}
        self.state = {s: "pending" for s in self.stages}
        self.took = {}
        self.cur = None
        self.cur_t0 = None
        self.frac = 0.0
        self.last_emit = -1.0
        self.t0 = self.now()

    def plan(self):
        self.ev.emit("plan", stages=[{"id": s, "name": step_name(s), "weight": self.weight[s]}
                                     for s in self.stages])

    def _end_current(self):
        if self.cur and self.state[self.cur] == "running":
            self.state[self.cur] = "done"
            self.took[self.cur] = round(self.now() - self.cur_t0, 2)
            self.ev.emit("stage", id=self.cur, state="done", seconds=self.took[self.cur])

    def _start(self, step):
        self._end_current()
        self.cur, self.cur_t0, self.frac = step, self.now(), 0.0
        self.state[step] = "running"
        self.ev.emit("stage", id=step, state="start", name=step_name(step))

    def overall(self):
        live = [s for s in self.stages if self.state[s] != "skipped"]
        total = sum(self.weight[s] for s in live) or 1.0
        done = sum(self.weight[s] for s in live if self.state[s] == "done")
        if self.cur and self.state[self.cur] == "running" and self.frac is not None:
            done += self.weight[self.cur] * self.frac
        return min(1.0, done / total)

    def speed(self):
        """this PC against the reference, from the compute-bound steps finished here (the disc's steps are
        bound by the drive, not the processor: they say nothing of the build's pace); None until one is done"""
        done = [s for s in self.stages if s in CPU_STAGES and self.state[s] == "done" and self.took.get(s, 0) > 1]
        ref = sum(self.weight[s] for s in done)
        return min(4.0, max(0.25, sum(self.took[s] for s in done) / ref)) if ref else None

    def eta(self):
        """(remaining seconds, basis) or (None, None) before any step has started. The current step: its own
        rate once 15% of it is measured, kept within a third and three times its weight's estimate; else its
        weight. The steps after it: their weights. Weights of compute-bound steps are scaled by this PC's speed
        once a compute-bound step has finished (basis "measured"); before that they are the reference PC's
        (basis "reference": the launcher says "about")."""
        if not self.cur or self.state.get(self.cur) != "running":
            return None, None
        sp = self.speed()
        scale = (lambda s: (sp if s in CPU_STAGES else 1.0)) if sp else (lambda s: 1.0)
        el = self.now() - self.cur_t0
        left_by_weight = self.weight[self.cur] * scale(self.cur) - el
        if self.frac is not None and self.frac >= 0.15 and el >= 1.0:
            by_rate = el * (1 - self.frac) / self.frac
            if left_by_weight <= 0:   # past its weight: its own rate alone
                cur = by_rate
            elif sp:   # both measured on this PC: half each (a step's rate runs in bursts: parallel compiles)
                cur = 0.5 * min(max(by_rate, left_by_weight / 3), left_by_weight * 3) + 0.5 * left_by_weight
            else:
                cur = min(max(by_rate, left_by_weight / 3), left_by_weight * 3)
            basis = "rate" if sp or not [s for s in self.stages if self.state[s] == "pending"] else "reference"
        else:
            cur = max(left_by_weight, 0.1 * el)
            basis = "measured" if sp else "reference"
        later = sum(self.weight[s] * scale(s) for s in self.stages if self.state[s] == "pending")
        if sp and basis != "rate":
            basis = "measured"
        return round(cur + later, 1), basis

    def progress(self, step, frac, message):
        self.control.check()
        if step != self.cur:
            self._start(step)
        self.frac = frac
        now = self.now()
        if now - self.last_emit < 0.1 and frac < 1.0:
            return
        self.last_emit = now
        eta, basis = self.eta()
        self.ev.emit("progress", stage=step, fraction=None if frac is None else round(frac, 4),
                     overall=round(self.overall(), 4), elapsed=round(now - self.t0, 1), eta=eta, eta_basis=basis,
                     detail=message)

    def skip(self, step):
        self.control.check()
        if step == self.cur or step not in self.state:
            return
        self.state[step] = "skipped"
        self.ev.emit("stage", id=step, state="skipped")

    def log(self, text):
        self.control.check()
        # the lifter's own counts give the recompile step a fraction ("... lifted 1200/9900 functions")
        if self.cur == "recomp":
            m = None
            for key, base, span in (("... lifted ", 0.0, 0.85), ("... emitting ", 0.85, 0.15)):
                at = text.find(key)
                if at >= 0:
                    m = (text[at + len(key):].split()[0], base, span)
            if m and "/" in m[0]:
                try:
                    d, n = (int(x) for x in m[0].split("/"))
                    if n:
                        self.progress("recomp", m[1] + m[2] * d / n, pipeline.STEP_NAMES["recomp"])
                except ValueError:
                    pass
        self.ev.emit("log", level="info", text=text[:2000])

    def finish(self):
        self._end_current()
        return {s: self.took.get(s) for s in self.stages}, [s for s in self.stages if self.state[s] == "skipped"]


def disk_full(e):
    return isinstance(e, OSError) and (e.errno in DISK_FULL_ERRNO or getattr(e, "winerror", None) in
                                       DISK_FULL_WINERROR)


def free_bytes(path):
    p = os.path.abspath(path)
    while not os.path.exists(p):
        up = os.path.dirname(p)
        if up == p:
            break
        p = up
    return shutil.disk_usage(p).free, p


# ---- the install's temporary folder (the portable root) ----
#
# Given --install, the commands that build, prepare, download or report (TEMP_COMMANDS) keep their temporary files,
# and their children's (the lifting tools, the compiler, the game's checks), in <install>/data/tmp/worker-<pid>:
# tempfile's folder and TMP, TEMP and TMPDIR. Its own folder per worker, so two workers on one install (a background
# preparation beside an update check) never touch each other's files; it is removed as the worker ends, and one left
# by a worker that is gone (ended by force, a crash) is removed by the next one -- only those, never anything else in
# data/tmp. Made on demand: never for a folder that is not an install yet (but `install`'s own), never for the
# commands that only look (space, info) or remove (uninstall, remove-launcher).
TEMP_COMMANDS = ("install", "update", "migrate", "prepare", "download-update", "stage-launcher", "diagnostics",
                 "selftest")
TEMP_VARS = ("TMP", "TEMP", "TMPDIR")
TEMP_PREFIX = "worker-"
_temp = {"dir": None, "saved": None, "tempdir": None}


def clean_install_temp(base):
    """the folders of workers that are gone, in an install's data/tmp; returns their names"""
    gone = []
    try:
        names = os.listdir(base)
    except OSError:
        return gone
    for n in names:
        pid = n[len(TEMP_PREFIX):]
        if not (n.startswith(TEMP_PREFIX) and pid.isdigit()) or int(pid) == os.getpid() or updates._alive(int(pid)):
            continue
        p = os.path.join(base, n)
        if os.path.isdir(p) and not layout.is_link(p):
            try:
                layout.rmtree(p)
                gone.append(n)
            except OSError:
                pass
    return gone


def use_install_temp(install, create_root=False):
    """this worker's temporary folder in the install (made on demand), for it and its children; None when the install
    is not there (and not to be made) or can't be written -- then nothing changes, and the command's own checks say
    why"""
    root = os.path.abspath(install)
    if not create_root and not os.path.isdir(root):
        return None
    base = layout.Layout(root).tmp
    mine = os.path.join(base, f"{TEMP_PREFIX}{os.getpid()}")
    try:
        os.makedirs(base, exist_ok=True)
        clean_install_temp(base)
        os.makedirs(mine, exist_ok=True)
    except OSError:
        return None
    if _temp["saved"] is None:
        _temp["saved"] = {k: os.environ.get(k) for k in TEMP_VARS}
        _temp["tempdir"] = tempfile.tempdir
    _temp["dir"] = mine
    tempfile.tempdir = mine
    for k in TEMP_VARS:
        os.environ[k] = mine
    return mine


def release_install_temp():
    """the temporary folder removed (what is still held in it stays for the next worker), the environment as it was"""
    mine = _temp["dir"]
    if _temp["saved"] is not None:
        for k, v in _temp["saved"].items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
        tempfile.tempdir = _temp["tempdir"]
    _temp.update(dir=None, saved=None, tempdir=None)
    if mine and os.path.isdir(mine) and not layout.is_link(mine):
        shutil.rmtree(mine, ignore_errors=True)


def system_temp():
    """the operating system's temporary folder, as this worker found it (remove-launcher's helper, the exception)"""
    if _temp["saved"] is not None:
        for k in ("TMPDIR", "TEMP", "TMP"):
            v = _temp["saved"].get(k)
            if v and os.path.isdir(v):
                return os.path.abspath(v)
    return os.path.abspath(tempfile.gettempdir())


def check_writable(root, create=False):
    """the install's root can be written (create: made first when it is not there yet), else a clear result:
    root_not_writable (permissions, a read-only drive or folder, a drive that can't be reached) or disk_space (a
    full drive) -- before anything is changed"""
    root = os.path.abspath(root)

    def refused(e=None, what="can't be written to"):
        if e is not None and disk_full(e):
            return pipeline.SetupError("The install's drive is full.", "disk_space", str(e))
        why = f" ({e.strerror or e})" if e is not None else ""
        return pipeline.SetupError(f"The install folder {what}{why}: choose another folder, or allow writing to "
                                   "this one.", "root_not_writable", f"{root}: {e!r}" if e is not None else root)
    if create:
        try:
            os.makedirs(root, exist_ok=True)
        except OSError as e:
            raise refused(e, "can't be made")
    if not os.path.isdir(root):
        raise pipeline.SetupError("There is no install folder there.", "no_install", root)
    if not os.access(root, os.W_OK):
        raise refused()
    try:
        _write_probe(root)
    except OSError as e:
        raise refused(e)


def _write_probe(root):
    """a file made, written and removed in root (what os.access can't tell on Windows: its permissions, a read-only
    drive)"""
    probe = os.path.join(root, f".write-test-{os.getpid()}")
    try:
        fd = os.open(probe, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
        try:
            os.write(fd, b"ok")
        finally:
            os.close(fd)
    finally:
        try:
            os.remove(probe)
        except OSError:
            pass


# ---------------- commands ----------------

def cmd_version(a, ev, control):
    from version import VERSION
    return {"worker": VERSION, "protocol": PROTOCOL, "platform": sys.platform,
            "game": pipeline.version_info()["title_id"]}


def disc_key(a):
    """the disc key the player gave (--disc-key: a .dkey or IRD file, or a file holding the 32 digits typed), or None;
    an IRD's needs the D1 line of the keys file (--keys). The key itself is never logged or returned."""
    if not getattr(a, "disc_key", None):
        return None
    import ps3_disc
    if not os.path.isfile(a.disc_key):   # --disc-key is always a file (the window writes a typed key to one): a path
        raise pipeline.SetupError("The disc key file wasn't found.", "bad_key_file")   # isn't read as typed digits
    try:
        return ps3_disc.load_key(os.path.abspath(a.disc_key), os.path.abspath(a.keys) if a.keys else None)
    except ps3_disc.DiscKeyError as e:
        raise pipeline.SetupError(str(e), e.code)


def cmd_probe_disc(a, ev, control):
    out = pipeline.probe_disc(os.path.abspath(a.disc), disc_key(a))
    out["encrypted"] = bool(getattr(a, "disc_key", None))
    return out


def cmd_probe_keys(a, ev, control):
    k = os.path.abspath(a.keys)
    elf = pipeline.is_elf(k)
    return pipeline.probe_keys(os.path.abspath(a.disc), None if elf else k, k if elf else None, disc_key(a))


def cmd_space(a, ev, control):
    free, at = free_bytes(a.install)
    need = NEEDED[a.op]
    return {"free": free, "needed": need, "enough": free >= need, "checked_at": at}


def install_report(install):
    """what an install folder holds (info's data; find-legacy's for each folder it finds): only read"""
    install = os.path.abspath(install)
    lay = layout.Layout(install)
    info = pipeline.install_info(install) or {}
    st = info.get("stamps", {})
    out = lay.summary()
    out.update(version=info.get("version"), disc_copy=os.path.exists(pipeline.Paths(install).elf),
               lift=info.get("lift"),
               # an install interrupted before its first build: the disc's steps it finished (install --resume)
               interrupted=None if out["kind"] != "partial" else
               {"source": info.get("source"), "done": [s for s in ("verify", "extract", "decrypt") if st.get(s)]})
    return out


def cmd_info(a, ev, control):
    return install_report(a.install)


def check_space(install, need):
    try:
        free, _ = free_bytes(install)
    except OSError as e:   # the drive can't be reached (removed, a network drive gone): a result, not a trace
        raise pipeline.SetupError(f"The free space on the install's drive can't be checked ({e.strerror or e}).",
                                  "root_not_writable", repr(e))
    if free < need:
        raise pipeline.SetupError(f"Not enough free space on the install's drive: about {need / 2**30:.1f} GB "
                                  f"is needed, {free / 2**30:.1f} GB is free.", "disk_space",
                                  json.dumps({"free": free, "needed": need}))


# A new build is checked before it becomes active (docs/M9_LAUNCHER_PLAN.md section 8.4): its own report
# (`dotheroes --info`: FFmpeg loads beside it, the native release) and its memory test. The environment the game
# reads its developers' switches from is not passed on.
VALIDATE_TIMEOUT_S = 120


def validate_build(slot, log):
    exe = os.path.join(slot, layout.exe_name())
    env = {k: v for k, v in os.environ.items() if not k.startswith(("DOTHEROES_", "PS3_", "PS3RECOMP_"))}
    # the game's report and messages in UTF-8 (a path in them may hold any character), whatever the code page
    kw = dict({"creationflags": 0x08000000} if os.name == "nt" else {}, encoding="utf-8", errors="replace")
    try:
        r = subprocess.run([exe, "--info"], capture_output=True, text=True, timeout=VALIDATE_TIMEOUT_S, env=hostenv.host_env(env),
                           stdin=subprocess.DEVNULL,
                           cwd=slot, **kw)
        info = json.loads(r.stdout.strip().splitlines()[-1]) if r.stdout.strip() else {}
    except (OSError, subprocess.TimeoutExpired, ValueError) as e:
        raise pipeline.SetupError("The new build doesn't start.", "build_invalid", repr(e))
    if not info.get("native_release") or not info.get("ffmpeg", {}).get("ok"):
        raise pipeline.SetupError("The new build is incomplete: " + ("FFmpeg doesn't load beside it" if not
                                  info.get("ffmpeg", {}).get("ok") else "not the native release") + ".",
                                  "build_invalid", json.dumps(info))
    try:
        m = subprocess.run([exe, "--memtest"], capture_output=True, text=True, timeout=VALIDATE_TIMEOUT_S, env=hostenv.host_env(env),
                           stdin=subprocess.DEVNULL,
                           cwd=slot, **kw)
    except (OSError, subprocess.TimeoutExpired) as e:
        raise pipeline.SetupError("The new build's self-test didn't finish.", "build_invalid", repr(e))
    if m.returncode:
        raise pipeline.SetupError("The new build failed its self-test.", "build_invalid",
                                  (m.stdout + m.stderr)[-3000:])
    last = [ln for ln in (m.stdout + m.stderr).splitlines() if "checks" in ln][-1:]
    log("checked: " + info.get("ffmpeg", {}).get("what", "") + "; " + (last[0] if last else "memory test passed"))
    return {"info": info, "memtest": last[0] if last else "passed"}


# The steps of a build into a new slot, after the conversion's
BUILD_TAIL = ["validate", "activate", "prepare"]
STEP_NAMES_M9 = {"validate": "Checking the new build", "activate": "Making it the active build",
                 "prepare": "Preparing shaders and pipelines", "copy": "Copying the installation",
                 "check-copy": "Checking the copy", "place": "Putting it in place"}


def build_slot(lay, prog, source=None, keys=None, elf=None, full=False, resume=False, validate=validate_build,
               disc_key=None):
    """the conversion (or a rebuild) into a staged slot, checked, recorded, promoted and made active; any failure
    leaves the active build as it was (the staged slot removed). Returns (build id, info, changed)."""
    lay.ensure_dirs()
    lay.clean_staging()
    active = lay.read_active()["active"] if os.path.exists(lay.active_json) else None
    same_as = None if full or not active or lay.check_build(active) else (lay.build_info(active) or {}).get("inputs")
    staging = lay.stage()
    try:
        made = pipeline.run(lay.root, source=source, keys=keys, elf=elf, progress=prog.progress, log=prog.log,
                            skip=prog.skip, full=full, out_dir=staging, keep_objects=lay.kept_objects(), resume=resume,
                            unchanged_inputs=same_as, disc_key=disc_key)
        prog.control.check()
        if made.get("unchanged"):   # the active build is made of exactly this: nothing to build or switch
            layout.rmtree(staging)
            prog.log(f"nothing changed: the active build ({active}) is made of the same lift, objects and libraries")
            return active, lay.build_info(active), False
        prog.progress("validate", 0.0, STEP_NAMES_M9["validate"])
        checked = validate(staging, prog.log)
        prog.control.check()
        prog.progress("activate", 0.0, STEP_NAMES_M9["activate"])
        info = lay.record(staging, {k: made[k] for k in ("lift", "objects", "shader_cache", "version", "inputs")})
        info["checked"] = checked
        layout.write_json(os.path.join(staging, "build.json"), info)
        before = lay.read_active()["active"] if os.path.exists(lay.active_json) else None
        bid = lay.promote(staging, info)
        lay.activate(bid)
        refresh_game_appimage(lay, prog.log)   # M11.4: the player's game AppImage follows the active build
        prog.progress("activate", 1.0, STEP_NAMES_M9["activate"])
        return bid, lay.build_info(bid), bid != before
    except BaseException:
        if os.path.isdir(staging):
            try:
                layout.rmtree(staging)
            except OSError:   # a file in it still held (GitHub #5): clean_staging removes it at the next start, and
                pass          # the failure the player sees stays the one that stopped the build
        raise


def op_install(install, source, keys, elf, prog, resume=False, validate=validate_build, prepare=True, disc_key=None):
    """a first install, a reinstall or a repair from the disc: the disc's steps, then a new build (the saves, the
    settings and the kept builds untouched)"""
    lay = layout.Layout(install)
    if lay.kind() == "setup-app":
        raise pipeline.SetupError("This folder holds an install made by the setup app: migrate it (no need to "
                                  "choose the disc again).", "needs_migration")
    check_writable(install, create=True)
    check_space(install, NEEDED["install"])
    bid, info, _ = build_slot(lay, prog, source, keys, elf, resume=resume, validate=validate, disc_key=disc_key)
    game_settings.settle_after_conversion(install)
    pruned = lay.prune()
    prepared = prepare_after(lay, prog, validate, prepare)
    return {"build": bid, "exe": os.path.join(lay.game_link, layout.exe_name()), "pruned": pruned["removed"],
            "prepared": prepared}


# M9.2: the shaders and pipelines made before play (the game's `--prepare`: its recipes, recorded as the game
# first makes each pipeline, translated by this build and made through its pipeline cache). After an install, a
# repair or an update -- when there are recipes (a first install's: the manifest's, below) -- and whenever the
# launcher's check finds the cache not valid for this GPU and driver.
PREPARE_TIMEOUT_S = 1800
# The first session (docs/M9_LAUNCHER_PLAN.md section 15): the pipelines the project's test routes met, listed by
# where their programs are on the disc; resolved against the install's own copy of the disc into its recipes, so a
# first install prepares them before it is played. No list, or a disc that differs: nothing added.
MANIFEST = os.path.join(pipeline.config_dir(), "pipeline_manifest.txt")
# M9.2b/c (docs/M92_SHADER_PLAN.md): the comprehensive preparation -- every program on the install's own copy of the
# disc translated, and a pipeline for each shader package record by its group's pass profiles (numbers learned from
# the project's recordings: tools/pipeline_profiles.py), besides the recipes. Its figures are kept beside the
# pipeline cache (PREPARED) for the check and the Shaders page.
PROFILES = os.path.join(pipeline.config_dir(), "pipeline_profiles.txt")
PREPARED = "prepare-result.json"


def comprehensive(lay):
    """whether the comprehensive preparation can run: the profiles shipped, the install's disc copy present"""
    return os.path.exists(PROFILES) and os.path.isdir(os.path.join(lay.disc, "PS3_GAME", "USRDIR"))


def first_session_recipes(lay, prog):
    """the manifest's pipelines added to the install's recipes (those it lacks); the counts, or None"""
    if not os.path.exists(MANIFEST) or not os.path.isdir(lay.disc):
        return None
    try:
        return pipeline_manifest.resolve(MANIFEST, lay.disc, os.path.join(lay.cache, "native-recipes.bin"))
    except (OSError, ValueError) as e:
        prog.log(f"the pipeline manifest was not used: {e}"[:2000])
        return {"error": str(e)}


def op_prepare(install, prog, check=False, rebuild=False):
    """the active build's `--prepare`: --check says whether the cache is valid for this GPU and driver (nothing
    made; with the recipes counted, the manifest's not yet added, and the disk the shaders use). Else, under the
    install's preparation lock (one at a time, whoever starts it), the manifest's pipelines added to the recipes,
    then the recipes prepared in parts -- each part's pipeline cache saved as it ends, so a preparation stopped
    part-way keeps what it made and the next one goes quickly over those (resuming where it stopped)."""
    lay = layout.Layout(install)
    if lay.kind() != "slots":
        raise pipeline.SetupError("This folder has no install with build slots.", "no_install")
    if not check:   # it writes the recipes, the shader folder and its lock
        check_writable(install)
    if check:
        out = _run_prepare(lay, prog, ["--check"], None, None)
        out.update(prepare_disk(lay))
        out["comprehensive"] = comprehensive(lay)
        try:   # the last comprehensive preparation's figures, kept with the cache they were made into
            with open(os.path.join(shader_folder(lay), PREPARED), encoding="utf-8") as f:
                done = json.load(f)
            out["comprehensive_done"] = bool(done.get("package", {}).get("made"))
            out["coverage"] = {k: done.get(k) for k in ("pipelines", "translations", "recorded", "from_package",
                                                         "package_records", "failed_all")}
        except (OSError, ValueError):
            out["comprehensive_done"] = False
        return out
    with PrepareLock(lay):
        listed = first_session_recipes(lay, prog)
        if rebuild:   # the manual rebuild: the active build's translated shaders and pipeline cache deleted, all made
            sf = shader_folder(lay)   # again from the recipes (kept: they are the list of what to prepare)
            if os.path.isdir(sf) and not layout.is_link(sf):
                for n in os.listdir(sf):
                    q = os.path.join(sf, n)
                    if os.path.isfile(q) and not layout.is_link(q):
                        os.remove(q)
        recipes = os.path.join(lay.cache, "native-recipes.bin")
        if comprehensive(lay):   # one run: the recipes, the disc's programs and the package's records
            prog.progress("prepare", 0.0, step_name("prepare"))
            result = _run_prepare(lay, prog, ["--static", PROFILES], None, (0, 0))
            pk = result.get("package") or {}
            sh = result.get("shaders") or {}
            result["recorded"] = result.get("made", 0)
            result["from_package"] = pk.get("made", 0)
            result["package_records"] = pk.get("records", 0)
            result["pipelines"] = result.get("made", 0) + pk.get("made", 0)
            result["failed_all"] = result.get("failed", 0) + pk.get("failed", 0) + sh.get("spirv_failed", 0) + \
                sh.get("text_failed", 0) + sh.get("disc_failed", 0)
            result["translations"] = sh.get("translations", 0)
            if listed is not None:
                result["manifest"] = listed
            if result.get("exit") == 0:
                os.makedirs(shader_folder(lay), exist_ok=True)
                tmp = os.path.join(shader_folder(lay), PREPARED + ".part")
                with open(tmp, "w", encoding="utf-8") as f:
                    json.dump({k: v for k, v in result.items() if k != "failures"}, f)
                os.replace(tmp, os.path.join(shader_folder(lay), PREPARED))
            result.update(prepare_disk(lay))
            return result
        records = pipeline_manifest.read_recipes(recipes)
        prog.progress("prepare", 0.0, step_name("prepare"))
        part_file = os.path.join(lay.cache, "prepare-part.bin")
        size = max(PREPARE_PART, -(-len(records) // 8))
        parts = [records[i:i + size] for i in range(0, len(records), size)] or [[]]
        total, done, result = len(records), 0, None
        try:
            for part in parts:
                with open(part_file, "wb") as f:
                    for _, head, vp, fp in part:
                        f.write(head + vp + fp + pipeline_manifest.struct.pack(
                            "<I", pipeline_manifest.recipe_sum(head, vp, fp)))
                r = _run_prepare(lay, prog, [], part_file, (done, total))
                done += len(part)
                if result is None:
                    result = dict(r)
                else:
                    for k in ("made", "failed", "translated", "from_cache"):
                        result[k] = result.get(k, 0) + r.get(k, 0)
                    result["ms"] = round(result.get("ms", 0) + r.get("ms", 0), 1)
                    result["cache_mb"], result["gpu"], result["exit"] = r.get("cache_mb"), r.get("gpu"), r.get("exit")
        finally:
            if os.path.exists(part_file):
                os.remove(part_file)
        result["recipes"], result["parts"] = total, len(parts)
        if listed is not None:
            result["manifest"] = listed
        result.update(prepare_disk(lay))
        return result


PREPARE_PART = 32   # recipes per part (at least; at most eight parts)


def shader_folder(lay):
    sid = (lay.build_info(lay.read_active()["active"]) or {}).get("shader_cache") or "default"
    return os.path.join(lay.shaders, sid)


def prepare_disk(lay):
    """what the shaders use on disk: the active build's shader folder (its translations and pipeline cache) and
    the recipes; with --check, the manifest's pipelines not yet among the recipes"""
    def size(path):
        if os.path.isfile(path):
            return os.path.getsize(path)
        return sum(os.path.getsize(os.path.join(d, f)) for d, _, fs in os.walk(path) for f in fs)
    out = {"disk_mb": round((size(shader_folder(lay)) + size(os.path.join(lay.cache, "native-recipes.bin"))) / 2**20, 2)}
    if os.path.exists(MANIFEST):
        try:
            out["pending"] = pipeline_manifest.resolve(MANIFEST, lay.disc, os.path.join(lay.cache, "native-recipes.bin"),
                                                       dry_run=True)["added"]
        except (OSError, ValueError):
            pass
    return out


class PrepareLock:
    """one preparation at a time per install, whoever starts it (the launcher twice, the launcher and an update):
    data/cache/prepare.lock holding the preparing worker's process id; a lock whose process is gone is taken over"""

    def __init__(self, lay):
        self.path = os.path.join(lay.cache, "prepare.lock")

    def __enter__(self):
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        for _ in range(3):
            try:
                fd = os.open(self.path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            except FileExistsError:
                try:
                    with open(self.path) as f:
                        pid = int(f.read().strip() or 0)
                except (OSError, ValueError):
                    pid = 0
                if pid and pid != os.getpid() and updates._alive(pid):
                    raise pipeline.SetupError("Shaders are already being prepared for this install.", "already_preparing")
                try:
                    os.remove(self.path)   # its process is gone
                except OSError:
                    pass
                continue
            os.write(fd, str(os.getpid()).encode())
            os.close(fd)
            return self
        raise pipeline.SetupError("Shaders are already being prepared for this install.", "already_preparing")

    def __exit__(self, *exc):
        try:
            os.remove(self.path)
        except OSError:
            pass
        return False


def _run_prepare(lay, prog, extra, recipes, at):
    """one run of the active build's --prepare (recipes: a file of them, else the install's); at = (done before
    this run, the whole preparation's total) for the progress"""
    exe = lay.active_exe()   # the build itself, not game/ (a moved folder's link may still point at the old place)
    env = {k: v for k, v in os.environ.items() if not k.startswith(("DOTHEROES_", "PS3_", "PS3RECOMP_"))}
    if recipes:
        env["DOTHEROES_RECIPES"] = recipes
    kw = {"creationflags": 0x08000000} if os.name == "nt" else {}
    os.makedirs(lay.logs, exist_ok=True)
    result = None
    with open(os.path.join(lay.logs, "prepare.log"), "a" if at and at[0] else "w", encoding="utf-8",
              errors="replace") as err:
        p = subprocess.Popen([exe, "--prepare"] + extra, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=err,
                             text=True, encoding="utf-8", errors="replace", cwd=lay.root, env=hostenv.host_env(env),
                             **kw)
        try:
            for ln in p.stdout:
                try:
                    e = json.loads(ln)
                except ValueError:
                    continue
                if e.get("ev") == "prepare" and at == (0, 0):   # one run: the game's own total
                    d, n = e["done"], max(1, e.get("total", 0))
                    prog.progress("prepare", d / n, f"{d} of {n} shaders and pipelines")
                elif e.get("ev") == "prepare" and at:
                    d, n = at[0] + e["done"], max(1, at[1])
                    prog.progress("prepare", d / n, f"{d} of {at[1]} pipelines")
                elif e.get("ev") == "prepared":
                    result = e.get("result")
            p.wait(timeout=PREPARE_TIMEOUT_S)
        except BaseException:
            p.kill()
            p.wait()
            raise
    if not isinstance(result, dict) or "error" in result:
        with open(os.path.join(lay.logs, "prepare.log"), encoding="utf-8", errors="replace") as f:
            last = f.read()[-3000:]
        raise pipeline.SetupError("Preparing the shaders and pipelines failed" + (
            f": {result['error']}." if isinstance(result, dict) and result.get("error") else "."), "prepare_failed",
            last)
    result["exit"] = p.returncode
    return result


def prepare_after(lay, prog, validate, enabled=True):
    """after a build became active: a preparation when there are recipes (the manifest's added first); its failure
    is reported, never the operation's (the game still prepares what it meets). Not with the tests' stand-in
    builds (validate given), nor when the launcher asked to prepare in the background itself (--no-prepare)."""
    if validate is not validate_build or not enabled:
        prog.skip("prepare")
        return None
    first_session_recipes(lay, prog)
    if not os.path.exists(os.path.join(lay.cache, "native-recipes.bin")):
        prog.skip("prepare")
        return None
    try:
        return op_prepare(lay.root, prog)
    except pipeline.SetupError as e:
        prog.log(f"preparation: {e} ({e.details or ''})"[:2000])
        return {"error": str(e)}


def op_update(install, prog, full=False, validate=validate_build, prepare=True):
    """the game rebuilt from the install's own copy of the disc into a new slot (only what changed redone), the
    saves backed up first; the build before it kept for rollback"""
    lay = layout.Layout(install)
    if lay.kind() != "slots":
        raise pipeline.SetupError("This folder has no active build to update" + (": migrate it first." if lay.kind()
                                  == "setup-app" else "."), "needs_migration" if lay.kind() == "setup-app" else
                                  "no_install")
    check_writable(install)
    check_space(install, NEEDED["update"])
    backup = lay.backup_saves("before-update") if os.path.isdir(lay.gamedata) and os.listdir(lay.gamedata) else None
    bid, info, changed = build_slot(lay, prog, full=full, validate=validate)
    game_settings.settle_after_conversion(install)
    pruned = lay.prune()
    lay.prune_backups()
    prepared = prepare_after(lay, prog, validate, prepare) if changed else prog.skip("prepare")
    return {"build": bid, "changed": changed, "saves_backup": backup and backup["id"], "pruned": pruned["removed"],
            "prepared": prepared}


def op_migrate(install, prog, validate=validate_build, prepare=True):
    """the setup app's install into the layout with build slots, the disc not asked for again and nothing redone
    that is still valid: the current build made from the install's own lift and objects (when the tools are the
    same, a link with every object reused), then the data moved and the old game removed"""
    lay = layout.Layout(install)
    if lay.kind() != "setup-app":
        raise pipeline.SetupError("This folder isn't an install made by the setup app.", "not_setup_app")
    check_writable(install)
    backup = lay.backup_saves("before-migration")
    info = pipeline.install_info(install) or {}
    st = info.get("stamps", {})
    elf_sha = info.get("elf_sha256")
    old_lift = None
    if elf_sha and st.get("recomp") == pipeline.tool_fingerprint("recomp", elf_sha) and \
            st.get("spu") == pipeline.tool_fingerprint("spu", elf_sha):
        old_lift = pipeline.lift_id(elf_sha)   # the same tools made it: kept, not lifted again
    lay.ensure_dirs()
    moved = lay.move_legacy_data(old_lift, "setup-app")
    if old_lift:
        with open(os.path.join(lay.gen_root, old_lift, "done.json"), "w") as f:
            json.dump({"lift": old_lift, "elf_sha256": elf_sha, "migrated": True}, f)
    bid, binfo, _ = build_slot(lay, prog, validate=validate)
    legacy_shaders = os.path.join(lay.shaders, "setup-app")
    mine = os.path.join(lay.shaders, binfo.get("shader_cache") or "default")
    if os.path.isdir(legacy_shaders):   # the translations it made: the game keeps those of its own translator
        os.makedirs(mine, exist_ok=True)
        for n in os.listdir(legacy_shaders):
            if not os.path.exists(os.path.join(mine, n)):
                os.replace(os.path.join(legacy_shaders, n), os.path.join(mine, n))
        layout.rmtree(legacy_shaders)
        moved.append(f"data/cache/shaders/setup-app -> data/cache/shaders/{os.path.basename(mine)}")
    removed = lay.remove_legacy_game()
    game_settings.settle_after_conversion(install)
    lay.prune()
    prepare_after(lay, prog, validate, prepare)
    return {"build": bid, "lift_kept": bool(old_lift), "moved": moved, "removed": removed,
            "saves_backup": backup["id"], "exe": os.path.join(lay.game_link, layout.exe_name()),
            "shortcuts": "the old shortcuts started the setup app's game: make them again (they start game/)"}


def _progress(ev, control, steps):
    prog = Progress(ev, control, steps)
    prog.plan()
    return prog


def cmd_install(a, ev, control):
    k = os.path.abspath(a.keys)
    elf = pipeline.is_elf(k)
    prog = _progress(ev, control, pipeline.STEPS + BUILD_TAIL)
    out = op_install(os.path.abspath(a.install), os.path.abspath(a.disc), None if elf else k, k if elf else None,
                     prog, resume=a.resume, prepare=not a.no_prepare, disc_key=disc_key(a))
    out["seconds"], out["skipped"] = prog.finish()
    return out


def cmd_update(a, ev, control):
    prog = _progress(ev, control, [s for s in pipeline.STEPS if s not in ("verify", "extract", "decrypt")] +
                     BUILD_TAIL)
    out = op_update(os.path.abspath(a.install), prog, full=a.full, prepare=not a.no_prepare)
    out["seconds"], out["skipped"] = prog.finish()
    return out


def cmd_migrate(a, ev, control):
    prog = _progress(ev, control, [s for s in pipeline.STEPS if s not in ("verify", "extract", "decrypt")] +
                     BUILD_TAIL)
    out = op_migrate(os.path.abspath(a.install), prog, prepare=not a.no_prepare)
    out["seconds"], out["skipped"] = prog.finish()
    return out


# ---- the portable root: an existing installation copied into it; the old installations found ----

MIGRATE_ROOT_STEPS = ["copy", "check-copy", "place"]


def op_migrate_root(old, new, prog, dry_run=False):
    """an existing installation (old) COPIED into the root new (tools/root_migration.py): refused when they overlap,
    when new already holds an install, when old isn't one (or is the setup app's: migrate it first), while the game
    runs from old or a preparation holds its lock, when new can't be written or lacks the space -- all before anything
    is written. dry_run: what would be copied and whether there is room, nothing written. old is only ever read."""
    import uninstall
    try:
        p = root_migration.plan(old, new)
        if dry_run:
            try:
                free, at = free_bytes(new)
            except OSError as e:
                raise pipeline.SetupError(f"The free space on the new folder's drive can't be checked "
                                          f"({e.strerror or e}).", "root_not_writable", repr(e))
            p.update(free=free, enough=free >= p["needed"], checked_at=at)
            return p
        try:
            uninstall._check_idle(layout.Layout(old))
        except uninstall.UninstallError as e:
            raise pipeline.SetupError(str(e), e.code)
        check_writable(new, create=True)
        check_space(new, p["needed"])
        use_install_temp(new)
        return root_migration.migrate(old, new, progress=prog.progress, log=prog.log)
    except root_migration.MigrationError as e:
        raise pipeline.SetupError(str(e), e.code, e.details)


def cmd_migrate_root(a, ev, control):
    """--from OLD_ROOT --install NEW_ROOT [--dry-run]: an existing installation copied into the (portable) root"""
    prog = _progress(ev, control, [] if a.dry_run else MIGRATE_ROOT_STEPS)
    out = op_migrate_root(os.path.abspath(a.from_root), os.path.abspath(a.install), prog, dry_run=a.dry_run)
    out["seconds"], _ = prog.finish()
    return out


def cmd_find_legacy(a, ev, control):
    """read only: the old launcher's preferences (and the setup app's settings), the install each names, the old
    default install folders, and what each holds (info's report); --install: the root in use now (never offered)"""
    return root_migration.find_legacy(install_report, current=a.install)


def _layout(a, kind="slots"):
    lay = layout.Layout(os.path.abspath(a.install))
    if kind and lay.kind() != kind:
        raise pipeline.SetupError("This folder has no install with build slots.", "no_install")
    lay.repair_game_link()   # M11.3: a moved or copied folder's game/ points at the old place until repaired here
    return lay


def cmd_builds(a, ev, control):
    lay = _layout(a)
    st = lay.read_active()
    return {"active": st["active"], "previous": st["previous"], "retention": st["retention"],
            "builds": [dict(info, id=bid, state="active" if bid == st["active"] else "kept" if bid in st["previous"]
                            else "unused", damaged=lay.check_build(bid))
                       for bid, info in sorted(lay.builds().items())]}


def refresh_game_appimage(lay, log=print):
    """M11.4 (the owner's D2): the player's game AppImage (<root>/exports/) made again from the active build -- after
    an install, an update or a rollback. Linux only. Never fails the operation that changed the build: the game is
    installed either way; a failure is logged and returned."""
    if os.name == "nt":
        return {"made": False, "why": "Windows"}
    try:
        import game_appimage
        return {"made": True, "path": game_appimage.make(lay, log)}
    except Exception as e:   # noqa: BLE001 -- the build changed all the same
        log(f"the game's AppImage was not made again: {e}")
        return {"made": False, "why": str(e)}


def cmd_rollback(a, ev, control):
    lay = _layout(a)
    st = lay.rollback(a.to)
    ai = refresh_game_appimage(lay)
    return {"active": st["active"], "previous": st["previous"], "game_appimage": ai}


def cmd_game_appimage(a, ev, control):
    """the player's game AppImage made again from the active build (Linux; normally done by install, update and
    rollback themselves)"""
    lay = _layout(a)
    if os.name == "nt":
        raise pipeline.SetupError("The game's AppImage is Linux's.", "unsupported")
    import game_appimage
    return {"path": game_appimage.make(lay)}


def cmd_retention(a, ev, control):
    lay = _layout(a)
    st = lay.set_retention(a.keep)
    return {"retention": st["retention"], "previous": st["previous"], "pruned": lay.prune()["removed"]}


# ---- M9.3: updates (tools/updates.py, tools/release_trust.py; docs/RELEASE_SIGNING.md) ----

def _update_error(e):
    return pipeline.SetupError(str(e), e.code)


def _keys(a):
    """the keys trusted: config/release_keys.json, or --trusted-keys FILE (tests; the owner's trial of a new key)"""
    import release_trust
    return release_trust.trusted_keys(a.keys_file) if a.keys_file else None


def _managed_elsewhere(*folders):
    """0.2.0-beta.5: RecompLauncher manages this installation (it started us: RECOMPLAUNCHER=1; or its mark is at the
    root, install_layout.RECOMP_MARK): its updates are RecompLauncher's, so this launcher's own updater stays off --
    the two must never change the same folder (the SDK's rule). The launcher says so first; this is the worker's own
    check of the same rule."""
    import install_layout as layout
    return os.environ.get("RECOMPLAUNCHER") == "1" or any(
        f and os.path.isfile(os.path.join(f, layout.RECOMP_MARK)) for f in folders)


def _refuse_managed(*folders):
    if _managed_elsewhere(*folders):
        raise pipeline.SetupError("RecompLauncher installs this installation's updates, so this launcher's own updater "
                                  "is off.", "managed_elsewhere")


def cmd_check_update(a, ev, control):
    """the newest release of the channel, its manifest verified; never an error for being offline (the result says
    offline, play goes on)"""
    import updates
    from version import VERSION
    if _managed_elsewhere(a.install, a.launcher):
        return {"available": False, "managed": "RecompLauncher", "installed": a.installed or VERSION}
    try:
        r = updates.check(a.channel, a.installed or VERSION, a.source, _keys(a))
    except updates.trust.ReleaseError as e:
        if e.code == "offline":
            return {"available": False, "offline": True, "why": str(e)}
        raise _update_error(e)
    for k in ("manifest", "manifest_bytes", "signature_bytes"):
        r.pop(k, None)
    r["installed"] = a.installed or VERSION
    return r


def cmd_download_update(a, ev, control):
    """the platform's download of the newest release of the channel (its manifest verified again), resumed where a
    previous attempt stopped, checked against the manifest (size, SHA-256) before it is kept"""
    import updates
    from version import VERSION
    _refuse_managed(a.install, a.launcher)
    check_writable(a.dest or a.install, create=True)   # the download goes into the install (downloads/<version>)
    if updates.installed_kind() == "appimage" and os.environ.get("APPIMAGE"):
        try:   # the new AppImage can only be put beside the running one: nothing downloaded for nothing
            updates.check_appimage_folder(os.environ["APPIMAGE"])
        except updates.trust.ReleaseError as e:
            raise _update_error(e)
    try:
        r = updates.check(a.channel, a.installed or VERSION, a.source, _keys(a))
    except updates.trust.ReleaseError as e:
        raise _update_error(e)
    if not r.get("available") or not r.get("assets"):
        raise pipeline.SetupError("No release for this platform to download.", "no_release")
    if a.version and r["version"] != a.version:
        raise pipeline.SetupError(f"The release to download is {r['version']}, not {a.version}.", "no_release")
    asset = updates.pick_asset(r["assets"])
    if not asset:
        raise pipeline.SetupError(f"The release has no {updates.installed_kind()} package for this platform.",
                                  "no_release")
    dest = a.dest or os.path.join(os.path.abspath(a.install), "downloads", r["version"])
    prog = _progress(ev, control, ["download"])
    try:
        path = updates.download(asset, r["urls"][asset["name"]], dest,
                                progress=lambda done, total: prog.progress("download", done / max(1, total),
                                                                           f"{done >> 20} of {total >> 20} MB"),
                                cancelled=lambda: control.cancel.is_set())
    except updates.trust.ReleaseError as e:
        if e.code == "cancelled":
            raise pipeline.Cancelled()
        raise _update_error(e)
    # the release's manifest and signature kept as fetched: apply-update verifies the download again from them
    # before anything is installed (beta.4)
    for name, data in ((updates.MANIFEST, r["manifest_bytes"]), (updates.SIGNATURE, r["signature_bytes"])):
        with open(os.path.join(dest, name + ".part"), "wb") as f:
            f.write(data)
        os.replace(os.path.join(dest, name + ".part"), os.path.join(dest, name))
    out = {"version": r["version"], "path": path, "size": asset["size"], "notes": r.get("notes", "")}
    out["seconds"], _ = prog.finish()
    return out


def cmd_stage_launcher(a, ev, control):
    """a downloaded launcher package (checked by download-update) unpacked into the launcher's folder
    (downloads/launcher-new: inside the folder, which is also the install's root), and -- packaged -- the worker's
    copy the swap runs as (downloads/launcher-helper: the swap replaces worker/, so it can't run from there). The
    launcher then starts `<helper> --worker swap-launcher ...` as it exits."""
    import updates
    _refuse_managed(a.install, a.launcher)
    check_writable(a.launcher)
    try:
        staged = updates.stage_launcher(a.package, a.launcher)
        helper = updates.stage_helper(a.launcher, os.path.dirname(os.path.abspath(sys.executable))) \
            if getattr(sys, "frozen", False) else None
    except updates.trust.ReleaseError as e:
        raise _update_error(e)
    except (OSError, ValueError) as e:
        if isinstance(e, OSError) and disk_full(e):
            raise pipeline.SetupError("The launcher's drive is full.", "disk_space", str(e))
        raise pipeline.SetupError(f"The launcher's package couldn't be unpacked ({e}).", "bad_download")
    return {"staged": staged, "helper": helper, "items": sorted(os.listdir(staged))}


def cmd_swap_launcher(a, ev, control):
    """the helper apply-update and rollback-launcher start (their copy of this worker, in downloads/launcher-helper):
    once the launcher has exited, the staged package (or the kept one: --source rollback) in place of the launcher's
    items -- an AppImage installation's: the AppImage file (--appimage) -- journalled, checked, the previous one put
    back if anything fails (updates.swap_launcher). The install's items are never moved."""
    import updates

    def log(t):
        ev.emit("log", level="info", text=t)
    try:
        if a.appimage:
            out = updates.swap_appimage(a.appimage, a.launcher, a.wait_pid, log, a.health_timeout, a.expect_version,
                                        a.swap_from, not a.no_restart, extracted=a.appimage_extracted,
                                        old_appdir=a.old_appdir)
        else:
            if not a.start:
                raise ValueError("swap-launcher needs --start (or --appimage)")
            out = updates.swap_launcher(a.launcher, a.start, a.wait_pid, log, a.health_timeout, a.expect_version,
                                        a.swap_from, not a.no_restart)
    except ValueError as e:
        raise pipeline.SetupError(str(e), "usage")
    except updates.trust.ReleaseError as e:
        raise _update_error(e)
    finally:
        if not updates.read_journal(a.launcher):   # an unfinished update: its recovery runs from this copy
            _remove_own_copy("swap", a.launcher)
    return {"result": out}


def _helper_command(a, args):
    """the swap's helper: packaged, a copy of this worker in <launcher>/downloads/launcher-helper (the swap replaces
    worker/, and an AppImage's worker is inside its image); from a checkout, this Python and this file"""
    import updates
    if getattr(sys, "frozen", False):
        helper = updates.stage_helper(a.launcher, os.path.dirname(os.path.abspath(sys.executable)))
        return [helper, "--worker"] + args
    return [sys.executable, os.path.abspath(__file__)] + args


def _not_while_playing(install):
    import uninstall
    playing = uninstall.running_from(install)
    if playing:
        raise pipeline.SetupError("The game is running: close it first, then install the update.", "game_running",
                                  ", ".join(playing))


_last_helper = [None]   # the program the last helper was started as (its journal and lock record it)


def _start_swap(a, version, source, ev):
    """the helper started, detached; the caller hands it the update's lock, then this worker and the launcher exit"""
    import updates
    args = ["swap-launcher", "--launcher", os.path.abspath(a.launcher), "--expect-version", version,
            "--swap-from", source]
    args += ["--appimage", os.path.realpath(a.appimage)] if a.appimage else ["--start", updates.launcher_names()[0]]
    if a.appimage and os.environ.get("APPDIR"):   # the old image's mount or extracted copy: the helper waits for it
        args += ["--old-appdir", os.environ["APPDIR"]]
    if a.appimage and updates.appimage_extracted():   # started without FUSE: the new one is started the same way
        args.append("--appimage-extracted")
    if a.wait_pid:
        args += ["--wait-pid", str(a.wait_pid)]
    if a.no_restart:
        args.append("--no-restart")
    if a.install:
        logs = os.path.join(os.path.abspath(a.install), "logs")
        os.makedirs(logs, exist_ok=True)
        args += ["--log", os.path.join(logs, "launcher-update.jsonl")]
    cmd = _helper_command(a, args)
    _last_helper[0] = cmd[0]
    p = updates._start_detached(cmd, os.path.abspath(a.launcher))
    ev.emit("log", level="info", text=f"launcher update: the helper started ({p.pid}); the launcher exits now")
    return p.pid


def cmd_apply_update(a, ev, control):
    """(the player said yes) a downloaded update (download-update's, in <install>/downloads/<version>) verified
    again from the manifest and signature kept with it -- the trusted key, a version newer than this one, this
    system's and this kind's package, its size and SHA-256 -- and staged (updates.prepare_update); the swap's helper
    is then started: it waits for the launcher (--wait-pid) to exit, swaps, checks the new launcher and starts it
    (--no-restart: not). Refused while the game runs or another update is under way. Nothing of the working
    installation is changed here."""
    import updates
    _refuse_managed(a.install, a.launcher)
    check_writable(a.launcher)
    if a.appimage and not os.path.isfile(a.appimage):
        raise pipeline.SetupError("--appimage names no file.", "usage")
    lock = updates.UpdateLock(a.launcher, "apply")
    try:
        lock.acquire()
        j = updates.read_journal(a.launcher)
        if j and j.get("state") != "staged":
            raise pipeline.SetupError("An earlier update of the launcher didn't finish; restart the launcher first.",
                                      "busy")
        _not_while_playing(a.install)
        j = updates.prepare_update(a.install, a.launcher, a.version, _keys(a), a.appimage, a.installed)
        pid = _start_swap(a, a.version, "staged", ev)
        lock.hand_over(pid, _last_helper[0])
        # the journal names the helper from now on: a launcher started meanwhile sees an update under way, not an
        # interrupted one
        updates._write_json(updates.update_dirs(a.launcher)["journal"],
                            dict(j, helper_pid=pid, helper_exe=_last_helper[0], helper_since=time.time()))
    except updates.trust.ReleaseError as e:
        raise _update_error(e)
    finally:
        lock.release()
    return {"version": j["version"], "from": j["from"], "kind": j["kind"], "helper_pid": pid}


def cmd_rollback_launcher(a, ev, control):
    """the launcher put back to the version the last update replaced (kept in downloads/launcher-rollback), by the
    same journalled swap; the newer one is kept there in turn"""
    import updates
    _refuse_managed(a.install, a.launcher)
    check_writable(a.launcher)
    v = updates.rollback_version(a.launcher)
    item = os.path.join(updates.update_dirs(a.launcher)["rollback"],
                        updates.APPIMAGE_KEPT if a.appimage else updates.launcher_names()[0])
    if not v or not os.path.isfile(item):
        raise pipeline.SetupError("No earlier launcher is kept to go back to.", "no_rollback")
    lock = updates.UpdateLock(a.launcher, "rollback")
    try:
        lock.acquire()
        if updates.read_journal(a.launcher):
            raise pipeline.SetupError("An earlier update of the launcher didn't finish; restart the launcher first.",
                                      "busy")
        _not_while_playing(a.install or a.launcher)
        pid = _start_swap(a, v, "rollback", ev)
        lock.hand_over(pid, _last_helper[0])
    except updates.trust.ReleaseError as e:
        raise _update_error(e)
    finally:
        lock.release()
    return {"version": v, "helper_pid": pid}


def cmd_recover_launcher(a, ev, control):
    """an update of the launcher a crash or a power cut interrupted: undone (the previous launcher put back from
    what is on disk), or kept when the swap had finished and the new launcher answers as expected
    (updates.recover_launcher). The launcher starts it at its next start, and exits; --no-restart: no launcher is
    started afterwards."""
    import updates
    here = os.path.dirname(os.path.abspath(sys.executable))
    appdir = os.environ.get("APPDIR") or ""
    if getattr(sys, "frozen", False) and appdir and \
            os.path.normcase(here + os.sep).startswith(os.path.normcase(os.path.abspath(appdir) + os.sep)):
        # started from an AppImage's image (no copy was there): its runtime would stay alive for as long as this
        # runs, and the recovery waits for the AppImage to end -- so it runs from a copy out of the image instead
        helper = updates.stage_helper(a.launcher, here)
        args = ["recover-launcher", "--launcher", os.path.abspath(a.launcher), "--old-appdir", appdir]
        for flag, v in (("--appimage", a.appimage), ("--start", a.start),
                        ("--wait-pid", str(a.wait_pid) if a.wait_pid else None)):
            if v:
                args += [flag, v]
        if a.no_restart:
            args.append("--no-restart")
        p = updates._start_detached([helper, "--worker"] + args, os.path.abspath(a.launcher))
        return {"result": "relaunched", "helper_pid": p.pid}
    try:
        out = updates.recover_launcher(a.launcher, a.start, lambda t: ev.emit("log", level="info", text=t),
                                       a.wait_pid, restart=not a.no_restart,
                                       prefixes=(a.old_appdir,) if a.old_appdir else ())
    except updates.trust.ReleaseError as e:
        raise _update_error(e)
    finally:
        if not updates.read_journal(a.launcher):   # still unfinished: the next recovery runs from this copy
            _remove_own_copy("swap", a.launcher)
    return {"result": out}


def cmd_launcher_state(a, ev, control):
    """for the launcher's Updates page: this version, a downloaded newer one ready to install, the launcher kept for
    rollback, the last update's result, an unfinished update"""
    import updates
    return updates.launcher_state(a.install, a.launcher, a.appimage, a.installed, _keys(a))


# ---- M9.4's back end: the settings, the diagnostics (docs/M9_LAUNCHER_PLAN.md section 13) ----

def cmd_settings(a, ev, control):
    """the settings as the game and launcher will use them, with any problems; --set KEY=VALUE (each checked; none
    written if one is not valid), --reset KEY (or --reset all), --repair (a damaged file written again, the damaged
    one kept)"""
    import settings
    install = os.path.abspath(a.install)
    try:
        if a.repair:
            return settings.repair(install)
        if a.reset:
            return settings.reset(install, None if a.reset == ["all"] else a.reset)
        if a.set:
            changes = {}
            for kv in a.set:
                if "=" not in kv:
                    raise pipeline.SetupError(f"--set {kv}: not KEY=VALUE.", "usage")
                k, v = kv.split("=", 1)
                changes[k.strip()] = v.strip()
            return settings.set_values(install, changes)
        return settings.effective(install)
    except settings.SettingsError as e:
        raise pipeline.SetupError(str(e), e.code)


def cmd_uninstall(a, ev, control):
    """--dry-run: what each part would free (nothing removed). Else, with --confirm: the parts removed (--parts, comma-
    separated; default: all but the saves, their backups and the settings) -- docs/M9_LAUNCHER_PLAN.md section 20"""
    import uninstall
    parts = tuple(p for p in (a.parts or ",".join(uninstall.DEFAULT_PARTS)).split(",") if p)
    try:
        if a.dry_run:
            return {"parts": uninstall.plan(os.path.abspath(a.install), uninstall.PARTS),
                    "default": list(uninstall.DEFAULT_PARTS)}
        if not a.confirm:
            raise pipeline.SetupError("Uninstalling needs the player's confirmation.", "needs_confirmation")
        return uninstall.remove(os.path.abspath(a.install), parts, exe=a.exe)
    except uninstall.UninstallError as e:
        raise pipeline.SetupError(str(e), e.code)


def cmd_remove_launcher(a, ev, control):
    """the launcher itself removed (docs/M9_LAUNCHER_PLAN.md section 20): --dry-run what goes (nothing removed);
    --confirm --wait-pid PID: this worker -- inside the folder being removed -- puts a copy of itself in the operating
    system's temporary folder (the one exception to the portable root: the helper must run from outside the folder it
    removes) and starts it (--stage2), which waits for the launcher (PID) and this worker to exit, removes the
    launcher's package, its data folder's items (--data-dir) and its shortcuts, then its own temporary folder. The
    game's install (--install; portable by default, the launcher's own folder), its saves and settings are kept --
    unless the player chose everything: --everything (with --dry-run: what everything is), confirmed separately
    (--confirm --confirm-everything), removes the install whose root is the launcher's folder too, every part of it."""
    import uninstall
    folder = os.path.abspath(a.launcher)
    try:
        if a.dry_run:
            return uninstall.launcher_plan(folder, a.data_dir, a.install, everything=a.everything)
        if not a.confirm or (a.everything and not a.confirm_everything):
            raise pipeline.SetupError("Removing the launcher needs the player's confirmation" + (
                " -- and removing the game and its saves with it, a confirmation of its own." if a.everything else "."),
                "needs_confirmation")
        if a.stage2:
            for pid in a.also_wait or ():   # the worker that started this one (its files are in the folder too)
                uninstall._wait_exit(pid, 30)
            try:
                res = uninstall.remove_launcher(folder, a.data_dir, a.install, wait_pid=a.wait_pid,
                                                everything=a.everything)
            finally:
                _remove_own_copy("remove")
            return res
        uninstall.launcher_plan(folder, a.data_dir, a.install, everything=a.everything)   # refused now, while the
        if not getattr(sys, "frozen", False):                                             # launcher can say why
            raise pipeline.SetupError("Only a downloaded launcher removes itself.", "not_packaged")
        here = os.path.dirname(sys.executable)
        tmp = tempfile.mkdtemp(prefix="CubicHeroes-remove-", dir=system_temp())
        shutil.copy2(sys.executable, tmp)
        for d in ("_internal", "tools", "config"):
            if os.path.isdir(os.path.join(here, d)):
                shutil.copytree(os.path.join(here, d), os.path.join(tmp, d), symlinks=True)
        cmd = [os.path.join(tmp, os.path.basename(sys.executable)), "--worker", "remove-launcher", "--launcher",
               folder, "--confirm", "--stage2", "--wait-pid", str(a.wait_pid or 0), "--also-wait", str(os.getpid()),
               "--log", os.path.join(tmp, "remove.jsonl")]
        if a.data_dir:
            cmd += ["--data-dir", os.path.abspath(a.data_dir)]
        if a.install:
            cmd += ["--install", os.path.abspath(a.install)]
        if a.everything:
            cmd += ["--everything", "--confirm-everything"]
        # detached, no window, out of this worker's job (which ends its processes with it)
        kw = ({"creationflags": 0x00000008 | 0x00000200 | 0x08000000 | 0x01000000} if os.name == "nt"
              else {"start_new_session": True})
        subprocess.Popen(cmd, cwd=tmp, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL, **kw)
        return {"started": True, "helper": tmp, "everything": bool(a.everything)}
    except uninstall.UninstallError as e:
        raise pipeline.SetupError(str(e), e.code)


def _remove_own_copy(kind, launcher=None):
    """the copy this worker runs from, removed a moment after it exits -- never anything else: remove-launcher's (a
    CubicHeroes-remove-* folder in the operating system's temporary folder) or the launcher update's (the
    downloads/launcher-helper folder of the launcher it updated)"""
    here = os.path.dirname(os.path.abspath(sys.executable))
    if not getattr(sys, "frozen", False):
        return
    if kind == "remove":
        parent = system_temp()
        if not (os.path.basename(here).startswith("CubicHeroes-remove-")
                and os.path.normcase(os.path.dirname(here)) == os.path.normcase(parent)):
            return
    elif kind == "swap":
        if not launcher or os.path.normcase(here) != os.path.normcase(updates.update_dirs(launcher)["helper"]):
            return
        parent = os.path.dirname(here)
    else:
        return
    if os.name == "nt":   # the folder through the environment: a path with any character (a % above all) is safe
        kw = dict(cwd=parent, env=dict(os.environ, DOTHEROES_REMOVE_DIR=here), stdin=subprocess.DEVNULL,
                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        cmd = 'cmd /d /c ping -n 3 127.0.0.1 >nul & rmdir /s /q "%DOTHEROES_REMOVE_DIR%"'
        try:
            subprocess.Popen(cmd, creationflags=0x00000008 | 0x00000200 | 0x08000000 | 0x01000000, **kw)
        except OSError:   # a job that refuses the breakaway
            subprocess.Popen(cmd, creationflags=0x00000008 | 0x00000200 | 0x08000000, **kw)
    else:
        subprocess.Popen(["sh", "-c", 'sleep 2; rm -rf -- "$0"', here], cwd=parent, start_new_session=True)


def cmd_diagnostics(a, ev, control):
    """a report for the player to review: its files, privacy-sanitized, into --out DIR (the launcher shows them; the
    player may remove any); --pack ZIP: exactly what is in --out now, as one zip"""
    import diagnostics
    out = os.path.abspath(a.out)
    if a.pack:
        return diagnostics.pack(out, os.path.abspath(a.pack))
    return {"folder": out, "files": diagnostics.collect(os.path.abspath(a.install), out)}


def cmd_shortcuts(a, ev, control):
    """the launcher in the Start menu (and on the desktop with --desktop), or in Steam's library (--steam: Steam
    must be closed, it rewrites its shortcuts when it exits); the shortcut starts the launcher, the single entry
    point. --steam also downloads the game's library artwork once (shortcuts.STEAM_ART, 0.2.0-beta.5) onto this
    computer: any failure leaves the game added without it (`artwork`: placed, kept, failed)."""
    import shortcuts
    exe = os.path.abspath(a.exe)
    icon = a.icon or ""
    if a.remove_steam:   # the launcher's own Steam entry, left by an uninstall while Steam ran: removed once it's closed
        import uninstall
        if shortcuts.steam_running():
            return {"later": uninstall.steam_entry_left(exe)}
        return {"removed": len(uninstall.remove_shortcuts(exe, steam_running=False, links=False)),
                "later": False}
    if a.steam:
        if not shortcuts.steam_roots():
            raise pipeline.SetupError("Steam wasn't found on this PC.", "no_steam")
        if shortcuts.steam_running():
            raise pipeline.SetupError("Close Steam first: it rewrites its library's shortcuts when it exits.",
                                      "steam_running")
        report = {}
        written = shortcuts.add_to_steam(exe, icon, report=report)
        return {"steam": sum(1 for p in written if os.path.basename(p) == "shortcuts.vdf"), "artwork": report}
    return {"made": len(shortcuts.create_shortcuts(exe, icon, desktop=a.desktop))}


def cmd_steam_art(a, ev, control):
    """the player's own image (--image: a PNG or JPEG they downloaded themselves) as the game's Steam library artwork
    (--kind poster | hero) in every Steam account whose library has the launcher (shortcuts.import_steam_art); their own
    artwork already there is kept unless --replace (they confirmed). Nothing is downloaded. Steam may be running: it
    shows new artwork when it next starts."""
    import shortcuts
    if not (a.exe and a.kind and a.image):
        raise pipeline.SetupError("steam-art needs --exe, --kind and --image.", "usage")
    try:
        return shortcuts.import_steam_art(os.path.abspath(a.exe), a.kind, os.path.abspath(a.image), replace=a.replace)
    except shortcuts.ArtError as e:
        raise pipeline.SetupError(str(e), e.code) from None


def cmd_prepare(a, ev, control):
    """the shaders and pipelines made before play (--check: only whether the cache is valid for this GPU and driver,
    and how many recipes there are)"""
    prog = _progress(ev, control, [] if a.check else ["prepare"])
    out = op_prepare(os.path.abspath(a.install), prog, check=a.check, rebuild=a.rebuild)
    out["seconds"], _ = prog.finish()
    return out


def cmd_repair_link(a, ev, control):
    lay = _layout(a)
    lay.point_game(lay.read_active()["active"])
    return {"game": lay.game_link}


def cmd_saves(a, ev, control):
    """--action list (default) | backup [--note] | restore --backup ID --confirm | delete --backup ID --confirm"""
    lay = _layout(a, kind=None)
    act = a.action or "list"
    if act in ("restore", "delete") and not a.confirm:
        raise pipeline.SetupError(f"{act} needs --confirm (it replaces or removes saves).", "needs_confirmation")
    if act in ("restore", "delete") and not a.backup:
        raise pipeline.SetupError(f"{act} needs --backup ID.", "usage")
    if act == "backup":
        return lay.backup_saves("manual", a.note)
    if act == "restore":
        return lay.restore_saves(a.backup)
    if act == "delete":
        return lay.delete_backup(a.backup)
    return {"backups": lay.backups(), "saves_bytes": layout.tree_size(lay.gamedata)}


def cmd_devsaves(a, ev, control):
    """developer mode's test saves (tools/dev_saves.py; docs/M11_PLAN.md section 3.2): --action states (default) |
    copy-player | keep --name N [--confirm: replace] | restore --name N | delete --name N"""
    import dev_saves
    lay = _layout(a, kind=None)
    act = a.action or "states"
    try:
        if act == "copy-player":
            return dev_saves.copy_player(lay)
        if act == "keep":
            return dev_saves.keep(lay, a.name, replace=a.confirm)
        if act == "restore":
            return dev_saves.restore(lay, a.name)
        if act == "delete":
            return dev_saves.delete(lay, a.name)
        if act == "states":
            return dev_saves.states(lay)
    except dev_saves.DevSavesError as e:
        raise pipeline.SetupError(str(e), e.code)
    raise pipeline.SetupError(f"devsaves: no action {act}", "usage")


def cmd_selftest(a, ev, control):
    """A pretend operation of three steps over --seconds, with progress, for the protocol's tests and for
    developing the launcher's progress screens without converting a disc. --block: the second step stops
    calling back (as a long compile does), so cancelling it takes the forced path. --child: first a tool run as
    the conversion runs its tools (pipeline.run_tool) and a compiler-style process (zig_build._run), each to its
    end -- a child must start while the launcher holds the worker's stdin open (M9.1: it once could not)."""
    steps = ["verify", "build", "finish"]
    prog = Progress(ev, control, steps)
    prog.plan()
    if a.child:
        pipeline.run_tool(os.path.join(ROOT, "tools", "version.py"), [], prog.log)
        # what a compiler started as the build starts one sees: its temporary folder and Zig's caches (the portable
        # root's, given --install), and a path in any characters printed back intact
        r = pipeline.zig_build._run([sys.executable, "-c", "import os, sys; sys.stdout.reconfigure(encoding='utf-8'); "
                                     "print('child ran'); print('child temp', os.environ.get('TEMP') or "
                                     "os.environ.get('TMPDIR') or ''); print('child zig', "
                                     "os.environ.get('ZIG_GLOBAL_CACHE_DIR', ''), '|', "
                                     "os.environ.get('ZIG_LOCAL_CACHE_DIR', ''))"],
                                    capture_output=True, text=True, timeout=60)
        for ln in r.stdout.splitlines():
            prog.log(ln.strip())
    n = max(1, int(a.seconds * 20))
    for s in steps:
        for i in range(n):
            if a.block and s == "build":
                time.sleep(a.seconds * 10)
            prog.progress(s, (i + 1) / n, pipeline.STEP_NAMES[s])
            time.sleep(a.seconds / n / len(steps))
    took, skipped = prog.finish()
    return {"seconds": took, "skipped": skipped}


CHAR_SAVEDATA = ("dev_hdd0", "home", "00000001", "savedata")


def cmd_characters(a, ev, control):
    """M12, the local character library (tools/char_library.py, docs/CHARACTERS.md), in <install>/data/characters:
    --char-action list (default) | import --paths P [--paths P ...] [--with-key] [--keep-key] | import-saves [--id FOLDER
    ...] [--with-key] [--keep-key] | export --id I [--id ...] --out PATH | install --id I | uninstall --id I --confirm |
    rename --id I --label L | delete --id I --confirm | previews [--id I ...] [--full] | key-forget.
    import --with-key (with --control): the player's PS3 key is read from the control channel ("key <hex>") -- never
    from the command line; it is validated on the first encrypted character before any is decrypted, kept only with
    --keep-key and only once it decrypted a character, and never written to the events or the log. Long actions
    report progress and stop between characters on "cancel": each character is added whole or not at all."""
    import char_library
    import ps3_save
    import secret_store
    lay = _layout(a, kind=None)
    root = os.path.join(lay.data, "characters")

    def log(level, text):
        ev.emit("log", level=level, text=text)

    try:
        lib = char_library.Library(root, log)
    except OSError as e:
        raise pipeline.SetupError(f"The character library's folder can't be used ({e.strerror or e}).", "io_error",
                                  details=root)
    savedata = os.path.join(lay.gamedata, *CHAR_SAVEDATA)
    act = a.action_chars or "list"
    ids = a.char_id or []

    def progress(i, n, label):
        ev.emit("progress", stage="characters", fraction=(i / n) if n else 1.0, overall=(i / n) if n else 1.0,
                detail=f"{i} of {n}" + (f": {label}" if label else ""))

    def key_status():
        ok, where = secret_store.available()
        return {"stored": secret_store.load() is not None, "store": where if ok else None,
                "store_message": None if ok else where}

    def read_key():
        """(key bytes or None, typed): a key typed now (over the control channel), else the kept one"""
        if a.with_key:
            if not a.control:
                raise pipeline.SetupError("--with-key needs --control (the key comes over the control channel).", "usage")
            ev.emit("needs", what="key")
            control.key_ready.wait(600)
            text, control.key_text = control.key_text, None
            if text:
                try:
                    return ps3_save.parse_key_text(text), True
                except ps3_save.SaveError as e:
                    raise pipeline.SetupError(str(e), "bad_key")
        return secret_store.load(), False

    def do_import(cands, errors):
        key, typed = None, False
        if any(ps3_save.classify(c.edt, c.pfd)[0] == "encrypted" for c in cands):
            key, typed = read_key()
        elif a.with_key:
            read_key()   # asked for nothing: the launcher's key is read and dropped
        program = None
        elf = os.path.join(lay.data, "disc", "PS3_GAME", "USRDIR", "EBOOT.elf")
        if key is not None and os.path.isfile(elf):
            with open(elf, "rb") as f:
                program = f.read()
        results = lib.import_candidates(cands, key, program, progress, control.cancel.is_set)
        kept = None
        decrypted = any(r.get("id") and r["status"] in ("imported", "similar", "duplicate") and
                        (lib.meta(r["id"]).get("source") or {}).get("decrypted_from_ps3_save") for r in results)
        if typed and decrypted:
            if a.keep_key:
                try:
                    kept = secret_store.save(key, keep=True)
                except secret_store.StoreUnavailable as e:
                    secret_store.save(key, keep=False)
                    kept = f"this session only ({e})"
            else:
                secret_store.save(key, keep=False)
                kept = "this session"
        counts = {}
        for r in results:
            counts[r["status"]] = counts.get(r["status"], 0) + 1
        counts["errors"] = len(errors)
        log("info", "characters: import " + ", ".join(f"{k} {v}" for k, v in sorted(counts.items()) if v))
        return {"results": results, "errors": errors, "counts": counts, "key_kept": kept,
                "key_wrong": any(r["status"] == "wrong-key" for r in results)}

    if act == "list":
        idx = lib.index()
        have, _unusable = lib.installed(savedata)
        out, classes, sources, stale = [], {}, {}, 0
        for cid, e in sorted(idx.items()):
            e = dict(e)
            folder = have.get(e["sha256"])
            e["installed"] = folder
            e["installed_by_library"] = bool(folder) and folder == e.get("installed_folder")
            e["in_library"] = True
            e["thumb"], e["views"] = lib.preview_path(cid, "thumb.rle"), lib.preview_path(cid, "views.rle")
            for k in ("shape", "original_sha256"):
                e.pop(k, None)
            classes[e["class"]] = classes.get(e["class"], 0) + 1
            sources[e["source"]] = sources.get(e["source"], 0) + 1
            stale += 0 if e["preview_current"] else 1
            out.append(e)
        in_game = lib.in_game(savedata)
        for g in in_game:
            g["installed"] = g["folder"]
            for k in ("shape", "original_sha256"):
                g.pop(k, None)
        return {"root": root, "characters": out, "total": len(out), "classes": classes, "sources": sources,
                "stale_previews": stale, "in_saves": lib.scan_saves(savedata), "in_game": in_game, "key": key_status()}
    if act == "import":
        if not a.char_paths:
            raise pipeline.SetupError("import needs --paths.", "usage")
        cands, errors = char_library.find(a.char_paths)
        return do_import(cands, errors)
    if act == "import-saves":
        found = lib.scan_saves(savedata)
        folders = [f["folder"] for f in found if not ids or f["folder"] in ids]
        cands, errors = char_library.find([os.path.join(savedata, f) for f in folders]) if folders else ([], [])
        return do_import(cands, errors)
    if act == "previews":
        todo = ids or [cid for cid, e in lib.index().items() if not e["preview_current"]]
        made = failed = 0
        for i, cid in enumerate(todo):
            if control.cancel.is_set():
                break
            progress(i, len(todo), "")
            try:
                ok, new = lib.ensure_previews(cid, force=bool(ids) and a.full)
                made += 1 if new else 0
                failed += 0 if ok else 1
            except (OSError, ValueError) as e:
                failed += 1
                log("warning", f"characters: entry {cid}: previews not made ({type(e).__name__})")
        return {"made": made, "failed": failed, "total": len(todo)}
    if act == "export":
        if not ids or not a.out:
            raise pipeline.SetupError("export needs --id and --out.", "usage")
        written, skipped = [], []
        for cid in ids:
            b = lib.export(cid)
            if len(ids) == 1 and a.out.lower().endswith(".chchar"):
                path = a.out
            else:
                label = "".join(ch if ch.isalnum() or ch in " -_" else "_" for ch in lib.meta(cid)["label"]).strip()
                path = os.path.join(a.out, f"{label or 'character'}-{cid[:8]}.chchar")
            if os.path.exists(path):
                skipped.append({"id": cid, "path": path, "reason": "a file of that name already exists"})
                continue
            os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
            tmp = path + ".part"
            with open(tmp, "wb") as f:
                f.write(b)
            os.replace(tmp, path)
            written.append(path)
        return {"written": written, "skipped": skipped, "folder": os.path.dirname(os.path.abspath(written[0]))
                if written else os.path.abspath(a.out)}
    if act == "install":
        done = []
        for cid in ids:
            folder, new = lib.install(cid, savedata)
            done.append({"id": cid, "folder": folder, "new": new})
        return {"installed": done}
    if act == "uninstall":
        if not a.confirm:
            raise pipeline.SetupError("uninstall needs --confirm.", "needs_confirmation")
        try:
            return {"uninstalled": [{"id": cid, "folder": lib.uninstall(cid, savedata)} for cid in ids]}
        except char_library.LibraryError as e:
            raise pipeline.SetupError(str(e), "not_uninstalled")
    if act == "rename":
        if len(ids) != 1 or not a.label:
            raise pipeline.SetupError("rename needs one --id and --label.", "usage")
        lib.rename(ids[0], a.label)
        return {"id": ids[0], "label": lib.meta(ids[0])["label"]}
    if act == "delete":
        if not a.confirm:
            raise pipeline.SetupError("delete needs --confirm.", "needs_confirmation")
        for cid in ids:
            lib.delete(cid)
        return {"deleted": ids}
    if act == "key-forget":
        secret_store.forget()
        return {"key": key_status()}
    raise pipeline.SetupError(f"unknown action {act}", "usage")


COMMANDS = {"characters": cmd_characters, "version": cmd_version, "probe-disc": cmd_probe_disc, "probe-keys": cmd_probe_keys,
            "space": cmd_space, "info": cmd_info, "install": cmd_install, "update": cmd_update,
            "migrate": cmd_migrate, "builds": cmd_builds, "rollback": cmd_rollback, "game-appimage": cmd_game_appimage, "retention": cmd_retention,
            "repair-link": cmd_repair_link, "saves": cmd_saves, "prepare": cmd_prepare,
            "check-update": cmd_check_update, "download-update": cmd_download_update,
            "stage-launcher": cmd_stage_launcher, "swap-launcher": cmd_swap_launcher, "settings": cmd_settings,
            "apply-update": cmd_apply_update, "rollback-launcher": cmd_rollback_launcher,
            "recover-launcher": cmd_recover_launcher, "launcher-state": cmd_launcher_state,
            "diagnostics": cmd_diagnostics, "shortcuts": cmd_shortcuts, "steam-art": cmd_steam_art,
            "selftest": cmd_selftest,
            "uninstall": cmd_uninstall, "remove-launcher": cmd_remove_launcher, "devsaves": cmd_devsaves,
            "migrate-root": cmd_migrate_root, "find-legacy": cmd_find_legacy}


def parser():
    p = argparse.ArgumentParser(prog="worker", description="the launcher's pipeline worker")
    p.add_argument("command", choices=sorted(COMMANDS))
    p.add_argument("--disc")
    p.add_argument("--keys")
    p.add_argument("--install")
    p.add_argument("--for", dest="op", choices=sorted(NEEDED), default="install")
    p.add_argument("--full", action="store_true")
    p.add_argument("--resume", action="store_true", help="install: keep the disc's steps a previous run finished")
    p.add_argument("--to", help="rollback: the kept build to go back to (default: the newest)")
    p.add_argument("--keep", type=int, help="retention: how many previous builds to keep")
    p.add_argument("--action", choices=("list", "backup", "restore", "delete", "states", "copy-player", "keep"),
                   help="saves; devsaves")
    p.add_argument("--name", help="devsaves: a test state's name")
    p.add_argument("--backup", help="saves: a backup's id")
    p.add_argument("--note", help="saves backup: a note kept with it")
    p.add_argument("--confirm", action="store_true", help="saves restore/delete: the player confirmed")
    p.add_argument("--check", action="store_true", help="prepare: only whether a preparation is due")
    p.add_argument("--exe", help="shortcuts: the program the shortcut starts (the launcher)")
    p.add_argument("--icon", help="shortcuts: its icon")
    p.add_argument("--steam", action="store_true", help="shortcuts: in Steam's library")
    p.add_argument("--desktop", action="store_true", help="shortcuts: on the desktop too")
    p.add_argument("--remove-steam", action="store_true",
                   help="shortcuts: the launcher's own Steam entry removed (later: true while Steam runs)")
    p.add_argument("--rebuild", action="store_true", help="prepare: the pipeline cache made again from scratch")
    p.add_argument("--kind", choices=("poster", "hero"), help="steam-art: which artwork")
    p.add_argument("--image", help="steam-art: the player's own image file (PNG or JPEG)")
    p.add_argument("--replace", action="store_true", help="steam-art: replace the player's own artwork (they confirmed)")
    p.add_argument("--char-action", dest="action_chars",
                   choices=("list", "import", "import-saves", "export", "install", "uninstall", "rename", "delete",
                            "previews", "key-forget"),
                   help="characters: what to do (default list)")
    p.add_argument("--paths", dest="char_paths", action="append", help="characters import: a file or folder (repeat)")
    p.add_argument("--id", dest="char_id", action="append", help="characters: a library entry (repeat)")
    p.add_argument("--label", help="characters: rename's new label; list's search text")
    p.add_argument("--with-key", action="store_true", help="characters import: the PS3 save key comes over --control")
    p.add_argument("--keep-key", action="store_true", help="characters import: keep the validated key (OS-protected)")
    p.add_argument("--disc-key", help="an encrypted disc image's key: a .dkey or IRD file, or a file of the 32 digits")
    p.add_argument("--parts", help="uninstall: the parts, comma-separated (builds,data,shaders,objects,settings,saves,shortcuts)")
    p.add_argument("--dry-run", action="store_true",
                   help="uninstall, remove-launcher, migrate-root: what would be done; nothing removed or written")
    p.add_argument("--from", dest="from_root", help="migrate-root: the existing installation's root (only read)")
    p.add_argument("--everything", action="store_true",
                   help="remove-launcher: the install in the launcher's folder too, saves and settings included")
    p.add_argument("--confirm-everything", dest="confirm_everything", action="store_true",
                   help="remove-launcher --everything: the player confirmed removing the game and its saves too")
    p.add_argument("--no-prepare", action="store_true",
                   help="install/update/migrate: no preparation at the end (the launcher prepares in the background)")
    p.add_argument("--set", action="append", help="settings: KEY=VALUE (repeatable)")
    p.add_argument("--reset", action="append", help="settings: a key to its default (repeatable; all: every one)")
    p.add_argument("--repair", action="store_true", help="settings: write a damaged settings file again")
    p.add_argument("--out", help="diagnostics: the folder the report's files are written into; characters export: a .chchar file, or a folder for several")
    p.add_argument("--pack", help="diagnostics: the zip to make from --out as it is now")
    p.add_argument("--channel", choices=("stable", "prerelease"), default="stable", help="updates: the channel")
    # a release list other than GitHub's (a mirror, for tests): --source, or CUBIC_UPDATE_SOURCE for the commands the
    # launcher runs itself. It changes only where releases are looked for: whatever it lists must still be signed by
    # a key in config/release_keys.json, so it can't get anything installed that GitHub's list couldn't
    p.add_argument("--source", default=os.environ.get("CUBIC_UPDATE_SOURCE") or None,
                   help="updates: a release list other than GitHub's (a mirror, for tests; or CUBIC_UPDATE_SOURCE)")
    p.add_argument("--trusted-keys", dest="keys_file",
                   help="updates: a trusted-keys file other than config/release_keys.json")
    p.add_argument("--installed", help="updates: the installed version (default: this worker's)")
    p.add_argument("--version", dest="version", help="download-update, apply-update: the version expected")
    p.add_argument("--dest", help="download-update: the folder (default <install>/downloads/<version>)")
    p.add_argument("--package", help="stage-launcher: the downloaded launcher package")
    p.add_argument("--launcher", help="the launcher update's commands: the launcher's folder (an AppImage's: the "
                                      "installation's root)")
    p.add_argument("--start", help="swap-launcher, recover-launcher: the launcher's executable in its folder")
    p.add_argument("--wait-pid", dest="wait_pid", type=int,
                   help="the launcher update's commands: the launcher's process, to wait for")
    p.add_argument("--appimage", help="the launcher update's commands: an AppImage installation's AppImage file")
    p.add_argument("--expect-version", dest="expect_version", help="swap-launcher: the version being put in place")
    p.add_argument("--swap-from", dest="swap_from", choices=("staged", "rollback"), default="staged",
                   help="swap-launcher: the staged update, or the launcher kept by the last one")
    p.add_argument("--appimage-extracted", dest="appimage_extracted", action="store_true", help=argparse.SUPPRESS)
    p.add_argument("--old-appdir", dest="old_appdir", help=argparse.SUPPRESS)
    p.add_argument("--no-restart", dest="no_restart", action="store_true",
                   help="the launcher update's commands: no launcher started afterwards (the command line's)")
    p.add_argument("--health-timeout", dest="health_timeout", type=float, default=60.0, help=argparse.SUPPRESS)
    p.add_argument("--seconds", type=float, default=3.0, help=argparse.SUPPRESS)   # selftest
    p.add_argument("--block", action="store_true", help=argparse.SUPPRESS)        # selftest
    p.add_argument("--child", action="store_true", help=argparse.SUPPRESS)        # selftest
    p.add_argument("--data-dir", dest="data_dir", help="remove-launcher: the launcher's own data folder")
    p.add_argument("--stage2", action="store_true", help=argparse.SUPPRESS)        # remove-launcher's helper
    p.add_argument("--also-wait", dest="also_wait", type=int, action="append", help=argparse.SUPPRESS)
    p.add_argument("--control", action="store_true")
    p.add_argument("--log")
    return p


REQUIRED = {"probe-disc": ("disc",), "probe-keys": ("disc", "keys"), "space": ("install",), "info": ("install",),
            "install": ("disc", "keys", "install"), "update": ("install",), "migrate": ("install",),
            "builds": ("install",), "rollback": ("install",), "game-appimage": ("install",), "retention": ("install", "keep"),
            "repair-link": ("install",), "saves": ("install",), "prepare": ("install",), "uninstall": ("install",),
            "download-update": ("install",), "stage-launcher": ("package", "launcher"),
            "swap-launcher": ("launcher",), "settings": ("install",), "diagnostics": ("install", "out"),
            "apply-update": ("install", "launcher", "version"), "rollback-launcher": ("launcher",),
            "recover-launcher": ("launcher",), "launcher-state": ("install", "launcher"),
            "shortcuts": ("exe",), "remove-launcher": ("launcher",),
            "devsaves": ("install",), "migrate-root": ("from_root", "install")}
OPTION_NAMES = {"from_root": "from", "keys_file": "trusted-keys", "data_dir": "data-dir", "wait_pid": "wait-pid",
                "expect_version": "expect-version", "no_restart": "no-restart"}


def main(argv=None):
    a = parser().parse_args(argv)
    missing = [f"--{OPTION_NAMES.get(x, x)}" for x in REQUIRED.get(a.command, ()) if getattr(a, x) in (None, "")]
    for name in ("stdout", "stderr"):   # a windowed worker started without standard handles (beta.4): never an
        if getattr(sys, name) is None:  # error dialog for having nowhere to write -- the null device instead
            setattr(sys, name, open(os.devnull, "w", encoding="utf-8"))
    out = sys.stdout
    try:   # UTF-8 and bare newlines whatever the console's code page (a path may hold any character)
        out.reconfigure(encoding="utf-8", errors="replace", newline="\n")
    except (AttributeError, ValueError):
        pass
    sys.stdout = sys.stderr   # anything else that prints goes to stderr: stdout carries events only
    ev = Events(out, a.log)
    if missing:
        ev.emit("result", ok=False, code="usage", message=f"{a.command} needs {', '.join(missing)}")
        return EXIT_USAGE
    control = Control(ev)
    owned = own_children()
    if a.control:
        control.listen()
    ev.emit("hello", protocol=PROTOCOL, command=a.command, owned_children=owned)
    code = EXIT_OK
    if a.install and a.command in TEMP_COMMANDS:   # the portable root: temporary files and Zig's caches inside it
        if use_install_temp(a.install, create_root=a.command == "install"):
            pipeline.zig_build.use_install(a.install)
    try:
        data = COMMANDS[a.command](a, ev, control)
        control.check()
        result = {"ok": True, "data": data}
    except (pipeline.Cancelled, KeyboardInterrupt):
        result, code = {"ok": False, "code": "cancelled", "message": "Cancelled."}, EXIT_CANCELLED
    except pipeline.SetupError as e:
        result, code = {"ok": False, "code": e.code, "message": str(e), "details": e.details}, EXIT_FAILED
    except layout.LayoutError as e:
        result, code = {"ok": False, "code": e.code, "message": str(e), "details": None}, EXIT_FAILED
    except pipeline.zig_build.BuildError as e:
        result, code = {"ok": False, "code": "compile_failed", "message": "Building the game failed.",
                        "details": str(e)[-4000:]}, EXIT_FAILED
    except OSError as e:
        if disk_full(e):
            result = {"ok": False, "code": "disk_space", "message": "The install's drive is full.",
                      "details": str(e)}
        elif e.errno == errno.EROFS:
            result = {"ok": False, "code": "root_not_writable", "message": "The install's drive is read-only.",
                      "details": str(e)}
        else:
            result = {"ok": False, "code": "io_error", "message": f"A file couldn't be read or written "
                      f"({e.strerror or e}).", "details": traceback.format_exc()[-4000:]}
        code = EXIT_FAILED
    except Exception:  # noqa: BLE001 -- anything else: still a result, with the trace as its details
        result, code = {"ok": False, "code": "internal", "message": "Something went wrong in the worker.",
                        "details": traceback.format_exc()[-4000:]}, EXIT_FAILED
    control.finished.set()
    release_install_temp()
    pipeline.zig_build.use_install(None)
    ev.emit("result", **result)
    ev.close()
    return code


if __name__ == "__main__":
    sys.exit(main())

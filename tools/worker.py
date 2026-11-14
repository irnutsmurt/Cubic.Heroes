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

Options for every command: --control (the launcher: "cancel" lines on stdin, and the end of stdin, cancel the
operation), --log FILE (every event appended as it is sent, for the launcher's logs).

In a source checkout: `python tools/worker.py ...`; packaged, the setup's frozen executable runs it as
`<app> --worker ...` (setup_app.py main). Keys: read only by the decryption step and the key probe, never
written, logged or put into an event.
"""
import argparse
import json
import os
import shutil
import sys
import threading
import time
import traceback

ROOT = (os.path.dirname(sys.executable) if getattr(sys, "frozen", False)
        else os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import game_settings  # noqa: E402
import setup_pipeline as pipeline  # noqa: E402

PROTOCOL = 1
# Exit codes: the result event says the same
EXIT_OK, EXIT_FAILED, EXIT_CANCELLED, EXIT_USAGE = 0, 1, 2, 64
# A cancelled operation unwinds at its next progress or log callback; one blocked longer than this (a compile
# in flight) is ended by the worker's exit, which ends its children with it (the job object below)
CANCEL_GRACE_S = 5.0
# Each step's typical length on the reference PC (seconds; M9.0's measurement, docs/M9_WORKER_PROTOCOL.md
# section 6): the weights of the overall progress, and the remaining time before a step's own rate is known
STAGE_SECONDS = {"verify": 15.0, "extract": 25.0, "decrypt": 1.0, "recomp": 40.0, "spu": 12.0, "build": 275.0,
                 "finish": 1.0}
CPU_STAGES = {"recomp", "spu", "build"}   # bound by the processor (the others by the drive)
# What an operation needs on the install's drive (bytes; measured, section 6)
NEEDED = {"install": 6 << 30, "update": 3 << 29}
DISK_FULL_ERRNO = {28}          # ENOSPC
DISK_FULL_WINERROR = {39, 112}  # ERROR_HANDLE_DISK_FULL, ERROR_DISK_FULL


class Events:
    """The event stream: one JSON object per line, flushed, from any thread."""

    def __init__(self, stream, log_path=None):
        self.out = stream
        self.lock = threading.Lock()
        self.t0 = time.monotonic()
        self.log = None
        if log_path:
            os.makedirs(os.path.dirname(os.path.abspath(log_path)), exist_ok=True)
            self.log = open(log_path, "a", encoding="utf-8", newline="\n")
        self.closed = False

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
            if self.log:
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

    def listen(self):
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in sys.stdin:
            if line.strip() == "cancel":
                return self.request("the player")
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
        lim.Basic.LimitFlags = 0x2000   # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if job and k32.SetInformationJobObject(job, 9, ctypes.byref(lim), ctypes.sizeof(lim)):
            return bool(k32.AssignProcessToJobObject(job, k32.GetCurrentProcess()))
        return False
    try:
        os.setpgid(0, 0)
        return True
    except OSError:
        return False


def hard_exit(code):
    if os.name != "nt":
        import signal
        try:
            if os.getpgid(0) == os.getpid():
                signal.signal(signal.SIGTERM, signal.SIG_IGN)
                os.killpg(0, signal.SIGTERM)
        except OSError:
            pass
    os._exit(code)   # Windows: the job's last handle closes with the process; its children end


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
        self.ev.emit("plan", stages=[{"id": s, "name": pipeline.STEP_NAMES.get(s, s), "weight": self.weight[s]}
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
        self.ev.emit("stage", id=step, state="start", name=pipeline.STEP_NAMES.get(step, step))

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


# ---------------- commands ----------------

def cmd_version(a, ev, control):
    from version import VERSION
    return {"worker": VERSION, "protocol": PROTOCOL, "platform": sys.platform,
            "game": pipeline.version_info()["title_id"]}


def cmd_probe_disc(a, ev, control):
    return pipeline.probe_disc(os.path.abspath(a.disc))


def cmd_probe_keys(a, ev, control):
    k = os.path.abspath(a.keys)
    elf = pipeline.is_elf(k)
    return pipeline.probe_keys(os.path.abspath(a.disc), None if elf else k, k if elf else None)


def cmd_space(a, ev, control):
    free, at = free_bytes(a.install)
    need = NEEDED[a.op]
    return {"free": free, "needed": need, "enough": free >= need, "checked_at": at}


def cmd_info(a, ev, control):
    install = os.path.abspath(a.install)
    info = pipeline.install_info(install)
    paths = pipeline.Paths(install)
    return {"exists": info is not None, "version": (info or {}).get("version"),
            "steps_done": sorted((info or {}).get("stamps", {})), "game": os.path.exists(paths.exe),
            "disc_copy": os.path.exists(paths.elf)}


def _convert(a, ev, control, source, keys, elf, full=False):
    install = os.path.abspath(a.install)
    free, _ = free_bytes(install)
    need = NEEDED["install" if source else "update"]
    if free < need:
        raise pipeline.SetupError(f"Not enough free space on the install's drive: about {need / 2**30:.1f} GB "
                                  f"is needed, {free / 2**30:.1f} GB is free.", "disk_space",
                                  json.dumps({"free": free, "needed": need}))
    steps = pipeline.STEPS if source else [s for s in pipeline.STEPS if s not in ("verify", "extract", "decrypt")]
    prog = Progress(ev, control, steps)
    prog.plan()
    exe = pipeline.run(install, source=source, keys=keys, elf=elf, progress=prog.progress, log=prog.log,
                       skip=prog.skip, full=full)
    control.check()
    game_settings.settle_after_conversion(install)
    took, skipped = prog.finish()
    return {"exe": exe, "seconds": took, "skipped": skipped}


def cmd_install(a, ev, control):
    k = os.path.abspath(a.keys)
    elf = pipeline.is_elf(k)
    return _convert(a, ev, control, os.path.abspath(a.disc), None if elf else k, k if elf else None)


def cmd_update(a, ev, control):
    return _convert(a, ev, control, None, None, None, full=a.full)


def cmd_selftest(a, ev, control):
    """A pretend operation of three steps over --seconds, with progress, for the protocol's tests and for
    developing the launcher's progress screens without converting a disc. --block: the second step stops
    calling back (as a long compile does), so cancelling it takes the forced path."""
    steps = ["verify", "build", "finish"]
    prog = Progress(ev, control, steps)
    prog.plan()
    n = max(1, int(a.seconds * 20))
    for s in steps:
        for i in range(n):
            if a.block and s == "build":
                time.sleep(a.seconds * 10)
            prog.progress(s, (i + 1) / n, pipeline.STEP_NAMES[s])
            time.sleep(a.seconds / n / len(steps))
    took, skipped = prog.finish()
    return {"seconds": took, "skipped": skipped}


COMMANDS = {"version": cmd_version, "probe-disc": cmd_probe_disc, "probe-keys": cmd_probe_keys,
            "space": cmd_space, "info": cmd_info, "install": cmd_install, "update": cmd_update,
            "selftest": cmd_selftest}


def parser():
    p = argparse.ArgumentParser(prog="worker", description="the launcher's pipeline worker")
    p.add_argument("command", choices=sorted(COMMANDS))
    p.add_argument("--disc")
    p.add_argument("--keys")
    p.add_argument("--install")
    p.add_argument("--for", dest="op", choices=sorted(NEEDED), default="install")
    p.add_argument("--full", action="store_true")
    p.add_argument("--seconds", type=float, default=3.0, help=argparse.SUPPRESS)   # selftest
    p.add_argument("--block", action="store_true", help=argparse.SUPPRESS)        # selftest
    p.add_argument("--control", action="store_true")
    p.add_argument("--log")
    return p


REQUIRED = {"probe-disc": ("disc",), "probe-keys": ("disc", "keys"), "space": ("install",), "info": ("install",),
            "install": ("disc", "keys", "install"), "update": ("install",)}


def main(argv=None):
    a = parser().parse_args(argv)
    missing = [f"--{x}" for x in REQUIRED.get(a.command, ()) if not getattr(a, x)]
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
    try:
        data = COMMANDS[a.command](a, ev, control)
        control.check()
        result = {"ok": True, "data": data}
    except (pipeline.Cancelled, KeyboardInterrupt):
        result, code = {"ok": False, "code": "cancelled", "message": "Cancelled."}, EXIT_CANCELLED
    except pipeline.SetupError as e:
        result, code = {"ok": False, "code": e.code, "message": str(e), "details": e.details}, EXIT_FAILED
    except pipeline.zig_build.BuildError as e:
        result, code = {"ok": False, "code": "compile_failed", "message": "Building the game failed.",
                        "details": str(e)[-4000:]}, EXIT_FAILED
    except OSError as e:
        if disk_full(e):
            result = {"ok": False, "code": "disk_space", "message": "The install's drive is full.",
                      "details": str(e)}
        else:
            result = {"ok": False, "code": "io_error", "message": f"A file couldn't be read or written "
                      f"({e.strerror or e}).", "details": traceback.format_exc()[-4000:]}
        code = EXIT_FAILED
    except Exception:  # noqa: BLE001 -- anything else: still a result, with the trace as its details
        result, code = {"ok": False, "code": "internal", "message": "Something went wrong in the worker.",
                        "details": traceback.format_exc()[-4000:]}, EXIT_FAILED
    control.finished.set()
    ev.emit("result", **result)
    ev.close()
    return code


if __name__ == "__main__":
    sys.exit(main())

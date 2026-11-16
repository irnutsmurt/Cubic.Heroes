#!/usr/bin/env python3
"""Automated tests stay off the owner's desktop (the owner, 2026-11-16: "Automated tests must remain offscreen.
Enforce this centrally in the test harness ... Ask me before opening anything on my desktop").

One place decides whether a test may show anything, so no single run depends on a remembered switch:

- **Off by default.** A runner asks `onscreen_allowed(what)`. It is true only while an approval written by
  `allow` is in force (build/test/ONSCREEN_APPROVAL.json: until when, and for what). Without one, every request
  for a window (WINDOW=1, VISIBLE=1, TOPMOST=1, GRAB_EVERY, a scenario's `#! env` line, a caller's environment) is
  overridden: the game renders offscreen (PS3RECOMP_VK_WINDOW=0) and the launcher's scripted test stays hidden.
- **Checked, not assumed.** `Watch(pid)` looks every quarter second for a visible top-level window belonging to
  the process a run started. If one appears while no approval is in force, the process is ended at once and the
  run says why (`violation`). A window that should not have opened is not left on screen for the run's length.
- **Tools that can only run on screen** (an RPCS3 capture) call `require_onscreen(what)`, which stops them unless
  an approval is in force.

  py -3 tests/desktop_guard.py status
  py -3 tests/desktop_guard.py allow MINUTES "WHAT"   only after the owner has said yes, in chat, to that run
  py -3 tests/desktop_guard.py revoke
"""
import json
import os
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APPROVAL = os.path.join(ROOT, "build", "test", "ONSCREEN_APPROVAL.json")
WIN = sys.platform == "win32"


def approval():
    """The approval in force, {until, what, granted}, or None."""
    try:
        with open(APPROVAL, encoding="utf-8") as f:
            a = json.load(f)
        return a if float(a.get("until", 0)) > time.time() else None
    except (OSError, ValueError):
        return None


def onscreen_allowed(what=""):
    return approval() is not None


def require_onscreen(what):
    """For a tool that cannot run offscreen: stop unless an approval is in force."""
    if approval() is None:
        print(f"desktop guard: {what} would open a window on the desktop, and no on-screen run is approved "
              f"(tests/desktop_guard.py). Not started.")
        sys.exit(3)


def game_env(env, want_window, quiet=False):
    """The game's environment with the window decided here: shown only when asked for AND approved. Returns
    whether the window is shown."""
    show = bool(want_window) and onscreen_allowed()
    if want_window and not show and not quiet:
        print("desktop guard: a window was asked for (WINDOW/VISIBLE/TOPMOST), but no on-screen run is approved: "
              "the game renders offscreen")
    env["PS3RECOMP_VK_WINDOW"] = "1" if show else "0"
    if not show:
        env.pop("PS3RECOMP_VK_WINDOW_HIDDEN", None)
        for k in ("VISIBLE", "TOPMOST", "GRAB_EVERY"):
            env.pop(k, None)
    return show


def launcher_env(env):
    """The launcher's scripted test: hidden unless approved (CUBIC_NAV_SHOW=1 is honoured only then)."""
    if env.get("CUBIC_NAV_SHOW") == "1" and not onscreen_allowed():
        print("desktop guard: the launcher's window was asked for, but no on-screen run is approved: hidden")
        env.pop("CUBIC_NAV_SHOW", None)
    env["CUBIC_NAV_HIDDEN"] = "0" if env.get("CUBIC_NAV_SHOW") == "1" else "1"
    return env


def visible_windows(pid):
    """The visible top-level windows of a process, as (class, title, w, h)."""
    if not WIN:
        return []
    import ctypes
    import ctypes.wintypes as wt
    u = ctypes.WinDLL("user32")
    out = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(h, _):
        p = wt.DWORD()
        u.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid and u.IsWindowVisible(h):
            r = wt.RECT()
            u.GetWindowRect(h, ctypes.byref(r))
            w, hh = r.right - r.left, r.bottom - r.top
            if w > 0 and hh > 0:
                cls = ctypes.create_unicode_buffer(64)
                u.GetClassNameW(h, cls, 64)
                title = ctypes.create_unicode_buffer(128)
                u.GetWindowTextW(h, title, 128)
                out.append((cls.value, title.value, w, hh))
        return True
    u.EnumWindows(cb, 0)
    return out


class Watch:
    """Ends a process that shows a window while no on-screen run is approved. `violation` says what was seen."""

    def __init__(self, proc, label="the game", every=0.25):
        self.proc, self.label, self.every = proc, label, every
        self.violation = None
        self._stop = threading.Event()
        self._t = threading.Thread(target=self._run, daemon=True)

    def start(self):
        if WIN:
            self._t.start()
        return self

    def stop(self):
        self._stop.set()
        if self._t.is_alive():
            self._t.join(2)

    def _run(self):
        while not self._stop.wait(self.every):
            if self.proc.poll() is not None:
                return
            if onscreen_allowed():
                continue
            seen = visible_windows(self.proc.pid)
            if seen:
                cls, title, w, h = seen[0]
                self.violation = f"{self.label} showed a window ({cls} {w}x{h}) with no on-screen run approved: ended"
                print("desktop guard: " + self.violation, flush=True)
                try:
                    self.proc.kill()
                except OSError:
                    pass
                return


def main(argv):
    cmd = argv[1] if len(argv) > 1 else "status"
    if cmd == "status":
        a = approval()
        print("on-screen runs: " + (f"approved until {time.strftime('%H:%M', time.localtime(a['until']))} for "
                                    f"{a.get('what', '?')}" if a else "not approved (tests stay offscreen)"))
        return 0
    if cmd == "allow" and len(argv) >= 4:
        os.makedirs(os.path.dirname(APPROVAL), exist_ok=True)
        until = time.time() + float(argv[2]) * 60
        with open(APPROVAL, "w", encoding="utf-8") as f:
            json.dump({"until": until, "what": argv[3], "granted": time.strftime("%Y-%m-%d %H:%M:%S")}, f)
        print(f"on-screen runs approved for {argv[2]} minutes: {argv[3]}")
        return 0
    if cmd == "revoke":
        try:
            os.remove(APPROVAL)
        except OSError:
            pass
        print("on-screen runs: not approved")
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))

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
- **Linux**: without an approval the run has no display at all -- `DISPLAY` removed and `WAYLAND_DISPLAY` pointed at
  a socket that does not exist (unset, libwayland would fall back to `wayland-0`) -- so neither X11, XWayland nor
  Wayland can be reached; `Watch` checks it by the process tree's connections to a display server (an X11 or a
  Wayland socket, found through `ss`), which come before any window and cover every compositor alike.
- **Tools that can only run on screen** (an RPCS3 capture) call `require_onscreen(what)`, which stops them unless
  an approval is in force.

  py -3 tests/desktop_guard.py status                  (Linux: python3 tests/desktop_guard.py ...)
  py -3 tests/desktop_guard.py allow MINUTES "WHAT"   only after the owner has said yes, in chat, to that run
  py -3 tests/desktop_guard.py revoke
"""
import json
import os
import re
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APPROVAL = os.path.join(ROOT, "build", "test", "ONSCREEN_APPROVAL.json")
WIN = sys.platform == "win32"
# A display server's listening socket: Wayland's $XDG_RUNTIME_DIR/wayland-N (or any name a compositor chose under
# the runtime folder) and X11's /tmp/.X11-unix/XN (also as an abstract name, "@/tmp/.X11-unix/XN").
DISPLAY_SOCKET = re.compile(r"(?:^|/)wayland-[^/]*$|/\.X11-unix/X\d+$")


def session_display_sockets():
    """the display sockets of the session the guard runs in, whatever they are named (M11.4, the review's L-5: a
    compositor may name its Wayland socket anything, not only wayland-*): $WAYLAND_DISPLAY's, as a path or in
    $XDG_RUNTIME_DIR"""
    out = set()
    w = os.environ.get("WAYLAND_DISPLAY")
    if w and w != NO_WAYLAND:
        out.add(w if os.path.isabs(w) else os.path.join(os.environ.get("XDG_RUNTIME_DIR", ""), w))
    return out
NO_WAYLAND = "desktop-guard-no-display"   # a Wayland socket name that is never created


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
        no_display(env)
    return show


def no_display(env):
    """Linux: no display server reachable from this environment (X11, XWayland or Wayland). No-op on Windows."""
    if WIN:
        return env
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_SOCKET", None)
    env["WAYLAND_DISPLAY"] = NO_WAYLAND
    return env


def launcher_env(env):
    """The launcher's scripted test: hidden unless approved (CUBIC_NAV_SHOW=1 is honoured only then)."""
    if env.get("CUBIC_NAV_SHOW") == "1" and not onscreen_allowed():
        print("desktop guard: the launcher's window was asked for, but no on-screen run is approved: hidden")
        env.pop("CUBIC_NAV_SHOW", None)
    env["CUBIC_NAV_HIDDEN"] = "0" if env.get("CUBIC_NAV_SHOW") == "1" else "1"
    if env["CUBIC_NAV_HIDDEN"] == "1" and not WIN:
        no_display(env)
        env["SDL_VIDEODRIVER"] = "offscreen"   # SDL2's own windowless driver: the launcher still draws its frames
    return env


def process_tree(pid):
    """pid and every descendant (Linux, from /proc)."""
    children = {}
    for d in os.listdir("/proc"):
        if d.isdigit():
            try:
                with open(f"/proc/{d}/stat", encoding="utf-8", errors="replace") as f:
                    ppid = int(f.read().rsplit(")", 1)[1].split()[1])
            except (OSError, IndexError, ValueError):
                continue
            children.setdefault(ppid, []).append(int(d))
    out, todo = [], [pid]
    while todo:
        p = todo.pop()
        out.append(p)
        todo.extend(children.get(p, []))
    return out


def socket_inodes(pids):
    out = set()
    for p in pids:
        try:
            fds = os.listdir(f"/proc/{p}/fd")
        except OSError:
            continue
        for fd in fds:
            try:
                link = os.readlink(f"/proc/{p}/fd/{fd}")
            except OSError:
                continue
            if link.startswith("socket:["):
                out.add(link[8:-1])
    return out


_ss_missing = False


class DisplayUncheckable(Exception):
    """the display connections cannot be checked here (no `ss`): an unapproved run is ended rather than unchecked"""


def display_connections(pid):
    """Linux: the display-server sockets (X11, Wayland) the process or its descendants are connected to, as
    (kind, path). A Unix socket's client end has no name; `ss` gives its peer, whose row names the listening
    socket's path."""
    global _ss_missing
    mine = socket_inodes(process_tree(pid))
    if not mine:
        return []
    try:
        rows = subprocess.run(["ss", "-xan"], capture_output=True, text=True, timeout=5).stdout.splitlines()
    except (OSError, subprocess.SubprocessError):
        # the review's L-5: never silently unchecked -- the Watch ends an unapproved run it cannot check
        _ss_missing = True
        raise DisplayUncheckable("`ss` (iproute2) is not available: display connections can't be checked")
    path_of, peers = {}, set()
    for ln in rows:
        f = ln.split()
        if len(f) < 8 or not f[0].startswith("u_"):
            continue
        path_of[f[5]] = f[4]
        if f[5] in mine:
            peers.add(f[7])
    out = []
    session = session_display_sockets()
    for peer in peers:
        path = path_of.get(peer, "")
        if DISPLAY_SOCKET.search(path) or path.lstrip("@") in session:
            out.append(("x11" if ".X11-unix" in path else "wayland", path))
    return out


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
            if WIN:
                seen = visible_windows(self.proc.pid)
                what = seen and f"showed a window ({seen[0][0]} {seen[0][2]}x{seen[0][3]})"
            else:
                try:
                    seen = display_connections(self.proc.pid)
                    what = seen and f"connected to a display server ({seen[0][0]}: {seen[0][1]})"
                except DisplayUncheckable as e:
                    seen, what = True, f"could not be checked ({e})"
            if seen:
                self.violation = f"{self.label} {what} with no on-screen run approved: ended"
                print("desktop guard: " + self.violation, flush=True)
                self.end()
                return

    def end(self):
        """the process and, elsewhere than Windows, every descendant (the review's L-5: a child may own the window);
        Windows: the process (its job ends the rest)"""
        pids = [self.proc.pid] if WIN else list(reversed(process_tree(self.proc.pid)))
        for pid in pids:
            try:
                if pid == self.proc.pid:
                    self.proc.kill()
                else:
                    os.kill(pid, 9)
            except OSError:
                pass


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

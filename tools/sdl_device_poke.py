#!/usr/bin/env python3
"""sdl_device_poke.py: make a running game's SDL see a device change, without plugging anything in (Windows).

SDL 2.32's joystick code (src/joystick/windows/SDL_windowsjoystick.c) learns of a USB device arriving or leaving
through a message-only window of class "Message" on its joystick thread: on WM_DEVICECHANGE it sets two timers
(300 ms and 2 s), and on each timer's WM_TIMER (ids 1200 and 1201) it marks the device list changed, so the next
SDL_JoystickUpdate re-enumerates every DirectInput and XInput device -- under SDL's joystick lock, which both the
window's event pump and the game thread's pad poll take. This posts that WM_TIMER to the game's own window, so the
re-enumeration a real plug or unplug causes happens on demand: the controlled reproduction of the title-screen
freezes (docs/PC_EXPERIENCE.md section 7b). It changes nothing outside the game process.

  sdl_device_poke.py [--name dotheroes.exe | --pid N] [--count N] [--interval S] [--wait S]

Prints each post's time (the system's monotonic clock, ms) so a run's log lines can be matched to it.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import sys
import time

HWND_MESSAGE = wt.HWND(-3)
WM_TIMER = 0x0113
IDT_SDL_DEVICE_CHANGE_TIMER_1 = 1200   # SDL_windowsjoystick.c

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
user32.FindWindowExW.restype = wt.HWND
user32.FindWindowExW.argtypes = [wt.HWND, wt.HWND, wt.LPCWSTR, wt.LPCWSTR]
user32.GetWindowThreadProcessId.restype = wt.DWORD
user32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
user32.PostMessageW.restype = wt.BOOL
user32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
kernel32.GetTickCount64.restype = ctypes.c_ulonglong


def pids_named(name):
    """Process ids by executable name (Toolhelp32)."""
    TH32CS_SNAPPROCESS = 0x2

    class PROCESSENTRY32W(ctypes.Structure):
        _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
                    ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", wt.DWORD), ("cntThreads", wt.DWORD),
                    ("th32ParentProcessID", wt.DWORD), ("pcPriClassBase", ctypes.c_long), ("dwFlags", wt.DWORD),
                    ("szExeFile", ctypes.c_wchar * 260)]
    kernel32.CreateToolhelp32Snapshot.restype = wt.HANDLE
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    e = PROCESSENTRY32W()
    e.dwSize = ctypes.sizeof(e)
    out = []
    ok = kernel32.Process32FirstW(snap, ctypes.byref(e))
    while ok:
        if e.szExeFile.lower() == name.lower():
            out.append(e.th32ProcessID)
        ok = kernel32.Process32NextW(snap, ctypes.byref(e))
    kernel32.CloseHandle(snap)
    return out


def sdl_windows(pid):
    """The process's message-only windows of class "Message" (SDL's device-notification window among them)."""
    out, after = [], None
    while True:
        h = user32.FindWindowExW(HWND_MESSAGE, after, "Message", None)
        if not h:
            return out
        p = wt.DWORD(0)
        user32.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid:
            out.append(h)
        after = h


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--name", default="dotheroes.exe")
    ap.add_argument("--pid", type=int)
    ap.add_argument("--count", type=int, default=1)
    ap.add_argument("--interval", type=float, default=5.0)
    ap.add_argument("--wait", type=float, default=0.0, help="seconds to wait for the process and its window first")
    a = ap.parse_args()
    deadline = time.monotonic() + a.wait
    while True:
        pids = [a.pid] if a.pid else pids_named(a.name)
        wins = [(p, h) for p in pids for h in sdl_windows(p)]
        if wins or time.monotonic() >= deadline:
            break
        time.sleep(0.5)
    if not wins:
        sys.exit("no SDL device-notification window found (is the game running, with SDL's joystick subsystem?)")
    for i in range(a.count):
        if i:
            time.sleep(a.interval)
        for p, h in wins:
            ok = user32.PostMessageW(h, WM_TIMER, IDT_SDL_DEVICE_CHANGE_TIMER_1, 0)
            print("poke %d: pid %d window %#x at %d ms (tick): %s" % (
                i + 1, p, h or 0, kernel32.GetTickCount64(), "posted" if ok else "failed %d" % ctypes.get_last_error()),
                flush=True)


if __name__ == "__main__":
    main()

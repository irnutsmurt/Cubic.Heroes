#!/usr/bin/env python3
"""3D Dot Game Heroes native PC port: the setup app.

Turns your own disc (a decrypted ISO of BLUS-30490, or its folder) into the
native game, on your machine: pick the disc, your keys file (or an EBOOT.elf
you decrypted) and an install folder, Convert, Play. Nothing from the game is
shipped with this app; it reads your disc and builds from it locally
(tools/setup_pipeline.py).
"""
import json
import os
import queue
import shutil
import subprocess
import sys
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

ROOT = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import setup_pipeline as pipeline  # noqa: E402
import self_update  # noqa: E402
import shortcuts  # noqa: E402
from version import VERSION  # noqa: E402

TITLE = "3D Dot Game Heroes — PC port setup"
NEEDED_GB = 6  # extracted disc (3.3 GB) + recompiled code + compiled objects
UPDATE_GB = 1.5  # an update: recompiled code + objects
FPS_CHOICES = [("60 fps (smoother; the game runs at the same speed)", "60"),
               ("30 fps (the original)", "30"), ("Unlimited", "0")]


def settings_path():
    """This app's own settings (the last disc image and install folder), per user."""
    if os.name == "nt":
        base = os.environ.get("APPDATA") or os.path.expanduser("~")
        return os.path.join(base, "DotHeroes-Setup", "settings.json")
    base = os.environ.get("XDG_CONFIG_HOME") or os.path.join(os.path.expanduser("~"), ".config")
    return os.path.join(base, "dotheroes-setup", "settings.json")


def load_settings():
    try:
        with open(settings_path()) as f:
            v = json.load(f)
        return v if isinstance(v, dict) else {}
    except (OSError, ValueError):
        return {}


def save_settings(**values):
    v = load_settings()
    v.update({k: x for k, x in values.items() if x is not None})
    try:
        os.makedirs(os.path.dirname(settings_path()), exist_ok=True)
        with open(settings_path(), "w") as f:
            json.dump(v, f, indent=1)
    except OSError:
        pass  # remembering is a convenience


def same_file(a, b):
    try:
        return bool(a and b) and os.path.samefile(a, b)
    except OSError:
        return False


# ---------------- look: dark (the default) or light ----------------

PALETTES = {
    "dark": {"bg": "#1e2026", "panel": "#262931", "field": "#2e323b", "fg": "#e6e8ee", "muted": "#a4a9b6",
             "faint": "#7d8391", "border": "#3c414c", "button": "#343944", "button_hi": "#414755",
             "accent": "#4c8dff", "link": "#6ea8ff", "ok": "#5cc46a", "busy": "#f0a030", "err": "#ff6b6b",
             "trough": "#2e323b", "select": "#3d5a8a"},
    "light": {"bg": "#f2f2f2", "panel": "#f2f2f2", "field": "#ffffff", "fg": "#1e1e1e", "muted": "#555555",
              "faint": "#777777", "border": "#b8b8b8", "button": "#e4e4e4", "button_hi": "#d6d6d6",
              "accent": "#3576d0", "link": "#1a5fb4", "ok": "#2a8a2a", "busy": "#c07000", "err": "#c02020",
              "trough": "#dcdcdc", "select": "#b5cdf0"},
}


def apply_theme(root, dark, log=None):
    """Colour every widget: ttk styles (Muted / Faint / Link / Ok / Busy / Err .TLabel for coloured text),
    the window, the log, and on Windows the title bar."""
    c = PALETTES["dark" if dark else "light"]
    style = ttk.Style(root)
    if "clam" in style.theme_names():
        style.theme_use("clam")  # the one theme every colour can be set in, on Windows too
    style.configure(".", background=c["bg"], foreground=c["fg"], fieldbackground=c["field"],
                    bordercolor=c["border"], lightcolor=c["bg"], darkcolor=c["bg"], troughcolor=c["trough"],
                    selectbackground=c["select"], selectforeground=c["fg"], insertcolor=c["fg"])
    style.map(".", foreground=[("disabled", c["faint"])])
    style.configure("TLabelframe", background=c["bg"], bordercolor=c["border"])
    style.configure("TLabelframe.Label", background=c["bg"], foreground=c["muted"])
    style.configure("TButton", background=c["button"], bordercolor=c["border"], focuscolor=c["accent"],
                    lightcolor=c["button"], darkcolor=c["button"], padding=(10, 4))
    style.map("TButton", background=[("disabled", c["bg"]), ("pressed", c["accent"]), ("active", c["button_hi"])],
              foreground=[("disabled", c["faint"])])
    style.configure("TEntry", fieldbackground=c["field"], foreground=c["fg"], insertcolor=c["fg"])
    style.map("TEntry", bordercolor=[("focus", c["accent"])])
    style.configure("TCheckbutton", background=c["bg"], foreground=c["fg"], indicatorbackground=c["field"],
                    indicatorforeground=c["fg"])
    style.map("TCheckbutton", background=[("active", c["bg"])], indicatorbackground=[("selected", c["accent"])])
    style.configure("Horizontal.TProgressbar", background=c["accent"], troughcolor=c["trough"],
                    bordercolor=c["border"], lightcolor=c["accent"], darkcolor=c["accent"])
    for name, key in (("Muted", "muted"), ("Faint", "faint"), ("Link", "link"), ("Ok", "ok"), ("Busy", "busy"),
                      ("Err", "err")):
        style.configure(f"{name}.TLabel", foreground=c[key])
    if not hasattr(root, "skip_font"):  # a step that won't run: struck through (the font kept alive on the window)
        from tkinter import font
        root.skip_font = font.nametofont("TkDefaultFont").copy()
        root.skip_font.configure(overstrike=1)
    style.configure("Skip.TLabel", foreground=c["faint"], font=root.skip_font)
    root.configure(background=c["bg"])
    if log is not None:
        log.configure(background=c["field"], foreground=c["fg"], insertbackground=c["fg"],
                      selectbackground=c["select"], highlightthickness=0, relief="flat")
    if os.name == "nt":  # the title bar (Windows 10 1809+ / 11; ignored elsewhere)
        try:
            import ctypes
            root.update_idletasks()
            hwnd = ctypes.windll.user32.GetParent(root.winfo_id())
            on = ctypes.c_int(1 if dark else 0)
            for attr in (20, 19):  # DWMWA_USE_IMMERSIVE_DARK_MODE (and its pre-20H1 number)
                if not ctypes.windll.dwmapi.DwmSetWindowAttribute(hwnd, attr, ctypes.byref(on), ctypes.sizeof(on)):
                    break
        except Exception:
            pass


class Spinner:
    """A small turning arc next to the step that is running (drawn, so it looks the same everywhere)."""
    SIZE = 14

    def __init__(self, parent, dark):
        self.canvas = tk.Canvas(parent, width=self.SIZE, height=self.SIZE, highlightthickness=0, borderwidth=0)
        self.arc = self.canvas.create_arc(2, 2, self.SIZE - 2, self.SIZE - 2, start=0, extent=270, style="arc",
                                          width=2)
        self.angle, self.running = 0, False
        self.recolor(dark)

    def recolor(self, dark):
        c = PALETTES["dark" if dark else "light"]
        self.canvas.configure(background=c["bg"])
        self.canvas.itemconfigure(self.arc, outline=c["busy"])

    def show(self, row):
        self.canvas.grid(row=row, column=0, padx=(10, 4), pady=1)
        if not self.running:
            self.running = True
            self.turn()

    def hide(self):
        self.running = False
        self.canvas.grid_remove()

    def turn(self):
        if not self.running:
            return
        self.angle = (self.angle - 30) % 360
        self.canvas.itemconfigure(self.arc, start=self.angle)
        self.canvas.after(70, self.turn)


def default_install():
    if os.name == "nt":
        base = os.environ.get("LOCALAPPDATA") or os.path.expanduser("~")
        return os.path.join(base, "3D Dot Game Heroes")
    return os.path.join(os.path.expanduser("~"), "Games", "3D Dot Game Heroes")


def game_settings_path(install):
    return os.path.join(install, "dotheroes.cfg")


def read_game_settings(install):
    """<install>/dotheroes.cfg: NAME=value lines the game reads at start (port/main.cpp)"""
    out = {}
    try:
        for line in open(game_settings_path(install)):
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                out[k.strip()] = v.strip()
    except OSError:
        pass
    return out


# The settings that hold 60 fps in debris-heavy fights (docs/HANDOFF.md,
# "Performance configuration"): native collision and three native solver
# commands, the capped command-ring lap, one present per flip. Without them an
# install fights at p99 ~27 ms (10-16% of frames over 18 ms); with them 17.3-17.5,
# as in development. Experimental: native collision is not bit-exact with the
# game's SPU task. Written for a new install and added to an older one's
# settings if missing; a player's own value is never replaced.
PERFORMANCE = {"DOTHEROES_NATIVE_CD": "1", "DOTHEROES_NATIVE_SOLVER": "4,5,6", "GCM_WRAP_LAP": "1",
               "DOTHEROES_PRESENT_ONCE": "1"}


def ensure_performance_settings(install):
    cur = read_game_settings(install)
    missing = {k: v for k, v in PERFORMANCE.items() if k not in cur}
    if missing or not os.path.exists(game_settings_path(install)):
        write_game_settings(install, **missing)


def write_game_settings(install, **values):
    cur = read_game_settings(install)
    cur.update(values)
    try:
        with open(game_settings_path(install), "w") as f:
            f.write("# 3D Dot Game Heroes settings (the setup app writes this; editing by hand is fine)\n")
            f.write("# DOTHEROES_FPS: 60, 30 (the original) or 0 (no limit)\n")
            f.write("# PS3RECOMP_VK_FULLSCREEN: 1 = fullscreen\n")
            f.write("# DOTHEROES_NATIVE_CD, DOTHEROES_NATIVE_SOLVER, GCM_WRAP_LAP, DOTHEROES_PRESENT_ONCE: the\n")
            f.write("#   settings that hold 60 fps in fights (0 / empty: the original's way, slower)\n")
            for k, v in sorted(cur.items()):
                f.write(f"{k}={v}\n")
    except OSError:
        pass


class App:
    def __init__(self, root, after_update=None, update_failed=False):
        self.root = root
        self.update_failed = update_failed  # started again by the update's helper, which had to undo it
        self.events = queue.Queue()
        self.worker = None
        self.game = None
        self.info = None  # the install's install.json
        root.title(f"{TITLE} {VERSION}")
        root.minsize(620, 520)
        self.dark = tk.BooleanVar(value=load_settings().get("dark", True))
        apply_theme(root, self.dark.get())
        pad = {"padx": 12, "pady": 6}

        ttk.Label(root, text="3D Dot Game Heroes", font=("TkDefaultFont", 16, "bold")).pack(anchor="w", padx=12,
                                                                                           pady=(14, 0))
        self.subtitle = ttk.Label(root, text="Builds the native PC version from your own PS3 disc. Nothing from "
                                             "the game is included with this app.", wraplength=590, style="Muted.TLabel")
        self.subtitle.pack(anchor="w", **pad)

        self.update_bar = ttk.Frame(root)  # shown when a newer version of this app is out
        self.update_label = ttk.Label(self.update_bar, style="Link.TLabel", wraplength=470)
        self.update_label.pack(side="left")
        self.update_btn = ttk.Button(self.update_bar, text="Get it", command=self.get_new_app)
        self.update_btn.pack(side="left", padx=8)
        self.new_app = None
        remembered = load_settings()
        self.auto_update = tk.BooleanVar(value=bool(remembered.get("auto_update_game", False)))
        ttk.Checkbutton(self.update_bar, text="Then update the game by itself", variable=self.auto_update,
                        command=lambda: save_settings(auto_update_game=self.auto_update.get())).pack(side="left")

        form = ttk.Frame(root)
        form.pack(fill="x", **pad)
        form.columnconfigure(1, weight=1)
        ttk.Label(form, text="Disc").grid(row=0, column=0, sticky="w")
        last_image = remembered.get("image", "")
        self.image = tk.StringVar(value=last_image if os.path.exists(last_image) else "")
        ttk.Entry(form, textvariable=self.image).grid(row=0, column=1, sticky="ew", padx=6)
        disc_btns = ttk.Frame(form)
        disc_btns.grid(row=0, column=2)
        ttk.Button(disc_btns, text="ISO…", command=self.pick_image).pack(side="left")
        ttk.Button(disc_btns, text="Folder…", command=self.pick_folder).pack(side="left", padx=(4, 0))
        ttk.Label(form, text="Your decrypted ISO of 3D Dot Game Heroes (US, BLUS-30490), or its folder (PS3_GAME)",
                  style="Faint.TLabel").grid(row=1, column=1, sticky="w", padx=6)
        ttk.Label(form, text="Keys").grid(row=2, column=0, sticky="w", pady=(8, 0))
        last_keys = remembered.get("keys", "")
        self.keys = tk.StringVar(value=last_keys if os.path.isfile(last_keys) else "")
        ttk.Entry(form, textvariable=self.keys).grid(row=2, column=1, sticky="ew", padx=6, pady=(8, 0))
        ttk.Button(form, text="Browse…", command=self.pick_keys).grid(row=2, column=2, pady=(8, 0))
        ttk.Label(form, text="Your keys file (APP key revision 0001), or an EBOOT.elf you decrypted",
                  style="Faint.TLabel").grid(row=3, column=1, sticky="w", padx=6)
        ttk.Label(form, text="Install to").grid(row=4, column=0, sticky="w", pady=(8, 0))
        self.install = tk.StringVar(value=(after_update or {}).get("install") or remembered.get("install")
                                    or default_install())
        ttk.Entry(form, textvariable=self.install).grid(row=4, column=1, sticky="ew", padx=6, pady=(8, 0))
        ttk.Button(form, text="Browse…", command=self.pick_install).grid(row=4, column=2, pady=(8, 0))
        self.desktop = tk.BooleanVar(value=True)
        ttk.Checkbutton(form, text="Create a desktop shortcut", variable=self.desktop).grid(
            row=5, column=1, sticky="w", padx=6, pady=(6, 0))
        # an update does only what changed (pipeline.py); this does it all again
        self.full = tk.BooleanVar(value=False)
        self.full_box = ttk.Checkbutton(form, text="Rebuild everything (slower: only if an update misbehaves)",
                                        variable=self.full)
        self.full_box.grid(row=6, column=1, sticky="w", padx=6, pady=(2, 0))

        opts = ttk.LabelFrame(root, text="Game settings")
        opts.pack(fill="x", **pad)
        ttk.Label(opts, text="Frame rate").grid(row=0, column=0, sticky="w", padx=(10, 6), pady=4)
        self.fps = tk.StringVar(value=FPS_CHOICES[0][0])
        fps_box = ttk.Combobox(opts, textvariable=self.fps, values=[c[0] for c in FPS_CHOICES], state="readonly",
                               width=48)
        fps_box.grid(row=0, column=1, sticky="w", pady=4)
        fps_box.bind("<<ComboboxSelected>>", lambda _e: self.save_game_settings())
        self.fullscreen = tk.BooleanVar(value=False)
        ttk.Checkbutton(opts, text="Fullscreen", variable=self.fullscreen,
                        command=self.save_game_settings).grid(row=1, column=1, sticky="w", pady=(0, 6))

        steps = ttk.LabelFrame(root, text="Progress")
        steps.pack(fill="x", **pad)
        self.marks, self.step_labels, self.skipped = {}, {}, set()
        for i, s in enumerate(pipeline.STEPS):
            mark = ttk.Label(steps, text="○", width=2)
            mark.grid(row=i, column=0, padx=(10, 4), pady=1)
            self.step_labels[s] = ttk.Label(steps, text=pipeline.STEP_NAMES[s])
            self.step_labels[s].grid(row=i, column=1, sticky="w")
            self.marks[s] = mark
        self.spinner = Spinner(steps, self.dark.get())
        self.current = None  # the step running
        self.bar = ttk.Progressbar(steps, maximum=1.0)
        self.bar.grid(row=len(pipeline.STEPS), column=0, columnspan=2, sticky="ew", padx=10, pady=(8, 2))
        steps.columnconfigure(1, weight=1)
        self.status = ttk.Label(steps, text="Ready.", style="Muted.TLabel", wraplength=570)
        self.status.grid(row=len(pipeline.STEPS) + 1, column=0, columnspan=2, sticky="w", padx=10, pady=(0, 8))

        self.log_shown = tk.BooleanVar(value=False)
        toggles = ttk.Frame(root)
        toggles.pack(fill="x", padx=12)
        ttk.Checkbutton(toggles, text="Show details", variable=self.log_shown, command=self.toggle_log).pack(
            side="left")
        ttk.Checkbutton(toggles, text="Dark mode", variable=self.dark, command=self.toggle_dark).pack(side="right")
        self.log = tk.Text(root, height=8, wrap="word", state="disabled", font=("TkFixedFont", 9))
        apply_theme(root, self.dark.get(), self.log)

        buttons = ttk.Frame(root)
        buttons.pack(side="bottom", fill="x", padx=12, pady=12)
        self.convert_btn = ttk.Button(buttons, text="Convert", command=self.start)
        self.convert_btn.pack(side="right")
        self.play_btn = ttk.Button(buttons, text="Play", command=self.play, state="disabled")
        self.play_btn.pack(side="right", padx=6)
        self.folder_btn = ttk.Button(buttons, text="Open folder", command=self.open_folder, state="disabled")
        self.folder_btn.pack(side="right")
        self.steam_btn = ttk.Button(buttons, text="Add to Steam", command=self.add_to_steam, state="disabled")
        self.steam_btn.pack(side="left")
        self.root.after(100, self.poll)
        self.find_existing()
        if not self.image.get() and self.info and os.path.exists(self.info.get("source") or ""):
            self.image.set(self.info["source"])  # the disc this install was made from
        self.install.trace_add("write", lambda *_: self.find_existing())
        self.image.trace_add("write", lambda *_: self.find_existing())
        if after_update:  # this is a new version the old one just downloaded
            threading.Thread(target=lambda: self_update.remove_old(after_update.get("old")), daemon=True).start()
            if self.info:
                if not self.is_update():
                    self.image.set("")  # an update, never a conversion from another image
                if self.auto_update.get():
                    self.root.after(500, self.start)  # bring the game up to this version straight away
                else:
                    self.status.config(text=f"This app is now version {VERSION}. Press Update to rebuild your game "
                                            "with it (your saves and settings stay).")
        elif self_update.current_folder():
            threading.Thread(target=self.check_for_app_update, daemon=True).start()

    # ---------------- inputs ----------------

    def pick_image(self):
        p = filedialog.askopenfilename(title="Your disc image", filetypes=[
            ("Disc images", "*.iso *.ISO"), ("All files", "*")])
        if p:
            self.image.set(p)
            save_settings(image=os.path.abspath(p))

    def pick_folder(self):
        p = filedialog.askdirectory(title="Your disc's folder (the one holding PS3_GAME)")
        if p:
            self.image.set(p)
            save_settings(image=os.path.abspath(p))

    def pick_keys(self):
        p = filedialog.askopenfilename(title="Your keys file, or a decrypted EBOOT.elf", filetypes=[
            ("Keys or EBOOT.elf", "*.txt *.keys *.elf *.ELF"), ("All files", "*")])
        if p:
            self.keys.set(p)
            save_settings(keys=os.path.abspath(p))

    def save_game_settings(self):
        install = self.install.get().strip()
        if not os.path.isdir(install):
            return
        fps = next(v for label, v in FPS_CHOICES if label == self.fps.get())
        write_game_settings(install, DOTHEROES_FPS=fps, PS3RECOMP_VK_FULLSCREEN="1" if self.fullscreen.get() else "0")

    def load_game_settings(self):
        cur = read_game_settings(self.install.get().strip())
        fps = cur.get("DOTHEROES_FPS", "60")
        self.fps.set(next((label for label, v in FPS_CHOICES if v == fps), FPS_CHOICES[0][0]))
        self.fullscreen.set(cur.get("PS3RECOMP_VK_FULLSCREEN") == "1")

    def pick_install(self):
        p = filedialog.askdirectory(title="Install the game to", initialdir=os.path.dirname(self.install.get()))
        if p:
            name = "3D Dot Game Heroes"
            self.install.set(os.path.join(p, name) if os.listdir(p) and os.path.basename(p) != name else p)
            save_settings(install=os.path.abspath(self.install.get()))

    def find_existing(self):
        install = self.install.get().strip()
        exe = os.path.join(install, pipeline.exe_name())
        self.info = pipeline.install_info(install) if os.path.exists(exe) else None
        installed = os.path.exists(exe)
        self.game = exe if installed else None
        for b in (self.play_btn, self.folder_btn, self.steam_btn):
            b.config(state="normal" if installed else "disabled")
        can_update = self.is_update()
        self.convert_btn.config(text="Update" if can_update else "Convert")
        if can_update:
            self.full_box.grid()
        else:
            self.full_box.grid_remove()
            self.full.set(False)
        self.show_skipped()
        if installed:
            self.load_game_settings()
        if not installed:
            self.status.config(text="Ready.")
        elif can_update:
            built = self.info.get("version", "?")
            self.status.config(text=f"Installed (version {built}). Update rebuilds it with version {VERSION}, "
                                    "keeping your saves and settings. Choose a different disc image only to convert "
                                    "from scratch.")
        else:
            self.status.config(text="Already installed. Press Convert to rebuild it from the chosen disc image."
                               if self.image.get().strip() else
                               "Already installed. Choose your disc image and press Convert to rebuild it.")

    def show_skipped(self):
        """strike through the steps that won't run (while nothing is running)"""
        if self.worker and self.worker.is_alive():
            return
        self.skipped = {"verify", "extract", "decrypt"} if self.is_update() else set()
        for s in pipeline.STEPS:
            self.step_labels[s].config(text=pipeline.STEP_NAMES[s],
                                       style="Skip.TLabel" if s in self.skipped else "TLabel")

    def is_update(self):
        """An install to bring up to date: no image chosen, or the one it was made from."""
        if not (self.info and os.path.exists(pipeline.Paths(self.install.get().strip()).elf)):
            return False
        image = self.image.get().strip()
        return not image or same_file(image, self.info.get("source"))

    def toggle_dark(self):
        apply_theme(self.root, self.dark.get(), self.log)
        self.spinner.recolor(self.dark.get())
        save_settings(dark=self.dark.get())

    def toggle_log(self):
        if self.log_shown.get():
            self.log.pack(fill="both", expand=True, padx=12, pady=(0, 6))
        else:
            self.log.pack_forget()
        self.root.geometry("")  # grow / shrink to fit

    def add_log(self, text):
        self.log.config(state="normal")
        self.log.insert("end", text + "\n")
        self.log.see("end")
        self.log.config(state="disabled")

    # ---------------- converting ----------------

    def start(self):
        image, install = self.image.get().strip(), self.install.get().strip()
        keys = self.keys.get().strip()
        update = self.is_update()
        if not image and not update:
            return self.fail("Choose your disc first.")
        if image and not os.path.exists(image):
            return self.fail("That disc wasn't found.")
        if not update and not keys:
            return self.fail("Choose your keys file (or an EBOOT.elf you decrypted).")
        if keys and not os.path.isfile(keys):
            return self.fail("That keys file wasn't found.")
        try:
            os.makedirs(install, exist_ok=True)
        except OSError as e:
            return self.fail(f"Can't create the install folder ({e.strerror}).")
        free, need = shutil.disk_usage(install).free / 2**30, UPDATE_GB if update else NEEDED_GB
        if free < need:
            return self.fail(f"Not enough free space: about {need} GB is needed ({free:.1f} GB free).")
        self.show_skipped()
        for s in pipeline.STEPS:
            self.marks[s].config(text="–" if s in self.skipped else "○", style="TLabel")
        self.convert_btn.config(state="disabled")
        self.play_btn.config(state="disabled")
        self.bar.config(value=0)
        save_settings(image=os.path.abspath(image) if image else None, install=os.path.abspath(install),
                      keys=os.path.abspath(keys) if keys else None)
        is_elf = bool(keys) and open(keys, "rb").read(4) == b"\x7fELF"
        self.worker = threading.Thread(target=self.work, args=(
            None if update else image, install, self.desktop.get(), None if is_elf else keys,
            keys if is_elf else None, update and self.full.get()), daemon=True)
        self.worker.start()

    def work(self, image, install, desktop, keys, elf, full=False):
        try:
            out = pipeline.run(install, source=image, keys=keys, elf=elf,
                               progress=lambda st, f, m: self.events.put(("progress", st, f, m)),
                               log=lambda m: self.events.put(("log", m)),
                               skip=lambda st: self.events.put(("skip", st)), full=full)
            if not os.path.exists(game_settings_path(install)):
                write_game_settings(install, DOTHEROES_FPS="60", PS3RECOMP_VK_FULLSCREEN="0")
            ensure_performance_settings(install)
            if image is not None:  # an update keeps the shortcuts as the player left them
                try:
                    made = shortcuts.create_shortcuts(out, self.icon_for(out), desktop=desktop)
                    self.events.put(("log", "shortcuts: " + ", ".join(made)))
                except Exception as e:  # shortcuts are a convenience: never fail the install over them
                    self.events.put(("log", f"shortcuts: not created ({e})"))
            self.events.put(("done", out))
        except (pipeline.SetupError, pipeline.zig_build.BuildError) as e:
            self.events.put(("error", str(e)))
        except Exception as e:  # anything unexpected: still tell the player
            self.events.put(("error", f"something went wrong: {e!r}"))

    def poll(self):
        try:
            while True:
                ev = self.events.get_nowait()
                if ev[0] == "progress":
                    _, st, frac, msg = ev
                    i = pipeline.STEPS.index(st)
                    if st in self.skipped:  # it runs after all
                        self.skipped.discard(st)
                        self.step_labels[st].config(text=pipeline.STEP_NAMES[st], style="TLabel")
                    for s in pipeline.STEPS[:i]:
                        if s not in self.skipped:
                            self.marks[s].config(text="✔", style="Ok.TLabel")
                    self.marks[st].config(text="", style="TLabel")  # the spinner turns in its place
                    if self.current != st:
                        self.current = st
                        self.spinner.show(i)
                    self.bar.config(value=frac)
                    self.status.config(text=msg + (f"  {int(frac * 100)}%" if 0 < frac < 1 else ""))
                elif ev[0] == "log":
                    self.add_log(ev[1])
                elif ev[0] == "skip":  # nothing changed for this step since the last build: its work reused
                    st = ev[1]
                    self.skipped.add(st)
                    self.marks[st].config(text="–", style="TLabel")
                    self.step_labels[st].config(text=pipeline.STEP_NAMES[st] + " (unchanged)", style="Faint.TLabel")
                elif ev[0] == "done":
                    self.spinner.hide()
                    self.current = None
                    for s in pipeline.STEPS:
                        if s not in self.skipped:
                            self.marks[s].config(text="✔", style="Ok.TLabel")
                    self.game = ev[1]
                    self.bar.config(value=1)
                    self.status.config(text="Done! Press Play.", style="Muted.TLabel")
                    self.info = pipeline.install_info(self.install.get().strip())
                    self.load_game_settings()
                    self.convert_btn.config(text="Update" if self.is_update() else "Convert")
                    self.convert_btn.config(state="normal")
                    self.play_btn.config(state="normal")
                    self.folder_btn.config(state="normal")
                    self.steam_btn.config(state="normal")
                elif ev[0] == "app-update":
                    self.new_app = ev[1]
                    self.update_label.config(text=f"Version {ev[1][0]} of this app is available." + (
                        " The last try couldn't replace this app's files (something still had them open): "
                        "close anything else running from its folder and try again." if self.update_failed else ""))
                    self.update_bar.pack(after=self.subtitle, anchor="w", padx=12, pady=(0, 4))
                elif ev[0] == "app-progress":
                    self.update_label.config(text=ev[1])
                elif ev[0] == "app-ready":
                    self.update_label.config(text="Installing the new version… the app restarts by itself.")
                    self.root.update_idletasks()
                    try:
                        self_update.apply_and_restart(ev[1], self_update.current_folder(), self.install.get().strip())
                    except OSError as e:
                        self.update_btn.config(state="normal")
                        self.update_label.config(text=f"Couldn't install the new version ({e}).")
                        continue
                    self.root.destroy()
                    return
                elif ev[0] == "app-failed":
                    self.update_btn.config(state="normal")
                    self.update_label.config(text=ev[1])
                elif ev[0] == "steam":
                    self.steam_btn.config(state="normal")
                    if ev[1]:
                        self.status.config(text="Added to your Steam library, with artwork. Start Steam to see it.",
                                           style="Muted.TLabel")
                        self.add_log("steam: " + ", ".join(ev[2]))
                    else:
                        self.fail(ev[2])
                elif ev[0] == "error":
                    self.spinner.hide()
                    if self.current:
                        self.marks[self.current].config(text="✖", style="Err.TLabel")
                    self.current = None
                    self.convert_btn.config(state="normal")
                    self.fail(ev[1][:1].upper() + ev[1][1:])
        except queue.Empty:
            pass
        self.root.after(100, self.poll)

    def fail(self, msg):
        self.status.config(text=msg, style="Err.TLabel")
        self.add_log("error: " + msg)
        self.root.after(8000, lambda: self.status.config(style="Muted.TLabel"))

    # ---------------- this app's own updates ----------------

    def check_for_app_update(self):
        try:
            new = self_update.latest()
        except Exception:
            return  # offline, or GitHub unreachable: try again next time
        if new and self_update.parse(new[0]) > self_update.parse(VERSION):
            self.events.put(("app-update", new))

    def get_new_app(self):
        if not self.new_app or (self.worker and self.worker.is_alive()):
            return
        self.update_btn.config(state="disabled")
        version, url = self.new_app
        app_dir = self_update.current_folder()

        def work():
            try:
                staged = self_update.stage(
                    url, version, app_dir,
                    progress=lambda d, t: self.events.put(("app-progress", f"Downloading version {version}… "
                                                           f"{d * 100 // t if t else d >> 20}{'%' if t else ' MB'}")))
                self.events.put(("app-ready", staged))
            except Exception as e:
                self.events.put(("app-failed", f"The download failed ({e}). Try again later."))
        threading.Thread(target=work, daemon=True).start()

    # ---------------- after ----------------

    @staticmethod
    def icon_for(exe):
        # Windows: the icon inside dotheroes.exe; Linux: icon.png next to it
        return exe if os.name == "nt" else os.path.join(os.path.dirname(exe), "icon.png")

    def add_to_steam(self):
        if not self.game or not os.path.exists(self.game):
            return self.fail("Convert the game first.")
        if not shortcuts.steam_roots():
            return self.fail("Steam doesn't seem to be installed for this user.")
        running = shortcuts.steam_running()
        if running and not messagebox.askyesno(
                "Add to Steam", "Steam has to be closed while the game is added to your library.\n\n"
                                "Close Steam now? You can start it again right after."):
            return
        self.steam_btn.config(state="disabled")
        self.status.config(text="Closing Steam…" if running else "Adding to Steam…", style="Muted.TLabel")
        game = self.game

        def work():
            try:
                if running and not shortcuts.close_steam():
                    return self.events.put(("steam", False, "Steam didn't close. Close it yourself, then try again."))
                written = shortcuts.add_to_steam(game, self.icon_for(game))
                if written:
                    self.events.put(("steam", True, written))
                else:
                    self.events.put(("steam", False, "No Steam account found on this computer."))
            except Exception as e:
                self.events.put(("steam", False, f"Couldn't add the game to Steam ({e})."))
        threading.Thread(target=work, daemon=True).start()

    def play(self):
        if self.game and os.path.exists(self.game):
            subprocess.Popen([self.game], cwd=os.path.dirname(self.game))

    def open_folder(self):
        folder = self.install.get()
        if os.name == "nt":
            os.startfile(folder)  # noqa: S606 (opens Explorer)
        else:
            subprocess.Popen(["xdg-open", folder])


def convert_headless(image, install, keys=None, full=False):
    """DotHeroes-Setup --convert DISC KEYS FOLDER: the same conversion without the window
    (KEYS: a keys file or a decrypted EBOOT.elf).
    DotHeroes-Setup --update FOLDER [--full]: update an install; --full: everything again."""
    def show(step, frac, msg):
        print(f"\r[{pipeline.STEPS.index(step) + 1}/{len(pipeline.STEPS)}] {msg} {int(frac * 100)}%   ",
              end="", flush=True)

    def skipped(step):
        print(f"\r[{pipeline.STEPS.index(step) + 1}/{len(pipeline.STEPS)}] {pipeline.STEP_NAMES[step]}: unchanged")
    is_elf = bool(keys) and open(keys, "rb").read(4) == b"\x7fELF"
    try:
        out = pipeline.run(install, source=image, keys=None if is_elf else keys, elf=keys if is_elf else None,
                           progress=show, log=lambda m: None, skip=skipped, full=full)
    except (pipeline.SetupError, pipeline.zig_build.BuildError) as e:
        sys.exit(f"\nfailed: {e}")
    if not os.path.exists(game_settings_path(install)):
        write_game_settings(install, DOTHEROES_FPS="60", PS3RECOMP_VK_FULLSCREEN="0")
    ensure_performance_settings(install)
    print(f"\ndone: {out}")


def run_script():
    """`DotHeroes-Setup <tool>.py args`: the lifting tools run as their own processes
    (setup_pipeline.run_tool); in the packaged app this executable runs them."""
    import runpy
    script = sys.argv[1]
    sys.argv = sys.argv[1:]
    sys.path.insert(0, os.path.dirname(os.path.abspath(script)))
    runpy.run_path(script, run_name="__main__")


def main():
    import multiprocessing
    multiprocessing.freeze_support()  # the lifter's worker processes, in the packaged app
    if len(sys.argv) >= 2 and sys.argv[1].endswith(".py") and os.path.isfile(sys.argv[1]):
        return run_script()
    if len(sys.argv) == 5 and sys.argv[1] == "--convert":
        return convert_headless(os.path.abspath(sys.argv[2]), os.path.abspath(sys.argv[4]),
                                os.path.abspath(sys.argv[3]))
    if len(sys.argv) in (3, 4) and sys.argv[1] == "--update":
        return convert_headless(None, os.path.abspath(sys.argv[2]), full=sys.argv[3:] == ["--full"])
    after = None
    if len(sys.argv) == 4 and sys.argv[1] == "--after-update":  # started by the previous version
        after = {"old": sys.argv[2], "install": sys.argv[3]}
    root = tk.Tk()
    App(root, after_update=after, update_failed=sys.argv[1:] == ["--update-failed"])
    root.mainloop()


if __name__ == "__main__":
    main()

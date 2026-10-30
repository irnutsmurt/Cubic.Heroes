#!/usr/bin/env python3
"""The setup pipeline: the player's own disc -> the native game, on their machine.

Shared by the setup app (setup_app.py) and its command line. Steps:
    1. verify   the disc is BLUS-30490 v01.00 (the ISO's SHA-256, or a disc
                folder's EBOOT.BIN)
    2. extract  its files into <install>/data/disc
    3. decrypt  EBOOT.BIN -> EBOOT.elf, with the player's keys file (or an
                EBOOT.elf they decrypted themselves); checked by SHA-256
    4. recomp   the PPU code lifted to C++ (ps3recomp's loader and lifter, with
                this port's function fixes, setjmp map and patch switches)
    5. spu      the SPU programs lifted to C
    6. build    compiled with the bundled Zig and linked with the prebuilt
                runtime (tools/zig_build.py)
    7. finish   the libraries and icon next to the game, install.json

Progress: progress(step, fraction, message); skip(step) when a step had
nothing to do. An update (no disc given: the install's own copy is used)
redoes only steps whose inputs changed (install.json "stamps": fingerprints of
the disc, the tools' code, the configuration and the prebuilt libraries);
compiled objects are cached by fingerprint in <install>/data/cache.

Nothing from the game ships with the app: everything here is made from the
player's disc and stays in their install folder.
"""
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys

# the source checkout, or the packaged app's folder (PyInstaller: next to the executable)
ROOT = (os.path.dirname(sys.executable) if getattr(sys, "frozen", False)
        else os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import zig_build  # noqa: E402

STEPS = ["verify", "extract", "decrypt", "recomp", "spu", "build", "finish"]
STEP_NAMES = {
    "verify": "Checking the disc",
    "extract": "Extracting the game files",
    "decrypt": "Decrypting the game program",
    "recomp": "Recompiling the game code (PPU)",
    "spu": "Recompiling the SPU programs",
    "build": "Building the game",
    "finish": "Finishing",
}
GAME = "dotheroes"
PPU_CHUNKS = "48"


class SetupError(Exception):
    pass


def config_dir():
    return os.path.join(ROOT, "config")


def ps3_tools():
    """ps3recomp's tools: tools/ps3recomp/ in the app, the pinned checkout in a source tree"""
    packaged = os.path.join(ROOT, "tools", "ps3recomp")
    if os.path.isdir(packaged):
        return packaged
    return os.path.join(os.environ.get("PS3RECOMP_DIR", os.path.join(ROOT, "build", "ref", "ps3recomp")), "tools")


def version_info():
    with open(os.path.join(config_dir(), "game_version.json")) as f:
        return json.load(f)


def exe_name(name=GAME):
    return name + (".exe" if os.name == "nt" else "")


def sha256_file(path, progress=None):
    h = hashlib.sha256()
    total, done = os.path.getsize(path), 0
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(8 << 20), b""):
            h.update(chunk)
            done += len(chunk)
            if progress:
                progress(done / total)
    return h.hexdigest()


def install_info(install):
    """What an earlier setup recorded in <install>/data/install.json, or None."""
    try:
        with open(os.path.join(install, "data", "install.json")) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


class Paths:
    """An install folder:
        <install>/dotheroes[.exe]   the game
        <install>/lib/              Linux: SDL2 (Windows: SDL2.dll beside the game)
        <install>/data/disc/        the disc's files, EBOOT.elf decrypted
        <install>/data/gen/         the recompiled code (work files)
        <install>/data/cache/       compiled objects (updates reuse them)
        <install>/data/gamedata/    saves
        <install>/data/install.json what this setup did"""

    def __init__(self, install):
        self.install = os.path.abspath(install)
        self.data = os.path.join(self.install, "data")
        self.disc = os.path.join(self.data, "disc")
        self.gen = os.path.join(self.data, "gen")
        self.cache = os.path.join(self.data, "cache")
        self.exe = os.path.join(self.install, exe_name())
        self.eboot_bin = os.path.join(self.disc, "PS3_GAME", "USRDIR", "EBOOT.BIN")
        self.elf = os.path.join(self.disc, "PS3_GAME", "USRDIR", "EBOOT.elf")


def tool_fingerprint(*names):
    """the code of the tools a step runs, and the configuration they read"""
    h = hashlib.sha1()
    for d in (ps3_tools(), os.path.join(ROOT, "tools"), config_dir()):
        for n in sorted(os.listdir(d)):
            if n.endswith((".py", ".json")):
                h.update(n.encode() + b"\0")
                with open(os.path.join(d, n), "rb") as f:
                    h.update(f.read())
    for n in names:
        h.update(n.encode())
    return h.hexdigest()


def run_tool(script, args, log):
    """Run one of the lifting tools as its own process: `python script args` from a
    checkout; in the packaged app, the app itself runs it (setup_app.py main)."""
    cmd = [sys.executable, script, *args]
    kw = {"creationflags": 0x08000000} if os.name == "nt" else {}
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, **kw)
    tail = []
    for line in p.stdout:
        line = line.rstrip()
        tail = (tail + [line])[-40:]
        if log:
            log(line)
    if p.wait():
        raise SetupError(f"{os.path.basename(script)} failed:\n" + "\n".join(tail[-12:]))


def spu_text_ea(eboot, image):
    """Guest EA of an extracted SPU image's executable segment inside the EBOOT:
    where its loader DMAs the code from (tools/pipeline.py, the same)."""
    e, b = open(eboot, "rb").read(), open(image, "rb").read()
    at = e.find(b[:0x200])
    if at < 0 or e.find(b[:0x200], at + 1) >= 0:
        raise SetupError(f"{os.path.basename(image)}: not found exactly once in the EBOOT")
    phoff, phn = struct.unpack(">I", b[0x1C:0x20])[0], struct.unpack(">H", b[0x2C:0x2E])[0]
    text = None
    for i in range(phn):
        t, off, va, pa, fsz, msz, fl, al = struct.unpack(">8I", b[phoff + i * 32:phoff + i * 32 + 32])
        if t == 1 and fl & 1:
            text = off
    eph, ephn = struct.unpack(">Q", e[0x20:0x28])[0], struct.unpack(">H", e[0x38:0x3A])[0]
    for i in range(ephn):
        t, fl, off, va, pa, fsz, msz, al = struct.unpack(">IIQQQQQQ", e[eph + i * 56:eph + i * 56 + 56])
        if t == 1 and off <= at < off + fsz:
            return va + at - off + text
    raise SetupError(f"{os.path.basename(image)}: not inside a loadable EBOOT segment")


# ---------------- the steps ----------------

def verify(source, progress):
    """source: the ISO or a disc folder. Returns its kind."""
    want = version_info()
    if os.path.isdir(source):
        bin_ = os.path.join(source, "PS3_GAME", "USRDIR", "EBOOT.BIN")
        if not os.path.exists(bin_):
            raise SetupError("That folder has no PS3_GAME/USRDIR/EBOOT.BIN: choose the disc's top folder.")
        if sha256_file(bin_) != want["files"]["PS3_GAME/USRDIR/EBOOT.BIN"]:
            raise SetupError("This isn't 3D Dot Game Heroes BLUS-30490 v01.00 (US): EBOOT.BIN doesn't match.")
        return "folder"
    if os.path.getsize(source) != want["disc_image"]["size"]:
        raise SetupError("This disc image isn't the supported one: 3D Dot Game Heroes BLUS-30490 v01.00 (US), "
                         "a decrypted ISO (the size doesn't match).")
    if sha256_file(source, progress) != want["disc_image"]["sha256"]:
        raise SetupError("This disc image doesn't match BLUS-30490 v01.00 (US) (SHA-256). "
                         "Is it a decrypted dump of the US disc?")
    return "iso"


def extract(source, kind, paths, progress, log):
    if os.path.isdir(paths.disc):
        shutil.rmtree(paths.disc)
    os.makedirs(paths.disc)
    if kind == "folder":
        files = [os.path.join(d, f) for d, _, fs in os.walk(os.path.join(source, "PS3_GAME")) for f in fs]
        total = sum(os.path.getsize(f) for f in files) or 1
        done = 0
        for f in files:
            dst = os.path.join(paths.disc, os.path.relpath(f, source))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copyfile(f, dst)
            done += os.path.getsize(f)
            progress(done / total)
        return
    import iso9660
    with open(source, "rb") as f:
        ents = [(p, ext) for p, ext in iso9660.entries(f) if p.startswith("PS3_GAME/")]
        total = sum(n for _, ext in ents for _, n in ext) or 1
        done = 0
        for p, ext in ents:
            dst = os.path.join(paths.disc, p)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "wb") as o:
                for lba, n in ext:
                    f.seek(lba * iso9660.SECTOR)
                    while n:
                        chunk = f.read(min(n, 8 << 20))
                        o.write(chunk)
                        n -= len(chunk)
                        done += len(chunk)
                        progress(done / total)
    want = version_info()["files"]["PS3_GAME/USRDIR/EBOOT.BIN"]
    if sha256_file(paths.eboot_bin) != want:
        raise SetupError("The extracted EBOOT.BIN doesn't match: the disc image may be damaged.")
    log(f"extracted {len(ents)} files")


def decrypt(paths, keys=None, elf=None):
    want = version_info()["decrypted_elf"]["sha256"]
    if elf:
        data = open(elf, "rb").read()
    else:
        import self_decrypt
        try:
            k = self_decrypt.load_keys(keys)
        except OSError as e:
            raise SetupError(f"Can't read the keys file ({e.strerror}).")
        if not k:
            raise SetupError("The keys file has no APP keys (lines: APP <revision> <erk> <riv>).")
        try:
            data = self_decrypt.decrypt_self(open(paths.eboot_bin, "rb").read(), k)
        except Exception as e:
            raise SetupError(f"Decrypting EBOOT.BIN failed ({e}). The keys file needs the APP key "
                             "revision 0x0001.")
    if hashlib.sha256(data).hexdigest() != want:
        raise SetupError("The decrypted program doesn't match the expected one: wrong keys, or a different "
                         "EBOOT.elf.")
    tmp = paths.elf + ".part"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, paths.elf)


def recomp(paths, log):
    t = ps3_tools()
    out = os.path.join(paths.gen, "out") + os.sep
    rec = os.path.join(paths.gen, "recompiled") + os.sep
    if os.path.isdir(rec):
        shutil.rmtree(rec)
    os.makedirs(out, exist_ok=True)
    run_tool(os.path.join(t, "ppu_loader.py"), [paths.elf, "-o", out], log)
    fixed = os.path.join(out, "EBOOT.functions.fixed.json")
    run_tool(os.path.join(ROOT, "tools", "fix_functions.py"),
             [paths.elf, os.path.join(out, "EBOOT.functions.json"), fixed], log)
    c = config_dir()
    run_tool(os.path.join(t, "ppu_lifter.py"),
             [paths.elf, "--functions", fixed, "--hle-stubs", os.path.join(out, "EBOOT.imports.json"),
              "--sjlj", os.path.join(c, "sjlj.json"), "--insn-patches", os.path.join(c, "insn_patches.json"),
              "--chunks", PPU_CHUNKS, "-o", rec], log)


def spu(paths, log):
    t = ps3_tools()
    spu_dir = os.path.join(paths.gen, "spu")
    if os.path.isdir(spu_dir):
        shutil.rmtree(spu_dir)
    images = os.path.join(spu_dir, "images")
    run_tool(os.path.join(t, "extract_spu_images.py"), [paths.elf, "--output", images], log)
    reloc = []
    with open(os.path.join(config_dir(), "spu_relocatable.json")) as f:
        for img in json.load(f)["images"]:
            reloc += ["--relocatable", f"{img}=0x{spu_text_ea(paths.elf, os.path.join(images, img + '.elf')):08X}"]
    run_tool(os.path.join(t, "build_spu_workloads.py"),
             ["--images", images, "--lifted", spu_dir, "--out", os.path.join(spu_dir, "spu_workloads.c"),
              "--constructor", "--relift", *reloc], log)


def finish(paths, log):
    lib = zig_build.Sources().lib_dir
    if os.name == "nt":
        shutil.copy2(os.path.join(lib, "SDL2.dll"), paths.install)
    else:
        os.makedirs(os.path.join(paths.install, "lib"), exist_ok=True)
        shutil.copy2(os.path.join(lib, "libSDL2-2.0.so.0"), os.path.join(paths.install, "lib"))
    for icon in (os.path.join(ROOT, "assets", "icon.png"), os.path.join(ROOT, "lib", "icon.png")):
        if os.path.exists(icon):
            shutil.copy2(icon, os.path.join(paths.install, "icon.png"))
            break
    for d in ("gamedata", "dev_flash"):
        os.makedirs(os.path.join(paths.data, d), exist_ok=True)


# ---------------- the whole thing ----------------

def run(install, source=None, keys=None, elf=None, progress=None, log=None, skip=None, full=False):
    """Set up (source given) or update (source None: the install's own copy of the
    disc) the game in `install`. keys / elf: how EBOOT.BIN is decrypted (an update
    reuses the decrypted program). Returns the game's path."""
    from version import VERSION
    paths = Paths(install)
    progress = progress or (lambda s, f, m: None)
    log = log or (lambda m: None)
    skip = skip or (lambda s: None)
    info = install_info(install) or {}
    stamps = {} if full or source else dict(info.get("stamps", {}))
    os.makedirs(paths.data, exist_ok=True)
    if full and os.path.isdir(paths.cache):
        shutil.rmtree(paths.cache)

    def save(**kw):
        info.update(kw, version=VERSION, stamps=stamps)
        tmp = os.path.join(paths.data, "install.json.part")
        with open(tmp, "w") as f:
            json.dump(info, f, indent=1)
        os.replace(tmp, os.path.join(paths.data, "install.json"))

    if source:
        progress("verify", 0.0, STEP_NAMES["verify"])
        kind = verify(source, lambda f: progress("verify", f, STEP_NAMES["verify"]))
        progress("extract", 0.0, STEP_NAMES["extract"])
        extract(source, kind, paths, lambda f: progress("extract", f, STEP_NAMES["extract"]), log)
        progress("decrypt", 0.0, STEP_NAMES["decrypt"])
        if not (keys or elf):
            raise SetupError("Choose your keys file, or an EBOOT.elf you decrypted.")
        decrypt(paths, keys, elf)
        save(source=os.path.abspath(source), stamps={})
    else:
        if not os.path.exists(paths.elf):
            raise SetupError("This install has no copy of the disc: choose your disc image to set it up again.")
        for s in ("verify", "extract", "decrypt"):
            skip(s)

    elf_sha = info.get("elf_sha256") or sha256_file(paths.elf)
    for step, fn in (("recomp", recomp), ("spu", spu)):
        fp = tool_fingerprint(step, elf_sha)
        marker = os.path.join(paths.gen, "recompiled" if step == "recomp" else "spu")
        if stamps.get(step) == fp and os.path.isdir(marker):
            skip(step)
            continue
        progress(step, 0.0, STEP_NAMES[step])
        stamps.pop(step, None)
        fn(paths, log)
        stamps[step] = fp
        save(elf_sha256=elf_sha)

    progress("build", 0.0, STEP_NAMES["build"])
    zig_build.build_game(paths.gen, paths.exe, paths.cache,
                         progress=lambda f, m: progress("build", f, m), log=log)
    progress("finish", 0.0, STEP_NAMES["finish"])
    finish(paths, log)
    save(elf_sha256=elf_sha)
    progress("finish", 1.0, "Done")
    return paths.exe

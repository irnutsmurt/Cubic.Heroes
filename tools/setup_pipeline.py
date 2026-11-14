#!/usr/bin/env python3
"""The setup pipeline: the player's own disc -> the native game, on their machine.

Shared by the setup app (setup_app.py) and its command line. Steps:
    1. verify   the disc is BLUS-30490 v01.00 (the ISO's SHA-256, or a disc
                folder's EBOOT.BIN)
    2. extract  its files into <install>/data/disc
    3. decrypt  EBOOT.BIN -> EBOOT.elf, with the player's keys file (or an
                EBOOT.elf they decrypted themselves); checked by SHA-256
    4. recomp   the PPU code lifted to C++ (ps3recomp's loader and lifter, with
                this port's function fixes, setjmp map and patch switches, and
                the native release's hooks: LIFT_CONFIG)
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
    """A failure the player can act on. code: what kind (docs/M9_WORKER_PROTOCOL.md section 4), for the
    launcher to choose its advice by; details: the technical part, shown on request."""

    def __init__(self, message, code="failed", details=None):
        super().__init__(message)
        self.code = code
        self.details = details


class Cancelled(BaseException):
    """Raised from a progress or log callback when the player cancelled: a BaseException, so no step's own
    `except Exception` (spu_native's) takes it for a failure of that step."""


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
        <install>/lib/              Linux: SDL2, OpenAL Soft and FFmpeg (Windows: their DLLs beside the game)
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


def run_tool(script, args, log, code="lift_failed"):
    """Run one of the lifting tools as its own process: `python script args` from a
    checkout; in the packaged app, the app itself runs it (setup_app.py main)."""
    cmd = [sys.executable, script, *args]
    kw = {"creationflags": 0x08000000} if os.name == "nt" else {}
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, **kw)
    tail = []
    try:
        for line in p.stdout:
            line = line.rstrip()
            tail = (tail + [line])[-40:]
            if log:
                log(line)   # the worker's log may raise its cancellation here
    except BaseException:
        p.kill()
        p.wait()
        raise
    if p.wait():
        raise SetupError(f"Recompiling the game failed in {os.path.basename(script)}.", code,
                         "\n".join(tail[-12:]))


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
    if not os.path.exists(source):
        raise SetupError("The disc image wasn't found: it may have been moved, or its drive is not connected.",
                         "not_found")
    if os.path.isdir(source):
        bin_ = os.path.join(source, "PS3_GAME", "USRDIR", "EBOOT.BIN")
        if not os.path.exists(bin_):
            raise SetupError("That folder has no PS3_GAME/USRDIR/EBOOT.BIN: choose the disc's top folder.",
                             "not_a_disc")
        if sha256_file(bin_) != want["files"]["PS3_GAME/USRDIR/EBOOT.BIN"]:
            raise SetupError("This isn't 3D Dot Game Heroes BLUS-30490 v01.00 (US): EBOOT.BIN doesn't match.",
                             "unsupported_version")
        return "folder"
    if os.path.getsize(source) != want["disc_image"]["size"]:
        raise SetupError("This disc image isn't the supported one: 3D Dot Game Heroes BLUS-30490 v01.00 (US), "
                         "a decrypted ISO (the size doesn't match).", "unsupported_version")
    if sha256_file(source, progress) != want["disc_image"]["sha256"]:
        raise SetupError("This disc image doesn't match BLUS-30490 v01.00 (US) (SHA-256). "
                         "Is it a decrypted dump of the US disc?", "damaged_disc")
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
        raise SetupError("The extracted EBOOT.BIN doesn't match: the disc image may be damaged.", "damaged_disc")
    log(f"extracted {len(ents)} files")


def decrypt_eboot(eboot_bytes, keys=None, elf=None):
    """EBOOT.BIN's bytes -> the decrypted program's, checked against the supported version's SHA-256.
    keys: the player's keys file; elf: an EBOOT.elf they decrypted themselves. The keys are read here and
    nowhere else, and never written, logged or put in an error's text."""
    want = version_info()["decrypted_elf"]["sha256"]
    if elf:
        try:
            data = open(elf, "rb").read()
        except OSError as e:
            raise SetupError(f"Can't read that EBOOT.elf ({e.strerror}).", "keys_unreadable")
    else:
        import self_decrypt
        try:
            k = self_decrypt.load_keys(keys)
        except (OSError, UnicodeDecodeError) as e:
            raise SetupError("Can't read the keys file" + (f" ({e.strerror})." if isinstance(e, OSError) else
                                                          ": it isn't a text file."), "keys_unreadable")
        except ValueError:
            raise SetupError("The keys file has a line that isn't a key (APP <revision> <erk> <riv>, in hex).",
                             "keys_unreadable")
        if not k:
            raise SetupError("The keys file has no APP keys (lines: APP <revision> <erk> <riv>).", "keys_empty")
        try:
            data = self_decrypt.decrypt_self(eboot_bytes, k)
        except Exception as e:  # noqa: BLE001 -- the reason only: never the keys
            raise SetupError("Decrypting the game program failed: the keys file needs the APP key revision "
                             "0x0001.", "wrong_key", type(e).__name__)
    if hashlib.sha256(data).hexdigest() != want:
        raise SetupError("The keys don't match this game: the decrypted program isn't the expected one.",
                         "wrong_key")
    return data


# ---------------- probes: what the player chose, checked before anything is written ----------------

def parse_sfo(b):
    """PARAM.SFO's entries (TITLE_ID, APP_VER, TITLE, ...)."""
    if b[:4] != b"\0PSF":
        raise ValueError("not a PARAM.SFO")
    key_tab, data_tab, n = struct.unpack_from("<III", b, 8)
    out = {}
    for i in range(n):
        ko, fmt, ln, _, do = struct.unpack_from("<HHIII", b, 20 + i * 16)
        k = b[key_tab + ko:b.index(b"\0", key_tab + ko)].decode("ascii", "replace")
        v = b[data_tab + do:data_tab + do + ln]
        out[k] = struct.unpack("<I", v[:4])[0] if fmt == 0x0404 else v.rstrip(b"\0").decode("utf-8", "replace")
    return out


def _iso_files(f):
    """{path: extents} of an ISO9660 image, or None when it isn't one"""
    import iso9660
    try:
        vd, _ = iso9660.find_volume(f)
        if vd is None:
            return None
        return dict(iso9660.entries(f))
    except (OSError, struct.error, IndexError, ValueError):
        return None


def _iso_read(f, extents, limit=1 << 30):
    import iso9660
    out = bytearray()
    for lba, n in extents:
        f.seek(lba * iso9660.SECTOR)
        out += f.read(min(n, limit - len(out)))
    return bytes(out)


def _disc_file(source, rel, files=None, f=None):
    """one file of the disc (a folder or an ISO), or None"""
    if os.path.isdir(source):
        p = os.path.join(source, *rel.split("/"))
        return open(p, "rb").read() if os.path.isfile(p) else None
    return _iso_read(f, files[rel]) if rel in files else None


def probe_disc(source):
    """What the chosen disc is, without extracting it: PARAM.SFO's title and version, EBOOT.BIN's SHA-256
    (a few seconds; the whole image's SHA-256 is the install's first step). Returns a dict; raises
    SetupError (not_found, not_a_disc, unsupported_version, damaged_disc)."""
    want = version_info()
    if not source or not os.path.exists(source):
        raise SetupError("The disc image wasn't found.", "not_found")
    f = files = None
    try:
        if not os.path.isdir(source):
            f = open(source, "rb")
            files = _iso_files(f)
            if files is None:
                raise SetupError("This file isn't a disc image (no ISO 9660 file system). Choose a decrypted "
                                 "ISO of your disc.", "not_a_disc")
        sfo_b = _disc_file(source, "PS3_GAME/PARAM.SFO", files, f)
        if sfo_b is None:
            raise SetupError("This isn't a PS3 game disc (no PS3_GAME/PARAM.SFO). Choose a decrypted ISO of "
                             "your disc, or its top folder.", "not_a_disc")
        try:
            sfo = parse_sfo(sfo_b)
        except (ValueError, struct.error, IndexError):
            raise SetupError("The disc's PARAM.SFO is unreadable: the image may be encrypted or damaged.",
                             "damaged_disc")
        info = {"kind": "folder" if files is None else "iso", "title_id": sfo.get("TITLE_ID", ""),
                "app_ver": sfo.get("APP_VER", ""), "title": sfo.get("TITLE", ""),
                "size": None if files is None else os.path.getsize(source)}
        supported = f"{want['title']} {want['title_id'][:4]}-{want['title_id'][4:]} v{want['app_ver']} (US)"
        if info["title_id"] != want["title_id"]:
            raise SetupError(f"This is {info['title'] or 'another game'} ({info['title_id'] or 'no title ID'}). "
                             f"Supported: {supported}.", "unsupported_version", json.dumps(info))
        if info["app_ver"] != want["app_ver"]:
            raise SetupError(f"This is version {info['app_ver']} of the game; supported: {supported}.",
                             "unsupported_version", json.dumps(info))
        eboot = _disc_file(source, "PS3_GAME/USRDIR/EBOOT.BIN", files, f)
        if eboot is None or hashlib.sha256(eboot).hexdigest() != want["files"]["PS3_GAME/USRDIR/EBOOT.BIN"]:
            raise SetupError("The disc's game program (EBOOT.BIN) isn't the expected one: the image may be "
                             "encrypted, damaged or modified.", "damaged_disc", json.dumps(info))
        if files is not None and info["size"] != want["disc_image"]["size"]:
            raise SetupError("The disc image's size isn't the supported dump's: it may be incomplete or "
                             "modified.", "damaged_disc", json.dumps(info))
        return info
    finally:
        if f:
            f.close()


def probe_keys(source, keys=None, elf=None):
    """Whether the keys file (or EBOOT.elf) decrypts this disc's program to the supported one, in memory:
    nothing is written. Raises SetupError (keys_missing, keys_unreadable, keys_empty, wrong_key)."""
    if not (keys or elf):
        raise SetupError("Choose your keys file, or an EBOOT.elf you decrypted.", "keys_missing")
    if not os.path.isfile(keys or elf):
        raise SetupError("The keys file wasn't found.", "keys_unreadable")
    if os.path.isdir(source):
        eboot = _disc_file(source, "PS3_GAME/USRDIR/EBOOT.BIN")
    else:
        with open(source, "rb") as f:
            eboot = _disc_file(source, "PS3_GAME/USRDIR/EBOOT.BIN", _iso_files(f) or {}, f)
    if eboot is None:
        raise SetupError("The disc's game program (EBOOT.BIN) wasn't found.", "not_a_disc")
    decrypt_eboot(eboot, keys, elf)
    return {"match": True, "via": "elf" if elf else "keys"}


def is_elf(path):
    """an EBOOT.elf the player decrypted, chosen in place of a keys file"""
    try:
        with open(path, "rb") as f:
            return f.read(4) == b"\x7fELF"
    except OSError:
        return False


def decrypt(paths, keys=None, elf=None):
    data = decrypt_eboot(open(paths.eboot_bin, "rb").read(), keys, elf)
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
    run_tool(os.path.join(t, "ppu_lifter.py"),
             [paths.elf, "--functions", fixed, "--hle-stubs", os.path.join(out, "EBOOT.imports.json"),
              *lift_options(), "-o", rec], log)


# The release lift, as tools/r8_lift.sh makes it (docs/NATIVE_R8.md): the
# function fixes' setjmp map and patch switches, R8's physics hooks beside them,
# R7's entry hooks into the native renderer (without them the native release,
# which has no RSX command path, draws nothing) and R8's leaf promotion.
# tests/test_player_pipeline.py checks these against tools/r8_lift.sh.
LIFT_CONFIG = [("--sjlj", "sjlj.json"), ("--insn-patches", "insn_patches.json"), ("--insn-patches", "r8_hooks.json"),
               ("--insn-patches", "audio_hooks.json"),
               ("--insn-patches", "m8_native_movie.json"),   # the movies on FFmpeg (M8): the movie class's Bink calls
               ("--entry-observers", "r7_native_hooks.json"),
               ("--traps", "excluded_code.json")]   # FMOD's (M5) and Bink's (M8.4) code is not lifted: traps in its place
LIFT_FLAGS = ["--chunks", PPU_CHUNKS, "--promote-leaf"]


def excluded_spu_images():
    """the SPU images config/excluded_code.json names: code the game never runs here (FMOD's), not lifted"""
    with open(os.path.join(config_dir(), "excluded_code.json")) as f:
        return list(json.load(f).get("spu_images", []))


def lift_options():
    c = config_dir()
    return [a for opt, f in LIFT_CONFIG for a in (opt, os.path.join(c, f))] + LIFT_FLAGS


def spu(paths, log):
    t = ps3_tools()
    spu_dir = os.path.join(paths.gen, "spu")
    if os.path.isdir(spu_dir):
        shutil.rmtree(spu_dir)
    images = os.path.join(spu_dir, "images")
    run_tool(os.path.join(t, "extract_spu_images.py"), [paths.elf, "--output", images], log)
    excluded = excluded_spu_images()   # FMOD's SPU programs (M5): not lifted
    reloc = []
    with open(os.path.join(config_dir(), "spu_relocatable.json")) as f:
        for img in json.load(f)["images"]:
            if img not in excluded:
                reloc += ["--relocatable", f"{img}=0x{spu_text_ea(paths.elf, os.path.join(images, img + '.elf')):08X}"]
    run_tool(os.path.join(t, "build_spu_workloads.py"),
             ["--images", images, "--lifted", spu_dir, "--out", os.path.join(spu_dir, "spu_workloads.c"),
              "--constructor", "--relift", "--entry-hooks", os.path.join(config_dir(), "spu_entry_hooks.json"),
              *reloc, *[a for img in excluded for a in ("--exclude", img)]], log)   # entry hooks: native code for hot SPU routines (the map packs' inflate)
    spu_native(paths, log)


# The SPU tasks that run as native code generated from the player's own lift (port/native/spu_native.h):
# (configuration in tools/spu_native/, the image as the extraction names it).
SPU_NATIVE = [("water", "spu_0000_at_00F47380"), ("waver", "spu_0001_at_00F48F80")]
# FFmpeg beside the game (the movies; LGPL): the libraries, the notice and the licence, as tools/build_ffmpeg.py
# (LIB_FILES, NOTICE, LICENCE) names them -- the setup carries no copy of that developer tool
FFMPEG_FILES = (["avutil-61.dll", "avcodec-63.dll", "avformat-63.dll"] if os.name == "nt" else
                ["libavutil.so.61", "libavcodec.so.63", "libavformat.so.63"]) + ["FFmpeg-NOTICE.txt",
                                                                                 "FFmpeg-LGPL-2.1.txt"]


def spu_native(paths, log):
    """gen/spu_native/<task>_gen.cpp from gen/spu: tools/gen_spu_native.py on each task's lift and image. An
    image that is not the one its configuration describes (another executable) is skipped with a note: the
    build then compiles the stub and the lifted tasks run."""
    t = os.path.join(ROOT, "tools")
    out = os.path.join(paths.gen, zig_build.SPU_NATIVE_DIR)
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)
    made = []
    for name, image in SPU_NATIVE:
        spu_dir = os.path.join(paths.gen, "spu")
        args = [os.path.join(t, "spu_native", name + ".py"), os.path.join(spu_dir, image, "spu_recomp.c"),
                os.path.join(spu_dir, "images", image + ".elf"), os.path.join(t, "spu_native_task.cpp.in"),
                os.path.join(out, name + "_gen.cpp")]
        try:
            run_tool(os.path.join(t, "gen_spu_native.py"), args, log)
            made.append(name)
        except Exception as e:   # noqa: BLE001 -- any failure: that task stays lifted
            if log:
                log(f"spu_native: {name} not generated ({e}); its lifted task runs")
    if len(made) != len(SPU_NATIVE):   # all or none: the build compiles both or the stub
        shutil.rmtree(out)


def finish(paths, log):
    lib = zig_build.Sources().lib_dir
    # the shared libraries beside the game: SDL2, OpenAL Soft (the game's audio) with its notice, FFmpeg (the
    # movies) with its notice and licence
    dlls = ["SDL2.dll", "OpenAL32.dll"] if os.name == "nt" else ["libSDL2-2.0.so.0", "libopenal.so.1"]
    dest = paths.install if os.name == "nt" else os.path.join(paths.install, "lib")
    os.makedirs(dest, exist_ok=True)
    for name in dlls + ["OpenAL-Soft-NOTICE.txt"] + FFMPEG_FILES:
        if os.path.exists(os.path.join(lib, name)) or name.startswith(("SDL2", "libSDL2")):
            shutil.copy2(os.path.join(lib, name), dest)
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
            raise SetupError("Choose your keys file, or an EBOOT.elf you decrypted.", "keys_missing")
        decrypt(paths, keys, elf)
        save(source=os.path.abspath(source), stamps={})
    else:
        if not os.path.exists(paths.elf):
            raise SetupError("This install has no copy of the disc: choose your disc image to set it up again.",
                             "no_disc_copy")
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

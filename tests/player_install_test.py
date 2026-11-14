#!/usr/bin/env python3
"""A player's fresh install and its updates, through the launcher's pipeline worker (tools/worker.py: the
operations op_install, op_update, op_migrate on an install with build slots, tools/install_layout.py), into a
test folder:
    build/venv/Scripts/python.exe tests/player_install_test.py [stages...] [--run-game]
Stages, in order (default: all but game):
    fresh      a new install from the disc image and build/EBOOT.elf: the disc's steps, the lift, the build into
               a slot, checked (`dotheroes --info`, its memory test), made active; game/ points at it
    check      what the install holds: the slot's game and libraries, the lift, the settings, `--info`
    update     an update with nothing changed: nothing rebuilt, the active build kept (the same inputs)
    relink     an update whose link changed (the active build's record of its inputs made stale): a new slot,
               every object reused, the build before it kept for rollback; the saves backed up first and
               untouched
    rollback   back to the kept build (offline, nothing rebuilt), its game the one game/ starts, then forward again
    failed     an update whose build fails, and one whose new build fails its check: the active build as it was
    relift     the lift's completion marker removed: the lift redone, the same lift, nothing rebuilt
    resume     an install in a second folder cancelled in its build, then taken up again (--resume): the disc's
               steps and the lift not redone, the objects compiled before the cancel reused
    migrate    a copy of a setup app's install (MIGRATE_FROM) moved into this layout: the disc not needed again,
               the saves, settings and translated shaders kept
    settings   a player's own values kept by an update's settings pass, missing performance switches added back
    game       (only with --run-game: it starts the game) the game a shortcut starts (game/) runs its memory test,
               then a river fight as a player starts the install (tests/boot_smoke.py INSTALLED=)
    prepare    (with --run-game, after game) the recipes the run recorded; the launcher's check; a driver change
               (the cache's recorded version changed): not prepared; the worker's prepare: every recipe made; the
               check again: prepared
Needs the prebuilt parts in their release configuration (build/lib/windows/game_build.json "native-release";
tools/zig_build.py build_prebuilt). The folder is INSTALL_TEST_DIR (default F:/3d dot game heroes/install-test: an
install is ~4 GB); `fresh` replaces it. Writes <folder>/test-report.json. docs/M9_LAUNCHER_PLAN.md section 10."""
import glob
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, ROOT)
import game_settings  # noqa: E402
import install_layout as layout  # noqa: E402
import setup_pipeline as pipeline  # noqa: E402
import worker  # noqa: E402
import zig_build  # noqa: E402

DIR = os.path.abspath(os.environ.get("INSTALL_TEST_DIR", "F:/3d dot game heroes/install-test"))
DIR2 = DIR + "-resume"
MIGRATE_FROM = os.environ.get("MIGRATE_FROM", "F:/3d dot game heroes/m9/install-w")   # a setup app's install
MIGRATED = DIR + "-migrated"
ISO = os.path.join(ROOT, "3D DOT GAME HEROES [BLUS-30490].iso")
ELF = os.path.join(ROOT, "build", "EBOOT.elf")
# the developers' release lift to compare with (tools/r8_lift.sh)
DEV_LIFT = os.environ.get("DEV_LIFT", os.path.join(ROOT, "build", "r8", "gen15"))
PY = sys.executable
EXE = layout.exe_name()
report = {"dir": DIR, "stages": {}}
failed = []


def check(stage, ok, what):
    report["stages"].setdefault(stage, {}).setdefault("checks", []).append({"ok": bool(ok), "what": what})
    print(f"  {'ok  ' if ok else 'FAIL'} {what}", flush=True)
    if not ok:
        failed.append(f"{stage}: {what}")


def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for c in iter(lambda: f.read(1 << 20), b""):
            h.update(c)
    return h.hexdigest()


class Run:
    """an operation in-process, its log lines and skipped steps kept"""

    def __init__(self, stage):
        self.stage, self.lines, self.skipped = stage, [], []
        self.cb = worker.Callbacks(log=self.lines.append, skip=self.skipped.append)
        self.t = time.monotonic()

    def done(self, out=None):
        secs = time.monotonic() - self.t
        st = report["stages"].setdefault(self.stage, {})
        st.update(seconds=round(secs), skipped=sorted(set(self.skipped)))
        if out is not None:
            st["result"] = {k: v for k, v in out.items() if k != "pruned"}
        print(f"  {self.stage}: {secs:.0f} s, skipped {sorted(set(self.skipped))}", flush=True)
        return out


def reuse_counts(lines):
    for ln in lines:
        m = re.match(r"(\d+) of (\d+) files unchanged", ln)
        if m:
            return int(m.group(1)), int(m.group(2))
    return 0, None


def lay():
    return layout.Layout(DIR)


def active_exe():
    return os.path.join(DIR, "game", EXE)


def game_info(exe):
    env = {k: v for k, v in os.environ.items() if not k.startswith(("DOTHEROES_", "PS3_", "PS3RECOMP_"))}
    r = subprocess.run([exe, "--info"], capture_output=True, text=True, timeout=120, env=env)
    try:
        return json.loads(r.stdout.strip().splitlines()[-1])
    except (ValueError, IndexError):
        return {"exit": r.returncode, "out": r.stdout[-500:]}


def saves_hash(d):
    h = hashlib.sha256()
    for root, _, fs in sorted(os.walk(d)):
        for f in sorted(fs):
            p = os.path.join(root, f)
            h.update(os.path.relpath(p, d).encode() + b"\n")
            with open(p, "rb") as fh:
                h.update(fh.read())
    return h.hexdigest()


def stage_fresh():
    lib = zig_build.Sources().lib_dir
    gb = zig_build.game_defines(lib)
    check("fresh", gb and any(d.startswith("-DDOTHEROES_NATIVE_RELEASE") for d in gb),
          f"the prebuilt parts are the native release ({os.path.join(lib, zig_build.GAME_BUILD)})")
    if os.name == "nt":   # M6's release on Windows (zig_build.release_cmake): what rel48 and later were tested as
        check("fresh", gb and "-DPPU_STORES_NO_SPU_COH=1" in gb and "-DDOTHEROES_NO_LIFTED_SPU=1" in gb,
              "the prebuilt parts are M6's release: the stores without the SPU reservation test, no lifted SPU program")
    if failed:
        return
    if os.path.isdir(DIR):
        layout.rmtree(DIR)
    os.makedirs(DIR)
    r = Run("fresh")
    out = r.done(worker.op_install(DIR, ISO, None, ELF, r.cb))
    n, total = reuse_counts(r.lines)
    check("fresh", n == 0, f"nothing reused in a fresh install's build ({n})")
    check("fresh", lay().kind() == "slots" and lay().read_active()["active"] == out["build"],
          f"the new build active ({out['build']})")
    check("fresh", layout.is_link(lay().game_link) and os.path.exists(active_exe()), "game/ starts it")
    report["stages"]["fresh"]["exe_sha256"] = sha(active_exe())


def stage_check():
    exe = active_exe()
    check("check", os.path.exists(exe), "the game is there (game/)")
    if os.path.exists(exe):   # M8.4: the runtime's cellAudio built without host output (its retired Init's line)
        with open(exe, "rb") as f:
            check("check", b"retired in this build" in f.read(), "cellAudio retired in the built game")
    slot = lay().build_dir(lay().read_active()["active"])
    libdir = slot if os.name == "nt" else os.path.join(slot, "lib")
    check("check", os.path.exists(os.path.join(libdir, "SDL2.dll")) or os.name != "nt", "SDL2.dll beside it")
    al = "OpenAL32.dll" if os.name == "nt" else "libopenal.so.1"
    check("check", os.path.exists(os.path.join(libdir, al)), f"OpenAL Soft ({al}) beside it")
    check("check", os.path.exists(os.path.join(libdir, "OpenAL-Soft-NOTICE.txt")), "OpenAL Soft's notice beside it")
    for name in pipeline.FFMPEG_FILES:
        check("check", os.path.exists(os.path.join(libdir, name)), f"FFmpeg's {name} beside it")
    for name in ("build.json", "build.cfg"):
        check("check", os.path.exists(os.path.join(slot, name)), f"the slot's {name}")
    info = game_info(exe)
    report["stages"]["check"]["info"] = info
    check("check", info.get("layout") == "slots" and info.get("disc") and info.get("ffmpeg", {}).get("ok"),
          f"--info from game/: {info.get('layout')}, the disc found, {info.get('ffmpeg', {}).get('what')}")
    binfo = lay().build_info(lay().read_active()["active"]) or {}
    check("check", binfo.get("checked", {}).get("memtest", "").find("0 failed") >= 0,
          f"the memory test passed before it became active ({binfo.get('checked', {}).get('memtest')})")
    cfg = game_settings.read_game_settings(DIR)
    check("check", os.path.exists(os.path.join(DIR, "config", "dotheroes.cfg")), "the settings in config/")
    for k, v in game_settings.PERFORMANCE.items():
        check("check", cfg.get(k) == v, f"dotheroes.cfg {k}={v}")
    info = pipeline.install_info(DIR) or {}
    rec = os.path.join(DIR, "data", "gen", info.get("lift", "?"), "recompiled")
    check("check", os.path.isdir(rec), f"the lift in data/gen/{info.get('lift')}")
    text = "".join(open(os.path.join(rec, f), encoding="utf-8", errors="replace").read()
                   for f in sorted(os.listdir(rec)) if f.endswith(".cpp")) if os.path.isdir(rec) else ""
    leaf, obs = text.count("/* --promote-leaf */"), text.count("entry observer")
    report["stages"]["check"]["lift"] = {"promote_leaf": leaf, "entry_observers": obs,
                                         "phys_hooks": text.count("g_phys_direct")}
    check("check", leaf > 7000, f"leaf promotion in the lift ({leaf} functions; M5's lift: 7,428)")
    check("check", obs > 0, f"R7's entry hooks in the lift ({obs})")
    check("check", text.count("g_phys_direct") >= 2, "R8's physics hooks in the lift")
    if os.path.isdir(DEV_LIFT) and os.path.isdir(rec):   # the same lifter, options and program: the same code
        diff = [f for f in sorted(os.listdir(DEV_LIFT)) if f.endswith((".cpp", ".h"))
                and (not os.path.exists(os.path.join(rec, f)) or sha(os.path.join(rec, f)) != sha(os.path.join(DEV_LIFT, f)))]
        report["stages"]["check"]["lift_differs_from_dev"] = diff
        check("check", not diff, f"the lift equals the developers' ({os.path.relpath(DEV_LIFT, ROOT)}): "
                                 f"{len(diff)} files differ")


def put_save():
    """a save file of the test's own in the install's save root (the hash is what is compared)"""
    p = os.path.join(DIR, "data", "gamedata", "dev_hdd0", "home", "00000001", "savedata", "TEST", "DATA.BIN")
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write(os.urandom(4096))


def stage_update():
    before = lay().read_active()
    r = Run("update")
    out = r.done(worker.op_update(DIR, r.cb))
    check("update", not out["changed"] and lay().read_active()["active"] == before["active"],
          "nothing changed: the active build kept, nothing built")
    check("update", {"recomp", "spu", "build"} <= set(r.skipped), "the lift and the build skipped")


def stage_relink():
    put_save()
    saves = saves_hash(lay().gamedata)
    a = lay().read_active()["active"]
    p = os.path.join(lay().build_dir(a), "build.json")
    info = layout.read_json(p)
    info["inputs"] = "made stale by the test"
    layout.write_json(p, info)
    r = Run("relink")
    out = r.done(worker.op_update(DIR, r.cb))
    n, total = reuse_counts(r.lines)
    check("relink", total is not None and n == total, f"every object reused ({n} of {total})")
    st = lay().read_active()
    check("relink", out["changed"] and st["active"] != a and st["previous"] == [a],
          f"a new slot active ({st['active']}), the one before kept ({a})")
    check("relink", out.get("saves_backup") and os.path.isdir(os.path.join(lay().backups_dir, out["saves_backup"])),
          f"the saves backed up first ({out.get('saves_backup')})")
    check("relink", saves_hash(lay().gamedata) == saves, "the saves untouched")
    report["stages"]["relink"]["slot_mb"] = round(layout.tree_size(lay().build_dir(st["active"])) / 2**20, 1)


def stage_rollback():
    st = lay().read_active()
    if not st["previous"]:
        return check("rollback", False, "a kept build to go back to (run relink first)")
    cur, prev = st["active"], st["previous"][0]
    saves = saves_hash(lay().gamedata)
    t = time.monotonic()
    lay().rollback()
    secs = time.monotonic() - t
    check("rollback", lay().read_active()["active"] == prev and game_info(active_exe()).get("build_id") == prev,
          f"back to {prev} in {secs:.2f} s: game/ starts it (--info says so)")
    check("rollback", sha(active_exe()) == (lay().build_info(prev) or {}).get("exe_sha256"), "its game as recorded")
    check("rollback", saves_hash(lay().gamedata) == saves, "the saves untouched")
    lay().rollback()
    check("rollback", lay().read_active()["active"] == cur, f"forward again to {cur}")


def stage_failed():
    st = lay().read_active()
    real = pipeline.zig_build.build_game

    def broken(*a, **kw):
        raise pipeline.zig_build.BuildError("compiling failed: made to fail by the test")
    pipeline.zig_build.build_game = broken
    p = os.path.join(lay().build_dir(st["active"]), "build.json")
    info = layout.read_json(p)
    keep_inputs = info.get("inputs")
    info["inputs"] = "made stale by the test"
    layout.write_json(p, info)
    try:
        r = Run("failed")
        try:
            worker.op_update(DIR, r.cb)
            check("failed", False, "the broken build refused")
        except pipeline.zig_build.BuildError:
            check("failed", True, "the broken build refused (compile_failed)")
        r.done()
    finally:
        pipeline.zig_build.build_game = real
    check("failed", lay().read_active() == st and not [n for n in os.listdir(lay().builds_dir) if n.startswith(".")],
          "the active build as it was, no staged slot left")

    def bad(slot, log):
        raise pipeline.SetupError("The new build failed its self-test.", "build_invalid")
    try:
        worker.op_update(DIR, Run("failed").cb, validate=bad)
        check("failed", False, "the build failing its check refused")
    except pipeline.SetupError as e:
        check("failed", e.code == "build_invalid", "the build failing its check refused (build_invalid)")
    check("failed", lay().read_active() == st, "the active build as it was")
    info["inputs"] = keep_inputs
    layout.write_json(p, info)
    check("failed", game_info(active_exe()).get("ffmpeg", {}).get("ok"), "the active game still starts (--info)")


def stage_relift():
    info = pipeline.install_info(DIR) or {}
    done = os.path.join(DIR, "data", "gen", info.get("lift", "?"), "done.json")
    if os.path.exists(done):
        os.remove(done)
    r = Run("relift")
    out = r.done(worker.op_update(DIR, r.cb))
    check("relift", "recomp" not in r.skipped and "spu" not in r.skipped, "the lift redone")
    check("relift", not out["changed"], "the same lift: the same build, nothing rebuilt")


def stage_resume():
    if os.path.isdir(DIR2):
        layout.rmtree(DIR2)
    os.makedirs(DIR2)
    cmd = [PY, os.path.join(ROOT, "tools", "worker.py"), "install", "--disc", ISO, "--keys", ELF, "--install", DIR2,
           "--control"]
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, encoding="utf-8")
    compiled, res = 0, {}
    for ln in p.stdout:
        e = json.loads(ln)
        if e["ev"] == "log" and e["text"].startswith("compiled "):
            compiled += 1
            if compiled == 10:   # ten objects in: cancel
                p.stdin.write("cancel\n")
                p.stdin.flush()
        if e["ev"] == "result":
            res = e
    p.wait(timeout=60)
    check("resume", res.get("code") == "cancelled", f"the install cancelled in its build ({res.get('code')}, "
                                                    f"forced: {(res.get('data') or {}).get('forced', False)})")
    l2 = layout.Layout(DIR2)
    check("resume", not os.path.exists(l2.active_json), "no build active after the cancel")
    info = worker.cmd_info(type("A", (), {"install": DIR2})(), None, None)
    check("resume", info["kind"] == "partial" and set(info["interrupted"]["done"]) == {"verify", "extract", "decrypt"},
          f"info: an interrupted install, its disc's steps done ({info['interrupted']})")
    r = Run("resume")
    out = r.done(worker.op_install(DIR2, ISO, None, ELF, r.cb, resume=True))
    check("resume", {"verify", "extract", "decrypt", "recomp", "spu"} <= set(r.skipped),
          "the disc's steps and the lift not redone")
    n, total = reuse_counts(r.lines)
    check("resume", n >= 10, f"the objects compiled before the cancel reused ({n} of {total})")
    check("resume", l2.read_active()["active"] == out["build"], "the resumed install's build active")
    layout.rmtree(DIR2)


def stage_migrate():
    if not os.path.isdir(MIGRATE_FROM) or layout.Layout(MIGRATE_FROM).kind() != "setup-app":
        return check("migrate", False, f"a setup app's install to copy ({MIGRATE_FROM})")
    if os.path.isdir(MIGRATED):
        layout.rmtree(MIGRATED)
    t = time.monotonic()
    shutil.copytree(MIGRATE_FROM, MIGRATED)
    print(f"  copied {MIGRATE_FROM} in {time.monotonic() - t:.0f} s", flush=True)
    m = layout.Layout(MIGRATED)
    os.makedirs(os.path.join(m.gamedata, "savedata"), exist_ok=True)
    with open(os.path.join(m.gamedata, "savedata", "TEST.BIN"), "wb") as f:
        f.write(os.urandom(2048))
    saves = saves_hash(m.gamedata)
    with open(os.path.join(MIGRATED, "dotheroes.cfg"), "a") as f:
        f.write("DOTHEROES_FPS=30\n")
    os.makedirs(os.path.join(m.data, "cache", "shaders"), exist_ok=True)
    with open(os.path.join(m.data, "cache", "shaders", "test-translation.spv"), "wb") as f:
        f.write(b"x")
    r = Run("migrate")
    out = r.done(worker.op_migrate(MIGRATED, r.cb))
    check("migrate", m.kind() == "slots" and not os.path.exists(os.path.join(MIGRATED, EXE)),
          "the layout with build slots; the setup app's game removed")
    check("migrate", {"verify", "extract", "decrypt"} <= set(r.skipped), "the disc not needed (its steps skipped)")
    n, total = reuse_counts(r.lines)
    report["stages"]["migrate"]["objects_reused"] = [n, total]
    print(f"  lift kept: {out['lift_kept']}; objects reused {n} of {total}")
    check("migrate", saves_hash(m.gamedata) == saves, "the saves untouched")
    check("migrate", game_settings.read_game_settings(MIGRATED).get("DOTHEROES_FPS") == "30",
          "the player's settings kept (config/dotheroes.cfg)")
    sh = os.path.join(m.shaders, (m.build_info(m.read_active()["active"]) or {}).get("shader_cache", "?"),
                      "test-translation.spv")
    check("migrate", os.path.exists(sh), "the translated shaders kept, in the build's translator's folder")
    check("migrate", game_info(os.path.join(MIGRATED, "game", EXE)).get("disc"), "the migrated game finds its disc")
    layout.rmtree(MIGRATED)


def stage_settings():
    path = game_settings.game_settings_path(DIR)
    with open(path) as f:
        keep = f.read()
    try:
        game_settings.write_game_settings(DIR, DOTHEROES_FPS="0", DOTHEROES_NATIVE_CD="0", PS3RECOMP_VK_FULLSCREEN="1")
        cur = game_settings.read_game_settings(DIR)
        cur.pop("GCM_WRAP_LAP", None)
        with open(path, "w") as f:
            f.write("".join(f"{k}={v}\n" for k, v in cur.items()))
        game_settings.settle_after_conversion(DIR)
        cfg = game_settings.read_game_settings(DIR)
        check("settings", cfg.get("DOTHEROES_FPS") == "0" and cfg.get("DOTHEROES_NATIVE_CD") == "0"
              and cfg.get("PS3RECOMP_VK_FULLSCREEN") == "1", "the player's own values kept")
        check("settings", cfg.get("GCM_WRAP_LAP") == "1", "a missing performance switch added back")
        old = {k: v[0] for k, v in game_settings.PREVIOUS_DEFAULTS.items()}
        game_settings.write_game_settings(DIR, **old)
        game_settings.settle_after_conversion(DIR)
        cfg = game_settings.read_game_settings(DIR)
        check("settings", all(cfg.get(k) == game_settings.PERFORMANCE[k] for k in old),
              "an earlier default brought up to date (%s)" % ", ".join("%s=%s" % (k, cfg.get(k)) for k in old))
    finally:
        with open(path, "w") as f:
            f.write(keep)


def stage_game():
    exe = active_exe()
    env = dict(os.environ, DOTHEROES_LOG_TO_CONSOLE="1")
    r = subprocess.run([exe, "--memtest"], cwd=DIR, env=env, capture_output=True, text=True, timeout=300)
    tail = (r.stdout + r.stderr).strip().splitlines()[-3:]
    report["stages"]["game"] = {"memtest_exit": r.returncode, "memtest_tail": tail}
    check("game", r.returncode == 0, f"game/{EXE} --memtest: exit {r.returncode} ({' / '.join(tail)})")
    # The river fight from the install, long enough to reach it (the title, CONTINUE from the kept checkpoint's
    # saves, the route: the fight starts ~65 s in) and judged by the fight, not the exit. The save-data list is off
    # for this run (INSTALLED_KEEP): a player's install shows it at CONTINUE, and the river route is written for the
    # title's first entry; the list has its own drives (save_picker, save_picker_edit) in the validation set.
    env = dict(os.environ, INSTALLED=DIR, SCENARIO="river_fight", RUN_TAG="install-test", AUDIO="1",
               INSTALLED_KEEP="PS3_SAVEDATA_PICKER", PS3_SAVEDATA_PICKER="0", KEEP_RUNS="50")
    r = subprocess.run([PY, os.path.join(ROOT, "tests", "boot_smoke.py"), "220"], cwd=ROOT, env=env, timeout=1200)
    check("game", r.returncode == 0, f"the game from the install ran (tests/boot_smoke.py INSTALLED=): exit {r.returncode}")
    runs = sorted(glob.glob(os.path.join(ROOT, "build", "test", "boot-*-install-test")), key=os.path.getmtime)
    run = runs[-1] if runs else ""
    fights, native = [], 0
    try:
        for ln in open(os.path.join(run, "segments.jsonl"), encoding="utf-8"):
            s = json.loads(ln)
            if s.get("kind") == "fight":
                fights.append(s)
    except OSError:
        pass
    try:
        for ln in open(os.path.join(run, "stderr.log"), encoding="utf-8", errors="replace"):
            m = re.match(r"\[phys-direct\] (\d+) native tasks issued directly", ln)
            if m:
                native = max(native, int(m.group(1)))
    except OSError:
        pass
    # Judged by fighting, not by finishing: an install takes normal damage (no can't-die), so the hero may die
    # and the route start again; a fight of 30 s or more with native physics tasks issued is what is checked.
    done = [s for s in fights if s.get("ok")]
    fought = [s for s in fights if (s.get("wall1") or 0) - (s.get("wall0") or 0) >= 30]
    deaths = sum(1 for s in fights if "life" in str(s.get("why", "")))
    report["stages"]["game"]["fight"] = {"run": run, "segments": len(fights), "completed": len(done),
                                         "fought_30s": len(fought), "deaths": deaths, "native_tasks": native}
    check("game", bool(fought) and native > 0,
          f"a river fight from the install: {len(fought)} fight segment(s) of 30 s or more ({len(done)} completed, "
          f"{deaths} ended by the hero's death), native physics ({native} tasks issued directly)")


def stage_prepare():
    """after the game has been played from the install (its recipes recorded): the launcher's check, a driver
    change (the cache's recorded driver version changed, as the 2026-11-02 cache-tamper test), the check again, the
    preparation through the worker, the check once more"""
    rec = os.path.join(DIR, "data", "cache", "native-recipes.bin")
    check("prepare", os.path.exists(rec), f"the recipes recorded by the game's run ({rec})")
    cb = Run("prepare").cb
    first = worker.op_prepare(DIR, cb, check=True)
    report["stages"]["prepare"]["check_before"] = first
    check("prepare", first.get("recipes", 0) > 0 and first.get("prepared"),
          f"check: {first.get('recipes')} recipes, the cache {first.get('cache')}")
    sid = (lay().build_info(lay().read_active()["active"]) or {}).get("shader_cache")
    pc = os.path.join(DIR, "data", "cache", "shaders", sid or "?", "native-pipelines.vkpc")
    import struct
    with open(pc, "r+b") as f:
        f.seek(20)
        d = struct.unpack("<I", f.read(4))[0]
        f.seek(20)
        f.write(struct.pack("<I", d + 1))
    after = worker.op_prepare(DIR, cb, check=True)
    check("prepare", not after.get("prepared") and "driver" in str(after.get("cache")),
          f"after a driver change, check: not prepared ({after.get('cache')})")
    r = Run("prepare")
    out = r.done(worker.op_prepare(DIR, r.cb))
    check("prepare", out.get("made") == first.get("recipes") and out.get("failed") == 0,
          f"prepared: {out.get('made')} of {out.get('recipes')} pipelines made in {out.get('ms')} ms, "
          f"{out.get('translated')} translated by glslang")
    last = worker.op_prepare(DIR, cb, check=True)
    check("prepare", last.get("prepared"), f"check again: prepared ({last.get('cache')}, {last.get('cache_mb')} MB)")


STAGES = {"fresh": stage_fresh, "check": stage_check, "update": stage_update, "relink": stage_relink,
          "rollback": stage_rollback, "failed": stage_failed, "relift": stage_relift, "resume": stage_resume,
          "migrate": stage_migrate, "settings": stage_settings, "game": stage_game, "prepare": stage_prepare}


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    run_game = "--run-game" in sys.argv
    stages = args or [s for s in STAGES if s not in ("game", "prepare") or run_game]
    if "game" in stages and not run_game:
        sys.exit("the game stage starts the game: add --run-game")
    for s in stages:
        print(f"== {s}", flush=True)
        try:
            STAGES[s]()
        except Exception as e:  # noqa: BLE001 -- a stage that throws is a failed stage, the others still run
            import traceback
            traceback.print_exc()
            check(s, False, f"the stage raised {e!r}")
        if failed and s == "fresh":
            break
    report["failed"] = failed
    os.makedirs(DIR, exist_ok=True)
    with open(os.path.join(DIR, "test-report.json"), "w") as f:
        json.dump(report, f, indent=1)
    print(f"{'FAILED: ' + '; '.join(failed) if failed else 'all passed'} ({os.path.join(DIR, 'test-report.json')})")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

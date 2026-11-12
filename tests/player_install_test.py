#!/usr/bin/env python3
"""A player's fresh install and its updates, through the setup app's own path
(tools/setup_pipeline.py and setup_app.py's settings), into a test folder:
    build/venv/Scripts/python.exe tests/player_install_test.py [stages...] [--run-game]
Stages, in order (default: all but game):
    fresh     setup_app --convert's steps: the disc image, build/EBOOT.elf, a new folder
    check     what the install holds: the native release's lift and configuration
    update    an update with nothing changed: every step but the build skipped, every
              object reused, the game relinked
    relift    an update whose lift has to be redone (its stamp cleared): the same lift
              again (deterministic), so every object is still reused
    settings  a player's own dotheroes.cfg values kept by an update's settings pass,
              missing performance switches added back
    game      (only with --run-game: it starts the game) `dotheroes.exe --memtest`, then
              a river fight as a player starts the install (tests/boot_smoke.py INSTALLED=)
Needs the prebuilt parts in their release configuration (build/lib/windows/game_build.json
"native-release"; tools/zig_build.py build_prebuilt). The folder is INSTALL_TEST_DIR
(default F:/3d dot game heroes/install-test: an install is ~10 GB); it is replaced by `fresh`.
Writes <folder>/test-report.json. docs/NATIVE_R8.md section 7.2."""
import glob
import hashlib
import json
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, ROOT)
import setup_pipeline as pipeline  # noqa: E402
import zig_build  # noqa: E402

DIR = os.path.abspath(os.environ.get("INSTALL_TEST_DIR", "F:/3d dot game heroes/install-test"))
ISO = os.path.join(ROOT, "3D DOT GAME HEROES [BLUS-30490].iso")
ELF = os.path.join(ROOT, "build", "EBOOT.elf")
# the developers' release lift to compare with (tools/r8_lift.sh; rel7/rel8's)
DEV_LIFT = os.environ.get("DEV_LIFT", os.path.join(ROOT, "build", "r8", "gen6"))
PY = sys.executable
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


def run_pipeline(stage, source=None):
    """pipeline.run as the app runs it; returns (skipped steps, log lines, seconds)"""
    skipped, lines = [], []
    t = time.monotonic()
    pipeline.run(DIR, source=source, elf=ELF if source else None, log=lines.append, skip=skipped.append,
                 progress=lambda s, f, m: None)
    secs = time.monotonic() - t
    report["stages"].setdefault(stage, {})["seconds"] = round(secs)
    report["stages"][stage]["skipped"] = skipped
    print(f"  {stage}: {secs:.0f} s, skipped {skipped}", flush=True)
    return skipped, lines, secs


def settings_pass():
    """what the app does after a conversion or an update (setup_app.convert_headless)"""
    import setup_app
    if not os.path.exists(setup_app.game_settings_path(DIR)):
        setup_app.write_game_settings(DIR, DOTHEROES_FPS="60", PS3RECOMP_VK_FULLSCREEN="0")
    setup_app.ensure_performance_settings(DIR)
    return setup_app


def reuse_counts(lines):
    for ln in lines:
        m = re.match(r"(\d+) of (\d+) files unchanged", ln)
        if m:
            return int(m.group(1)), int(m.group(2))
    return 0, None


def stage_fresh():
    lib = zig_build.Sources().lib_dir
    gb = zig_build.game_defines(lib)
    check("fresh", gb and any(d.startswith("-DDOTHEROES_NATIVE_RELEASE") for d in gb),
          f"the prebuilt parts are the native release ({os.path.join(lib, zig_build.GAME_BUILD)})")
    if failed:
        return
    if os.path.isdir(DIR):
        import shutil
        shutil.rmtree(DIR)
    os.makedirs(DIR)
    _, lines, _ = run_pipeline("fresh", source=ISO)
    settings_pass()
    n, total = reuse_counts(lines)
    check("fresh", n == 0, f"nothing reused in a fresh install's build ({n})")
    report["stages"]["fresh"]["exe_sha256"] = sha(os.path.join(DIR, pipeline.exe_name()))


def stage_check():
    exe = os.path.join(DIR, pipeline.exe_name())
    check("check", os.path.exists(exe), "the game is there")
    check("check", os.path.exists(os.path.join(DIR, "SDL2.dll")) or os.name != "nt", "SDL2.dll beside it")
    libdir = DIR if os.name == "nt" else os.path.join(DIR, "lib")
    al = "OpenAL32.dll" if os.name == "nt" else "libopenal.so.1"
    check("check", os.path.exists(os.path.join(libdir, al)), f"OpenAL Soft ({al}) beside it")
    check("check", os.path.exists(os.path.join(libdir, "OpenAL-Soft-NOTICE.txt")), "OpenAL Soft's notice beside it")
    info = pipeline.install_info(DIR) or {}
    check("check", {"recomp", "spu"} <= set(info.get("stamps", {})), "install.json has the lift's stamps")
    import setup_app
    cfg = setup_app.read_game_settings(DIR)
    for k, v in setup_app.PERFORMANCE.items():
        check("check", cfg.get(k) == v, f"dotheroes.cfg {k}={v}")
    rec = os.path.join(DIR, "data", "gen", "recompiled")
    text = "".join(open(os.path.join(rec, f), encoding="utf-8", errors="replace").read()
                   for f in sorted(os.listdir(rec)) if f.endswith(".cpp"))
    leaf, obs = text.count("/* --promote-leaf */"), text.count("entry observer")
    report["stages"]["check"]["lift"] = {"promote_leaf": leaf, "entry_observers": obs,
                                         "phys_hooks": text.count("g_phys_direct")}
    check("check", leaf > 8000, f"leaf promotion in the lift ({leaf} functions; R8's lift: 8,395)")
    check("check", obs > 0, f"R7's entry hooks in the lift ({obs})")
    check("check", text.count("g_phys_direct") >= 2, "R8's physics hooks in the lift")
    if os.path.isdir(DEV_LIFT):   # the same lifter, options and program: the same code
        diff = [f for f in sorted(os.listdir(DEV_LIFT)) if f.endswith((".cpp", ".h"))
                and (not os.path.exists(os.path.join(rec, f)) or sha(os.path.join(rec, f)) != sha(os.path.join(DEV_LIFT, f)))]
        report["stages"]["check"]["lift_differs_from_dev"] = diff
        check("check", not diff, f"the lift equals the developers' ({os.path.relpath(DEV_LIFT, ROOT)}): "
                                 f"{len(diff)} files differ")


def stage_update():
    before = sha(os.path.join(DIR, pipeline.exe_name()))
    skipped, lines, secs = run_pipeline("update")
    check("update", set(skipped) >= {"verify", "extract", "decrypt", "recomp", "spu"}, "only the build ran")
    n, total = reuse_counts(lines)
    check("update", total is not None and n == total, f"every object reused ({n} of {total})")
    after = sha(os.path.join(DIR, pipeline.exe_name()))
    report["stages"]["update"]["exe_same"] = before == after
    print(f"  relinked game {'identical' if before == after else 'differs'} (information)")


def stage_relift():
    p = os.path.join(DIR, "data", "install.json")
    info = json.load(open(p))
    info["stamps"]["recomp"] = "cleared by the test"
    json.dump(info, open(p, "w"), indent=1)
    skipped, lines, _ = run_pipeline("relift")
    check("relift", "recomp" not in skipped and "spu" in skipped, "the lift redone, the SPU step skipped")
    n, total = reuse_counts(lines)
    check("relift", total is not None and n == total, f"the same lift: every object reused ({n} of {total})")


def stage_settings():
    import setup_app
    path = setup_app.game_settings_path(DIR)
    keep = open(path).read()
    try:
        setup_app.write_game_settings(DIR, DOTHEROES_FPS="0", DOTHEROES_NATIVE_CD="0", PS3RECOMP_VK_FULLSCREEN="1")
        cur = setup_app.read_game_settings(DIR)
        cur.pop("GCM_WRAP_LAP", None)
        with open(path, "w") as f:
            f.write("".join(f"{k}={v}\n" for k, v in cur.items()))
        settings_pass()
        cfg = setup_app.read_game_settings(DIR)
        check("settings", cfg.get("DOTHEROES_FPS") == "0" and cfg.get("DOTHEROES_NATIVE_CD") == "0"
              and cfg.get("PS3RECOMP_VK_FULLSCREEN") == "1", "the player's own values kept")
        check("settings", cfg.get("GCM_WRAP_LAP") == "1", "a missing performance switch added back")
        # an earlier default (setup_app.PREVIOUS_DEFAULTS) is not the player's: brought up to date
        old = {k: v[0] for k, v in setup_app.PREVIOUS_DEFAULTS.items()}
        setup_app.write_game_settings(DIR, **old)
        settings_pass()
        cfg = setup_app.read_game_settings(DIR)
        check("settings", all(cfg.get(k) == setup_app.PERFORMANCE[k] for k in old),
              "an earlier default brought up to date (%s)" % ", ".join("%s=%s" % (k, cfg.get(k)) for k in old))
    finally:
        with open(path, "w") as f:
            f.write(keep)


def stage_game():
    exe = os.path.join(DIR, pipeline.exe_name())
    env = dict(os.environ, DOTHEROES_LOG_TO_CONSOLE="1")
    r = subprocess.run([exe, "--memtest"], cwd=DIR, env=env, capture_output=True, text=True, timeout=300)
    tail = (r.stdout + r.stderr).strip().splitlines()[-3:]
    report["stages"]["game"] = {"memtest_exit": r.returncode, "memtest_tail": tail}
    check("game", r.returncode == 0, f"dotheroes.exe --memtest: exit {r.returncode} ({' / '.join(tail)})")
    # The river fight from the install, long enough to reach it (the title, CONTINUE from the kept checkpoint's
    # saves, the route: the fight starts ~65 s in) and judged by the fight, not the exit: before 2026-11-10 this
    # ran 60 s, the run ended at the title or the continue, and the stage passed on exit 0 alone.
    # The save-data list is off for this run (INSTALLED_KEEP): a player's install shows it at CONTINUE, and the
    # river route is written for the title's first entry; the list has its own drives (save_picker,
    # save_picker_edit) in the validation set.
    env = dict(os.environ, INSTALLED=DIR, SCENARIO="river_fight", RUN_TAG="install-test", AUDIO="1",
               INSTALLED_KEEP="PS3_SAVEDATA_PICKER", PS3_SAVEDATA_PICKER="0")
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


STAGES = {"fresh": stage_fresh, "check": stage_check, "update": stage_update, "relift": stage_relift,
          "settings": stage_settings, "game": stage_game}


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    run_game = "--run-game" in sys.argv
    stages = args or [s for s in STAGES if s != "game" or run_game]
    if "game" in stages and not run_game:
        sys.exit("the game stage starts the game: add --run-game")
    for s in stages:
        print(f"== {s}", flush=True)
        STAGES[s]()
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

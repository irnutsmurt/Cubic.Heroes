"""The launcher's footer (beta.6, the owner's Store check by mouse): the device label at the ground's left never runs into
the prompts at its right -- it ran into the Character Store's row ("Keyboard and mouse (a controller is connected)"
beside eight prompts) -- and the keyboard's keycaps are drawn, the Store's tab keys "[" and "]" among them (drawn "?").
Hidden (--nav-test), on a made-up v1.00 installation (tests/launcher_game_version.py's), the Character Store a signed
copy served on 127.0.0.1 (tests/launcher_store_thumbs.py's: nothing else reached), at each of --sizes, with the prompts
in each style of --styles (the launcher's LAUNCHER_PROMPTS setting, set by the worker):

  every page, the Characters page's three tabs (the Character Store's the widest row): the device label made the
  long one the launcher has, then a longer one still (devlabel), then the real one -- each checked clear of the
  prompts (footer); a picture of the Store's row in each style

  py -3 tests/launcher_footer.py --launcher <the launcher to test> [--sizes 1280x800,1280x720]
      [--styles keyboard,xbox,playstation]
"""
import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import launcher_game_version as lgv  # noqa: E402
import launcher_store_thumbs as sts  # noqa: E402

LONG = "Keyboard and mouse (a controller is connected)"   # the longest the launcher has
LONGER = LONG + " -- a much longer device name, for the layout"


def checks(where):
    """the label made long, longer, then the real one: each clear of the prompts"""
    return (f"devlabel {LONG}\nwait 6\nfooter\ndevlabel {LONGER}\nwait 6\nfooter\ndevlabel\nwait 6\nfooter\n"
            f"# {where}\n")


SCRIPT = ("until 200000 PLAY\nexpect PLAY\nwait 120\n" + checks("Play") +
          "rb\nwait 90\n" + checks("Install / Manage") +
          "rb\nwait 90\n" + checks("Saves") +
          "rb\nwait 120\n" + checks("Characters: My Library") +
          "rt\nwait 60\n" + checks("Characters: Installed") +
          "rt\nwait 120\nuntil 20000 card:\n" + checks("the Character Store") +
          f"devlabel {LONG}\nwait 6\nshot store\nwait 20\ndevlabel\n" +
          "rb\nwait 90\n" + checks("Settings") +
          "rb\nwait 90\n" + checks("Diagnostics") + "quit\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--launcher", required=True)
    ap.add_argument("--work", default=os.path.join(ROOT, "build", "qual", "launcher-footer"))
    ap.add_argument("--sizes", default="1280x800,1280x720")
    ap.add_argument("--styles", default="keyboard,xbox,playstation")
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--keep", action="store_true", help="keep the work folder (the logs, the pictures)")
    a = ap.parse_args()
    launcher, work = os.path.abspath(a.launcher), os.path.abspath(a.work)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    repo, keys, tampered = sts.make_store(os.path.join(work, "store"))
    server = sts.Server(repo, tampered)
    server.up()
    ok = True
    try:
        for style in [s.strip() for s in a.styles.split(",") if s.strip()]:
            for size in [s.strip() for s in a.sizes.split(",") if s.strip()]:
                d = os.path.join(work, style, size)
                root = os.path.join(d, "Cubic.Heroes")
                lgv.make_install(root)
                subprocess.run([sys.executable, os.path.join(ROOT, "tools", "worker.py"), "settings", "--install",
                                root, "--set", f"LAUNCHER_PROMPTS={style}"], stdout=subprocess.DEVNULL, check=True)
                lines, rc = sts.run(launcher, root, os.path.join(d, "run"), f"footer-{style}",
                                    f"# the footer, prompts in the {style} style (tests/launcher_footer.py)\n" + SCRIPT,
                                    size, server, keys, a.timeout)
                ran = [ln for ln in lines if " page=" in ln]
                want = sum(1 for ln in SCRIPT.splitlines() if ln.strip() and not ln.startswith("#"))
                bad = [ln for ln in lines if ln.startswith("FAILED")]
                if not bad and (rc not in (0, None) or len(ran) != want):
                    bad.append(f"{len(ran)} of {want} steps ran, exit {rc}")
                r = not bad
                print(("PASS" if r else "FAIL") + f": footer, {style} prompts, at {size}: " +
                      (bad[0] if bad else f"{sum(1 for ln in lines if 'footer:' in ln)} footer checks clear"), flush=True)
                ok = r and ok
    finally:
        server.down()
    if ok and not a.keep:
        shutil.rmtree(work, ignore_errors=True)
    print("launcher footer:", "PASS" if ok else "FAIL", flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

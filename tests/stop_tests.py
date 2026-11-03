#!/usr/bin/env python3
"""Stop testing so someone can use the PC -- or let it go on.

  tests/stop_tests.py            hold: no test run starts (build/test/HOLD),
                                 every test run still going is stopped the
                                 game's own way (stop.trigger: it writes its
                                 logs and exits), then the game is waited for
  tests/stop_tests.py --resume   remove the hold

A batch of runs (a script looping over tests/boot_smoke.py) sees the hold as
its next run's refusal ("held: ...", status 5) and should end there rather
than wait: a play session (SCENARIO=owner_play) sets the same hold, so tests
waiting for its game to close do not start again when it does. Only test runs
under build/test are stopped; a play session's (build/sessions) is not.
"""
import glob
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOLD = os.path.join(ROOT, "build", "test", "HOLD")


def game_running():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq dotheroes.exe", "/NH", "/FO", "CSV"],
                         capture_output=True, text=True).stdout.lower() if os.name == "nt" else ""
    return "dotheroes.exe" in out


def main():
    if "--resume" in sys.argv[1:]:
        if os.path.exists(HOLD):
            os.remove(HOLD)
        print("hold removed: test runs may start")
        return 0
    os.makedirs(os.path.dirname(HOLD), exist_ok=True)
    with open(HOLD, "w") as f:
        f.write(f"set by tests/stop_tests.py at {time.strftime('%Y-%m-%d %H:%M:%S')}\n")
    live = [d for d in glob.glob(os.path.join(ROOT, "build", "test", "boot-*"))
            if not os.path.exists(os.path.join(d, ".done"))]
    for d in live:
        open(os.path.join(d, "stop.trigger"), "w").close()
        print(f"stopping {os.path.basename(d)}")
    for _ in range(80):
        if not live or all(os.path.exists(os.path.join(d, ".done")) for d in live):
            break
        time.sleep(0.5)
    left = [os.path.basename(d) for d in live if not os.path.exists(os.path.join(d, ".done"))]
    print(f"held ({os.path.relpath(HOLD, ROOT)}); " + (f"still running: {', '.join(left)}" if left else
          "no test run left") + ("; a game is running (a play session?)" if game_running() else ""))
    return 1 if left else 0


if __name__ == "__main__":
    sys.exit(main())

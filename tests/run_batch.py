#!/usr/bin/env python3
"""Run a batch of tests one after another, then report it.

  tests/run_batch.py <name> <run> [<run> ...]

Each <run> is `tag:secs:NAME=value,NAME=value,...` (secs may be empty: the
scenario's own `#! secs`), run as tests/boot_smoke.py with that environment
and RUN_TAG=<tag>. When it is done -- or stopped -- tests/batch_report.py over
the batch's runs goes to build/reports/<date>-<name>.md and is printed.

Ends at once, without starting the next run, when tests/boot_smoke.py says the
batch is held (build/test/HOLD: tests/stop_tests.py, or a play session
starting); waits, a minute at a time, while another game is running; stops
after an hour of that. Each run's summary line is printed as it ends.
"""
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    name, specs = sys.argv[1], sys.argv[2:]
    runs = []
    for spec in specs:
        tag, secs, envs = (spec.split(":", 2) + ["", ""])[:3]
        env = dict(os.environ, RUN_TAG=tag)
        env.setdefault("KEEP_RUNS", "80")
        for kv in filter(None, envs.split(",")):
            k, _, v = kv.partition("=")
            env[k] = v
        waited = 0
        while True:
            out = subprocess.run([sys.executable, os.path.join(ROOT, "tests", "boot_smoke.py")] + ([secs] if secs else []),
                                 env=env, capture_output=True, text=True, cwd=ROOT).stdout
            if out.startswith("held:"):
                print(f"{time.strftime('%H:%M:%S')} {tag}: held -- the batch ends here", flush=True)
                return report(name, runs)
            if "another dotheroes.exe is running" in out and waited < 60:
                waited += 1
                time.sleep(60)
                continue
            break
        run_dir = next((l.split(":", 1)[1].strip() for l in out.splitlines() if l.startswith("run dir")), None)
        if run_dir:
            runs.append(os.path.join(ROOT, run_dir))
        exit_line = next((l for l in out.splitlines() if l.startswith("exit")), out.strip()[:200])
        print(f"{time.strftime('%H:%M:%S')} {tag}: {exit_line}", flush=True)
    return report(name, runs)


def report(name, runs):
    if not runs:
        print("no runs to report")
        return 1
    out = os.path.join(ROOT, "build", "reports", f"{time.strftime('%Y-%m-%d-%H%M')}-{name}.md")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tests", "batch_report.py"), *runs, "--title", name, "-o", out],
                       capture_output=True, text=True)
    print(r.stdout)
    print(f"report: {os.path.relpath(out, ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

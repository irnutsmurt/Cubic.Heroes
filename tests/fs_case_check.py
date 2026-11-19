#!/usr/bin/env python3
"""File-name case on a case-sensitive file system (M11.4): every file the game failed to open, in the given runs,
checked against the disc copy without regard to case. Windows' file system hides a name the game asks for in another
case than the disc's; Linux's does not -- such a file would be missing on Linux only.

    python3 tests/fs_case_check.py DISC_ROOT RUN_DIR_OR_LOG [...]

DISC_ROOT: the extracted disc (build/disc, or an installation's data/disc). Each RUN_DIR (a tests/boot_smoke.py run,
its stderr.log) or log file is read for the runtime's "[fs] open FAIL '<guest>' -> '<host path>'" lines. Exit 1 if
any failed path exists on the disc in another case (the finding, with the disc's own name), else 0. A failed open of a
file that exists nowhere, in any case, is the game's own probing (a fallback it tries first) and is only counted.
"""
import os
import re
import sys

FAIL = re.compile(r"\[fs\] open FAIL '[^']*' -> '([^']+)'")


def logs(args):
    for a in args:
        if os.path.isdir(a):
            for name in ("stderr.log", "game.log"):
                p = os.path.join(a, name)
                if os.path.exists(p):
                    yield p
        elif os.path.exists(a):
            yield a


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    disc = os.path.abspath(argv[1])
    by_lower = {}
    for d, _, fs in os.walk(disc):
        for f in fs:
            p = os.path.join(d, f)
            by_lower[p.lower()] = p
    failed = set()
    for log in logs(argv[2:]):
        with open(log, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = FAIL.search(line)
                if m:
                    failed.add(m.group(1))
    mismatched = []
    for p in sorted(failed):
        real = by_lower.get(os.path.abspath(p).lower())
        if real and real != p:
            mismatched.append((p, real))
    for asked, real in mismatched:
        print(f"[case] asked for {asked}\n       the disc has {real}")
    print(f"[case] {len(failed)} distinct failed open(s) in the runs; {len(mismatched)} only a case apart from a disc "
          f"file; the rest exist in no case (the game's own probing)")
    return 1 if mismatched else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

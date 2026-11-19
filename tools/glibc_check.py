#!/usr/bin/env python3
"""Linux's compatibility baseline, checked (M11.4, docs/LINUX_PACKAGING.md): every shipped ELF file runs on glibc
GLIBC_BASELINE and needs nothing the player's system may lack.

    python3 tools/glibc_check.py FILE_OR_DIR [...]

For each ELF file (a directory: every ELF file under it): the newest GLIBC_x.y symbol version it needs must be at most
the baseline, and each DT_NEEDED library must be glibc's own or one the package ships beside it (NEEDED_OK). Zig links
libc++ statically, so no GLIBCXX version may be needed at all. Exit 1 with each finding listed. Uses readelf
(binutils).
"""
import os
import re
import subprocess
import sys

GLIBC_BASELINE = (2, 31)   # Debian 11, Ubuntu 20.04 and newer; Zig's target x86_64-linux-gnu.2.31
GLIBC_LIBS = {"libc.so.6", "libm.so.6", "libpthread.so.0", "libdl.so.2", "librt.so.1", "libutil.so.1",
              "ld-linux-x86-64.so.2"}
SHIPPED = {"libSDL2-2.0.so.0", "libopenal.so.1", "libavutil.so.61", "libavcodec.so.63", "libavformat.so.63"}
NEEDED_OK = GLIBC_LIBS | SHIPPED


def is_elf(path):
    try:
        with open(path, "rb") as f:
            return f.read(4) == b"\x7fELF"
    except OSError:
        return False


def files(args):
    for a in args:
        if os.path.isdir(a):
            for d, _, fs in os.walk(a):
                for f in sorted(fs):
                    p = os.path.join(d, f)
                    if not os.path.islink(p) and is_elf(p):
                        yield p
        elif is_elf(a):
            yield a


def check(path, shipped=frozenset()):
    """[] when it holds, else what does not. shipped: the library names the package itself carries (a frozen
    Python's own libraries beside it, for example)"""
    out = subprocess.run(["readelf", "-W", "--dynamic", "--version-info", path], capture_output=True,
                         text=True).stdout
    bad = []
    versions = {tuple(int(x) for x in m.split(".")) for m in re.findall(r"GLIBC_(\d+(?:\.\d+)+)", out)}
    newest = max(versions) if versions else None
    if newest and newest[:2] > GLIBC_BASELINE:
        bad.append(f"needs GLIBC_{'.'.join(map(str, newest))} (the baseline is {'.'.join(map(str, GLIBC_BASELINE))})")
    if "GLIBCXX_" in out or "CXXABI_" in out:
        bad.append("needs the system's libstdc++ (GLIBCXX/CXXABI): Zig's libc++ is linked statically")
    for lib in re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]", out):
        if lib not in NEEDED_OK and lib not in shipped:
            bad.append(f"needs {lib}, which is neither glibc's nor shipped")
    return bad, newest


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    failed = n = 0
    found = list(files(argv[1:]))
    shipped = frozenset(os.path.basename(p) for p in found)   # a library the package carries is no system need
    for p in found:
        n += 1
        bad, newest = check(p, shipped)
        print(f"[glibc] {p}: {'GLIBC_' + '.'.join(map(str, newest)) if newest else 'no glibc versions'}"
              + ("" if not bad else " -- " + "; ".join(bad)))
        failed += bool(bad)
    if not n:
        print("[glibc] no ELF file found")
        return 1
    print(f"[glibc] {n} file(s), {failed} outside the baseline")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

#!/usr/bin/env python3
"""FFmpeg for the movies (M8, docs/FFMPEG_MIGRATION_PLAN.md): the minimal LGPL shared libraries, built with the
bundled Zig, without make.

    python3 tools/build_ffmpeg.py [--platform windows|linux|all] [--jobs N]

FFmpeg's own configure script (a shell script: Git Bash runs it on Windows) writes the configuration -- config.h,
config_components.h, ffbuild/config.mak and the component lists -- for exactly the parts the movies need: the Bink
demuxer and the Bink video and audio decoders, in avformat, avcodec and avutil, nothing else (no programs, no
network, no autodetected system libraries, no assembly). This driver then does what FFmpeg's makefiles would: it
reads each library's object list from its Makefile (OBJS and the OBJS-$(CONFIG_...) lines that configure turned
on, SHLIBOBJS for a shared build), compiles them with configure's flags, generates libavutil/ffversion.h with
FFmpeg's version.sh, and links the shared libraries: avutil-61.dll, avcodec-63.dll, avformat-63.dll with their
import libraries on Windows (the exports a .def file lists, derived as FFmpeg's compat/windows/makedef does from
each library's version script); libavutil.so.61, libavcodec.so.63, libavformat.so.63 on Linux (with FFmpeg's
version scripts). It installs them with the public headers into build/deps/<platform>/ and writes
FFmpeg-NOTICE.txt: the source (the release archive pinned in config/deps.sha256, its commit), the configure line,
any correction made to configure's output (CORRECTIONS) and this driver -- what the LGPL asks to ship with the
libraries, beside COPYING.LGPLv2.1. FFmpeg's sources are used as they are: nothing is modified.

configure refuses a path with spaces: on Windows the tree is reached through the folder's 8.3 short name.
"""
import argparse
import concurrent.futures
import ctypes
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import build_deps  # noqa: E402

NAME = "FFmpeg"
COMMIT = "946fcce07b6dcd0331c8cc609192aeff5e1924f8"   # the tag n9.0.2
LIBS = ["avutil", "avcodec", "avformat"]              # in link order: each needs the ones before it
CONFIGURE = ["--disable-everything", "--disable-autodetect", "--disable-asm", "--disable-programs", "--disable-doc",
             "--disable-network", "--disable-avdevice", "--disable-avfilter", "--disable-swscale",
             "--disable-swresample", "--enable-shared", "--disable-static",
             "--enable-demuxer=bink", "--enable-decoder=bink,binkaudio_dct,binkaudio_rdft"]
TARGET = {"windows": ("x86_64-windows-gnu", "mingw32"), "linux": ("x86_64-linux-gnu.2.31", "linux")}
NOTICE = "FFmpeg-NOTICE.txt"
LICENCE = ("COPYING.LGPLv2.1", "FFmpeg-LGPL-2.1.txt")
# Corrections to what configure writes into config.h, per platform: (macro, value, why). Each is listed in the
# NOTICE. configure probes system functions by linking only, and has no option to turn one off.
CORRECTIONS = {
    "linux": [("HAVE_SYSCTL", "0",
               "configure's link probe finds glibc's compatibility sysctl symbol, but <sys/sysctl.h> is gone since "
               "glibc 2.32 (Zig's headers); Linux never takes that path (libavutil/cpu.c counts CPUs with "
               "sched_getaffinity first)")],
}


def short_win(path):
    """A Windows path without spaces (the 8.3 short form), for a process's working directory."""
    if os.name == "nt":
        buf = ctypes.create_unicode_buffer(1024)
        if ctypes.windll.kernel32.GetShortPathNameW(path, buf, 1024):
            return buf.value
    return path


def short(path):
    """The path as configure can take it: on Windows the 8.3 short form (no spaces), with forward slashes in the
    MSYS style (/c/...)."""
    if os.name == "nt":
        buf = ctypes.create_unicode_buffer(1024)
        if ctypes.windll.kernel32.GetShortPathNameW(path, buf, 1024):
            path = buf.value
        path = path.replace("\\", "/")
        if re.match(r"^[A-Za-z]:/", path):
            path = "/" + path[0].lower() + path[2:]
    if " " in path:
        sys.exit("[ffmpeg] %s: a path with spaces and no short name (configure refuses it)" % path)
    return path


def zig():
    return os.path.join(ROOT, "build", "toolchain", "zig-x86_64-windows-0.17.0", "zig.exe") if os.name == "nt" \
        else build_deps.zig_build.zig_path() if hasattr(build_deps.zig_build, "zig_path") else "zig"


def tools(platform):
    """sh wrappers for configure: the target's compiler, the host's, ar and ranlib (the bundled Zig). configure is
    given them by name (they are put on PATH), not by path: avutil embeds configure's command line
    (avutil_configuration()), and a build folder's path does not belong in a distributed library."""
    d = os.path.join(build_deps.DEPS, "work", "tools")
    os.makedirs(d, exist_ok=True)
    z = short(zig())
    target = TARGET[platform][0]
    wrappers = {"zcc-" + platform: 'exec "%s" cc -target %s "$@"' % (z, target),
                "zcc-host": 'exec "%s" cc "$@"' % z, "zar": 'exec "%s" ar "$@"' % z,
                "zranlib": 'exec "%s" ranlib "$@"' % z}
    for n, body in wrappers.items():
        with open(os.path.join(d, n), "w", newline="\n") as f:
            f.write("#!/bin/sh\n" + body + "\n")
        os.chmod(os.path.join(d, n), 0o755)
    return d


def configure_args(platform):
    """configure's arguments: no paths in them (see tools())."""
    return ["--target-os=" + TARGET[platform][1], "--arch=x86_64", "--enable-cross-compile",
            "--cc=zcc-" + platform, "--host-cc=zcc-host", "--ar=zar", "--ranlib=zranlib", "--nm=true",
            "--strip=true"] + CONFIGURE


def configure(platform, src, work):
    """configure, unless the work folder was configured with the same arguments (configure.args): a change of
    the arguments starts the folder afresh, so no object or configuration of another configuration is kept."""
    args = configure_args(platform)
    stamp = os.path.join(work, "configure.args")
    if os.path.exists(os.path.join(work, "ffbuild", "config.mak")) and os.path.exists(stamp) \
            and open(stamp).read() == "\n".join(args):
        return
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    t = tools(platform)
    tmp = os.path.join(build_deps.DEPS, "work", "tmp")
    os.makedirs(tmp, exist_ok=True)
    env = dict(os.environ, TMPDIR=short(tmp), TEMP=short(tmp), TMP=short(tmp),
               PATH=t + os.pathsep + os.environ.get("PATH", ""))
    print("[ffmpeg] %s: configure" % platform, flush=True)
    with open(os.path.join(work, "configure.out"), "w") as log:
        subprocess.check_call(["sh", short(src) + "/configure"] + args, cwd=short_win(work), env=env, stdout=log,
                              stderr=subprocess.STDOUT)
    with open(stamp, "w") as f:
        f.write("\n".join(args))


def correct(platform, work):
    """CORRECTIONS applied to configure's config.h (idempotent)."""
    path = os.path.join(work, "config.h")
    text = open(path, encoding="utf-8", newline="").read()
    for macro, value, _why in CORRECTIONS.get(platform, []):
        text, n = re.subn(r"^#define %s \S+$" % macro, "#define %s %s" % (macro, value), text, flags=re.M)
        if n != 1:
            sys.exit("[ffmpeg] %s: config.h defines %s %d times (expected once)" % (platform, macro, n))
    open(path, "w", encoding="utf-8", newline="").write(text)


def config_mak(work):
    v = {}
    for line in open(os.path.join(work, "ffbuild", "config.mak"), encoding="utf-8"):
        line = line.rstrip("\n")
        m = re.match(r"^([A-Za-z0-9_-]+)\s*:?=\s?(.*)$", line)
        if m and not line.startswith("!"):
            v[m.group(1)] = m.group(2)
        m = re.match(r"^(!?)([A-Z0-9_]+)=yes$", line)   # "!NAME=yes": configure turned NAME off
        if m:
            v[m.group(2)] = "" if m.group(1) else "yes"
    return v


def expand(s, v, depth=0):
    if depth > 8:
        return s
    out = re.sub(r"\$\(([A-Za-z0-9_-]+)\)", lambda m: v.get(m.group(1), ""), s)
    return out if out == s else expand(out, v, depth + 1)


def objects(src, lib, v):
    """A library's objects as its Makefile (and the Makefiles it includes) lists them, for this configuration."""
    objs, seen = [], set()

    def read(path):
        lines, cont = [], ""
        for raw in open(path, encoding="utf-8"):
            raw = raw.rstrip("\n")
            if raw.endswith("\\"):
                cont += raw[:-1] + " "
                continue
            lines.append(cont + raw)
            cont = ""
        stack = [True]
        for line in lines:
            s = line.strip()
            m = re.match(r"^(ifdef|ifndef)\s+(\S+)", s)
            if m:
                on = v.get(m.group(2)) == "yes"
                stack.append(stack[-1] and (on if m.group(1) == "ifdef" else not on))
                continue
            if s.startswith("ifeq") or s.startswith("ifneq"):
                stack.append(False if stack[-1] else False)   # not used by the parts built here
                continue
            if s == "else":
                stack[-1] = (not stack[-1]) and all(stack[:-1])
                continue
            if s == "endif":
                stack.pop()
                continue
            if not stack[-1]:
                continue
            m = re.match(r"^include\s+\$\(SRC_PATH\)/(\S+)", s)
            if m:
                read(os.path.join(src, m.group(1)))
                continue
            m = re.match(r"^(OBJS|SHLIBOBJS)(-\$\(([A-Z0-9_]+)\))?\s*\+?=\s*(.*)$", s)
            if not m:
                continue
            if m.group(3) and v.get(m.group(3)) != "yes":
                continue
            for o in m.group(4).split():
                if o.endswith(".o") and "$" not in o and o not in seen:
                    seen.add(o)
                    objs.append(o)
    read(os.path.join(src, "lib" + lib, "Makefile"))
    return objs


def compile_all(platform, src, work, v, jobs):
    cc = [zig(), "cc", "-target", TARGET[platform][0]]
    srcs = short_win(src)   # no spaces: the flags are split on them
    flags = ["-I.", "-I" + srcs + "/"] + expand(v.get("CPPFLAGS", ""), dict(v, SRC_PATH=srcs)).split() \
        + expand(v.get("CFLAGS", ""), v).split() + ["-DHAVE_AV_CONFIG_H"]
    flags = [f for f in flags if f]
    # the source and build folders as neutral names in __FILE__ (FFmpeg's assertion messages) and debug info:
    # no build folder's path in a distributed library
    for real, name in ((src, "FFmpeg-n9.0.2"), (srcs, "FFmpeg-n9.0.2"), (work, "build"), (short_win(work), "build")):
        flags.append("-ffile-prefix-map=%s=%s" % (real, name))
    todo = []
    per_lib = {}
    for lib in LIBS:
        objs = objects(src, lib, v)
        per_lib[lib] = []
        for o in objs:
            c = os.path.join(src, "lib" + lib, o[:-2] + ".c")
            gen = os.path.join(work, "lib" + lib, o[:-2] + ".c")   # configure's own lists (codec_list.c, ...)
            if not os.path.exists(c) and os.path.exists(gen):
                c = gen
            if not os.path.exists(c):
                sys.exit("[ffmpeg] lib%s/%s: no source for it" % (lib, o))
            out = os.path.join(work, "lib" + lib, o)
            per_lib[lib].append(out)
            if not os.path.exists(out) or os.path.getmtime(out) < os.path.getmtime(c):
                todo.append((c, out))
    print("[ffmpeg] %s: %d objects to compile (%s)" % (platform, len(todo),
          ", ".join("%s %d" % (k, len(x)) for k, x in per_lib.items())), flush=True)

    def one(job):
        c, out = job
        os.makedirs(os.path.dirname(out), exist_ok=True)
        r = subprocess.run(cc + flags + ["-c", "-o", out, c], cwd=work, capture_output=True, text=True)
        if r.returncode:
            return "%s:\n%s" % (c, r.stderr[-4000:])
        return None
    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        errs = [e for e in ex.map(one, todo) if e]
    if errs:
        sys.exit("[ffmpeg] %d object(s) failed:\n%s" % (len(errs), errs[0]))
    return per_lib


def coff_exports(objs, patterns):
    """What FFmpeg's compat/windows/makedef derives with nm, read from the COFF objects themselves: each defined
    external symbol matching the library's version script (its global patterns), "DATA" for one outside a code
    section. Returns the .def file's lines."""
    import fnmatch
    import struct
    out, seen = [], set()
    for path in objs:
        d = open(path, "rb").read()
        machine, nsec = struct.unpack_from("<HH", d, 0)
        if machine == 0 and nsec == 0xFFFF:
            sys.exit("[ffmpeg] %s: a bigobj COFF file (not read here)" % path)
        symoff, nsym, opt = struct.unpack_from("<IIH", d, 8)
        sec_chars = []
        for i in range(nsec):
            sec_chars.append(struct.unpack_from("<I", d, 20 + opt + 40 * i + 36)[0])
        strtab = symoff + 18 * nsym
        i = 0
        while i < nsym:
            e = symoff + 18 * i
            name_raw = d[e:e + 8]
            value, secnum, typ, sclass, naux = struct.unpack_from("<IhHBB", d, e + 8)
            if name_raw[:4] == b"\0\0\0\0":
                off = struct.unpack_from("<I", name_raw, 4)[0]
                end = d.index(b"\0", strtab + off)
                name = d[strtab + off:end].decode("ascii", "replace")
            else:
                name = name_raw.rstrip(b"\0").decode("ascii", "replace")
            if sclass == 2 and secnum > 0 and name not in seen and any(fnmatch.fnmatchcase(name, pat) for pat in patterns):
                seen.add(name)
                code = sec_chars[secnum - 1] & 0x20
                out.append(name if code else name + " DATA")
            i += 1 + naux
    return sorted(out)


def version_globals(path):
    """The global patterns of an FFmpeg version script (lib*.v)."""
    text = open(path).read()
    g = text.split("global:")[1].split("local:")[0]
    return [p.strip().rstrip(";").strip() for p in g.split(";") if p.strip()]


def majors(src):
    out = {}
    for lib in LIBS:
        for f in ("version_major.h", "version.h"):
            p = os.path.join(src, "lib" + lib, f)
            m = os.path.exists(p) and re.search(r"#define LIB%s_VERSION_MAJOR\s+(\d+)" % lib.upper(), open(p).read())
            if m:
                out[lib] = m.group(1)
                break
    return out


def link(platform, src, work, v, per_lib):
    cc = [zig(), "cc", "-target", TARGET[platform][0]]
    major = majors(src)
    made = {}
    for lib in LIBS:
        deps = LIBS[:LIBS.index(lib)]
        extra = expand(v.get("EXTRALIBS-" + lib, ""), v).split()
        if platform == "windows":
            name = "%s-%s.dll" % (lib, major[lib])
            out = os.path.join(work, "lib" + lib, name)
            implib = os.path.join(work, "lib" + lib, "lib%s.dll.a" % lib)
            # the exports, as FFmpeg's makedef writes them from the version script (lib<name>.v)
            deffile = os.path.join(work, "lib" + lib, "%s-%s.def" % (lib, major[lib]))
            exports = coff_exports(per_lib[lib], version_globals(os.path.join(src, "lib" + lib, "lib%s.v" % lib)))
            with open(deffile, "w", newline="\n") as f:
                f.write("EXPORTS\n" + "".join("    %s\n" % e for e in exports))
            args = ["-shared", "-o", out, "-Wl,--out-implib," + implib, deffile,
                    "-Wl,--nxcompat", "-Wl,--dynamicbase", "-Wl,--high-entropy-va"]
            args += [os.path.join(work, "lib" + d, "lib%s.dll.a" % d) for d in deps]
        else:
            name = "lib%s.so.%s" % (lib, major[lib])
            out = os.path.join(work, "lib" + lib, name)
            ver_in = os.path.join(src, "lib" + lib, "lib%s.v" % lib)
            ver = os.path.join(work, "lib" + lib, "lib%s.ver" % lib)
            with open(ver, "w", newline="\n") as f:
                f.write(open(ver_in).read().replace("MAJOR", major[lib]))
            # configure's SHFLAGS and LDFLAGS; stripped as FFmpeg's `make install` does (its STRIP step; the
            # objects carry configure's -g)
            args = ["-shared", "-o", out, "-Wl,-soname," + name, "-Wl,--version-script," + ver, "-Wl,-Bsymbolic",
                    "-Wl,-z,noexecstack", "-Wl,--as-needed", "-Wl,--strip-all",
                    "-Wl,-rpath,$ORIGIN"]   # each finds the others beside it (as libopenal.so.1 does)
            args += [os.path.join(work, "lib" + d, "lib%s.so.%s" % (d, major[d])) for d in deps]
        r = subprocess.run(cc + args + per_lib[lib] + [e for e in extra if e != "-latomic"],
                           cwd=work, capture_output=True, text=True)
        if r.returncode:
            sys.exit("[ffmpeg] linking %s:\n%s" % (name, r.stderr[-4000:]))
        made[lib] = out
        print("[ffmpeg] %s: %s, %.1f MB" % (platform, name, os.path.getsize(out) / 1e6), flush=True)
    return made


def headers(src, lib):
    """The public headers a library installs (its Makefile's HEADERS, as `make install-headers` does)."""
    text = re.sub(r"\\\n", " ", open(os.path.join(src, "lib" + lib, "Makefile"), encoding="utf-8").read())
    m = re.search(r"^HEADERS\s*=\s*(.*)$", text, re.M)
    return m.group(1).split() if m else []


def install(platform, src, work, made):
    prefix = os.path.join(build_deps.DEPS, platform)
    bindir = os.path.join(prefix, "bin" if platform == "windows" else "lib")
    os.makedirs(bindir, exist_ok=True)
    for lib, out in made.items():
        shutil.copy2(out, bindir)
        if platform == "windows":
            libdir = os.path.join(prefix, "lib")
            os.makedirs(libdir, exist_ok=True)
            shutil.copy2(os.path.join(work, "lib" + lib, "lib%s.dll.a" % lib), libdir)
        inc = os.path.join(prefix, "include", "lib" + lib)
        os.makedirs(inc, exist_ok=True)
        for h in headers(src, lib):
            for base in (os.path.join(src, "lib" + lib), os.path.join(work, "lib" + lib)):
                if os.path.exists(os.path.join(base, h)):
                    shutil.copy2(os.path.join(base, h), inc)
                    break
    for gen in ("avconfig.h", "ffversion.h"):
        shutil.copy2(os.path.join(work, "libavutil", gen), os.path.join(prefix, "include", "libavutil"))
    shutil.copy2(os.path.join(src, LICENCE[0]), os.path.join(bindir, LICENCE[1]))
    with open(os.path.join(bindir, NOTICE), "w", encoding="utf-8", newline="\n") as f:
        f.write(notice(platform))
    # the configuration as built (configure's config.h with CORRECTIONS, its component list): shipped with the
    # source (source_files)
    conf = os.path.join(prefix, "share", "FFmpeg-build")
    os.makedirs(conf, exist_ok=True)
    for gen in ("config.h", "config_components.h"):
        shutil.copy2(os.path.join(work, gen), conf)
    print("[ffmpeg] %s: installed into %s" % (platform, os.path.relpath(prefix, ROOT)), flush=True)


# ---- what ships (tools/zig_build.py beside the game, tools/make_bundle.py in the setup's licenses/) ----

# the libraries the game loads, per platform: (where install() puts them under build/deps/<platform>, names)
LIB_FILES = {"windows": ("bin", ["avutil-61.dll", "avcodec-63.dll", "avformat-63.dll"]),
             "linux": ("lib", ["libavutil.so.61", "libavcodec.so.63", "libavformat.so.63"])}
SOURCE_ARCHIVE = "FFmpeg-n9.0.2.tar.gz"   # licenses/source/: the release archive itself (the pinned sha256)
BUILD_DIR = "FFmpeg-build"                # licenses/source/: this driver and the configuration as built


def notice(platform):
    url = build_deps.SOURCES[NAME][0]
    digest = build_deps.pins().get(NAME, "?")
    libs = ", ".join(LIB_FILES[platform][1])
    return ("FFmpeg 9.0.2 -- the game's movie decoder\n\n"
            "%s are FFmpeg's libavutil, libavcodec and libavformat, version 9.0.2,\n"
            "copyright the FFmpeg developers, licensed under the GNU Lesser General Public License version 2.1 or\n"
            "(at your option) any later version (%s). No GPL or non-free part is enabled. They are not\n"
            "modified. The game loads them at run time from its own folder (dynamic linking): you may replace them\n"
            "with another build of FFmpeg 9.0.x.\n\n" % (libs, LICENCE[1]) +
            "Enabled: the Bink demuxer and FFmpeg's own Bink video and Bink audio (DCT, RDFT) decoders; nothing\n"
            "else (no programs, devices, filters, scaling, resampling, network or assembly).\n\n"
            "Source: %s\n  (sha256 %s),\n  the release tagged n9.0.2, commit %s.\n"
            "The corresponding source is shipped with the setup as licenses/source/%s (the same archive),\n"
            "with licenses/source/%s/: the build driver and the configuration as built.\n\n"
            % (url, digest, COMMIT, SOURCE_ARCHIVE, BUILD_DIR) +
            "Configured with FFmpeg's configure:\n  " + " ".join(configure_args(platform)) + "\n"
            "  (zcc-%s, zcc-host, zar, zranlib: Zig's clang, ar and ranlib for the target %s)\n"
            % (platform, TARGET[platform][0]) +
            "".join("Corrected in configure's config.h: %s set to %s -- %s.\n" % c
                    for c in CORRECTIONS.get(platform, [])) +
            "Built by this project's tools/build_ffmpeg.py (in %s/), which compiles the objects configure\n"
            "selects with configure's flags and links the shared libraries with FFmpeg's version scripts, as\n"
            "FFmpeg's makefiles do; the libraries are stripped. FFmpeg's own `make` after the same configure line\n"
            "(and the correction above) builds the same libraries.\n" % BUILD_DIR)


def source_files(out_dir, platform):
    """licenses/source/ for FFmpeg: the pinned release archive (checked against config/deps.sha256), this driver
    and the configuration install() kept. Returns the archive's path."""
    build_deps.fetch(NAME)
    archive = os.path.join(build_deps.DEPS, "src", os.path.basename(build_deps.SOURCES[NAME][0]))
    import hashlib
    if hashlib.sha256(open(archive, "rb").read()).hexdigest() != build_deps.pins().get(NAME):
        sys.exit("[ffmpeg] %s: not the pinned archive" % archive)
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, SOURCE_ARCHIVE)
    shutil.copy2(archive, out)
    b = os.path.join(out_dir, BUILD_DIR)
    os.makedirs(b, exist_ok=True)
    shutil.copy2(os.path.abspath(__file__), b)
    conf = os.path.join(build_deps.DEPS, platform, "share", "FFmpeg-build")
    for gen in ("config.h", "config_components.h"):
        shutil.copy2(os.path.join(conf, gen), b)
    return out


def ship(platform, out_dir):
    """The libraries, the notice and the licence beside the game (tools/zig_build.py's prebuilt folder)."""
    deps = os.path.join(build_deps.DEPS, platform)
    sub, names = LIB_FILES[platform]
    for n in names + [NOTICE, LICENCE[1]]:
        shutil.copy2(os.path.join(deps, sub, n), out_dir)


def build(platform, jobs=os.cpu_count() or 4):
    src = build_deps.fetch(NAME)
    work = os.path.join(build_deps.DEPS, "work", platform, NAME)
    configure(platform, src, work)
    correct(platform, work)
    v = config_mak(work)
    ffv = os.path.join(work, "libavutil", "ffversion.h")
    if not os.path.exists(ffv):
        os.makedirs(os.path.dirname(ffv), exist_ok=True)
        subprocess.check_call(["sh", short(src) + "/ffbuild/version.sh", short(src), "libavutil/ffversion.h"],
                              cwd=short_win(work), env=dict(os.environ, revision="n9.0.2"))
    per_lib = compile_all(platform, src, work, v, jobs)
    made = link(platform, src, work, v, per_lib)
    names = sorted(os.path.basename(m) for m in made.values())
    if names != sorted(LIB_FILES[platform][1]):
        sys.exit("[ffmpeg] %s: built %s, LIB_FILES names %s" % (platform, names, LIB_FILES[platform][1]))
    install(platform, src, work, made)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--platform", choices=("windows", "linux", "all"), default="all")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()
    for p in ("windows", "linux") if a.platform == "all" else (a.platform,):
        build(p, a.jobs)


if __name__ == "__main__":
    main()

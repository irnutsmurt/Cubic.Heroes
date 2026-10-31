#!/usr/bin/env python3
"""Building with the bundled Zig toolchain (clang + libc/libc++ for every target).

Platforms: "linux" (x86_64, glibc 2.31 and newer) and "windows" (x86_64, MinGW
ABI). The developer builds the prebuilt parts for both from Linux; a player's
machine builds the game for itself (HOST).
"""
import os
import sys

# the source checkout, or the packaged app's folder (PyInstaller: next to the executable)
ROOT = (os.path.dirname(sys.executable) if getattr(sys, "frozen", False)
        else os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# the oldest Linux the game runs on: glibc 2.31 (Ubuntu 20.04, Debian 11 and newer)
TARGETS = {"linux": "x86_64-linux-gnu.2.31", "windows": "x86_64-windows-gnu"}
HOST = "windows" if os.name == "nt" else "linux"

# The prebuilt parts (runtime library, dependencies) run on any player's machine,
# so they target x86-64-v2 (SSE4.2, SSSE3, POPCNT), which Windows 11 requires
# anyway. Code a machine compiles for itself targets that machine's own level.
# The baseline (SSE2) costs dearly in lifted SPU code: its shufb fell back to a
# byte loop and its float helpers to one lane at a time, and 3D Dot Game Heroes'
# Bullet collision task ran 3.77 ms a time instead of 0.80 ms at v3.
PORTABLE_CPU = "x86_64_v2"
_V3 = ("__AVX2__", "__BMI2__", "__FMA__", "__F16C__", "__LZCNT__", "__MOVBE__")
_V2 = ("__SSE4_2__", "__SSSE3__", "__POPCNT__")
_host_cpu = None


class BuildError(Exception):
    pass


def exe_suffix(platform=HOST):
    return ".exe" if platform == "windows" else ""


def find_zig():
    """The bundled zig (build/toolchain/zig-*/zig, or toolchain/ next to the app), else one on PATH."""
    exe = "zig.exe" if os.name == "nt" else "zig"
    for base in (os.path.join(ROOT, "toolchain"), os.path.join(ROOT, "build", "toolchain")):
        if os.path.isdir(base):
            for d in sorted(os.listdir(base), reverse=True):
                p = os.path.join(base, d, exe)
                if d.startswith("zig") and os.path.exists(p):
                    return p
    for d in os.environ.get("PATH", "").split(os.pathsep):
        p = os.path.join(d, exe)
        if os.path.exists(p):
            return p
    raise BuildError("no Zig toolchain found (build/toolchain/zig-*/ or PATH)")


def host_cpu(zig=None):
    """The CPU level this machine compiles its own code for, as zig names it:
    x86_64_v3 when the host has AVX2, FMA, BMI2 and the rest of v3, else
    x86_64_v2 or the baseline. DOTHEROES_CPU=<zig cpu name> overrides."""
    global _host_cpu
    if os.environ.get("DOTHEROES_CPU"):
        return os.environ["DOTHEROES_CPU"]
    if _host_cpu is None:
        import subprocess
        r = subprocess.run([zig or find_zig(), "cc", "-mcpu=native", "-dM", "-E", "-x", "c", "-"],
                           input="", capture_output=True, text=True)
        have = set(l.split()[1] for l in r.stdout.splitlines() if l.startswith("#define "))
        _host_cpu = ("x86_64_v3" if all(m in have for m in _V3 + _V2) else
                     "x86_64_v2" if all(m in have for m in _V2) else "x86_64")
    return _host_cpu


def cmake_toolchain(platform, zig=None, extra_include_after=(), cpu=None):
    """A CMake toolchain file that compiles for `platform` with zig, through small
    wrapper scripts (CMake wants one executable per tool). Developer side only:
    players' machines never run CMake. `cpu`: the zig CPU level, by default this
    machine's (host_cpu); what is shipped prebuilt passes PORTABLE_CPU. Returns
    its path."""
    zig = zig or find_zig()
    cpu = cpu or host_cpu(zig)
    d = os.path.join(ROOT, "build", "toolchain", "cmake-" + platform + ("-host" if extra_include_after else "")
                     + ("" if cpu == host_cpu(zig) else "-" + cpu))
    os.makedirs(d, exist_ok=True)
    extra = f" -mcpu={cpu}" + "".join(f' -idirafter "{p}"' for p in extra_include_after)
    tools = {}
    for name, cmd in (("cc", f"cc -target {TARGETS[platform]}{extra}"),
                      ("cxx", f"c++ -target {TARGETS[platform]}{extra}"),
                      ("ar", "ar"), ("ranlib", "ranlib"), ("rc", "rc")):
        p = os.path.join(d, "zig-" + name)
        if os.name == "nt":  # a Windows host runs .cmd files, not shell scripts
            p += ".cmd"
            with open(p, "w", newline="\r\n") as f:
                f.write(f'@"{zig}" {cmd} %*\n')
        else:
            with open(p, "w") as f:
                f.write(f'#!/bin/sh\nexec "{zig}" {cmd} "$@"\n')
            os.chmod(p, 0o755)
        tools[name] = p.replace("\\", "/")
    tc = os.path.join(d, "toolchain.cmake")
    with open(tc, "w") as f:
        if platform == "windows":
            f.write("set(CMAKE_SYSTEM_NAME Windows)\nset(CMAKE_SYSTEM_PROCESSOR x86_64)\n")
            f.write(f'set(CMAKE_RC_COMPILER "{tools["rc"]}")\n')
        else:
            f.write("set(CMAKE_SYSTEM_NAME Linux)\nset(CMAKE_SYSTEM_PROCESSOR x86_64)\n")
        f.write(f'set(CMAKE_C_COMPILER "{tools["cc"]}")\n'
                f'set(CMAKE_CXX_COMPILER "{tools["cxx"]}")\n'
                f'set(CMAKE_AR "{tools["ar"]}")\n'
                f'set(CMAKE_RANLIB "{tools["ranlib"]}")\n'
                "set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)\n")
    return tc


# ---------------- the sources a player's machine compiles ----------------
#
# The game's own code (lifted from the player's disc into <gen>/recompiled and
# <gen>/spu) plus the parts of the port that are compiled against it: the
# port's main program and the runtime's PPU scaffold, which include the
# generated ppu_recomp.h. Everything else of the runtime is prebuilt
# (libps3recomp_runtime.a, game-independent). In a checkout these come from
# port/ and build/ref/ps3recomp/; in the packaged app from src/.

import concurrent.futures as cf  # noqa: E402
import hashlib  # noqa: E402
import json  # noqa: E402
import shutil  # noqa: E402
import subprocess  # noqa: E402
import time  # noqa: E402

CXX_STD, C_STD = "-std=gnu++20", "-std=gnu17"
COMMON = ["-fno-strict-aliasing", "-fwrapv", "-DPS3RECOMP_RSX_VULKAN"]
PORT_FLAGS = [CXX_STD, "-O1", "-w"] + COMMON
PPU_FLAGS = [CXX_STD, "-O1", "-w", "-ffp-contract=off"] + COMMON
SPU_FLAGS = [C_STD, "-O2", "-w", "-DSPU_USE_DISPATCH_MT"] + COMMON
# No build paths in what ships: the checkout and the home folder, as given and
# (Windows) with forward slashes, which is how CMake hands sources over.
SHIP_FLAGS = ["-g0"] + [f"-ffile-prefix-map={d}={to}" for d, to in ((ROOT, "."), (os.path.expanduser("~"), "~"))
                        for d in sorted({d, d.replace(os.sep, "/")})]
SCAFFOLD = ["runtime/ppu/ppu_loader.cpp", "runtime/ppu/ppu_hle.cpp", "runtime/ppu/ppu_fs.cpp",
            "runtime/ppu/ppu_sysprx.cpp"]
PORT_SOURCES = ["main.cpp", "stubs.cpp", "game_patches.cpp", "diag/diag.cpp"]
RUNTIME_LIB = "libps3recomp_runtime.a"
NATIVE_LIB = "libdotheroes_native.a"   # port/native: the native SPU-task replacements (Bullet 2.74 in it)


class Sources:
    """Where the port's and the runtime's sources and headers are."""

    def __init__(self, platform=HOST):
        packaged = os.path.isdir(os.path.join(ROOT, "src", "ps3recomp"))
        if packaged:
            self.port = os.path.join(ROOT, "src", "port")
            self.ps3 = os.path.join(ROOT, "src", "ps3recomp")
            self.nids = os.path.join(ROOT, "src", "generated", "ppu_hle_nids.cpp")
            self.lib_dir = os.path.join(ROOT, "lib")
        else:
            self.port = os.path.join(ROOT, "port")
            self.ps3 = os.environ.get("PS3RECOMP_DIR", os.path.join(ROOT, "build", "ref", "ps3recomp"))
            self.lib_dir = os.path.join(ROOT, "build", "lib", platform)
            self.nids = os.path.join(self.lib_dir, "src", "ppu_hle_nids.cpp")

    def includes(self, gen_ppu):
        p = self.ps3
        return ["-I" + gen_ppu, "-I" + os.path.join(p, "include"), "-I" + os.path.join(p, "runtime", "ppu"),
                "-I" + os.path.join(p, "runtime", "spu"), "-I" + os.path.join(p, "runtime", "memory"),
                "-I" + os.path.join(p, "runtime", "syscalls"), "-I" + os.path.join(p, "runtime", "platform"),
                "-I" + os.path.join(p, "libs", "video"), "-I" + self.port]

    def headers(self):
        """every header the compiled sources may include"""
        for top in (os.path.join(self.ps3, "include"), os.path.join(self.ps3, "runtime"),
                    os.path.join(self.ps3, "libs"), self.port):
            for d, _, fs in os.walk(top):
                for f in fs:
                    if f.endswith((".h", ".hpp", ".inc")):
                        yield os.path.join(d, f)


def jobs_for_memory():
    """Parallel compiles: one per core, but at most one per 3 GB of RAM (a
    recompiled chunk is ~60k lines of C++; clang needs 2-3 GB for one)."""
    cores = max(1, os.cpu_count() or 1)
    gb = 8.0
    try:
        if os.name == "nt":
            import ctypes

            class MS(ctypes.Structure):
                _fields_ = [("len", ctypes.c_ulong), ("load", ctypes.c_ulong), ("total", ctypes.c_ulonglong),
                            ("avail", ctypes.c_ulonglong), ("tpf", ctypes.c_ulonglong),
                            ("apf", ctypes.c_ulonglong), ("tv", ctypes.c_ulonglong), ("av", ctypes.c_ulonglong),
                            ("aev", ctypes.c_ulonglong)]
            m = MS()
            m.len = ctypes.sizeof(MS)
            ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
            gb = m.total / 2**30
        else:
            for line in open("/proc/meminfo"):
                if line.startswith("MemTotal:"):
                    gb = int(line.split()[1]) / 2**20
    except Exception:
        pass
    return max(1, min(cores, int(gb // 3)))


def _run(cmd, **kw):
    # no console window flashing up for every compile on Windows
    if os.name == "nt":
        kw.setdefault("creationflags", 0x08000000)  # CREATE_NO_WINDOW
    return subprocess.run(cmd, **kw)


_ZIG_VERSIONS = {}


def _zig_version(zig):
    """The compiler's version, not where it is: moving the app keeps the cache."""
    if zig not in _ZIG_VERSIONS:
        r = _run([zig, "version"], capture_output=True, text=True)
        _ZIG_VERSIONS[zig] = "zig " + (r.stdout.strip() if r.returncode == 0 else zig)
    return _ZIG_VERSIONS[zig]


def _file_hash(h, path):
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)


def headers_fingerprint(src, gen_ppu):
    """what every object depends on besides its own source: the compiler and
    every header it may include, generated (ppu_recomp.h) or not"""
    h = hashlib.sha1()
    paths = sorted(src.headers()) + sorted(os.path.join(gen_ppu, f) for f in os.listdir(gen_ppu)
                                           if f.endswith(".h"))
    for p in paths:
        h.update(os.path.basename(p).encode() + b"\0")
        _file_hash(h, p)
    return h.digest()


def game_jobs(src, gen_dir, obj_dir, zig, platform=HOST):
    """(source, object, compiler, flags) for everything the player compiles. Objects are
    named by a fingerprint of source + flags + compiler + headers: existing means current."""
    gen_ppu, gen_spu = os.path.join(gen_dir, "recompiled"), os.path.join(gen_dir, "spu")
    inc = src.includes(gen_ppu)
    cpu = ["-mcpu=" + host_cpu(zig)]
    common = _zig_version(zig).encode() + TARGETS[platform].encode() + headers_fingerprint(src, gen_ppu)
    groups = []
    groups += [(os.path.join(src.port, f), "c++", PORT_FLAGS) for f in PORT_SOURCES]
    groups += [(os.path.join(src.ps3, f), "c++", PORT_FLAGS) for f in SCAFFOLD]
    groups.append((src.nids, "c++", PORT_FLAGS))
    groups += [(os.path.join(gen_ppu, f), "c++", PPU_FLAGS) for f in sorted(os.listdir(gen_ppu))
               if f.endswith(".cpp")]
    spu = sorted(os.path.join(gen_spu, d, "spu_recomp.c") for d in os.listdir(gen_spu)
                 if os.path.isfile(os.path.join(gen_spu, d, "spu_recomp.c")))
    spu += [os.path.join(gen_spu, f) for f in ("spu_workloads.c", "jobmods_register.c", "taskset_policy_bytes.c")
            if os.path.exists(os.path.join(gen_spu, f))]
    spu.append(os.path.join(src.ps3, "runtime", "spu", "spu_dispatch_mt.c"))
    groups += [(s, "cc", SPU_FLAGS) for s in spu]
    jobs = []
    for s, tool, flags in groups:
        flags = flags + cpu
        h = hashlib.sha1(common + tool.encode() + " ".join(flags).encode())
        _file_hash(h, s)
        rel = os.path.relpath(s, gen_dir) if s.startswith(gen_dir) else os.path.basename(s)
        stem = rel.replace(os.sep, "_").rsplit(".", 1)[0]
        obj = os.path.join(obj_dir, f"{stem}-{h.hexdigest()[:16]}.o")
        jobs.append((s, obj, tool, flags + SHIP_FLAGS + inc))
    return jobs


def _compile_all(zig, jobs, platform, progress=None, log=None):
    """jobs: game_jobs(); the biggest first (a recompiled chunk takes minutes).
    progress(done, total, fraction by size). Returns how many compiled."""
    todo = sorted((j for j in jobs if not os.path.exists(j[1])), key=lambda j: -os.path.getsize(j[0]))
    done, total, errors = 0, len(todo), []
    size_done, size_total = 0, sum(os.path.getsize(j[0]) for j in todo) or 1
    n = jobs_for_memory()

    def one(job):
        s, obj, tool, flags = job
        tmp = obj + ".part"
        r = _run([zig, tool, "-target", TARGETS[platform], "-c", *flags, s, "-o", tmp],
                 capture_output=True, text=True)
        if r.returncode == 0:
            os.replace(tmp, obj)
        return s, r.returncode, r.stderr

    if log and len(todo) < len(jobs):
        log(f"{len(jobs) - len(todo)} of {len(jobs)} files unchanged since the last build: reused")
    if log and todo:
        log(f"compiling {len(todo)} files, {n} at a time")
    with cf.ThreadPoolExecutor(n) as ex:
        for fut in cf.as_completed([ex.submit(one, j) for j in todo]):
            s, rc, err = fut.result()
            done += 1
            size_done += os.path.getsize(s)
            if log:
                log(f"{'FAILED' if rc else 'compiled'} {os.path.basename(s)} ({done} of {total})")
            if rc:
                errors.append(f"{os.path.basename(s)}:\n{err[-2000:]}")
            if progress:
                progress(done, total, size_done / size_total)
    if errors:
        raise BuildError("compiling failed:\n" + "\n".join(errors[:3]))
    return total


def link_args(lib_dir, platform=HOST):
    """The prebuilt libraries and system libraries the game links."""
    glslang = [os.path.join(lib_dir, n) for n in ("libglslang.a", "libSPIRV.a", "libglslang-default-resource-limits.a",
                                                   "libMachineIndependent.a", "libGenericCodeGen.a",
                                                   "libOSDependent.a")
               if os.path.exists(os.path.join(lib_dir, n))]
    rt = [os.path.join(lib_dir, NATIVE_LIB), os.path.join(lib_dir, RUNTIME_LIB)]
    if platform == "windows":
        res = [os.path.join(lib_dir, "dotheroes.res")] if os.path.exists(os.path.join(lib_dir, "dotheroes.res")) else []
        return res + rt + glslang + [os.path.join(lib_dir, "libSDL2.dll.a"),
                                     "-lws2_32", "-lole32", "-lbcrypt", "-ldbghelp", "-luser32", "-lgdi32",
                                     "-lwinmm", "-lshell32", "-Wl,--subsystem,windows", "-Wl,--stack,8388608"]
    return rt + glslang + [os.path.join(lib_dir, "libSDL2-2.0.so.0"), "-lpthread", "-ldl", "-lm",
                           "-Wl,-rpath,$ORIGIN/lib"]


def build_game(gen_dir, out_exe, cache_dir, zig=None, platform=HOST, progress=None, log=None):
    """Compile what the player's disc became (gen_dir) with the port's sources and
    link it with the prebuilt runtime. progress(fraction, message) for the whole
    build. cache_dir keeps the objects (fingerprinted): an update recompiles only
    what changed, often nothing, and relinks."""
    zig = zig or find_zig()
    src = Sources(platform)
    lib = os.path.join(src.lib_dir, RUNTIME_LIB)
    if not os.path.exists(lib):
        raise BuildError(f"the runtime library is missing: {lib}")
    os.makedirs(cache_dir, exist_ok=True)
    jobs = game_jobs(src, gen_dir, cache_dir, zig, platform)
    timings_path = os.path.join(cache_dir, "timings.json")
    try:
        timings = json.load(open(timings_path))
    except (OSError, ValueError):
        timings = {}
    per_file, link_est = float(timings.get("per_file", 20.0)), float(timings.get("link", 30.0))
    todo = sum(1 for j in jobs if not os.path.exists(j[1]))
    est = todo * per_file
    share = est / (est + link_est) if est else 0.0

    def report(frac, msg):
        if progress:
            progress(min(max(frac, 0.0), 1.0), msg)

    t0 = time.monotonic()
    compiled = _compile_all(zig, jobs, platform, lambda d, t, f: report(share * f, f"Compiling ({d} of {t})"), log)
    if compiled:
        timings["per_file"] = (time.monotonic() - t0) / compiled
    used = {os.path.basename(j[1]) for j in jobs}
    for f in os.listdir(cache_dir):  # objects this build no longer uses
        if f.endswith(".o") and f not in used:
            os.remove(os.path.join(cache_dir, f))
    if log:
        log(f"linking {os.path.basename(out_exe)}...")
    cmd = [zig, "c++", "-target", TARGETS[platform], "-o", out_exe + ".part", *[j[1] for j in jobs],
           *link_args(src.lib_dir, platform), "-s"]
    t1 = time.monotonic()
    kw = {"creationflags": 0x08000000} if os.name == "nt" else {}
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, **kw)
    while True:
        try:
            _, err = p.communicate(timeout=0.25)
            break
        except subprocess.TimeoutExpired:  # creep towards the end, never quite reaching it
            report(share + (1 - share) * min(0.97, (time.monotonic() - t1) / link_est), "Linking")
    if p.returncode:
        raise BuildError("linking failed:\n" + err[-3000:])
    os.replace(out_exe + ".part", out_exe)
    timings["link"] = time.monotonic() - t1
    try:
        json.dump(timings, open(timings_path, "w"))
    except OSError:
        pass
    report(1.0, "Linking")
    if log:
        log(f"linked: {out_exe} ({os.path.getsize(out_exe) >> 20} MB)")
    return out_exe


def link_fingerprint(platform=HOST, zig=None):
    """what the game exe depends on besides its objects: the prebuilt libraries and the linker"""
    src = Sources(platform)
    h = hashlib.sha1((_zig_version(zig or find_zig()) + TARGETS[platform]).encode())
    for x in link_args(src.lib_dir, platform):
        if os.path.isfile(x):
            h.update(os.path.basename(x).encode() + b"\0")
            _file_hash(h, x)
        else:
            h.update(x.encode() + b"\0")
    return h.hexdigest()


# ---------------- developer side: the prebuilt parts ----------------

def cmake_flags(flags):
    """flags as one CMAKE_<LANG>_FLAGS string: CMake splits it at spaces, so a
    flag with a space in it (a checkout path) is quoted"""
    return " ".join(f'"{f}"' if " " in f else f for f in flags)


def build_prebuilt(platform, ps3recomp, gen_ppu, log=print):
    """build/lib/<platform>/: the runtime library (CMake + zig, release settings:
    no dev diagnostics, not strict by default), its HLE NID table source, and the
    third-party libraries from tools/build_deps.py. Game-independent: the CMake
    configure needs a lifted ppu_recomp.h to exist, but only the runtime target
    and the NID table are built."""
    deps = os.path.join(ROOT, "build", "deps", platform)
    out = os.path.join(ROOT, "build", "lib", platform)
    work = os.path.join(ROOT, "build", "rel-" + platform)
    if not os.path.isdir(os.path.join(deps, "lib")):
        raise BuildError(f"run tools/build_deps.py --platform {platform} first")
    tc = cmake_toolchain(platform, cpu=PORTABLE_CPU)
    subprocess.check_call(["cmake", "-S", os.path.join(ROOT, "port"), "-B", work, "-G", "Ninja",
                           f"-DCMAKE_TOOLCHAIN_FILE={tc}", "-DCMAKE_BUILD_TYPE=Release",
                           f"-DPS3RECOMP_DIR={ps3recomp}", f"-DRECOMP_DIR={gen_ppu}",
                           "-DPS3RECOMP_RSX_VULKAN=ON", "-DDOTHEROES_DEV_DIAGNOSTICS=OFF",
                           f"-DPS3RECOMP_VULKAN_INCLUDE_DIR={os.path.join(deps, 'include')}",
                           f"-DCMAKE_PREFIX_PATH={deps}", f"-DCMAKE_FIND_ROOT_PATH={deps}",
                           "-DCMAKE_C_FLAGS=" + cmake_flags(SHIP_FLAGS), "-DCMAKE_CXX_FLAGS=" + cmake_flags(SHIP_FLAGS)],
                          stdout=subprocess.DEVNULL)
    nids = os.path.join(work, "ppu_hle_nids.cpp")
    subprocess.check_call(["cmake", "--build", work, "--target", "ps3recomp_runtime", "dotheroes_native", nids,
                           "--parallel", "6"], stdout=subprocess.DEVNULL)
    os.makedirs(os.path.join(out, "src"), exist_ok=True)
    shutil.copy2(os.path.join(work, "ps3recomp_sdk", RUNTIME_LIB), out)
    shutil.copy2(os.path.join(work, NATIVE_LIB), out)
    shutil.copy2(nids, os.path.join(out, "src"))
    for f in os.listdir(os.path.join(deps, "lib")):
        if f.endswith(".a"):
            shutil.copy2(os.path.join(deps, "lib", f), out)
    if platform == "windows":
        shutil.copy2(os.path.join(deps, "bin", "SDL2.dll"), out)
        windows_resources(os.path.join(out, "dotheroes.res"), os.path.join(ROOT, "assets", "icon.png"))
    else:
        shutil.copy2(os.path.join(deps, "lib", "libSDL2-2.0.so.0"), out)
    log(f"prebuilt: {os.path.relpath(out, ROOT)}")
    return out


def windows_resources(out_res, icon_png, zig=None):
    """The game's Windows resources as a .res it links: the manifest
    (port/win/dotheroes.manifest: UTF-8 file names, long paths) and the icon,
    made from icon_png as a multi-size .ico (Pillow)."""
    from PIL import Image
    zig = zig or find_zig()
    out_dir = os.path.dirname(out_res)
    Image.open(icon_png).save(os.path.join(out_dir, "dotheroes.ico"),
                              sizes=[(256, 256), (64, 64), (48, 48), (32, 32), (16, 16)])
    shutil.copy2(os.path.join(ROOT, "port", "win", "dotheroes.manifest"), out_dir)
    rc = os.path.join(out_dir, "dotheroes.rc")
    with open(rc, "w") as f:
        f.write('1 ICON "dotheroes.ico"\n1 24 "dotheroes.manifest"\n')
    subprocess.check_call([zig, "rc", "/fo", out_res, rc], cwd=out_dir)
    for tmp in (rc, os.path.join(out_dir, "dotheroes.manifest"), os.path.join(out_dir, "dotheroes.ico")):
        os.remove(tmp)
    return out_res

#!/usr/bin/env python3
"""The launcher (M9.4; launcher/, docs/M9_LAUNCHER_PLAN.md section 14), built with the project's Zig:

    build/venv/Scripts/python.exe tools/build_launcher.py [--out build/launcher] [--debug]

Dear ImGui's sources (tools/build_deps.py fetches the pinned release: config/deps.sha256) are compiled into it with
its SDL2 and SDL_Renderer backends; it links the SDL2 the game ships. Beside it: SDL2's library and the licences
(Dear ImGui's MIT licence, SDL2's zlib licence). Windows: a GUI program (no console), the project's icon and
manifest (the game's .res from the prebuilt parts).
"""
import argparse
import concurrent.futures as cf
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import build_deps  # noqa: E402
import zig_build  # noqa: E402

LAUNCHER_SOURCES = ["main.cpp"]
IMGUI_SOURCES = ["imgui.cpp", "imgui_draw.cpp", "imgui_tables.cpp", "imgui_widgets.cpp",
                 "backends/imgui_impl_sdl2.cpp", "backends/imgui_impl_sdlrenderer2.cpp"]
EXE = "CubicHeroes" + zig_build.exe_suffix()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "launcher"))
    ap.add_argument("--debug", action="store_true")
    a = ap.parse_args()
    platform = zig_build.HOST
    zig = zig_build.find_zig()
    imgui = build_deps.fetch("imgui")
    deps = os.path.join(build_deps.DEPS, platform)
    lib = zig_build.Sources(platform).lib_dir
    obj = os.path.join(a.out, "obj")
    os.makedirs(obj, exist_ok=True)
    inc = ["-I", os.path.join(ROOT, "launcher"), "-I", imgui, "-I", os.path.join(imgui, "backends"),
           "-I", os.path.join(deps, "include", "SDL2"), "-I", os.path.join(deps, "include")]
    opt = ["-O0", "-g"] if a.debug else ["-O2"]
    import version   # M11.2c: the release's version, for --version and --status
    flags = ["-std=c++17", "-DSDL_MAIN_HANDLED", "-DIMGUI_DISABLE_OBSOLETE_FUNCTIONS", "-DUNICODE", "-D_UNICODE",
             f'-DCUBIC_VERSION="{version.VERSION}"',
             "-Wno-deprecated-declarations", *opt, *inc]
    # M11.3: the flags are part of what an object is made of (the version's define above all): objects of other flags
    # -- a new version, --debug -- are made again (the times alone kept a 0.1.0 launcher in a 0.2.0-beta.1 package)
    stamp = os.path.join(obj, "flags.txt")
    stamp_text = zig_build._zig_version(zig) + "\n" + "\n".join(flags) + "\n"
    if not os.path.exists(stamp) or open(stamp, encoding="utf-8").read() != stamp_text:
        for f in os.listdir(obj):
            if f.endswith(".o"):
                os.remove(os.path.join(obj, f))
        with open(stamp, "w", encoding="utf-8", newline="\n") as f:
            f.write(stamp_text)
    jobs = [(os.path.join(ROOT, "launcher", s), os.path.join(obj, "launcher_" + os.path.splitext(s)[0] + ".o"))
            for s in LAUNCHER_SOURCES]
    jobs += [(os.path.join(imgui, s), os.path.join(obj, os.path.basename(os.path.splitext(s)[0]) + ".o"))
             for s in IMGUI_SOURCES]

    def compile_one(job):
        src, out = job
        if os.path.exists(out) and os.path.getmtime(out) > max(os.path.getmtime(src), *(
                os.path.getmtime(os.path.join(ROOT, "launcher", h)) for h in ("json.h", "worker_client.h", "cli.h"))):
            return src, 0, ""
        # Linux: ImGui's SDL2 backend includes SDL_syswm.h, which includes X11's headers (SDL2 is built with its X11
        # backend); the system's headers after Zig's own, as tools/build_deps.py builds SDL2 (headers only, nothing
        # of them linked)
        host = ["-idirafter", "/usr/include"] if platform == "linux" and src.endswith("imgui_impl_sdl2.cpp") else []
        r = subprocess.run([zig, "c++", "-target", zig_build.TARGETS[platform], "-c", *flags, *host, src, "-o", out],
                           capture_output=True, text=True, stdin=subprocess.DEVNULL)
        return src, r.returncode, r.stderr

    failed = False
    with cf.ThreadPoolExecutor(min(8, os.cpu_count() or 4)) as ex:
        for src, rc, err in ex.map(compile_one, jobs):
            print(f"{'FAILED' if rc else 'ok'} {os.path.relpath(src, ROOT)}", flush=True)
            if rc:
                print(err[-6000:])
                failed = True
    if failed:
        sys.exit(1)
    exe = os.path.join(a.out, EXE)
    if platform == "windows":
        res = os.path.join(lib, "dotheroes.res")
        libs = ([res] if os.path.exists(res) else []) + [os.path.join(lib, "libSDL2.dll.a"), "-lole32", "-lshell32",
                                                          "-luuid", "-luser32", "-Wl,--subsystem,windows"]
    else:
        libs = [os.path.join(lib, "libSDL2-2.0.so.0"), "-lpthread", "-ldl", "-Wl,-rpath,$ORIGIN/lib"]
    r = subprocess.run([zig, "c++", "-target", zig_build.TARGETS[platform], "-o", exe, *[o for _, o in jobs], *libs] +
                       ([] if a.debug else ["-s"]), capture_output=True, text=True, stdin=subprocess.DEVNULL)
    if r.returncode:
        print(r.stderr[-6000:])
        sys.exit("linking failed")
    if platform == "windows":
        shutil.copy2(os.path.join(lib, "SDL2.dll"), a.out)
    else:
        os.makedirs(os.path.join(a.out, "lib"), exist_ok=True)
        shutil.copy2(os.path.join(lib, "libSDL2-2.0.so.0"), os.path.join(a.out, "lib"))
    lic = os.path.join(a.out, "licenses")
    os.makedirs(lic, exist_ok=True)
    shutil.copy2(os.path.join(imgui, "LICENSE.txt"), os.path.join(lic, "Dear-ImGui-LICENSE.txt"))
    sdl_src = build_deps.fetch("SDL2")
    for n in ("LICENSE.txt", "COPYING.txt"):
        if os.path.exists(os.path.join(sdl_src, n)):
            shutil.copy2(os.path.join(sdl_src, n), os.path.join(lic, "SDL2-LICENSE.txt"))
            break
    print(f"built {exe} ({os.path.getsize(exe) >> 10} KB)")


if __name__ == "__main__":
    main()

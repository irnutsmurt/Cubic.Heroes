# Cubic.Heroes on Linux: the packaging plan

**Status: plan (M11.2, 2026-11-17).** Windows comes first; this plan keeps M11.2's work usable on Linux and lists what
remains for the move to Linux hardware (the owner's Manjaro laptop). Nothing here is validated on Linux yet.

## 1. What ships

Two packages of the same files, both x86-64:

- **`Cubic.Heroes-<version>-x86_64.AppImage`**: one file, double-clicked or started from a terminal. Its contents are
  a read-only image mounted while it runs.
- **`Cubic.Heroes-<version>-linux-x86_64.tar.gz`**: the same files in a folder, for players and packagers who prefer
  a plain directory (and for distributions where FUSE is not available). Extracted, the folder is a portable
  installation root exactly as on Windows (docs/CLI.md section 1).

| In the package | What |
|---|---|
| `CubicHeroes` | the launcher and the public command line (docs/CLI.md): SDL2 and Dear ImGui, as on Windows (tools/build_launcher.py already builds it for Linux: `lib/libSDL2-2.0.so.0`, rpath `$ORIGIN/lib`) |
| `lib/` | SDL2, OpenAL Soft, FFmpeg's libraries (avcodec, avformat, avutil, swresample -- LGPL builds, dynamic, as on Windows; docs/LICENSE_AUDIT.md) |
| `worker/` | the worker, a PyInstaller onedir app built on Linux (`DotHeroes-Setup --worker`), with its config, tools and the toolchain (Zig) |
| `licenses/`, `LICENSE.txt`, `README.txt` | as on Windows |

Not shipped: the Vulkan loader (`libvulkan.so.1`) and the GPU's driver -- the system's own, as every native Vulkan
game uses them; the player's disc image and keys.

The game itself is built on the player's machine by the worker (its lift, the compile with the bundled Zig, linked
against the bundled libraries) into `builds/<id>/dotheroes` with an rpath to the installation's `lib/` copy -- as on
Windows, never in the package.

## 2. The installation root

Everything the application manages lives under one writable folder (docs/CLI.md section 1); the same code chooses it
on both systems (launcher/main.cpp `resolve_root`):

- **tarball**: the extracted folder itself, as on Windows;
- **AppImage**: the program runs from the image's read-only mount, so the default root is a **`Cubic.Heroes` folder
  beside the `.AppImage` file** (`$APPIMAGE` names the file; launcher/main.cpp `home_dir_of_program`). Moving the
  AppImage file and that folder together keeps working; `cubicheroes-root.txt` beside the `.AppImage` file, or
  `--install-dir` / `CUBIC_INSTALL_DIR`, choose another folder (for example `~/Games/Cubic.Heroes`);
- no default use of `~/.config` or `$XDG_DATA_HOME` (the old launcher's `~/.config/cubic-heroes` is only read, to
  offer an earlier installation for use or copying, as `%APPDATA%` is on Windows).

A root on a read-only filesystem, or one without the free space an operation needs, is refused with exit code 7 and
a message, before anything is written.

## 3. Building the packages (to do on Linux)

1. The launcher: `tools/build_launcher.py` on Linux (it already handles the platform: no `--subsystem`, the SDL2
   `.so` copied into `lib/`).
2. The worker: `tools/make_bundle.py` on Linux (PyInstaller onedir; the toolchain fetched for `x86_64-linux`).
3. FFmpeg and OpenAL Soft: the same sources and configure options as the Windows builds (docs/FFMPEG_MIGRATION_PLAN.md,
   docs/OPENAL_MIGRATION_PLAN.md), built for `x86_64-linux-gnu` against an old glibc (2.28 or so) so the package runs
   on current and older distributions; `ldd` of every shipped binary checked for nothing outside glibc, libstdc++
   (or a static libc++), libm, libdl, libpthread, libvulkan and the X11/Wayland libraries SDL2 loads itself.
4. **tarball**: the bundle folder tarred.
5. **AppImage**: the same folder as the AppDir (`AppRun` starting `CubicHeroes` with the arguments passed on, a
   `.desktop` file and the icon), packed with appimagetool (a tool of the build machine only, not shipped). The
   AppImage's own update information is not used: Cubic.Heroes's signed updates (docs/RELEASE_SIGNING.md) replace the
   package items in the installation root, never the image -- for the AppImage, an update means downloading the new
   `.AppImage` file (the launcher says so; the root beside it stays).

## 4. Third-party launchers

The command line is the same (docs/CLI.md section 6): `CubicHeroes --play` (or `Cubic.Heroes.AppImage --play`) stays
running for as long as the game does and exits with its status; the game is in the same process group, and
`SIGTERM` to `CubicHeroes` reaches it. Steam (a non-Steam game, with or without a compatibility tool: it is a
native program, so none), Lutris, Heroic and Playnite-like frontends start it the same way. `--status --json` and
`--install ... --json` give them the installation's state and a headless install.

## 5. Tests on Linux

The regression suite is Python and already platform-aware in most places:

- `tests/boot_smoke.py` and the drives: the program name without `.exe` (`zig_build.exe_suffix()`), the desktop
  guard's offscreen mode (`PS3RECOMP_VK_WINDOW=0`) unchanged;
- `tests/desktop_guard.py`: the "a window appeared" check needs an X11/Wayland equivalent of its Windows window
  enumeration (`wmctrl -l` or `xdotool` on X11; on Wayland the guard relies on the offscreen environment alone) --
  to write;
- the worker's unit tests (`tests/test_*.py`) run as they are;
- the M11.2 integration tests (docs/M11_PLAN.md section 32) with the AppImage's root rule added: a root beside the
  `.AppImage`, a moved AppImage, a read-only image directory.

## 6. Open points for the move

- glibc baseline and the build container for steps 3-5;
- the desktop guard's window check on Linux (section 5);
- FUSE-less systems: the tarball, or `--appimage-extract-and-run`;
- shortcuts: `tools/shortcuts.py` already writes XDG `.desktop` files and Steam's `shortcuts.vdf`; for the AppImage
  they must name the `.AppImage` file (`$APPIMAGE`), not the mounted program.

## 7. The glibc baseline, the build environment and security updates (M11.4, 2026-11-18)

**Where the baseline comes from.** Everything this project compiles for Linux -- the game (on the player's machine),
the launcher, the runtime library, SDL2, OpenAL Soft and FFmpeg -- is compiled by Zig for `x86_64-linux-gnu.2.31`
(tools/zig_build.py `TARGETS`): Zig carries glibc 2.31's symbol versions itself, so what it builds needs glibc 2.31
whatever machine built it. `tools/glibc_check.py` checks every shipped ELF file (GLIBC_2.31 at most, no GLIBCXX, only
glibc's or shipped libraries needed) in CI and in packaging.

**What the build environment (tools/linux_build_env, Debian 11) is still for**, and so what Debian 11's end of life
(2026) does and does not affect:
- *the frozen worker's Python*: CPython 3.12 and OpenSSL 3.5 (LTS) are built **from pinned upstream sources** in the
  image (the Dockerfile's `PYTHON_VERSION`/`OPENSSL_VERSION` and their SHA-256), not taken from Debian -- their
  security updates are ours to take, by raising those pins and rebuilding, whatever Debian does. What the image
  contributes is glibc 2.31 to build them against (so the frozen worker runs on 2.31).
- *headers only* for the run-time backends SDL2 and OpenAL Soft load with `dlopen` (X11, Wayland, PipeWire 0.3.65 from
  bullseye-backports, PulseAudio, ALSA): no code of them is shipped; the player's own system libraries are used.
- *mksquashfs*, linked statically with Debian 11's zlib and zstd: a tool that only writes the player's game AppImage
  from the player's own files (no untrusted input). To be independent of Debian's copies, zlib and zstd can be built
  from pinned sources as Python is (a follow-up).
- The Debian packages themselves are pinned to one day (snapshot.debian.org, 2026-08-01; backports from Debian's
  archive): the image is the same whenever it is rebuilt, but no further Debian update reaches it.

**Security updates of what is shipped** (the Linux packages; the Windows package's list is the same less the AppImage
pieces), each pinned in `config/deps.sha256` or the Dockerfile:

| Component | Shipped as | Exposure | Update path |
|---|---|---|---|
| CPython 3.12 | the worker's runtime | reads the player's disc and keys; HTTPS to GitHub for updates | raise `PYTHON_VERSION` (3.12 receives security fixes until 2028-10; then 3.13+) |
| OpenSSL 3.5 LTS | the worker's TLS and signature checks | HTTPS; Ed25519 verification of release manifests | raise `OPENSSL_VERSION` (3.5 LTS supported until 2030-04) |
| PyInstaller bootloader 6.x | starts the worker | none beyond startup | pin in the Dockerfile |
| pycryptodome | AES for the player's disc | local files only | pin in the Dockerfile |
| SDL2 2.32 | the window, input | the desktop, controllers | `tools/build_deps.py` SOURCES and its pin |
| OpenAL Soft 1.25 | audio | none (game data) | idem |
| FFmpeg 9.0 (Bink only) | movies | the game's own movie files | idem |
| AppImage runtime 20251108 (+libfuse, squashfuse, zstd, zlib, musl) | mounts the AppImages | the AppImage file itself | a newer release: pin, verify its signature (tools/fetch_appimage.py) |
| squashfs-tools 4.7 | mksquashfs | the player's own files | pin |

A release is preceded by a check of each against its upstream advisories (and the OSV database); a fix in any of
them is a new package version through the signed-update channel. A CI job that compares the pins with upstream
releases and OSV each week is proposed as a follow-up.

**Longer term: a maintained build environment with the same baseline (a proposal for the owner, not adopted).**
PyPA's `manylinux_2_28` image (AlmaLinux 8, security support until 2029; the Python packaging ecosystem's standard
environment for portable Linux binaries) would replace Debian 11 for the frozen worker and the headers. Building there
makes the worker need glibc **2.28** or newer -- lower than the 2.31 floor, so every system supported now stays
supported -- while everything Zig compiles keeps targeting 2.31 exactly as now. Nothing about the shipped baseline
changes; it is recorded here for the owner's decision (the owner, 2026-11-18: the glibc target is not changed without
discussion).

# Cubic.Heroes on Linux: the packages

**Status: built and qualified locally (M11.4, 2026-11-18; docs/M11_4_PLAN.md sections 18-19).** Both packages are
made in the pinned build environment and qualified from the packages themselves (tests/release_qualification.py);
nothing is published until the owner approves the qualification and signs the release manifest.

## 1. What ships

Two packages of the same files, both x86-64, both under the project's signed release manifest
(docs/RELEASE_SIGNING.md; each asset carries `platform` and `kind`):

- **`Cubic.Heroes-<version>-x86_64.AppImage`**: one file, started by a double click or from a terminal; a read-only
  image mounted while it runs (FUSE), or `--appimage-extract-and-run` where FUSE is not available (see "Without
  FUSE" below: the tarball is recommended there).
- **`Cubic.Heroes-<version>-linux-x86_64.tar.xz`**: the same files in one folder, `Cubic.Heroes`, which is a portable
  installation root exactly as on Windows (docs/CLI.md section 1) -- for systems without FUSE, for packagers, and for
  players who prefer a plain folder.

The AppImage is made from the tarball's folder and checked to hold it file for file (bytes and executable bits).

**Without FUSE, use the tarball.** The pinned AppImage runtime's `--appimage-extract-and-run` (and
`APPIMAGE_EXTRACT_AND_RUN=1`) extracts the whole image to `$TMPDIR/appimage_extracted_<hash>` (`/tmp` when TMPDIR is
not set) and **does not remove it** when the program exits: about 450 MB for the launcher's AppImage, about 110 MB for
a game AppImage, one copy per AppImage file (the same file reuses its copy; each new version leaves another). On a
system whose `/tmp` is in memory (tmpfs), that is memory, then swap, until the next reboot or until the folder is
deleted. Normal FUSE runs and the tarball leave nothing behind. The cause is in the runtime, not in Cubic.Heroes
(docs/M11_4_PLAN.md section 31): its cleanup calls `nftw()` with a descriptor limit of 0, which the musl C library it
is built with treats as "nothing to do". Cubic.Heroes keeps the official runtime unmodified and adds no cleanup of its
own; the runtime is reevaluated at each dependency update (section 7).

| In the package | What |
|---|---|
| `CubicHeroes` | the launcher and the public command line (docs/CLI.md); SDL2 and Dear ImGui; RUNPATH `$ORIGIN/lib` |
| `lib/` | SDL2 (the launcher's) |
| `worker/` | the worker (PyInstaller onedir, Python 3.12 and OpenSSL 3.5 built for the baseline), its tools and config, Zig (trimmed), the prebuilt runtime library and the game's libraries (SDL2, OpenAL Soft with its PipeWire/PulseAudio/ALSA backends, FFmpeg's Bink decoder), `toolchain/appimage/` (the AppImage runtime and mksquashfs, for the player's game AppImage), `licenses/` with every licence and the LGPL/GPL sources |
| `licenses/`, `LICENSE.txt`, `README.txt` | as on Windows |

Not shipped: the Vulkan loader and the GPU's driver (the system's), the player's disc image and keys, and the game
itself -- built on the player's machine from their own disc into `builds/<id>/dotheroes`.

## 2. The installation root

Everything the application manages lives under one writable folder (docs/CLI.md section 1), chosen by the same code
on both systems (launcher/main.cpp `resolve_root`):

- **tarball**: the extracted folder itself, as on Windows;
- **AppImage**: a **`Cubic.Heroes` folder beside the `.AppImage` file** (`$APPIMAGE`); moving the file and that folder
  together keeps working; `cubicheroes-root.txt` beside the file, `--install-dir` or `CUBIC_INSTALL_DIR` choose another;
- nothing in `~/.config` or `~/.local/share` by default: only the menu entries, the desktop shortcut and their icon
  (`~/.local/share/applications`, the desktop, `~/.local/share/icons/hicolor/256x256/apps/cubicheroes.png`) when the
  player asks for shortcuts; uninstall removes those it made.

A root that can't be written is refused with exit code 7 before anything is written.

**The player's game AppImage** (the owner's D2): after every install, update and rollback the worker writes
`<root>/exports/Cubic.Heroes-Game-x86_64.AppImage` -- the active build's program and libraries only (the data, saves,
settings and caches stay in the root). It finds its root through `CUBIC_INSTALL_DIR`, a `cubicheroes-root.txt`
beside it, the folder above `exports/` (so the root and it move together), or the root it was made in, and plays as
`CubicHeroes --play` does. It is made offline with the runtime and mksquashfs the worker ships, and it is never
distributed by the project.

## 3. Building the packages

    docker build -t cubic-heroes-linux-build tools/linux_build_env   # once: the pinned environment (section 7)
    python3 tools/linux_package.py                                    # everything, below

tools/linux_package.py: the pinned Zig; the AppImage runtime and the sources it and mksquashfs come from, verified
(tools/fetch_appimage.py: SHA-256 pinned in config/deps.sha256, the runtime's GPG signature against the pinned key);
then, in the container with the checkout at `/src`, from clean: the libraries against the baseline's headers, a static
mksquashfs, the runtime library, the worker frozen and the launcher (tools/make_bundle.py), the tarball, and the
AppImage (tools/make_appimage.py). Each package goes through the boundary check, the privacy scan and
tools/glibc_check.py (GLIBC_2.31 at most for every ELF file in it).

## 4. Third-party launchers

The command line is the same as on Windows (docs/CLI.md section 6): `CubicHeroes --play` (or `<file>.AppImage --play`)
runs for as long as the game does and exits with its status; `SIGTERM` or `SIGINT` to it reaches the game (exit 6);
if it is killed outright the game ends with it (`PR_SET_PDEATHSIG`). Steam (a non-Steam game: it is native, no
compatibility tool), Lutris and Heroic start it the same way; `--status --json` and `--install ... --json` give them
the state and a headless install.

**Wayland and X11**: SDL2's default on a Wayland session is X11 through XWayland; native Wayland is used with
`SDL_VIDEODRIVER=wayland` (verified: the swapchain, the display's refresh). Known limits: exclusive fullscreen is
desktop fullscreen on Wayland (SDL2's behaviour there: the compositor owns the mode; the game never changes the
desktop's resolution) -- expected, to be verified with the window modes under P8.

## 5. Tests on Linux

- the desktop guard (tests/desktop_guard.py): no display reachable unless an on-screen run is approved; a connection
  to an X11 or Wayland server by the run's process tree ends it;
- the unit tests (tests/test_*.py), CI's Linux job (.github/workflows/ci.yml);
- tests/release_qualification.py from the tarball and from the AppImage (an AppImage's update is a new `.AppImage`
  file; it is never unpacked into the root);
- tests/distro_check.py: the shipped files in clean containers of Debian 11, Ubuntu 20.04 and 24.04, Fedora 41;
- tests/fs_case_check.py: failed opens against the disc's names without regard to case.

## 6. Open points

Tracked in docs/M11_4_PLAN.md's acceptance checklist (section 8).

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
| AppImage runtime 20251108 (+libfuse, squashfuse, zstd, zlib, musl) | mounts the AppImages | the AppImage file itself | a newer release: pin, verify its signature (tools/fetch_appimage.py); check whether its extract-and-run removes its copy (the `nftw` descriptor limit: section 1, "Without FUSE") |
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

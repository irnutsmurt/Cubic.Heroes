# M11.4: the native Linux port and the first AMD validation -- plan and checkpoint

Begun 2026-11-18 on the owner's Linux laptop. Windows 0.2.0-beta.2 is the functional baseline: this is a platform
port of the existing implementation, not a rewrite. Linux support is additive -- the same repository, source tree
and `main` branch build and package both systems, and nothing here may break, replace or slow the Windows build.
Evidence marks as in the other plans: [C] read in the code, [M] measured, [I] inferred, [H] a hypothesis to check.

## Checkpoint (kept current; the newest state first)

- **Stage** (2026-11-18): **P2 -- the first native Linux build boots and plays offscreen** (section 11): the title,
  the attract demo with its movies, the overworld, the desert and a dungeon drawn by the native renderer on RADV;
  `--memtest` 628/628 on Linux. **D1 done** (section 12): the Linux build in Windows' release configuration boots
  strict-clean with no PS3 SPU machinery used. **P6 (CI) written** and the Windows build checked from
  this tree by a cross-build (section 14). **P4 begun** (section 13): the launcher and a
  full headless player install on Linux, the `--play` signal contract, the updater's Linux packages.
- **Blocked on the owner**: pushing runtime patches 0170+ -- `.git/push-guard.local` (written this session) lists the
  commit identity's name, which every patch's `From:` header carries (asked 2026-11-18).
- P1 done -- the desktop guard enforced on Linux (section 9); the unit tests run on Linux
  for the first time (section 10: one real Linux bug fixed, two Windows-only tests given their Linux case). P2 next.
- P0 done (onboarding, safeguards verified on Linux); this plan written.
- **Decisions**: D1-D4 approved by the owner on 2026-11-18 as recommended, with their additions (section 4); Windows
  and Linux CI approved. Correctness first, then AMD profiling and optimisation.

## 1. The machine [M]

| | |
|---|---|
| CPU | AMD Ryzen 5 5500U (Zen 2, 6 cores / 12 threads, up to 4.06 GHz) |
| GPU | AMD Radeon integrated (Renoir / Lucienne, GCN 5 "Vega", 1002:164c), shared memory |
| Driver | Mesa 26.2.4, RADV (Vulkan 1.4.354, conformance 1.4.5.3); kernel driver amdgpu |
| Vulkan | loader 1.4.363; layers: Khronos validation 1.4.363, Mesa device-select and anti-lag, Steam's fossilize and overlay |
| Memory | 17 GiB, much of it in use by the desktop's own programs (swap active) -- the lift and compile must be watched |
| System | Manjaro (Arch-based), Linux 6.18, glibc 2.44 |
| Session | KDE Plasma on Wayland, XWayland available (`DISPLAY` set) |
| Tools present | GCC 16, Clang/LLD 23, CMake 4.4, Ninja, NASM, gdb, perf, Xvfb, glslang, SPIRV-Tools, patchelf, Docker; system SDL2 2.32, OpenAL Soft 1.25.2, FFmpeg 9.0, X11/Wayland/xkbcommon/libdecor/udev headers |
| Tools absent | appimagetool, strace, valgrind, the `py` launcher (`python3` instead); Zig 0.17.0 placed in `build/toolchain/` (sha256 checked against ziglang.org's index) |

This is the hardware class the project began on (docs/HANDOFF.md: "Ryzen 4000 / Radeon integrated, RADV"). Then, on
the pre-native renderer, gameplay ran at 20-31 fps, GPU-bound at about 22 ms a frame. The native renderer (R1-R8)
has never run here. **60 fps on this integrated GPU is a target to measure, not an assumption**; every figure will be
reported as measured.

## 2. Safeguards (P0) [M]

| Item | State |
|---|---|
| `attribution.sessionUrl: false` and the commit trailer, user settings and `.claude/settings.json` | set and verified (the session's attribution changed to the plain trailer); `.claude/settings.json` was never tracked though CLAUDE.md names it -- now committed |
| `core.hooksPath tools/githooks` | set |
| `tools/githooks/pre-push` | **was mode 100644 in git: on Linux git silently skips a non-executable hook**, so the guard never ran on a Linux clone. Fixed (100755) |
| `.git/push-guard.local` | created (private strings blocked, the noreply identity and the repository names allowed) |
| push guard | planted session link, session trailer, personal address, home path and account name each caught; clean text passes |
| boundary check | `tree` passes on Linux |
| the hook through git itself | a scratch clone pushing to a local bare repository: a commit with a session trailer refused, the clean one accepted |
| commit identity | the project's noreply identity (all 339 earlier commits use it) |

## 3. The audit: what exists and what is missing [C]

Linux was scaffolded through M2-M11 but has not been **built** since the native port began (docs/NATIVE_R8.md,
docs/FEASIBILITY.md), nor run since 2026-10-30.

### 3.1 The runtime (`port/`, `patches/ps3recomp/`)
- **Compile blockers**: `GetWriteWatch` unguarded (port/native/render/ppfx_render.cpp `ww_poll`); `_putenv_s`
  unguarded (port/native/render/ppfx_recorder.cpp `r7_defaults`).
- **Configuration divergence**: the release on Windows is mov stores with no SPU machinery linked; on Linux CMake
  forces checked stores with the lifted SPU programs (port/CMakeLists.txt, tools/zig_build.py `release_cmake`) -- a
  different, unvalidated configuration carrying more PS3 machinery (decision D1).
- Present and plausible, never run: the guest arena (`mmap` + `MAP_NORESERVE`), the dev builds' crash and hang
  diagnostics (sigaction, `backtrace`, the thread dump), SDL2 windowing, FFmpeg and OpenAL loaded from `lib/`.
- **Missing on Linux**: the release crash record (Windows `SetUnhandledExceptionFilter` only); the display's refresh
  rate (falls back to 60 Hz); HiDPI (no `SDL_WINDOW_ALLOW_HIGHDPI`); the R5 write watch (re-hashing every frame
  instead -- a performance gap); `vm_watch`; host thread IDs (forcing the recorder's locked path); the flip-wait
  sleep path; CPU placement; "Return to Launcher" relaunching the launcher; OpenAL's device-change events (the Linux
  OpenAL build is SDL2-backend only); thread names and priorities; some dev tools.
- **To review**: the SPU pool's stack-range forget is Windows-only while Linux runs with coherency on (patch 0045);
  file-name case (the disc's Joliet names against what the game asks for: hidden by Windows, fatal on Linux); where
  a build's `lib/` lives relative to `builds/<id>/`.

### 3.2 The launcher, the worker and the tools
- **`--play` contract broken on Linux**: `signal()` restarts `waitpid`, so `SIGTERM` to `CubicHeroes` never reaches
  the game (launcher/cli.h); no `PR_SET_PDEATHSIG`, so a killed launcher orphans the game (Windows: a job object).
- **Updates**: the Linux package is `.tar.xz` but `stage_launcher` unpacks zip only (and zip drops exec bits); the
  architecture is hard-coded; the first matching asset is taken (an AppImage and a tarball in one release would be
  ambiguous); in an AppImage an update must not write into the mount.
- **The frozen worker** likely passes PyInstaller's `LD_LIBRARY_PATH` to every child (the game, Steam, `gio`) [H].
- Shortcuts and uninstall hard-code `CubicHeroes.exe`; `.desktop` files are never removed; an AppImage's shortcuts
  must name `$APPIMAGE`.
- Packaging: no AppImage builder; `make_bundle --platform all` cannot finish on one host; `build_ffmpeg.py` uses any
  `zig` on PATH on Linux; Zig itself is fetched by no tool; the worker frozen on this machine would need glibc 2.44.
- Present and plausible: `resolve_root` with `$APPIMAGE`, the worker spawn (fork, `setsid`), `/proc` checks, rpath
  `$ORIGIN/lib`, the release manifest's `linux-x86_64` platform, `libcrypto.so` for verification.

### 3.3 Tests
- **The desktop guard does nothing on Linux**: no window detection, nothing killed; only `boot_smoke.py` strips
  `DISPLAY`/`WAYLAND_DISPLAY`. No game or launcher run starts on this machine until P1 fixes it.
- Windows-only runners: `cli_integration.py` (`tasklist`, `TerminateProcess`), `release_qualification.py` (`.exe`
  names, zip, an `F:` default), `window_place_check.py`, `rpcs3_capture.py`, `nsys_capture.py`,
  `tests/native/run_gcm_tests.sh`, `inflate_test.c`; `boot_smoke.py`'s one-game check and memory sampler.
- Unit tests: stdlib `unittest`, mostly platform-independent; Linux branches of the updater swap, the legacy-root
  finder and `owned_run` are untested.

### 3.4 Release and CI
- **No CI exists** (`.github/` holds issue templates only).
- The signed manifest already allows `windows-x86_64` and `linux-x86_64` assets under one version
  (tools/release_trust.py); the signing scheme (Ed25519 through OpenSSL) is unchanged by this milestone.
- docs/LICENSE_AUDIT.md is stale (577 rows; 832 files tracked).

## 4. The owner's decisions (asked and approved 2026-11-18)

**Approved as recommended, with these additions (the owner's):**
- D1: Linux uses the same native runtime architecture as Windows. Checked stores may remain a developer-only
  diagnostic fallback; lifted SPU programs are never linked into the release.
- D2: the thin game AppImage stays relocatable (relative paths wherever possible) and is regenerated automatically
  when the active build changes by an update or a rollback.
- D3: the owner provides the local keys file's path. Its contents never appear in logs, diagnostics, commits or
  release artifacts.
- D4: validated stages are pushed with every safeguard on; changes to shared runtime code are checked for Windows
  regressions.
- Windows and Linux CI approved. The glibc baseline is verified on actual supported distributions, not only Manjaro.
  Correctness before performance: profiling and AMD optimisation once the game runs correctly here.

The questions as asked:

- **D1 -- the Linux release configuration.** Recommended: **the same as Windows** (mov stores, no lifted SPU
  programs linked, cellAudio retired), with Linux's own fault handling where the Windows path uses Windows
  facilities; checked stores kept only as a development fallback. Reason: the architecture requirement ("no
  unnecessary PS3 machinery") and parity with the validated configuration.
- **D2 -- the generated game AppImage.** Recommended: **a thin AppImage** made by the worker from the active build:
  the game's program and its `lib/` (tens of MB), whose AppRun finds its installation root (a pointer written beside
  it, as `cubicheroes-root.txt` is for the launcher) and plays through it -- the converted data, saves, settings and
  caches stay in the root, never copied. Remade when the active build changes; never distributed. The alternative
  (a self-contained AppImage holding the converted game data, several GB) duplicates the root.
- **D3 -- the keys file.** The disc image here matches `config/game_version.json` and a decrypted `EBOOT.elf` is
  present; the qualification's keys checks (docs/RELEASE_QUALIFICATION.md item 11) need the keys file. Is it on this
  machine?
- **D4 -- pushing.** Recommended: push each validated stage to `main` as M11.0-M11.3 did (hooks on, every message
  read first), never a release.

## 5. Design

### 5.1 Platform code
Platform differences stay behind the interfaces that already hold them -- `win32_compat.h` in the runtime,
`install_layout.py`/`zig_build.py` in the tools, `#ifdef _WIN32` blocks within the few platform files of the
launcher -- never scattered through game logic. New Linux code is added beside the Windows code, never in place of
it. Shared changes (a renderer fix found on AMD) are made once for both and validated on both.

### 5.2 Packages and the root
- `Cubic.Heroes-<version>-linux-x86_64.tar.xz` and `Cubic.Heroes-<version>-x86_64.AppImage`: the same files.
  docs/LINUX_PACKAGING.md's `.tar.gz` becomes `.tar.xz` (what make_bundle and the signed manifest already use).
- The root: the tarball's folder; the AppImage's `Cubic.Heroes` folder beside the `.AppImage` (already in
  `resolve_root`). Nothing in `~/.config` or `~/.local/share` unless asked (shortcuts).
- The manifest gains an explicit `platform` and package `kind` (`zip`, `tar.xz`, `appimage`) per asset; the updater
  picks by OS, `platform.machine()` and the kind it was installed from, and refuses an asset of another platform. An
  AppImage installation is told to download the new `.AppImage` (the image is never written); the tarball updates in
  place as Windows does.
- **The game AppImage** (D2): `builds/<id>/` stays the build slot; the worker writes
  `exports/Cubic.Heroes-Game-x86_64.AppImage` (or a path the player names) from the active build after a successful
  link, with the pointer to its root. A boundary-check rule refuses it anywhere publishable.

### 5.3 The reproducible build environment and glibc baseline
- **Baseline: glibc 2.31** (Debian 11, Ubuntu 20.04, and every SteamOS 3): the game and the libraries are already
  compiled for `x86_64-linux-gnu.2.31` by Zig.
- The launcher and the frozen worker (PyInstaller embeds the build machine's Python, so it must be built on the
  baseline) are built in a pinned container (`tools/linux_build_env/`, a Debian 11 image by digest, Python and the
  build tools at fixed versions). The same container builds SDL2 and OpenAL Soft against its own headers rather than
  the host's.
- Checked by `objdump -T`: no shipped binary needs a symbol newer than GLIBC_2.31 / GLIBCXX of the baseline; every
  shipped `.so`'s `DT_NEEDED` is in an allow-list (glibc, libstdc++/libgcc_s or static, libvulkan at run time,
  X11/Wayland/PipeWire/Pulse libraries loaded by SDL2/OpenAL with `dlopen`).
- Run on at least one distribution besides Manjaro: a Debian 11 and a current Ubuntu or Fedora container running
  the CLI headless (`--status`, `--install` to the offline unpack), and the game offscreen under lavapipe where it
  can.

### 5.4 CI (GitHub Actions)
`windows-latest` and `ubuntu` jobs (the Linux job inside the baseline container): the unit tests; the settings and CLI
schema checks; the launcher built; the runtime library built with Zig from the pinned ps3recomp plus this
project's patches (no game data: the lift is not run); the dependency builds against `config/deps.sha256`; the
boundary check and the push guard over the tree; a packaging dry run per platform. No game data, keys, generated
code or signing key in CI. **CI proves compilation, not behaviour**: the Windows game's behaviour is proved only on
the owner's Windows PC (section 7).

## 6. The stages

| Stage | Content | Exit evidence |
|---|---|---|
| **P0** | onboarding, safeguards | section 2 |
| **P1** | the desktop guard on Linux: no display variables unless approved (game, launcher, every runner); an X11 watcher (`_NET_CLIENT_LIST` + `_NET_WM_PID`) and, on KDE Wayland, KWin's window list over D-Bus; `py -3`/`Scripts/` paths made per-platform; its unit tests | a game started without approval has no display; a deliberately opened window under approval is seen on X11 and on Wayland and ends a non-approved run |
| **P2** | the first native dev build: the pipeline (lift from the disc and `EBOOT.elf`), the two compile blockers, the runtime and the game linked, booted offscreen (`boot_smoke.py`) | a Linux dev build's boot to the title and into play offscreen, its log clean; Windows CI build green |
| **P3** | the release configuration (D1), the release build path (`zig_build.build_prebuilt` for Linux), the release crash record, refresh rate, HiDPI, OpenAL with PipeWire/Pulse/ALSA backends and device events, Return to Launcher, file-name case check, the SPU forget-range review, the write watch and thread IDs on Linux | a release build boots, plays and ends in order; a forced crash writes a report; a device unplugged mid-play recovers |
| **P4** | the launcher, CLI and worker: `SIGTERM`/`SIGINT`/`PDEATHSIG`, the frozen worker's environment, shortcuts and `.desktop` files ($APPIMAGE aware), uninstall, the updater's tar.xz and AppImage paths, asset selection | `tests/cli_integration.py` (Linux branches) 17/17 and the no-game cases; updater unit tests on Linux |
| **P5** | packaging: the baseline container, deps, the worker frozen, the tarball, the launcher AppImage, the game AppImage (D2), licences, boundary and privacy checks of each | both packages built reproducibly; glibc/`DT_NEEDED` checks pass; they run on Manjaro and in the other distributions' containers |
| **P6** | CI for Windows and Linux (5.4) | both jobs green on `main` |
| **P7** | AMD validation and performance: shader preparation, validation layers, the RPCS3 reference scenes and Windows reference pictures, water/shadows/lighting/reflections/cube maps/post-processing, resolution scaling, GPU memory, frame pacing, load times, CPU/GPU limits | measured tables (fps, p99, spikes, VRAM, load times) per preset; any rendering fix validated on both systems |
| **P8** | the regression suite on Linux: the map sweep (166 maps), the bosses, the devtools suite, audio and movies, saves, controllers and remapping, settings, X11 and Wayland, window modes, clean exit, crash and hang reports | the matrix in section 8 |
| **P9** | release qualification from the packaged AppImage and tarball (the M11.3 checklist, Linux edition); the cross-platform report; README and docs | the owner's review and signature; nothing published before |

## 7. Windows: kept working throughout

- Every commit that touches shared code: the Windows CI job (compile and unit tests), and the change reviewed for
  its Windows path.
- **Compiled here** for every stage that changes shared code: the Windows game cross-built from the same tree
  (section 14), and CI's Windows job once pushed.
- **Needs the owner's Windows PC** (a list kept current here, asked for at each stage that changes shared runtime
  code): the Windows release build from the same commit, `tests/devtools_suite.py` basic set, `tests/cli_integration.py
  --with-game`, the map sweep, the RPCS3 reference scenes, and an update from the 0.2.0-beta.2 package to the next
  (the manifest's new fields read by an older launcher).

## 8. Acceptance checklist

States: **VERIFIED** (implemented, evidence named), **UNVERIFIED** (implemented, not yet shown), **PARTIAL**,
**NOT DONE**, **BLOCKED** (on what). Every row needs its evidence before it reads VERIFIED.

| # | Criterion | State | Evidence |
|---|---|---|---|
| A1 | Privacy configuration and git safeguards active on Linux | VERIFIED | section 2 |
| A2 | The desktop guard enforced on Linux (X11 and Wayland) | VERIFIED (mechanism); runners' Linux kill paths: P4 | section 9 |
| A3 | Native Linux dev build boots and plays (offscreen) | PARTIAL: boots to the title and the attract demo, movies decoded; play from a save not yet | section 11 |
| A4 | Linux release configuration as Windows' (D1: no lifted SPU in the release) | VERIFIED at boot (--memtest 601/601; the census all zero); longer play: P8 | section 12 |
| A5 | Release crash record and hang report on Linux | PARTIAL: the release crash record and the developer crash report and thread dump verified; a real hang's detection (shared code): P8 | section 15 |
| A6 | Audio through OpenAL Soft with the system's server; device change recovered | PARTIAL: PipeWire output with events and reopen; the audio suite (device losses included) 230/230; a real default-device change: P8 | section 15 |
| A7 | Cinematics through FFmpeg | NOT DONE | P2/P8 |
| A8 | X11 and Wayland: windowed, borderless, fullscreen; the desktop's mode never changed | PARTIAL: windowed on X11 (XWayland) and native Wayland (fixed: section 16); borderless, fullscreen, the mode: P8 | section 16 |
| A9 | Rendering resolution presets and custom; VSync; frame limits | NOT DONE | P8 |
| A10 | Controllers: detection, prompts, hot-plug, remapping; keyboard remapping | NOT DONE | P8 (hot-plug by hand) |
| A11 | The in-game overlay, performance overlays, developer tools, diagnostics and log export | NOT DONE | P8 |
| A12 | The public CLI and exit codes (docs/CLI.md), `SIGTERM` reaching the game | PARTIAL: exits 0/2/3/6/7 and the signals verified; tests/cli_integration.py on Linux: to do | section 13 |
| A13 | Graphical launcher: install from the disc, local recompilation, shader preparation | PARTIAL: headless install verified (2458 pipelines, none failed); the window: to do | section 13 |
| A14 | Signed updates, repair and rollback; the right package per OS and architecture | PARTIAL: package kind, architecture, tar.xz staging (unit tests); from packages: P9 | section 13 |
| A15 | Portable root: spaces, Unicode, relocation, another drive, no FUSE (tarball) | NOT DONE | P4/P9 |
| A16 | Launcher AppImage and tarball, equivalent | NOT DONE | P5 |
| A17 | The locally generated thin game AppImage: relocatable, regenerated on update and rollback, never distributed | NOT DONE | P5 |
| A18 | glibc 2.31 baseline verified on actual supported distributions, not only Manjaro | NOT DONE | P5 |
| A19 | Steam, Lutris and Heroic through the CLI | NOT DONE | P9 |
| A20 | CI: Windows and Linux builds, tests, boundary and privacy checks | UNVERIFIED: written, its Linux steps run locally; first GitHub run after the push | section 14 |
| A21 | Windows build and behaviour unbroken | PARTIAL: Windows' game compiles and links from this tree (a Zig cross-build, Windows' release configuration); behaviour: the owner's Windows PC (section 7) | section 14 |
| A22 | AMD rendering matches the Windows references; validation layers clean | NOT DONE | P7 |
| A23 | Performance measured and reported (target 60 fps) | NOT DONE | P7 |
| A24 | 166 maps, bosses, gameplay, audio, saves on Linux | NOT DONE | P8 |
| A25 | Release qualification from the Linux packages | NOT DONE | P9 |
| A26 | Licences, boundary, privacy audits of the Linux packages; docs and README | NOT DONE | P9 |

## 9. P1: the desktop guard on Linux (2026-11-18) [M]

- **No display unless approved**: `desktop_guard.no_display` removes `DISPLAY` and `WAYLAND_SOCKET` and points
  `WAYLAND_DISPLAY` at a socket name that is never created -- removing it is not enough, since libwayland then tries
  `wayland-0`. `game_env` applies it whenever the window is not shown; `launcher_env` also selects SDL2's
  `offscreen` video driver for the hidden launcher. `boot_smoke.py` (which only removed the variables),
  `cli_integration.py`'s commands and `player_install_test.py`'s `--info`/`--memtest` now take it too.
- **Checked, not assumed**: on Linux `Watch` looks for the process tree's connections to a display server. A Unix
  socket's client end has no name; `ss -xan` gives its peer, whose row names the listener (`$XDG_RUNTIME_DIR/wayland-N`,
  `/tmp/.X11-unix/XN` or its abstract twin). A connection comes before any window, and the check is the same on every
  compositor (no KWin-specific scripting). `cli_integration.py`'s `--play` runs are now watched (the game included,
  as a descendant).
- Evidence on this machine (KDE Plasma 6, Wayland, XWayland): with the guard's environment `vkcube --wsi wayland`
  "Cannot connect to wayland", `--wsi xcb` refused for want of `DISPLAY`; a real client of KWin's `wayland-0` seen
  as `wayland`; a live `xprop -spy` seen as `x11: @/tmp/.X11-unix/X1` and ended within one poll.
  tests/test_desktop_guard.py: 9 tests (3 new: the Linux environment; a grandchild connected to a listener named like
  a Wayland socket seen and ended; another socket ignored). Windows: `visible_windows` and its use unchanged.

## 10. The unit tests on Linux (2026-11-18) [M]

First run on Linux (Python 3.14): 17 files, 224 tests. Results and actions:

| File | First run | Cause | Action |
|---|---|---|---|
| test_worker | 1 failure | **a Linux bug**: a forced cancel (an operation blocked past the 5 s grace) called `hard_exit` from the grace thread, which called `signal.signal` -- allowed only on the main thread -- so it raised before `os._exit`: the worker said "cancelled" and kept running (with its compile or lift), on every launcher-started worker (they lead their own group) | `hard_exit` ends the other members of its group by `/proc` without touching handlers; measured: exit at 5.01 s with a child in its group ended (was 10.45 s, the child left) |
| test_portable_root | 1 failure | the Windows junction is absolute, so a moved folder's `game/` breaks until repaired; Linux's link is relative and keeps working | the test expects each platform's behaviour |
| test_settings_diagnostics | 1 failure | a Windows install path in a test run on Linux | Windows case kept; a POSIX case added (an install with a space, the home folder) |
| test_install_layout (5), test_player_pipeline (3) | errors | need the ps3recomp checkout (`build/ref/ps3recomp`), made by the pipeline | rerun after P2 |
| the other 12 | pass | | |

## 11. P2: the first native Linux build (2026-11-18) [M]

- **The pipeline** (`tools/pipeline.py --elf`): ps3recomp 4e8f3ca and all 169 patches applied on Linux, the image
  verified, extracted, lifted: 76 s. `--elf` now also places the ELF at `build/EBOOT.elf`, where the production lift
  (tools/r8_lift.sh) reads it, as the keys' path always did. The production lift: 36 s.
- **The libraries** (`tools/build_deps.py --platform linux`, Zig for x86_64-linux-gnu.2.31): SDL2 2.32.10, OpenAL
  Soft 1.25.2, FFmpeg n9.0.2, glslang, Bullet, imgui. Fixed: `build_ffmpeg.py` on Linux named a `zig_path()` that does
  not exist and fell back to any `zig` on PATH; it uses `find_zig()` (the pinned Zig). The newest glibc symbol any of
  the five shipped libraries needs: GLIBC_2.29 (the baseline is 2.31); their only non-glibc dependency is OpenAL ->
  SDL2.
- **The build** (`tools/r8_release_build.sh`, now choosing its platform by `uname`: on Linux the Zig Linux toolchain,
  `build/deps/linux`, the libraries copied into `bld/lib/`): native release with the developer diagnostics, the
  checked stores and loads (Linux's configuration until D1's change). Found and fixed on the way:
  - `ppfx_render.cpp` called `GetWriteWatch` unguarded; `ppfx_recorder.cpp` `_putenv_s` (both compile errors);
  - ps3recomp **0170**: the save list's file time read `st.st_mtime`, which `cellSaveData.c` undefines on glibc;
  - ps3recomp **0171**: the lifter's call-site caches anchored their 32-bit function offsets at the PE linker's
    `__ImageBase` (18,036 references, the link failed); elsewhere than Windows the anchor is the ELF header,
    `__ehdr_start` (hidden: PC-relative), the cache word and its atomicity unchanged. The Windows code is the same.
  The program: 857 MB with debug information; needs GLIBC_2.30 at most.
- **`--memtest` on Linux**: at first 579 of 693 -- the two failing groups were Windows' commit state (VirtualQuery):
  Linux has no commit step (one read-write `MAP_NORESERVE` mapping). They now test Linux's form: the reads across a
  64 KB unit with either side untouched (the commit-state assertions are Windows'), and the space covered without a
  gap by anonymous read-write mappings (`/proc/self/maps`; `dotheroes_guest_space_check`, which the runtime's own
  start check for the bare loads uses too) with a zero guard past 4 GB. Result: **628 checks, 0 failed** -- the
  bare-load reader included, so what the Windows release relies on holds here by test.
- **The first boot** (tests/boot_smoke.py, 90 s, offscreen, strict, a fresh save root): exit 0, no strict
  violation, 0 unresolved NIDs; the title movie and the attract demo decoded by FFmpeg (3.95 ms a frame, none late);
  pictures of the title, the overworld (water, shadows), the desert (lava) and a dungeon (lights), the HUD and the
  prompts, as on Windows by eye. Five pipelines built in play (no preparation yet); two long frames (765 ms, 110 ms)
  -- to look at under P7, after correctness.

## 12. D1: Linux in Windows' release configuration (2026-11-18) [M]

What kept Linux on the checked stores and loads was not a Windows facility but where the R8 assumptions had been
**tested** (docs/NATIVE_R8.md section 5.3: "Linux's mapping is another mechanism, not yet tested"). With
`--memtest` passing on Linux (section 11), the gate is opened to Linux x86-64:
- port/CMakeLists.txt: `_dotheroes_x64_space` = x86-64 and (Windows or Linux); the bare loads, the stores' default
  (`mov-nospu` in a native release) and the refusal of non-`checked` stores elsewhere follow it. Windows' result is
  the same as before.
- port/main.cpp `bare_loads_gate`: the `#error` off Windows replaced by the Linux check at every start
  (`dotheroes_guest_space_check`, a zero guard), fatal if it fails.
- tools/zig_build.py `release_cmake`: one configuration for both -- `mov-nospu`, `DOTHEROES_LIFTED_SPU=OFF`,
  cellAudio off. `checked` remains a developer's diagnostic build (the owner's D1). tests/player_install_test.py's
  check of the prebuilt's configuration now applies on both systems.

Evidence: build/linux/rel1 (configure: "Lifted loads: bare", "Lifted stores: mov-nospu", "Lifted SPU: not linked",
"SPU interpreter: none", the water and waver tasks native): built in 799 s; `--memtest` 601 checks, 0 failed; the
boot (90 s offscreen, strict): exit 0, no strict violation, 0 unresolved NIDs, the title movie 961 frames none late,
`[census] spu_atomic 0 spu_watch 0 ppu_coh_store 0 spu_dma 0 spu_mailbox 0 spu_group 0 spu_overlay 0 spurs_instance 0
spurs_task 0 native_decline 0 cell_audio 0`, no frame over 100 ms; pictures as the checked build's.

## 13. P4: the launcher, the command line and the updater on Linux (2026-11-18) [M]

- **The launcher** (`tools/build_launcher.py` on Linux): builds once Dear ImGui's SDL2 backend is given the system's
  X11 headers after Zig's (that file only; headers, nothing linked). `CubicHeroes`: RUNPATH `$ORIGIN/lib`, GLIBC_2.29 at
  most. The prebuilt runtime library (`build_prebuilt('linux')`) records the D1 configuration (`PPU_BARE_LOADS`,
  `PPU_STORE_MOV`, `PPU_STORES_NO_SPU_COH`, `DOTHEROES_NO_LIFTED_SPU`, no SPU interpreter).
- **The command line, headless** (the guard's environment): `--version`; `--status --json` on a root named with a
  space, an accent and Japanese; exit 2 (an unknown option), 3 (`--play` with nothing installed), 7 (a read-only
  root).
- **A full player install, headless** (`--install --disc <image> --keys <EBOOT.elf> --json` into `build/linux/inst 1`):
  verify 6 s, extract 17 s, lift 46 s, SPU 16 s, the compile and link 731 s (78 files, 5 at a time), activate, and the
  shader preparation 8 s -- **2458 pipelines, 0 failed** (2265 from the package's records, 193 made), on "AMD Radeon
  Graphics (RADV RENOIR)", driver 26.2.4. 824 s in all, exit 0.
- **`--play` on Linux** (launcher/cli.h), measured on that install: `SIGTERM` to `CubicHeroes` -- before, never passed
  on (`signal()` restarts `waitpid`); now the game ends in 0.1 s and `CubicHeroes` exits 6 (cancelled, docs/CLI.md).
  `SIGKILL` to `CubicHeroes` -- before, the game was left running; now `PR_SET_PDEATHSIG` ends it at once (Windows'
  job object's counterpart). The stale `logs/session-end` removed on both systems.
- **Return to Launcher on Linux**: the launcher sets `CUBIC_LAUNCHER` (in an AppImage the `.AppImage` file) and
  `CUBIC_LAUNCHER_PID`, and honours "close when the game starts"; the game reopens a launcher that closed itself
  (detached: a double fork, `setsid`). Compiled; to run under P8.
- **The updater** (tools/updates.py, release_trust.py, release.py, worker.py): an optional per-download `kind`
  (`zip`, `tar.xz`, `appimage`; inferred from the name when absent, so beta.2 launchers read new manifests as before),
  each kind of one system; `platform_name()` from the processor; `pick_asset` takes the installed package's kind; a
  `.tar.xz` staged with tarfile's `data` filter (executable bits kept; absolute paths, escaping members and links
  out refused); an AppImage never unpacked; a zip refused on Linux. tests/test_updates.py: the package helpers make
  each platform's own format, and new tests for these (31 tests pass on Linux).
- **Not yet**: `--update` in place and rollback from a package (a development checkout's worker has no package to
  compare with: P9); `tests/cli_integration.py` on Linux.

## 14. P6: CI, and Windows checked from this tree (2026-11-18) [M]

**CI** (`.github/workflows/ci.yml`, approved by the owner):
- *Boundary and privacy* (Ubuntu): `boundary_check.py tracked`, `license_audit.py` (forbidden middleware paths),
  `push_guard.py range` over the push's or pull request's commits.
- *Build and test* on **windows-2022** and **ubuntu-24.04**, each native: `tools/ci_build.py` -- the pinned Zig
  (`tools/fetch_zig.py`: 0.17.0 from ziglang.org, SHA-256 from its index, checked before unpacking), ps3recomp with the
  patch series, the third-party libraries, the runtime library and the native code in the release configuration
  against a **stub lift** (`tools/ci_stub_lift.py`: the lifter's own preamble, no function of any game -- the targets
  `build_prebuilt` builds need nothing else), the launcher, and the command line's smoke test (`--version`, `--status
  --json`, exit codes 2 and 3); then every `tests/test_*.py`; on Linux `tools/glibc_check.py` over the launcher and
  the shipped libraries.
- No game data, keys, generated game code or signing key. CI proves compilation and the unit and command-line tests,
  not the game's behaviour.
- Run locally on this machine: the stub-lift build of the runtime, the native code and the NID table (1275 handlers,
  94 modules); `ci_build.py --steps zig,ps3recomp,launcher,smoke` (all 171 patches applied from a clean checkout); the
  unit tests: **227 tests in 17 files, all pass on Linux**.

**`tools/glibc_check.py`**: the newest GLIBC_ version each ELF needs (baseline 2.31), no GLIBCXX/CXXABI (Zig's libc++
is static), and every DT_NEEDED glibc's or shipped. On this machine: the launcher 2.29, SDL2 2.29, avutil 2.29,
OpenAL 2.28, avcodec 2.27, avformat 2.14, and **the game the worker built here 2.30** -- all within; a Manjaro system
program (2.38, libcap) fails it, as it should.

**Windows from this tree, cross-built here** (Zig targets Windows from Linux): the libraries
(`build_deps.py --platform windows`) once two host dependencies of OpenAL Soft's Windows build were closed -- the
POSIX audio backends are now off by name (a Linux host's PipeWire headers were being compiled into the Windows DLL)
and its MSVC import library is not made (a Windows host never found the sed/dlltool it needs; Zig's linker has no
`--output-def`); the result on a Windows host is the same as before. Then the game (`R8_PLATFORM=windows
tools/r8_release_build.sh`, the R8 lift of section 11, the developer diagnostics on): configure "Lifted loads: bare",
"Lifted stores: mov-nospu", "Lifted SPU: not linked" -- Windows' configuration unchanged -- and `dotheroes.exe` (PE32+
x86-64, 87 MB) linked in 717 s, importing only Windows' own DLLs, the UCRT and SDL2.dll. **A compile and link check
only**: Windows' behaviour is checked on the owner's Windows PC (section 7).

## 15. P3: runtime parity, first part (2026-11-18) [M]

- **The write watch on Linux** (R5's optimisation; Windows: `MEM_WRITE_WATCH` + `GetWriteWatch(RESET)`). The guest
  space is registered with userfaultfd for asynchronous write protection (`UFFD_FEATURE_WP_ASYNC`, user-mode only:
  no system setting) and `PAGEMAP_SCAN` (`PM_SCAN_WP_MATCHING | PM_SCAN_CHECK_WPASYNC`) reports a run's written pages
  and protects them again in one call -- Wine's way to the same Windows call. port/main.cpp (`linux_write_watch_init`,
  `dotheroes_ww_scan`; the kernel interface kept beside the arena), port/native/render/ppfx_render.cpp `ww_poll` (its
  Linux branch stamps the same per-page epochs; everything downstream shared). Linux 6.7+; otherwise the pages are
  hashed as before. **`--memtest`'s new group** (Linux): user writes to pages 3, 10, 11 reported exactly, a kernel
  write through `read()` reported, reads never, the range protected again -- 606/606. **Effect** (the same 90 s
  offscreen boot): **17,258 MB hashed -> 2,514 MB** (-85%); 14,690 MB skipped as unwritten; 6,105 polls, 386 ms in all.
- **The crash record on Linux** (port/diag/game_log.cpp, as `on_crash` on Windows): `[crash]` lines in the game's
  log -- the signal, the module and offset (`dladdr`), the fault address and whether it was read or written (the
  x86-64 error code), the thread -- on an alternate stack, then the previous handler (a developer build's report) or
  the default action, so the process ends by the signal. `DOTHEROES_CRASH_TEST=25`: "access violation (SIGSEGV) at
  dotheroes+0x6037FF", "writing address 0000000000000008", exit -11; the developer build's report written to diag/
  with its backtrace (addr2line resolves it offline to game_log.cpp:429, the deliberate fault; in-report names: a
  follow-up). **The thread dump** (the hang report's mechanism; `DIAG_DUMP_AT=20`): 17 threads with their stacks and
  last HLE call, lifted functions named.
- **Audio** (tests/native/openal_check.sh on Linux): the Linux OpenAL Soft has the PipeWire, PulseAudio, ALSA, JACK
  and SDL2 backends (loaded at run time, none linked); the game opens "Ryzen HD Audio Controller Analog Stereo"
  through PipeWire with system events and reopen. Found and fixed: the manager and stress tests named their data
  folder after `TEMP`/`TMPDIR`, which Linux does not set -- "./sound_manager_check", the test's own program (now the
  system's temporary folder); and **an exit-time crash**: unloading OpenAL Soft unloaded its PipeWire/PulseAudio
  libraries, one of which had registered an exit handler -- OpenAL is now opened `RTLD_NODELETE` (port/native/audio/
  al_api.cpp; the game's own end skips exit handlers, so only other paths were exposed). Result: **230 checks, 0
  failed** -- synthetic cases, all 326 files, the 42 loops, a stream through a device loss and reopen, the manager
  (32), voices (32), pause (19), device losses (16), the real-time path with failing and slow reopens (9).
- **CI's first run**: the boundary and privacy job passed; both builds failed in OpenAL Soft's configure -- the
  runners' CMake 3 refuses its `add_dependencies` with the empty list our dropped HRTF data leaves (CMake 4 accepts
  it). CI now installs CMake 4 and Ninja from PyPI on both runners (the library stays unmodified); the actions moved
  off Node 20.
- Both platforms rebuilt after these shared changes: the Windows game cross-built and linked; the Linux build booted
  strict-clean with the write watch on.

## 16. P3: the display -- refresh, native Wayland, HiDPI (2026-11-18) [M]

On-screen runs (the owner's standing approval; each recorded through `tests/desktop_guard.py allow` and revoked).
- **The display's refresh off Windows** (for `DOTHEROES_FPS=display` and the display-paced vblank, which fell back
  to 60 Hz): the Vulkan backend, which owns SDL, reads the primary display's current mode (ps3recomp 0172
  `rsx_vk_display_refresh_hz`; Windows keeps DWM and EnumDisplaySettings), once, on the main thread before the
  window opens, and only when a window will be shown (offscreen: no display is asked -- and the desktop guard would
  count the connection). No setting added, nothing about the display changed. Measured: "eDP-1 17", 60 Hz" (KDE:
  1600x900 @ 60.07 Hz), the limiter at 60.00 fps.
- **Native Wayland** (SDL's `wayland` driver): the window opened but "vkGetPhysicalDeviceSurfaceFormatsKHR failed
  (VkResult 5)" -- VK_INCOMPLETE: the backend read at most 64 formats, and RADV's Wayland surfaces list more (the
  formats times the colour spaces of VK_EXT_swapchain_colorspace); the game went headless (docs/HANDOFF.md's "no
  window on Wayland", 2026-10-30, was this). The list is now read in full (0172), and the format chosen in the sRGB
  colour space first -- an HDR or extended colour space could otherwise have been the first B8G8R8A8 match. After:
  "window swapchain 1280x720, 4 images, format 44, present mode fifo" under `wayland`.
- **HiDPI**: `SDL_WINDOW_ALLOW_HIGHDPI` off Windows (0172): on a scaled Wayland display the surface at the display's
  own pixels (the swapchain already takes the drawable size); X11 unaffected; Windows' DPI awareness is the program's
  own, unchanged. A scaled display is not available on this laptop's 1600x900 panel at scale 1: verifying the scaled
  case needs a scale changed (a system setting: asked of the owner when P8 reaches it).
- SDL2's default on a Wayland session is X11 (XWayland); both now work. The default stays SDL's (docs/LINUX_PACKAGING
  .md will say how to choose `SDL_VIDEODRIVER=wayland`).
- The Windows game cross-built after 0172.

## 17. P3: thread ids and the developer keys (2026-11-18) [M]

- **Host thread ids** (port/native/host_thread.h): Windows' `GetCurrentThreadId`; elsewhere the kernel's id, one
  `gettid` per thread, kept thread-local (the recorder asks on every record). Five places read 0 off Windows; the
  recorder was therefore forced onto its locked path. Now the R8 lock-free producer is the path on Linux too:
  `recorder_locked 0`, `other_threads 0`, the boot strict-clean (phys_direct, inflate_direct and the recorder's own
  checks use the same helper).
- **The developer keys** (`DOTHEROES_DEV_KEYS=1`; port/native/dev_state.cpp): the key loop -- F8 or L3+R3 a hitch
  mark, F7 the frame-rate readout, F9 a rumble pulse, F6 the credits, and the drive scripts' mark, trace, life and
  position triggers -- was compiled for Windows only, so drive scripts using `life.trigger`/`pos.trigger` could not
  run on Linux. It now runs everywhere: Windows reads its keys as before; elsewhere the runtime's SDL state does
  (ps3recomp **0173** `cellPad_host_fkeys`, focus-gated; `cellPad_host_buttons` for L3+R3). Verified offscreen: two
  `mark.trigger`s logged 3 s apart. The keys themselves need hands on the keyboard (an owner check under P8). The
  CPU-debug-register watch (`DEV_HWWATCH`) stays Windows' (a development tool).
- **CI's second run**: the Linux job passed on GitHub (9 min 33 s: Zig, ps3recomp and its patches, the libraries,
  the runtime against the stub lift, the launcher, the command line, the unit tests, the glibc check).
- The Windows game cross-built after these changes.

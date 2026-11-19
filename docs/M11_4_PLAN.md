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
- D3 (revised by the owner, 2026-11-18, evening): the already decrypted EBOOT.elf is the installation and
  qualification input; the separate keys file blocks nothing (the qualification's `keys` step is not applicable to it).
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
| A3 | Native Linux dev build boots and plays (offscreen) | VERIFIED: plays from saves -- the 166-map sweep, the river fight, seven bosses, the ending (sections 25, 27, 30) | sections 11, 25, 27, 30 |
| A4 | Linux release configuration as Windows' (D1: no lifted SPU in the release) | VERIFIED at boot (--memtest 601/601; the census all zero); longer play: P8 | section 12 |
| A5 | Release crash record and hang report on Linux | VERIFIED: the release crash record, the developer crash report and thread dump (section 15); the graphics' start watched in every build -- a stalled window system ends the game with status 70 and a report (section 23) | sections 15, 23 |
| A6 | Audio through OpenAL Soft with the system's server; device change recovered | VERIFIED: the audio suite 230/230; a real default-output change and back during play, followed both ways (section 23); the shipped OpenAL with PipeWire, PulseAudio and ALSA | section 23 |
| A7 | Cinematics through FFmpeg | VERIFIED: the opening 4,620 of 4,620 frames, the credits 7,860 of 7,860 with all 11,554,189 sound frames, 0 late as a player runs it (section 30) | section 30 |
| A8 | X11 and Wayland: windowed, borderless, fullscreen; the desktop's mode never changed | VERIFIED: all three modes on screen under native Wayland and X11 (XWayland, RADV presenting), the desktop at 1600x900@60.07 before and after every run; HiDPI at 150% (section 23) | sections 22, 23 |
| A9 | Rendering resolution presets and custom; VSync; frame limits | NOT DONE | P8 |
| A10 | Controllers: detection, prompts, hot-plug, remapping; keyboard remapping | PARTIAL: detection, prompts and hot-plug VERIFIED by the owner's hands with the packaged SDL2 (section 29); remapping: to do | section 29 |
| A11 | The in-game overlay, performance overlays, developer tools, diagnostics and log export | NOT DONE | P8 |
| A12 | The public CLI and exit codes (docs/CLI.md), `SIGTERM` reaching the game | VERIFIED: tests/cli_integration.py --with-game 17/17 on Linux (Windows: 17/17, M11.2); the signals; every command from both packages (section 20) | section 21 |
| A13 | Graphical launcher: install from the disc, local recompilation, shader preparation | PARTIAL: headless install verified (2458 pipelines, none failed); the window: to do | section 13 |
| A14 | Signed updates, repair and rollback; the right package per OS and architecture | PARTIAL: tests/linux_update_check.py 10/10 from the package (signed release offered and swapped in; bad signature, untrusted key, Windows-only release, corrupted download and a zip refused; each install kind its own package); update and rollback from the packages; `--install --repair` from the tarball's install, its game removed: rebuilt, saves byte-identical, plays (section 28) -- VERIFIED on the packages rebuilt with the rollback fix: to do | sections 23, 28 |
| A15 | Portable root: spaces, Unicode, relocation, another drive, no FUSE (tarball) | VERIFIED except another drive: spaces, accents and Japanese, the whole root moved and played (both packages), no FUSE (containers). **Another physical drive: DEFERRED by the owner's decision (2026-11-18) -- not verified, not a release blocker** | sections 20, 23 |
| A16 | Launcher AppImage and tarball, equivalent | VERIFIED: one folder; the AppImage = the folder file for file; each qualified 14/14 from the package, offline (section 20) | section 20 |
| A17 | The locally generated thin game AppImage: relocatable, regenerated on update and rollback, never distributed | VERIFIED: made by the packaged installs, of the active build after the update and after the rollback; through FUSE and extract-and-run; before and after the root moved (section 20) | section 20 |
| A18 | glibc 2.31 baseline verified on actual supported distributions, not only Manjaro | VERIFIED: 114 ELF files at GLIBC_2.30 at most; 8/8 checks on Debian 11, Ubuntu 20.04, Ubuntu 24.04, Fedora 41 (the game built here included) | section 19 |
| A19 | Steam, Lutris and Heroic through the CLI | NOT DONE | P9 |
| A20 | CI: Windows and Linux builds, tests, boundary and privacy checks | VERIFIED: run 44450410663 -- Linux 13m16s, Windows 22m46s, boundary and privacy, all pass | section 19 |
| A21 | Windows build and behaviour unbroken | PARTIAL: Windows' game compiles and links from this tree (a Zig cross-build, Windows' release configuration); behaviour: the owner's Windows PC (section 7) | section 14 |
| A22 | AMD rendering matches the Windows references; validation layers clean | PARTIAL: validation layers clean (0 errors, section 27); the five scenes look like RPCS3's, 1 of 5 within Windows' accepted figures; the rest separated (section 33): the shadow map's last steps D32's, the reflections, the corridor's spread brightening and the editor's lines AMD/RADV's; no change before beta.3 recommended -- the owner's decision | sections 26, 27, 33 |
| A23 | Performance measured and reported (target 60 fps) | PARTIAL: 60 fps held -- the castle-front route 540p-1080p (section 24), the river fight and the seven boss fights with no frame over 25 ms (section 27); dungeon exploration and the 540p spike's repeat: to do | sections 24, 27 |
| A24 | 166 maps, bosses, gameplay, audio, saves on Linux | PARTIAL: the 166-map sweep passed (207 of 207 pipelines prepared, section 25); the developer-tools suite 16/16 -- a guardian fought to its defeat, the ending to the credits, saves across processes (section 27); the seven boss fights; audio in play, the player's own saves: to do | sections 25, 27 |
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

## 18. P5: the Linux packages (2026-11-18) [M]

**The owner's decisions (2026-11-18)**: the official AppImage type-2 runtime, prebuilt, pinned (release `20251108`,
never "continuous"), its GPG signature verified before use, recorded in config/deps.sha256, its corresponding source
and the LGPL relinking materials shipped; mksquashfs bundled for the player's game AppImage with its complete source;
both AppImages made offline with nothing for the player to install; Debian 11's end of life documented with a
security-update plan (docs/LINUX_PACKAGING.md section 7); the glibc target unchanged without discussion.

- **The build environment** (tools/linux_build_env/Dockerfile): Debian 11 by digest; its packages pinned to
  snapshot.debian.org 2026-08-01 (backports from Debian's archive -- the live mirrors had started dropping Debian 11's
  files); OpenSSL 3.5.9 (LTS) and Python 3.12.16 built from pinned sources (Debian 11's are 1.1.1 and 3.9); PyInstaller
  6.22.3, CMake 4.4.5, Ninja pinned. tools/linux_package.py runs the steps in it with the checkout at `/src` (a
  neutral path: nothing built records the developer's folders) as the developer's user, from clean.
- **The AppImage runtime** (tools/fetch_appimage.py): every file must already be pinned in config/deps.sha256 (no
  pinning on first use); the signature verified in a throwaway keyring against the pinned fingerprint
  570C77ACEA40C0F1B758902CBF96CCA56490F695, the key itself cross-checked against the repository at the release's
  commit; a runtime with one byte appended was refused. What it links statically (its own build scripts at that commit):
  libfuse 3.15.0 (LGPL-2.1, patched), squashfuse 0.5.2, zstd, zlib, mimalloc, musl -- their licences, the runtime's and
  libfuse's and squashfuse's complete sources and the rebuild instructions in `worker/licenses/` (AppImage-NOTICE.txt).
- **mksquashfs**: squashfs-tools 4.7.5 from its pinned source, gzip and zstd only (what the runtime reads), fully
  static: `worker/toolchain/appimage/` with the runtime; GPL-2.0, its source shipped.
- **tools/appimage.py**: runtime + squashfs image; tested with a path holding a space and Japanese: started through
  FUSE and with `--appimage-extract-and-run`, the arguments and `$APPIMAGE` as given.
- **The packages** (`python3 tools/linux_package.py`): `Cubic.Heroes-0.2.0-beta.2-linux-x86_64.tar.xz` (106 MB) and
  `Cubic.Heroes-0.2.0-beta.2-x86_64.AppImage` (116 MB), from the same folder; the AppImage unpacked again is that folder
  file for file (bytes and executable bits); both through the boundary check and the privacy scan.
- **Found and fixed on the way**: the frozen worker carried **Debian 11's OpenSSL 1.1** beside its own 3.5 (PyInstaller
  followed release_trust's `find_library("crypto")`), and its candidate order ("libcrypto.so.1.1" sorts before
  "libcrypto.so.3") would have checked release signatures with the old one -- the newest is now tried first and the
  Linux bundle drops 1.1 once `_ssl` is seen to link 3; **Pillow** (the Windows worker's, for the game's .ico) was
  frozen into the Linux worker with a dozen image libraries and a libstdc++ -- excluded on Linux; Python's licence file
  is found where a source build installs it; the Windows FFmpeg build no longer takes a CI runner's windres
  (`--windres=false`: the developer PC had none, so the DLLs are as before).
- **glibc**: tools/glibc_check.py over the whole package (the frozen worker's Python, OpenSSL and libraries included,
  a library the package itself carries counting as shipped): **114 ELF files, GLIBC_2.30 at most, none outside**.
- **The player's game AppImage** (D2; tools/game_appimage.py, tools/worker.py `refresh_game_appimage`): made after
  every activation (install, update) and rollback, and on demand (`worker game-appimage`); the active build's program,
  build.cfg and lib/ only; its start script finds the root by `CUBIC_INSTALL_DIR`, a `cubicheroes-root.txt` beside it,
  the folder above `exports/` (relative: a moved root keeps working), or the root it was made in, and starts the game
  as `--play` does; the game takes the root from `DOTHEROES_INSTALL_ROOT` only when that folder holds an installation
  (port/main.cpp). tests/test_game_appimage.py: the four rules, a moved root, a refusal with no installation -- with
  spaces and Japanese in every path. A failure to make it never fails the install.
- **Shortcuts on Linux** (tools/shortcuts.py, uninstall.py, launcher/main.cpp `self_program`): Steam and the menu name
  the `.AppImage` file in an AppImage (else `CubicHeroes`; `CubicHeroes.exe` was passed on every system); the project's
  icon installed in the user's theme as `cubicheroes` when shortcuts are asked for; the entry written UTF-8; uninstall
  removes the entries whose Exec is this launcher (another copy's are left) and then the icon (test_uninstall: 17).
- **Release qualification on Linux** (tests/release_qualification.py): a `.tar.xz` or an `.AppImage` as well as
  Windows' zip; new steps `gameappimage` and `relocate`; `remove`/`reunpack` recorded as not applicable to an AppImage
  (a file the player replaces, never unpacked into the root).
- Windows: the game cross-built and the launcher compiled for Windows from this tree.

## 19. P5/P6: CI green, the packages on other distributions, the worker's children (2026-11-18) [M]

- **CI** (.github/workflows/ci.yml), after three fixes found by its runs -- CMake 4 for OpenAL Soft's configure (run
  1), FFmpeg's Windows build without the runner's windres (run 2), owned_run's test with a native sleeper (run 3: MSYS's
  sleep is not a process of its own in the runner's Windows tree) -- **run 44450410663: all three jobs pass**: Linux
  (13 min 16 s), Windows (22 min 46 s: Zig, ps3recomp and its 173 patches, every library, the runtime against the stub
  lift, the launcher, the command-line smoke test, every unit test), boundary and privacy.
- **Other distributions** (tests/distro_check.py; clean containers, no network, no display, the image digests in the
  run's JSON): Debian 11 (glibc 2.31), Ubuntu 20.04 (2.31), Ubuntu 24.04 (2.39), Fedora 41 (2.40) -- each **8 of 8**:
  the launcher (`--version`, `--status --json` on a folder with a space and an accent, the usage exit code 2), the
  frozen worker, the AppImage through `--appimage-extract-and-run` (no FUSE in a container), and the game the worker
  built on this laptop (`--memtest` 601 checks 0 failed; `--info` with FFmpeg loaded).
- **The frozen worker's children** (tools/hostenv.py): PyInstaller's `_internal/` first on `LD_LIBRARY_PATH` reached
  every program the worker starts (shown in the package: a child of the worker inherited it). A first fix restored the
  player's value for the whole worker -- and broke its own lifting tools, which are the frozen program again and reuse
  their parent's set-up ("libffi.so.7: cannot open shared object file": the packaged qualification's install failed at
  the lift; found there, not shipped). Now only the system's and the player's programs get the player's value (the
  game's `--info`, `--memtest`, `--prepare`; diagnostics; xdg-user-dir, gio, pgrep, Steam); the worker's own helpers
  keep theirs. In the package: a tool child loads its bundled libraries, a system program sees no worker path.
  tests/test_hostenv.py.
- docs/LINUX_PACKAGING.md rewritten to the packages as built (it was the M11.2 plan).

## 20. The packaged qualification on Linux (2026-11-18) [M]

tests/release_qualification.py from the final packages (`Cubic.Heroes-0.2.0-beta.2-linux-x86_64.tar.xz`, 101 MB;
`Cubic.Heroes-0.2.0-beta.2-x86_64.AppImage`, 116 MB), each in a fresh folder named "Cubic Heroes RC" with an accent, a
diaeresis and Japanese, `--offline` (a dead proxy for every step), the disc image and the decrypted EBOOT.elf (the
keys file's run, and its `keys` step, wait for the owner's file: D3):

| Step | Tarball | AppImage |
|---|---|---|
| unpack (top items; executable bits) | pass | pass (copied into the folder; the image's own items) |
| empty (`--status`: nothing installed; the root: the folder / "beside the AppImage") | pass | pass |
| install (interrupted after 240 s, then resumed) | pass, 591 s | pass, 613 s |
| status, shaders, noop (`--update`: nothing rebuilt), checkup (offline: answered at once) | pass | pass |
| play (`--play`, the game's own end through its exit question, status 0, no report) | pass | pass |
| rebuild (a new version: a tool and the game's code changed -- the AppImage's: a new `.AppImage` file) | pass | pass |
| rollback (the first build active again, nothing rebuilt; played) | pass | pass |
| gameappimage (exports/, of the active build; FUSE and extract-and-run) | pass | pass |
| relocate (the whole folder moved: found, played, its game AppImage) | pass | pass |
| remove, reunpack | pass | not applicable (an AppImage is a file, never unpacked into the root) |

**14 of 14 for each.** Found by these runs and fixed before this result: the frozen worker's library path in its
children (section 19, and the first fix's regression of the lifting tools); the AppImage's update path (a new file);
Linux's reproducible build making the old rebuild step's comment-only change a no-op. Not covered by this run:
`suite` (the developer tools' installed checks: a save from the owner's checkpoints) and `keys` (the owner's keys file).

## 21. The command line's integration tests on Linux (2026-11-18) [M]

tests/cli_integration.py --with-game (the development launcher, a copy of the development install, offscreen): **17 of
17** -- the version and help; the default root from another working folder; CUBIC_INSTALL_DIR; the root pointer
(relative); a relative --install-dir; an unusable root (exit 7); nothing installed (--play 3, --install without a disc
2); the installation in the launcher's folder; --set kept and a refused value; the folder moved; --migrate-from (9 save
files checked, the source unchanged) and its refusal; two installations side by side; --remember; and with the game:
session options in effect and not kept, the game's own end passed on (0); the game's exit status passed on -- on Linux
the game killed with SIGKILL and CubicHeroes exiting 137 (128 + 9: a status cannot be set from outside as Windows' 42
is); the game ending with CubicHeroes (PR_SET_PDEATHSIG). The runner's Windows-only parts (`tasklist`,
`TerminateProcess`) now have Linux forms (/proc, a signal).

## 22. Window modes, and the shipped SDL2's lost X11 backend (2026-11-18) [M]

- **Wayland, on screen** (approved runs, each recorded through the guard): windowed 1280x720; borderless 1600x900 at
  0,0; exclusive "at the display's mode 1600x900 60 Hz". **The desktop's mode, read before and after each run with
  kscreen-doctor, stayed 1600x900@60.07 throughout** (exclusive fullscreen takes the display's current mode).
- **Found: the container-built SDL2 had no X11 backend** -- "SDL video init failed: x11 not available", the game
  headless on any X11 session (and on SDL's default XWayland path). The libraries built in the pinned environment had
  lost every backend found through `find_library`: SDL2's X11 (libX11, Xext, Xcursor, Xi, Xrandr, Xss, ...) and libudev
  (controller hot-plug), OpenAL Soft's PulseAudio and ALSA (only PipeWire left). Debian keeps libraries in
  lib/x86_64-linux-gnu, which a cross toolchain's find_library does not search. Fixed in zig_build's toolchain file
  (`CMAKE_LIBRARY_PATH` += that folder on a multiarch host; `CMAKE_LIBRARY_ARCHITECTURE` was tried first and also put
  the host's glibc internals ahead of Zig's headers). **tools/linux_package.py now refuses a build whose SDL2 lacks X11,
  Wayland or udev, or whose OpenAL Soft lacks PipeWire, PulseAudio or ALSA** -- no offscreen test can see a missing
  window system, which is how this reached the packages (it was caught before any release, by the window-mode runs).
- **X11**: with the fixed SDL2, the game's windows did not appear on the desktop -- SDL waiting in `XIfEvent` for the
  window's map (the game's own thread dump). Manjaro's own system SDL2 hangs the same way, and both open X11 windows on
  a private Xvfb display: the desktop session's XWayland stopped mapping new windows (the morning's X11 run worked).
  On Xvfb the three modes are created through SDL's X11 driver (windowed 1280x720; borderless and exclusive 1600x900 at
  0,0); presenting needs a GPU path Xvfb lacks, so headless there, as expected. The owner is asked to restart the
  session (log out and in) for the X11 desktop runs.

## 23. Closing checks, first part (2026-11-18, evening) [M]

The owner restarted the KDE session (the stalled XWayland), copied the Windows checkpoints, RPCS3 references and
baselines into `build/` (local, git-ignored), approved the audio and scaling changes, and revised D3 (the decrypted
EBOOT.elf is the qualification input).

- **The packages re-qualified after the backend fix** (section 22): tarball and AppImage **14 of 14 each** again, from
  the rebuilt packages (SHA-256 49a463c4... and f44ea0f6...); the distribution check 8/8 on all four images again.
- **Signed updates on Linux** (tests/linux_update_check.py, the package's own frozen worker, a throwaway Ed25519 key,
  a local mirror): **10 of 10** -- a signed newer release offered; the tarball install downloads the tar.xz, an AppImage
  install the AppImage, each checked; the tar.xz staged with CubicHeroes executable; the swap: the new launcher started
  offscreen, healthy, kept, the player's save untouched; refused: a manifest whose signature does not match, one signed
  by an untrusted key, a release with only a Windows package (nothing for Linux), a download not the manifest's
  (nothing kept), a zip handed to Linux's staging.
- **Why the start-up watchdog said nothing about the X11 stall**: the developer build's watchdog allows 120 s before
  the first frame (the runs were 25-40 s), and **release builds had no watchdog at all** (diag is the developer
  build's) -- a player would have had a game hanging with no window and no word, on either system. Now (port/main.cpp
  `startup_watch`, every build, both systems): the graphics' start -- the display's refresh read on the main thread,
  then the window, device and swapchain -- is watched; after `DOTHEROES_STARTUP_TIMEOUT_S` (30 s) the game logs why
  (naming the video driver), writes `logs/crash/startup-<time>.txt` (a developer build also dumps every thread) and
  ends with **status 70**, which `CubicHeroes --play` passes on. **tests/startup_stall_check.py**: a fake X server that
  accepts and never answers -- the game ended in 10.0 s (timeout 10) with status 70, its log naming the stall, the
  report written. The Windows game cross-built with it.
- **X11 with hardware presentation, on the desktop** (after the session restart): windowed 1280x720, borderless (a
  1280x720 swapchain made again at 1600x900 once X11's fullscreen size arrived), exclusive at the display's mode --
  each "window swapchain ... format 44, present mode fifo" through XWayland on RADV; the desktop 1600x900@60.07 before
  and after each.
- **HiDPI at 150%** (KDE's scale set with kscreen-doctor, then restored to 1, confirmed): under native Wayland a
  1059x536 window (fitted to the 1067x600 desktop units) with a **1589x804 swapchain** -- the display's own pixels, not
  a stretched buffer.
- **A real default-output change** (a temporary null sink made the default during play, then the original back, the
  module unloaded; the original default confirmed): "the default output changed: moving to it" / "device reopened:
  CubicTestSink" and back to "Ryzen HD Audio Controller Analog Stereo".

## 24. P7: AMD gameplay performance, the castle-front route (2026-11-18) [M]

**The route** (tests/scripts/res_perf.drive): Windows' M11.2f frame-pacing route, reconstructed line for line from
its kept baselines' drive.log (build/baselines/2026-11-18-render-res-perf) and committed -- CONTINUE from the
castle-front checkpoint, no death, a square walked in front of the castle; PS3_FTRACE 24:27 (27 s of gameplay),
offscreen, DUMP=0, the development build in Windows' release configuration (build/linux/rel1). Measured with
tests/perf_run.py: the frame times (the guest's flip requests, as tests/frame_times.py reads them), amdgpu's GPU busy
percentage and VRAM/GTT growth, the game's CPU time (100% = one core); a run whose drive did not finish is marked
INVALID.

**Like for like with Windows' baseline (shader cache off, every pipeline built in play), 1080p, 60 limit**:
Windows (RTX 3070): 60.0 fps, median 16.7, p95 18.7, p99 19.3, worst 20.8 ms, none over 25 ms. This laptop: 37.0
fps -- median 16.7 and p95 18.5 as Windows, but 14 frames over 25 ms, the worst 4.2 s: pipelines built in play (one
frame built 22 in 2.4 s) -- RADV's compiler on this APU is far slower than NVIDIA's. Players never run that way: the
install prepares the pipelines (below).

**As players run it -- the shaders prepared** (the development build given its own comprehensive preparation: 197
recipes + 2265 package records, 0 failed; Mesa's disk cache warm). Each run's drive finished its route:

| Rendering | 60 limit: fps / p95 / p99 / worst / 1% low / >25 ms | Unlimited: fps / median / p99 | GPU busy (60 / unl.) | Game CPU (unl.) | VRAM over idle |
|---|---|---|---|---|---|
| 960x540 | 59.3 / 17.9 / 19.6 / 212 / 25.5 / 2 | 96.9 / 10.1 / 14.0 | 45% / 70% | 199% | 275 MB |
| 1280x720 | **60.0** / 17.8 / 18.7 / 24.3 / 49.8 / 0 | 88.2 / 11.1 / 15.4 | 52% / 84% | 209% | 290 MB |
| 1600x900 (the panel) | **60.0** / 18.2 / 20.3 / 26.1 / 45.5 / 1 | 82.2 / 11.7 / 16.2 | 65% / 92% | 212% | 316 MB |
| 1920x1080 | **60.0** / 18.4 / 20.9 / 28.0 / 43.4 / 2 | 72.9 / 13.4 / 18.9 | 77% / 95% | 212% | 341 MB |

GTT growth stayed under 15 MB in every run. **Findings**: 60 fps held at every resolution to 1080p on the Renoir
iGPU (p99 within 2 ms of the RTX 3070's at 1080p); unlimited, the frame rate falls with the resolution while the GPU
is 84-95% busy -- **GPU-bound from 720p up**; at 540p the GPU is 70% busy and the game uses about two cores --
**CPU/synchronisation-bound** there (~97 fps). The 540p/60 run's single 212 ms frame is unexplained so far (a repeat
run to classify it; this machine runs with ~9 GB of swap in use). The four "not prepared" pipelines each run met (2-16
ms in all) are in the preparation's records now.

**Not valid, discarded**: a first matrix through the installed game (INSTALLED=) -- an installed game with developer
mode off ignores the drive's state probe (B-001's rule), so the drive failed at the title and the trace timed the
title screen (900 fps). tests/perf_run.py now refuses such runs.

## 25. P8: the 166-map sweep, and the preparation's depth format (2026-11-18/19) [M]

**The sweep** (tests/map_sweep.py run, six chunks, offscreen, the development build in Windows' release
configuration, over its own comprehensive preparation): every one of the 166 maps reached, none failed, the known
B-007 draw in chunk 4 only. **First run: 167 of 209 pipelines met unprepared** (Windows' sweep: 26 of 190) -- 84
distinct keys, all asp's (the engine's); PPFX's all prepared.

**The cause**: a recipe keeps the depth format it was recorded with. The manifest's and the profiles' were recorded
on Windows (NVIDIA: D24_UNORM_S8); the Renoir has no D24, so every depth surface is D32_SFLOAT_S8 (depth_vkformat) --
and the depth format is part of an asp pipeline's key, so nothing prepared from those recipes was the pipeline a
draw looked up. **Fixed** (port/native/render/ppfx_render.cpp, recipe_host_depth): a recipe's depth target is read as
this device's one depth format, for the recorded recipes, the warm-up's and the profiles'. A device with D24 reads
them unchanged (Windows as before). The preparation then held 193 recipes, not 197: those this machine had recorded
with D32 are now the same pipelines as their D24 twins.

**Second run, over the preparation made by the fixed build**: 166 of 166 maps; **207 pipelines first met after the
start, 207 prepared, 0 unforeseen; 0 translations compiled in play**; PASS (51 min). The castle-front figures of
section 24 were measured over the first preparation: each run met 4 pipelines unprepared (2-16 ms in all, Mesa's
disk cache warm), within the figures given there.

**To check (A22)**: D24 was chosen because a float depth surface's bias lost the castle hall pillars' shadows
(2026-11-09). This device has only D32_SFLOAT_S8: the RPCS3 reference scenes (the entry hall's shadow map) decide
whether that comes back here.

## 26. P8: the RPCS3 reference scenes on AMD (2026-11-19) [M]

**The drives could not walk the hero on Linux**: the developer tools found the player's object (and the objects
near it, and the fields' classes) by scanning the committed guest heap with VirtualQuery -- Windows only; on Linux the
hero's position read 0, so every drive's `goto` and `cross` failed (the reference scenes, and every route that walks:
the developer-tools suite, the bosses). The map sweep was not affected (it warps). **Fixed** (port/native/dev_state.cpp,
guest_runs): Windows walks its committed regions as before; Linux reads the pages the game has touched from
/proc/self/pagemap (present or swapped). The developer tools' memory figures on Linux from /proc/self/status (they
read 0 before). Each scene's camera now lands exactly on RPCS3's.

**The five scenes** (tests/rpcs3_reference.py run, offscreen, the development build; LAUNCHER_PROMPTS=playstation so
that the HUD's prompts are the reference's -- without a controller the game draws keyboard keys):

| Scene | Result | Notes |
|---|---|---|
| dungeon_door | within the accepted figures | |
| entry_hall | over: refl mean 1.4/0.7/1.8 (accepted 0.18/0.16/0.33), combined far 17% (1.4%) | the floor reflection whole, its stair walls drawn: the depth clamp right on D32 |
| corridor | over: scene far 28% (2.5%), refl mean 2.0/1.0/2.8 (0.14) | the lit scene slightly brighter on the walls and pillars (221k pixels brighter, 17k darker, ~0.005 linear); the carpet the same |
| king | combined, bloom, glare, reflection within; the shadow map 59% of texels within two 2^-24 steps (accepted 100%) | the depth surfaces are D32_SFLOAT_S8 here (no D24 on this GPU) |
| editor | over: final blue mean 1.1 (0.018), 0.19% of pixels | the 1-pixel grid lines only: RADV's lines are not strict (strictLines false), NVIDIA's are; the triangles identical |

Pictures side by side: the same scenes, the same lighting, the pillars shaded; the differences are faint and spread
over the surfaces, not missing or wrong objects. Our runs agree with one another (the corridor's two runs identical,
the entry hall's three within 1% of pixels), so the difference is stable, not the hero's placement.

**Ruled out or measured**: reduced precision (no SPIR-V module carries RelaxedPrecision); the float depth bias -- an
experiment with VK_EXT_depth_bias_control (the exact 2^-24 step on the float surface; RADV has it) brought the shadow
map's mean difference from 2.8e-6 to 6.7e-7 but its within-two-steps share only from 59% to 63%, and changed neither
the corridor nor the entry hall: not the cause, and not kept (it needs a Vulkan 1.1 instance, a change to every
system's start; the instance stays 1.0). Rectangular lines (VK_EXT_line_rasterization) for the editor's grid: the
same instance requirement, for 0.19% of one scene's pixels: not made. Both kept as patches: patches/experiments/.

**Not yet separated**: Windows' build passed 5 of 5 on 2026-11-16 (docs/M11_PLAN.md); the renderer has changed since,
and this machine differs in GPU (AMD: D32 depth, non-strict lines, its own filtering) as well as system. A Windows run
of this commit, with D24 (the default) and with DOTHEROES_PPFX_DEPTH_D32=1, separates NVIDIA-with-D32 from AMD and
from the code: asked of the owner.

## 27. P7/P8: the developer-tools suite, the validation layer, the river fight (2026-11-19) [M]

**The developer-tools suite on Linux: 16 of 16** (tests/devtools_suite.py, offscreen): the channel and Lua runner,
warps, the in-game menu, player state, progress, encounters, diagnostics, persistence across a save and a new
process, the ending to the credits, every enemy spawned, the first temple's guardian from its introduction to its
defeat, forced encounters, cube maps, triggers.

**The Vulkan validation layer** (PS3RECOMP_VK_VALIDATION=1, the Khronos layer 1.4.363; separate runs, not timed): the
castle-front route, the king's close-up (bloom, the shadow map), the guardian's fight and every enemy: **0 errors**.
One warning, the same in each (reported to the layer's limit of 10): a fragment program writes colour outputs in a
pass with no colour target (the depth-only passes: the shadow map) -- unused writes, not a fault. CI on a05cda3: all
three jobs green (run 44490173256).

**The river fight** (tests/scripts/fight_river.drive, castle-front save, can't-die; the preparation of section 25
copied as the shader folder, the warm-up on: as an install runs it -- 2,458 prepared pipelines made in 0.2 s, **0
built in play**). The trace's first 1.4 s held the map load into the river's area (six frames 25-77 ms); the fight
itself, 47-101 s:

| Limit | fps | median / p95 / p99 / worst (ms) | 1% low | >25 ms | GPU busy | game CPU |
|---|---|---|---|---|---|---|
| 60 | **60.0** | 16.7 / 18.5 / 20.1 / 23.9 | 47.4 | 0 | 54% | 148% |
| unlimited | 80.1 | 11.8 / 15.7 / 17.3 / 23.6 | 53.8 | 0 | 81% | 205% |

Windows (R8, the RTX 3070 desktop): 60.0 in every arm capped, ~200 fps uncapped. Here capped play holds 60 with no
frame over 25 ms; uncapped the GPU is 81-88% busy -- the iGPU is the limit, as on the castle front at 720p and up.
Without a preparation (a first run, nothing prepared) the same fight built 55 pipelines in play (140 ms, 21 frames
held, the longest 94 ms): the preparation is what keeps play smooth on RADV.

**The bosses** (tests/scripts/fps_bosses.drive: the six temples' guardians and the last enemy, each entered the game's
way, 40 s against an invulnerable hero; 60 limit; the same preparation, 0 pipelines built in play), each fight from 2 s
after its mark (the arena's entry done):

| Fight | fps | p99 / worst (ms) | >25 ms |
|---|---|---|---|
| temple 1-6, the last enemy | **60.0** each | 18.3-19.6 / 19.8-23.0 | 0 in each |
| all seven (16,169 frames) | 60.0 | 19.0 / 23.0 (1% low 50.7) | 0 |

GPU 41% busy on average (51% at most), the game 78% of a core on average; VRAM 367 MB over idle. The nine frames of
100-447 ms in the whole run all fall between fights: the tools' `progress set` and the entry into the next temple
(map loads), not play.

## 28. `--install --repair` on Linux (2026-11-19) [M]

From the packaged tarball's qualification install (the root with spaces, accents and Japanese, moved), the active
build's game removed: `--play` refused it ("The game's program is missing ... Repair the installation"); `--status`
reports the layout only (installed). `CubicHeroes --install --repair --disc <image> --keys <EBOOT.elf> --json`: exit 0
in 21 s (the compiled objects reused: extract 8.2 s, decrypt 1.5, build 4.0, verify 4.2, the preparation 0.9); a new
build active; the 11 save files byte-identical before and after; the repaired game played offscreen (the title movie,
962 frames; SIGTERM forwarded, status 6).

**Found and fixed**: the activation kept the build it replaced for rollback whatever its state -- after this repair,
the damaged build was the rollback and the whole one before it was pruned (a rollback would then have been refused:
"build_damaged"). tools/install_layout.py activate keeps only whole builds (check_build); tests/test_install_layout.py
test_a_repair_keeps_the_whole_build_for_rollback (26/26). Windows shares the change (its install layout is the same
code).

## 29. Controller hot-plug, by the owner's hands (2026-11-19) [O, M]

The packaged tarball's install (the corrected SDL2: X11, Wayland and udev backends), on screen (the desktop guard's
approval recorded for the run and withdrawn after), PAD_DEBUG=1. An Xbox Series X controller over USB: **6 connections
and 5 disconnections, every one on port 0** ("SDL controller reconnected on port 0 ... instance 1-5"), input read
after every reconnection (the owner's play and 45+ button and stick changes logged), no stall at a device change (SDL
polled on its own thread; no "update took" report), the game closed normally (exit 0). The prompts follow the device
used last, as designed: the Xbox glyphs after a press on the controller, the keyboard's after a key (the owner:
working). Not tested: a second controller of another make at once, Bluetooth.

## 30. The cinematics, and the credits' stalls traced to the test harness (2026-11-19) [M]

**Decoded through FFmpeg** (n9.0.2, LGPL, the Bink decoder): the opening (tests/scenarios/m8_movie_trace.drive) 4,620
of 4,620 frames, 2.27 ms a frame, none late (silent by design: its music is the sound manager's); the credits (the
ending scenario) 7,860 of 7,860 frames, 44.1 kHz stereo, all 11,554,189 sound frames.

**The credits' stalls**: in the developer-tools suite's ending the game's own frames stalled during the credits (4 to
68 frames over 100 ms, up to 1.1 s; 70-754 movie frames late) -- Windows: 0 late. Traced with a recorder beside the
run (build/tmp/bench, memrec.py: every second the system's available memory, swap in/out, major faults, memory and IO
pressure, /tmp's tmpfs, the game's resident set, swap and faults; every 10 ms each game thread in uninterruptible
sleep and its kernel wait point):
- **not memory**: during the credits >= 8.9 GB available, 0 pages swapped out, 15 in, the game 0 major faults and
  nothing in swap;
- **disk writes**: the game's present thread blocked ~16 s of the credits in `rq_qos_wait` (the block layer's
  writeback throttling) and on page writeback, the main thread ~7 s on file pages; system IO pressure (full) to 27%.
  The writes: the test harness's live frame dump (PS3RECOMP_VK_DUMP: frame.ppm, 2.7 MB, rewritten every 15th
  present on the present path, for the drives' `shot` steps) -- never on in a player's game;
- **as a player runs it** (DUMP=0, the same ending without its `shot` steps, the same preparation): **0 frames
  late, 0 waited for, no game frame over 100 ms** in the credits; IO pressure at most 3.8%.

Timing runs already use DUMP=0 (tests/perf_run.py: sections 24 and 27). /tmp on this machine is a RAM-backed tmpfs
(8.8 GB; 3.5 GB in use, most of it swapped out: other sessions' files and seven 108 MB `appimage_extracted_*`
folders left by this milestone's extract-and-run tests, removed); large temporary files and benchmark captures go to
the git-ignored build/tmp (TMPDIR) from now on.

## 31. Extract-and-run leaves its copy behind (2026-11-19) [M; root cause found; documented, not a release blocker]

The pinned AppImage runtime (type2-runtime 20251108, dd6cebed) in extract-and-run mode (`--appimage-extract-and-run`
or APPIMAGE_EXTRACT_AND_RUN=1: the fallback where FUSE is missing) extracts to `$TMPDIR/appimage_extracted_<MD5 of
the file>`, runs AppRun, and **leaves the folder** when the program exits -- also for a minimal AppImage whose AppRun
only prints (built here with the same runtime and mksquashfs), with NO_CLEANUP unset; no "Failed to clean up"
message. Upstream: the type-2 runtime leaves extractions in /tmp (cleanup only on request, since two instances share
one folder by hash). The folder is per file: the same AppImage reuses it; **every new version leaves another full
copy** (the launcher AppImage 449 MB, a game AppImage ~108 MB). On a /tmp that is a tmpfs, that is RAM and then swap
until the next reboot. Seven such folders from this milestone's tests had accumulated on this laptop. FUSE runs
(the normal case) are not affected; the tarball is not affected.

## 32. Disk space and clean-up (2026-11-19) [M]

Rules in CLAUDE.md ("Development artifacts and disk space"). First pass: build/ 28 GB -> 13 GB (the drive 351 -> 366 GB
free); /tmp (tmpfs) 3.5 GB -> 0.2 GB. Removed: the old packages' qualification installs and the update test's work
(10.4 GB; their qualification.json kept in build/evidence/qual), the first P2 build tree (build/linux/dev1, 2.0 GB,
superseded by rel1), the superseded reference-scene runs (failed first attempts, the reverted bias experiment, the
validation duplicate: ~1.3 GB; the latest run of each scene kept for A22), four copied shader folders and two
obsolete preparations, 45 test runs whose results are recorded here (761 -> 18 MB: the 15 kept -- the prepared
resolution matrix, validation, river, bosses, the opening, the two final ending runs -- without screenshots, their
logs xz-compressed), the credits recordings packed into build/evidence/credits-stalls-2026-11-19.tar.xz (15 KB), seven
extract-and-run folders in /tmp. Kept: the dev build rel1, the install `inst 1`, the current preparation prep-rel2,
the disc copy, the checkpoints, the baselines, the dependency caches.

**Root cause** (2026-11-19, the minimal AppImage, gdb as the tracer -- strace is not installed and no package was
added): after its child exits, the runtime's parent makes **no file system call** before exit_group(0). Stopped at
the cleanup branch of the prebuilt binary (disassembly: getenv at 0x2e526, nftw at 0x79201 called from 0x2e545, the
pinned source's runtime.c 1661-1667): `getenv("NO_CLEANUP")` returns NULL, `rm_recursive()` calls
`nftw(prefix, rm_recursive_callback, 0, FTW_DEPTH | FTW_MOUNT | FTW_PHYS)` -- **fd_limit 0** -- and nftw returns 0 at
once. The runtime is statically linked with **musl**, whose nftw begins `if (fd_limit <= 0) return 0;`: nothing is
walked or removed, and "success" is returned, so no "Failed to clean up" message either. (glibc's nftw does not
return early on 0, which would explain why upstream's cleanup appears to work in some builds.) Not our application:
the minimal AppImage (AppRun = `echo`) behaves the same. Setting TMPDIR from the launcher could not help: the runtime
extracts before any of our code runs.

**The owner's decision (2026-11-19)**: documented, not worked around -- the official pinned runtime is kept unmodified;
no upstream issue or pull request is filed on the owner's behalf; no cleanup of our own (no directory scanning);
docs/LINUX_PACKAGING.md ("Without FUSE") and the packages' README.txt recommend the tarball where FUSE is missing and
describe the leftover; the runtime is reevaluated at each dependency update (docs/LINUX_PACKAGING.md section 7's
table). Not a blocker for the Linux beta.

## 33. AMD against NVIDIA D24 and D32: the reference scenes separated (2026-11-19) [M, I]

Windows' runs of a05cda3 (RTX 3070, driver 616.92; tests/rpcs3_reference.py as committed, thresholds unchanged; each
scene D24, forced D32 -- DOTHEROES_PPFX_DEPTH_D32=1 --, D24 again, and the beta.2-era build): every scene within the
accepted figures with D24; the beta.2-era build the same. Their report and pictures stay local (build/evidence/
windows-ref: game output, never committed). Set against this machine's (sections 26, 30):

| Difference on AMD | NVIDIA D24 | NVIDIA D32 | Attributed to |
|---|---|---|---|
| king: the shadow map 59% within two 2^-24 steps (mean 2.8e-6) | 100% | 58.6%, **mean 0.1009**: 37.7% of texels > 0.01 farther, the nearest depth 0.578 against 0.474 | **D32**, at two magnitudes: NVIDIA's float bias pushes casters far back; RADV's stays bias-sized |
| corridor: the lit scene ~0.005 brighter, spread over the walls and pillars on both sides (scene far 28%) | within | one large connected region unshadowed (the right-hand wall, top right to bottom right), scene mean 0.023, far 17.9% | **not the same defect** [M: the pictures]: NVIDIA's D32 loses a whole caster; AMD's difference has no such region. AMD's: **AMD/RADV**, with the reflection below [I] |
| corridor: the reflection 2.0/1.0/2.8 | within | within (0.08) | **AMD/RADV** |
| entry hall: the reflection 1.4/0.7/1.8, combined far 17% | within | within | **AMD/RADV** |
| editor: the grid lines (blue mean 1.1, 0.19% of pixels) | within | within | **AMD/RADV**: non-strict lines (strictLines false) |

**Ruled out on AMD** (sections 26, 30): reduced precision (no RelaxedPrecision in any module), the shadow comparison's
reference (the translator compares in the shader with `saturate(coordinate.z)`: the same for unorm and float
formats), the bias (patches/experiments/m11_4-depth-bias-control: the shadow map's mean 2.8e-6 -> 6.7e-7, the corridor
and entry hall unchanged), the code between beta.2 and a05cda3 (NVIDIA: no rendering change).

**The depth-bias experiment's relevance**: it is the mechanism for NVIDIA's large D32 error (Vulkan's float bias
r = 2^(e-23), e of the primitive's largest depth, which passes 1 under the game's depth clamp; the exact 2^-24
representation replaces it) -- but D32 is used on NVIDIA only when forced, so no NVIDIA player meets it. On AMD (every
AMD GPU lacks D24_UNORM_S8 in Vulkan: desktop Radeon and the Steam Deck) it reduces a small error; it does not touch
what is visibly different there. Untested on NVIDIA with D32: the one run that would show whether it closes NVIDIA's
large error (the patch, `PS3RECOMP_VK_GPL=1 DOTHEROES_GPL=0`, DOTHEROES_PPFX_DEPTH_D32=1).

**For parity**:
1. A device using D32 (AMD) is safe from NVIDIA's large error, by RADV's bias; parity of the shadow map's last steps
   would need the exact representation (VK_EXT_depth_bias_control) -- a Vulkan 1.1 instance on every system: a shared
   change.
2. The editor's lines on non-strict devices: rectangular lines (VK_EXT_line_rasterization) -- the same instance
   requirement.
3. The AMD/RADV reflection and the corridor's spread brightening: cause not found yet -- the r565 floor reflection's
   pass (its sampling and filtering, the reflected scene's depth clamp on RADV) to be compared on AMD alone, pass by
   pass, with the corridor's lit scene; no fix proposed until a cause is shown.

**Before beta.3**: not recommended. The visible result on AMD matches RPCS3 to the eye in all five scenes (faint,
spread differences; nothing missing or misplaced), NVIDIA's default (D24) is within every accepted figure, and items
1-2 change every system's Vulkan start (the instance version), which needs the Windows regression review first.
Proposed for after the beta: the Windows run of the bias patch with D32 (no change made: evidence only), then items
1-2 as one shared change behind that review, and item 3's investigation on AMD. The thresholds stay as they are: the
AMD scenes stay "over" in the record until the causes are fixed or understood.

## 34. The packages' licence audit (2026-11-19) [M]

tests/package_license_audit.py (new): every shared library, program and static library in a Linux package matched to
its component, each component to the licence texts and (LGPL/GPL) corresponding sources it must carry; anything
unaccounted for fails. On the packages built before: **5 findings** -- the worker's Python carries OpenSSL 3.5
(Apache-2.0), libffi (MIT), bzip2, xz's liblzma, and in CPython itself expat, libmpdec and HACL*, whose notices were
missing (make_bundle took Python's licence to carry them, which the Windows installer's does and a Python built from
source does not). Fixed: tools/linux_build_env keeps OpenSSL's LICENSE.txt and CPython's Doc/license.rst; make_bundle
ships them with Debian 11's copyright files of libffi, bzip2 and xz, and refuses to package without them. The final
packages: folder, tar.xz and AppImage each **121 binaries in 16 components, 0 findings**; boundary and privacy checks
pass on all three (tar.xz ac686c74..., AppImage 7c557d65...).

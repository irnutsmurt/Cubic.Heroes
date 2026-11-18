# M11.4: the native Linux port and the first AMD validation -- plan and checkpoint

Begun 2026-11-18 on the owner's Linux laptop. Windows 0.2.0-beta.2 is the functional baseline: this is a platform
port of the existing implementation, not a rewrite. Linux support is additive -- the same repository, source tree
and `main` branch build and package both systems, and nothing here may break, replace or slow the Windows build.
Evidence marks as in the other plans: [C] read in the code, [M] measured, [I] inferred, [H] a hypothesis to check.

## Checkpoint (kept current; the newest state first)

- **Stage** (2026-11-18): **P0 done** (onboarding, safeguards verified on Linux); this plan written; P1 next.
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
| A2 | The desktop guard enforced on Linux (X11 and Wayland) | NOT DONE | P1 |
| A3 | Native Linux dev build boots and plays (offscreen) | NOT DONE | P2 |
| A4 | Linux release configuration as Windows' (D1: no lifted SPU in the release) | NOT DONE | P3 |
| A5 | Release crash record and hang report on Linux | NOT DONE | P3 |
| A6 | Audio through OpenAL Soft with the system's server; device change recovered | NOT DONE | P3 |
| A7 | Cinematics through FFmpeg | NOT DONE | P2/P8 |
| A8 | X11 and Wayland: windowed, borderless, fullscreen; the desktop's mode never changed | NOT DONE | P8 |
| A9 | Rendering resolution presets and custom; VSync; frame limits | NOT DONE | P8 |
| A10 | Controllers: detection, prompts, hot-plug, remapping; keyboard remapping | NOT DONE | P8 (hot-plug by hand) |
| A11 | The in-game overlay, performance overlays, developer tools, diagnostics and log export | NOT DONE | P8 |
| A12 | The public CLI and exit codes (docs/CLI.md), `SIGTERM` reaching the game | NOT DONE | P4 |
| A13 | Graphical launcher: install from the disc, local recompilation, shader preparation | NOT DONE | P4/P9 |
| A14 | Signed updates, repair and rollback; the right package per OS and architecture | NOT DONE | P4/P9 |
| A15 | Portable root: spaces, Unicode, relocation, another drive, no FUSE (tarball) | NOT DONE | P4/P9 |
| A16 | Launcher AppImage and tarball, equivalent | NOT DONE | P5 |
| A17 | The locally generated thin game AppImage: relocatable, regenerated on update and rollback, never distributed | NOT DONE | P5 |
| A18 | glibc 2.31 baseline verified on actual supported distributions, not only Manjaro | NOT DONE | P5 |
| A19 | Steam, Lutris and Heroic through the CLI | NOT DONE | P9 |
| A20 | CI: Windows and Linux builds, tests, boundary and privacy checks | NOT DONE | P6 |
| A21 | Windows build and behaviour unbroken | NOT DONE | P6 + section 7 |
| A22 | AMD rendering matches the Windows references; validation layers clean | NOT DONE | P7 |
| A23 | Performance measured and reported (target 60 fps) | NOT DONE | P7 |
| A24 | 166 maps, bosses, gameplay, audio, saves on Linux | NOT DONE | P8 |
| A25 | Release qualification from the Linux packages | NOT DONE | P9 |
| A26 | Licences, boundary, privacy audits of the Linux packages; docs and README | NOT DONE | P9 |

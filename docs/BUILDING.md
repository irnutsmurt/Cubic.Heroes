# Building from source

For developers. A clone builds the tools and the runtime -- not the game, which still needs your own disc. The
developer pipeline in detail: [SETUP.md](SETUP.md).

- Windows: Python 3.12 in `build/venv` (with `pycryptodome`, `pillow`, `pyinstaller`), CMake, Ninja, Zig 0.17 in
  `build/toolchain/`, the Vulkan SDK.
- Linux: Python 3.10+ with `pycryptodome`, CMake 4, Ninja, Zig 0.17 (`tools/fetch_zig.py`) and the system's X11,
  Wayland, udev, PulseAudio and ALSA headers; the release packages are built in the pinned container of
  `tools/linux_package.py` (glibc 2.31 baseline) ([LINUX_PACKAGING.md](LINUX_PACKAGING.md)).

```sh
python3 tools/pipeline.py --iso "<your BLUS-30490>.iso" --keys <keys.txt>   # extract, decrypt, lift into build/gen
cmake -S port -B build/port -G Ninja -DPS3RECOMP_RSX_VULKAN=ON
cmake --build build/port
build/port/dotheroes build/disc/PS3_GAME/USRDIR/EBOOT.elf
```

The pipeline clones [ps3recomp](https://github.com/sp00nznet/ps3recomp) at a pinned revision and applies this
project's patch series (`patches/ps3recomp/`). The downloads: `tools/build_deps.py`, then
`tools/make_bundle.py --prebuilt` (the launcher with `tools/build_launcher.py`). Before every push the pre-push hook
runs `tools/boundary_check.py` and `tools/push_guard.py` (`git config core.hooksPath tools/githooks`).

| Path | What |
|---|---|
| `launcher/` | the launcher, `CubicHeroes` (SDL2 and Dear ImGui), and its command line |
| `setup_app.py`, `tools/worker.py` | the worker the launcher runs: install, update, prepare, diagnostics |
| `tools/` | the recompilation pipeline, the build with Zig, the downloads, the checks |
| `patches/ps3recomp/` | this project's changes to the ps3recomp recompiler and runtime |
| `port/` | the native program: start-up, the native renderer's hooks, physics, audio, the in-game menu, diagnostics |
| `config/` | the exact game version (hashes and the game files' manifest), the recompiler's inputs, the settings schema, the release keys |
| `tests/` | test harnesses, scenarios and the unit tests |
| `docs/` | design records, milestones, validation, the command line |

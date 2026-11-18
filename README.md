# Cubic.Heroes — 3D Dot Game Heroes Recomp

**Cubic.Heroes** is a native PC port (Windows and Linux) of the **US
PlayStation 3 release of 3D Dot Game Heroes**, built by **statically
recompiling** the game's own PPU and SPU machine code to C/C++ and pairing it
with a native runtime: a Vulkan renderer for its RSX graphics work, SDL2 input
and audio, and the PS3 system libraries it calls reimplemented on the PC. It
is not an emulator: no PS3 code is interpreted or JIT-compiled while you play.

This repository was called `3D-Dot-Game-Heroes-PS3-Recomp` until November
2026; links to the old name lead here, and setup apps downloaded before the
rename still find new versions.

> **Built 100% with AI.** All of this project's code, from the pipeline,
> runtime changes and patches to the setup app and build tooling, was written
> by AI ([Claude Code](https://claude.com/claude-code), Anthropic's Claude),
> with a human directing and testing it.

**Status: being qualified for a first Windows beta.** The game plays on Windows from its title screen to its
ending. What has been validated, area by area, and what remains is in
[docs/M11_VALIDATION.md](docs/M11_VALIDATION.md); the release checklist and the known issues in
[docs/RELEASE_QUALIFICATION.md](docs/RELEASE_QUALIFICATION.md); the milestones in
[docs/MILESTONES.md](docs/MILESTONES.md). The Linux package is planned
([docs/LINUX_PACKAGING.md](docs/LINUX_PACKAGING.md)).

## You need your own copy of the game

**Use your own legally acquired disc of 3D Dot Game Heroes for the PS3 (US
release).** This project does not include the game, and it does not tell you
where to download it or its keys.

Only this release is supported:

| | |
|---|---|
| Title | 3D Dot Game Heroes (USA), PlayStation 3 |
| Product number | BLUS-30490 |
| Version | 01.00 |

Other releases are rejected: the recompilation is tied to this exact build's
code. The setup checks your disc (SHA-256) before doing anything else.

## This repository contains no game data

This project distributes the **tools that rebuild the game from your own
disc**, not the game. Nothing from the game ships here: no code, assets, keys
or recompiled output. The setup reads your disc, verifies it, decrypts the
game's program with your key, recompiles it and builds the executable on your
own machine -- including the native versions of the game's SPU tasks, which are
generated from your own copy's recompiled code. Everything derived from the game
stays on your machine (`build/` in a checkout, or the install folder the setup
app creates) and must not be redistributed. Nor does any proprietary
middleware's code ship here. The game's audio runs on OpenAL Soft and its movies
on FFmpeg; the audio middleware's and the video codec's code are left out of your
recompiled game altogether (their functions are recompiled as traps:
`docs/OPENAL_MIGRATION_PLAN.md`, `docs/FFMPEG_MIGRATION_PLAN.md`).

What the repository and the setup's downloads hold: the recompilation tools and
the setup, this project's independently written runtime code, the open-source
libraries it builds on (Vulkan, OpenAL Soft, SDL2, ...), configuration,
compatibility patches, tests and documentation. What they never hold: the
game's code (extracted, lifted or translated), a built game or any object made
from it, the game's assets, sounds, music, textures or movies, proprietary
middleware, decryption keys or firmware, or a player's saves. A clone without the game builds the
tools and the runtime, not a playable game. `tools/boundary_check.py` checks
this: every push (the pre-push hook: the tree of each pushed commit), the
setup's download before it is archived and after, and a developer's working
tree (`tools/boundary_check.py tree`: what `git add -A` would add, and that the
build's outputs and any save or PS3 file-system folder beside the checkout
are ignored).

## 1. Dump your disc

You need a dump of your disc, either:

- a **decrypted ISO** (3,332,812,800 bytes), as made by a PS3 running homebrew
  backup software in "decrypted ISO" mode;
- an **encrypted ISO** (the same size: a raw Blu-ray rip) with its **disc key**:
  a `.dkey` file, the 32-character key typed in, or an IRD file for the disc
  (an IRD's key is converted with the `D1` line of your own keys file). The
  setup detects an encrypted image, asks for the key only then, checks it
  before anything is written, and decrypts as it extracts; the ISO itself is
  never changed, and the key is never copied or logged; or
- the disc's **folder** (the one holding `PS3_GAME/`), as a PS3 or a PC
  Blu-ray dumper writes it.

The game's program, `PS3_GAME/USRDIR/EBOOT.BIN`, is encrypted on every copy.
The setup decrypts it with **your own keys file** (`APP <revision> <erk>
<riv>` lines; this game needs revision `0001`), or takes an `EBOOT.elf` you
decrypted yourself, for example with RPCS3's *Utilities → Decrypt PS3
Binaries* using your own console's firmware. Either way the result is checked
against the known SHA-256.

## 2. Install

Download **Cubic.Heroes** for your system from the
[Releases](https://github.com/irnutsmurt/Cubic.Heroes/releases) page and unpack it into a folder of your choice
(for example `D:\Games\Cubic.Heroes`). Run **CubicHeroes** (`CubicHeroes.exe` on Windows): the launcher, made to be
used with a controller as well as a keyboard and mouse. The first time, it asks for your disc (ISO or folder) and your
keys file and recompiles the game on your PC: expect 10-30 minutes depending on your CPU and about 6 GB of free space.
Nothing else needs installing: the package carries its own C/C++ compiler (Zig), SDL2, OpenAL Soft, FFmpeg and the
port's runtime, prebuilt. The game needs a Vulkan graphics driver (Windows 10 or 11, 64-bit).

**One folder holds everything.** The folder you unpacked into is the installation: the recompiled game, your saves
and their backups, settings, shader caches, logs and updates all stay inside it -- nothing is written to `AppData` by
default. Move or copy the folder whole and it keeps working; another folder can be chosen
(`CubicHeroes --install-dir PATH`, or the launcher's settings). Your disc image and keys file are only read, never
changed. An installation made by an earlier version (in `%LOCALAPPDATA%`) is found on the first start and offered:
copied into the new folder and checked file by file, the earlier folder left as it was.

**Updating:** the launcher looks for new versions and updates itself in place, replacing only its own files: your
installation stays. The game is rebuilt only when the new version's tools changed (no disc needed), your saves are
backed up first, and the previous build is kept so you can go back to it.

**Other launchers** (Steam as a non-Steam game, Playnite, LaunchBox, ...): `CubicHeroes --play` starts the game
directly, without the launcher's window, and ends with it; `--status --json`, a headless `--install`, `--update` and
`--prepare-shaders` are there for scripts. Everything, with exit codes and examples: [docs/CLI.md](docs/CLI.md).

### From source (developers)

Linux: Python 3.10+ with `pycryptodome`, CMake, Ninja, clang, SDL2, Vulkan
headers, glslang. See [docs/SETUP.md](docs/SETUP.md).

```sh
python3 tools/pipeline.py --iso "<your BLUS-30490>.iso" --keys <keys.txt>
cmake -S port -B build/port -G Ninja -DPS3RECOMP_RSX_VULKAN=ON
cmake --build build/port
build/port/dotheroes build/disc/PS3_GAME/USRDIR/EBOOT.elf
```

The pipeline clones [ps3recomp](https://github.com/sp00nznet/ps3recomp) at a
pinned revision and applies this project's patch series
(`patches/ps3recomp/`), then extracts, decrypts and lifts the game into
`build/gen/`.

Making the downloads: `tools/build_deps.py`, then `tools/make_bundle.py --prebuilt` (Zig 0.17 in
`build/toolchain/`; on Windows the checkout's `build/venv` with PyInstaller, from Linux Windows Python with
PyInstaller in the Wine prefix `build/wine`); the launcher with `tools/build_launcher.py`.

## 3. Play

| Keyboard | PS3 |
|---|---|
| Arrows | D-pad |
| Z / X / A / S | Cross / Circle / Square / Triangle |
| Q / W | L1 / R1 |
| 1 / 2 | L2 / R2 |
| 3 / 4 | L3 / R3 |
| Enter | Start |
| Tab | Select |

Gamepads work too (anything SDL2 recognises), plugged in or out while the game runs.

**The in-game menu**: hold **L3 + R3** for half a second (another combination can be chosen), or press **F1** or
**Esc**. The game pauses while it is open. It has Display & Graphics, Audio, Controls (button prompts, the
shortcut, and your own controller buttons and keyboard keys), Performance, Return to Launcher and Exit Game.
Pressing L3 or R3 on its own still reaches the game (the character editor's undo and redo); only the held pair
opens the menu. The game's own START menu is unchanged. The title menu's last item is **QUIT GAME** (in place of
the PS3's INSTALL): it asks first, Cancel by default, then quits -- back to the launcher if it started the game.

**Settings** are shared by the launcher (*Settings*) and the in-game menu, kept in the install's
`config/dotheroes.cfg`. The main ones:

| Setting | Values |
|---|---|
| Display mode (`DOTHEROES_DISPLAY_MODE`) | windowed (resizable; its size and place remembered), borderless, exclusive fullscreen (at your display's own resolution and refresh rate -- the display's mode is never changed) |
| VSync (`DOTHEROES_VSYNC`) | on / off, independent of the frame limit |
| Frame limit (`DOTHEROES_FPS`) | 30 (the original), 60 (default), 75, 100, 120, 144, 165, 240, your display's rate, a custom number, unlimited. The game's logic runs on measured time, so the frame rate never changes its speed. |
| Volumes (`DOTHEROES_VOLUME_MASTER`, `_MUSIC`, `_SFX`, `DOTHEROES_MUTE`) | 0-100 each, applied at once |
| Skip the start-up logos (`DOTHEROES_SKIP_LOGOS`) | on (default): straight to the title screen; the new game's opening is kept |
| Performance overlay (`DOTHEROES_PERF_OVERLAY`) | off, the frame rate, or the frame rate and frame times |

Saves are kept in the install folder (`data/gamedata`).

**Developer mode** (`DOTHEROES_DEVELOPER_MODE`, Settings, Diagnostics; off by default) is for testing the port: the
in-game menu gains a Developer page -- warp to any map or entry point, the player's life, magic and money,
invulnerability and no death, the story's progress, flags and items, bosses, any enemy type and events, pipeline
coverage and frame times -- all through the game's own functions. While it is on, the game plays on separate test saves
(`data/devmode/gamedata`) and shows a "DEVELOPER MODE" tag; your own saves are never used or changed. Turn it off to
play normally again: with it off, none of it is active, and the older test switches that change the game are
ignored. The design and its tests: [docs/M11_PLAN.md](docs/M11_PLAN.md).

## Layout

| Path | What |
|---|---|
| `launcher/` | the launcher, CubicHeroes (SDL2 and Dear ImGui) and its command line |
| `setup_app.py` | the worker app the launcher runs (`--worker`; `tools/worker.py`) |
| `tools/setup_pipeline.py` | the setup's steps: verify, extract, decrypt, recompile, build |
| `tools/pipeline.py` | the developer pipeline (into `build/`) |
| `tools/zig_build.py`, `tools/build_deps.py`, `tools/make_bundle.py` | building with Zig; the downloads |
| `patches/ps3recomp/` | this project's changes to the ps3recomp runtime and tools |
| `config/` | the exact game version (hashes) and per-game inputs to the recompiler |
| `port/` | the native program: start-up, frame clock, patch switches, diagnostics |
| `tests/` | isolated test harnesses and input scripts |
| `docs/` | feasibility report, milestones, setup |
| `build/` | **your** extracted disc and all derived output (never commit) |

## License

This project's code is licensed under the **GNU General Public License v3.0**
(see [`LICENSE`](LICENSE)).

The licence covers this project's own code and images only. 3D Dot Game
Heroes belongs to its rights holders (developed by Silicon Studio, published
by FromSoftware, and by Atlus in North America); none of its code or data is
part of this repository, and the GPL grants no rights to it.

Third-party components keep their own licences:

| Component | Licence | Use |
|---|---|---|
| [ps3recomp](https://github.com/sp00nznet/ps3recomp) (patched, `patches/ps3recomp/`) | MIT | the recompiler and PS3 runtime |
| at3_standalone (in ps3recomp, from PPSSPP/FFmpeg) | LGPL-2.1 | ATRAC3plus decoding |
| [SDL2](https://libsdl.org) | zlib | window and input (and, on Linux, OpenAL Soft's audio output) |
| [Dear ImGui](https://github.com/ocornut/imgui) | MIT | the launcher's interface |
| [OpenAL Soft](https://openal-soft.org) 1.25.2 | LGPL-2.0-or-later (parts BSD-3-Clause, PFFFT's licence, and {fmt}'s and Microsoft GSL's MIT) | the game's audio (replacing FMOD: `docs/OPENAL_MIGRATION_PLAN.md`): a shared library the game loads at run time from its own folder, unmodified, built by `tools/build_deps.py` without its HRTF data set; the setup's download carries its licences and its source (`licenses/source/`) |
| [FFmpeg](https://ffmpeg.org) 9.0.2 | LGPL-2.1-or-later | the movies (replacing Bink: `docs/FFMPEG_MIGRATION_PLAN.md`): libavutil, libavcodec and libavformat with the Bink demuxer and FFmpeg's own Bink decoders only, shared libraries the game loads at run time from its own folder, unmodified, built by `tools/build_ffmpeg.py`; the setup's download carries its licence, its source, the build driver and the configuration as built (`licenses/source/`) |
| [glslang](https://github.com/KhronosGroup/glslang) | BSD-3-Clause and others | the game's shaders, translated at run time |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | Apache-2.0 / MIT | building the renderer |
| [Zig](https://ziglang.org) | MIT | the setup app's bundled compiler |
| [pycryptodome](https://www.pycryptodome.org) | BSD-2-Clause / public domain | decrypting the game's program |
| [Python](https://www.python.org) 3.12 (with OpenSSL, libffi, zlib, bzip2 and the other libraries its Windows build carries) | PSF License 2.0; the libraries' own (Apache-2.0, MIT, zlib, BSD), in its licence file | the worker's runtime, packed by PyInstaller; the package carries its licence file |
| [PyInstaller](https://pyinstaller.org)'s bootloader | GPL-2.0-or-later with its bootloader exception | starts the packed worker |
| [Bullet Physics](https://github.com/bulletphysics/bullet3) 2.74 | zlib | the physics tasks' native code: its narrowphase compiled from its source (fetched by `tools/build_deps.py`); `port/native/bullet_cd.cpp` and `bullet_solver.cpp` are altered versions of its algorithms and carry its notice |
| [DejaVu Sans](https://dejavu-fonts.github.io) (`port/assets/ui/`) | Bitstream Vera / DejaVu font licence (`LICENSE-DejaVuSans.txt`) | the save-data screen's text |
| [Kenney input prompts](https://kenney.nl) (`port/assets/ui/`) | CC0 (`LICENSE-Kenney.txt`) | the save-data screen's button prompts |

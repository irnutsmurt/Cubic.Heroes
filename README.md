# 3D Dot Game Heroes — native PC recompilation (PS3)

A native PC port (Windows and Linux) of the **US PlayStation 3 release of 3D
Dot Game Heroes**, built by **statically recompiling** the game's own PPU and
SPU machine code to C/C++ and pairing it with a native runtime: a Vulkan
renderer for its RSX graphics work, SDL2 input and audio, and the PS3 system
libraries it calls reimplemented on the PC. It is not an emulator: no PS3 code
is interpreted or JIT-compiled while you play.

> **Built 100% with AI.** All of this project's code, from the pipeline,
> runtime changes and patches to the setup app and build tooling, was written
> by AI ([Claude Code](https://claude.com/claude-code), Anthropic's Claude),
> with a human directing and testing it.

**Status: work in progress.** The game boots, plays its movies and music, and
is playable from the title screen through the opening into the overworld on
Linux. The Windows build is new. Known issues and milestones are in
[docs/MILESTONES.md](docs/MILESTONES.md).

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

Nothing from the game ships here: no code, assets, keys or recompiled output.
The setup reads your disc, verifies it, decrypts the game's program with your
key, recompiles it and builds the executable on your own machine. Everything
derived from the game stays on your machine (`build/` in a checkout, or the
install folder the setup app creates) and must not be redistributed.

## 1. Dump your disc

You need the disc as a **decrypted** dump, either:

- a **decrypted ISO** (3,332,812,800 bytes), as made by a PS3 running homebrew
  backup software in "decrypted ISO" mode, or by decrypting a Blu-ray rip with
  its disc key; or
- the disc's **folder** (the one holding `PS3_GAME/`), as a PS3 or a PC
  Blu-ray dumper writes it.

The game's program, `PS3_GAME/USRDIR/EBOOT.BIN`, is encrypted on every copy.
The setup decrypts it with **your own keys file** (`APP <revision> <erk>
<riv>` lines; this game needs revision `0001`), or takes an `EBOOT.elf` you
decrypted yourself, for example with RPCS3's *Utilities → Decrypt PS3
Binaries* using your own console's firmware. Either way the result is checked
against the known SHA-256.

## 2. Install

### The easy way: the setup app

Download `DotHeroes-Setup` for your system from the
[Releases](https://github.com/irnutsmurt/Cubic.Heroes/releases)
page, unpack it and run it. Choose your disc (ISO or folder), your keys file
(or `EBOOT.elf`) and an install folder, press **Convert**, then **Play**. It
needs nothing else installed: the app carries its own C/C++ compiler (Zig),
SDL2 and the port's runtime, prebuilt. Recompiling takes a while (the game is
large: expect 10-30 minutes depending on your CPU, and about 6 GB of free
space). The game needs a Vulkan graphics driver: on Windows, Windows 10 or 11;
on Linux, glibc 2.31 or newer (Ubuntu 20.04, Debian 11, Fedora 32 or later).

`DotHeroes-Setup --convert DISC KEYS FOLDER` does the same without the window,
and `--update FOLDER` brings an install up to date.

**Updating:** run the setup app again. It finds your install and offers
**Update**: no disc needed, your saves and settings stay, and only what the
new version changed is redone. The app tells you when a new version of itself
is out.

The app adds the game to your Start Menu (Windows) or app menu (Linux), and to
your desktop if you leave that ticked. **Add to Steam** puts it in your Steam
library as a non-Steam game.

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

Making the setup app's downloads (both platforms, from Linux):
`tools/build_deps.py`, then `tools/make_bundle.py --prebuilt` (Zig 0.17 in
`build/toolchain/`, Windows Python with PyInstaller in the Wine prefix
`build/wine`).

## 3. Play

| Keyboard | PS3 |
|---|---|
| Arrows | D-pad |
| Z / X / A / S | Cross / Circle / Square / Triangle |
| Q / W | L1 / R1 |
| 1 / 2 | L2 / R2 |
| Enter | Start |
| Tab | Select |

Gamepads work too (anything SDL2 recognises).

**Settings** are in the setup app (*Game settings*) and in `dotheroes.cfg`
next to the game:

| Setting | Values |
|---|---|
| `DOTHEROES_FPS` | `60` (default), `30` (the original cap) or `0` (no limit). The game's logic runs on measured time, so a higher frame rate does not speed it up. |
| `PS3RECOMP_VK_FULLSCREEN` | `1` fullscreen, `0` window |

Saves are kept in the install folder (`data/gamedata`).

## Layout

| Path | What |
|---|---|
| `setup_app.py` | the setup app (Tkinter) |
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
| [SDL2](https://libsdl.org) | zlib | window, input, audio |
| [glslang](https://github.com/KhronosGroup/glslang) | BSD-3-Clause and others | the game's shaders, translated at run time |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | Apache-2.0 / MIT | building the renderer |
| [Zig](https://ziglang.org) | MIT | the setup app's bundled compiler |
| [pycryptodome](https://www.pycryptodome.org) | BSD-2-Clause / public domain | decrypting the game's program |

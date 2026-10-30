# 3D Dot Game Heroes — native PC recompilation

A native Windows/Linux port of *3D Dot Game Heroes* (PS3, BLUS-30490) built by
ahead-of-time recompilation of the game's own PPU and SPU code, a Vulkan
renderer for its RSX workload, and native audio/video output.

**Status:** Milestone 1 (analysis and feasibility). See
[docs/FEASIBILITY.md](docs/FEASIBILITY.md) and [docs/MILESTONES.md](docs/MILESTONES.md).

## What this repository contains

Only original code: tools, runtime integration, tests and documentation. It
contains **no** game code, game assets, firmware, encryption keys, or code
generated from the game. Everything derived from your disc is produced locally
under `build/` (git-ignored).

| Path | Contents |
|---|---|
| `tools/` | Disc extraction, SELF decryption (keys supplied by the user), analysis |
| `config/` | The exact game version this port targets (hashes), and per-game lift inputs: `sjlj.json` (guest setjmp/longjmp), `insn_patches.json` (guarded instruction patches), `spu_relocatable.json` (SPU plugins loaded at run-time addresses) |
| `patches/ps3recomp/` | Our patch series on the pinned ps3recomp revision, applied by the pipeline |
| `port/` | The native executable: integration code, patch guards (`game_patches.cpp`), dev diagnostics (`diag/`) |
| `tests/` | Isolated test harnesses (`boot_smoke.sh`) |
| `docs/` | Feasibility report, architecture, milestones, known limitations |
| `build/` | **Local only.** Extracted disc, decrypted ELF, generated C, binaries |

## Supported game version

Only the US disc, BLUS-30490 v01.00. Hashes are in
[config/game_version.json](config/game_version.json).

## Toolchain

The PS3 runtime comes from [ps3recomp](https://github.com/sp00nznet/ps3recomp)
(MIT), pinned to a known revision; see docs/FEASIBILITY.md for what it
provides and what this project adds.

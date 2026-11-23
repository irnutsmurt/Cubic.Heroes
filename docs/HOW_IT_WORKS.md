# How Cubic.Heroes works

What the launcher does with your disc, and what replaces the PS3's hardware. For players who want the detail; the
[README](../README.md) has what you need to install and play.

1. **Your disc is checked**: every one of the game's files against its size and SHA-256 in the verified game's
   manifest (`config/disc_manifest.json`), whatever form your dump takes (an ISO of the whole disc -- also checked by
   its own SHA-256 --, an ISO rebuilt from the disc's folder, an encrypted ISO with its disc key, or the disc's folder).
   Then the game's program is decrypted with your own keys.
2. **The program is recompiled**: each PowerPC function becomes a C++ function, compiled with a bundled Zig/Clang
   toolchain into a native x86-64 executable on your PC. Your recompiled game never leaves your machine.
3. **The PS3's hardware work is done natively** -- not emulated:

| On the PS3 | In Cubic.Heroes |
|---|---|
| RSX graphics chip, its command buffer and the game's GCM graphics backend | a native Vulkan renderer fed from the game's own draw data; no command stream is generated or decoded |
| Shader programs (NV40 microcode) | translated to SPIR-V and prepared ahead of play |
| SPU co-processor tasks (physics, water, cloth, decompression) | native C++ (the physics derived from Bullet 2.74's algorithms; the water and cloth generated from your copy's code) |
| The game's audio middleware | a native sound manager on OpenAL Soft (the middleware's code is not compiled) |
| Bink video | FFmpeg's open-source Bink decoder |
| The PS3's pad, file system, save data, threads and timers | native implementations of the system interfaces the game calls |

**What remains PS3-shaped**, to be precise: the recompiled game still runs on its own big-endian memory layout and
calls the PS3 system interfaces it was written against (memory and display-buffer mapping, flip status, pad, file
system, save data, threads, locks and events); those interfaces are implemented natively on the PC, not emulated. The
game also still creates the (now empty) SPU task-scheduler instances it expects; no task is ever given to them. The
recompiler and runtime are [ps3recomp](https://github.com/sp00nznet/ps3recomp), patched by this project
(`patches/ps3recomp/`).

## Features in detail

**The game, native**
- **Static recompilation to x86-64**: the game's PPU program is translated ahead of time to C++ and compiled on your PC
  for your CPU; no interpreter, no just-in-time compiler.
- **A native Vulkan renderer**: the game's PS3 graphics backend and its post-processing are drawn natively; the PS3's
  graphics command stream is not generated or decoded at all. The game's shaders are translated to SPIR-V.
- **Native physics and SPU workloads**: collision, the physics solver, the map packs' decompression, the water and the
  hero's waving cloth run as native code; no SPU program runs. (The water's and the cloth's native code is generated on
  your PC from your own copy.)
- **OpenAL Soft audio** in place of the game's proprietary audio middleware, and **FFmpeg** for the cinematics -- the
  middleware's code is left out of your recompiled game entirely.
- **Comprehensive shader and pipeline preparation**: every shader and pipeline the game uses (2,458 on the reference
  PC) is prepared ahead of play, so new places don't stutter.

**Launcher and installation**
- **Portable installation**: everything stays in one folder -- the recompiled game, saves and backups, settings, shader
  caches, logs. Move it and it keeps working.
- **Direct play from the command line**, and **third-party launcher integration**: `CubicHeroes --play` starts the
  game without the launcher's window and exits with it; `--status --json` and a headless `--install` for scripts;
  "Add to Steam" and Start-menu shortcuts (on Linux, Heroic and Lutris too).
- **Signed updates and rollback**: updates are offered only if signed with the project's release key, and every
  download is checked against that signature; the game is rebuilt only when something it is made of changed; the
  previous build is kept to go back to. Since 0.2.0-beta.4 the launcher installs its own updates when it restarts,
  and keeps the launcher it replaced to go back to ([UPDATING.md](UPDATING.md)).
- **Save backups and restoration**: automatic before every update, and on demand.
- **Diagnostics and crash reporting**: crash and hang reports, logs, and a diagnostics export that removes your name,
  paths and e-mail addresses and never includes saves, keys or game files.
- **Optional developer mode** for testers: warp anywhere, set the story's progress, spawn enemies and bosses -- on test
  saves of its own, never yours, and fully off unless switched on.

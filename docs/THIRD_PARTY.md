# Third-party components

This project's own code is licensed under the **GNU General Public License v3.0** ([`LICENSE`](../LICENSE)). The
components below keep their own licences; their texts and, where the licence asks, their sources ship in the
download's `licenses/` folders.

| Component | Licence | Use |
|---|---|---|
| [ps3recomp](https://github.com/sp00nznet/ps3recomp) (patched, `patches/ps3recomp/`) | MIT | the recompiler and PS3 runtime |
| at3_standalone (in ps3recomp, from PPSSPP/FFmpeg) | LGPL-2.1 | ATRAC3plus decoding (its source ships in the download) |
| [SDL2](https://libsdl.org) | zlib | window and input |
| [Dear ImGui](https://github.com/ocornut/imgui) | MIT | the launcher's interface |
| [OpenAL Soft](https://openal-soft.org) 1.25.2 | LGPL-2.0-or-later (parts BSD-3-Clause, PFFFT's licence, {fmt}'s and Microsoft GSL's MIT) | the game's audio: a shared library, unmodified, its source in the download |
| [FFmpeg](https://ffmpeg.org) 9.0.2 | LGPL-2.1-or-later | the movies: avutil, avcodec and avformat with the Bink demuxer and decoders only, shared libraries, unmodified, their source and build configuration in the download |
| [glslang](https://github.com/KhronosGroup/glslang) | BSD-3-Clause and others | the game's shaders, translated |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | Apache-2.0 / MIT | building the renderer |
| [Bullet Physics](https://github.com/bulletphysics/bullet3) 2.74 | zlib | the native physics: its narrowphase compiled from source; `bullet_cd.cpp` and `bullet_solver.cpp` are altered versions of its algorithms and carry its notice |
| [Zig](https://ziglang.org) | MIT | the bundled compiler |
| [Python](https://www.python.org) 3.12 (with OpenSSL, libffi, zlib, bzip2 and the other libraries its Windows build carries) | PSF License 2.0; the libraries' own (Apache-2.0, MIT, zlib, BSD) | the worker's runtime |
| [PyInstaller](https://pyinstaller.org)'s bootloader | GPL-2.0-or-later with its bootloader exception | starts the packed worker |
| [pycryptodome](https://www.pycryptodome.org) | BSD-2-Clause / public domain | decrypting the game's program |
| [DejaVu Sans](https://dejavu-fonts.github.io) (`port/assets/ui/`) | Bitstream Vera / DejaVu font licence | interface text |
| [Kenney input prompts](https://kenney.nl) (`port/assets/ui/`) | CC0 | controller button prompts |

The licence review of every file in the repository: [LICENSE_AUDIT.md](LICENSE_AUDIT.md).

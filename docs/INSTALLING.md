# Installing Cubic.Heroes

Everything about installing, in detail. The [README](../README.md#installation) has the short version.

- [Which download?](#which-download)
- [What you need](#what-you-need): [the PC](#the-pc), [your game](#your-game), [your disc's dump](#your-discs-dump),
  [your keys](#your-keys)
- [Installing](#installing): [Windows](#windows), [Linux: the AppImage](#linux-the-appimage),
  [Linux: the tarball](#linux-the-tarball)
- [If the installation fails](#if-the-installation-fails)
- [The portable folder](#the-portable-folder)

## Which download?

Every file is on the [releases page](https://github.com/irnutsmurt/Cubic.Heroes/releases), with `SHA256SUMS.txt`
and the signed manifest (`release.json`, `release.json.sig`) to check them against.

| Your system | Download | |
|---|---|---|
| **Windows 10 or 11** (64-bit) | `Cubic.Heroes-<version>-windows-x86_64.zip` | extract it where the game should live |
| **Linux** (x86-64), most desktops | `Cubic.Heroes-<version>-x86_64.AppImage` | one file; needs FUSE (`fusermount`), which most desktop distributions have |
| **Linux** without FUSE, or if you prefer a plain folder | `Cubic.Heroes-<version>-linux-x86_64.tar.xz` | extract it where the game should live |

**Handhelds** (the Steam Deck among them) haven't been tested by us yet; on SteamOS the Linux packages are the ones to
use (in Desktop Mode; **Add to Steam** on the launcher's Install / Manage page then puts the game in Game Mode).
Reports are very welcome ([Testing status](TESTING_STATUS.md)).

## What you need

### The PC

- **Windows 10 or 11, 64-bit**, or **Linux on x86-64 with glibc 2.31 or newer** (Debian 11, Ubuntu 20.04 and
  Fedora 32 have that glibc; see [Testing status](TESTING_STATUS.md) for what has actually been tested).
- An **x86-64 CPU with SSE4.2** (x86-64-v2: Intel since 2008, AMD since 2011).
- A GPU with a **Vulkan** driver: any recent AMD, NVIDIA or Intel driver on Windows; on Linux, Mesa's (RADV for AMD,
  ANV for Intel) or NVIDIA's, with the Vulkan loader (`libvulkan1` or `vulkan-icd-loader`; installed with the driver
  on most distributions).
- About **6 GB** of free disk space.
- Nothing else to install: the launcher carries its own runtime (on Linux too: no Python or SDL from the system;
  the graphics driver and its Vulkan loader are the system's).

These are the minimum; recommended requirements will follow once more hardware has been tested.

### Your game

Only this release is supported:

| | |
|---|---|
| Title | 3D Dot Game Heroes (USA), PlayStation 3 |
| Product number | **BLUS-30490** |
| Version | 01.00 |

Other regions and releases are rejected (the recompilation is tied to this exact build's code). This project does not
include the game. It includes no keys either: when your computer has none, the launcher downloads them from public
sources ([Your keys](#your-keys)). The official v1.01 update is optional ([OFFICIAL_UPDATE.md](OFFICIAL_UPDATE.md)).

### Your disc's dump

Any of these forms -- the launcher tells which one you chose and asks only for what that form needs:

| Your dump | Also needs | |
|---|---|---|
| a **decrypted ISO** of the whole disc (3,332,812,800 bytes), as made by a PS3 running homebrew backup software in decrypted mode | -- | |
| an **ISO rebuilt from the disc's folder** (PS3 ISO Tools, makeps3iso and similar), of any size | -- | |
| an **encrypted ISO** (a raw Blu-ray rip) | its **disc key** | decrypted as it is read; the ISO itself is never changed |
| the disc's **folder** (the one holding `PS3_GAME/`) | -- | |

To choose the folder: Install / Manage, **Your disc**, Choose..., open the disc's folder (the one holding `PS3_GAME/`,
or `PS3_GAME/` itself), then **Use this folder**. You can also drop the folder onto the launcher's window.

Whatever the form, **every one of the game's files is checked** against its size and SHA-256 in the verified game's
manifest before anything is installed; a missing, changed or doubled file is refused, with its name.

### Your keys

Two different things, for two different locks:

- **The disc key** -- *only for an encrypted ISO*. It unlocks the disc image itself. A `.dkey` file (the 32-character
  key), a `.key` file (16 bytes), the 32 characters typed, or an IRD file (which needs one more line, `D1`, in your
  keys file). A decrypted ISO, a rebuilt ISO or a folder doesn't need one, and the launcher doesn't ask for it.
- **The keys for the game's program** -- *always*, and **found or downloaded by the launcher itself**. The program
  (`PS3_GAME/USRDIR/EBOOT.BIN`) is encrypted on every dump, even a decrypted ISO. It needs the APP key of revision
  `0001`. Cubic.Heroes includes no keys: if your computer has none, the launcher downloads them (below). You can also
  give your own, in any of these forms:
  - a **text keys file**: this project's `APP <revision> <erk> <riv>` lines, or the `[section]` / `name=value` keys
    files other PS3 tools use;
  - a **folder of key files**, one key per file, as PS3 tools keep them (`app-key-...` with `app-iv-...`);
  - a **ZIP archive** of either -- read as it is, never unpacked;
  - an **`EBOOT.elf` you decrypted yourself**;
  - or **your own RPCS3**, if you have it: the launcher asks it to decrypt the program, in the background, and uses
    the result. RPCS3 is never required, and nothing of it is part of Cubic.Heroes.

If you choose one in the other's place, the launcher says so. Keys are only read: never copied, logged or uploaded.

**Where the launcher looks for the keys** -- in this order, and nowhere else:
1. the keys you choose;
2. where keys were found or chosen before (only that place is remembered, never the keys);
3. **beside your disc**: in the folder that holds your ISO (or your disc folder), the files and folders whose names
   say they hold keys -- `keys.txt`, `*.keys`, `ps3keys.zip`, a `keys` folder and the like -- and an `EBOOT.elf`. No
   other folder is searched;
4. **the keys it downloaded before** for this installation (its `data/keys` folder), and otherwise **a download now**
   (a few small files, a few seconds) from these public sources, in this order:
   - [codyps/ps3keys](https://github.com/codyps/ps3keys) (the disc's v1.00 program);
   - the `data/keys` file of [aldostools/webMAN-MOD](https://github.com/aldostools/webMAN-MOD) (v1.00 too, and the
     official v1.01 update's program and package).

   Each is pinned to one commit, and every file to its size and SHA-256 (`config/key_downloads.json`). A file that
   differs is refused and removed. The files are only read as keys, never run. They stay in your installation's
   `data/keys` folder, never in a backup, a log or a diagnostics export, so a later repair works offline;
5. your RPCS3, if you pointed the launcher at it (optional: never required).

Whatever is found is used only if it turns the program into exactly the supported one (its SHA-256), so a wrong or
damaged key file can never produce a broken game.

**What that means in practice** (BLUS-30490 v01.00):

| You have | Install |
|---|---|
| the ISO (or disc folder) only, online | choose the ISO, press **Install**: the keys are downloaded and checked by themselves |
| the ISO (or disc folder) only, offline | the launcher says the keys couldn't be found automatically: connect and press **Look for the keys again** (under **Advanced**), or choose your keys there |
| the ISO, with your keys beside it (a keys file, or a folder or ZIP of key files) | choose the ISO, press **Install**: the keys are found by themselves |
| the ISO, and keys you used before | the same: the place you chose them is remembered |
| the ISO, and your RPCS3 | choose the ISO, and your RPCS3 once under **Advanced**, press **Install**: no keys needed |
| the ISO, keys elsewhere | choose them once under **Advanced** (a keys file, a key folder or ZIP, or an EBOOT.elf) |
| an encrypted ISO | as above, plus its disc key (asked for only then) |

## Installing

On every system the launcher does the same: you choose your disc (and its disc key if it is encrypted) and your keys
file, and it checks the disc, decrypts the game's program and **recompiles it on your PC** (10-30 minutes, depending
on your CPU), then prepares the shaders (a few seconds to a few minutes). Nothing of the game is downloaded: it is
built from your own files.

### Windows

1. **Download** `Cubic.Heroes-<version>-windows-x86_64.zip`.
2. **Extract** it where you want the game to live -- for example `D:\Games\`. It unpacks to a `Cubic.Heroes` folder;
   that folder *is* the installation.
3. **Run `Cubic.Heroes\CubicHeroes.exe`**, choose your disc and your keys file, and **Install**.
4. **Shaders** are prepared right after the install; the launcher's Install / Manage page shows their state and can
   prepare them again after a GPU driver change.
5. **Play.**

An installation from an earlier version in `%LOCALAPPDATA%` is found on the first start and offered: copied into the
new folder and checked file by file, the earlier folder left as it was.

### Linux: the AppImage

1. **Download** `Cubic.Heroes-<version>-x86_64.AppImage` into the folder you want the game in (for example
   `~/Games/`).
2. **Make it executable** -- `chmod +x Cubic.Heroes-<version>-x86_64.AppImage`, or the file's Properties >
   Permissions -- and **start it**.
3. Choose your disc and your keys file, and **Install**, as on Windows.

The AppImage is one read-only file: its installation is a **`Cubic.Heroes` folder created beside it** (in the example,
`~/Games/Cubic.Heroes/`), holding everything listed under [The portable folder](#the-portable-folder). **Move the
AppImage and that folder together**, keeping them side by side.

**FUSE**: an AppImage is mounted with FUSE while it runs. This one carries its own FUSE library, so it only needs the
`fusermount` program and `/dev/fuse`, which most desktop distributions have (the `fuse3` or `fuse` package; `libfuse2`
isn't needed). Without FUSE (some containers and sandboxes), use **the tarball** below. `--appimage-extract-and-run`
works too, but leaves an extracted copy (about 450 MB) in `$TMPDIR` (normally `/tmp`) until it is deleted or the
machine restarts ([LINUX_PACKAGING.md](LINUX_PACKAGING.md)).

### Linux: the tarball

1. **Download** `Cubic.Heroes-<version>-linux-x86_64.tar.xz` and **extract** it where you want the game to live:
   `tar -xJf Cubic.Heroes-<version>-linux-x86_64.tar.xz -C ~/Games`. It extracts to a `Cubic.Heroes` folder; that
   folder *is* the installation, exactly as on Windows.
2. **Run `Cubic.Heroes/CubicHeroes`**, choose your disc and your keys file, and **Install**.

The tarball needs no FUSE and leaves nothing outside its folder. The launcher (`CubicHeroes`), its runtime (`worker/`)
and its libraries (`lib/`) sit beside the installation's own folders.

## If the installation fails

The launcher says what went wrong and what to do; the most common cases:

| The launcher says | What to do |
|---|---|
| it is another game, region or version | Only BLUS-30490 version 01.00 (US) is supported; the message names what your dump is |
| the disc is damaged, or a file "doesn't match" | One of the game's files is missing, changed or incomplete in your dump: dump the disc again (or copy its folder again). An ISO of another size is fine as long as its files are right |
| the disc image is encrypted | It is a raw rip: choose its disc key too ([Your keys](#your-keys)) |
| the disc key doesn't open this disc image | The key is for another disc: check it matches this dump |
| that is a disc key, not the program's keys | Choose your keys (a keys file, a key folder or ZIP) or an `EBOOT.elf` under **Advanced**; the disc key goes in the disc key's own step |
| the keys couldn't be downloaded | Connect to the internet and press **Look for the keys again** (under **Advanced**), or choose your keys, an `EBOOT.elf`, or your RPCS3 there ([Your keys](#your-keys)) |
| no keys were found beside your disc | Put your keys beside the ISO and press **Look for the keys again**, or choose them, an `EBOOT.elf`, or your RPCS3 under **Advanced** ([Your keys](#your-keys)) |
| the keys don't open this game's program | They need the APP key of revision `0001`; or give an `EBOOT.elf` you decrypted, or use your RPCS3 |
| not enough space, or the folder can't be written | About 6 GB free is needed; extract to a folder you own (not `Program Files`) |
| the install stopped part-way | Run **Install** again: it resumes where it stopped. A failed or stopped install leaves the game you already have, your saves and your settings as they were |

Still stuck? **Diagnostics > Export Logs** (review it, then save the zip), and ask on
[Discord](https://discord.gg/ZcafYCYb7A) or [open an issue](https://github.com/irnutsmurt/Cubic.Heroes/issues/new/choose).
Every message and its fix: [TROUBLESHOOTING.md](TROUBLESHOOTING.md).

## The portable folder

Everything the game and the launcher make stays inside the `Cubic.Heroes` folder:

| Folder | What |
|---|---|
| `data/gamedata/` | **your saves** |
| `backups/saves/` | save backups (automatic before updates, and your own) |
| `config/` | settings (`dotheroes.cfg`), the window's place, the launcher's preferences |
| `builds/` | the recompiled game (the active build and the previous one) |
| `data/cache/` | shader, pipeline and compiler caches |
| `logs/` | logs; `logs/crash/` crash and hang reports |

Move the whole folder and it keeps working (to copy it, see
[The installation was copied](TROUBLESHOOTING.md#the-installation-was-copied-game-is-in-the-way)). Nothing is
written to `AppData` by default; the only things outside it are the shortcuts and Steam entry you ask for and your GPU
driver's own caches. Another folder can be used for the data (`--install-dir`, or Settings). To remove everything, use
the launcher's uninstall, or delete the folder.

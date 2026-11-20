# Cubic.Heroes: the public command line and the portable installation

**Status: M11.2 (2026-11-17).** `CubicHeroes` (`CubicHeroes.exe` on Windows) is the one program a player, Steam or
a third-party launcher or frontend needs. With no arguments it opens the launcher's window; with a command it does
that one thing and exits with a documented code, never opening a window it was not asked for. Everything it uses
-- the worker that converts and rebuilds the game, the game's own program -- is an internal detail that callers never
need to know about or start themselves.

## 1. The installation folder (portable by default)

Everything Cubic.Heroes manages lives in one folder, the **installation root**:

| Under the root | What |
|---|---|
| `CubicHeroes.exe`, `SDL2.dll`, `worker/`, `licenses/`, `LICENSE.txt`, `README.txt` | the program itself (what an update replaces) |
| `config/launcher.json` | the launcher's own preferences (its window, the last disc and keys paths chosen) |
| `config/dotheroes.cfg` | the settings (the game's and the launcher's: section 4) |
| `active.json`, `builds/<id>/` | the game's builds: the one in use and the one kept for rollback |
| `data/disc/`, `data/gen/`, `data/install.json` | the game data converted from your disc |
| `data/gamedata/` | **your saves** (and the system data the game keeps) |
| `data/devmode/` | developer mode's own test saves (never your saves) |
| `backups/saves/` | save backups (made before every update, or by you) |
| `data/cache/` | shader and pipeline caches, the compiler's object cache, its Zig caches |
| `logs/` | the game's log, the worker's logs, crash and hang reports (`logs/crash/`) |
| `downloads/` | downloaded updates (`<version>/`: the package with its signed `release.json` and `release.json.sig`), and the launcher update's own: `launcher-new/` (staged), `launcher-rollback/` (the launcher the last update replaced), `launcher-helper/` (the program that swaps them), `launcher-update.json` (an update under way), `launcher-update-last.json` (how the last one ended) |
| `data/tmp/` | temporary files of the conversion and of updates |

**By default the root is the folder `CubicHeroes` is in.** The whole folder can be moved or copied to another place
or drive: nothing inside it records where it is (the game is started through `active.json`, which names its build by
a relative path). Nothing is written to `%APPDATA%`, `%LOCALAPPDATA%` or `~/.config` by default.

The exceptions, all outside your control only when you ask for them: Start-menu, desktop and Steam shortcuts you
ask the launcher to make (they point at this folder), the graphics driver's own shader cache, and the helper that
removes the launcher when you ask it to (it runs from the system's temporary folder, since it deletes the folder it
came from).

**Another root**, in this order of precedence:

1. `--install-dir PATH` on the command line (add `--remember` to keep it for the next start);
2. the environment variable `CUBIC_INSTALL_DIR`
   (a relative path in either is taken from the current folder);
3. `cubicheroes-root.txt` beside `CubicHeroes`: one line, a folder (a relative path is taken from `CubicHeroes`'s
   folder) -- what the launcher's "Choose another install folder" and `--remember` write;
4. the folder `CubicHeroes` is in.

Paths may contain spaces and any Unicode characters. A root that can't be written (a read-only or protected folder,
a full drive) is reported at once -- the window says so; the command line exits with code 7.

**An earlier installation** (Cubic.Heroes before M11.2 kept its install in `%LOCALAPPDATA%\Cubic.Heroes`, or
`~/Games/Cubic.Heroes`, and its preferences in `%APPDATA%\CubicHeroes`): nothing is moved or deleted. Either use it
where it is (`--install-dir` that folder, with `--remember`, or the launcher's "Choose another install folder"), or
copy it into the new folder with `--migrate-from PATH` -- your saves, settings, builds and caches copied and checked,
the original left exactly as it was (delete it yourself once you are happy). The launcher offers both when it finds
one.

## 2. Commands

One command at a time; options may follow in any order. `--opt=value` and `--opt value` are the same.

| Command | What it does |
|---|---|
| *(none)*, `--launcher` | open the launcher's window |
| `--play` | start the game directly, with no launcher window; `CubicHeroes` waits for it and exits with the game's own exit status. The game runs in a job of `CubicHeroes`'s: a caller that ends `CubicHeroes` ends the game too |
| `--install --disc PATH --keys PATH` | install from your disc image (or disc folder) and keys file, headless; resumes an interrupted install. **Does nothing if the game is already installed** (`--repair` rebuilds it from the disc anyway) |
| `--update` | bring the game up to date with this version of Cubic.Heroes (after you put a newer `CubicHeroes` in the folder): your saves backed up, then rebuilt **only if the tools changed** ("Already up to date: nothing rebuilt" otherwise) |
| `--check-update` | look for a newer release on the installation's update channel (the setting `LAUNCHER_UPDATE_CHANNEL`: a beta's default includes test releases); nothing downloaded |
| `--apply-update` | **install the newer release** (0.2.0-beta.4): download it if it isn't already, check it again against the release's signature (the trusted key, a newer version, this system's package of this installation's kind, its size and SHA-256), stage it inside the folder, then **exit**: a helper replaces the launcher's own files a few seconds later, checks the new launcher (`--version`: the launcher and its worker are the new version) and keeps it -- or puts this one back if anything fails. Refused while the game runs (exit 5). Then `--update` brings the game up to date. `--status` says how it ended |
| `--rollback-launcher` | put back the launcher the last update replaced (kept in `downloads/launcher-rollback/`), the same way; the newer one is kept there in turn |
| `--prepare-shaders` | prepare the shaders and pipelines the game needs before play, headless (resumes where a stopped one left off) |
| `--rebuild-shaders` | prepare them again from scratch |
| `--status` | the installation, its build, its settings and its shader cache |
| `--set KEY=VALUE` | change a setting (repeatable). Alone: kept. With `--play`: for that session only, unless `--persist` |
| `--migrate-from PATH` | copy an existing installation into this one (section 1) |
| `--version` | the versions of Cubic.Heroes and its worker |
| `--help` | the summary of this page |

| Option | |
|---|---|
| `--install-dir PATH` | the installation root (section 1); `--remember` keeps it |
| `--disc PATH`, `--keys PATH` | for `--install`. The keys file is read by the decryption step only; its contents never appear in output, logs or arguments passed on |
| `--json` | machine-readable output (section 3) |
| `--no-gui` | never open a window: with no command it is an error (exit 2) instead of the launcher |
| `--persist` | keep `--play`'s display options and `--set` values as settings |
| `--fullscreen`, `--windowed`, `--borderless` | the display mode for `--play` (fullscreen: exclusive, at the display's current mode) |
| `--width N --height N` | the window's size for `--play` (windowed; together); the window's remembered place and size are left as they were |
| `--resolution R` | the rendering resolution for `--play`: `native` (the output's own size: the display's in fullscreen, the window's drawable in a window), `1280x720` (the original), `1920x1080`, `2560x1440`, `3840x2160`, or any size from `320x180` to `7680x4320` (kept as the custom one). The size the game's 16:9 picture is drawn at, then scaled to the window or display with its shape kept -- independent of `--width`/`--height` and the display mode; the display's own resolution is never changed. Example: `--play --windowed --width 1280 --height 720 --resolution 3840x2160` (supersampled into a small window) |
| `--fps N` | the frame limit for `--play`: 30 (the original pacing), 60, any number from 30 to 1000, `display` (the display's refresh), or 0 (uncapped) |
| `--vsync on\|off` | vertical sync for `--play` |

Session options (`--fullscreen`, `--fps`, `--resolution`, `--set` with `--play` ...) apply to that one session: the settings file is
not changed unless `--persist` is given (then they are checked and kept first, and a wrong value stops before the
game starts). Settings changed in the game's own menu during the session are kept, as always.

**Direct play** (`--play`) never opens the launcher's window, also not on errors: they are printed (stderr, or the
JSON result) and reported by the exit code. The in-game menu then offers Exit Game but not Return to Launcher, and
the title menu's QUIT GAME ends the game.

## 3. Output

`CubicHeroes` is a windowed program: run from a script or a launcher that reads its output (a pipe), its text and
JSON go there; run from a console, it writes to that console. From `cmd.exe`, use `start /wait "" CubicHeroes.exe
--status` to wait for it and read `%ERRORLEVEL%`; PowerShell: `Start-Process -Wait -PassThru` or pipe it
(`.\CubicHeroes.exe --status | Out-String`).

Without `--json`: plain sentences on stdout, errors on stderr prefixed `CubicHeroes:`.

With `--json`:

- `--status`, `--version`, `--set`, `--check-update`, a refused command: **one JSON object** on stdout.
  `"ok": true` with the data, or `"ok": false` with `exit` (the exit code), `code` (a stable error name: section 5)
  and `message` (a sentence), and `details` when there are any.
- `--install`, `--update`, `--prepare-shaders`, `--rebuild-shaders`, `--migrate-from`: the operation's **events as
  JSON lines**, as the worker sends them (docs/M9_WORKER_PROTOCOL.md section 2: `hello`, `plan`, `stage`,
  `progress` with `overall` 0-1 and `eta`, `log`, and last `result`), then, if it failed, the failure object above.
- `--play`: `{"ev":"started","pid":N,"game":PATH}` once the game runs, `{"ev":"ended","exit":N}` when it ends (with
  `"why"` for a status section 5 explains, such as 70).

**Installing an update** (0.2.0-beta.4; the window's **Restart and update** does the same):
- Nothing of the working installation changes while an update downloads or is checked: the download goes into
  `downloads/<version>/`, and the new package is unpacked into `downloads/launcher-new/` (an AppImage: copied beside
  the AppImage as `.<its name>.new`) only after every check.
- The swap replaces only the launcher's own items (section 1's first row): never `builds/`, `data/`, `config/`,
  `backups/`, `logs/` or anything else in the folder. Each step is recorded; the launcher's program is replaced last,
  in one step, so a launcher is always there to start. An AppImage installation's AppImage file is replaced in one
  step and keeps its name and place (shortcuts keep working).
- A crash or a power cut part-way is put right at the next start of `CubicHeroes` (the window or a command): the
  previous launcher put back, or a finished update kept. A command started meanwhile exits with status 5 and says
  so: run it again a moment later.
- No administrator or root rights: everything is inside the folder.

`--status --json` holds: `cubicheroes` (the version), `install_dir`, `install_dir_from` (which of section 1's rules
chose it), `writable`, `installed`, `game` (the game's program, when installed), `install` (what the folder holds:
`kind` none / partial / slots / setup-app, ...), and when installed `builds` (`active`, `previous`, `retention`,
`builds`), `shaders` (the cache's state for this GPU and driver: `cache` is `"reused"` when valid, else why not;
`comprehensive_done`; `coverage.pipelines` ...) and `settings` (each setting's value, the file's `path`,
`problems`); and `launcher_update`: `version`, `kind` (zip, tar.xz or appimage), `ready` (a downloaded newer
version that `--apply-update` would install), `rollback` (the launcher `--rollback-launcher` would put back),
`last` (how the last update of the launcher ended: `result` updated / restored / busy, `version`, `why`) and
`interrupted` (an update a crash left: the next start puts it right).

## 4. Settings

The settings are one file, `config/dotheroes.cfg`, checked against `worker/config/settings_schema.json` (the
launcher's Settings page and the game's own menu edit the same file). Any setting can be changed with `--set
KEY=VALUE`; the values are checked (a wrong one: exit 1, `code` `invalid_setting` or `unknown_setting`). The ones a
player sees:

| Key | Values |
|---|---|
| `DOTHEROES_DISPLAY_MODE` | `windowed`, `borderless`, `exclusive` |
| `DOTHEROES_RENDER_RES` | `native`, `1280x720`, `1920x1080`, `2560x1440`, `3840x2160`, `custom` (then `DOTHEROES_RENDER_RES_CUSTOM`, WxH from 320x180 to 7680x4320) |
| `DOTHEROES_VSYNC` | `1`, `0` |
| `DOTHEROES_FPS` | `60`, `30`, `75`, `100`, `120`, `144`, `165`, `240`, `display`, `0` (uncapped), `custom` (then `DOTHEROES_FPS_CUSTOM`) |
| `DOTHEROES_PERF_OVERLAY` | `off`, `fps`, `graph` |
| `DOTHEROES_VOLUME_MASTER`, `_MUSIC`, `_SFX` | 0-100 |
| `DOTHEROES_MUTE` | `1`, `0` |
| `DOTHEROES_OVERLAY_COMBO` | `l3r3`, `selectstart`, `l1r1select` |
| `DOTHEROES_OVERLAY_HINT`, `DOTHEROES_SKIP_LOGOS` | `1`, `0` |
| `LAUNCHER_DISPLAY_MODE` | `windowed`, `borderless`, `exclusive` |
| `LAUNCHER_CLOSE_ON_START`, `LAUNCHER_VSYNC` | `1`, `0` |
| `LAUNCHER_PROMPTS` | `auto` (the device used last), `xbox`, `playstation` (the game's original pictures), `nintendo`, `steam`, `generic`, `keyboard` -- the launcher's and the whole game's button prompts |
| `DOTHEROES_PAD_MAP`, `DOTHEROES_KEY_MAP` | the game's buttons on other pad buttons or keys: `Cross=Square,Square=Cross`, `Cross=Space` (the game's button = the pad's, by PlayStation names, or an SDL key name); the prompts show them |
| `LAUNCHER_UPDATE_CHANNEL` | `stable`, `prerelease` |
| `LAUNCHER_CHECK_UPDATES`, `LAUNCHER_SHADERS_AFTER_UPDATE`, `LAUNCHER_SHADERS_ON_DRIVER_CHANGE` | `1`, `0` |
| `DOTHEROES_LOG_LEVEL` | `error`, `warning`, `info`, `debug`, `trace` |

`--status --json` lists them all with their current values. Developer mode (`DOTHEROES_DEVELOPER_MODE`) keeps its
own rules: it plays on its own test saves (`data/devmode/gamedata`), never yours.

## 5. Exit codes

| Code | Means |
|---|---|
| 0 | success (`--play`: the game's own exit status -- 0 when it was quit normally) |
| 1 | the operation failed (the message and `code` say why) |
| 2 | the command line is wrong (an unknown option, a missing value, two commands at once) |
| 3 | not installed (play, update or prepare asked of a folder with no installation) |
| 4 | a prerequisite is missing or wrong (the disc image, the keys, the game's program, the worker beside `CubicHeroes`) |
| 5 | busy (another operation holds this installation: a preparation, an update, the game running) |
| 6 | cancelled (Ctrl+C, the console closed, the calling launcher ended `CubicHeroes`) |
| 7 | the installation folder can't be used (read-only, protected, or not enough free space) |

`--play` passes the game's status on; a game that crashed exits with the system's code for it (on Windows, an
NTSTATUS such as `0xC0000005`). One status of the game's own:

| Game status | Means |
|---|---|
| 70 | **the graphics didn't start in time**: the window or the graphics driver never finished starting, and the game ended itself instead of waiting with no window. Its log (`logs/game.log`) names the step that stalled ("stalled at the graphics backend: the Vulkan device (the GPU driver)", ...) and `logs/crash/startup-<time>.txt` keeps the message. `--play` prints an explanation on stderr (with `--json`: the `ended` event's `why`); the launcher's window shows it when a game it started ends so. Usually a graphics driver or window-system problem: update the driver, close overlay or recording tools, restart the computer |

The time allowed: 120 seconds on Windows (and no limit while a debugger is attached to the game), 30 seconds on Linux;
`DOTHEROES_STARTUP_TIMEOUT_S` (an environment variable, seconds; 0: no limit) changes it. It covers the window's and
the graphics device's start only: shader preparation (`--prepare-shaders`) and the game's own loading after the
graphics have started are never cut short by it.

The `code` names in JSON are the worker's (docs/M9_WORKER_PROTOCOL.md section 4) plus `usage`, `no_install`,
`no_game`, `no_worker`, `root_not_writable`.

## 6. Integrating with a launcher or frontend

Track the process you start: `CubicHeroes --play` stays running for as long as the game does and exits with its
status. Nothing needs to be started after it, and no other program needs to be known.

**Steam** (a non-Steam game): target `"D:\Games\Cubic.Heroes\CubicHeroes.exe"`, launch options `--play` (add
`--fullscreen` or `--fps 144` as you like). Steam's overlay and playtime follow the process.

**Playnite, LaunchBox, Decomp Launcher and similar**: the play action

```
"<folder>\CubicHeroes.exe" --play
```

with the install's status read once, to decide what to show:

```
"<folder>\CubicHeroes.exe" --status --json
```

(`installed`; `shaders.cache == "reused"` and `shaders.comprehensive_done` for "ready"), a headless install from the
player's own files:

```
"<folder>\CubicHeroes.exe" --install --disc "D:\Discs\3D Dot Game Heroes.iso" --keys "D:\Keys\BLUS30490.txt" --json
```

(progress from the `progress` events' `overall`), and the shaders prepared before the first play:

```
"<folder>\CubicHeroes.exe" --prepare-shaders --json
```

A frontend that keeps several installations, or keeps them outside the program's folder:

```
"<folder>\CubicHeroes.exe" --install-dir "E:\Cubic.Heroes copies\speedrun" --play --fps 30
```

**Linux** (same commands, `CubicHeroes` instead of `CubicHeroes.exe`): see docs/LINUX_PACKAGING.md for the AppImage,
whose installation root is a writable folder outside the image.

## 7. The title menu's QUIT GAME

The game's title menu (NEW GAME, CONTINUE, EDITOR, ALBUM; on a cleared game NEW GAME+ too) ends with **QUIT GAME**,
in the place the PS3 version had INSTALL. It asks first (Cancel is the default) over the title, with the controller,
the keyboard or the mouse, and then ends the game in order (the logs and the window's place written, no data lost
beyond what was not saved). Started from the launcher's window, the launcher comes back (also when it had closed
itself as the game started); played directly (`--play`, a third-party launcher), the game just ends.

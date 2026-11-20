# M9: the pipeline worker and its progress protocol

**Status: M9.0, 2026-11-13.** The launcher (docs/M9_LAUNCHER_PLAN.md, section 3.1) is a native window; the
conversion of the player's disc and every rebuild stay in the existing Python pipeline, run as a separate,
windowless process: the **worker** (tools/worker.py). This is the contract between them. Version 1.

## 1. Running it

| Where | How |
|---|---|
| a source checkout | `python tools/worker.py <command> [options]` |
| the packaged app | `<app executable> --worker <command> [options]` (setup_app.py main; the launcher's download carries it) |

The launcher starts it with no console window (Windows: `CREATE_NO_WINDOW`), stdout and stdin as pipes, and
`--control`. One operation per process; the process ends when the operation does.

| Command | Options | What it does | Its result's data |
|---|---|---|---|
| `version` | | the worker's version and this protocol's | `worker`, `protocol`, `platform`, `game` |
| `probe-disc` | `--disc PATH` | what the chosen disc image or disc folder is: PARAM.SFO's title and version, EBOOT.BIN's SHA-256, the image's size; seconds, nothing written | `kind`, `title_id`, `app_ver`, `title`, `size` |
| `probe-keys` | `--disc PATH --keys PATH` | whether the keys file (or an EBOOT.elf the player decrypted) turns this disc's program into the supported one; in memory, nothing written | `match`, `via` (`keys` or `elf`) |
| `space` | `--install DIR [--for install\|update]` | the free space on the folder's drive (the nearest existing parent) against what the operation needs | `free`, `needed`, `enough`, `checked_at` |
| `info` | `--install DIR` | what an install folder holds (M9.1: `none`, `partial` -- a first install interrupted, with the disc's steps it finished --, `setup-app` -- to migrate --, or `slots`) | `kind`, `active`, `previous`, `retention`, `builds`, `game_link`, `backups`, `version`, `disc_copy`, `lift`, `interrupted` |
| `install` | `--disc PATH --keys PATH --install DIR [--resume] [--no-prepare]` | the conversion into a new build slot (docs/M9_LAUNCHER_PLAN.md section 10): verify, extract, decrypt, the lift (PPU, SPU), the build, finish; the new build checked (`validate`), then made active (`activate`); the settings file's defaults. `--resume`: the disc's steps an interrupted install of the same disc finished are not done again. Also a reinstall or repair from the disc: the saves, settings and kept builds untouched | `build`, `exe` (the game a shortcut starts: `<install>/game/...`), `seconds` (per step), `skipped`, `pruned` |
| `update` | `--install DIR [--full] [--no-prepare]` | the saves backed up, then the game rebuilt from the install's own disc copy into a new slot -- only what changed (the lift when the tools changed; objects by fingerprint); nothing at all when the inputs equal the active build's -- checked, made active, the one before kept; `--full`: the lift and every object again | `build`, `changed`, `saves_backup`, `seconds`, `skipped`, `pruned` |
| `migrate` | `--install DIR [--no-prepare]` | a setup app's install moved into the layout with build slots: the saves backed up, the current build made from the install's own disc copy, its lift and objects (relifted only when the tools changed), checked, made active; the settings, the objects and the translated shaders moved; the old game removed. `--no-prepare` (also for `install` and `update`): no preparation at the end -- the launcher prepares in the background, as its settings or the player say (section 16 of the plan) | `build`, `lift_kept`, `moved`, `removed`, `saves_backup`, `exe` |
| `builds` | `--install DIR` | the slots: each one's record and state (`active`, `kept`, `unused`), whether it is whole | `active`, `previous`, `retention`, `builds` |
| `rollback` | `--install DIR [--to ID]` | back to a kept build (the newest by default): `active.json` and `game/` only -- offline, nothing rebuilt; the build left becomes the kept one (forward again: rollback once more) | `active`, `previous` |
| `retention` | `--install DIR --keep N` | how many previous builds are kept (default 1); the ones beyond it removed with what only they used | `retention`, `previous`, `pruned` |
| `repair-link` | `--install DIR` | `game/` pointed at the active build again | `game` |
| `saves` | `--install DIR [--action list\|backup\|restore\|delete] [--backup ID] [--note TEXT] [--confirm]` | the save backups: list them (dates, sizes, why made), make one, restore one (the current saves backed up first), delete one; restore and delete only with `--confirm` (the player confirmed) | the list (`backups`, `saves_bytes`), or the backup made, or `restored` and `previous_saves`, or `deleted` |
| `devsaves` | `--install DIR [--action states\|copy-player\|keep\|restore\|delete] [--name NAME] [--confirm]` | M11.0 (docs/M11_PLAN.md section 3.2): developer mode's test saves (`data/devmode/gamedata`): `copy-player` replaces them with a checked copy of the player's saves (only read; nothing is ever written back), `keep` keeps them as a named test state (`--confirm` replaces one of that name), `restore` puts a kept state back, `delete` removes one; refused while the game runs from the install | `states` (each kept state: name, time, files, bytes), `test_saves_files`, `test_saves_bytes`; or the operation's `files`, `bytes` and what it did |
| `prepare` | `--install DIR [--check]` | M9.2: the shaders and Vulkan pipelines made before play -- the active build's `dotheroes --prepare`: every recipe the game has recorded translated by this build and made through its pipeline cache, which is then saved (progress per pipeline); also run by `install`, `update` (when the build changed) and `migrate` when there are recipes. Before it (not with `--check`), the first-session manifest's pipelines (config/pipeline_manifest.txt, when there is one; docs/M9_LAUNCHER_PLAN.md section 15) that the install's recipes lack are added from the install's copy of the disc. One preparation at a time per install (`data/cache/prepare.lock`, its worker's process id; a lock whose process is gone is taken over; else `already_preparing`); the recipes prepared in parts (32 or more, at most eight), each part's pipeline cache saved as it ends, so a preparation cancelled part-way keeps what it made and the next one passes quickly over it. `--check`: only whether the cache is valid for this GPU, driver and renderer, and how many recipes there are (the launcher asks at every start; a preparation is due when `prepared` is false and there are recipes) | `recipes`, `made`, `failed`, `translated` (by glslang), `from_cache`, `ms`, `cache` (its state before: `reused`, `none yet`, `made with another driver version`, `made on another GPU`, `damaged (its checksum differs)`, ...), `cache_mb_before`, `cache_mb`, `manifest` (`listed`, `held`, `added`, `not_on_disc`; when there is a manifest), `parts`, `disk_mb` (the active build's shader folder and the recipes); with `--check`: `recipes`, `cache`, `cache_mb`, `prepared`, `disk_mb`, `pending` (the manifest's pipelines not yet among the recipes) |
| `check-update` | `[--channel stable\|prerelease] [--installed VERSION] [--source URL\|DIR] [--trusted-keys FILE]` | M9.3: the newest release of the channel (stable; prerelease: any) whose manifest verifies (docs/RELEASE_SIGNING.md); never an error for being offline (`offline: true` in the result, play goes on); nothing requested at all when no key is trusted | `available`, `version`, `channel`, `notes` (shown before an update), `min_launcher`, `newer`, `assets` (this platform's: name, size, SHA-256), `tag`, `refused` (releases skipped and why), `installed`, `tls_store` (beta.4: the certificate store HTTPS was verified with); or `offline` |
| `download-update` | `--install DIR [--channel ...] [--version V] [--dest DIR] [--source ...] [--trusted-keys FILE]` | the platform's download of that release (its manifest verified again), resumed where an earlier attempt stopped, checked against the manifest's size and SHA-256 before it is kept (removed when it is not); too little space refused first; the verified manifest kept beside it | `version`, `path`, `size`, `notes`, `seconds` |
| `stage-launcher` | `--package ZIP --launcher DIR` | a downloaded launcher package unpacked inside the launcher's folder -- which is, portable by default, also the install's root -- into `DIR/downloads/launcher-new` (a zip with one top folder: its contents); a member whose path leaves it, or a top item that is the install's own (`builds`, `data`, `config`, `logs`, `backups`, `downloads`, `active.json`, `game`, the root marker), refuses the package (`bad_download`). Packaged, it also copies the worker the swap runs as into `DIR/downloads/launcher-helper` (the program, its Python runtime, `tools/`, `config/`): the swap replaces `worker/`, so it must not run from there. The launcher then starts `<helper> --worker swap-launcher ...` as it exits (detached, outside any job of its own) | `staged`, `helper` (null from a source checkout), `items` |
| `swap-launcher` | `--launcher DIR --start EXE [--wait-pid PID]` | the helper (stage-launcher's copy) the launcher starts as it exits to update itself: after PID has exited, the launcher package's items in DIR (install_layout.LAUNCHER_PACKAGE: `CubicHeroes[.exe]`, `SDL2.dll` or `lib/`, `licenses/`, `LICENSE.txt`, `README.txt`, `worker/`; and any name the new package also has) move to `DIR/downloads/launcher-previous`, the new package's items take their places; nothing else in DIR -- the install's items above all -- is moved, renamed or removed. The new launcher is started with `--after-update` and has 60 s to write `.started-ok` in DIR: then the previous items are removed. If the swap fails part-way, or the new one does not come up, its items go to `DIR/downloads/launcher-failed`, the previous ones are put back and started with `--update-failed`. (The earlier whole-folder swap, `<DIR>.new`/`<DIR>.old`, is gone: it would move the install with the launcher, and no launcher had used it) | `result`: `updated` or `restored` |
| `settings` | `--install DIR [--set KEY=VALUE ...] [--reset KEY ...\|--reset all] [--repair]` | M9.4: the one authoritative settings file (config/dotheroes.cfg, config/settings_schema.json): the values the game and the launcher will use, every problem with the file (an invalid value: its default used; a line that is not NAME=value: left out; a damaged file); `--set`: each change checked first, none written if one is invalid; `--reset`: defaults back; `--repair`: a damaged file written again with its valid values, the damaged one kept beside it | `settings` ({key: value, default, source, owner, applies -- `live` or `restart` --, category, label}), `problems`, `unknown` (keys kept that the schema does not know), `path`, `readable`; `repaired_from`, `dropped` |
| `diagnostics` | `--install DIR --out DIR [--pack ZIP]` | M9.4: a report for the player to review: about.json (the versions, the operating system, the processor and memory, the GPU and its driver, the install's state and sizes) and the logs' ends, every text with the home and install folders, the user's and computer's names and e-mail addresses replaced; never saves, keys, the disc's or the game's files, recipes or shader caches (their sizes only). `--pack`: exactly what `--out` holds after the player's review, as one zip. Nothing is sent anywhere | `folder`, `files` ({file, bytes, what}); with `--pack`: `zip`, `files`, `bytes` |
| `uninstall` | `--install DIR (--dry-run | --confirm [--parts LIST] [--exe LAUNCHER])` | M9 (plan section 20): `--dry-run`: what each part holds, nothing removed. `--confirm`: the parts removed -- `builds` (the builds, game/ as a link only, active.json, the logs), `data` (the disc's files, the lift, install.json, developer mode's test saves), `shaders` (the shader and pipeline caches, the recipes), `objects` (the compilation caches: `data/cache/objects`, and Zig's own, `data/cache/zig-global` and `zig-local`), `settings`, `saves` (the saves and their backups), `shortcuts` (the Start-menu and desktop shortcuts and the Steam entry that start `--exe`; no other Steam entry is touched, and while Steam runs its list is left alone: `removed.shortcuts.steam_later`, the launcher removes the entry once Steam is closed); default: all but `settings` and `saves`. Only inside an install the launcher made, never through a link (one met is removed as a link), never a disc image or keys file; refused while the game runs from the install or a preparation holds its lock (the launcher's own programs, in a portable root, are not the game); a file held open (in a portable root, the worker's own `--log` in `logs/`) is left and listed, never a failure | `--dry-run`: `parts` ({part: {bytes, files}}), `default`; else `removed`, `kept`, `protected`, `left`, `bytes` |
| `remove-launcher` | `--launcher DIR [--data-dir DIR] [--install DIR] [--everything] (--dry-run \| --confirm [--confirm-everything] --wait-pid PID)` | M9 (plan section 20): the launcher itself. `--dry-run`: what goes, nothing removed (refused: `not_packaged` -- not a downloaded copy, or a source folder; `install_inside` -- an install inside one of the package's own items; `unsafe_path`). `--confirm`: the worker copies itself to a `CubicHeroes-remove-*` folder in the operating system's temporary folder (the one place outside the install root the worker writes: the helper must run from outside the folder it removes) and starts the copy, which waits for `--wait-pid` (the launcher) and this worker to exit, then removes the package's own items and the launcher update's leftovers (`downloads/launcher-*`; never anything else in the folder), the data folder's own items (not when it lies in the install being kept), the launcher's shortcuts and Steam entry (Steam open: the entry stays), then its temporary folder. The install -- portable by default, in DIR itself: its builds, data, settings, saves, backups, logs, downloads -- is kept, as is an install anywhere else. `--everything` (a separate option the launcher confirms separately: with `--confirm` it also needs `--confirm-everything`, else `needs_confirmation`): the install whose root is DIR goes too, every part of it, the saves and settings included (never a disc image or keys file, never through a link; refused while the game runs from it or a preparation holds its lock), and the root's marker | `--dry-run`: `folder`, `bytes`, `files`, `items`, `kept_items`, `data_bytes`, `portable`, `install_kept`, `everything`, `install_bytes`, `steam_open`; `--confirm`: `started`, `helper`, `everything` |
| `migrate-root` | `--from OLD_ROOT --install NEW_ROOT [--dry-run]` | the portable root: an existing installation COPIED (never moved: OLD_ROOT is only read and stays a complete install) into NEW_ROOT (the launcher's folder) -- `builds/`, `data/` (the disc's files, the lifts, the caches, the saves in `data/gamedata`, developer mode's `data/devmode`, `install.json`), `config/`, `backups/`, `active.json`; not `game/` (made again for the active build), `logs/` (they tell of the old folder, which keeps them; the new root keeps its own), `downloads/`, `data/tmp/`, a preparation's lock and part file, a staged build, or any link met (never followed). Refused before anything is written: `root_overlap` (the same folder, or one inside the other), `install_exists` (NEW_ROOT has `active.json` or `builds/`), `no_install` / `needs_migration` (OLD_ROOT holds no install, or the setup app's: `migrate` it first), `game_running` / `busy` (the game runs from OLD_ROOT, or a preparation holds its lock), `root_not_writable`, `disk_space` (the copy plus 256 MB). The copy goes into `NEW_ROOT/.migrate-root.part`, every file is checked (sizes; SHA-256 against the source for the saves, their backups, the settings and the records; each build's game against its `build.json`, as a rollback checks it) -- `copy_failed` otherwise -- and only then moved into place, `active.json` last; a file of NEW_ROOT's own in the way (a `config/dotheroes.cfg` made before) is kept beside it as `<name>.before-migrate-root`. Cancelled or failed: the part folder removed, NEW_ROOT as it was. Steps `copy`, `check-copy`, `place` (progress by bytes). `--dry-run`: what would be copied and whether there is room, nothing written | `from`, `to`, `kind`, `copied` ({item: {bytes, files}}), `bytes`, `files`, `skipped`, `links_skipped`, `replaced`, `verified` (`files`, `hashed`, `builds`), `active`, `game_link`, `seconds`; `--dry-run`: `from`, `to`, `kind`, `items`, `bytes`, `files`, `needed`, `links`, `skipped`, `active`, `free`, `enough`, `checked_at` |
| `find-legacy` | `[--install DIR]` | read only, nothing created or changed: the pointers to an install from before the portable root -- the old launcher's preferences (`%APPDATA%\CubicHeroes\launcher.json`; `~/.config/cubic-heroes/launcher.json`) and the setup app's settings (`%APPDATA%\DotHeroes-Setup\settings.json`; `$XDG_CONFIG_HOME/dotheroes-setup/settings.json`) with the install each names, and the old default folders (`%LOCALAPPDATA%\Cubic.Heroes` and `%LOCALAPPDATA%\3D Dot Game Heroes`; `~/Games/Cubic.Heroes` and `~/Games/3D Dot Game Heroes`) -- each with what it holds (`info`'s report); `--install`: the root in use now (marked `current`, never offered) | `prefs` ([{source, file, present, readable, install, holds}]), `defaults` ([{source, path, holds}]), `installs` (the folders holding an install: path, from, kind, active, version, migratable, ...), `migratable` (their paths) |
| `shortcuts` | `--exe LAUNCHER [--icon ICO] [--desktop \| --steam \| --remove-steam]` | M9.4: the launcher in the Start menu (and on the desktop), or in Steam's library (refused while Steam runs: it rewrites its list as it exits); `--remove-steam`: the launcher's own Steam entry removed (an uninstall left it while Steam ran) -- while Steam runs, nothing is written | `made` or `steam`; `--remove-steam`: `later` (Steam runs and the entry is still there), `removed` |
| `selftest` | `[--seconds S] [--block]` | a pretend three-step operation with progress, for the protocol's tests and for the launcher's progress screens without a disc; `--block` stops calling back in its second step, as a long compile does | `seconds`, `skipped` |

Every command also takes `--control` (section 3) and `--log FILE` (every event appended to the file as it is
sent: the launcher's log of the operation).

**The portable root.** The install's root is, by default, the launcher's own folder, and the worker writes nothing
for an install outside it: given `--install`, the commands that build, prepare, download or report (`install`,
`update`, `migrate`, `prepare`, `download-update`, `stage-launcher`, `diagnostics`, `selftest`) keep their temporary
files and their children's in `<install>/data/tmp/worker-<pid>` (tempfile's folder, `TMP`, `TEMP`, `TMPDIR`) --
removed as the worker ends; one left by a worker that is gone, by the next worker; nothing else in `data/tmp` is
ever touched; never made for a folder that is not there (but `install`'s), nor by the commands that only look
(`space`, `info`) or remove (`uninstall`, `remove-launcher`) -- and every Zig run gets `ZIG_GLOBAL_CACHE_DIR` and
`ZIG_LOCAL_CACHE_DIR` in `<install>/data/cache/zig-global` and `zig-local`, never the user's profile. Downloads go to
`<install>/downloads/<version>`, the launcher's update to `<launcher>/downloads/launcher-*`. The one exception:
`remove-launcher`'s helper copies itself into the operating system's temporary folder. `install`, `update`,
`migrate`, `prepare` (not `--check`), `download-update`, `stage-launcher` and `migrate-root` check first that the
root can be written (a file made and removed there) and has the space: `root_not_writable` or `disk_space`, before
anything is changed. Paths may hold spaces and any characters: tools' and the compiler's output is read as UTF-8.

## 2. The event stream

stdout carries **only events**: one JSON object per line, UTF-8, each line flushed as it is written. Anything
else a tool prints goes to stderr. Every event has `ev` (its kind) and `t` (seconds since the worker started).

| `ev` | Fields | When |
|---|---|---|
| `hello` | `protocol`, `command`, `owned_children` | first, always (but a usage error's `result` comes alone) |
| `plan` | `stages`: `[{id, name, weight}]` | before a conversion's first step: the steps in order, their display names and weights (section 5) |
| `stage` | `id`, `state` (`start`, `done`, `skipped`), `name` (start), `seconds` (done) | each step's start and end; a step an update does not need is `skipped` |
| `progress` | `stage`, `fraction` (0-1, or null when the step has no measure of its own), `overall` (0-1), `elapsed` (s), `eta` (s, or null), `eta_basis` (`reference`, `measured`, `rate`, or null), `detail` | at most every 0.1 s per step, and at a step's end |
| `log` | `level`, `text` | a line from the tools (the lifter's counts, the compiler's file names) |
| `cancelling` | `by` (`the player`, `the launcher closed`) | a cancel was taken (section 3) |
| `result` | `ok`; `data` when ok; `code`, `message`, `details` when not | last, always exactly one |

Exit codes say the same as the result: 0 ok, 1 failed, 2 cancelled, 64 a usage error.

A launcher reads lines until the stream ends; a line that is not JSON, or an event kind it does not know, is
logged and skipped (a newer worker may add events; `protocol` changes only when an existing event changes
meaning).

## 3. Cancelling

- With `--control` the worker reads stdin: a line `cancel` cancels the operation. **The end of stdin also
  cancels** (the launcher was closed or crashed): a conversion never runs on unattended.
- The operation stops at its next progress or log callback (a few per second in every step), the steps'
  own `.part` files keeping a half-written output from ever looking finished.
- A step that does not call back for **5 seconds** after the cancel (a compile or a lift in flight) is ended
  by force: the worker sends `result` with `code: cancelled` and `data.forced: true` and exits.
- **Nothing the worker started outlives it**: on Windows it puts itself in a job object that ends every
  process in it when the worker's handle closes (any exit, a crash included); elsewhere it leads a process
  group of its own and ends the group. `hello.owned_children` says whether that was set up.
- What a cancel leaves (M9.1): an install is taken up again with `install --resume` -- the disc's steps it
  finished and a finished lift are not redone, the objects compiled before the cancel are reused; the staged
  slot it was building is removed by the next operation. A cancelled update leaves the active build as it was.

## 4. Errors

`result.code` names the kind of failure, so the launcher can give advice and choose what to offer (retry,
another file, another folder). `message` is a sentence for the player; `details` the technical part (a tool's
last lines, a trace), shown only on request.

| `code` | Means | The launcher offers |
|---|---|---|
| `usage` | a required option missing | (a launcher bug) |
| `not_found` | the disc image is not where it was chosen | choose it again |
| `not_a_disc` | not an ISO 9660 image, or no PS3_GAME | choose a decrypted ISO or the disc's top folder |
| `unsupported_version` | another game, region or version (the message names it and the supported one) | choose the supported disc |
| `damaged_disc` | the right title and version, but EBOOT.BIN, the size or the image's SHA-256 does not match: encrypted, incomplete or modified | dump the disc again |
| `keys_missing`, `keys_unreadable`, `keys_empty` | no keys file chosen, unreadable, or no APP keys in it | choose the keys file again |
| `wrong_key` | the keys do not decrypt this disc's program to the supported one | choose the matching keys |
| `disk_space` | too little free space before starting, or the drive filled during the operation | free space, or another folder |
| `root_not_writable` | the install's root (or `migrate-root`'s new root) can't be made or written to: permissions, a read-only folder or drive, a path through a file, a drive that can't be reached -- checked before anything is changed | another folder, or allow writing to it |
| `root_overlap`, `install_exists`, `copy_failed` | `migrate-root`: the two roots are the same or one is inside the other; the new root already holds an install; a copied file is not the original (nothing was put in place) | choose other folders; retry |
| `lift_failed`, `compile_failed` | a recompiling tool, the compiler or the linker failed (details: their last lines) | retry; the diagnostics export |
| `no_disc_copy` | an update of an install that has no extracted disc | reinstall from the disc |
| `needs_migration` | an install or update asked of a setup app's install | migrate it first (no disc needed) |
| `not_setup_app`, `no_install` | migrate asked of a folder that is not a setup app's install; an update, rollback or the like of a folder with no build slots | install, or choose the right folder |
| `build_invalid` | the new build failed its check (`dotheroes --info`: FFmpeg loads beside it, the native release; its memory test): it is not made active | retry; the diagnostics export |
| `build_damaged` | a kept build's game is not the one recorded (size, or SHA-256 before a rollback) | update, or reinstall |
| `no_previous` | a rollback with no kept build (or not that one) | -- |
| `needs_confirmation` | a save restore or delete without `--confirm` | ask the player, then send `--confirm` |
| `no_backup` | no save backup by that id | -- |
| `no_trusted_keys`, `no_crypto` | the launcher carries no release key (until the owner's is added), or OpenSSL's library is missing: no update can be checked | -- |
| `unsigned`, `bad_signature`, `untrusted_key`, `bad_manifest` | (in `check-update`'s `refused`, per release) a release without a signature, one whose signature does not verify, one signed by a key not trusted, a manifest that breaks its rules -- never installed | -- |
| `bad_download` | a download that is not the file the release was signed for (size, SHA-256), or a package with a path escaping it -- removed | retry the download |
| `interrupted`, `server`, `offline` | a download cut off (it resumes where it stopped), the server's refusal, no connection | retry later; play goes on |
| `tls` | (beta.4) the update server's certificate does not verify (an untrusted authority, expired, another host's), or on Linux no certificate store was found (SSL_CERT_FILE / SSL_CERT_DIR as set, else the distribution's bundle: tools/updates.py `ca_store`) -- never reported as `offline`, nothing downloaded | the message names the cause; play goes on |
| `no_release` | no release for this platform, or not the version asked for | -- |
| `invalid_setting`, `unknown_setting`, `damaged_settings` | a setting's value breaks its rule (the message says how); a key this version does not know; the settings file is damaged (repair it first) | correct the value; repair |
| `needs_confirmation` | `uninstall` or `remove-launcher` without `--confirm` (the launcher asks the player first) | ask |
| `unsafe_path` | `uninstall`: the install folder is a link, a drive or the home folder, or a path would lie outside the install or be reached through a link -- nothing removed | none: report it |
| `game_running` | `uninstall`, `remove-launcher --everything`, `migrate-root` (its source): the game runs from this install (the launcher's own programs in a portable root are not the game) | close the game |
| `busy` | `uninstall`, `remove-launcher --everything`, `migrate-root` (its source): a preparation holds the install's lock | wait, or stop it |
| `already_preparing` | another worker is preparing this install's shaders (its process holds `data/cache/prepare.lock`) | wait for it; the launcher shows the one running |
| `prepare_failed` | the preparation pass did not finish (no Vulkan device, the renderer did not start; details: its log, `logs/prepare.log`) -- after an install or update it is reported in the result, not raised: the game still prepares what it meets | retry; the diagnostics export |
| `io_error` | a file could not be read or written (details: the trace) | retry; the diagnostics export |
| `cancelled` | cancelled (`data.forced` if a step had to be ended) | resume or start again |
| `internal` | anything else (details: the trace) | the diagnostics export |

**Keys** are read only by the decryption step and the key probe; their contents never appear in an event, a
log, an error's message or details (tests/test_worker.py checks a probe's whole output for them).

## 5. Progress and the remaining time

- Each step reports a fraction of its own where it can measure one: bytes for verify and extract, the
  lifter's function counts for the PPU recompile, the files compiled (by size) and the link's expected length
  for the build. The SPU step and decrypt have no inner measure (short).
- **The overall fraction** weighs each step by its typical length (`plan.stages[].weight`, seconds on the
  reference PC, section 6); a skipped step leaves the total, so an update's bar is about the steps it runs.
- **The remaining time** (`eta`, with `eta_basis` saying what it rests on):
  - the current step: its own rate once 15% of it is measured, kept within a third and three times what its
    weight leaves (and, once this PC's speed is known, averaged with that: the build's rate comes in bursts, ten
    compiles finishing together); before that, what its weight leaves;
  - the steps after it: their weights;
  - the compute-bound steps' weights (recompile, SPU, build) are scaled by **this PC's speed** once one of them has
    finished here -- the disc's steps are bound by the drive and say nothing of the processor;
  - `eta_basis`: `reference` (the reference PC's figures: the launcher says "about"), `measured` (this PC's speed),
    `rate` (the current step's own rate). Null only before a step has started.
  - Replayed against M9.0's measured install (section 6): within 11% of what actually remained from the first
    second to 80% of the install, then too long by about 20 s at the end (the last compiles).

## 6. M9.0's measurements

A fresh install through the worker on the reference PC (the project's Windows machine: 10 compiles at a time),
the disc image in the operating system's file cache: 352 s -- verify 2.3 s (15 s from a cold drive is the weight),
extract 24.6, decrypt 0.5, recompile 39.1, SPU 11.8, build 273.9 (76 files at 3.6 s each, ten at a time; the link
0.27 s), finish 0.03. These are STAGE_SECONDS, and tools/zig_build.py's first-build estimate (3.6 s a file, 2 s to
link; it had assumed 20 s and 30 s). More in docs/M9_LAUNCHER_PLAN.md section 9.

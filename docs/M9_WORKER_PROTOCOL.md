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
| `install` | `--disc PATH --keys PATH --install DIR [--resume]` | the conversion into a new build slot (docs/M9_LAUNCHER_PLAN.md section 10): verify, extract, decrypt, the lift (PPU, SPU), the build, finish; the new build checked (`validate`), then made active (`activate`); the settings file's defaults. `--resume`: the disc's steps an interrupted install of the same disc finished are not done again. Also a reinstall or repair from the disc: the saves, settings and kept builds untouched | `build`, `exe` (the game a shortcut starts: `<install>/game/...`), `seconds` (per step), `skipped`, `pruned` |
| `update` | `--install DIR [--full]` | the saves backed up, then the game rebuilt from the install's own disc copy into a new slot -- only what changed (the lift when the tools changed; objects by fingerprint); nothing at all when the inputs equal the active build's -- checked, made active, the one before kept; `--full`: the lift and every object again | `build`, `changed`, `saves_backup`, `seconds`, `skipped`, `pruned` |
| `migrate` | `--install DIR` | a setup app's install moved into the layout with build slots: the saves backed up, the current build made from the install's own disc copy, its lift and objects (relifted only when the tools changed), checked, made active; the settings, the objects and the translated shaders moved; the old game removed | `build`, `lift_kept`, `moved`, `removed`, `saves_backup`, `exe` |
| `builds` | `--install DIR` | the slots: each one's record and state (`active`, `kept`, `unused`), whether it is whole | `active`, `previous`, `retention`, `builds` |
| `rollback` | `--install DIR [--to ID]` | back to a kept build (the newest by default): `active.json` and `game/` only -- offline, nothing rebuilt; the build left becomes the kept one (forward again: rollback once more) | `active`, `previous` |
| `retention` | `--install DIR --keep N` | how many previous builds are kept (default 1); the ones beyond it removed with what only they used | `retention`, `previous`, `pruned` |
| `repair-link` | `--install DIR` | `game/` pointed at the active build again | `game` |
| `saves` | `--install DIR [--action list\|backup\|restore\|delete] [--backup ID] [--note TEXT] [--confirm]` | the save backups: list them (dates, sizes, why made), make one, restore one (the current saves backed up first), delete one; restore and delete only with `--confirm` (the player confirmed) | the list (`backups`, `saves_bytes`), or the backup made, or `restored` and `previous_saves`, or `deleted` |
| `prepare` | `--install DIR [--check]` | M9.2: the shaders and Vulkan pipelines made before play -- the active build's `dotheroes --prepare`: every recipe the game has recorded translated by this build and made through its pipeline cache, which is then saved (progress per pipeline); also run by `install`, `update` (when the build changed) and `migrate` when there are recipes. `--check`: only whether the cache is valid for this GPU, driver and renderer, and how many recipes there are (the launcher asks at every start; a preparation is due when `prepared` is false and there are recipes) | `recipes`, `made`, `failed`, `translated` (by glslang), `from_cache`, `ms`, `cache` (its state before: `reused`, `none yet`, `made with another driver version`, `made on another GPU`, `damaged (its checksum differs)`, ...), `cache_mb_before`, `cache_mb`; with `--check`: `recipes`, `cache`, `cache_mb`, `prepared` |
| `check-update` | `[--channel stable\|prerelease] [--installed VERSION] [--source URL\|DIR] [--trusted-keys FILE]` | M9.3: the newest release of the channel (stable; prerelease: any) whose manifest verifies (docs/RELEASE_SIGNING.md); never an error for being offline (`offline: true` in the result, play goes on); nothing requested at all when no key is trusted | `available`, `version`, `channel`, `notes` (shown before an update), `min_launcher`, `newer`, `assets` (this platform's: name, size, SHA-256), `tag`, `refused` (releases skipped and why), `installed`; or `offline` |
| `download-update` | `--install DIR [--channel ...] [--version V] [--dest DIR] [--source ...] [--trusted-keys FILE]` | the platform's download of that release (its manifest verified again), resumed where an earlier attempt stopped, checked against the manifest's size and SHA-256 before it is kept (removed when it is not); too little space refused first; the verified manifest kept beside it | `version`, `path`, `size`, `notes`, `seconds` |
| `stage-launcher` | `--package ZIP --launcher DIR` | a downloaded launcher package unpacked beside the launcher's folder (`<DIR>.new`); a member whose path leaves it refuses the package | `staged` |
| `swap-launcher` | `--launcher DIR --start EXE [--wait-pid PID]` | the helper the launcher starts as it exits to update itself: after PID has exited, `<DIR>.new` takes DIR's place (the old one kept as `<DIR>.old`) and is started with `--after-update`; it has 60 s to write `.started-ok` in its folder, else it is put back (`<DIR>.failed`) and the old one started with `--update-failed` | `result`: `updated` or `restored` |
| `selftest` | `[--seconds S] [--block]` | a pretend three-step operation with progress, for the protocol's tests and for the launcher's progress screens without a disc; `--block` stops calling back in its second step, as a long compile does | `seconds`, `skipped` |

Every command also takes `--control` (section 3) and `--log FILE` (every event appended to the file as it is
sent: the launcher's log of the operation).

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
| `no_release` | no release for this platform, or not the version asked for | -- |
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

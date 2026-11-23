# Cubic.Heroes and RecompLauncher

[RecompLauncher](https://recomplauncher.com) is a third-party launcher that installs, updates and starts PC
recompilations. From 0.2.0-beta.5 every Cubic.Heroes package carries a `recomplauncher.json` (the SDK's manifest,
`format` 1) so RecompLauncher can **start the game** and offer its **display and prompt settings**. The game's setup
from your own disc and keys stays with Cubic.Heroes's own launcher (section 5). The standalone launcher is unchanged
and remains the main way to play; nothing here is needed to use it.

**Status**: being tested; not yet listed in RecompLauncher's catalog. Until it is, RecompLauncher can use Cubic.Heroes
only as a developer build (its Developer tab).

## 1. What RecompLauncher does with Cubic.Heroes

| | |
|---|---|
| **Start** | `CubicHeroes.exe` (Windows) or `CubicHeroes` (Linux), with `--play --or-launcher` and the session settings below |
| **First start** | nothing is installed yet, so Cubic.Heroes's own launcher opens for the setup (your disc image and keys file, then the shader preparation); after that, starts go straight to the game |
| **After an update** | the first start opens Cubic.Heroes's launcher, which brings the game up to date with the new version (your saves backed up first), then you play from it; later starts go straight to the game |
| **Settings** | display mode, rendering resolution, VSync, frame limit, button prompts -- for each start from RecompLauncher (section 3) |
| **Saves** | `data/gamedata/dev_hdd0/home/00000001/savedata` in the installation folder |
| **Logs** | `logs/` in the installation folder |
| **Updates** | RecompLauncher's (section 4); Cubic.Heroes's own updater is then off |

## 2. The manifest

`launcher/recomplauncher.json` in this repository; `tools/make_bundle.py` copies it into every package: the zip's and
the tarball's `Cubic.Heroes/` folder, beside `CubicHeroes(.exe)`, and the AppImage's root. It is checked against the
SDK's published schema (`tests/fixtures/recomplauncher.schema.json`, MIT, kept with its licence) by
`tests/test_recomplauncher.py`, which also checks that every program, option and setting it names exists.

What it declares: `start` (above); `settings` (section 3); `saves` and `folders.logs`; `exit_codes` -- 3 (not set up
yet), 4 (a file the game needs is missing), 5 (busy), 7 (the folder can't be used), 70 (the graphics didn't start in
time), with the same advice as Cubic.Heroes's own messages (docs/CLI.md section 5); `system` -- Windows 10, Linux
glibc 2.31, Vulkan 1.1; and `requires` -- the SDK features it relies on, so an older RecompLauncher refuses it instead
of starting it wrongly. It declares no `game`, `inputs` or `prepare` (section 5).

**The AppImage**: RecompLauncher unpacks it (no FUSE needed) and starts its `AppRun`. Cubic.Heroes then runs as a
folder installation, like the tarball, in the folder RecompLauncher chose.

**An installation you already have** (the standalone zip, tarball or AppImage) is separate from the one RecompLauncher
makes: its first start there finds nothing installed. Instead of setting up again, copy the existing one in -- from a
terminal in RecompLauncher's folder for the game, before its first start: `CubicHeroes --migrate-from "<your
installation's folder>"` (docs/CLI.md; for the AppImage, the `Cubic.Heroes` folder beside it). Your saves, settings
and the game come along; the old installation stays as it was. Don't point RecompLauncher's folder at the old one
(`cubicheroes-root.txt`): RecompLauncher would then look for the saves in the wrong place.

**Updates unpacked over the folder**: RecompLauncher unpacks a new version over the installation folder. Everything
Cubic.Heroes keeps there -- the game it built, your saves, settings, shader caches, logs, `downloads/` -- has names no
package uses, so it stays.

## 3. Settings

Every setting RecompLauncher shows is passed to `--play` as a session option (docs/CLI.md section 2): `--fullscreen`,
`--borderless` or `--windowed`; `--resolution`; `--vsync`; `--fps`; `--set LAUNCHER_PROMPTS=...`. Each one's first
choice, **As set in Cubic.Heroes**, passes nothing, so the game's own setting applies. A choice made in RecompLauncher
applies to that start only: Cubic.Heroes's settings file (`config/`) is not changed, and stays the one place settings
are kept -- RecompLauncher never writes it. Settings changed in the game's own menu are kept, as always.

When a start opens Cubic.Heroes's launcher instead (the first start, after an update), RecompLauncher's settings don't
apply to that session; the launcher's own do.

## 4. Updates: one updater per installation

The SDK's rule is that a game started by RecompLauncher must not update itself: RecompLauncher installs the
developer's releases. So in an installation RecompLauncher manages, Cubic.Heroes's own updater -- the update check,
**Download it**, **Restart and update**, and **Go back to the previous launcher version (...)** -- is off:

- RecompLauncher sets `RECOMPLAUNCHER=1` on every start it makes. The first such start also writes
  `recomplauncher-managed.txt` at the installation's root, so the updater stays off when the launcher is opened some
  other way (a shortcut, the file manager).
- The launcher's home page and its updates (on Play) say **RecompLauncher installs this game's updates**. The command line's
  `--check-update`, `--apply-update` and `--rollback-launcher` refuse with exit code **8** (`managed_elsewhere`), and so
  does the worker.
- **Update it with this launcher instead** (Play's updates, when the launcher was not started by RecompLauncher)
  hands the updates back: it removes the mark, and the launcher checks for updates as usual. Starting the game from
  RecompLauncher again hands them back to RecompLauncher -- use one or the other.
- The game's own update (`--update`, and the launcher's at its start after a new version) stays on: it rebuilds the
  game inside the installation and never changes the files RecompLauncher installs.

A **standalone installation** -- never started by RecompLauncher -- is not affected: its signed updates work exactly as
before.

**How each verifies a download** -- they differ, and this is said plainly: Cubic.Heroes's launcher installs only
releases signed with the project's release key (docs/RELEASE_SIGNING.md); RecompLauncher checks a download against the
SHA-256 digest GitHub publishes for the release asset, not against the project's signature. Under RecompLauncher, the
trust is GitHub's and RecompLauncher's.

**Pre-releases**: RecompLauncher installs a project's latest full release, and pre-releases only when the developer
offers them as a beta there. Every Cubic.Heroes release so far is a pre-release.

## 5. What RecompLauncher can't do for Cubic.Heroes: the setup

The SDK's full setup (`inputs` and `prepare`) would let RecompLauncher ask for the player's files and prepare the game
itself. Cubic.Heroes needs two: the disc image and the **keys file**. The SDK accepts key files only as a few shared
types it knows (Nintendo Switch and Wii U keys: role `key`), and every other input must be a game, disc, DLC or update
file. A PS3 keys file declared as one of those would lose what the SDK does for keys -- keeping them out of saves,
cloud sync, backups and diagnostics -- so Cubic.Heroes doesn't do that. The setup stays in Cubic.Heroes's own
launcher, which RecompLauncher opens at the first start (section 1).

The SDK's recomp standards ask a game to take its keys as `--keys <path>`, so a launcher can pass the file the player
added. Cubic.Heroes already does (`--install --disc PATH --keys PATH`, headless: docs/CLI.md); what is missing is a
PS3 key type among the manifest's inputs for RecompLauncher to collect the file through. If the SDK adds one, the full
setup needs only the manifest's `inputs` and a `prepare` step running that command -- and an end-to-end test before
it is claimed.

## 6. For developers: checking it

- `python -m unittest tests.test_recomplauncher` -- the manifest against the schema, and against the launcher's options.
- `CubicHeroes --play --or-launcher` in a folder with nothing installed opens the launcher; with an installation that
  is up to date, it plays.
- `RECOMPLAUNCHER=1 CubicHeroes --check-update` exits 8 and leaves `recomplauncher-managed.txt`.
- With RecompLauncher itself: add the unpacked package as a developer build (its Developer tab), then the checks it
  runs there, a start, the settings, and an update unpacked over the folder (saves, settings and shader caches kept).

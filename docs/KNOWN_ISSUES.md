# Known issues

Every known issue of the current beta. The [README](../README.md#known-issues) lists the main ones; what has been
tested, and on what: [TESTING_STATUS.md](TESTING_STATUS.md).

## The game

- **B-011**: a hang was seen once in the priest's arena, 13 minutes into a long fight; not reproduced in more than two
  hours of the same fight since. If the game freezes, Diagnostics > Export Logs collects the hang report.
- **B-007**: one effect frame is missing when arriving at the castle battle's arena (cosmetic).
- The **minigames** run, but none has yet been played to its end in testing.
- The **extra Block Defense stage** is drawn pink in play; not yet compared with the original.
- The **purple spell** works only at its puzzle places, as on the original; two of its three places are still to be
  tested.
- The keyboard has no right stick: the camera's prompts keep a stick's picture there.
- The editor's own world, New Game+'s carry-over, the side quests and the consumables are not yet fully tested.

## Installing and updating

- **A copied installation folder** can stop updates, repairs and going back to an earlier build ("game is in the
  way"). It happens when the folder is copied in a way that turns its `game` link into a plain folder. See
  [The installation was copied](TROUBLESHOOTING.md#the-installation-was-copied-game-is-in-the-way) for how to
  check and recover safely.
- **Coming from beta.2 or beta.3 takes one manual step**: their launchers can't install updates (and beta.3's can't
  reach the update server on Linux), so the current version is installed by hand once
  ([UPDATING.md](UPDATING.md#from-beta2-or-beta3-once-by-hand)); updates install themselves after that.
- **An update waits for the game to close**: the launcher doesn't install one while the game is running.
- **Windows: a console window flashes for about two seconds** after the launcher installs an update or removes itself:
  its helper waiting a moment, then deleting its own temporary copy. Nothing to do; a fix is planned.
- **An AppImage in a folder you can't write to** (`/opt`, for example) can't update itself: the launcher says so
  before downloading anything; download the new AppImage from the releases page instead.

## Linux

- Without FUSE, use the tarball: an AppImage started with `--appimage-extract-and-run` leaves its extracted copy in
  `$TMPDIR` (normally `/tmp`) until it is deleted or the machine restarts.
- On Wayland, exclusive fullscreen behaves as borderless fullscreen (the compositor owns the display mode).
- On AMD graphics a few shadows and floor reflections differ slightly from the reference captures (faint; nothing
  missing or misplaced); being investigated.

## Reporting a bug

Use the [issue forms](https://github.com/irnutsmurt/Cubic.Heroes/issues/new/choose) -- gameplay, rendering, crashes and
hangs, performance, installation and the launcher, hardware -- and attach the launcher's **Diagnostics > Export Logs**
zip (review it first; it removes your name and paths). **Never attach game files, keys, save files or personal
information.**

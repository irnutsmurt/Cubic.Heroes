# Updating

**Since 0.2.0-beta.4 the launcher installs updates itself.** It checks for new versions (betas on the pre-release
channel, a beta's default; Settings > Updates). **Download it** downloads a new version and checks it against the
project's release signature; **Restart and update** then:
- checks the download again (the signature, and that it is this system's package of your kind of installation);
- closes the launcher, puts the new version's files in place of its own, and opens the new version;
- brings the game up to date if the new version needs it (your saves are backed up first; the previous build is kept:
  Install > **Rollback Last Update** goes back to it).

Your saves, settings, shader caches, logs and builds stay as they are. The launcher it replaced is kept: **Go back to
version ...** on the Updates page (or `CubicHeroes --rollback-launcher`). If an update is stopped part-way -- the
launcher or the computer stopping in the middle -- the previous launcher is put back at the next start. On Linux the
AppImage is replaced in place, keeping its name and its shortcuts. No administrator or root rights are needed. Close
the game first: an update isn't installed while it runs. From a terminal: `CubicHeroes --check-update`, then
`CubicHeroes --apply-update`, then `CubicHeroes --update` for the game ([CLI.md](CLI.md)).

## From beta.2 or beta.3: once by hand

Their launchers can't install updates (and beta.3's can't reach the update server on Linux), so install the current
version by hand once; from then on updates install themselves.

1. **Back up your saves** (the launcher's Saves > Back up now), then **close the launcher and the game**.
2. **Put the new version in place of yours**. Only the launcher's own files are replaced; your installation's folders
   are not touched:
   - **Windows**: extract the new zip into the folder that *holds* your `Cubic.Heroes` folder (for
     `D:\Games\Cubic.Heroes`, into `D:\Games`), so that it merges with yours, and replace the files when asked. In
     Windows Explorer: right-click the zip > Extract All..., set that folder as the destination, then *Replace the
     files in the destination*. In PowerShell: `Expand-Archive -Force <the zip> -DestinationPath D:\Games`.
   - **Linux, the tarball**: extract it into the folder that holds `Cubic.Heroes`, the same way:
     `tar -xJf <the tarball> -C ~/Games`.
   - **Linux, the AppImage**: put the new `.AppImage` beside the old one (beside the `Cubic.Heroes` folder) and
     make it executable; it uses the same installation. Then remove the old file, or keep it. If Steam, Heroic or
     Lutris start the old file, point them at the new one.
   - If you renamed the `Cubic.Heroes` folder, extract the zip or tarball somewhere else instead, and copy the
     contents of its `Cubic.Heroes` folder into yours, replacing the files.
3. **Start the launcher**. It brings the game up to date by itself, rebuilding it from your disc's conversion where the
   new version needs it (no disc needed), your saves backed up first and the earlier build kept (Install > **Rollback
   Last Update**). From a terminal instead: `CubicHeroes --update` in the installation's folder.

For 0.2.0-beta.4 these steps were tested before release from the published packages: beta.2 and beta.3 on Windows,
beta.3 with the Linux tarball and with the AppImage -- the saves and settings byte for byte the same, the game played
afterwards, and from beta.2 the rollback to beta.2's build still in place after the launcher was started again. (From
beta.3 the game was up to date in a few seconds; from beta.2 the rebuild took about 6 minutes on a recent desktop CPU.)

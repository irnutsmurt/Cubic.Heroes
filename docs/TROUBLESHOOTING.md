# Troubleshooting

The [README](../README.md#if-the-installation-fails) has the common problems; this page has all of them, and the
longer fixes.

| Problem | What to do |
|---|---|
| The disc is refused as another game or version | Only BLUS-30490 version 01.00 (US) is supported; the message names what your dump is |
| The disc is refused as damaged, or a file "doesn't match" | One of the game's files is missing, changed or incomplete in your dump: dump the disc again (or extract its folder again). Every format is checked file by file, so an ISO of another size is fine as long as its files are right |
| "This disc image is encrypted" | It is a raw (encrypted) rip: choose its disc key too -- a `.dkey` or `.key` file, an IRD file, or the 32-character key typed |
| "That disc key doesn't open this disc image" | The key is for another disc: check it matches this dump |
| "That is a disc key, not the keys file" | The keys file is a different thing: a text file of `APP <revision> <erk> <riv>` lines (this game needs revision `0001`), or choose an `EBOOT.elf` you decrypted yourself |
| The keys file is refused | It needs an `APP` line for revision `0001`; or decrypt `EBOOT.BIN` yourself and give the `EBOOT.elf` |
| The install stopped part-way | Run Install again: it resumes where it stopped. A stopped or failed install never touches the game you already have installed, your saves or your settings |
| Not enough space | About 6 GB free is needed in the installation's folder; the launcher checks before it starts |
| Stutter the first time somewhere | Shaders page: prepare them (again after a GPU driver update) |
| The game is too fast or slow | Report it with your frame limit and display rate; set the frame limit to 60 meanwhile |
| A black screen or the game won't start | Update your GPU driver (Vulkan); Diagnostics > Export Logs, and report it |
| The folder can't be written | Extract to a folder you own (not `Program Files`); the launcher says if it can't write |
| Linux, beta.3: the update check says "Offline" though you're online | A known issue of 0.2.0-beta.3, fixed in beta.4: install the current version by hand once ([From beta.2 or beta.3](UPDATING.md#from-beta2-or-beta3-once-by-hand)) |
| Linux: "couldn't verify the update server's certificate" | The system has no trusted certificate store the launcher can use: install your distribution's `ca-certificates` package, or start the launcher with `SSL_CERT_FILE` set to a CA bundle (for example `SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt ./Cubic.Heroes/CubicHeroes`) |
| Linux: "The AppImage's folder can't be written" | The AppImage is in a folder you can't write to (`/opt`, for example), so it can't update itself: download the new AppImage from the releases page, or keep the AppImage in a folder of yours |
| Linux: the AppImage doesn't start | Make it executable (`chmod +x`); without FUSE, use the tarball |
| Linux: Heroic can't start the game or doesn't see its folder | Add `CubicHeroes` as a native game with `--play`, and allow the folder for Heroic's Flatpak (README, "Other launchers") |
| Linux on Wayland: exclusive fullscreen looks like borderless | Expected: on Wayland the compositor owns the display mode, so exclusive fullscreen behaves as borderless |
| An update, a repair or going back to an earlier build fails with "...game is in the way (not a link)" | The installation folder was copied in a way that turned its `game` link into a plain folder: see below |
| Something else | Diagnostics > Export Logs, review, save the zip -- and [open an issue](https://github.com/irnutsmurt/Cubic.Heroes/issues/new/choose) |

The error codes behind these messages, for scripts and the command line: [M9_WORKER_PROTOCOL.md](M9_WORKER_PROTOCOL.md)
and [CLI.md](CLI.md).

## The installation was copied: "game is in the way"

Inside the installation folder, `game` is a link to the build in use (a junction on Windows, a symbolic link on
Linux), not a folder of its own. Some ways of copying the installation copy the build's files into a plain `game`
folder instead: Windows Explorer copying it to another drive, for example, or on Linux `cp -r` without `-a` or a
file manager that follows links. The game still plays,
but updating, repairing and going back to an earlier build stop at their last step with "...game is in the way (not
a link)". This is a known limitation of 0.2.0-beta.4 (and earlier); a fix is planned.

**To check**, open a command prompt in the installation folder and run `dir`:
- a healthy installation lists `<JUNCTION>  game [...\builds\...]`;
- a copied one lists `<DIR>  game`.

In PowerShell, `(Get-Item game).LinkType` says `Junction`, or nothing for a copied folder. On Linux, `ls -l` shows
`game -> builds/...` for the link, and a plain `game` directory for a copied one.

**To recover safely** (nothing is deleted before it is checked):
1. Close the game and the launcher.
2. Find the build in use. `CubicHeroes --status --json` names it (`builds`, then `active`), and so does the `active`
   line of `active.json` in the installation folder.
3. Compare the `game` folder with that build's own folder, `builds\<the build in use>`. It should hold the same files
   and nothing else:
   - the game's program (`dotheroes.exe`; `dotheroes` on Linux);
   - its libraries (`.dll` files on Windows; the `lib` folder on Linux);
   - `build.json`, `build.cfg`, `icon.png`;
   - on Windows, the libraries' licence notices (on Linux they're inside `lib`).

   Your saves and settings are never in `game`: they are in `data` and `config`.
4. Move the `game` folder out of the installation folder (to your desktop, for example). Don't delete it yet.
5. Open the launcher and run the update (or Repair) again: it makes `game` a link again. Then play to check.
6. Delete the folder you moved, but only if step 3 found nothing in it except those files. If it held anything else,
   keep it and report it with Diagnostics > Export Logs.

**To avoid it**:
- Move the installation folder on the same drive (cut and paste): a move keeps the link.
- To copy it elsewhere, use a copy that keeps links or leaves them out:
  - `robocopy` with `/XJ` leaves the link out, and the launcher makes it again on the next update;
  - on Linux, `cp -a` or `rsync -a` keep the link.

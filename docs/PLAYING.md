# Playing Cubic.Heroes

Controls, settings, other launchers and the command line. The [README](../README.md#playing) has the short version.

## Controls

| Keyboard | PS3 |
|---|---|
| Arrows | D-pad |
| Z / X / A / S | Cross / Circle / Square / Triangle |
| Q / W | L1 / R1 |
| 1 / 2 | L2 / R2 |
| 3 / 4 | L3 / R3 |
| Enter | Start |
| Tab | Select |

Any controller SDL2 recognises works, connected or disconnected while you play; buttons and keys can be changed in the
in-game menu. **The in-game menu**: hold **L3 + R3**, or press **F1** or **Esc** -- the game pauses while it is open.
The game's own START menu is unchanged; its title menu ends with **QUIT GAME**.

## Settings

Settings are shared by the launcher and the in-game menu:

| Setting | Values |
|---|---|
| Display mode | windowed (resizable; its size and place remembered), borderless, exclusive fullscreen (your display's own mode) |
| Rendering resolution | native (default: your display's in fullscreen, the window's in a window), 1280x720 (the original), 1920x1080, 2560x1440, 3840x2160, custom -- changed while you play; independent of the display mode and the window's size; your display's own resolution is never changed |
| VSync | on / off, independent of the frame limit |
| Frame limit | 30 (the original), 60 (default), 75, 100, 120, 144, 165, 240, your display's rate, custom, unlimited |
| Volumes | master, music and effects, 0-100, and mute -- applied at once |
| Controls | your own controller buttons and keyboard keys; the button prompts (automatic, Xbox, PlayStation, Nintendo Switch, Steam, generic, keyboard and mouse); the in-game menu's shortcut |
| Performance overlay | off, the frame rate, or the frame rate and frame times |

## Other launchers

- **Steam** (Windows and Linux): the launcher's **Add to Steam** (Install / Manage) adds the launcher as a non-Steam
  game: Steam opens the launcher, and its Play starts the game. Close Steam first. It also downloads the library
  artwork onto your computer -- the poster by Fami-Kon and the hero image by Jib, hosted on SteamGridDB -- without
  replacing artwork you put there yourself; if that fails, the same page lets you choose the image files yourself. The
  library logo is Cubic.Heroes's own ([assets/steam-logo.png](../assets/steam-logo.png)), placed from the launcher's
  files -- use it in other launchers too.
- **Heroic or Lutris** (Linux): add the tarball's `Cubic.Heroes/CubicHeroes` as a **native** game (not Wine or
  Proton) with the argument `--play`. Heroic's Flatpak can read only some folders (your Documents, the desktop, and
  others it already allows); for an installation elsewhere, allow it once with
  `flatpak override --user --filesystem=<the installation folder> com.heroicgameslauncher.hgl`.
- **Playnite, LaunchBox and others** (Windows): point them at `CubicHeroes.exe --play`.
- **RecompLauncher** (Windows and Linux): every package carries its manifest, so it can start the game and offer the
  display and prompt settings. The setup from your disc and keys stays in Cubic.Heroes's own launcher, which it opens
  at the first start; in that installation RecompLauncher then installs the updates, and Cubic.Heroes's own updater is
  off ([RECOMPLAUNCHER.md](RECOMPLAUNCHER.md)).

## The command line

The launcher is also a command-line program, for these launchers and for scripts:

```bash
CubicHeroes --play                                   # the game directly, no launcher window; exits with the game
CubicHeroes --play --fps 144 --borderless            # this session only (add --persist to keep it)
CubicHeroes --status --json                          # installed? which build? shaders prepared? (machine-readable)
```

Every option, the JSON output, exit codes and integration examples: [CLI.md](CLI.md).

## Where your saves and settings are

In the installation's own folder: [The portable folder](INSTALLING.md#the-portable-folder).

# The PC-experience milestone: requirements

The settings agreed with the owner on 2026-11-07, written down as
requirements. **Nothing of this milestone is implemented.** Each section
gives:
- what exists today, read in the code [C] or seen in runs [M];
- what is required;
- what still has to be established [H];
- how it will be tested.

The settings UI itself is not designed here.

## Where it sits

1. **The base**: the native release (R7's graphics, R8's CPU and physics
   work; docs/NATIVE_R8.md), its validation, and the player's build path
   producing it (docs/NATIVE_R8.md section 7.2).
2. **This milestone: the PC settings** below.
3. **Launcher and update management** (section 8), once the base and the
   settings are stable.
4. **RTX**: later, after the launcher.

## Rules for every setting

- **The monitor's mode is never changed**: not its refresh rate, not its
  resolution. Nothing calls ChangeDisplaySettings or SetDisplayConfig, or
  asks SDL for a display mode other than the current one.
- **A setting never changes gameplay.**
  - The game's timers, movement and physics follow time, not frames
    (docs/NATIVE_R8.md section 3; tested at 30, 60, about 120, 144 and
    200 fps).
  - The frame limit and VSync change how often frames are shown, never how
    fast the game runs.
- **One place for the values**: the install's settings file (today
  `<install>/dotheroes.cfg`, NAME=value lines the game reads at start and
  the setup app writes; port/main.cpp read_settings).
  - A fresh install gets the defaults.
  - An existing install keeps the player's values, and gains new keys with
    their defaults, as the performance switches are added today
    (setup_app.py ensure_performance_settings).
  - The old keys map onto the new ones: DOTHEROES_FPS and
    PS3RECOMP_VK_FULLSCREEN.
- **Testable without a person**: the game logs the effective value of every
  setting at start and on every change, so a scripted run can check it.
- **Prompts are separate from remapping**: the prompt style chooses how a
  button is drawn; the bindings choose which button does what.

Proposed keys, to settle when implementing:

| Key | Values |
|---|---|
| DISPLAY_MODE | windowed, borderless, fullscreen |
| DISPLAY | the monitor's index |
| WINDOW_SIZE | WxH |
| VSYNC | on, off |
| FPS_LIMIT | 30, 60, 120, 144, 165, 240, display, a number, off |
| VOLUME_MASTER, VOLUME_MUSIC, VOLUME_SFX | 0-100 |
| PROMPTS | auto, xbox, playstation, switch, keyboard |

## 1. Display mode: windowed, borderless, fullscreen

**Today** [C, ps3recomp libs/video/rsx_vulkan_backend.c vk_window_open]:
- With PS3RECOMP_VK_FULLSCREEN=1, the window is
  SDL_WINDOW_FULLSCREEN_DESKTOP: it covers the desktop at the display's own
  mode, so it is borderless in effect.
- Otherwise it is a decorated window at the backend's surface size.
- There is no choice of display, no live switch, and no "fullscreen" that
  takes the display exclusively.

**Required.**
- **Windowed**: a decorated, resizable window. Its size and position are
  remembered; it opens on the chosen display.
- **Borderless**: a window covering the chosen display exactly, at the
  display's current mode.
- **Fullscreen**: exclusive presentation on the chosen display, **at the
  display's current mode** (the same resolution and refresh). It must
  never switch to another mode. In Vulkan this is SDL's fullscreen with the
  desktop's own mode, or VK_EXT_full_screen_exclusive where the driver has
  it [H: which of the two holds the mode unchanged on this PC's driver is
  to be checked].
- **Live switching**: from the setting, and Alt+Enter between windowed and
  the last full-screen mode. The swapchain is recreated; no device loss and
  no restart.
- With several monitors, the display is chosen by index and falls back to
  the primary if that display is gone.

**Tests.**
- Each mode starts and logs its window flags and size.
- The display's mode (EnumDisplaySettings: width, height, refresh) is the
  same before, during and after each mode, and across live switches.
- Synchronisation validation: 0 errors across 20 switches.
- Grabs show the whole frame in each mode.

## 2. Resolution

**Today** [C, M]: the game draws a 1280x720 frame (its PS3 output: PPFX's
final composite, NATIVE_R1.md). The window or display shows it scaled.

**Required.**
- **The output size**: in windowed mode, window-size presets (1280x720,
  1600x900, 1920x1080, 2560x1440, 3840x2160, as far as the display holds)
  and free resizing. In borderless and fullscreen, the display's own size.
  The image keeps 16:9 inside any size, letterboxed or pillarboxed, never
  stretched.
- **The internal render resolution** (drawing the game above 720p) is a
  separate item. It needs the native renderer to draw PPFX's chain and asp's
  passes at scale (their targets, the reflection, the post-effects' sizes).
  It is not part of these settings until that work exists [H: its own work
  item; scope a decision for the owner when the renderer work is planned].
- The scaling filter for the 720p frame (linear by default; a sharp option
  for the pixel-art look) is a choice to offer [H: which options, a design
  decision].

**Tests.** Grabs at each preset and at odd sizes: no stretching, the bars
where expected. The game's frame time is unchanged by the output size at
720p internal (a short uncapped A/B).

## 3. VSync, separate from the frame limit

**Today** [C]:
- Presentation is FIFO (VSync on) unless PS3RECOMP_VK_PRESENT_MODE=mailbox
  or immediate, a benchmark switch.
- VSync caps an uncapped game at the display's refresh: 6.94 ms at 144 Hz.
- The frame limit is the game's own pacing (DOTHEROES_FPS, section 4).

**Required.**
- **VSync On**: FIFO.
- **VSync Off**: IMMEDIATE where the surface offers it, otherwise MAILBOX.
  The log says which.
- Chosen independently of the frame limit. The combinations:

| VSync | Limit | What the player gets |
|---|---|---|
| on | off, or above the refresh | the refresh rate, no tearing |
| on | a divisor of the refresh (72 on 144) | even pacing, no tearing |
| on | not a divisor (60 on 144) | the limit on average, presents on the nearest refresh (uneven); the setting says so and suggests "display" |
| off | a number | that rate, tearing possible |
| off | off | as fast as the game runs |

**Tests.**
- Each combination over 60 s of the fields scene: the presented rate (the
  present thread's intervals) and the game's frame rate (frames.log) as the
  table says.
- Tearing, with VSync off, is not tested automatically [H: a manual check].

## 4. The frame limit: presets, a custom number, uncapped

**Today** [C, port/game_patches.cpp, port/main.cpp]: DOTHEROES_FPS takes
four values, and any other number means no limit:
- 30: the game's original pacing (its own sleep and two vblanks);
- 60: one vblank a frame (the default);
- 0: none of the game's own pacing. **But outside benchmark mode a flip
  still completes on the next tick of the port's vblank clock, which runs at
  60 Hz** (port/main.cpp). So 0 ran at exactly 60 fps on the 143 Hz monitor
  [M, the owner's rel8b session, 2026-11-07: 95,474 presents, median 16.67
  ms]. Only DOTHEROES_BENCH=1 makes a flip complete when executed;
- display: a frame per display refresh (the vblank clock at the display's
  own period).

So "uncapped" in this milestone must also complete flips when executed, as
benchmark mode does, and must be tested by the measured rate, not by the
setting's name.

**Required.**
- **Presets**: 30 (the original pacing, kept for fidelity), 60, 120, 144,
  165, 240, and **display** (the monitor's refresh, read from the display,
  never set).
- **A custom number**: any whole frame rate in a sane range, say 20-1000.
- **Uncapped**.
- Except at 30, the limit is a host frame limiter at the flip, not the
  game's vblank patches: a deadline on the high-resolution clock, a sleep
  and then a short spin, so the cap holds evenly at any number.
- The game's own vblank pacing is off whenever a host limit applies.

**Tests.**
- The fields scene for 60 s at 30, 60, 75, 100, 144 and display, and at
  odd numbers (47, 113): the average within 1% of the cap, p99 frame time
  reported, no drift.
- The timing drive (tests/scripts: the camera target's distance a second,
  physics steps a second) the same at each cap, as R8 section 3 measured.
- Uncapped with VSync off reaches the release's uncapped rate.

## 5. Volume: master, music, sound effects

**Today**:
- [C] The port has no volume setting.
- [M] The game's own OPTIONS screen has never had its settings changed
  under test (HANDOFF "Game coverage").
- [H] Whether it offers its own music and effects volumes is not known.

**Required.**
- Three sliders, 0-100%, default 100, applied live and remembered.
- **Master** is a gain on the final mix the port hands the audio device:
  cellAudio's output to SDL. It never boosts above 100%, so it never clips.
- **Music** and **sound effects** need the game's own split between them:
  - [H] FMOD's event categories or channel groups, music against effects,
    reached through the game's FMOD calls (FMOD runs as lifted SPU and PPU
    code until the audio milestone);
  - or the game's own options, if it has music and effect volumes; the
    sliders then drive those.

  Which of the two is to be established first, by reading the game's FMOD
  set-up in the lift and its OPTIONS screen. Until then the music/effects
  split is an open requirement, not a design.
- The audio milestone (a native replacement for FMOD's mixing path) may
  move where the split is made. The settings stay the same.

**Tests.**
- The output captured over a fixed scene with each slider at 0, 50 and 100:
  silence at 0, level ratios measured.
- A music-only and an effects-only moment showing the separation.
- No change in frame timing (a real-audio A/B).

## 6. Button prompts: automatic, Xbox, PlayStation, Switch, keyboard

**Today** [M]:
- The game draws its own PS3 prompts (its artwork).
- Pads come through SDL (XInput, DualShock/DualSense, Switch Pro and
  others); the keyboard through the port's mapping.

**Required.**
- **A prompt style setting**:
  - **Automatic** follows the last device used: the keyboard and mouse, or
    a pad drawn in its own family (Xbox, PlayStation, Switch) as SDL names
    its type. An unknown pad gets Xbox's style.
  - **Xbox**, **PlayStation**, **Switch** and **Keyboard** fix the style.
    PlayStation is the game's own artwork, untouched (assets, below).
- **Separate from remapping**: a prompt shows the physical button that
  performs the game's action, after any remapping, in the chosen style.
  Changing the style never changes a binding, and the reverse.
- **The Switch layout**: its A/B and X/Y sit where Xbox's B/A and Y/X are.
  A prompt names the button that actually does the action (its label on
  that pad), never the Xbox position.
- **The confirm and cancel buttons** follow the game's own convention (the
  US release confirms with Cross), mapped through the bindings like any
  other action.

**Assets.** The owner placed them in `button assets/` at the repository's
top. It is untracked: nothing of it is committed until the prompts are
implemented and the files they use are chosen. It holds Kenney's Input
Prompts sets for:
- Xbox Series;
- Nintendo Switch;
- Keyboard & Mouse;
- Steam Controller.

Each comes as single PNGs at default and double size, SVGs, sprite sheets
with XML atlases, and a prompt font with its character map.
- **No PlayStation set, by decision** (the owner, 2026-11-07): the game's
  own prompts are PlayStation buttons already. The **PlayStation** style
  therefore shows the game's original glyphs, unchanged. Only the Xbox,
  Switch and keyboard styles replace them. Automatic picks PlayStation for
  a PlayStation pad, as the game draws it.
- **Licence**: Kenney's assets are CC0. The pack's licence file is not in
  the folder and should travel with whatever ships.
- Ship only the glyphs the prompts use, and no platform logos (the Xbox
  guide button's logo, say).

**What has to be established first** [H]: how the game draws a prompt.
- One possibility: a region of a texture atlas. The native renderer would
  then swap that region for the chosen style's glyph, by the texture's
  content hash, at the same size.
- The other: a character in its text font, a button code inside a string.
  The text layer would then map the code to a glyph sprite.

Which it is decides the design. A look at the title menu's, the system
menu's and a dialogue's prompts in a frame capture answers it.

The glyphs' look against the game's dot art (nearest-neighbour scaling, the
size) is a design decision for the owner.

**Tests.**
- Each style's glyphs in the title menu, the system menu, a dialogue prompt
  and the item pages: grabs against reference images. The PlayStation
  style's frames are identical to the game's without the feature.
- Automatic switching when the keyboard, then an Xbox pad, then a
  PlayStation pad is used.
- A remapped action's prompt shows the remapped button.

## 7. Remove the PS3 "Install Game" option

**Today** [M, C]:
- The title menu offers INSTALL (NEW GAME, CONTINUE, EDITOR, ALBUM,
  INSTALL), the PS3's copy of game data to its hard disk for shorter loads.
- The port runs it: the data goes into `data/gamedata/dev_hdd0`, kept
  across restarts (patches 0072-0078, tests/scripts/roundtrip_*.drive).
- On PC it does nothing useful: the data is local already.

**Required.**
- The title menu without INSTALL: the other items in order, the cursor's
  movement and wrap-around right, no blank slot.
- An install that already holds installed data keeps working, and plays
  the same as one that does not [H: whether the game loads differently when
  its data is installed, and what it shows when it is, is to be read in
  the lift before choosing].
- Done as a guarded instruction patch (config/insn_patches.json, as the
  frame-limit patches are) on the menu's item table or count, read from
  the lift first.

**Tests.**
- A title-menu drive: every remaining item reached and working, INSTALL
  absent, the cursor wrapping.
- An install with installed data from before the change: it loads and
  plays.
- Saves are untouched: CONTINUE loads the same save.

## 7a. The system's save-data list (found 2026-11-07)

**Today** [M: docs/NATIVE_R8.md section 9.3]: where the PS3 shows its
save-data list and the player picks (CONTINUE with several saves, EDIT DATA
on the character select), this port shows nothing and loads the first entry
the game offers. With the owner's three imported edits (GreenHero, BlueRoyal,
RedMage) only GreenHero can be chosen.

**Required**: a list in the port's own UI wherever the game calls the
system's list (cellSaveDataListLoad2 and its save counterpart): each entry's
title, detail and icon from its PARAM.SFO and ICON0.PNG, the game's focus
position honoured, the selection returned to the game as the PS3 would, and
cancelling returned as a cancel. The same prompts as section 6.

**Tests**: a drive that picks the second and third edit on the character
select and checks which DATA.EDT the game read; CONTINUE with two saves,
each picked once; a cancel.

## 8. Launcher and update management (after the base and the settings are stable)

**Today** [C, setup_app.py, tools/self_update.py]:
- The setup app converts the disc into an install and updates an install,
  redoing only the steps whose inputs changed and reusing compiled objects.
- It checks for a newer app (and can update the game after it), adds a
  Steam shortcut, starts the game, and writes dotheroes.cfg (the frame
  limit, fullscreen, the performance switches).
- The fresh-install and update paths for the native release are prepared
  as a test (tests/player_install_test.py), not yet run (docs/NATIVE_R8.md
  section 7.2).

**Required, in its own milestone after this one:**
- a launcher that starts the game, shows the installed build and version,
  and holds the settings above (or opens them in the game, depending on
  where the settings UI goes);
- update management:
  - the app checked and updated;
  - the game install updated (relift and rebuild with the cache);
  - the previous build kept until the new one is confirmed to start, with a
    way back;
  - saves and settings never touched;
  - works offline and fails safely.

**Tests**: tests/player_install_test.py's fresh, update, relift and
settings stages; an update from the previous release's install; a failed
update rolled back.

## Not in this milestone

- Internal render resolution above 720p (section 2).
- Remapping's own design (only its separation from the prompts is
  required here).
- RTX.
- The audio milestone's FMOD replacement (section 5's split may wait on
  its findings).

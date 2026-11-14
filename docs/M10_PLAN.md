# M10: native PC settings and the gameplay experience -- plan and checkpoint

M10 follows M9 (docs/M9_LAUNCHER_PLAN.md; finished 2026-11-14, its hands-on acceptance later). The requirements
gathered before are in docs/PC_EXPERIENCE.md sections 1-7b and docs/MILESTONES.md (M10); the owner's request of
2026-11-14 (below) adds to them and orders the work. Evidence marks as elsewhere: [C] read in the code, [M]
measured, [H] a hypothesis still to check.

## Checkpoint (kept current; the newest state first)

- **Stage**: M10.0 (this plan and the inventory) done; M10.1 next.
- **Done**: M9 finished and pushed (c2c251d). M10.0: the inventory below.
- **Tests**: --
- **Known issues**: see "Found in the inventory".
- **Outstanding decisions**: none blocking. Proposed defaults are marked "decided here" and can be changed by the
  owner at review.
- **Next steps**: M10.1 in the order of its section (the settings keys and their mapping; the frame limit; VSync;
  the display modes and the window; the performance overlay; the frame-rate verification runs).

## The owner's request (2026-11-14)

1. **Display and performance**: the game side of the display settings: windowed, borderless fullscreen, exclusive
   fullscreen where supported, resizable windowed, the window's size and place remembered; VSync on and off; frame
   limits 30, 60, 75, 100, 120, 144, 165, 240, unlimited and a custom value; an optional FPS and performance
   overlay. VSync and the limit independent. Never change the monitor's refresh rate or resolution; keep the
   game's aspect ratio. Verify gameplay speed, movement, physics, animation, timers, effects, movies and audio at
   several frame rates (not merely a higher cap); measure frame pacing, CPU use and input responsiveness.
2. **The in-game settings overlay**, inspired by Dusklight, in the launcher's look. Shortcuts: controller L3+R3
   held 500 ms, keyboard F1, configurable (other controller combinations too). The original controls kept -- the
   character editor's undo and redo above all; no accidental actions or input leaking when it opens or closes;
   the buttons released before it can open again. The shortcut shown briefly at start (can be turned off). Items:
   Resume Game, Display & Graphics, Audio, Controls, Performance, Return to Launcher, Exit Game. Gameplay paused
   safely while it is open; the original START pause menu kept. The launcher's UI components reused where
   practical; full controller, keyboard and mouse navigation.
3. **Audio**: master, music, sound effects, mute (OpenAL Soft), applied at once to sounds already playing; the
   movies' audio follows the master; the original behaviour at the defaults.
4. **Controls**: controller remapping, keyboard and mouse configuration, automatic prompt detection, manual Xbox,
   PlayStation, Switch, Steam and generic prompts, hot-plug and reconnection, restore defaults, conflicting
   bindings detected, the prompt preference shared with the launcher. No new vibration (M14).
5. **Return to Launcher / Exit Game**: a graceful shutdown; the launcher restored or reopened with the
   controller's focus; Exit closes the game and the launcher. Both confirm (unsaved progress), Cancel focused.
   Caches, settings and logs flushed, resources released. A launcher preference to close itself when the game
   starts.
6. **Skip startup logos** (on by default): no developer, publisher and middleware splash screens; straight to the
   original interactive title screen with its music, animation and attract mode; the new-game opening cinematic
   kept; all required initialization kept; the original sequence when off.
7. **The original interface**: the PS3 INSTALL menu option removed; the native save/load screens polished toward
   the game's look; the prompts in the player's chosen style; save compatibility and the save safeguards kept.
8. **One settings architecture**: the launcher and the overlay use the same authoritative file; live where
   possible, those needing a restart marked; concurrent writes never overwrite unrelated settings; a player's
   options, not implementation details.
9. **Stages**: M10.0 inventory; M10.1 display, VSync, frame-rate independence; M10.2 overlay and shared UI; M10.3
   audio and input; M10.4 logos, menu cleanup, save/load; M10.5 integration, regression tests, documentation.
   Incremental, each change profiled and tested; a genuine blocker asked at once; physical tests documented and
   the independent work continued; the owner asked before the screen is taken over again.
10. **Checkpoint**: completed work, stage, test results, known issues, outstanding decisions, exact next steps
    (above). No public release. M11 full-game testing and release readiness (Linux, AMD, more controllers,
    broader shader coverage); M12 Vulkan ray tracing and enhanced lighting, vendor-neutral; M13 character
    import/export and sharing; M14 controller vibration.

## How the work is tested without the owner's screen

The owner asked to be asked before the screen is taken over. Every automated run therefore stays off it:
- **The game**: offscreen (PS3RECOMP_VK_WINDOW=0: the renderer draws into its own images, nothing is shown),
  silent (ALSOFT_DRIVERS=null: OpenAL Soft's null driver, the mix still made), ended through its own quit path
  (DOTHEROES_DEV_STOP=1 and a quit.trigger file: the same request a window close makes). build/m9/gamelog_test.py
  is the pattern; tests/boot_smoke.py runs scenarios the same way when told to.
- **The launcher**: its scripted controller (--nav-test) with CUBIC_NAV_HIDDEN=1 (the window never shown; the
  pictures read back).
- What only a visible window can show (the display modes' look, exclusive fullscreen, tearing, the overlay over a
  real frame on the owner's monitor) is listed for the owner's hands-on session; nothing waits on it.

## M10.0: the inventory (2026-11-14) [C unless marked]

### Settings
- **The file**: `<install>/config/dotheroes.cfg`, NAME=value; schema config/settings_schema.json; tools/settings.py
  (check, read, set, reset, repair); the game reads it once at start (port/main.cpp read_settings: each value an
  environment default; the environment wins). The launcher writes through the worker (`settings --set`), which
  rewrites the file through tools/game_settings.write_game_settings (read, merge, write).
- **Game keys today**: DOTHEROES_FPS (60, 30, 0), PS3RECOMP_VK_FULLSCREEN (0/1: borderless), DOTHEROES_LOG_LEVEL,
  and the advanced performance switches. Launcher keys: LAUNCHER_DISPLAY_MODE, LAUNCHER_VSYNC, LAUNCHER_PROMPTS
  (auto, xbox, playstation, nintendo, steam, generic), the shader, update, save and log keys.
- **Writes from the game**: none today. The overlay will write; the launcher may write at the same time.

### Frame pacing and the clocks
- **The vblank clock** (port/main.cpp vblank_clock): a thread ticking cellGcmTickVBlank/TickFlip on a
  high-resolution deadline (sleep to within 1 ms, then yield); 60 Hz, or DIAG_VBLANK_MS (fractions allowed), or
  DOTHEROES_FPS=display (a whole number of the display's refreshes, at most 90 fps, read from DWM, never set).
- **The game's pacing** (port/game_patches.cpp): DOTHEROES_FPS=30 the original (its 30 ms floor and two vblanks a
  frame); 60 one vblank a frame; 0 none of the game's own pacing -- but a flip still completes on the next vblank
  tick, so "0" runs at the vblank clock's rate (PC_EXPERIENCE section 4: measured 60 fps). DOTHEROES_BENCH=1 is
  the only truly uncapped mode (flips complete when executed: cellGcm_set_flip_immediate).
- **Frame-rate independence** (docs/NATIVE_R8.md section 3, game_patches.cpp): the game's logic steps by measured
  time; the hero's step rounding is carried between frames (dotheroes_step_round); objects written for a locked
  30 fps are gated to 30 Hz steps (DOTHEROES_TICK30). Verified before at 30, 60, about 120, 144 and 200 fps with
  tests/fps_check.py (a drive: standing, walking, cutting grass, debris settling; frames, travel, physics steps,
  the static clocks and the player's object compared against the 30 fps reference).
- **Measuring**: DIAG_FRAMES (every flip request and present, tests/frame_log.py), PS3_FTRACE, the game log's
  `[frame]` summary (M9), PS3_OSD_FPS (an on-screen readout: fps, mean and longest frame; =2 a graph).

### Display
- **The window** (ps3recomp libs/video/rsx_vulkan_backend.c vk_window_open): SDL, centred, the backend's size
  (1280x720), not resizable; PS3RECOMP_VK_FULLSCREEN=1 SDL_WINDOW_FULLSCREEN_DESKTOP. No exclusive mode, no place
  remembered, no live switch.
- **Presentation**: FIFO unless PS3RECOMP_VK_PRESENT_MODE=mailbox|immediate (a benchmark switch); a lost or
  suboptimal swapchain is recreated (vk_swapchain_lost) -- the path a resize needs.
- **The picture**: the game's 1280x720 frame blitted to the whole swapchain image (linear): **stretched** when the
  window is not 16:9 -- found here; M10.1 letterboxes it.
- **Host pictures over the frame**: copied into the swapchain image after the game's (vk_osd_draw one-line
  messages, vk_osd_fps_draw, vk_osd_saves_draw the save-data screen, vk_osd_list_draw). The save-data screen is
  composed on the CPU (libs/video/osd_saves.c: stb_truetype text in the title's font, rounded panels, images, the
  prompts in the player's style) into a window-sized picture -- the model for the overlay (M10.2).
- **Escape quits the game at once** (rsx_vulkan_backend_pump_messages), except while a host dialog is open --
  found here; M10.2 makes Escape and the window's close button ask first (Exit Game's confirmation).

### Input
- ps3recomp libs/input/cellPad.c: SDL GameController (hot-plug: pad_sdl_hotplug; each pad's family:
  pad_sdl_family), XInput, and a fixed keyboard map (arrows, Z X A S, Q W 1 2, Return, Tab). The SDL buttons map to
  the PS3's in code (pad_sdl_read). A host dialog takes the input while open (cellPad_ui_intercept): the model for
  the overlay's input capture. The prompt style for host screens: the save-data screen's (PS3_UI_PROMPTS_N).
- L3 and R3 reach the game (CELL_PAD_CTRL_L3/R3): the overlay's hold must not trigger the editor's actions [H:
  which buttons the character editor's undo and redo are, read in the lift or by the owner's description].

### Audio
- port/native/audio: OpenAL Soft (al_device), the music streams (al_stream Streamer: per-stream gain, fades), the
  sound effects (voice_pool), the game's sound manager (sound_manager; the game's own music and effects levels,
  options +0x948 and +0x94C, docs/OPENAL_MIGRATION_PLAN.md section 1); the movies (native/movie, FFmpeg) play
  through the same device [H: through the Streamer; to confirm]. The master: OpenAL's listener gain (every source,
  the movies' too). Music and effects: a factor on the streams and on the voices respectively.

### Startup, menus, saves
- The startup logos are movies or screens before the title [H: which, and the branch that skips them, to be read
  in the lift; docs/FFMPEG_MIGRATION_PLAN.md lists the movies].
- The title menu's INSTALL (PC_EXPERIENCE section 7): a guarded instruction patch on the menu's table or count,
  read from the lift first.
- The save-data screen: docs/SAVE_UI.md.

### Found in the inventory
1. The picture is stretched in a non-16:9 window (M10.1 fixes it with the resizable window).
2. Escape closes the game without asking (M10.2).
3. "Unlimited" (DOTHEROES_FPS=0) is capped by the 60 Hz vblank clock (M10.1).

## M10.1: display modes, VSync, frame-rate limits

**Settings** (the schema; all `owner: game`; the launcher's Settings page and later the overlay):
- `DOTHEROES_DISPLAY_MODE`: windowed (default), borderless, exclusive. PS3RECOMP_VK_FULLSCREEN=1 in an existing
  file maps to borderless (kept as a hidden, advanced key).
- `DOTHEROES_VSYNC`: 1 (default) / 0.
- `DOTHEROES_FPS`: 30, 60 (default), 75, 100, 120, 144, 165, 240, display, 0 (unlimited), custom; with custom,
  `DOTHEROES_FPS_CUSTOM` (20-1000).
- `DOTHEROES_PERF_OVERLAY`: off (default), fps, graph.
- The window's size and place: not a setting -- `<install>/config/window.json`, written by the game when it
  closes a windowed session (decided here: a state file of its own keeps the game from writing the settings file
  for a window move).

**The frame limit** (decided here: the existing vblank clock is the limiter -- a deadline on the high-resolution
clock with a sleep and a short wait, as PC_EXPERIENCE section 4 asks, already verified for frame-rate independence
at several rates): 30 the original pacing; any other number: one vblank a frame with the clock's period
1000/limit ms; display as today; unlimited: flips completing when executed (as the benchmark mode) with the
game's own pacing off.

**VSync**: on FIFO; off IMMEDIATE where the surface has it, else MAILBOX; logged.

**Display modes**: windowed (decorated, resizable, size and place restored and kept on a display that exists),
borderless (SDL_WINDOW_FULLSCREEN_DESKTOP: the display's mode), exclusive (SDL_WINDOW_FULLSCREEN with the
display's *current* mode -- the mode never changed; refused by the driver: borderless, logged). The picture
letterboxed or pillarboxed to 16:9 in any window.

**Performance overlay**: PS3_OSD_FPS from the setting (fps, mean and longest frame; the graph).

**Tests** (offscreen unless said): the effective values in the log; each limit's measured rate (DIAG_FRAMES:
average within 1% of the cap, the p99 frame time, no drift) at 30, 60, 75, 100, 144, 240, a custom 113 and
unlimited; tests/fps_check.py's drive at 30 (reference), 60, 144, 240 and unlimited (travel, physics steps, clocks
and the player's state following time); CPU use (the process's CPU time a second) at each; the display mode's
before/during/after EnumDisplaySettings unchanged (needs a window: the owner's session, or a hidden window if the
driver allows); input-to-frame latency estimated from the pad's poll to the next flip (the frame time bounds it).

## M10.2: the in-game overlay

- **Drawing**: a CPU-composed picture like the save-data screen (libs/video/osd_saves.c's primitives and font),
  over a dimmed copy of the last game frame (one read-back when it opens), in the launcher's look (its colours,
  pixel title font, panels, the gold focus, the prompts bar).
- **Input**: while open, the pads and keyboard go to the overlay (cellPad_ui_intercept), the game sees nothing;
  the opening buttons must be released before it can open again; the mouse moves and clicks.
- **Shortcut**: L3+R3 held 500 ms (configurable: a list of combinations), F1; a brief hint at start
  (`DOTHEROES_OVERLAY_HINT`).
- **Pause**: the guest held while the overlay is open [H: the safest point -- the PPU main thread at its frame
  wait, with the vblank clock and audio paused; to be designed after reading how the save-data dialog waits].
- **Pages**: Resume; Display & Graphics; Audio; Controls; Performance; Return to Launcher; Exit Game (each
  confirmed, Cancel focused). Settings written through one writer that merges (below).
- **One writer**: the game writes the settings file by read-merge-write under a lock file
  (`config/dotheroes.cfg.lock`), the worker the same, so neither overwrites the other's unrelated values.

## M10.3: audio and input

- Master (listener gain), music and effects (factors in the native sound manager and the Streamer), mute; live.
- Remapping: a binding table (pad buttons and keys to the PS3's buttons) in the settings, applied in cellPad;
  conflicts shown; defaults restorable; prompt style shared (LAUNCHER_PROMPTS becomes the game's too).

## M10.4: logos, the menu, saves

- Skip the startup logos (`DOTHEROES_SKIP_LOGOS`, on by default) by a guarded patch at the branch the lift shows;
  the title's music, animation and attract mode untouched; the new game's opening kept.
- The title menu without INSTALL (PC_EXPERIENCE section 7).
- The save-data screens' polish (docs/SAVE_UI.md).

## M10.5: integration

The whole set together, the regression runs (the R8 timing drive, the fights, the reference scenes), the
documents (README, the launcher plan, this checkpoint).

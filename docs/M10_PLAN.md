# M10: native PC settings and the gameplay experience -- plan and checkpoint

M10 follows M9 (docs/M9_LAUNCHER_PLAN.md; finished 2026-11-14, its hands-on acceptance later). The requirements
gathered before are in docs/PC_EXPERIENCE.md sections 1-7b and docs/MILESTONES.md (M10); the owner's request of
2026-11-14 (below) adds to them and orders the work. Evidence marks as elsewhere: [C] read in the code, [M]
measured, [H] a hypothesis still to check.

## Checkpoint (kept current; the newest state first)

- **Stage** (2026-11-16): M10 done -- M10.1-M10.5 implemented and tested without the owner; the owner's hands-on
  checks (below) remain, as M9's do.
- **Done**: M9 (d2d9852). M10.0 the inventory. M10.1: display modes, the remembered window, VSync, the frame
  limits through a host limiter, the performance overlay, the picture's aspect. M10.2: the in-game overlay (the
  hold, the pages, the shortcut kept from the game, Return to Launcher and Exit Game with the launcher's side, the
  one-writer settings). M10.3: volumes, controller and keyboard bindings, the shared prompts. M10.4: the logo skip.
- **Tests** (all offscreen or with a hidden window, silent; evidence on F: m10/evidence):
  - frame limits at the title: 30, 60, 75, 100, 120, 144, 165, 240, display (144.00) and a custom 113 each within
    0.15% of the cap; unlimited about 1,100 fps. The host limiter with its high-resolution timer: 60 at 60.00 fps,
    p99 18.0 ms, 43% of a core (the vblank pacing it replaced: 42%; a Sleep-and-yield wait: 64%); 144 at 144.00,
    p99 8.2 ms, 76%; 240 at 240.01, p99 4.9 ms, 65%;
  - the logo skip: the title's movie opened 3.6 s after the start against 21.4 s without (twice each); 24.7 s of
    the game's time passed in the speed-up; the title's movie then at its own pace (243 frames in 8 s);
  - the bindings through the menu: Cross given the pad's Circle wrote `DOTHEROES_PAD_MAP=Circle=Cross,Cross=Circle`
    (the clash swapped); "Restore the defaults" emptied it;
  - frame-rate independence in play (tests/fps_check.py, R8's castle-front drive) at 30, 60, 144, 240 and
    unlimited: the hero's walk 5.00 units a second (one camera-target measure missed a walk the drive's own
    measure saw: +9.15), physics 60 steps a second, the time-based clocks x1.00 at every rate; 60 and 144 held
    exactly (60.0, 143.8-143.9);
  - the hidden window: VSync on caps unlimited at the display's 141-144 Hz, off reaches 883 fps (IMMEDIATE); the
    window's place and size written at exit and restored (1000x700 at 140,120); the display's mode unchanged
    before, during and after every run;
  - the hold in play: 10.01 s between two snapshots with 6.03 s held -- the game's clocks advanced 3.99 s and
    4.00 s; no flip while held; no long frame after it;
  - the overlay through its test input: opened (the game held), Audio's master 100 -> 90 applied at once and
    written, the performance overlay turned on and written, the exit question (Cancel focused) cancelled, resumed
    (the game released); Return to Launcher: `session-end` "launcher", the game's own exit path;
  - unit tests: the new settings (every frame-limit value, custom, VSync, display mode; invalid ones refused).
- **Added after the first push (e6bf071)**:
  - the title menu without INSTALL (config/insn_patches.json, guard g_dotheroes_no_install_item;
    DOTHEROES_TITLE_INSTALL=1 shows it again for the install round-trip drives): the menu's item count -- the
    Title script's Menu.Size, or ClearMenu.Size after the game is cleared -- read at run time by the menu's drawing
    (func_005CD8E8) and its input (func_005CE018), one fewer after each read; INSTALL is the last item in both
    menus, so the cursor wraps at ALBUM and INSTALL's action is never chosen. tests/scripts/title_no_install.drive:
    NEW GAME, CONTINUE, EDITOR, ALBUM; down from ALBUM to NEW GAME, up back to ALBUM, ALBUM opens; with
    DOTHEROES_TITLE_INSTALL=1 the same drive reaches INSTALL. The cleared game's menu (6 items to 5) is covered by
    the same two reads but was not run (no cleared save at hand);
  - a silent movie's clock follows a hold (port/native/movie/bink_native.cpp): a hold over the title's movie no
    longer leaves a long frame after it (0.68 s before; none now);
  - no copy of every presented frame into host memory for a player (PS3RECOMP_VK_READBACK=0 unless a frame dump
    is asked for: only the test tooling reads it);
  - the bindings and the chord guard tested in the runtime (libs/input/tests/test_pad_bindings.c, a fake pad): the
    game's Cross pressed by the pad's Circle and the reverse, the port's own read unchanged, the defaults back;
    L3 alone reaching the game after its hold-back, a quick R3 tap passed on afterwards, L3+R3 held 600 ms never
    reaching it;
  - the save screens: they already follow the reference the owner gave (2026-11-10); here they take the shared
    prompt choice (LAUNCHER_PROMPTS) from the start; their look is unchanged pending the owner's view.
- **The "144 fps cap" was VSync, and a test fault**: tests/boot_smoke.py with INSTALLED= dropped the window switch,
  so the installed game opened its (visible) window with VSync on -- the play drives at 240 and unlimited in
  batches 1, 2 and 4 ran in a window on the owner's desktop at the display's 144 Hz, and the river-fight
  regression run asked for a visible, topmost window itself (its scenario's `#! env WINDOW=1 VISIBLE=1
  TOPMOST=1`). Both against the owner's instruction; reported to the owner. Fixed: boot_smoke keeps an INSTALLED
  run offscreen unless WINDOW=1; scenario runs pass WINDOW=0 VISIBLE=0 TOPMOST=0. Offscreen, play at 240 and
  unlimited runs at 143-160 fps (the castle front's own cost), the walk 5.01-5.16 units a second, physics 60
  steps a second, the clocks x1.00.
- **Known issues**: none open in M10's code. Not covered automatically (the owner's hands-on checks): borderless
  and exclusive fullscreen and the live switch between modes, tearing with VSync off, the overlay over a real
  frame and its mouse use, L3+R3 on a real controller in the character editor, the remapped buttons in play,
  PlayStation, Switch and Steam controllers, the cleared game's title menu.
- **Outstanding decisions**: none blocking. Proposed defaults ("decided here") can be changed at review: the
  shortcut chords offered, the window state file, the logo skip's x8 speed-up, the save screens' look.
- **Next**: the owner's M9 and M10 hands-on acceptance; then M11 (full-game testing and release readiness).

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

**The frame limit** -- a host limiter at the flip, as PC_EXPERIENCE section 4 asked (ps3recomp
include/ps3emu/guest_pause.h ps3_frame_limit_wait, called by the guest's render thread at each flip request;
port/main.cpp apply_pc_settings): 30 keeps the original pacing (the game's own sleep and two vblanks); every other
limit turns the game's own pacing off (DOTHEROES_FPS=0, each flip completing when executed, as the benchmark mode)
and holds each frame to its deadline on the high-resolution clock (a high-resolution waitable timer to 0.2 ms
before it, then a yield); "display" is the display's refresh period (read from DWM, never set); unlimited has no
deadline. Tried first and replaced: the limit through the game's vblank pacing (one vblank a frame, the vblank
clock at the limit's period) held exactly at the title, but in play a frame a little over one tick waited for two
-- 144 and 240 ran at 65 fps on the castle-front drive while unlimited ran at 144.

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

## M10.2: the in-game overlay (port/overlay.cpp)

- **The hold** (ps3recomp include/ps3emu/guest_pause.h; port/native/dev_state.cpp dotheroes_host_pause): every
  guest clock stands still (the timebase, sys_time_get_system_time, sys_time_get_current_time take off the time
  held), the guest's render thread waits at its next flip request (its frame loop stops between two frames), and
  OpenAL Soft's device pauses (ALC_SOFT_pause_device: music, effects and a movie's sound resume where they were).
  Its other threads run on but see no time pass. The log's frame watch leaves the held time out.
- **Drawing**: composed on the CPU at the window's size -- the paused frame (read back once when it opens: the
  renderer copies its last frame for it) scaled with its aspect kept and dimmed toward the launcher's night blue; a
  panel in the launcher's look (its colours, its stepped-corner frame, the project's 5x7 pixel font for titles --
  port/ui_pixel_font.h, now shared by the launcher and the game -- the gold focus and the pixel cursor); text in the
  game's UI font and the prompts in the player's style (libs/video/osd_saves.c's drawing, made public as
  ps3_ui_*). The renderer copies the picture over the frame in each present (g_ps3_osd_menu); while the game is
  held the overlay presents itself (g_ps3_osd_present_again).
- **Input**: the pads read directly (cellPad_host_buttons), the keys and mouse while the window has the keyboard
  (cellPad_host_keys); the game's pad withheld while it is open (cellPad_ui_intercept). D-pad, left stick
  (repeat after 400 ms), A/Cross or Start choose, B/Circle back; arrows, Enter, Space, Esc, Backspace; the mouse
  focuses a row under it and a click chooses it.
- **Shortcut**: the chosen chord held 500 ms (`DOTHEROES_OVERLAY_COMBO`: L3+R3 by default, Back/Select+Start,
  L1+R1+Back/Select), F1, Escape; the window's close button opens Exit Game's question (a second close then
  closes). The chord kept from the game (cellPad_set_chord_guard): its buttons alone reach the game as before
  (each held back at most 120 ms in case the rest follows; a quicker tap is passed on afterwards), the whole chord
  never does -- the character editor's undo and redo stay the player's. Released before it can open again; the
  menu closes only once every button and key is up. Not over the game's own save screen. A hint at start for 6 s
  (`DOTHEROES_OVERLAY_HINT`).
- **Pages**: PAUSED (Resume Game, Display & Graphics, Audio, Controls, Performance, Return to Launcher, Exit
  Game); Display & Graphics (display mode, VSync -- both at once, on the window's thread); Audio (master, music,
  effects, mute -- at once); Controls (button prompts -- shared with the launcher -- the shortcut, the start hint);
  Performance (frame limit -- at once between any two limits but 30 --, the performance overlay, log detail -- at
  the next start, said under the items). Return to Launcher and Exit Game ask first ("Progress since your last save
  will be lost."), Cancel focused.
- **Ending the session**: `<install>/logs/session-end` says how (launcher, exit); the game is released, its own
  shutdown runs (ps3_host_quit: the request to its callbacks, then exit with the exit handlers: the window's place,
  the caches, the log). The launcher reads the file when the game's process ends: Exit closes it too; Return
  brings it to the front with the controller's focus. With the new launcher setting "Close the launcher when the
  game starts" (`LAUNCHER_CLOSE_ON_START`) the game starts it again (`CUBIC_LAUNCHER`, `--after-game`).
- **One writer**: the game writes the settings file by read-merge-write under `config/dotheroes.cfg.lock`, and
  the worker (tools/game_settings.py) takes the same lock, so neither overwrites the other's values.

## M10.3: audio and input

- **Volumes** (port/native/audio/sound_manager.cpp nsm_set_volumes; settings DOTHEROES_VOLUME_MASTER, _MUSIC,
  _SFX 0-100 and DOTHEROES_MUTE; the launcher's Settings and the overlay's Audio page): music and effects as factors
  on their own sources, on top of the game's own levels (its options' music and effects levels stay as the game
  sets them); master and mute on OpenAL's listener -- every source of the context, a movie's sound too; applied at
  once to what is playing (every source's gain set again). 100/100/100 unmuted: factors of 1, the original output.
- **Bindings** (ps3recomp libs/input/cellPad.c; settings DOTHEROES_PAD_MAP, DOTHEROES_KEY_MAP; the overlay's
  Controls page: "Controller buttons..." and "Keyboard keys..."): each of the game's sixteen buttons pressed by a
  pad button of the player's choice (applied to what the game is given -- the menu reads the pads as they are, so
  a binding can never lock the player out of it) and a key of their choice. Choosing a row waits for the button
  or key (5 s; Esc keeps a key); one already bound to another of the game's buttons is swapped onto it -- never two
  on one, never one left without -- and the menu says which; "Restore the defaults". The keyboard's defaults gain
  L3 and R3 (3 and 4), which it lacked. The rows name the pad's buttons in the player's prompt style.
- **Prompts**: the launcher's LAUNCHER_PROMPTS is the game's too (its own host screens: the save list) from the
  start and when changed in the overlay; Steam and generic draw Xbox's shapes there. Automatic follows the
  controller in use (cellPad's device family), as before. Hot-plug and reconnection: the runtime's (cellPad's pad
  thread, docs/PC_EXPERIENCE.md section 7b).
- Not here: new vibration (M14).

## M10.4: logos, the menu, saves

- **The start-up logos** (port/main.cpp logos_begin; DOTHEROES_SKIP_LOGOS, on by default, the launcher's Settings,
  Game): nothing is cut out -- the original start-up runs whole, every initialization in it, with the guest's
  clocks eight times as fast (ps3emu/guest_pause.h ps3_guest_warp_set: a time added to every guest clock, never
  decreasing) and no frame limit, so its screens pass in a moment. It ends when the first movie opens (the
  title's background: the interactive title with its music, animation and attract mode at their own pace), at a
  save-data screen, or after 30 s -- always before a new game, so its opening is untouched. Off: the original pace.
- The title menu without INSTALL: done (the checkpoint above).
- The save-data screens: the shared prompt choice; their look unchanged pending the owner (the checkpoint above).

## M10.5: integration

The whole set together, the regression runs (the R8 timing drive, the fights, the reference scenes), the
documents (README, the launcher plan, this checkpoint).

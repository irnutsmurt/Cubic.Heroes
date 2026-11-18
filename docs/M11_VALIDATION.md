# M11.1: the Windows validation matrix and the bug list

The owner's M11.1 (docs/M11_PLAN.md section 13): the game validated area by area before a Windows beta, with the
developer tools and automated scenarios wherever they can see the thing checked, RPCS3 where rendering is in doubt,
and the owner's own play only where nothing else can. This file is the matrix and the bug list; it is kept current
as each stage runs. Map, script and enemy names below are the game's own identifiers (as in the other plans); no
game text, data or picture is in this repository.

**Methods**: **S** an automated scenario (tests/scenarios/*.drive, offscreen); **W** the map sweep
(tests/map_sweep.py); **T** the developer tools (dev.cmd; docs/M11_PLAN.md section 12); **R** compared with RPCS3
(tests/rpcs3_reference.py, or a capture); **O** the owner's own hands (asked for, never assumed). **States**:
`pass` (with its evidence), `fail` (a bug below), `partial` (what is missing said), `todo` (not run in M11.1 yet),
`owner` (waiting on a hands-on check). Evidence older than M11 is named but counts as `todo` until run again on
the M11.1 build.

**Running what is automated** (every one offscreen and silent; a build's results go into the rows below):
- `tests/devtools_suite.py --bin EXE [NAMES]` -- the developer tools' scenarios: basic, off, warp, menu, player,
  progress, encounter, diag, persist (+ persist_load), boss, enemies, ending; `--shaders DIR` over a preparation;
- `tests/devtools_suite.py --installed DIR` -- the install checks (a throwaway install: developer mode on, then off);
- `tests/map_sweep.py run EVIDENCE --bin EXE --shaders DIR` then `check EVIDENCE --baseline BASELINE` -- every map;
- `tests/rpcs3_reference.py run all --bin EXE` -- the reference scenes against RPCS3's captures;
- the unit tests (tests/test_*.py; the runtime's tests/test_rsx_texture_layout.c and the others).

## 1. Places: every map (167)

| Group | Maps | Method | M11.1 state | Evidence, notes |
|---|---|---|---|---|
| Fields (overworld) | 32 (Field_1..Field_35 less the missing numbers) | W, S | partial | sweep-warp1: every one arrived at its own number, frames advancing, no translation in play; Field_14's pipelines explained (PLAN 15). Visual review: todo |
| Towns (field screens with a village or the castle) | 4 | W | partial | arrived (sweep); visual: todo |
| Castle grounds, castle interiors | Field_50, D_20_1/2/9 | W | partial | arrived; visual: todo |
| Village and town interiors | 24 (D_21_x .. D_26_x) | W | partial | arrived; visual: todo |
| Caves, springs, mountain paths | 38 (D_60_x) | W | partial | arrived; visual: todo |
| Temple 1 (Grass) | D_1_1_1, D_1_2, D_1_3 | W, S | partial | arrived; R-series routes through the dungeon (dungeon1_*); M11.1 boss row below |
| Temple 2 (Forest) | D_2_1_1, D_2_2, D_2_3 | W | partial | arrived |
| Temple 3 (Desert) | D_3_1_1-2, D_3_2, D_3_3 | W | partial | arrived |
| Temple 4 (Aqua) | D_4_1_1-2, D_4_2, D_4_3 | W | partial | arrived |
| Temple 5 (Flame) | D_5_1_1-3, D_5_2, D_5_3 | W | partial | arrived |
| Temple 6 (Wind) | D_6_1_1-5, D_6_2, D_6_3 | W | partial | arrived |
| The tower | D_7_1_1-7, D_7_2_1-9, D_7_3 | W | partial | arrived |
| The castle battle | D_8_2 | W | partial | arrived |
| Minigame arenas | 18 (D_80_x) | W | partial | arrived; four minigames started and played a while (section 5); the rest: todo |
| Ending maps | Field_90, Field_91 | W | partial | arrived; the ending's own path: section 10 |
| The inn's dream map | Field_80 | W | partial | arrives through its own arrival record (M11.0b) |
| The character editor's world | Field_99 | S | partial | not in the map list: reached only through the editor (PLAN 16); archived editor runs load it; M11.1 editor rows: section 7 |

## 2. Bosses (15 encounters)

Each: entered the game's way (the arena's own event), the introduction, the fight to its **defeat**, its
**rewards** (the life vessel, money, items) and the **progression** it sets (the defeat flags, the sage's room or
what follows). Method: T to enter (`boss ID`), S to fight (no death on; the drive's `fight`), T to read the flags.

| Encounter | Arena | M11.1 state | Notes |
|---|---|---|---|
| Temple 1's guardian | D_1_2 | pass | devtools_boss (PLAN 18): introduction, the fight to the defeat (flags 6, 8), its money, the life vessel (max life 10 -> 12), the sage's room (flag 7, the first spell); its look and sound against RPCS3: todo |
| Temple 2's guardian | D_2_2 | pass | devtools_bosses (PLAN 22): the fight to its defeat flag (11), drops collected, its after-event answered |
| Temple 3's guardian | D_3_2 | pass | devtools_bosses: defeat flag 22 |
| Temple 4's guardian | D_4_2 | **PASS** | won by the game's mechanics (PLAN 29: the story's sword, the Wire Rod, the hero invulnerable): the head followed across the arena by walks and the rod's pulls, wounded through its three phases (six arms under 440, eight under 220), relocating after each wound; arms cut and grown again 40 s later; ink volleys and swats; defeated in about 90 s (80 swings); its after-event (the fairy freed), flags 33 and 37, life 10 -> 12 (its heart) |
| Temple 5's guardian | D_5_2 | pass | devtools_bosses: defeat flag 42 |
| Temple 6's guardian | D_6_2 | pass | devtools_bosses: defeat flag 50 (11 min) |
| The castle battle | D_8_2 | pass | devtools_bosses: scripted -- the priest beats the hero down (WaitHpOrTime), the sages, flag 64, the move to the castle |
| The tower's rematches 1, 2, 3, 5, 6 | D_7_2_1..6 | pass | devtools_bosses: flags 311, 313, 315, 319, 321; stronger (the tower's flag 310) |
| The tower's rematch 4 (the kraken) | D_7_2_4 | **PASS** | its stronger variant (life 1320, faster, contact damage 4) won the same way on the same arena (PLAN 29): six arms at 34 s, eight at 116 s, defeated at 153 s (180 swings); flag 317 |
| Before the last (the priest again) | D_7_2_7 | PASS | his offer's outcomes pass (tests/devtools_priest.py: accepted twice -- the game over, continued at the tower's entrance or declined to the title; refused -- the fight begins with his summons); the fight won by its mechanics (PLAN 30: the Point Light lit five times, the story's sword on the real priest, his summons' apples for the magic; 1200 -> 0 in 361 s), his after-event to D_7_2_8 (area 77). Not verified: his shadow against the original |
| The last enemy | D_7_2_8 | pass | devtools_bosses: the actual fight to its defeat (flag 74, cleared again by the ending it starts); its after-event onward: section 10 |

## 3. Enemy types (41, and 21 minigame versions)

Each type seen, its model and effects drawn (and its pipelines prepared), its movement and collisions, its sounds,
damage both ways, its death and drops. Method: T (spawned through the game's own object set-up: PLAN 19) or met on
its maps (W/S); R where a material is in doubt.

| Types | M11.1 state | Notes |
|---|---|---|
| Rabbit, SpiderBlue, Skeleton, MageGray, Ghost, Wood, WormRed, WormBlue, Bee, Jyugemu, Rock, Golem, SpiderRed, GoblinRed, GoblinBlue, Zombie, MageBlue, Squid, ScorpionRed, ScorpionBlue, Vampire, Bat, SlimeRed, SlimeBlue, DarkEye, CentaurusBlue, Mummy, RoperRed, RoperBlue, SilverKnight, BlackKnight, CentaurusRed, Cyclops, Succubus1, Succubus2, Shadow, Togetoge, TogetogeDash, GolemChild | partial | devtools_enemies (PLAN 19): every type created by the game's own set-up, found in the game's actor list, drawn (pictures reviewed); damage both ways, death, drops and sounds per type: todo. About 29 take a stronger version at random (StrongRate) |
| HagureSlime (rare: 5%, 10% in the second mode) | fixed (B-002, B-004); look against RPCS3: todo | was never drawn; now drawn with the engine's own cube map, decoded right; its pipelines in the preparation (PLAN 15) |
| BlueDragon (rare: 1%, 20% in the second mode) | partial | spawned at once and drawn (devtools_enemies); on its own spawn point on field 13 with the scripts' random forced (devtools_forced, PLAN 28), gone with the ordinary roll |
| The 21 minigame versions (MG*) | todo | met in their minigames (section 5); `spawn` refuses them elsewhere |

## 4. The main story (26 steps)

| Rows | Method | M11.1 state |
|---|---|---|
| Each step's flags set by playing it, in order (the story's own events, items and map progress) | S, T, O | todo |
| `progress set` (the tools) against the game's own state after each step | T | partial (devtools_progress: temple 1's boss room before and after; devtools_ending: step 24 lets the ending chain run) |

## 5. Side quests and optional content

| Rows | Method | M11.1 state |
|---|---|---|
| The quests the scripts keep (`quests`: each script function's flags), each started and finished | T, S | todo |
| The 18 minigames (D_80_x), each played to a result and its record | S | partial (build/m11 minigames1: four entered the game's way -- each map's header event -- and started: Blockout (D_80_1) its wall and arena, a Dash Circuit (D_80_2) its course and start lights, Block Defense (D_80_3_1) its overview, then play with its wave, crystal and life panels, and an extra Block Defense stage (D_80_4_1); played with plain inputs, none to a result. build/m11 minigamesresult (each left alone to end by itself): Block Defense's overview and controls pages wait for presses, its first wave then starts; the Dash Circuit waits at its start lights for the player -- neither ends without play, so a result needs a drive that plays them (todo). To compare: the extra stage drawn snowy at its overview and all pink in play (a season of the stage's, or colours wrong)) |
| Shops (buy, sell, the wallet), the inns, the sages' rooms, the signs and the people | S | partial (installed_save: an arrow bought, kept across a save) |

## 6. Items, equipment, magic and abilities

| Rows | Method | M11.1 state |
|---|---|---|
| Every sub-weapon used (bow, bombs, boomerang, anchor rod, fire rod, ...) and its effect and sound | T, S | partial (the menu: the sub-weapons and the six spells listed and marked for the item button; on the field L2/R2 cycle the item button, CIRCLE uses it: the casting probe pending) |
| Swords (95-118) and shields (89-94): given, equipped in the game's own screen, swung; the sword's full-life beam | T, S, O | partial (build/m11 equip4: swords 97 and 106 and shield 90 given; the equipment screen's tabs (L1/R1), the Swords tab with the Ancient Sword equipped, the Hero Sword's stats, chosen and equipped -- the player's sword 1 -> 3 -- and swung at its length; the full-life beam in the boss fights (PLAN 30). A shield equipped through the screen: pending -- build/m11 miscchecks gave shields 90 and 91, but its presses reached the Swords tab, not the shields' (the drive's navigation, to correct)) |
| Magic: each spell cast, its effect, its cost | T, S, R | partial (Point Light: cost 3, the room dark but for the torch it lights, 30 s, recast, magic back from apples -- PLAN 30. build/m11 magic2/magic3 on Field_14, the item button cycled, each cast given 40 s: four spells cast at costs 1 (the hero glints), 3 (rings about him), 4 (the screen as ice, still at 15 s) and 5 (nothing seen on the field); Point Light and the purple spell crossed out there by the game (not castable; Point Light needs the dark), the list's other entry a sub-weapon. RPCS3 (build/m11 magic_save: the game's save after those steps, continued in the scratch RPCS3 and here, the same presses): the purple spell crossed out and not cast there too; the yellow and red casts' first frames as white (with green blocks) there, the yellow's glint and the red's rings the same at 4 and 15 s; the blue's ice the same. The blue cast at the castle front (magic_save2): its item, white flash, close-up with its tablet over the hero, and ice at 4 and 15 s frame for frame as RPCS3's; the bottom quarter black once at Field_14 (magic3, 0.6 s in) not seen there -- likely the wipe into the close-up caught, not verified. In the first temple among skeletons (build/m11 miscchecks, magic 12 before each cast): yellow cost 1, red 3, blue 4 (the screen as ice), **green 5: its close-up with its tablet (an hourglass), then the room dimmed while it lasts, its icon crossed out until it ends** -- the green one's effect seen; the orange (Point Light) and the purple crossed out there too. Pending: the purple spell where the game allows it; the green one against RPCS3) |
| Consumables (potions, apples), keys, orbs, maps | T, S | todo |

## 7. Character creation and editing

| Rows | Method | M11.1 state |
|---|---|---|
| The editor opened from the title, a hero edited and saved, played with (its world: Field_99) | S | partial (build/m11 editor probes: EDITOR, NEW DATA, the standing pose's grid with blocks placed, SAVE -- the game's save data written: DATA.EDT and its icon in the run's own copy; back to the title with START and its question's YES (TRIANGLE does nothing, the question starts on NO); NEW GAME, EDIT DATA ("Select a custom edit"): the edited hero shown for "Is this okay?", named, the prologue skipped with START, in play at the castle front with it (editor5; the opening dream's full life and magic, as with a preset hero). Field_99, the editor's own world: todo) |
| A preset hero chosen; "change characters?" on CONTINUE | S | todo |

## 8. Death, respawn and checkpoints

| Rows | Method | M11.1 state |
|---|---|---|
| Death in a field, a dungeon and a boss arena; the game over and CONTINUE; where the hero comes back | S, R | pass (build/m11 death probe: a field enemy's hit on Field_14 -> back at the castle front; the game's damage in the first temple and in its boss room -> back at the temple's entrance room (D_1_1_1); life full each time -- as RPCS3 does after a death on Field_19, PLAN 25). The priest's own game over: B-009 |
| No death and invulnerability off again: the game's own damage | T | pass (devtools_player, M11.0c) |

## 9. Saving, loading and persistence

| Rows | Method | M11.1 state |
|---|---|---|
| Flags, story steps, money and items changed, saved with the game's own SAVE, loaded by CONTINUE and by a new process | T, S | pass (devtools_persist, PLAN 17) |
| A purchase kept across a save, an exit and a relaunch (an install as a player starts it) | S | todo (installed_save/installed_continue: M9 evidence) |
| The player's saves never touched by developer mode; with it off, nothing of it active | S | pass (the install checks and devtools_install_off, PLAN 14) |

## 10. Cinematics, the ending and post-game

| Rows | Method | M11.1 state |
|---|---|---|
| The opening and title movies, their skips | S | partial (build/m11 movies2, a new player's path on a fresh save root: the title movie (silent, as its file) three times, the attract demo with its sound to its last frame (115.5 s) and again cut short by a press, the opening movie to its end (154 s; the game's own music under it: the mix has sound throughout); 0 frames late in all six; the prologue's slides skipped with START: editor5) |
| Scenes inside events (`scene N`) | T | partial (devtools_encounter: scene 1) |
| Every conversation, sign, inn and chest event the catalogue places, started with the game's own StartEvent and answered to its end (build/m11 event_sweep) | T, S | pass (events-m111b: 278 events on 80 maps in 7 chunks; 250 ended, none refused, none left running. One ended the game itself in chunk 4: an imperial guard's event (n11imperialguarda3_320), whose last step is SetMap("D_23_5", "-gtf2355") -- started out of its place in the story. Not a defect of the port: the guard is an NPC only with flag 306 at 0, flag 279 set and three orbs (ImperialGuarda3_320Initialize); from the sweep's early save he has no body, and his event's moves wrote through null objects (dropped here, a crash on the console) before the game ended. With those flags (build/m11 guard_context) the event runs to its end, sets flag 306 and takes the hero to D_23_5, no null write. Chunk 4 again without it: its 27 others run, 25 ended; two sages' events could not start behind D_20_9's own long arrival event (still running after the sweep's 40 s). In all, 275 of the 278 ended in the sweep, the guard's in its context; the two sages' events then started on a plain field (build/m11 miscchecks: n21wiseman3a6_010 and n20wiseman2b8_010, each answered to its end -- 21 presses in 27.8 s, 38 in 50.2 s): all 278 ended) |
| **The ending**: the last enemy's after-event through the tower, the ending maps, the castle, the credits | T, S | pass (devtools_ending, PLAN 20) |
| The clear save, the title after it, the second mode | S | partial (devtools_ending: the save, the cleared title movie. build/m11 postgame2 from the cleared save: its title offers NEW GAME+; chosen, the difficulty (NORMAL MODE, FROM MODE), the character list, the name, the prologue -- the game's own answer IsNewGamePlus 1; postgame3: the prologue skipped (START), into play in the opening dream at the castle front, NEW GAME+ reported 1 and a NEW GAME from the same title 0 (both the dream's full life and magic). What NEW GAME+ carries over, seen after the dream: pending) |

## 11. Audio, music, environmental effects, visual correctness

| Rows | Method | M11.1 state |
|---|---|---|
| Each map's music (the map list's BGM) on arrival; sound effects of combat, items, menus; positional sound | S | todo (M4/M5 sound labs: older evidence) |
| Water, fire, lava, weather and post-processing (depth of field, bloom, the shadow map) | S, R | partial (the RPCS3 reference scenes: 5 of 5 on the M11.1a build) |
| Visual review of every map's arrival (pictures from the sweep) | W | todo |

## 12. Frame-rate independence and performance

| Rows | Method | M11.1 state |
|---|---|---|
| 60 fps held on the routes (the frame-time figures: p99, the longest, frames over 50/100 ms) | S | todo on the M11.1 build |
| Game speed the same at 30, 60, 120 and 144 Hz presents (movement, animation, timers, audio sync) | S, O | partial (PLAN 23, at 30, 60, 144 and 240: the castle front's walk, debris and clocks the same; every enemy type's speeds the same; the guardians of temples 1-5 and the last enemy the same; the dark knight faster at 60 and above (B-012, fixed: gated to 30 Hz); animation and audio sync by eye: todo) |
| Loading times per map; hitches named (the sweep's and the routes' frame logs) | W, S | todo |

## 13. Shader and pipeline coverage

| Rows | Method | M11.1 state |
|---|---|---|
| Every map's first visit: no translation in play, pipelines met unprepared listed | W | pass (sweep-m111 over the M11.1a preparation: 166/166, none unprepared, no translation; PLAN 15; again sweep-m111b over M11.1b's preparation, the same) |
| Bosses, enemies (the rare ones included), items, effects, events and the ending: `coverage export` from each stage's runs into the preparation | T, S | partial: the enemies (M11.1a: the slime's two pipelines added) and the boss fights (PLAN 22: two more, a material group with the stencil test off, added; 246 lines); the preparation made again with them (prep-m111b: 2458 pipelines, none failed; the manifest's 246 lines, 2 newly held) and the full sweep over it (sweep-m111b: 166/166 maps, none unprepared, no translation in play, music on every map; the check PASS); the rest of the stages' coverage: todo |

## 14. The PC side: the window, the exits, QUIT GAME, the installation folder and the command line

| Rows | Method | M11.1 state |
|---|---|---|
| The installed game's window kept where the player left it (after B-010's orderly end) | S (on screen) | pass (tests/window_place_check.py on the development installation: closed as a player closes it, back at the same place and size; PLAN 33) |
| Closing the window: the exit question; a second close ends the game in order; no hang report while the question holds the game | S (on screen) | pass after B-013's fix (PLAN 33) |
| QUIT GAME on the normal and the cleared title: the question, Cancel by default, the game's orderly end on Quit | S | pass (tests/title_quit.py, both titles, again after the question's redraw; on screen: the question over the title, PLAN 31). The controller's answers by the test input; the keyboard's and the mouse's: the in-game menu's own (M10.2), not run for this question -- owner |
| QUIT GAME with the launcher: back to the launcher; in direct play: the game ends | S, O | partial (direct play: the end's "exit" in the log; with the launcher: owner) |
| The portable root: default, CUBIC_INSTALL_DIR, the pointer, relative paths, spaces and Unicode, a moved folder, two installations, a root that can't be used, the copy of an earlier installation | S | pass (tests/cli_integration.py: 14 of 14; the window's offer of an earlier installation, declined and not made again: PLAN 32) |
| The command line: --play's session options, its exit status, the game ended with CubicHeroes | S | pass (tests/cli_integration.py --with-game: 17 of 17, PLAN 32) |
| Headless --install, --update, --prepare-shaders on a throwaway installation | S | todo |

## Bugs

Severity: **S1** a crash, a hang, lost progress or damaged saves; **S2** wrong behaviour or rendering a player meets;
**S3** minor; **S4** cosmetic.

| ID | Sev | Area | Found | Reproduction | Root cause | Fix | Regression test | State |
|---|---|---|---|---|---|---|---|---|
| B-001 | S3 | Developer mode | M11.1a audit | an installed game started with DOTHEROES_DEV_CANT_DIE=1 (or life.trigger and DOTHEROES_DEV_STATE=1, ...) in its environment, developer mode off | older test switches (R1-M8) read from the environment whatever the setting | in an installed game with developer mode off they are ignored and named in the log (PLAN 14) | devtools_install_off (tests/devtools_suite.py --installed) | fixed, verified |
| B-002 | S2 | Rendering | M11.1a, Field_14 | the rare metal slime on a field (5% a spawn point; or spawned with the tools) | the native renderer had no source for a cube-map texture: every draw reading one was dropped | cube maps sampled as cube images (PLAN 15) | the spawn scenario with the slime; the sweep | fixed, verified |
| B-003 | S3 | Rendering (pipelines) | M11.1a, Field_14 | the same | the pipeline key and translation took texture kinds of units the program never reads: a cube left bound made 14 needless pipelines and translations, a 166-226 ms frame on arrival | texture kinds limited to the units read (PLAN 15) | the sweep on Field_13/14; the spawn scenario's coverage | fixed, verified |
| B-004 | S2 | Rendering (textures) | M11.1a, the slime's cube map | a linear mipmapped texture (the engine's default cube map: the metal slime's reflection) | the runtime's layout packed a linear image's lower mip levels at their own width; the RSX keeps the row pitch on every level, so lower levels and every cube face after the first were read from the wrong bytes | each level of a linear image at the pitch register's row pitch (patches/ps3recomp/0165) | the runtime's test_rsx_texture_layout; the spawn scenario's cube faces | fixed, verified |
| B-005 | S4 | Tests | M11.1a, the unit tests run in full | tests/test_player_pipeline.py | since M9.2g tools/r8_lift.sh names its observers file through a variable (OBS=${R8_OBSERVERS:-config/...}), which the test's pattern did not read: the test failed though the player's lift and the developers' agree | the test reads such a variable's default | the test itself (15 of 15 unit test files pass) | fixed, verified |
| B-006 | S4 | Rendering | M11.1b, the sweep's stricter check (sweep-m111 chunk 5) | a map's first frames, every texture new (the sweep: arriving on Field_50; again in a short run of seven maps) | a batch is submitted once a record leaves it past half its 16 MB upload buffer, so a draw had only the other half for its textures, vertices and constants; on a map's first frame one draw's uploads did not fit and it was left out for that frame (`asp: the batch is full`) | the draw is given up before anything of its own is recorded, the batch submitted and the draw recorded again in the next one (PLAN 24) | build/m11's b006 runs: the same maps with batches carried nearly full (DOTHEROES_PPFX_SUBMIT_KB=15800): 9 draws recorded again, none left out, the pictures as with the usual batches (1 recorded again); the sweep's check now fails on it; the full sweep on the fix (sweep-m111b): one draw recorded again in the next batch (chunk 5), none left out, the check PASS | fixed |
| B-007 | S4 | Rendering | M11.1b, the sweep's stricter check (sweep-m111 chunk 4) | arriving at D_8_2 (the castle battle's arena), from any map (D_80_4_9, Field_91): one draw, 0.2 s after the arrival | the arena's arrival runs an effect that alternates between two 1280x720 surfaces (0:03190000 and 0:03550000 -- not the display buffers), each frame reading the other; drawn natively only for those ~60 frames. On its first frame the surface it reads has never been drawn: no native image, and the recorder keeps only small textures' bytes, so that one draw is left out (on the console it reads whatever the memory held). Not a transfer (none logged) | none: the input is undefined on the console as well; one frame | the sweep's check lists it by this number (any address); any new reason fails | root cause known; accepted (no change) |
| B-008 | S2 | Rendering | M11.1b, the RPCS3 comparison (PLAN 25) | the rare metal slime anywhere (forced: the tools' `spawn`, or a field's own spawn points with the scripts' random forced) | ours drew it as a flat dark-navy shape, RPCS3 as a pale pink-white dome with yellow glints. Not the cube map (its six faces decoded right: pastel blue, green, cyan, pink, magenta, yellow at every level), not the reflection's strength (the program's constant patched to 1), not the view (a cube view). The translator declares a program's textures either as one array of sixteen 2D images at binding 16 or -- when a unit it samples is a cube map -- each unit on its own, unit u at binding 16 + u; the native renderer's descriptor layout had only the array, so such a program's units 1-15 (the slime's cube map, its normal and shadow maps) read nothing (the validation layer: "viewType must match the Dim operand", "descriptor set in layout must match the shader") | the layout holds both: binding 16 the array, bindings 17-31 one image each, written for every draw (a cube view never in the array); the pipelines' version raised to 4 (PLAN 26). Every program that samples a cube map is covered -- the slime's two are the only ones the preparation translates | devtools_cubemap (tests/devtools_suite.py): the slime drawn under the Vulkan validation layer, no error; its picture now as RPCS3's (build/compare b008-before-after-rpcs3) | fixed, verified |
| B-009 | S1 (as reported) | Game flow | M11.1b, the priest's second fight (PLAN 22) | the priest's offer accepted twice (the game's own game over), "Continue?" answered -- and, on the tower's seventh floor, the system menu's second entry -- after the tools put the hero in the tower | the game ends itself: its GameClass.cpp check "NextIndex==MAP::MS_NON" (line 1762) finds no map to bring the hero back to and calls exit(0) (the call chain from the runtime's new PS3_HLE_ARGS back-chain walk: the game's update -> 0x566600 -> 0x5154A0 -> 0x50B410 -> exit). That map is the dungeon's resurrection point, which each dungeon's first floor sets with its own map event (the tower's: mapevent_d07_01 on D_7_1_1, Event:SetDungeonResurrectionPoint); the tools had warped past it. A player always enters there -- so far the game's own behaviour on an incomplete state, not a defect of the port | the tests enter the tower at its first floor first (the tools' warp runs that map's own event) | tests/devtools_priest.py (scenario devtools_priest): accept and continue -- back at the tower's entrance, flags, items and money as before; accept and decline; refuse -- the fight begins; the game never ends itself (PLAN 27) | not a defect of the port (the tools' warp); the regression passes (accept-continue, accept-decline, refuse, bag-door); a tower save continued in RPCS3 resumes at the same entrance; the tools' item read at the title fixed; B-010 found and fixed |
| B-010 | S1 | Stability | M11.1b, B-009's regression (the priest's game over declined, at the title: the game ended by the tools' item read, PLAN 27) | any end the game makes itself -- its sys_process_exit (quitting from the title, closing the window, its own fatal checks), the host quit's fallback, its entry returning; 2 of 10 such ends in the M11.1b runs | the process crashed while ending: an access violation in the heap (ntdll, the native renderer's thread allocating in guest_texture_live) and STATUS_HEAP_CORRUPTION (0xC0000374) -- the runtime's sys_process_exit called exit(), whose handlers and C++ destructors ran while the native renderer, the movie's decoder and the game's other threads still used what they tore down. A test's stop never met it: it drains the renderer and ends the process without them | the runtime's ends go through a host hook (patches/ps3recomp/0167: ps3_exit, g_ps3_exit_hook); the port's (main.cpp game_end) writes the window's place, the census and the game log's summary, then the test stop's own sequence (dev_state.cpp dotheroes_end: the renderer drained and stopped, the logs and reports) and ends the process with the game's status, without exit()'s handlers and destructors | the priest's decline (tests/devtools_priest.py accept-decline: the game's own end at its title), the tower's save before its first floor (the game's own fatal check): each ends with its status, no crash | fixed, verified (the game's own ends at the title and at the tower's fatal check: status 0, no crash; the test stop unchanged) |
| B-011 | S1 | Stability | M11.1b, the priest's second fight fought by the game's mechanics (build/m11 priest_aim) | the priest's fight, refused, 13 min in (47,860 frames): his warps, summons and fireballs, the hero swinging | the game stopped flipping (no guest flip for 10 s; the watchdog ended the run); nothing unusual logged before it. The Windows hang report had no thread stacks (they were POSIX-only): it now walks every thread's stack (port/diag/diag.cpp, the profiler's walk) for the next one | -- | run again with the stacks on: 25 min unlit twice (priest_aim2, priestaimlong) and the lit fights (PLAN 30, up to 7 min) without a hang; again 27 min twice (build/m11 priestaimlong1, priestaimlong2, 2026-11-17/18: no hang, no report) | open: not reproduced (once in about 130 min of that arena) |
| B-012 | S2 | Gameplay (frame rate) | M11.1b, frame-rate independence (PLAN 23; the actor log reading the game's own list) | temple 6's guardian (the dark knight) fought at 30, 60, 144 and 240 fps, the hero standing at the door: his parts' movement logged (build/m11 fps_knight: his arena alone, 60 s; 30 and 240 twice) | his main part travels 3.5 units a second at 30 fps (3.55, 3.44), 4.6 at 60, 5.8 at 144 and 240 (5.75, 5.87, 5.79), his fastest steps 14-17 at 30 against 28-29 at 144 and 240; his two other parts the same way -- more frames, more movement, levelling off near 144: the shape of motion made per frame (a step or an approach toward a target each update), as the first temple's guardian had (R8 9.6) | his body's update (0x002C8300, vtable 0x00FE52C0 slot 8) gated to 30 Hz as the first temple's guardian's (an entry hook, config/r8_hooks.json; port/game_patches.cpp dotheroes_tick30_knight_body; the lift build/r8/gen20); his parts follow it each frame (PLAN 23) | tests/scripts/fps_knight.drive at 30 and 240 with tests/fps_actors.py: his main part 3.5-4.0 units a second at 30, 60 and 240 (was 5.8 at 240), his fastest steps 15-16 at every rate; his fight and his tower rematch won on the gated build (devtools_bosses temple6, tower6 PASS; temple1 PASS) | fixed |
| B-013 | S3 | Diagnostics | M11.1, the window's place checked again (PLAN 33) | the game's window closed: the in-game menu's exit question up for more than 10 s | the question holds the game (its clocks stopped, no flips); the hang watchdog counted that as a hang and wrote a hang report after 10 s -- with DIAG_HANG_EXIT=1 (the tests) it would have ended the game | the watchdog counts no time while the host holds the game (port/diag/diag.cpp, ps3_guest_paused) | tests/window_place_check.py fails on any crash or hang report written during it | fixed, verified (the window check PASS, no report) |

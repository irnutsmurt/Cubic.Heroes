# Play session checklist (for the owner)

## R7's one request (2026-11-06, optional, about 10-15 minutes)

**The post-boss warp on the R7 build.** R7's release build has no PS3
graphics command path at all; the one thing no automated route reaches is
the floor warp in the first dungeon's portal room after its boss, which
captures the last displayed frame with a 2D transfer that R7 now does as a
native copy (docs/NATIVE_R7.md). From the repository folder, in Git Bash:

```bash
env SCENARIO=owner_warp_r7 RUN_TAG=owner-warp-r7 build/venv/Scripts/python.exe tests/boot_smoke.py
```

It starts from a copy of the boss-door save (CONTINUE: the dungeon's door;
the special key is in the room north of the north room). Beat the boss, step
on the floor warp in the portal room, watch the warp, then close the window.
Normal damage, sound, fullscreen. The report, the frame the warp captured
(xfer_dump/) and a grab a second are saved in the run's folder under
build/sessions/; your own saves are not touched. Any test of mine on screen
is stopped first and resumes afterwards. If the warp looks wrong, press F8
at that moment.

## What is wanted when you have time (optional coverage)

None of these blocks the work: R5 goes on with the saves and recordings
already here. Gathered in one place for whenever you want to play:

1. **The onward session** (below, "Playing onward on R4's build"): past the
   first dungeon on R4's build. What it adds: **a magic attack** once the
   hero has one, and **the next regions and their dungeons** (the desert,
   the lava dungeon, the volcano, the forest), which no save reaches yet.
   Your saves there become test checkpoints.
2. **The post-boss warp** (only if it comes up on its own): the floor
   warp in the first dungeon's portal room after its boss, which takes the
   hero back to the entrance. In both R4 boss sessions one post-processing
   draw there read the last displayed frame, which the native renderer had
   no copy of (docs/NATIVE_R4.md section 11). A save in area 37 right after
   the boss, before stepping on it, would let a test repeat it.
3. **Anything that looks or feels wrong**: F8 (or both sticks) at that
   moment.

The sections below are the sessions so far, kept as they were asked for:
the first one (saves at the first dungeon and its boss, now done), the boss
sessions, and the onward session.

## Start

From the repository folder, in Git Bash:

```bash
env SCENARIO=owner_play RUN_TAG=owner-play build/venv/Scripts/python.exe tests/boot_smoke.py
```

or in PowerShell:

```powershell
$env:SCENARIO='owner_play'; $env:RUN_TAG='owner-play'; build\venv\Scripts\python.exe tests\boot_smoke.py
```

If one of my test runs is on screen, it is asked to stop first (a few
seconds), and no further test starts until testing is resumed
(`tests/stop_tests.py --resume`). The game opens in a window with sound. Close the window when you are
done: that ends the recording.

## What it uses

- **Your save, copied**: `build/play-route/gamedata` holds a copy of your
  current save (in front of the castle). Progress you make there stays there
  for the next session; your own saves are not touched.
- **The validated 60 fps settings** (native collision and solver, the capped
  lap, one present per flip), as in every timing.
- **Normal damage.** No can't-die: deaths, knockback and the full-life sword
  beam stay as the game has them. If you die, choose Continue (YES) and carry
  on.
- The **frame-rate readout** with its graph, top right.
- **Fullscreen at your desktop's resolution** (1920x1080 here). The game
  itself draws 1280x720 -- the PS3 game's own resolution -- and that picture
  is scaled up to fill the screen; rendering at 1080p would be a renderer
  feature of its own (not done). For the 1280x720 window instead, put
  `PS3RECOMP_VK_FULLSCREEN=0` in front of the command (Git Bash:
  `env PS3RECOMP_VK_FULLSCREEN=0 SCENARIO=owner_play ...`). Checked
  2026-11-02: the picture and the readout at 1920x1080, the game in play,
  F8 marks, Alt-Tab away (the game keeps running) and back (input again);
  the controller's buttons in fullscreen are for you to confirm.

## Controls

- Your controller as usual. The keyboard (arrows, Z = cross, X = circle,
  A = square, S = triangle, Q / W = L1 / R1, Enter = START, Tab = SELECT) is
  read only while no controller is connected.
- **Mark a hitch: F8, or click both sticks (L3+R3).** "MARK n" shows on
  screen. Mark any stutter you notice, even a small one, right when you feel
  it -- the frame trace from a few seconds before is kept.
- F7: the readout off / text / text and graph.

## Checklist

1. **CONTINUE** from the title (it loads the newest save).
2. Head for the **first dungeon** (wherever the game sends you next). On the
   way, fight normally; if a spot has lots of grass, rabbits or debris, a
   minute of fighting there is useful.
3. At the **dungeon's entrance** (just outside): **save** (START -> SAVE).
4. Inside, **save about halfway** if the game allows it there, and again
   **right before the boss**.
5. **Fight the boss** normally (as long as it takes; dying is fine).
6. **Save after the boss.**
7. Mark hitches whenever you notice them. Stop whenever you like -- even only
   reaching the dungeon's entrance helps.

Each save is copied automatically, the moment the game writes it, into
`build/checkpoints/owner-play/<n>-area<area>/` -- the game writes every save
over the slot it loaded, so without the copy only the last one would survive.

## The boss session (2026-11-03)

```bash
env SCENARIO=owner_boss RUN_TAG=owner-boss build/venv/Scripts/python.exe tests/boot_smoke.py
```

The same game and recording as above, on a save root of its own,
`build/play-boss/gamedata`, seeded with a copy of checkpoint dungeon1-boss-door
(a test's save before the boss's portal). CONTINUE starts at the Grass
Temple's entrance (area 37 at (88.8, -130.4), life 10) with the doors the test
opened still open: the boomerang in the circle slot, the east room's door, the
north-east rooms, the six-pillar room's west door. The boss's door says "You
need a special key." without it. Your regular playthrough
(`build/play-route/gamedata`) is not touched. Each save is copied into
`build/checkpoints/owner-boss/`; its shader cache is `build/play-boss/cache`.
It runs at the display's pace (`DOTHEROES_FPS=display`: 72 fps on this 144 Hz
screen, every frame held for 2 refreshes, against 60 fps's mix of 2 and 3).
Tell me if motion looks better, worse or the same as before -- or if anything
plays oddly at that rate (timers, animations, sound, cutscenes).

## Playing onward on R4's build

Past the first boss, on R4's development build, with your progress kept
from one session to the next:

```bash
env SCENARIO=owner_onward_r4 build/venv/Scripts/python.exe tests/boot_smoke.py
```

CONTINUE starts on the field outside the first dungeon, where you saved
after beating its boss on R4's build (a copy, kept in
`build/play-onward/gamedata`). Play on as you like: the next regions and
their dungeons, and magic once the hero has it, are what no automated
route reaches yet. Save as you normally would: each save is also kept as a test
checkpoint in `build/checkpoints/owner-onward-r4/`. Press F8 (or both
sticks) at anything that looks or feels wrong. Close the window when done.

## The R4 boss session (played twice, 2026-11-05)

Played twice (docs/NATIVE_R4.md section 6), the second time smooth. The hero has no magic yet, so a
magic attack waits for the onward session above.

The same boss, on R4's development build (docs/NATIVE_R4.md): PPFX's
post-processing rendered by the native renderer, in 5-second blocks
alternating with the old path, which compares the two. Start it like the
others:

```bash
env SCENARIO=owner_boss_r4 build/venv/Scripts/python.exe tests/boot_smoke.py
```

It starts at the boss's door (a fresh copy of `dungeon1-boss-door`), with
normal damage, sound and fullscreen. Play the boss, and use a magic attack
or two if you have one. If something looks wrong -- a flicker every 5
seconds, an effect missing or different -- press F8 (or both sticks) right
then: the mark goes into the log with the time. Close the window when done.
The comparison's results go to `ppfx_report.txt` in
`build/sessions/boot-<time>-.../`, your saves to
`build/checkpoints/owner-boss-r4/`. A save in an area past the first
dungeon (the desert, the lava dungeon, the volcano, the forest) would let
the same checks run there.

## What is recorded (automatically)

In `build/sessions/boot-<time>-owner-play/`: every frame's time
(`frames.log`), your inputs with the area and the hero's position every
second and your marks (`record.log`), the frame trace around each mark and
each slow frame the game notices (`hitch/`), memory every 30 s, the log.
No screenshots or video. From it the tests get: checkpoints (your saves), a
route with position and area checks (`tests/route_from_record.py`), and the
hitches with their causes.

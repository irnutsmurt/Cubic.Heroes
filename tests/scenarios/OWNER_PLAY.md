# Play session checklist (for the owner)

What helps most now: **saves at the first dungeon and its boss**, and the
route there. Every test so far starts at the castle; scripted inputs alone
are slow to find the way across the overworld, but one session of yours gives
checkpoints and a route the tests can then repeat on their own.

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

## What is recorded (automatically)

In `build/sessions/boot-<time>-owner-play/`: every frame's time
(`frames.log`), your inputs with the area and the hero's position every
second and your marks (`record.log`), the frame trace around each mark and
each slow frame the game notices (`hitch/`), memory every 30 s, the log.
No screenshots or video. From it the tests get: checkpoints (your saves), a
route with position and area checks (`tests/route_from_record.py`), and the
hitches with their causes.

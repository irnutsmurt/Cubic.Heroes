# Test scenarios

Named, repeatable tests for `tests/boot_smoke.py` (`SCENARIO=<name>`). Each
file is a drive script (tests/drive.py) whose header says what it needs:

- `#! saves build/checkpoints/<name>` -- the kept checkpoint it starts from:
  save folders copied from the owner's saves or from a play session
  (build/checkpoints/, local only, never cleaned up by runs; each has a
  checkpoint.json saying where it came from and where CONTINUE puts the hero);
- `#! env NAME=value ...` -- the settings it needs (the environment wins);
- `#! secs <n>` -- how long the run may take at most (it ends with the drive).

Its comment block says how it reaches the encounter, what counts as a start
and as completion, the activity it must see (so an empty fight cannot pass)
and how it resets and retries. Segments it measures go to
`<run>/segments.jsonl`, failures to `<run>/failures.jsonl`;
`tests/batch_report.py <runs>` reports a batch; `tests/run_batch.py` runs one
and reports it. `common.drive` holds the blocks scenarios share (`include
common.drive`): CONTINUE, the system menu's TITLE and CONTINUE, the way back
after a death or from a menu. Checkpoints from the owner's first play session:
before-dungeon1, after-dungeon1 (both at the castle), field-area14. The first
dungeon's route blocks are in `dungeon1_route.drive`, shared by the two
dungeon scenarios. `tests/scripts/continue_where.drive` (`DRIVE=`, `SAVES=`)
says where CONTINUE puts the hero for any save: the game's own restart
point, not where the save was made.

| Scenario | Starts from | What it measures |
|---|---|---|
| `river_fight` | castle-front (in front of the castle, area 17) | the route east, confirmed in the river's area (18) at the arrival point; `ROUNDS` rounds of swings (default 20) with debris physics and enemies (class FE4500) losing life; back to the title and CONTINUE; `FIGHTS` cycles (default 1), three tries each |
| `dungeon1_approach` | before-dungeon1 (the owner's save at the castle before the session's dungeon trip) | the owner's route as positions: area 17 west to 16, west to 15, north through the dungeon's door into 37, each area change confirmed; the owner's first minute inside 37 (soft waypoints); back to the title and CONTINUE; `RUNS` cycles, two tries each |
| `dungeon1_save_continue` | before-dungeon1 | dungeon1_approach's route into the dungeon and past its first room's switch (`dungeon1_route.drive`), then the system menu's SAVE (into the run's own copy), TITLE and CONTINUE: the `state` after the menu segment is where the save restores the hero -- whether a save inside the dungeon makes a nearer checkpoint |
| `attract` | a fresh root (no saves) | the attract mode's demos, no input, for `ATTRACT_SECS` (default 44 min): rendering and stability coverage, not gameplay; hitch captures of frames of 40 ms+ |
| `owner_play` | build/play-route/gamedata (a copy of the owner's save, played in place) | a play session: the validated configuration, sound, the readout; frames, input/area/position record, hitch captures around marks and slow frames, memory; a copy of every save into build/checkpoints/owner-play/ |

To add one (the first dungeon, its boss): a checkpoint from a play session's
saves, a route from its record (`tests/route_from_record.py <run>` gives
`goto` waypoints and area checks to start from), a start check (area,
position, enemies near: `actors c<class>>=N`), the activity the segment must
show at its end (`endseg steps>+20,hp<class><-5`), and the way back to the
checkpoint for retries.

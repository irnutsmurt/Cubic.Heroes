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
`tests/batch_report.py <runs>` reports a batch.

| Scenario | Starts from | What it measures |
|---|---|---|
| `river_fight` | castle-front (in front of the castle, area 17) | the route east, confirmed in the river's area (18) at the arrival point; `ROUNDS` rounds of swings (default 20) with debris physics and enemies (class FE4500) losing life; back to the title and CONTINUE; `FIGHTS` cycles (default 1), three tries each |
| `owner_play` | build/play-route/gamedata (a copy of the owner's save, played in place) | a play session: the validated configuration, sound, the readout; frames, input/area/position record, hitch captures around marks and slow frames, memory; a copy of every save into build/checkpoints/owner-play/ |

To add one (the first dungeon, its boss): a checkpoint from a play session's
saves, a route from its record (`tests/route_from_record.py <run>` gives
`goto` waypoints and area checks to start from), a start check (area,
position, enemies near: `actors c<class>>=N`), the activity the segment must
show at its end (`endseg steps>+20,hp<class><-5`), and the way back to the
checkpoint for retries.

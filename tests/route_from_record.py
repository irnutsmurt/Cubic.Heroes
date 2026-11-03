#!/usr/bin/env python3
"""A drive route from a play session's record (DOTHEROES_DEV_RECORD=1:
<run>/record.log), with state checks instead of replayed timings.

  tests/route_from_record.py <run dir or record.log> [--from <s>] [--to <s>] [--eps 2.0]

The session's positions (one a second, the hero's in each field area's own
coordinates) between --from and --to (seconds on the record's clock) become,
per field area, a path thinned to the turns that matter (points within --eps
units of the straight line between their neighbours are dropped), written as
`goto <x> <z> 1.5 20` steps; each change of area as `cross <DIR> <area>` (on
toward that edge until the area changes) -- so a replay fails at the stage where it
goes wrong, with where the hero stopped, instead of drifting on. Marks
(the tester's F8 / L3+R3) and life changes are comments at their place;
with --buttons the tester's action buttons (CROSS, CIRCLE, ...: dialogue
pages, switches, the sword) are pressed where the tester was then (between the record's one-a-second
positions), turned first to the way they were facing.
--tol, --timeout and --soft set the gotos' (dungeons want --tol 0.25). Replaying
the inputs themselves is not attempted: enemies, knockback and loading
differ from run to run; positions and areas are the route.

The output is a block for a scenario (tests/scenarios/*.drive): check it by
running it, and expect to fix spots where the session's path went through a
door, a dialogue or a fight.
"""
import argparse
import os
import sys


# Buttons replayed where the tester pressed them (not the directions, which the
# waypoints are, nor START/SELECT or L3/R3: menus and marks).
ACTIONS = {0x4000: "CROSS", 0x2000: "CIRCLE", 0x8000: "SQUARE", 0x1000: "TRIANGLE", 0x0400: "L1", 0x0800: "R1",
           0x0100: "L2", 0x0200: "R2"}


# The digital pad's directions in the record's mask (with the analog stick
# after it: x, y 0-255, y 0 up).
DPAD = {0x0010: "UP", 0x0020: "RIGHT", 0x0040: "DOWN", 0x0080: "LEFT"}


def presses(path):
    """(t, button, facing) for each action button going down; facing is the
    direction the tester held then, or last held within 1.5 s (the stick past
    a third of its travel, or the pad), else None."""
    out, prev, held, held_t = [], 0, None, -1e9
    for ln in open(path, encoding="utf-8", errors="replace"):
        f = ln.split()
        if len(f) >= 3 and f[1] == "in":
            t, m = float(f[0]), int(f[2], 16)
            d = next((n for bit, n in DPAD.items() if m & bit), None)
            if d is None and len(f) >= 5:
                dx, dy = int(f[3]) - 128, int(f[4]) - 127
                if max(abs(dx), abs(dy)) > 42:
                    d = ("RIGHT" if dx > 0 else "LEFT") if abs(dx) >= abs(dy) else ("DOWN" if dy > 0 else "UP")
            if d:
                held, held_t = d, t
            for bit, name in ACTIONS.items():
                if m & bit and not prev & bit:
                    out.append((t, name, held if t - held_t <= 1.5 else None))
            prev = m
    return out


def where(run, t):
    """The hero's (x, z) at t, between the record's one-a-second positions."""
    for (t0, _, x0, z0), (t1, _, x1, z1) in zip(run, run[1:]):
        if t0 <= t <= t1:
            k = (t - t0) / max(t1 - t0, 1e-9)
            return x0 + k * (x1 - x0), z0 + k * (z1 - z0)
    return (run[0][2], run[0][3]) if t < run[0][0] else (run[-1][2], run[-1][3])


def parse(path):
    pos, events = [], []
    for ln in open(path, encoding="utf-8", errors="replace"):
        f = ln.split()
        if len(f) < 2:
            continue
        t, kind = float(f[0]), f[1]
        if kind == "pos" and len(f) >= 6:
            pos.append((t, int(f[2]), float(f[3]), float(f[5])))
        elif kind in ("area", "mark", "life"):
            events.append((t, kind, f[2:]))
    return pos, events


def thin(points, eps):
    """Ramer-Douglas-Peucker on (x, z)."""
    if len(points) < 3:
        return points
    (x0, z0), (x1, z1) = points[0][1:], points[-1][1:]
    dx, dz = x1 - x0, z1 - z0
    norm = (dx * dx + dz * dz) ** 0.5 or 1e-9
    worst, k = -1.0, 0
    for i, (_, x, z) in enumerate(points[1:-1], 1):
        d = abs(dz * (x - x0) - dx * (z - z0)) / norm
        if d > worst:
            worst, k = d, i
    if worst <= eps:
        return [points[0], points[-1]]
    return thin(points[:k + 1], eps)[:-1] + thin(points[k:], eps)


def direction(a, b):
    dx, dz = b[1] - a[1], b[2] - a[2]
    if abs(dx) >= abs(dz):
        return "RIGHT" if dx > 0 else "LEFT"
    return "UP" if dz > 0 else "DOWN"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("record")
    ap.add_argument("--from", dest="t0", type=float, default=0)
    ap.add_argument("--to", dest="t1", type=float, default=1e18)
    ap.add_argument("--eps", type=float, default=2.0)
    ap.add_argument("--tol", type=float, default=1.5, help="each goto's tolerance")
    ap.add_argument("--timeout", type=float, default=20)
    ap.add_argument("--soft", action="store_true", help="gotos that log, not fail, when blocked")
    ap.add_argument("--buttons", action="store_true", help="the tester's action buttons where they pressed them")
    a = ap.parse_args()
    path = os.path.join(a.record, "record.log") if os.path.isdir(a.record) else a.record
    pos, events = parse(path)
    btns = presses(path)
    pos = [p for p in pos if a.t0 <= p[0] <= a.t1 and not (p[2] == 0 and p[3] == 0)]
    # A position just after entering an area can still be the last area's: the
    # hero is placed once the load is done. Positions within 2 s of an area
    # change are dropped.
    changes = [t for t, k, _ in events if k == "area"]
    pos = [p for p in pos if not any(0 <= p[0] - t < 2.0 for t in changes)]
    if not pos:
        sys.exit("no positions in that span (was DOTHEROES_DEV_RECORD=1 on, and the hero in play?)")
    # runs of one area
    runs, cur = [], [pos[0]]
    for p in pos[1:]:
        if p[1] != cur[-1][1]:
            runs.append(cur)
            cur = [p]
        else:
            cur.append(p)
    runs.append(cur)
    out = [f"# route from {path}, {pos[0][0]:.0f}-{pos[-1][0]:.0f} s: {len(runs)} area stretch(es)",
           f"state area=={runs[0][0][1]},life>0 60"]
    for i, run in enumerate(runs):
        pts = thin([(t, x, z) for t, _, x, z in run], a.eps)
        out.append(f"# area {run[0][1]}: {run[0][0]:.0f}-{run[-1][0]:.0f} s, {len(run)} positions, {len(pts)} waypoints")
        prev_t = run[0][0]
        at = (pts[0][1], pts[0][2])
        for t, x, z in pts:
            if a.buttons:   # pressed on the way here: where and facing as the tester was, as far apart
                last, face = prev_t, None   # the first after as long as the tester waited (a dialogue opening)
                for tp, b, d in [e for e in btns if prev_t < e[0] <= t]:
                    px, pz = where(run, tp)
                    if ((px - at[0]) ** 2 + (pz - at[1]) ** 2) ** 0.5 > 0.5:
                        out.append(f"goto {px:.1f} {pz:.1f} 0.3 6 soft   # where it was pressed")
                        at, last = (px, pz), tp
                    if d and d != face:
                        out.append(f"hold {d} 0.12" + " " * (17 - len(d)) + f"# face {d.lower()}")   # a tap does not turn the hero
                        face = d
                    if tp - last > 0.05:
                        out.append(f"sleep {min(tp - last, 1.5):.2f}")
                    out.append(f"press {b}   # {tp:.1f} s")
                    last = tp
            prev_t = t
            at = (x, z)
            notes = [f"{k} {' '.join(v)}" for te, k, v in events if k in ("mark", "life") and t - 1 < te <= t]
            out.append(f"goto {x:.1f} {z:.1f} {a.tol:g} {a.timeout:g}{' soft' if a.soft else ''}" +
                       (f"   # {', '.join(notes)}" if notes else ""))
        if i + 1 < len(runs):
            last = pts[-2:] if len(pts) >= 2 else pts * 2
            nxt = runs[i + 1][0]
            jx, jz = nxt[2] - last[1][1], nxt[3] - last[1][2]
            # Across a field edge the hero comes in on the far side of the next
            # area: a big jump in x or z, the other kept, says which edge (in at
            # high x: out to the west). Through a door (a cave, a house) both
            # change -- the inside has coordinates of its own -- or neither: the
            # way it walked (a door read as an edge sent the hero away from it).
            if abs(jx) > 40 and abs(jz) < 10:
                d = "LEFT" if jx > 0 else "RIGHT"
            elif abs(jz) > 40 and abs(jx) < 10:
                d = "DOWN" if jz > 0 else "UP"
            else:   # the last step it took toward the door (raw positions, not the thinned path)
                end = run[-1]
                back = next((p for p in reversed(run[:-1]) if abs(p[2] - end[2]) + abs(p[3] - end[3]) > 0.3), run[0])
                d = direction((back[0], back[2], back[3]), (end[0], end[2], end[3]))
            out.append(f"cross {d} {runs[i + 1][0][1]} 20   # on across the edge (or a door)")
    print("\n".join(out))


if __name__ == "__main__":
    main()

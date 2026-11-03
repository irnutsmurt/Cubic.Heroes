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
pages, switches, the sword) are pressed after the waypoint before them.
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


def presses(path):
    """(t, button) for each action button going down."""
    out, prev = [], 0
    for ln in open(path, encoding="utf-8", errors="replace"):
        f = ln.split()
        if len(f) >= 3 and f[1] == "in":
            m = int(f[2], 16)
            for bit, name in ACTIONS.items():
                if m & bit and not prev & bit:
                    out.append((float(f[0]), name))
            prev = m
    return out


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
        for t, x, z in pts:
            if a.buttons:   # pressed on the way here: after the last waypoint, in order, as far apart
                last = prev_t   # the first after as long as the tester waited (a dialogue opening)
                for tp, b in [(tp, b) for tp, b in btns if prev_t < tp <= t]:
                    if tp - last > 0.05:
                        out.append(f"sleep {min(tp - last, 1.5):.2f}")
                    out.append(f"press {b}   # {tp:.1f} s")
                    last = tp
            prev_t = t
            notes = [f"{k} {' '.join(v)}" for te, k, v in events if k in ("mark", "life") and t - 1 < te <= t]
            out.append(f"goto {x:.1f} {z:.1f} {a.tol:g} {a.timeout:g}{' soft' if a.soft else ''}" +
                       (f"   # {', '.join(notes)}" if notes else ""))
        if i + 1 < len(runs):
            last = pts[-2:] if len(pts) >= 2 else pts * 2
            nxt = runs[i + 1][0]
            jx, jz = nxt[2] - last[1][1], nxt[3] - last[1][2]
            # Across a field edge the hero comes in on the far side of the next
            # area: a big jump in x or z says which edge (in at high x: out to
            # the west). Through a door there is no such jump: the way it walked.
            if abs(jx) > 40 and abs(jx) >= abs(jz):
                d = "LEFT" if jx > 0 else "RIGHT"
            elif abs(jz) > 40:
                d = "DOWN" if jz > 0 else "UP"
            else:
                d = direction(last[0], last[1])
            out.append(f"cross {d} {runs[i + 1][0][1]} 20   # on across the edge (or a door)")
    print("\n".join(out))


if __name__ == "__main__":
    main()

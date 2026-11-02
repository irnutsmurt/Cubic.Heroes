#!/usr/bin/env python3
"""A drive route from a play session's record (DOTHEROES_DEV_RECORD=1:
<run>/record.log), with state checks instead of replayed timings.

  tests/route_from_record.py <run dir or record.log> [--from <s>] [--to <s>] [--eps 2.0]

The session's positions (one a second, the hero's in each field area's own
coordinates) between --from and --to (seconds on the record's clock) become,
per field area, a path thinned to the turns that matter (points within --eps
units of the straight line between their neighbours are dropped), written as
`goto <x> <z> 1.5 20` steps; each change of area as a walk on toward that edge
and `state area==<n>,life>0 30` -- so a replay fails at the stage where it
goes wrong, with where the hero stopped, instead of drifting on. Marks
(the tester's F8 / L3+R3) and saves are comments at their place. Replaying
the inputs themselves is not attempted: enemies, knockback and loading
differ from run to run; positions and areas are the route.

The output is a block for a scenario (tests/scenarios/*.drive): check it by
running it, and expect to fix spots where the session's path went through a
door, a dialogue or a fight.
"""
import argparse
import os
import sys


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
    a = ap.parse_args()
    path = os.path.join(a.record, "record.log") if os.path.isdir(a.record) else a.record
    pos, events = parse(path)
    pos = [p for p in pos if a.t0 <= p[0] <= a.t1 and not (p[2] == 0 and p[3] == 0)]
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
        for t, x, z in pts:
            notes = [f"{k} {' '.join(v)}" for te, k, v in events if k in ("mark", "life") and t - 1 < te <= t]
            out.append(f"goto {x:.1f} {z:.1f} 1.5 20" + (f"   # {', '.join(notes)}" if notes else ""))
        if i + 1 < len(runs):
            last = pts[-2:] if len(pts) >= 2 else pts * 2
            out.append(f"hold {direction(last[0], last[1])} 2   # on across the edge")
            out.append(f"state area=={runs[i + 1][0][1]},life>0 30")
    print("\n".join(out))


if __name__ == "__main__":
    main()

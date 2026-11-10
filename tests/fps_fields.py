#!/usr/bin/env python3
"""Which logged fields of a class follow the frame rate (DOTHEROES_DEV_FIELD_LOG; tests/boss_timing.py logs them too).

  tests/fps_fields.py <30 fps run> <other run> [<other run> ...] [--class FE4500,FE4760]

Per class, per logged offset: the changes per second of the field while the objects are alive, averaged over the
objects seen for at least 5 s, in each run, with the ratio to the first run; `F` marks a field above 1.5 times the first
run's rate in every other run (it follows the frame rate, or the run differs). An integer that counts up gives its rate.
Then the speed of the position (+0x428, +0x430) in 0.25 s windows.

The fields' rates are compared, so runs of the same scene with the same input are what it is for: a fight that goes
differently has different rates for reasons that have nothing to do with the frame rate; look at more than one round.
"""
import collections
import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import boss_timing as bt  # noqa: E402


def summarize(run, classes):
    off, objs = bt.load(run)
    ix = {o: i for i, o in enumerate(off)}
    per = collections.defaultdict(dict)
    speeds = collections.defaultdict(list)
    n_obj = collections.Counter()
    for (vt, ea), recs in objs.items():
        if classes and vt not in classes:
            continue
        span = recs[-1][0] - recs[0][0]
        if span < 5:
            continue
        n_obj[vt] += 1
        for o in off:
            i = ix[o]
            vals = [r[1][i] for r in recs]
            ch = sum(1 for a, b in zip(vals, vals[1:]) if a != b)
            per[(vt, o)].setdefault("rates", []).append(ch / span)
            if all(v < 0x100000 for v in vals):
                d = [b - a for a, b in zip(vals, vals[1:]) if b != a]
                if d and all(0 < x < 0x80000 for x in d):
                    per[(vt, o)].setdefault("count", []).append((vals[-1] - vals[0]) / span)
        if 0x428 in ix and 0x430 in ix:
            X, Z = ix[0x428], ix[0x430]
            bins = collections.defaultdict(float)
            for (ta, wa), (tb, wb) in zip(recs, recs[1:]):
                d = ((bt.f(wb[X]) - bt.f(wa[X])) ** 2 + (bt.f(wb[Z]) - bt.f(wa[Z])) ** 2) ** 0.5
                if d < 3:
                    bins[int((tb - recs[0][0]) / 0.25)] += d
            speeds[vt].extend(v / 0.25 for v in bins.values() if v > 0.02)
    return off, per, speeds, n_obj


def main():
    argv = sys.argv[1:]
    classes = None
    if "--class" in argv:
        i = argv.index("--class")
        classes = {int(x, 16) for x in argv[i + 1].split(",")}
        del argv[i:i + 2]
    runs = argv
    data = [summarize(r, classes) for r in runs]
    allv = sorted({k[0] for d in data for k in d[1]})
    names = [os.path.basename(r.rstrip("/\\")) for r in runs]
    print("runs:", ", ".join(names))
    for vt in allv:
        print("class %08X: objects seen 5 s or more: %s" % (vt, ", ".join(str(d[3][vt]) for d in data)))
        sp = []
        for d in data:
            s = sorted(d[2].get(vt, []))
            sp.append("p50 %.2f p90 %.2f (%d)" % (s[len(s) // 2], s[len(s) * 9 // 10], len(s)) if s else "-")
        print("  speed in 0.25 s windows, units/s: " + " | ".join(sp))
        for o in data[0][0]:
            row = []
            base = None
            flag = True
            for k, d in enumerate(data):
                e = d[1].get((vt, o))
                if not e:
                    row.append("-")
                    flag = False
                    continue
                r = statistics.mean(e["rates"])
                c = statistics.mean(e["count"]) if e.get("count") else None
                if k == 0:
                    base = r
                elif not base or r < 1.5 * base:
                    flag = False
                row.append(("%.2f/s" % r) + (" (up %.1f/s)" % c if c is not None else ""))
            if any(x != "-" and not x.startswith("0.00/s") for x in row):
                print("  +%03X %s %s" % (o, "F" if flag and len(runs) > 1 and base and base > 0.1 else " ", " | ".join(row)))


if __name__ == "__main__":
    main()

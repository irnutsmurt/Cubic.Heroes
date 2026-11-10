#!/usr/bin/env python3
"""How a boss's objects run in real seconds, from fields.bin (DOTHEROES_DEV_FIELD_LOG, port/native/dev_state.cpp).

  tests/boss_timing.py <run dir> [--class FE4760]

Per object of the class, over the whole log: for each logged word, how often it changes per real second and, for a
word that only counts up, its rate; speed of the position (+0x428, +0x430) while moving; the spacing of changes of the
heading (+0x4A8). A rate that follows the frame rate is frame-counted; one that stays put from run to run is timed.
"""
import collections
import os
import statistics
import struct
import sys


def load(run):
    off = [int(x, 16) for x in open(os.path.join(run, "fields.txt")).read().split()]
    rec = struct.Struct("<QII" + "I" * len(off))
    data = open(os.path.join(run, "fields.bin"), "rb").read()
    objs = collections.defaultdict(list)
    for i in range(0, len(data) - rec.size + 1, rec.size):
        t, ea, vt, *w = rec.unpack_from(data, i)
        objs[(vt, ea)].append((t / 1e9, w))
    return off, objs


def f(u):
    return struct.unpack(">f", struct.pack(">I", u))[0]


def main():
    run = sys.argv[1]
    cls = int(sys.argv[sys.argv.index("--class") + 1], 16) if "--class" in sys.argv else 0xFE4760
    off, objs = load(run)
    ix = {o: i for i, o in enumerate(off)}
    mine = {k: v for k, v in objs.items() if k[0] == cls}
    print(run, "class %X: %d objects, %s records" % (cls, len(mine), sum(len(v) for v in mine.values())))
    for (vt, ea), recs in sorted(mine.items(), key=lambda kv: -len(kv[1]))[:8]:
        t0, t1 = recs[0][0], recs[-1][0]
        span = t1 - t0
        if span < 1:
            continue
        print(" object %08X: %.1f s, %d records (%.1f a second)" % (ea, span, len(recs), len(recs) / span))
        for o in off:
            i = ix[o]
            vals = [r[1][i] for r in recs]
            ch = sum(1 for a, b in zip(vals, vals[1:]) if a != b)
            if not ch:
                continue
            ints = all(v < 0x100000 for v in vals)
            if ints:
                inc = [b - a for a, b in zip(vals, vals[1:]) if b != a]
                ups = sum(1 for d in inc if 0 < d < 0x80000000)
                rate = (vals[-1] - vals[0]) / span if ups == len(inc) else None
                print("   +%03X int   changes %6.1f/s  %s  values %s" % (o, ch / span, ("counts up %.1f/s" % rate) if rate is not None else "", sorted(set(vals))[:10] if len(set(vals)) <= 10 else "%d distinct" % len(set(vals))))
            else:
                fl = [f(v) for v in vals]
                print("   +%03X float changes %6.1f/s  range %.3f .. %.3f  median |step| %.4f" % (o, ch / span, min(fl), max(fl), statistics.median(abs(b - a) for a, b in zip(fl, fl[1:]) if b != a)))
        ix_x, ix_z = ix.get(0x428), ix.get(0x430)
        if ix_x is not None and ix_z is not None:
            # speed over successive records less than 50 ms apart, while moving
            sp = []
            for (ta, wa), (tb, wb) in zip(recs, recs[1:]):
                dt = tb - ta
                d = ((f(wb[ix_x]) - f(wa[ix_x])) ** 2 + (f(wb[ix_z]) - f(wa[ix_z])) ** 2) ** 0.5
                if 0 < dt < 0.05 and 0 < d < 3:
                    sp.append(d / dt)
            if sp:
                sp.sort()
                print("   speed while moving: median %.2f units/s, p10 %.2f, p90 %.2f (%d steps)" % (sp[len(sp) // 2], sp[len(sp) // 10], sp[len(sp) * 9 // 10], len(sp)))
        if 0x4A8 in ix:
            i = ix[0x4A8]
            turns = [recs[k][0] for k in range(1, len(recs)) if recs[k][1][i] != recs[k - 1][1][i]]
            gaps = [b - a for a, b in zip(turns, turns[1:])]
            if gaps:
                print("   heading changes: %d, median gap %.3f s (p10 %.3f, p90 %.3f)" % (len(turns), statistics.median(gaps), sorted(gaps)[len(gaps) // 10], sorted(gaps)[len(gaps) * 9 // 10]))


def levels(run, head=0xFE4690, shots=0xFE4500):
    """The head's speed multiplier (+0x5C4: 1, and raised by each hit it takes) decides the boss's pace, so a
    comparison between runs is per multiplier: per level, the time at that level, the head's speed in 0.25 s windows,
    its turns' spacing, and the shots (new objects of the class `shots`) a minute."""
    off, objs = load(run)
    ix = {o: i for i, o in enumerate(off)}
    X, Z, H, S = ix[0x428], ix[0x430], ix[0x4A8], ix[0x5C4]
    heads = [v for k, v in objs.items() if k[0] == head and len(v) > 100]
    births = sorted(v[0][0] for k, v in objs.items() if k[0] == shots)
    print(run)
    for recs in heads:
        lv = collections.defaultdict(lambda: dict(t=0.0, bins=collections.defaultdict(float), turns=[], shots=0))
        last_turn = None
        for (ta, wa), (tb, wb) in zip(recs, recs[1:]):
            s = round(f(wa[S]), 2)
            e = lv[s]
            dt = tb - ta
            if dt < 0.25:   # a longer gap: nothing changed (a dialogue, a pause)
                e["t"] += dt
            d = ((f(wb[X]) - f(wa[X])) ** 2 + (f(wb[Z]) - f(wa[Z])) ** 2) ** 0.5
            if d < 3:
                e["bins"][int(tb / 0.25)] += d
            if wb[H] != wa[H]:
                if last_turn is not None:
                    e["turns"].append(tb - last_turn)
                last_turn = tb
            e["shots"] += sum(1 for b in births if ta < b <= tb)
        print("  multiplier   seconds   speed p10 p50 p90 (units/s)   turn gap p50 (s), turns   shots a minute")
        for s in sorted(lv):
            e = lv[s]
            if e["t"] < 1:
                continue
            sp = sorted(v / 0.25 for v in e["bins"].values() if v > 0.02)
            q = (lambda p: sp[int(len(sp) * p)]) if sp else (lambda p: 0.0)
            gaps = sorted(e["turns"])
            print("  %8.2f %9.1f   %6.2f %6.2f %6.2f            %s   %6.1f" % (
                s, e["t"], q(0.1), q(0.5), q(0.9),
                ("%6.3f, %3d" % (gaps[len(gaps) // 2], len(gaps))) if gaps else "     -,   0",
                e["shots"] * 60.0 / e["t"]))


if __name__ == "__main__":
    if "--levels" in sys.argv:
        for r in [a for a in sys.argv[1:] if not a.startswith("--")]:
            levels(r)
    else:
        main()

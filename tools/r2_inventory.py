"""R2: the PPFX chain structures a run showed, from its inventory.

r2_inventory.py <inventory.txt> [--programs-of <capture dir>] [--range <lo>-<hi>] [--pass1 <fp>/<rt>]

inventory.txt is what DOTHEROES_R2_INVENTORY writes (rsx_draw_engine.c):
for each distinct sequence of PPFX draws in an engine frame -- (fragment
program, colour target A, its size) per draw -- the sequence in full when
first seen, and a line whenever a frame's sequence differs from the frame
before's. Engine frames end at a present, which can fall inside a chain (a
loading or fading frame) and the final composite alternates display
buffers, so the frames' draws are joined into one stream and cut into
chains where pass 1 begins (fragment program 0x018F0000 into the scene
target), each chain keyed by its (program, target size) sequence. Prints,
per structure: how many chains and when (seconds since the process
started), its draws and distinct programs, and how it differs from the most
common structure (or, with --programs-of, from the captured chain) --
programs at the same position, extra or missing passes. --range keeps only
draws whose fragment program lies in [lo, hi) (an inventory taken over
every draw); --pass1 names the draw that begins a chain (fragment program
and colour target, hex; 018F0000/01AE0000 by default). With --programs
alone, prints the fragment programs of draws into small targets (at most
640x360) by how often each was used: where a scene keeps PPFX's programs.
Writes nothing.
"""
import json
import os
import sys

PASS1 = (0x018F0000, 0x01AE0000)


def parse(path):
    seqs, runs = {}, []
    cur = None
    for ln in open(path, errors="replace"):
        t = ln.split()
        if not t:
            continue
        if t[0] == "S":
            cur = []
            seqs[t[1]] = cur
        elif t[0] == "F":
            runs.append((int(t[1]), float(t[2]), t[3]))
            cur = None
        elif cur is not None and len(t) == 3:
            w, h = t[2].split("x")
            cur.append((int(t[0], 16), int(t[1], 16), int(w), int(h)))
    return seqs, runs


def chains(seqs, runs, lo=0, hi=1 << 32, pass1=PASS1):
    """(start seconds, [(fp, rt, w, h), ...]) for each chain of the run."""
    out, cur, t_cur = [], None, 0.0
    for k, (fr, t, sig) in enumerate(runs):
        nxt = runs[k + 1] if k + 1 < len(runs) else (fr + 1, t, sig)
        n = max(nxt[0] - fr, 1)
        for i in range(n):
            ts = t + (nxt[1] - t) * i / n
            for d in seqs.get(sig, []):
                if not lo <= d[0] < hi:
                    continue
                if (d[0], d[1]) == pass1:
                    if cur is not None:
                        out.append((t_cur, cur))
                    cur, t_cur = [], ts
                if cur is not None:
                    cur.append(d)
    if cur:
        out.append((t_cur, cur))
    return out


def key(ch):
    return tuple((d[0], d[2], d[3]) for d in ch)


def diff(a, b):
    """How chain structure a differs from b, position by position."""
    notes = []
    for i in range(max(len(a), len(b))):
        x = a[i] if i < len(a) else None
        y = b[i] if i < len(b) else None
        if x == y:
            continue
        if x is None:
            notes.append(f"pass {i + 1}: missing (reference {y[0]:08X} {y[1]}x{y[2]})")
        elif y is None:
            notes.append(f"pass {i + 1}: extra {x[0]:08X} {x[1]}x{x[2]}")
        else:
            notes.append(f"pass {i + 1}: {x[0]:08X} {x[1]}x{x[2]} (reference {y[0]:08X} {y[1]}x{y[2]})")
    return notes


def main():
    args = sys.argv[1:]
    ref = None
    if "--programs-of" in args:
        i = args.index("--programs-of")
        cap = args[i + 1]
        del args[i:i + 2]
        ch = json.load(open(os.path.join(cap, "chain.json")))
        ref = []
        for o in ch["frames"][0]["ops"]:
            if o["op"] == "draw":
                d = json.load(open(os.path.join(cap, o["dir"], "desc.json")))
                ref.append((d["provenance"]["fp_offset"], d["target"]["width"], d["target"]["height"]))
        ref = tuple(ref)
    lo, hi, pass1, survey = 0, 1 << 32, PASS1, False
    if "--range" in args:
        i = args.index("--range")
        lo, hi = (int(x, 16) for x in args[i + 1].split("-"))
        del args[i:i + 2]
    if "--pass1" in args:
        i = args.index("--pass1")
        pass1 = tuple(int(x, 16) for x in args[i + 1].split("/"))
        del args[i:i + 2]
    if "--programs" in args:
        survey = True
        args.remove("--programs")
    for path in args:
        seqs, runs = parse(path)
        if survey:
            use = {}
            for q in seqs.values():
                for d in q:
                    if d[2] * d[3] <= 640 * 360:
                        use.setdefault(d[0], set()).add(f"{d[2]}x{d[3]}")
            print(f"{path}: fragment programs drawing into targets of 640x360 or less")
            for fp, sz in sorted(use.items()):
                print(f"  {fp:08X}: {' '.join(sorted(sz))}")
            continue
        cs = chains(seqs, runs, lo, hi, pass1)
        by = {}
        for t, c in cs:
            by.setdefault(key(c), []).append(t)
        common = max(by, key=lambda k: len(by[k])) if by else ()
        base = ref if ref is not None else common
        print(f"{path}: {len(cs)} chains, {len(by)} structures; differences against "
              f"{'the captured chain' if ref is not None else 'the most common structure'}")
        allprogs = set()
        for k2, ts in sorted(by.items(), key=lambda kv: -len(kv[1])):
            progs = {d[0] for d in k2}
            allprogs |= progs
            spans, a = [], ts[0]
            for p, q in zip(ts, ts[1:] + [None]):
                if q is None or q - p > 2.0:
                    spans.append(f"{a:.0f}-{p:.0f}s" if p - a >= 1 else f"{a:.0f}s")
                    a = q
            print(f"  {len(ts)} chains ({', '.join(spans[:8])}{' ...' if len(spans) > 8 else ''}): "
                  f"{len(k2)} draws, {len(progs)} programs")
            for n in diff(k2, base)[:12]:
                print(f"      {n}")
        print(f"  programs over all chains: {len(allprogs)}: {' '.join(f'{p:08X}' for p in sorted(allprogs))}")


if __name__ == "__main__":
    main()

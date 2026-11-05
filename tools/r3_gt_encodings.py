"""R3: what each libgcm call PPFX makes writes into the command stream.

r3_gt_encodings.py <ground-truth dir> [<entry point> ...] [--n N]

Pairs a boundary log (the observed calls and their arguments;
port/native/render/r1_observe.cpp) with a frame trace of the same frames
(every command word with the libgcm function that wrote it and that
function's caller; RSX_FRAME_TRACE with PS3_ATTRIB_SHADOW, cellGcmSys.c).
Within each traced frame, the k-th call of a function from a call site is
paired with the k-th group of commands that function wrote for that call
site -- a group starting each time the first method of the function's
calls comes round again. Prints, per entry point, calls' arguments beside
the methods and values they wrote: the encoding R3's recorder models from
the arguments alone (port/native/render/ppfx_recorder.cpp).
"""
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from r1_boundary_check import load  # noqa: E402


def trace_frames(path):
    """[(fn, lr, method, data)] per traced frame, split at the flips."""
    frames = [[]]
    for ln in open(path, errors="replace"):
        t = ln.split()
        if len(t) < 6 or t[0].startswith("#") or "M" not in t:
            continue
        fn = int(t[2], 16)
        lr = next((int(x[3:], 16) for x in t if x.startswith("lr=")), 0)
        i = t.index("M")
        meth, cnt = int(t[i + 2], 16), int(t[i + 3])
        data = []
        for x in t[i + 4:i + 4 + cnt]:
            try:
                data.append(int(x, 16))
            except ValueError:
                break
        if fn == 0x0069DD60 and frames[-1] and frames[-1][-1][0] != 0x0069DD60:
            frames.append([])
        frames[-1].append((fn, lr, meth, data))
    return [f for f in frames if f]


def main():
    d = sys.argv[1]
    args = sys.argv[2:]
    n = 4
    if "--n" in args:
        i = args.index("--n"); n = int(args[i + 1]); del args[i:i + 2]
    want = {int(a, 16) for a in args}
    calls = load(os.path.join(d, "boundary.txt"))
    frames = trace_frames(os.path.join(d, "run", "frametrace.txt"))
    bframes = collections.defaultdict(list)
    for c in calls:
        bframes[c["frame"]].append(c)
    keys = sorted(bframes)

    def draws_b(f):
        return [c["lr"] for c in bframes[f] if c["addr"] == 0x006A1E28]

    def draws_t(fr):
        return [lr for fn, lr, m, dd in fr if fn == 0x006A1E28 and m == 0x1814]
    best = (-1, 0)
    for k0 in range(len(keys)):
        score = sum(1 for j, fr in enumerate(frames) if k0 + j < len(keys) and draws_t(fr)
                    and len(draws_b(keys[k0 + j])) == len(draws_t(fr)))
        if score > best[0]:
            best = (score, k0)
    k0 = best[1]
    print(f"# {len(frames)} traced frames, aligned to boundary frames from {keys[k0]} "
          f"({best[0]} with the same number of PPFX draws)")
    out = collections.defaultdict(list)
    for j, fr in enumerate(frames):
        if k0 + j >= len(keys):
            break
        firsts = {}
        groups = collections.defaultdict(list)
        for fn, lr, m, dd in fr:
            key = (fn, lr)
            g = groups[key]
            if key not in firsts:
                firsts[key] = m
            if not g or m == firsts[key]:
                g.append([])
            g[-1].append((m, dd))
        seen = collections.Counter()
        for c in bframes[keys[k0 + j]]:
            key = (c["addr"], c["lr"])
            k = seen[key]
            seen[key] += 1
            gl = groups.get(key, [])
            out[c["addr"]].append((c, gl[k] if k < len(gl) else None))
    for addr in sorted(out):
        if want and addr not in want:
            continue
        items = [x for x in out[addr] if x[1]]
        print(f"== {addr:08X}: {len(out[addr])} calls, {len(items)} with commands of their own")
        for c, cmds in items[:n]:
            regs = " ".join(f"r{i + 3}={v:X}" for i, v in enumerate(c["r"]))
            blobs = " ".join(f"B{r}={b[1][:32].hex()}" for r, b in sorted(c["blob"].items()))
            print(f"   lr {c['lr']:08X} {regs} f={c['f']} {blobs}")
            for m, dd in cmds:
                print(f"      {m:04X} " + " ".join(f"{x:08X}" for x in dd[:12]) + (" ..." if len(dd) > 12 else ""))


if __name__ == "__main__":
    main()

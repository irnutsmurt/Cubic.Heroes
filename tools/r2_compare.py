"""R2: a chain replay's every output against the existing renderer's.

r2_compare.py <capture dir> [--variant pe] [--repeated] [--png] [--prune]

For each frame of <capture dir>/chain.json (tools/r2_chain.py) that
r1_replay --chain ran, each operation's native output against its
reference, raw, at the format's own precision (tools/r1_compare.py's
measures):
  a draw   <draw dir>/native_after.bin, the target read back right after the
           native pass, before any later pass could overwrite it, against
           target_after.bin, the renderer's target copied on the GPU right
           after its draw;
  a copy   <frame dir>/native_copy_r<id>.bin, the feedback snapshot, against
           what its source held at that point of the frame by the
           references: the captured input (at the frame's start, or where
           work outside the chain re-supplied it), or the reference output
           of the draw that last wrote it.
--variant pe compares the --poison-each-frame run's outputs (*_pe.bin);
--repeated the outputs of the correctness pass after the timing
repetitions (native_after_repeated.bin, the timed frame's draws only).
Writes compare_chain[_pe|_repeated].json and prints a summary; exact
agreement is the aim, and any difference is reported per channel, not
tolerated. --png also writes, for each frame's final composite,
final_sheet.png (reference | native | difference) into the frame dir.
Every output's SHA-256 and its reference's go into the json; --prune then
deletes each native output that was bit-exact (its hash is the record).
Game data: keep the images local.
"""
import hashlib
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from r1_compare import FMT, channels, f16_ordinal, png, to8  # noqa: E402


def read(path):
    with open(path, "rb") as f:
        return f.read()


def sha(b):
    return hashlib.sha256(b).hexdigest()


def measure(ref, nat, fmt, w, h):
    name, kind, bpp = FMT[fmt]
    n = w * h
    res = {"format": name, "width": w, "height": h, "bit_exact": ref[:n * bpp] == nat[:n * bpp]}
    if res["bit_exact"]:
        return res, None
    rc, rv = channels(ref, kind, n)
    nc, nv = channels(nat, kind, n)
    chan = "RGBA" if kind != "u8bgra" else "BGRA"
    res["channels"] = {}
    lit = bytearray(n * 4)
    for i in range(n):
        lit[i * 4 + 3] = 255
    differing = set()
    for c in range(4):
        differ = 0; max_abs = 0.0; sum_abs = 0.0; max_ulp = 0; nonfinite = 0
        for i in range(n):
            k = i * 4 + c
            if rc[k] == nc[k]:
                continue
            differ += 1
            differing.add(i)
            a, b = rv[k], nv[k]
            if not (math.isfinite(a) and math.isfinite(b)):
                nonfinite += 1
                continue
            e = abs(a - b)
            sum_abs += e
            max_abs = max(max_abs, e)
            if kind == "f16":
                max_ulp = max(max_ulp, abs(f16_ordinal(rc[k]) - f16_ordinal(nc[k])))
            elif kind.startswith("u8"):
                max_ulp = max(max_ulp, abs(rc[k] - nc[k]))
        ch = {"differing_pixels": differ, "max_abs_error": max_abs, "mean_abs_error": sum_abs / n}
        ch["max_ulp" if kind == "f16" else "max_step_difference"] = max_ulp
        if nonfinite:
            ch["nonfinite_differences"] = nonfinite
        res["channels"][chan[c]] = ch
    res["differing_pixels"] = len(differing)
    poison = sum(1 for i in differing if nat[i * bpp:(i + 1) * bpp] == b"\xa5" * bpp)
    if poison:
        res["differing_pixels_still_poison"] = poison
    for i in differing:
        lit[i * 4:i * 4 + 4] = b"\xff\x00\xff\xff"
    return res, (rv, nv, kind, lit)


def sheet(path, w, h, ref, nat, fmt, lit=None):
    _, kind, _ = FMT[fmt]
    n = w * h
    _, rv = channels(ref, kind, n)
    _, nv = channels(nat, kind, n)
    if lit is None:
        lit = bytearray(n * 4)
        for i in range(n):
            lit[i * 4 + 3] = 255
    ref8, nat8 = to8(rv, kind), to8(nv, kind)
    gap = 8
    sw = 3 * w + 2 * gap
    out = bytearray(sw * h * 4)
    for y in range(h):
        row = y * sw * 4
        for k, src in enumerate((ref8, nat8, lit)):
            o = row + k * (w + gap) * 4
            out[o:o + w * 4] = src[y * w * 4:(y + 1) * w * 4]
    png(path, sw, h, out)


def main():
    cap = sys.argv[1]
    args = sys.argv[2:]
    variant = args[args.index("--variant") + 1] if "--variant" in args else ""
    repeated = "--repeated" in args
    sfx = f"_{variant}" if variant else ""
    chain = json.load(open(os.path.join(cap, "chain.json")))
    run = json.load(open(os.path.join(cap, f"replay_chain{sfx}{'_timing' if repeated else ''}.json")))
    fa, fb = run["frames"]
    out = {"capture": os.path.basename(os.path.normpath(cap)), "variant": variant or "carry",
           "repeated": repeated, "frames": []}
    total = exact = 0
    for fr in chain["frames"][fa:fb + 1]:
        fdir = os.path.join(cap, fr["dir"])
        latest = {int(r): os.path.join(cap, p) for r, p in fr["seed"].items()}
        ops = []
        prune = []
        last_draw = None
        for k, o in enumerate(fr["ops"]):
            if o["op"] == "outside":
                continue
            if o["op"] == "seed":     # re-supplied from outside the chain: the reference is the capture
                latest[o["resource"]] = os.path.join(cap, o["file"])
                continue
            if o["op"] == "copy":
                src, dst = o["src"], o["dst"]
                spec = chain["resources"][str(src)]
                ref_path = latest.get(src)
                if repeated:
                    latest[dst] = ref_path
                    continue
                nat_path = os.path.join(fdir, f"native_copy_r{dst}{sfx}.bin")
                if ref_path is None:
                    ops.append({"op": k, "copy": [src, dst], "error": "no reference for the source"})
                    continue
                rb, nb = read(ref_path), read(nat_path)
                r, _ = measure(rb, nb, spec["format"], spec["width"], spec["height"])
                r.update({"op": k, "copy": [src, dst], "reference": os.path.relpath(ref_path, cap).replace("\\", "/"),
                          "reference_sha256": sha(rb), "native_sha256": sha(nb)})
                if r["bit_exact"]:
                    prune.append(nat_path)
                ops.append(r)
                latest[dst] = ref_path
                total += 1; exact += r["bit_exact"]
                continue
            ddir = os.path.join(cap, o["dir"])
            desc = json.load(open(os.path.join(ddir, "desc.json")))
            t = desc["target"]
            ref_path = os.path.join(ddir, "target_after.bin")
            nat_path = os.path.join(ddir, "native_after_repeated.bin" if repeated else f"native_after{sfx}.bin")
            latest[o["target"]] = ref_path
            if repeated and not os.path.exists(nat_path):
                continue
            rb, nb = read(ref_path), read(nat_path)
            r, extra = measure(rb, nb, t["format"], t["width"], t["height"])
            r.update({"op": k, "draw": o["dir"], "target": o["target"], "reference_sha256": sha(rb),
                      "native_sha256": sha(nb)})
            if r["bit_exact"]:
                prune.append(nat_path)
            ops.append(r)
            total += 1; exact += r["bit_exact"]
            last_draw = (o, ddir, t, ref_path, nat_path, extra)
        if "--png" in args and last_draw:
            o, ddir, t, ref_path, nat_path, extra = last_draw
            sheet(os.path.join(fdir, f"final_sheet{sfx}.png"), t["width"], t["height"], read(ref_path), read(nat_path),
                  t["format"], extra[3] if extra else None)
        if "--prune" in args:     # a bit-exact native output is its hash from here on
            for pth in prune:
                os.remove(pth)
        out["frames"].append({"frame": fr["index"], "engine_frame": fr["engine_frame"], "operations": len(ops),
                              "bit_exact": sum(1 for r in ops if r.get("bit_exact")), "ops": ops})
    out["compared"] = total
    out["bit_exact"] = exact
    name = f"compare_chain{sfx}{'_repeated' if repeated else ''}.json"
    with open(os.path.join(cap, name), "w") as f:
        json.dump(out, f, indent=1)
    print(f"{cap}: {out['variant']}{' (after the timing repetitions)' if repeated else ''}: "
          f"{exact}/{total} outputs bit-exact over frames {fa}-{fb}")
    for fr in out["frames"]:
        bad = [r for r in fr["ops"] if not r.get("bit_exact")]
        print(f"  frame {fr['frame']} (engine {fr['engine_frame']}): {fr['bit_exact']}/{fr['operations']} exact"
              + ("" if not bad else ": differ " + ", ".join(
                  f"{r.get('draw', r.get('copy'))} ({r.get('differing_pixels', '?')} px)" for r in bad[:8])))


if __name__ == "__main__":
    main()

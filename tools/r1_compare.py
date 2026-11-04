"""R1: the native executor's output against the existing renderer's, per pixel.

r1_compare.py <capture dir> [--png] [--poisoned]

Reads the capture's desc.json (the target's format and size), the existing
renderer's output (target_after.bin, copied on the GPU right after the draw)
and r1_replay's (native_output.bin), and compares them raw, at the target
format's own precision:
  R16G16B16A16_SFLOAT  each channel's 16 bits: pixels that differ, the
                       largest difference in ulps, and, as values, the
                       largest and mean absolute error
  R8G8B8A8_UNORM, B8G8R8A8_UNORM  each channel's 8 bits: pixels that differ,
                       the largest and mean difference in 1/255 steps
Exact agreement is the aim; any difference is reported, not tolerated.
--poisoned compares r1_replay --poison's output instead (the target started
as 0xA5 bytes), into compare_poisoned.json: a pixel the native draw did not
produce cannot match there.
Writes compare.json; with --png also reference.png, native.png, diff.png
(any differing channel lit) and the inputs (tex<u>.png, target_before.png)
for viewing -- a float image clamped to 0..1, linear. Standard library only
(struct's 'e' reads half floats; zlib writes the PNGs). Game data: keep the
images local.
"""
import json
import math
import os
import struct
import sys
import zlib

FMT = {97: ("R16G16B16A16_SFLOAT", "f16", 8), 37: ("R8G8B8A8_UNORM", "u8", 4),
       44: ("B8G8R8A8_UNORM", "u8bgra", 4), 109: ("R32G32B32A32_SFLOAT", "f32", 16)}


def read(path):
    with open(path, "rb") as f:
        return f.read()


def channels(raw, kind, n):
    """n texels as four lists of channel values (and raw codes)."""
    if kind == "f16":
        codes = struct.unpack(f"<{n * 4}H", raw[:n * 8])
        vals = struct.unpack(f"<{n * 4}e", raw[:n * 8])
    elif kind == "f32":
        codes = struct.unpack(f"<{n * 4}I", raw[:n * 16])
        vals = struct.unpack(f"<{n * 4}f", raw[:n * 16])
    else:
        codes = raw[:n * 4]
        vals = [c / 255.0 for c in codes]
    return codes, vals


def f16_ordinal(c):
    """A half's bits as an ordered integer, for ulp distances."""
    return -(c & 0x7FFF) if c & 0x8000 else c


def png(path, w, h, rgba8):
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    rows = b"".join(b"\x00" + bytes(rgba8[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b""))


def to8(vals, kind):
    out = bytearray(len(vals))
    for i, v in enumerate(vals):
        if kind.startswith("u8"):
            out[i] = int(round(v * 255))
        else:
            out[i] = 0 if not (v == v) or v <= 0 else 255 if v >= 1 else int(v * 255 + 0.5)
    if kind == "u8bgra":
        for i in range(0, len(out), 4):
            out[i], out[i + 2] = out[i + 2], out[i]
    if kind in ("f16", "f32"):   # alpha shown opaque: the colour is what is compared by eye
        for i in range(3, len(out), 4):
            out[i] = 255
    return out


def compare(d, want_png, poisoned=False):
    desc = json.load(open(os.path.join(d, "desc.json")))
    t = desc["target"]
    w, h, fmt = t["width"], t["height"], t["format"]
    if fmt not in FMT:
        sys.exit(f"{d}: format {fmt} not handled")
    name, kind, bpp = FMT[fmt]
    n = w * h
    ref = read(os.path.join(d, "target_after.bin"))
    nat = read(os.path.join(d, "native_output_poisoned.bin" if poisoned else "native_output.bin"))
    before = read(os.path.join(d, "target_before.bin"))
    rc, rv = channels(ref, kind, n)
    nc, nv = channels(nat, kind, n)
    bc, _ = channels(before, kind, n)
    chan = "RGBA" if kind != "u8bgra" else "BGRA"
    res = {"capture": os.path.basename(os.path.normpath(d)), "format": name, "width": w, "height": h,
           "pixels": n, "bit_exact": ref[:n * bpp] == nat[:n * bpp],
           "pixels_written_by_the_pass": sum(1 for i in range(n) if any(rc[i * 4 + c] != bc[i * 4 + c] for c in range(4))),
           "channels": {}}
    diffmask = bytearray(n * 4)
    for c in range(4):
        differ = 0; max_abs = 0.0; sum_abs = 0.0; max_ulp = 0; nonfinite = 0
        for i in range(n):
            k = i * 4 + c
            if rc[k] == nc[k]:
                continue
            differ += 1
            diffmask[k] = 255
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
        if kind == "f16":
            ch["max_ulp"] = max_ulp
        elif kind.startswith("u8"):
            ch["max_step_difference"] = max_ulp
        if nonfinite:
            ch["nonfinite_differences"] = nonfinite
        res["channels"][chan[c]] = ch
    vals = [v for v in rv if math.isfinite(v)]
    res["reference_value_range"] = [min(vals), max(vals)] if vals else None
    res["native_target_start"] = "0xA5 bytes" if poisoned else "the captured content"
    with open(os.path.join(d, "compare_poisoned.json" if poisoned else "compare.json"), "w") as f:
        json.dump(res, f, indent=1)
    if want_png and not poisoned:
        png(os.path.join(d, "reference.png"), w, h, to8(rv, kind))
        png(os.path.join(d, "native.png"), w, h, to8(nv, kind))
        lit = bytearray(n * 4)
        for i in range(n):
            if any(diffmask[i * 4 + c] for c in range(4)):
                lit[i * 4:i * 4 + 4] = b"\xff\x00\xff\xff"
            else:
                lit[i * 4 + 3] = 255
        png(os.path.join(d, "diff.png"), w, h, lit)
        _, bv = channels(before, kind, n)
        png(os.path.join(d, "target_before.png"), w, h, to8(bv, kind))
        for tex in desc.get("textures", []):
            im = tex["image"]
            if im["format"] in FMT and im.get("levels", 1) == 1 and im.get("layers", 1) == 1:
                _, k2, _ = FMT[im["format"]]
                _, tv = channels(read(os.path.join(d, im["file"])), k2, im["width"] * im["height"])
                png(os.path.join(d, f"tex{tex['unit']}.png"), im["width"], im["height"], to8(tv, k2))
    return res


def main():
    d = sys.argv[1]
    r = compare(d, "--png" in sys.argv[2:], "--poisoned" in sys.argv[2:])
    print(json.dumps(r, indent=1))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""The first-session pipeline manifest (M9: docs/M9_LAUNCHER_PLAN.md section 15): the pipelines the test routes and
play sessions met, described so that a new install prepares them before its first session -- resolved against the
player's own disc, with none of the game's programs or text in the manifest.

    tools/pipeline_manifest.py make --disc DISC --out MANIFEST RECIPES.bin [RECIPES.bin ...]   (a developer's side)
    tools/pipeline_manifest.py resolve --manifest MANIFEST --disc DISC --recipes OUT.bin       (the player's side)

A line: a recipe's record without its programs -- its key, its pipeline state and the translation's parameters (the
renderer's own structures: numbers the game's use of its shaders gave), hex -- and for each of its two programs where
it is on the disc: a file under PS3_GAME/USRDIR, an offset, the transform the game's loading applies (0 as stored,
1 each 32-bit word's bytes reversed) and a 64-bit FNV-1a hash of the bytes there, which the player's disc must match.

A fragment program's constants are patched into it by the game as it is loaded; the renderer keeps them out of the
translation (they are buffered: the pipeline's key is the program's structural hash), so the stored program, which
differs from the recorded one only in those, translates to the same shader. `make` finds a fragment program by its
16-byte blocks (an instruction or a constant) and keeps the place where most are equal; that the found programs
translate the same is checked by preparing both (docs: section 15, the translation cache translating none again).

`resolve` appends to a recipes file (format 2, as the renderer writes them: port/native/render/ppfx_render.cpp) each
pipeline whose key it does not hold and whose programs the disc has as the manifest says; the game's `--prepare`
then makes them. The recipes it writes hold the player's own programs and stay in their install.
"""
import argparse
import os
import struct
import sys

HEAD = 536                  # RecipeHead: magic, format, key, PipeState (128), ProgFixed (384), vp_len, fp_len
MAGIC, FORMAT = 0x32504352, 2
MASK = 0xFFFFFFFFFFFFFFFF
FILES = ("data/shaders/shaders.ps3.pkg", "data/shaders/shaders_0.ps3.pkg", "data/shaders/SasShader.ps3.spk",
         "data/shaders/SasShader2.ps3.spk", "EBOOT.elf")
HEADER = ("# Cubic.Heroes' first-session pipeline manifest (tools/pipeline_manifest.py; docs/M9_LAUNCHER_PLAN.md "
          "section 15): per line a pipeline's\n# record without its programs (hex) and, for its vertex and fragment "
          "programs, a file on the player's disc, an offset, a transform\n# and the FNV-1a hash of the bytes there.\n")


def fnv64(b, h=0xcbf29ce484222325):
    for x in b:
        h = ((h ^ x) * 0x100000001b3) & MASK
    return h


def wswap(b):
    return b"".join(b[i:i + 4][::-1] for i in range(0, len(b) - len(b) % 4, 4))


TRANSFORMS = {0: lambda b: b, 1: wswap}     # each its own inverse


def recipe_sum(head, vp, fp):
    x = fnv64(head) ^ ((fnv64(vp) * 31) & MASK) ^ ((fnv64(fp) * 131) & MASK)
    return (x ^ (x >> 32)) & 0xFFFFFFFF


def read_recipes(path):
    """[(key, head, vp, fp)] of a recipes file's whole records"""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return []
    off, out = 0, []
    while off + HEAD <= len(data) and struct.unpack_from("<II", data, off) == (MAGIC, FORMAT):
        vl, fl = struct.unpack_from("<II", data, off + HEAD - 8)
        end = off + HEAD + vl + fl + 4
        if end > len(data):
            break
        head = data[off:off + HEAD]
        vp = data[off + HEAD:off + HEAD + vl]
        fp = data[off + HEAD + vl:end - 4]
        if struct.unpack_from("<I", data, end - 4)[0] != recipe_sum(head, vp, fp):
            break
        out.append((struct.unpack_from("<Q", head, 8)[0], head, vp, fp))
        off = end
    return out


def disc_files(disc):
    usr = os.path.join(disc, "PS3_GAME", "USRDIR")
    out = {}
    for f in FILES:
        p = os.path.join(usr, *f.split("/"))
        if os.path.exists(p):
            with open(p, "rb") as fh:
                out[f] = fh.read()
    return out


def locate(prog, blobs, tolerant):
    """(file, offset, transform) of the program on the disc: whole, or (tolerant) where its first 16-byte block and
    the most of its blocks (at least half) are equal"""
    best = None
    for tid, t in TRANSFORMS.items():
        tp = t(prog)
        for fn, blob in blobs.items():
            at = blob.find(tp)
            if at >= 0:
                return fn, at, tid
        if not tolerant or len(tp) < 32:
            continue
        nblk = len(tp) // 16
        for fn, blob in blobs.items():
            at = blob.find(tp[:16])
            while at >= 0:
                cand = blob[at:at + len(tp)]
                eq = sum(1 for i in range(0, len(tp), 16) if cand[i:i + 16] == tp[i:i + 16])
                if eq * 2 >= nblk and (best is None or eq > best[3]):
                    best = (fn, at, tid, eq)
                at = blob.find(tp[:16], at + 1)
    return best[:3] if best else None


def make(disc, recipe_files, out):
    blobs = disc_files(disc)
    seen, lines, missing = set(), [], 0
    for rf in recipe_files:
        for key, head, vp, fp in read_recipes(rf):
            if key in seen:
                continue
            seen.add(key)
            v, f = locate(vp, blobs, False), locate(fp, blobs, True)
            if not v or not f:
                missing += 1
                continue
            vb = blobs[v[0]][v[1]:v[1] + len(vp)]
            fb = blobs[f[0]][f[1]:f[1] + len(fp)]
            lines.append(f"{head.hex()} {v[0]} {v[1]} {v[2]} {fnv64(vb):016x} {f[0]} {f[1]} {f[2]} {fnv64(fb):016x}")
    with open(out, "w", encoding="ascii", newline="\n") as o:
        o.write(HEADER)
        for ln in sorted(lines):
            o.write(ln + "\n")
    return {"pipelines": len(lines), "left_out": missing}


def parse(path):
    """the manifest's entries: [(key, head, (file, off, transform, hash), (file, off, transform, hash))]"""
    out = []
    with open(path, encoding="ascii") as f:
        for ln in f:
            p = ln.split()
            if not p or p[0].startswith("#"):
                continue
            if len(p) != 9 or len(p[0]) != HEAD * 2:
                raise ValueError(f"{path}: a line not of this format")
            head = bytes.fromhex(p[0])
            if struct.unpack_from("<II", head) != (MAGIC, FORMAT):
                raise ValueError(f"{path}: a record of another format")
            prog = lambda q: (q[0], int(q[1]), int(q[2]), int(q[3], 16))  # noqa: E731
            out.append((struct.unpack_from("<Q", head, 8)[0], head, prog(p[1:5]), prog(p[5:9])))
    return out


def resolve(manifest, disc, recipes):
    """each pipeline of the manifest that the recipes file does not hold, its programs taken from the player's disc
    and checked by their hash, appended to the file; {"listed", "held", "added", "not_on_disc"}"""
    entries = parse(manifest)
    held = {k for k, _, _, _ in read_recipes(recipes)}
    good = 0
    if os.path.exists(recipes):     # whole records only (the game cuts a damaged tail the same way)
        good = sum(HEAD + len(vp) + len(fp) + 4 for _, _, vp, fp in read_recipes(recipes))
    blobs = disc_files(disc)
    add, absent = [], 0
    for key, head, v, f in entries:
        if key in held:
            continue
        vl, fl = struct.unpack_from("<II", head, HEAD - 8)
        progs = []
        for (fn, off, tid, h), n in ((v, vl), (f, fl)):
            b = blobs.get(fn, b"")[off:off + n]
            if len(b) != n or fnv64(b) != h or tid not in TRANSFORMS:
                break
            progs.append(TRANSFORMS[tid](b))
        if len(progs) != 2:
            absent += 1
            continue
        held.add(key)
        add.append(head + progs[0] + progs[1] + struct.pack("<I", recipe_sum(head, progs[0], progs[1])))
    if add:
        os.makedirs(os.path.dirname(os.path.abspath(recipes)), exist_ok=True)
        with open(recipes, "r+b" if os.path.exists(recipes) else "wb") as o:
            o.truncate(good)
            o.seek(good)
            for rec in add:
                o.write(rec)
    return {"listed": len(entries), "held": len(entries) - len(add) - absent, "added": len(add),
            "not_on_disc": absent}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("make")
    m.add_argument("--disc", required=True, help="the extracted disc (PS3_GAME/USRDIR, its EBOOT.elf decrypted)")
    m.add_argument("--out", required=True)
    m.add_argument("recipes", nargs="+")
    r = sub.add_parser("resolve")
    r.add_argument("--manifest", required=True)
    r.add_argument("--disc", required=True)
    r.add_argument("--recipes", required=True)
    a = ap.parse_args()
    if a.cmd == "make":
        res = make(a.disc, a.recipes, a.out)
        print(f"{a.out}: {res['pipelines']} pipelines; {res['left_out']} left out (a program not on the disc)")
    else:
        print(resolve(a.manifest, a.disc, a.recipes))


if __name__ == "__main__":
    sys.exit(main())

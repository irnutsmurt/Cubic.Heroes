#!/usr/bin/env python3
"""Pass profiles for comprehensive pipeline preparation (M9.2c, docs/M92_SHADER_PLAN.md section 3.3).

The engine draws a shader package record's vertex and fragment programs together, and the record's group names the
pass it belongs to. What a pipeline needs besides the programs -- the targets, blend, depth, stencil, cull, the
alpha test, a shadow-compare sampler -- is the pass's and the material's. This tool learns those *profiles* from
recorded recipes (a developer's side): for each recorded pipeline whose two programs are one package record, the
record's group and its normalised render state. The game (`--prepare`) applies them to every record of the player's
own package: a record gets the profiles learned for its exact group, or, for a group no recording reached, those of
its group's class (group_class).

    tools/pipeline_profiles.py learn --disc DISC --out PROFILES RECIPES.bin [RECIPES.bin ...]

A line: `g <group>` or `c <class>`, then the profile -- the renderer's pipeline state (PipeState, 128 bytes, hex,
normalised: state the GPU ignores zeroed; the vertex stride and count and the constant count zeroed, as they come
from the record's own programs), the alpha test and its function, and the shadow-sampler template (16 descriptors of
4 bytes, hex; applied to the units a record's fragment program samples) -- and how many recorded pipelines had it.
Numbers only: no program, no shader text, nothing of the game's (tools/boundary_check.py).
"""
import argparse
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pipeline_manifest as pm  # noqa: E402

PKG = "data/shaders/shaders.ps3.pkg"
# PipeState's words (port/native/render/ppfx_render.cpp)
PS = ["kind", "rt", "df", "stride", "count", "lines", "color_mask", "front_face", "zminmax", "cull", "cull_face",
      "blend", "blend_sf_rgb", "blend_df_rgb", "blend_sf_a", "blend_df_a", "blend_eq_rgb", "blend_eq_a",
      "depth_test", "depth_write", "depth_func", "stencil_test", "stencil_func", "stencil_func_mask",
      "stencil_write_mask", "stencil_fail", "stencil_zfail", "stencil_zpass", "nconst", "r0", "r1", "r2"]
STENCIL = ["stencil_test", "stencil_func", "stencil_func_mask", "stencil_write_mask", "stencil_fail",
           "stencil_zfail", "stencil_zpass"]
BLEND = ["blend", "blend_sf_rgb", "blend_df_rgb", "blend_sf_a", "blend_df_a", "blend_eq_rgb", "blend_eq_a"]
D24S8, D32S8 = 129, 130   # VkFormat with stencil (the renderer's depth formats)
PF_ALPHA_TEST, PF_ALPHA_FUNC = 8, 9   # ProgFixed's words
PF_SHADOW = 96                        # ProgFixed: the 16 shadow descriptors' offset
HEAD_PS, HEAD_PF = 16, 16 + 128       # RecipeHead: magic, format, key, PipeState, ProgFixed, lengths


def canon(s):
    """The render state with what the GPU ignores zeroed -- ppfx_render.cpp's canon(), word for word."""
    s = dict(s)
    if s["kind"] != 2:
        return s
    has_rt, has_df = s["rt"] != 0, s["df"] != 0
    if not has_df or not s["stencil_test"] or s["df"] not in (D24S8, D32S8):
        for k in STENCIL:
            s[k] = 0
    if not has_rt or not s["blend"]:
        for k in BLEND:
            s[k] = 0
    if not has_df or not s["depth_test"]:
        s["depth_test"] = s["depth_write"] = s["depth_func"] = 0
    if not s["cull"]:
        s["cull_face"] = 0
    if not has_rt:
        s["color_mask"] = 0
    return s


def fp_word(b, at):
    w = struct.unpack_from(">I", b, at)[0]
    return ((w << 16) | (w >> 16)) & 0xFFFFFFFF


def fp_structure(code, seed=0xF5):
    """rsx_fp_structural_hash: the instructions hashed, the inline constant blocks skipped (their count hashed)"""
    h = pm.fnv64(struct.pack("<I", 0x31535046), seed)
    off = ni = nc = 0
    while off + 16 <= len(code):
        w0, w1, w2, w3 = (fp_word(code, off + 4 * i) for i in range(4))
        h = pm.fnv64(code[off:off + 16], h)
        ni += 1
        off += 16
        if any((w & 3) == 2 for w in (w1, w2, w3)):
            if off + 16 > len(code):
                return 0
            nc += 1
            off += 16
        if w0 & 1:
            return pm.fnv64(struct.pack("<I", nc), pm.fnv64(struct.pack("<I", ni), h))
    return 0


def package_records(disc):
    """[(group, vertex code as the renderer has it, fragment structure)] of the disc's shader package"""
    with open(os.path.join(disc, "PS3_GAME", "USRDIR", *PKG.split("/")), "rb") as f:
        d = f.read()
    if d[:4] != b"spkg":
        raise SystemExit(f"{PKG}: not a shader package")
    n = struct.unpack_from(">I", d, 0x14)[0]
    out = []
    for i in range(n):
        voff, vsize, group, _ = struct.unpack_from(">IIII", d, 0x40 + 16 * i)
        foff, fsize, _, _ = struct.unpack_from(">IIII", d, 0x40 + 16 * (n + i))
        vr, fr = d[voff:voff + vsize], d[foff:foff + fsize]
        if vr[:4] != b"CGB\0" or fr[:4] != b"CGB\0":
            continue
        ub = struct.unpack_from(">H", vr, 8)[0]
        out.append((group, pm.wswap(vr[32:32 + ub]), fp_structure(fr[32:])))
    return out


def group_class(g):
    """A group's class, for groups no recording reached: scene materials (top four bits 2 and 3) by those bits; the
    rest (generic, interface, the 16-bit pass) by those bits and the second byte -- ppfx_render.cpp's group_class()"""
    top = g >> 28
    return top << 8 if top >= 2 else top << 8 | (g >> 8 & 0xFF)


def profile_of(head):
    s = dict(zip(PS, struct.unpack_from("<32I", head, HEAD_PS)))
    pf = struct.unpack_from("<24I", head, HEAD_PF)
    c = canon(s)
    c["stride"] = c["count"] = c["nconst"] = 0
    at = pf[PF_ALPHA_TEST]
    shadow = head[HEAD_PF + PF_SHADOW:HEAD_PF + PF_SHADOW + 64]
    state = struct.pack("<32I", *(c[k] for k in PS))
    return state.hex(), at, pf[PF_ALPHA_FUNC] if at else 0, shadow.hex()


def learn(disc, recipe_files, out):
    recs = package_records(disc)
    by_v = collections.defaultdict(set)
    by_f = collections.defaultdict(set)
    for i, (_, vcode, fs) in enumerate(recs):
        by_v[vcode].add(i)
        by_f[fs].add(i)
    groups = collections.defaultdict(collections.Counter)
    classes = collections.defaultdict(collections.Counter)
    seen = set()
    n = matched = 0
    for path in recipe_files:
        for key, head, vp, fp in pm.read_recipes(path):
            if key in seen:
                continue
            seen.add(key)
            n += 1
            if struct.unpack_from("<I", head, HEAD_PS)[0] != 2:
                continue
            common = by_v.get(vp, set()) & by_f.get(fp_structure(fp), set())
            if not common:
                continue
            matched += 1
            g = recs[min(common)][0]
            p = profile_of(head)
            groups[g][p] += 1
            classes[group_class(g)][p] += 1
    lines = ["# Cubic.Heroes' pass profiles (tools/pipeline_profiles.py; docs/M92_SHADER_PLAN.md section 3.3): per line a",
             "# package group (g) or group class (c), the normalised pipeline state (hex), the alpha test and function,",
             "# the shadow-sampler template (hex) and how many recorded pipelines had it. Numbers only."]
    for g in sorted(groups):
        for (st, at, af, sh), k in sorted(groups[g].items()):
            lines.append(f"g {g:08x} {st} {at} {af} {sh} {k}")
    for c in sorted(classes):
        for (st, at, af, sh), k in sorted(classes[c].items()):
            lines.append(f"c {c:04x} {st} {at} {af} {sh} {k}")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print(f"{n} recorded pipelines, {matched} of one package record: {sum(len(v) for v in groups.values())} profiles "
          f"in {len(groups)} groups, {sum(len(v) for v in classes.values())} in {len(classes)} classes -> {out}")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("learn")
    a.add_argument("--disc", required=True)
    a.add_argument("--out", required=True)
    a.add_argument("recipes", nargs="+")
    args = ap.parse_args(argv)
    return learn(args.disc, args.recipes, args.out)


if __name__ == "__main__":
    sys.exit(main())

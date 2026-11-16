#!/usr/bin/env python3
"""tools/pipeline_profiles.py: the normalised render state, the group classes, the fragment programs' structure, and
learning profiles from recipes against a made-up shader package (nothing of the game's)."""
import os
import struct
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import pipeline_manifest as pm  # noqa: E402
import pipeline_profiles as pp  # noqa: E402


def fp_word(w):   # a fragment program's words are stored with their 16-bit halves swapped
    return struct.pack(">I", ((w << 16) | (w >> 16)) & 0xFFFFFFFF)


def fp_program(const_value, op=0x01):
    """two instructions; the first reads an inline constant (its source type 2), the second ends the program"""
    i0 = fp_word(op << 24) + fp_word(2) + fp_word(0) + fp_word(0)
    c0 = struct.pack(">4f", const_value, 0, 0, 1)
    i1 = fp_word((0x02 << 24) | 1) + fp_word(0) + fp_word(0) + fp_word(0)
    return i0 + c0 + i1


def cgb(ucode, profile, inputs=0, ctrl=0x8440, regs=2):
    h = b"CGB\0" + struct.pack(">I", 0) + struct.pack(">HHH", len(ucode), profile, inputs) + b"\0\0"
    h += b"\0" * (26 - len(h)) + struct.pack(">H", ctrl) + bytes([regs]) + b"\0" * 3
    assert len(h) == 32
    return h + ucode


def package(records):
    """records: [(group, id, vertex ucode, fragment ucode)]"""
    n = len(records)
    head = b"spkg" + struct.pack(">III", 4, 1, 0) + b"ps3\0" + struct.pack(">III", n, n, n) + b"\0" * 32
    body, tabs = b"", [[], []]
    base = 0x40 + n * 40
    for g, i, vp, fp in records:
        for t, rec in ((0, cgb(vp, 3, 0x3)), (1, cgb(fp, 0x103))):
            tabs[t].append(struct.pack(">IIII", base + len(body), len(rec), g, i))
            body += rec
    index = b"".join(struct.pack(">II", g, i) for g, i, _, _ in records)
    return head + b"".join(tabs[0]) + b"".join(tabs[1]) + index + body


def recipe(key, state, vp, fp, alpha=0, func=0):
    ps = struct.pack("<32I", *state)
    pf = [0] * 24
    pf[pp.PF_ALPHA_TEST], pf[pp.PF_ALPHA_FUNC] = alpha, func
    pfb = struct.pack("<24I", *pf) + b"\0" * (384 - 96)
    head = struct.pack("<IIQ", pm.MAGIC, pm.FORMAT, key) + ps + pfb + struct.pack("<II", len(vp), len(fp))
    assert len(head) == pm.HEAD
    return head + vp + fp + struct.pack("<I", pm.recipe_sum(head, vp, fp))


def state(**kw):
    s = dict.fromkeys(pp.PS, 0)
    s.update(kind=2, rt=97, df=129, stride=32, count=2, color_mask=0x01010101, depth_test=1, depth_write=1,
             depth_func=0x203, cull=1, cull_face=0x405, front_face=0x901)
    s.update(kw)
    return [s[k] for k in pp.PS]


class ProfilesTest(unittest.TestCase):
    def test_canon_zeroes_what_the_gpu_ignores(self):
        s = dict(zip(pp.PS, state(stencil_test=0, stencil_func=0x207, blend=0, blend_sf_rgb=0x302, cull=0)))
        c = pp.canon(s)
        self.assertEqual((c["stencil_func"], c["blend_sf_rgb"], c["cull_face"]), (0, 0, 0))
        self.assertEqual(c["depth_func"], 0x203)                     # depth test on: kept
        c = pp.canon(dict(s, rt=0))
        self.assertEqual(c["color_mask"], 0)                         # no colour target
        c = pp.canon(dict(s, df=0, stencil_test=1, stencil_func=0x207))
        self.assertEqual((c["depth_test"], c["stencil_test"], c["stencil_func"]), (0, 0, 0))
        self.assertEqual(pp.canon(dict(s, kind=1, cull_face=0x405, cull=0))["cull_face"], 0x405)   # PPFX: untouched

    def test_group_class(self):
        self.assertEqual(pp.group_class(0x20000414), 0x200)
        self.assertEqual(pp.group_class(0x30040414), 0x300)
        self.assertEqual(pp.group_class(0x0000040F), 0x004)
        self.assertEqual(pp.group_class(0x00050404), 0x004)
        self.assertEqual(pp.group_class(0x10000114), 0x101)

    def test_fp_structure_ignores_constants(self):
        a, b, c = fp_program(1.0), fp_program(2.0), fp_program(1.0, op=0x03)
        self.assertNotEqual(pp.fp_structure(a), 0)
        self.assertEqual(pp.fp_structure(a), pp.fp_structure(b))
        self.assertNotEqual(pp.fp_structure(a), pp.fp_structure(c))

    def test_learn(self):
        vp = struct.pack(">4I", 0x401F9C6C, 0, 0, 1)
        fp1, fp2 = fp_program(1.0), fp_program(1.0, op=0x03)
        with tempfile.TemporaryDirectory() as d:
            usr = os.path.join(d, "disc", "PS3_GAME", "USRDIR", "data", "shaders")
            os.makedirs(usr)
            with open(os.path.join(usr, "shaders.ps3.pkg"), "wb") as f:
                f.write(package([(0x20000414, 0xBF001010, vp, fp1), (0x0000040F, 0xFF001002, vp, fp2)]))
            rec = os.path.join(d, "r.bin")
            with open(rec, "wb") as f:
                # the scene record twice, differing only in a disabled stencil test's function: one profile
                f.write(recipe(1, state(stencil_func=0x207), pm.wswap(vp), fp_program(5.0)))
                f.write(recipe(2, state(stencil_func=0x202), pm.wswap(vp), fp1))
                # the generic record with blending and the alpha test
                f.write(recipe(3, state(blend=1, blend_sf_rgb=0x302, blend_df_rgb=0x303, depth_write=0), pm.wswap(vp),
                               fp2, alpha=1, func=0x204))
                # a pipeline whose programs are not one record (PPFX's): not learned
                f.write(recipe(4, state(kind=1), b"\1" * 16, b"\2" * 16))
            out = os.path.join(d, "profiles.txt")
            self.assertEqual(pp.learn(os.path.join(d, "disc"), [rec], out), 0)
            with open(out, encoding="utf-8") as f:
                lines = [ln.split() for ln in f if not ln.startswith("#")]
        groups = [ln for ln in lines if ln[0] == "g"]
        self.assertEqual(sorted((ln[1], ln[3], ln[4], ln[-1]) for ln in groups),
                         [("0000040f", "1", "516", "1"), ("20000414", "0", "0", "2")])
        self.assertEqual(sorted(ln[1] for ln in lines if ln[0] == "c"), ["0004", "0200"])
        for ln in groups:   # normalised: stride, count, constants zero; numbers only
            st = struct.unpack("<32I", bytes.fromhex(ln[2]))
            self.assertEqual((st[3], st[4], st[28]), (0, 0, 0))
            self.assertEqual(len(ln[5]), 128)

    def test_key_class(self):   # M9.2g: the pass and the lighting the key says; the preferred-variant bit ignored
        self.assertEqual(pp.key_class(0x20000014, 0xBF5A102D), 0x112)    # depth-of-field output, per-pixel
        self.assertEqual(pp.key_class(0x30000014, 0xBF5A102D), 0x112)    # its preferred variant: the same class
        self.assertEqual(pp.key_class(0x0000000F, 0x01000000), 0x101)    # a shadow caster
        self.assertEqual(pp.key_class(0x0000000F, 0xFF001001), 0x130)    # unlit, per-pixel, neither pass
        self.assertEqual(pp.key_class(0x00000004, 0xBF001010), 0x100)    # per-vertex lit, neither pass

    def test_product(self):   # a pass part with every material part, normalised as the renderer would
        def prof(**kw):
            s = {k: 0 for k in pp.PS}
            s.update(kind=2, rt=97, df=129, depth_test=1, depth_func=0x203, **kw)
            return struct.pack("<32I", *(s[k] for k in pp.PS)).hex(), 0, 0, "00" * 64
        opaque = prof(depth_write=1)
        blend = prof(cull=1, cull_face=0x405, blend=1, blend_sf_rgb=0x302, blend_df_rgb=0x303, blend_eq_rgb=0x8006)
        out = pp.product({opaque, blend})
        self.assertEqual(len(out), 4)                    # two pass parts (cull or not) x two materials
        self.assertIn(opaque, out)
        self.assertIn(blend, out)
        culled_opaque = prof(depth_write=1, cull=1, cull_face=0x405)
        self.assertIn(culled_opaque, out)


if __name__ == "__main__":
    unittest.main()

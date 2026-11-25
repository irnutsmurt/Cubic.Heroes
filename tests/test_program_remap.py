"""tools/program_remap.py on two small made-up programs (no game data): v1.00's functions matched by content -- one
in place, one moved whose call's displacement changed, one relocated (an address immediate changed), one changed --
data that agrees and data that doesn't, the configuration written for the other program (observers with their v1.00
id), port/'s aliases and PA() table, the unresolved addresses stopping the run and a hand-made decision settling one."""
import json
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
import program_remap  # noqa: E402


def load(path):
    with open(path) as f:
        return json.load(f)


def text(path):
    with open(path) as f:
        return f.read()


def save(path, data, mode="w"):
    with open(path, mode) as f:
        f.write(data) if isinstance(data, (str, bytes)) else json.dump(data, f)

CODE, DATA = 0x10000, 0x20000
NOP, BLR = 0x60000000, 0x4E800020


def bl(frm, to):
    return 0x48000001 | ((to - frm) & 0x03FFFFFC)


def addi(rt, ra, imm):
    return (14 << 26) | (rt << 21) | (ra << 16) | (imm & 0xFFFF)


def elf(code_words, data_bytes):
    """a 64-bit big-endian ELF: a code segment at CODE (R E) and a data segment at DATA (RW)"""
    code = struct.pack(f">{len(code_words)}I", *code_words)
    ph_off, ph_n = 0x40, 2
    code_off = 0x100
    data_off = code_off + len(code)
    h = bytearray(0x40)
    h[0:4] = b"\x7fELF"; h[4] = 2; h[5] = 2; h[6] = 1
    struct.pack_into(">HHI", h, 0x10, 2, 21, 1)
    struct.pack_into(">QQQ", h, 0x18, CODE, ph_off, 0)
    struct.pack_into(">IHHHHHH", h, 0x30, 0, 0x40, 0x38, ph_n, 0, 0, 0)
    ph = b""
    ph += struct.pack(">IIQQQQQQ", 1, 5, code_off, CODE, CODE, len(code), len(code), 0x10)
    ph += struct.pack(">IIQQQQQQ", 1, 6, data_off, DATA, DATA, len(data_bytes), len(data_bytes) + 0x100, 0x10)
    img = bytes(h) + ph
    img += b"\0" * (code_off - len(img)) + code + data_bytes
    return img


class ProgramRemap(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        d = self.tmp.name
        # v1.00: f1 at +0 (4 words), f2 at +0x10 (calls f1), f3 at +0x20 (addi r3,r2,0x100), f4 at +0x30
        a = [NOP, NOP, NOP, BLR]
        a += [NOP, bl(CODE + 0x14, CODE), NOP, BLR]
        a += [addi(3, 2, 0x100), NOP, NOP, BLR]
        a += [NOP, NOP, 0x7C632214, BLR]
        # the other: f1 in place; 0x10 of new code inserted; f2 moved +0x10 (its call longer); f3 relocated
        # (+0x10, addi 0x180); f4 changed (another instruction)
        b = [NOP, NOP, NOP, BLR]
        b += [0x38600001, NOP, NOP, BLR]
        b += [NOP, bl(CODE + 0x24, CODE), NOP, BLR]
        b += [addi(3, 2, 0x180), NOP, NOP, BLR]
        b += [NOP, NOP, 0x7C632A14, BLR]
        da, db = bytes(range(64)), bytes(range(32)) + bytes(32)   # the second half of the data differs
        self.a, self.b = os.path.join(d, "a.elf"), os.path.join(d, "b.elf")
        save(self.a, elf(a, da), "wb")
        save(self.b, elf(b, db), "wb")
        self.fa, self.fb = os.path.join(d, "a.json"), os.path.join(d, "b.json")
        save(self.fa, [{"start": hex(CODE + i * 16), "end": hex(CODE + i * 16 + 16)} for i in range(4)])
        save(self.fb, {"functions": [{"start": CODE + i * 16, "end": CODE + i * 16 + 16} for i in range(5)]})
        self.cfg = os.path.join(d, "config")
        self.port = os.path.join(d, "port")
        os.makedirs(self.cfg)
        os.makedirs(self.port)
        save(os.path.join(self.cfg, "r8_hooks.json"),
             {"_comment": "test", "entry_hooks": [{"addr": "0x00010010", "host": "h", "guard": "g"},
                                                  {"addr": "0x00010028", "host": "h2", "guard": "g"}],
              "value": "0x00000004"})
        save(os.path.join(self.cfg, "r7_native_hooks.json"), {"observers": [{"addr": "0x00010000"}, {"addr": "0x00010010"}]})
        save(os.path.join(self.port, "x.cpp"), "void f() { func_00010010(ctx); g(PA(0x00020004u)); h(PA(0x00010020)); }\n")
        self.out = os.path.join(d, "out")

    def tearDown(self):
        self.tmp.cleanup()

    def run_tool(self, *extra):
        return program_remap.main([self.a, self.fa, self.b, self.fb, "TEST00000-01.01", "--out", self.out,
                                   "--config", self.cfg, "--port", self.port, *extra])

    def test_match(self):
        A, B = program_remap.Program(self.a, self.fa), program_remap.Program(self.b, self.fb)
        m = program_remap.match(A, B)
        self.assertEqual(m[CODE], (CODE, "identical"))
        self.assertEqual(m[CODE + 0x10], (CODE + 0x20, "moved"))        # its call's displacement masked
        self.assertEqual(m[CODE + 0x20], (CODE + 0x30, "relocated"))    # only an address immediate differs
        self.assertNotIn(CODE + 0x30, m)                                # changed
        mp = program_remap.Mapper(A, B)
        self.assertEqual(mp.map(CODE + 0x18, "t"), CODE + 0x28)          # inside a moved function: the same offset
        self.assertEqual(mp.map(DATA + 4, "t"), DATA + 4)                # data that agrees
        self.assertIsNone(mp.map(DATA + 0x30, "t"))                      # data that doesn't
        self.assertIsNone(mp.map(CODE + 0x34, "t"))                      # a changed function
        self.assertEqual(sorted(mp.unresolved), [CODE + 0x34, DATA + 0x30])

    def test_relocated_only(self):
        self.assertTrue(program_remap.relocated_only([addi(3, 2, 1), NOP], [addi(3, 2, 2), NOP]))
        self.assertFalse(program_remap.relocated_only([addi(3, 2, 1)], [addi(4, 2, 1)]))   # another register
        self.assertFalse(program_remap.relocated_only([NOP], [NOP, NOP]))                  # another length
        self.assertFalse(program_remap.relocated_only([0x7C632214], [0x7C632A14]))         # not an immediate

    def test_outputs(self):
        self.assertEqual(self.run_tool(), 0)    # every address resolved
        hooks = load(os.path.join(self.out, "r8_hooks.json"))
        self.assertEqual(hooks["_program"], "TEST00000-01.01")
        self.assertEqual([e["addr"] for e in hooks["entry_hooks"]], ["0x00010020", "0x00010038"])
        self.assertEqual(hooks["value"], "0x00000004")                   # not an address of the program: kept
        obs = load(os.path.join(self.out, "r7_native_hooks.json"))["observers"]
        self.assertEqual(obs, [{"addr": "0x00010000", "id": "0x00010000"}, {"addr": "0x00010020", "id": "0x00010010"}])
        self.assertIn("#define func_00010010 func_00010020", text(os.path.join(self.out, "port_aliases.h")))
        inc = text(os.path.join(self.out, "port_addrs.inc"))
        self.assertIn("{0x00020004u, 0x00020004u},", inc)
        self.assertIn("{0x00010020u, 0x00010030u},", inc)

    def test_unresolved_stops(self):
        save(os.path.join(self.port, "x.cpp"), "int z = PA(0x00010034);\n", "a")   # inside the changed f4
        rep = os.path.join(self.tmp.name, "rep.json")
        self.assertEqual(self.run_tool("--report", rep), 1)
        r = load(rep)
        self.assertEqual(list(r["unresolved"]), ["0x00010034"])
        self.assertEqual(r["unresolved"]["0x00010034"]["status"], "in a changed function")
        self.assertEqual(r["functions"]["identical"], 1)

    def test_manual_decides(self):
        os.makedirs(self.out)
        save(os.path.join(self.out, "remap_manual.json"), {"_comment": "reviewed", "0x00020030": {"to": None, "note": "no longer used"}})
        save(os.path.join(self.port, "x.cpp"), "int y = PA(0x00020030);\n", "a")
        self.assertEqual(self.run_tool(), 0)
        self.assertNotIn("0x00020030", text(os.path.join(self.out, "port_addrs.inc")))


if __name__ == "__main__":
    unittest.main()

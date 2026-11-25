"""port/'s per-program addresses (port/program_addrs.h) used the one right way in the lift's observers (beta.6; the
rule from the v1.01 port, 2026-11-25):

- an observer (an entry or exit hook the lift calls: `f(uint32_t id, ppu_context* ctx)`) is handed its id -- the number
  the lift gives it, v1.00's address in every program (ppu_lifter's observer "id", ps3recomp 0178). The id is compared
  with v1.00's literal, NEVER through PA()/PA_RT() or a name holding their value: in v1.01 the mapped address of a
  moved function never equals the id. Exactly this silently broke the developer tools' no-death on v1.01 (113f3f1:
  `addr != fn_damage_hp`, fn_damage_hp = PA_RT(0x0011A94C));
- addresses read from the guest at run time (a link register, a vtable's entry, a task's entry, a TOC) are the
  running program's and are compared with PA()/PA_RT() -- not this test's business.

Every observer in port/ is read, and a comparison (or switch case) of its id against a mapped address is reported with
its line. Static: no build, no game.
    py -3 -m unittest tests.test_program_addrs_rule
"""
import os
import re
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT = os.path.join(ROOT, "port")
OBSERVER = re.compile(r"\b(\w+)\s*\(\s*uint32_t\s+(\w+)\s*,\s*ppu_context\s*\*\s*\w+\s*\)\s*(\{|$)")
MAPPED_NAME = re.compile(r"\b([A-Za-z_]\w*)\s*(?:=|\{|\()\s*PA(?:_RT)?\(")   # a name given a mapped address


def sources(root=PORT):
    """[(path, [lines without // comments])]"""
    out = []
    for d, _, fs in os.walk(root):
        for f in fs:
            if f.endswith((".cpp", ".c", ".h")) and f != "program_addrs.h":
                p = os.path.join(d, f)
                with open(p, encoding="utf-8", errors="replace") as fh:
                    out.append((p, [ln.split("//", 1)[0] for ln in fh]))
    return out


def mapped_names(files):
    return {n for _, lines in files for ln in lines for n in MAPPED_NAME.findall(ln) if n not in ("PA", "PA_RT")}


def observers(files):
    """[(path, function, id variable, [(line number, code)] of its body)] -- definitions only"""
    out = []
    for p, lines in files:
        for i, ln in enumerate(lines):
            m = OBSERVER.search(ln)
            if not m or ln.rstrip().endswith(";"):
                continue
            # the body: from the first "{" (this line or the next) to its matching "}"
            j, depth, body, started = i, 0, [], False
            while j < len(lines):
                for ch in lines[j]:
                    if ch == "{":
                        depth += 1
                        started = True
                    elif ch == "}":
                        depth -= 1
                if started:
                    body.append((j + 1, lines[j]))
                if started and depth == 0:
                    break
                if not started and j > i + 1:
                    break
                j += 1
            if started:
                out.append((p, m.group(1), m.group(2), body))
    return out


def violations(files=None):
    files = sources() if files is None else files
    names = mapped_names(files)
    mapped = r"(?:PA(?:_RT)?\(|\b(?:%s)\b)" % ("|".join(map(re.escape, sorted(names))) or "$^")
    bad = []
    for p, fn, var, body in observers(files):
        v = re.escape(var)
        cmp_ = re.compile(r"\b%s\b\s*(?:==|!=)\s*%s|%s[^;&|?]*?\)?\s*(?:==|!=)\s*\b%s\b" % (v, mapped, mapped, v))
        switched = any(re.search(r"\bswitch\s*\(\s*%s\s*\)" % v, code) for _, code in body)
        for n, code in body:
            if cmp_.search(code) or (switched and re.search(r"\bcase\s+%s" % mapped, code)):
                bad.append(f"{os.path.relpath(p, ROOT)}:{n}: {fn}(): its id `{var}` against a mapped address: "
                           f"{code.strip()}")
    return bad


class Rule(unittest.TestCase):
    def test_observers_compare_their_ids_with_v100_literals(self):
        bad = violations()
        self.assertFalse(bad, "an observer's id compared through PA()/PA_RT() -- compare it with v1.00's literal (the "
                              "id the lift gives it):\n" + "\n".join(bad))

    def test_the_observers_are_found(self):
        found = {fn for _, fn, _, _ in observers(sources())}
        for fn in ("devtools_entry", "ppfx_rec_exit", "ppu_exit_observe", "r7_watch"):
            self.assertIn(fn, found)

    def test_the_113f3f1_bug_is_caught(self):
        """the developer tools before 113f3f1"""
        p = os.path.join(PORT, "devtools", "devtools.cpp")
        before = ["const uint32_t fn_damage_hp = PA_RT(0x0011A94C);   /* CPlayerActor::damageHP */",
                  'extern "C" void devtools_entry(uint32_t addr, ppu_context* ctx)', "{",
                  "    if (addr != fn_damage_hp) return;", "    const uint32_t p = (uint32_t)ctx->gpr[3];", "}"]
        self.assertEqual(len(violations([(p, before)])), 1)
        after = list(before)
        after[3] = "    if (addr != 0x0011A94Cu) return;"
        self.assertEqual(violations([(p, after)]), [])

    def test_other_shapes(self):
        p = os.path.join(PORT, "x.cpp")
        for body, n in [("    if (PA(0x00100000u) == id) go();", 1),           # the mapped address on the left
                        ("    switch (id) {\n    case PA(0x00100000u): go(); break;\n    }", 1),
                        ("    if (id == 0x00100000u) go();", 0),                # the literal: right
                        ("    if ((uint32_t)ctx->lr == PA(0x00100004u)) go();", 0)]:   # a guest read: not an id
            lines = ["void obs(uint32_t id, ppu_context* ctx)", "{"] + body.split("\n") + ["}"]
            self.assertEqual(len(violations([(p, lines)])), n, body)


if __name__ == "__main__":
    unittest.main()

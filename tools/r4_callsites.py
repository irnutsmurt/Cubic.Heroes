#!/usr/bin/env python3
"""R4's static check of the bypassed calls' callers (docs/NATIVE_R4.md).

R4 skips the bodies of the libgcm entry points listed in BYPASSED for PPFX's
calls (the recorder's r4_class); a skipped call leaves every register as the
caller had it. The PowerPC ABI lets a caller rely only on r3/r4 and f1-f4 as
results and on the non-volatile registers after a call; the volatile ones
(r0, r3-r12, f0-f13, v0-v19, CTR, XER, CR0, CR1, CR5-CR7) are dead. This
checks that on the lifted code itself: for every call of a bypassed entry
point from PPFX's code or libgcm's, the registers the caller reads after the
call before writing them -- along every path, up to the next call or the
function's return. A volatile register read there would be a dependency the
skip changes. r3 is reported apart: at a return it is the caller's own
result (the dynamic check, DOTHEROES_PPFX_BYPASS=verify, shows whether the
body leaves it as it was).

  tools/r4_callsites.py [build/r1/gen] > build/r4/callsites.txt
"""
import collections
import glob
import os
import re
import sys

BYPASSED = {
    0x0069E6E0, 0x0069E7EC, 0x0069E99C, 0x0069F19C, 0x0069F2B8, 0x0069FC38, 0x006A1E28, 0x006A56BC, 0x006A5EC0,
    0x006A5FE0, 0x006A6070, 0x006A6100, 0x006A62B4, 0x006A6344, 0x006A650C, 0x006A659C, 0x006A662C, 0x006A66BC,
    0x006A6810, 0x006A68A0, 0x006A6DA0, 0x006A6E30, 0x006A6F74, 0x006A7004, 0x006A7124, 0x006A71B4, 0x006A7244,
    0x006A7364, 0x006A75D8, 0x006A7674, 0x006A7744, 0x006A7834, 0x006A7BA4, 0x006A7CF8, 0x006A7EB0, 0x006A90EC,
    0x006A96E8, 0x006A5C30, 0x006A5CF4,
}
PPFX = (0x007497E0, 0x007D3E38)
GCM = (0x0069C000, 0x006B0000)

FUNC_RE = re.compile(r"^(?:static )?void (func_[0-9A-F]{8})(__body)?\(ppu_context\* ctx\) \{")
CALL_RE = re.compile(r"ctx->lr = 0x([0-9A-F]{8}); (func_([0-9A-F]{8}))\(ctx\);")
ANYCALL_RE = re.compile(r"\bfunc_[0-9A-F]{8}\(ctx\)|ps3_indirect_call|ps3_hle_call|g_trampoline_fn")
LABEL_RE = re.compile(r"^(loc_[0-9A-F]{8}):$")
GOTO_RE = re.compile(r"goto (loc_[0-9A-F]{8});")
REG_RE = re.compile(r"ctx->(gpr|fpr|vr)\[(\d+)\]|ctx->(ctr|lr)\b")
ASSIGN_RE = re.compile(r"ctx->(gpr|fpr|vr)\[(\d+)\]\s*=(?!=)")
CR_READ_RE = re.compile(r"\(ctx->cr >> \(?(\d+)")
CR_READ2_RE = re.compile(r"\(ctx->cr >> \(31 - (\d+)\)\)")
CR_WRITE_RE = re.compile(r"ctx->cr = \(ctx->cr & ~\(0xFu << (\d+)\)\)")
CR_WRITE2_RE = re.compile(r"ctx->cr = \(ctx->cr & ~\(1u << \(31 - (\d+)\)\)\)")


def volatile(name):
    k, n = name
    if k == "gpr":
        return n == 0 or 3 <= n <= 12
    if k == "fpr":
        return n <= 13
    if k == "vr":
        return n <= 19
    if k == "cr":
        return n in (0, 1, 5, 6, 7)
    return k == "ctr"


def stmt_effects(line):
    """(reads, writes) of one lifted line, as (kind, n) pairs."""
    writes = set()
    for m in ASSIGN_RE.finditer(line):
        writes.add((m.group(1), int(m.group(2))))
    for m in CR_WRITE_RE.finditer(line):
        writes.add(("cr", 7 - int(m.group(1)) // 4))
    for m in CR_WRITE2_RE.finditer(line):
        pass   # a single bit: not a whole field
    if "ctx->ctr = " in line:
        writes.add(("ctr", 0))
    reads = set()
    # every register mentioned that is not only the left side of its own assignment
    rhs = ASSIGN_RE.sub("", line)
    rhs = re.sub(r"ctx->cr = \(ctx->cr & ~\(0xFu << \d+\)\)", "", rhs)
    for m in REG_RE.finditer(rhs):
        if m.group(1):
            reads.add((m.group(1), int(m.group(2))))
        elif m.group(3) == "ctr" and "ctx->ctr = " not in line:
            reads.add(("ctr", 0))
    for m in CR_READ2_RE.finditer(line):
        reads.add(("cr", int(m.group(1)) // 4))
    for m in CR_READ_RE.finditer(line):
        reads.add(("cr", 7 - int(m.group(1)) // 4))
    return reads, writes


def analyse(lines, start, labels, depth=0, killed=None, seen=None):
    """Volatile registers read before written from lines[start:], following
    gotos, until a call or a return. Returns the set and whether a return
    with r3 untouched was reached."""
    killed = set(killed or ())
    seen = seen if seen is not None else set()
    found = set()
    ret_r3 = False
    i = start
    steps = 0
    while i < len(lines) and steps < 400:
        steps += 1
        ln = lines[i].strip()
        if (i, frozenset(killed)) in seen:
            return found, ret_r3
        seen.add((i, frozenset(killed)))
        if FUNC_RE.match(ln) or ln == "}":
            return found, ret_r3
        if LABEL_RE.match(ln) or not ln:
            i += 1
            continue
        reads, writes = stmt_effects(ln)
        for r in reads:
            if volatile(r) and r not in killed:
                found.add(r)
        killed |= writes
        if "return;" in ln and "g_trampoline_fn" not in ln:
            if ("gpr", 3) not in killed:
                ret_r3 = True
            return found, ret_r3
        if ANYCALL_RE.search(ln):
            return found, ret_r3   # another call: every volatile register is the callee's
        g = GOTO_RE.search(ln)
        if g:
            tgt = labels.get(g.group(1))
            if ln.startswith("if"):
                if tgt is not None and depth < 24:
                    f2, r2 = analyse(lines, tgt, labels, depth + 1, killed, seen)
                    found |= f2
                    ret_r3 |= r2
                i += 1
                continue
            if tgt is None:
                return found, ret_r3
            i = tgt
            continue
        i += 1
    return found, ret_r3


TAIL_RE = re.compile(r"g_trampoline_fn = \(void\(\*\)\(void\*\)\)func_([0-9A-F]{8}); return;")


def tail_callers(gen):
    """libgcm functions that end in a tail call of a bypassed entry point:
    the callee's results are theirs (a wrapper's), so their callers are
    checked too."""
    out = {}
    for path in sorted(glob.glob(os.path.join(gen, "ppu_recomp_*.cpp"))):
        cur = None
        for ln in open(path, encoding="utf-8", errors="replace"):
            m = FUNC_RE.match(ln)
            if m:
                cur = int(m.group(1)[5:], 16)
                continue
            t = TAIL_RE.search(ln)
            if t and cur is not None and GCM[0] <= cur < GCM[1] and int(t.group(1), 16) in BYPASSED:
                out[cur] = int(t.group(1), 16)
    return out


def main():
    gen = sys.argv[1] if len(sys.argv) > 1 else "build/r1/gen"
    sites = []
    tails = tail_callers(gen)
    for w, leaf in sorted(tails.items()):
        print("# %08X tail-calls bypassed %08X: its callers are checked as that entry point's" % (w, leaf))
    BYPASSED.update(tails)
    for path in sorted(glob.glob(os.path.join(gen, "ppu_recomp_*.cpp"))):
        lines = open(path, encoding="utf-8", errors="replace").read().split("\n")
        func, fstart, labels = None, 0, {}
        bodies = []
        for i, ln in enumerate(lines):
            m = FUNC_RE.match(ln)
            if m:
                bodies.append((m.group(1), i))
        for bi, (name, s0) in enumerate(bodies):
            e0 = bodies[bi + 1][1] if bi + 1 < len(bodies) else len(lines)
            addr = int(name[5:], 16)
            if not (PPFX[0] <= addr < PPFX[1] or GCM[0] <= addr < GCM[1]):
                continue
            labels = {}
            for i in range(s0, e0):
                m = LABEL_RE.match(lines[i].strip())
                if m:
                    labels[m.group(1)] = i
            for i in range(s0, e0):
                m = CALL_RE.search(lines[i])
                if not m:
                    continue
                callee = int(m.group(3), 16)
                if callee not in BYPASSED:
                    continue
                found, ret_r3 = analyse(lines, i + 1, labels)
                sites.append((callee, addr, int(m.group(1), 16), found, ret_r3))
    by = collections.defaultdict(list)
    for s in sites:
        by[s[0]].append(s)
    bad = 0
    print("# r4_callsites: calls of bypassed entry points from PPFX's and libgcm's code, and the volatile "
          "registers each caller reads after the call before writing them (r3 apart)")
    for callee in sorted(by):
        ss = by[callee]
        reads = collections.Counter()
        r3 = 0
        ret = 0
        for _, caller, lr, found, ret_r3 in ss:
            for r in found:
                if r == ("gpr", 3):
                    r3 += 1
                else:
                    reads["%s%d" % r] += 1
            ret += ret_r3
        bad += sum(reads.values())
        print("%08X sites %d | volatile read after the call: %s | r3 read: %d | returns with r3 from it: %d" % (
            callee, len(ss), ", ".join("%s at %d" % kv for kv in sorted(reads.items())) or "none", r3, ret))
        for _, caller, lr, found, ret_r3 in ss:
            other = sorted("%s%d" % r for r in found if r != ("gpr", 3))
            if other or ("gpr", 3) in found or ret_r3:
                print("   from %08X (lr %08X): reads %s%s" % (caller, lr, ", ".join(other) or "-",
                                                             " r3" if ("gpr", 3) in found else "") +
                      (" ; returns r3" if ret_r3 else ""))
    print("# total sites %d; sites reading a volatile register other than r3: %d" % (len(sites), bad))
    # r3 passed on: a caller that returns with the bypassed call's r3 makes it
    # its own result -- follow it to that caller's callers, until no one reads it
    passing = {caller for _, caller, _, _, ret_r3 in sites if ret_r3}
    seen = set()
    while passing - seen:
        cur = passing - seen
        seen |= cur
        found_more = set()
        for path in sorted(glob.glob(os.path.join(gen, "ppu_recomp_*.cpp"))):
            lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
            bodies = [(m.group(1), i) for i, ln in enumerate(lines) for m in [FUNC_RE.match(ln)] if m]
            for bi, (name, s0) in enumerate(bodies):
                e0 = bodies[bi + 1][1] if bi + 1 < len(bodies) else len(lines)
                labels = {}
                for i in range(s0, e0):
                    m = LABEL_RE.match(lines[i].strip())
                    if m:
                        labels[m.group(1)] = i
                for i in range(s0, e0):
                    m = CALL_RE.search(lines[i])
                    if not m or int(m.group(3), 16) not in cur:
                        continue
                    found, ret_r3 = analyse(lines, i + 1, labels)
                    caller = int(name[5:], 16)
                    print("# r3 of %08X (passed on from a bypassed call) at its call from %08X (lr %s): %s%s" % (
                        int(m.group(3), 16), caller, m.group(1),
                        "read" if ("gpr", 3) in found else "not read", "; returned again" if ret_r3 else ""))
                    if ret_r3:
                        found_more.add(caller)
        passing |= found_more


if __name__ == "__main__":
    main()

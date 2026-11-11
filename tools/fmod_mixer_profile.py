#!/usr/bin/env python3
"""Where the mixer's time goes, the lifted mixer against the port (docs/AUDIO.md step A3): the samples of
fmod_mixer_check's --profile N --sample PREFIX, named from the checker's .pdb (the system's dbghelp.dll, as
tools/symbolize.py does) and gathered by SPU function (a lifted piece counts for the function the generator puts
it in), and for the port by the kind of source line in fmod_mixer.cpp the sample is on.

    py -3 tools/fmod_mixer_profile.py <fmod_mixer_prof_*.exe> PREFIX [--top N]

PREFIX_lift.txt / PREFIX_port.txt: '# ... us a block X' then 'rva count' lines. Windows only.
"""
import collections
import ctypes
import ctypes.wintypes as wt
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_spu_native as G   # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIFT = os.path.join(ROOT, 'build/gen/spu/spu_0005_at_00F86A80/spu_recomp.c')
ELF = os.path.join(ROOT, 'build/gen/spu/images/spu_0005_at_00F86A80.elf')
CFG = os.path.join(ROOT, 'tools/fmod_mixer_config.py')
PORT = os.path.join(ROOT, 'port/native/fmod_mixer.cpp')


class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.ULONG), ("TypeIndex", wt.ULONG), ("Reserved", ctypes.c_uint64 * 2),
                ("Index", wt.ULONG), ("Size", wt.ULONG), ("ModBase", ctypes.c_uint64), ("Flags", wt.ULONG),
                ("Value", ctypes.c_uint64), ("Address", ctypes.c_uint64), ("Register", wt.ULONG),
                ("Scope", wt.ULONG), ("Tag", wt.ULONG), ("NameLen", wt.ULONG), ("MaxNameLen", wt.ULONG),
                ("Name", ctypes.c_char * 1024)]


class IMAGEHLP_LINE64(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.DWORD), ("Key", ctypes.c_void_p), ("LineNumber", wt.DWORD),
                ("FileName", ctypes.c_char_p), ("Address", ctypes.c_uint64)]


class Symbols:
    def __init__(self, exe):
        self.dbg = ctypes.WinDLL("dbghelp")
        self.proc = wt.HANDLE(0x4321)
        self.dbg.SymSetOptions(0x00000002 | 0x00000010)   # SYMOPT_UNDNAME | SYMOPT_LOAD_LINES
        if not self.dbg.SymInitialize(self.proc, os.path.dirname(exe).encode(), False):
            sys.exit("SymInitialize failed")
        self.dbg.SymLoadModuleEx.restype = ctypes.c_uint64
        self.dbg.SymLoadModuleEx.argtypes = [wt.HANDLE, wt.HANDLE, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint64,
                                             wt.DWORD, ctypes.c_void_p, wt.DWORD]
        self.base = 0x140000000
        if not self.dbg.SymLoadModuleEx(self.proc, None, exe.encode(), None, self.base, 0, None, 0):
            sys.exit("SymLoadModuleEx failed")

    def name(self, rva):
        si = SYMBOL_INFO()
        si.SizeOfStruct = 88
        si.MaxNameLen = 1024
        disp = ctypes.c_uint64(0)
        if self.dbg.SymFromAddr(self.proc, ctypes.c_uint64(self.base + rva), ctypes.byref(disp), ctypes.byref(si)):
            return si.Name.decode(errors="replace")
        return '?'

    def line(self, rva):
        ln = IMAGEHLP_LINE64()
        ln.SizeOfStruct = ctypes.sizeof(IMAGEHLP_LINE64)
        d2 = wt.DWORD(0)
        if self.dbg.SymGetLineFromAddr64(self.proc, ctypes.c_uint64(self.base + rva), ctypes.byref(d2), ctypes.byref(ln)):
            return os.path.basename(ln.FileName.decode(errors="replace")), ln.LineNumber
        return None, 0


def load(path):
    us, rows = 0.0, []
    for l in open(path):
        if l.startswith('#'):
            m = re.search(r'us a block ([0-9.]+)', l)
            us = float(m.group(1)) if m else 0.0
            continue
        a, n = l.split()
        rows.append((int(a, 16), int(n)))
    return us, rows


def classify_port_line(text):
    """The kind of statement a sample of the port is on."""
    t = text.strip()
    if t.startswith('Q r') or t.startswith('u128* const g'):
        return 'prologue (registers loaded from the file)'
    if 'SAVE();' in t and ('(c);' in t or 'branch(' in t) and 'LOAD();' in t:
        return 'a call (SAVE, the call, LOAD)'
    if 'SAVE();' in t:
        return 'a return or exit (SAVE)'
    if 'lsr(c,' in t or 'lsw(c,' in t:
        return 'local store (lsr / lsw)'
    if 'rdch(' in t or 'wrch(' in t or 'rchcnt(' in t:
        return 'channel (the call to the host)'
    if t.startswith('if (') or t.startswith('goto') or t.startswith('case ') or t.startswith('switch'):
        return 'a branch'
    if ' = spu_' in t:
        return 'an instruction (spu_helpers.h)'
    if t.startswith('L_') or t.startswith('COV('):
        return 'a label'
    return 'other: ' + t[:40]


def main():
    exe = sys.argv[1]
    prefix = sys.argv[2]
    top = int(sys.argv[sys.argv.index('--top') + 1]) if '--top' in sys.argv else 25
    sym = Symbols(os.path.abspath(exe))
    cfg = G.load_config(CFG)
    funcs = G.parse(LIFT, cfg.PREFIX)
    image, entry = G.load_image(ELF)
    gen = G.Gen(cfg, funcs, image, entry)
    port_lines = open(PORT, encoding='utf-8').read().split('\n')
    res = {}
    for side in ('lift', 'port'):
        us, rows = load(prefix + '_' + side + '.txt')
        total = sum(n for _, n in rows) or 1
        byf = collections.Counter()
        kinds = collections.Counter()
        names = {}
        for rva, n in rows:
            nm = names.get(rva)
            if nm is None:
                nm = names[rva] = sym.name(rva)
            m = re.search(r'spu_func_([0-9A-F]+)$', nm)
            m2 = re.search(r'\bf_([0-9A-F]{5})$', nm)
            if m:
                b = int(m.group(1), 16)
                key = 'SPU %05X' % gen.owner[b][0] if b in gen.owner else 'lifted %05X' % b
            elif m2:
                key = 'SPU %s' % m2.group(1)
                f, ln = sym.line(rva)
                if f == 'fmod_mixer.cpp' and 0 < ln <= len(port_lines):
                    kinds[classify_port_line(port_lines[ln - 1])] += n
                else:
                    kinds['(no line: %s)' % f] += n
            else:
                key = nm
            byf[key] += n
        res[side] = (us, total, byf, kinds)
    lu, lt, lf, _ = res['lift']
    pu, pt, pf, pk = res['port']
    print('the mixer\'s code a block: the lifted mixer %.1f us (%d samples), the port %.1f us (%d samples)' % (lu, lt, pu, pt))
    print('%-40s %8s %8s %8s' % ('function', 'lift us', 'port us', 'delta'))
    keys = sorted(set(lf) | set(pf), key=lambda k: -(pf[k] * pu / pt - lf[k] * lu / lt))
    shown = 0
    for k in sorted(set(lf) | set(pf), key=lambda k: -max(pf[k] * pu / pt, lf[k] * lu / lt)):
        if shown >= top:
            break
        shown += 1
        a, b = lf[k] * lu / lt, pf[k] * pu / pt
        print('%-40s %8.2f %8.2f %+8.2f' % (k[:40], a, b, b - a))
    print('largest losses:', ', '.join('%s %+.2f' % (k, pf[k] * pu / pt - lf[k] * lu / lt) for k in keys[:8]))
    ksum = sum(pk.values()) or 1
    print('the port\'s samples in its own functions, by the kind of source line (us a block):')
    for k, n in pk.most_common():
        print('  %-50s %6.2f  (%4.1f%%)' % (k[:50], n * pu / pt, 100.0 * n / ksum))
    if '--lines' in sys.argv:
        nl = int(sys.argv[sys.argv.index('--lines') + 1])
        _, rows = load(prefix + '_port.txt')
        byline = collections.Counter()
        for rva, n in rows:
            f, ln = sym.line(rva)
            if f == 'fmod_mixer.cpp':
                byline[ln] += n
        print('the port\'s hottest lines of fmod_mixer.cpp (us a block):')
        for ln, n in byline.most_common(nl):
            print('  %6d %6.2f  %s' % (ln, n * pu / pt, port_lines[ln - 1].strip()[:110]))


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Generates a native C++ port of a lifted SPU image: every statement the lift's own, the lifted
functions put back into the image's real functions, the runtime's transfers made gotos, calls and
returns, and everything that leaves the SPU made an explicit callback.

    py -3 gen_spu_native.py <config.py> <lifted spu_recomp.c> <image .elf> <template .cpp.in> <output .cpp>
                            [--report <file>]

A generalisation of gen_sfxreverb.py (A2) for an image that runs as a thread: stop-and-signal,
mailboxes, the decrementer, atomics, DMA of every kind (all of them channel operations), indirect calls
and jumps. The lift (ps3recomp's spu_lifter.py) cuts the image into one C function per branch target that
reach each other through the runtime (SPU_DIRECT tail calls, the drain loop, ctx). This puts them back
together: a C++ function per SPU function (each entry: the ELF entry, every brsl target, every address the
configuration names as taken), each lifted function a labelled block in it, and rewrites each statement for
the native interface (the template's header):

  ctx->gpr[N]                       -> rN, a local of the function, loaded from the register file at entry;
                                       around a call only the registers the callee can reach are stored and
                                       only those it can change reloaded (Gen.effects); every assigned one is
                                       stored at a return, exit, stop, tail jump and branch to the host (so the
                                       file is, there, what the lift leaves in ctx->gpr)
  spu_ls_read128 / write128         -> lsr / lsw (the same address masking)
  SPU_RELOC(x)                      -> (d + x) (relocatable lifts)
  spu_wrch / spu_rdch / spu_rchcnt  -> wrch / rdch / rchcnt (the host's channels)
  SPU_DIRECT to a block of the same function -> goto L_<address>
  SPU_DIRECT to another function's entry     -> a tail call ([[clang::musttail]])
  brsl $r0 (the lift's host call)   -> a call, then the lift's drain check: the callee returned to the link
  brsl $rN, N != 0                  -> rN = the link; goto
  bisl $r0, $rN (indirect call)     -> the dispatcher: a function the port has, else the host's branch
                                       callback (an overlay, or code the port cannot place), then the return check
  bi $rN (indirect jump)            -> a jump table (configured): a switch of gotos over the table's targets, a
                                       target outside it an exit (unresolved: the image's text was changed);
                                       else jump_to, a tail call into a function the port has or the host's
                                       branch callback
  SPU_RET                           -> return (c.ret = r0 word 0, the pc the lift's SPU_RET sets)
  the HALT-ASSERT (heqi)            -> an exit (halted, the pc)
  stop                              -> the host's stop callback; the lift goes on only past a stop it has
                                       `if (ctx->status != SPU_STATUS_RUNNING) return;` after, else an exit
  spu_unsupported (.word)           -> the host's note, then on as the lift does
  `loc_` labels, gotos              -> L_<address>

Every other statement (the instruction helpers of spu_helpers.h) is kept as it is, in the lift's order.
Fails on any statement it does not recognise.
"""
import importlib.util
import re
import struct
import sys

R_REG = re.compile(r'ctx->gpr\[(\d+)\]')
R_RELOC = re.compile(r'SPU_RELOC\((0x[0-9A-F]+)\)')
ADDR = r'(?:SPU_RELOC\()?(0x[0-9A-F]+)\)?'


def load_config(path):
    spec = importlib.util.spec_from_file_location('cfg', path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def load_image(path):
    """The ELF's loadable bytes at their addresses (a 256 KB local store)."""
    d = open(path, 'rb').read()
    if d[:4] != b'\x7fELF':
        raise SystemExit(path + ': not an ELF')
    phoff, = struct.unpack('>I', d[28:32])
    phnum, = struct.unpack('>H', d[44:46])
    ls = bytearray(0x40000)
    for i in range(phnum):
        typ, off, vaddr, paddr, filesz, memsz, flags, align = struct.unpack('>8I', d[phoff + 32 * i:phoff + 32 * i + 32])
        if typ == 1:
            ls[vaddr:vaddr + filesz] = d[off:off + filesz]
    entry, = struct.unpack('>I', d[24:28])
    return ls, entry


def word(ls, a):
    return struct.unpack('>I', bytes(ls[a:a + 4]))[0]


def parse(path, prefix):
    src = open(path, encoding='utf-8').read()
    pat = re.compile(r'void ' + prefix + r'([0-9A-F]+)\(spu_context\* ctx\) \{\n(.*?)\n\}\n', re.S)
    return {int(m.group(1), 16): [l.strip() for l in m.group(2).split('\n')] for m in pat.finditer(src)}


# ---- one statement of the lift, classified ---------------------------------------------------------
def classify(l, P):
    """(kind, fields) for one statement."""
    if not l or l.startswith('const uint32_t _rl = ctx->ls_reloc'):
        return ('skip',)
    if re.fullmatch(r'/\*.*\*/;?', l):
        return ('skip',)                                   # nop, lnop, branch hints
    m = re.fullmatch(r'loc_([0-9A-F]+):', l)
    if m:
        return ('label', int(m.group(1), 16))
    m = re.fullmatch(r'(?:(if \(.*\)) )?goto loc_([0-9A-F]+);', l)
    if m:
        return ('goto', m.group(1), int(m.group(2), 16))
    # brsl $r0: a nested host call, then the drain
    m = re.fullmatch(r'ctx->gpr\[0\] = spu_link\(' + ADDR + r'\); \{ int32_t _si = \(int32_t\)ctx->image_id; '
                     r'ctx->host_depth\+\+; ' + P + r'([0-9A-F]+)\(ctx\); spu_drain_call\(ctx, ' + ADDR + r'\); '
                     r'ctx->host_depth--; spu_img_restore\(ctx, _si\);(?: ctx->ls_reloc = _rl;)? \}', l)
    if m:
        assert m.group(1) == m.group(3), l
        return ('call', int(m.group(1), 16), int(m.group(2), 16), 'SPU_RELOC' in l)
    # bisl $r0, $rN: an indirect call
    m = re.fullmatch(r'\{ uint32_t _tg = ctx->gpr\[(\d+)\]\._u32\[0\]; ctx->gpr\[0\] = spu_link\(' + ADDR + r'\); '
                     r'int32_t _si = \(int32_t\)ctx->image_id; ctx->pc = _tg; ctx->host_depth\+\+; '
                     r'spu_indirect_branch\(ctx\); spu_drain_call\(ctx, ' + ADDR + r'\); ctx->host_depth--; '
                     r'spu_img_restore\(ctx, _si\);(?: ctx->ls_reloc = _rl;)? \}', l)
    if m:
        assert m.group(2) == m.group(3), l
        return ('icall', int(m.group(1)), int(m.group(2), 16), 'SPU_RELOC' in l)
    # bi $rN: an indirect jump
    m = re.fullmatch(r'ctx->pc = ctx->gpr\[(\d+)\]\._u32\[0\]; g_spu_trampoline_fn = spu_indirect_branch; return;', l)
    if m:
        return ('ijump', int(m.group(1)))
    m = re.fullmatch(r'ctx->pc = (0x[0-9A-F]+); g_spu_trampoline_fn = spu_indirect_branch; return;', l)
    if m:
        return ('xjump', int(m.group(1), 16))
    # brsl $rN (N != 0): a jump with a link
    m = re.fullmatch(r'ctx->gpr\[(\d+)\] = spu_link\(' + ADDR + r'\); \{ SPU_DIRECT\(ctx, ' + ADDR + r', '
                     + P + r'([0-9A-F]+)\); \}', l)
    if m:
        assert int(m.group(3), 16) == int(m.group(4), 16), l
        return ('linkjump', int(m.group(1)), int(m.group(2), 16), int(m.group(3), 16), 'SPU_RELOC' in l)
    m = re.fullmatch(r'(?:(if \(.*\)) )?\{ SPU_DIRECT\(ctx, ' + ADDR + r', ' + P + r'([0-9A-F]+)\); \}', l)
    if m:
        assert int(m.group(2), 16) == int(m.group(3), 16), l
        return ('direct', m.group(1), int(m.group(2), 16))
    m = re.fullmatch(r'(?:(if \(.*\)) )?\{? ?SPU_RET\(ctx\); ?\}?', l)
    if m:
        return ('ret', m.group(1))
    m = re.fullmatch(r'if \((.*)\) \{ extern void spu_halt\(spu_context\*\); '
                     r'fprintf\(stderr, "\[spu\] HALT-ASSERT (\w+) pc=0x([0-9A-F]+) img=%d\\n", ctx->image_id\); '
                     r'ctx->status = SPU_STATUS_STOPPED_BY_HALT; spu_halt\(ctx\); return; \}', l)
    if m:
        return ('halt', m.group(1), m.group(2), int(m.group(3), 16))
    m = re.fullmatch(r'ctx->stop_code = (0x[0-9A-F]+)u; ctx->status = SPU_STATUS_STOPPED_BY_STOP; spu_stop\(ctx\); '
                     r'(if \(ctx->status != SPU_STATUS_RUNNING\) )?return;', l)
    if m:
        return ('stop', int(m.group(1), 16), bool(m.group(2)))
    m = re.fullmatch(r'/\* TODO spu: (.*?) \*/ spu_unsupported\((0x[0-9A-F]+), "(.*)"\);', l)
    if m:
        return ('unsupported', int(m.group(2), 16), m.group(1))
    if l == 'spu_arch_fence();':
        return ('fence',)
    m = re.fullmatch(r'spu_ls_write128\(ctx, (.*), (ctx->gpr\[\d+\])\);', l)
    if m:
        return ('lsw', m.group(1), m.group(2))
    m = re.fullmatch(r'spu_wrch\(ctx, (\w+), (ctx->gpr\[\d+\])\);', l)
    if m:
        return ('wrch', m.group(1), m.group(2))
    m = re.fullmatch(r'(ctx->gpr\[\d+\]) = (spu_\w+\(.*\));', l)
    if m:
        return ('assign', m.group(1), m.group(2))
    return ('unknown', l)


def expr(e):
    """An expression of the lift, rewritten for the port."""
    e = R_REG.sub(lambda m: 'r' + m.group(1), e)
    e = R_RELOC.sub(lambda m: '(d + %su)' % m.group(1), e)
    e = e.replace('spu_ls_read128(ctx, ', 'lsr(c, ')
    e = e.replace('spu_rdch(ctx, ', 'rdch(c, ')
    e = e.replace('spu_rchcnt(ctx, ', 'rchcnt(c, ')
    if 'ctx' in e or re.search(r'\bSPU_(RELOC|DIRECT|RET|STATUS)', e):
        raise SystemExit('unexpected: ' + e)
    return e


def wrap_comment(text, lead, width=118):
    lines, cur = [], ''
    for w in text.split():
        if cur and len(lead) + len(cur) + 1 + len(w) > width:
            lines.append(lead + cur)
            cur = w
        else:
            cur = cur + ' ' + w if cur else w
    if cur:
        lines.append(lead + cur)
    return lines


class Gen:
    def __init__(self, cfg, funcs, image, image_entry):
        self.cfg = cfg
        self.P = cfg.PREFIX
        self.funcs = funcs
        self.image = image
        self.stmts = {a: [classify(l, self.P) for l in ls] for a, ls in funcs.items()}
        bad = [(a, s[1]) for a, ss in self.stmts.items() for s in ss if s[0] == 'unknown']
        if bad:
            raise SystemExit('\n'.join('0x%X: not recognised: %s' % b for b in bad[:20]))
        self.addrs = sorted(funcs)
        self.reloc = any('SPU_RELOC' in l for ls in funcs.values() for l in ls)
        # jump tables: the lifted function whose `bi` precedes each table
        self.tables = {}          # lifted function -> (table address, [targets])
        for t, n in cfg.JUMP_TABLES.items():
            owner = max(a for a in self.addrs if a <= t - 4)
            if not any(s[0] == 'ijump' for s in self.stmts[owner]):
                raise SystemExit('jump table 0x%X: no indirect jump in 0x%X' % (t, owner))
            targets = [word(image, t + 4 * i) for i in range(n)]
            for x in targets:
                if x not in funcs:
                    raise SystemExit('jump table 0x%X: entry 0x%X is no lifted function' % (t, x))
            self.tables[owner] = (t, targets)
        # the entries
        self.entries = {cfg.ENTRY}
        self.call_sites = {}      # entry -> set of callers (lifted function addresses)
        for a, ss in self.stmts.items():
            for s in ss:
                if s[0] == 'call':
                    self.entries.add(s[2])
                    self.call_sites.setdefault(s[2], set()).add(a)
        for a in cfg.ADDRESS_TAKEN:
            if a not in funcs:
                raise SystemExit('address-taken 0x%X is no lifted function' % a)
            self.entries.add(a)
        # functions: the closure of each entry over direct jumps, gotos and table targets, other entries
        # excluded (a jump to one is a tail call); only what the entries reach is generated
        self.blocks = {}
        changed = True
        while changed:
            changed = False
            for e in sorted(self.entries):
                if e in self.blocks:
                    continue
                bl = {e}
                st = [e]
                while st:
                    x = st.pop()
                    for y in self.jumps_of(x):
                        if y not in self.funcs:
                            raise SystemExit('0x%X jumps to 0x%X, no lifted function' % (x, y))
                        if y in self.entries and y != e:
                            continue
                        if y not in bl:
                            bl.add(y)
                            st.append(y)
                self.blocks[e] = sorted(bl)
        self.owner = {}
        for e in sorted(self.entries):
            for b in self.blocks[e]:
                self.owner.setdefault(b, []).append(e)
        self.effects()

    # ---- what each function does to the register file ------------------------------------------------------
    # A function holds the registers it names in locals (regs), loaded at its entry. The ones its own statements
    # assign (dirty) differ from the file until stored. The file is the one copy callees and the host see, so:
    #   before a call to G: store the dirty locals G can reach (acc[G]: the registers G and everything it calls or
    #     jumps to name; all of them where G can reach the host's branch callback, which runs any code on the
    #     file, or a stop, where the host -- and the checker, comparing every register at each block -- sees it);
    #   after it: reload the locals G can change (wr[G]: what G and its callees assign; all where it can reach a
    #     branch to the host);
    #   on the way out of G with c.exit set: the dirty locals not stored before the call (the rest is in the file);
    #   at a return, a halt, a stop, a tail jump, a branch to the host: every dirty local.
    # So the file is complete at every return, exit, stop and branch callback, as before; between them a
    # callee's registers pass through the file and the caller's stay in its locals.
    def effects(self):
        ALL = frozenset(range(128))
        self.regs, self.dirty, acc, wr, edges = {}, {}, {}, {}, {}
        for e in self.entries:
            blocks = self.blocks[e]
            self.regs[e] = frozenset({int(x) for b in blocks for l in self.funcs[b] for x in R_REG.findall(l)} | {0})
            dirty, host, stop, out = set(), False, False, set()
            for b in blocks:
                for s in self.stmts[b]:
                    k = s[0]
                    if k == 'assign':
                        dirty.add(int(R_REG.fullmatch(s[1]).group(1)))
                    elif k in ('call', 'icall'):
                        dirty.add(0)
                    elif k == 'linkjump':
                        dirty.add(s[1])
                    if k == 'call':
                        out.add(s[2])
                    if k in ('direct', 'linkjump'):
                        t = s[2] if k == 'direct' else s[3]
                        if t not in blocks:
                            out.add(t)
                    if k == 'icall' or k == 'xjump' or (k == 'ijump' and b not in self.tables):
                        host = True
                    if k == 'stop':
                        stop = True
            self.dirty[e] = frozenset(dirty)
            acc[e] = ALL if host or stop else self.regs[e]
            wr[e] = ALL if host else self.dirty[e]
            edges[e] = out
        changed = True
        while changed:
            changed = False
            for e in sorted(self.entries):
                a, w = set(acc[e]), set(wr[e])
                for t in edges[e]:
                    a |= acc[t]
                    w |= wr[t]
                if len(a) != len(acc[e]) or len(w) != len(wr[e]):
                    acc[e], wr[e] = frozenset(a), frozenset(w)
                    changed = True
        self.acc, self.wr = acc, wr

    def jumps_of(self, a):
        out = []
        for s in self.stmts[a]:
            if s[0] == 'direct':
                out.append(s[2])
            elif s[0] == 'linkjump':
                out.append(s[3])
        if a in self.tables:
            out.extend(self.tables[a][1])
        return out

    def fname(self, e):
        return 'f_%05X' % e

    def stmt_pc(self, a, pred):
        """The address of the first instruction word in lifted function a's span that satisfies pred."""
        nxt = min([x for x in self.addrs if x > a] + [a + 0x1000])
        for p in range(a, nxt, 4):
            if pred(word(self.image, p)):
                return p
        raise SystemExit('0x%X: instruction not found' % a)

    # ---- one function --------------------------------------------------------------------------------
    # Which of a function's locals may differ from the file (may be dirty) at each statement: a forward dataflow
    # over its blocks and the lift's labels and jumps (din: the set at each label). An assignment makes a register
    # dirty; storing it makes it clean, and so does reloading it. So each site stores only what may be dirty there:
    #   a call to G: the dirty ones G can reach (acc[G]); then reloads what G can change (wr[G]); the dirty ones
    #     outside acc[G] stay dirty (stored on the way out when G exits);
    #   a branch to the host, a stop: every dirty one (the host sees the whole file); after a branch every local is
    #     reloaded, after a stop the host went on past they all equal the file;
    #   a return, a halt, a tail jump, an exit: every dirty one.
    def gen_function(self, e, block_index, out):
        cfg = self.cfg
        blocks = self.blocks[e]
        regs = sorted(self.regs[e])
        desc = cfg.FUNCS.get(e, '')
        callers = sorted(self.call_sites.get(e, ()))
        how = []
        if e == cfg.ENTRY:
            how.append('the ELF entry')
        if e in cfg.ADDRESS_TAKEN:
            how.append('through a pointer: ' + cfg.ADDRESS_TAKEN[e])
        if callers:
            how.append('called from ' + ', '.join('%X' % x for x in callers[:8]) + (' ...' if len(callers) > 8 else ''))
        text = '---- 0x%05X%s (%d blocks; %s; %d registers, %d assigned; reaches %s, changes %s) ----' % (
            e, ' ' + desc if desc else '', len(blocks), '; '.join(how), len(regs), len(self.dirty[e]),
            'all' if len(self.acc[e]) == 128 else '%d' % len(self.acc[e]), 'all' if len(self.wr[e]) == 128 else '%d' % len(self.wr[e]))
        out.extend(wrap_comment(text, '// '))
        din = {e: frozenset()}
        for _ in range(1000):
            before = dict(din)
            for b in blocks:
                self.gen_block(e, b, blocks, din, None)
            if din == before:
                break
        else:
            raise SystemExit('0x%X: the dataflow does not settle' % e)
        out.append('void %s(Run& c)' % self.fname(e))
        out.append('{')
        out.append('    u128* const g = c.g;')
        if self.reloc:
            out.append('    const uint32_t d = c.d;')
            out.append('    (void)d;')
        decl = ['r%d = g[%d]' % (r, r) for r in regs]
        for i in range(0, len(decl), 8):
            out.append('    Q ' + ', '.join(decl[i:i + 8]) + ';')
        if blocks[0] != e:
            out.append('    goto L_%05X;' % e)
        body = []
        for b in blocks:
            if b in cfg.BLOCK_NOTES:
                body.extend(wrap_comment(cfg.BLOCK_NOTES[b], '    // '))
            body.append('L_%05X:' % b)
            body.append('    COV(%d);' % block_index[b])
            self.gen_block(e, b, blocks, din, body)
        out.extend(body)
        out.append('}')
        out.append('')

    @staticmethod
    def st(regs):
        return ''.join('g[%d] = r%d; ' % (r, r) for r in sorted(regs))

    @staticmethod
    def ld(regs):
        return ''.join(' r%d = g[%d];' % (r, r) for r in sorted(regs))

    def gen_block(self, e, b, blocks, din, out):
        """Block b of function e: its code appended to out, or (out None) only the dataflow (din updated)."""
        P = self.P
        ss = self.stmts[b]
        R = self.regs[e]
        emit = out.append if out is not None else (lambda x: None)
        empty = frozenset()

        def flow(target, cur):
            old = din.get(target)
            new = cur if old is None else old | cur
            if old is None or new != old:
                din[target] = frozenset(new)

        cur = din.get(b)
        for i, s in enumerate(ss):
            k = s[0]
            if k == 'skip':
                continue
            if k == 'label':
                if s[1] != b:
                    if cur is not None:
                        flow(s[1], cur)
                    cur = din.get(s[1])
                    emit('L_%05X:' % s[1])
                continue
            if cur is None:
                cur = empty   # after an unconditional transfer, before any label: code no path reaches
            if k == 'goto':
                cond = expr(s[1]) + ' ' if s[1] else ''
                flow(s[2], cur)
                emit('    %sgoto L_%05X;' % (cond, s[2]))
                if not s[1]:
                    cur = None
                continue
            if k == 'call':
                link, tgt = s[1], s[2]
                lk = '(d + 0x%Xu)' % link if s[3] else '0x%Xu' % link
                emit('    r0 = spu_link(%s);   // brsl $r0, 0x%X' % (lk, tgt))
                cur = cur | {0}
                before, rest, after = cur & self.acc[tgt], cur - self.acc[tgt], R & self.wr[tgt]
                ex = ('{ %sreturn; }' % self.st(rest)) if rest else 'return;'
                bad = '{ %sbad_return(c, %s); return; }' % (self.st(rest), lk)
                emit('    %s%s(c); if (c.exit) %s if ((c.ret ^ %s) & 0x3FFFFu) %s%s' % (
                    self.st(before), self.fname(tgt), ex, lk, bad, self.ld(after)))
                cur = rest
                continue
            if k == 'icall':
                n, link = s[1], s[2]
                lk = '(d + 0x%Xu)' % link if s[3] else '0x%Xu' % link
                site = link - 4
                cur = cur | {0}
                emit('    { const uint32_t t = r%d._u32[0]; r0 = spu_link(%s); %s'
                     'if (!branch(c, t & 0x3FFFFu, 0x%Xu, true)) return; '
                     'if ((c.ret ^ %s) & 0x3FFFFu) { bad_return(c, %s); return; }%s }   // bisl $r0, $r%d'
                     % (n, lk, self.st(cur), site, lk, lk, self.ld(R), n))
                cur = empty
                continue
            if k == 'ijump':
                n = s[1]
                site = self.stmt_pc(b, lambda w, n=n: (w >> 21) == 0x1A8 and (w >> 7) & 0x7F == n)   # bi $rN
                if b in self.tables:
                    t, targets = self.tables[b]
                    emit('    switch (r%d._u32[0] & 0x3FFFFu) {   // bi $r%d at 0x%X: the jump table at 0x%X' % (n, n, site, t))
                    for x in sorted(set(targets)):
                        flow(x, cur)
                        emit('    case 0x%05Xu: goto L_%05X;' % (x, x))
                    # a target outside the table (the image's text in the local store changed): an exit, not the host
                    emit('    default: { %sunresolved(c, r%d._u32[0] & 0x3FFFFu, 0x%Xu); return; }' % (self.st(cur), n, site))
                    emit('    }')
                else:
                    emit('    { %sc.jt = r%d._u32[0] & 0x3FFFFu; c.js = 0x%Xu; TAIL jump_to(c); }   // bi $r%d at 0x%X'
                         % (self.st(cur), n, site, n, site))
                cur = None
                continue
            if k == 'xjump':
                emit('    { %sc.jt = 0x%05Xu; c.js = 0x%Xu; TAIL jump_to(c); }   // a branch out of the image' % (self.st(cur), s[1], b))
                cur = None
                continue
            if k == 'linkjump':
                n, link, tgt = s[1], s[2], s[3]
                lk = '(d + 0x%Xu)' % link if s[4] else '0x%Xu' % link
                emit('    r%d = spu_link(%s);   // brsl $r%d' % (n, lk, n))
                cur = cur | {n}
                s = ('direct', None, tgt)
                k = 'direct'
            if k == 'direct':
                tgt = s[2]
                cond = expr(s[1]) + ' ' if s[1] else ''
                if tgt in blocks:
                    flow(tgt, cur)
                    if cond or out is None or out[-1] != '    goto L_%05X;' % tgt:
                        emit('    %sgoto L_%05X;' % (cond, tgt))
                else:
                    assert tgt in self.entries
                    emit('    %s{ %sTAIL %s(c); }   // a tail jump' % (cond, self.st(cur), self.fname(tgt)))
                if not s[1]:
                    cur = None
                continue
            if k == 'ret':
                cond = expr(s[1]) + ' ' if s[1] else ''
                emit('    %s{ %sc.ret = r0._u32[0]; return; }' % (cond, self.st(cur)))
                if not s[1]:
                    cur = None
                continue
            if k == 'halt':
                emit('    if (%s) { %shalt(c, 0x%Xu); return; }   // %s' % (expr(s[1]), self.st(cur), s[3], s[2]))
                continue
            if k == 'stop':
                code, cont = s[1], s[2]
                pc = self.stmt_pc(b, lambda w, code=code: w == code)
                if cont:
                    emit('    { %sif (!stop(c, 0x%Xu, 0x%Xu, 0x%Xu)) return; }   // stop 0x%X: on past it when the host '
                         'services it' % (self.st(cur), code, pc, b, code))
                    cur = empty
                else:
                    emit('    { %sstop_end(c, 0x%Xu, 0x%Xu, 0x%Xu); return; }   // stop 0x%X: the lift returns after it'
                         % (self.st(cur), code, pc, b, code))
                    cur = None
                continue
            if k == 'unsupported':
                emit('    unsupported(c, 0x%Xu);   // %s: the lift notes it and goes on' % (s[1], s[2]))
                continue
            if k == 'fence':
                emit('    spu_arch_fence();')
                continue
            if k == 'lsw':
                emit('    lsw(c, %s, %s);' % (expr(s[1]), expr(s[2])))
                continue
            if k == 'wrch':
                emit('    wrch(c, %s, %s);' % (s[1], expr(s[2])))
                continue
            if k == 'assign':
                emit('    %s = %s;' % (expr(s[1]), expr(s[2])))
                cur = cur | {int(R_REG.fullmatch(s[1]).group(1))}
                continue
            raise SystemExit('0x%X: %s' % (b, s))
        if cur is not None:
            raise SystemExit('0x%X: the block does not end in a transfer' % b)

    # ---- the report: entries, functions, reach -----------------------------------------------------------
    def report(self):
        cfg = self.cfg
        lines = []
        gen = sorted(set(b for e in self.entries for b in self.blocks[e]))
        orphans = [a for a in self.addrs if a not in self.owner]
        shared = {b: es for b, es in self.owner.items() if len(es) > 1}
        lines.append('%d lifted functions; %d entries (functions); %d blocks generated (%d in more than one function); '
                     '%d lifted functions reached by no entry' % (len(self.addrs), len(self.entries), len(gen), len(shared), len(orphans)))
        # reach from the ELF entry: calls, tail jumps, address-taken (through the dispatcher)
        reach, st = set(), [cfg.ENTRY]
        while st:
            e = st.pop()
            if e in reach:
                continue
            reach.add(e)
            for b in self.blocks[e]:
                for s in self.stmts[b]:
                    if s[0] == 'call':
                        st.append(s[2])
                    if s[0] in ('direct', 'linkjump'):
                        t = s[2] if s[0] == 'direct' else s[3]
                        if t in self.entries and t != e:
                            st.append(t)
                    if s[0] in ('icall', 'ijump'):
                        st.extend(cfg.ADDRESS_TAKEN)
        lines.append('functions reachable from the entry 0x%X (calls, tail jumps, and the pointer targets once an '
                     'indirect branch is reached): %d of %d' % (cfg.ENTRY, len(reach), len(self.entries)))
        lines.append('')
        lines.append('entry  blocks  lifted-lines  how reached')
        for e in sorted(self.entries):
            n = sum(len(self.funcs[b]) for b in self.blocks[e])
            how = []
            if e == cfg.ENTRY:
                how.append('ELF entry')
            if e in cfg.ADDRESS_TAKEN:
                how.append('pointer')
            if e in self.call_sites:
                how.append('called from %d sites' % len(self.call_sites[e]))
            tails = sorted(set(b for b in self.addrs if b in self.owner and any(
                (s[0] == 'direct' and s[2] == e) or (s[0] == 'linkjump' and s[3] == e) for s in self.stmts[b])
                and e not in self.blocks.get(self.owner[b][0], [])))
            if tails:
                how.append('tail-jumped to from ' + ' '.join('%X' % t for t in tails[:6]))
            lines.append('%05X  %5d  %6d  %s%s%s' % (e, len(self.blocks[e]), n, '; '.join(how), '' if e in reach else ' (not reached)',
                                                    ' -- ' + cfg.FUNCS[e] if e in cfg.FUNCS else ''))
        lines.append('')
        if shared:
            lines.append('blocks in more than one function (generated in each): ' +
                         ' '.join('%X(%s)' % (b, ','.join('%X' % x for x in es)) for b, es in sorted(shared.items())))
        lines.append('lifted functions reached by no entry (not generated): ' + ' '.join('%X' % a for a in orphans))
        kinds = {}
        for e in self.entries:
            for b in self.blocks[e]:
                for s in self.stmts[b]:
                    kinds[s[0]] = kinds.get(s[0], 0) + 1
        lines.append('statements generated by kind: ' + ', '.join('%s %d' % kv for kv in sorted(kinds.items())))
        sites = []
        for e in sorted(self.entries):
            for b in self.blocks[e]:
                for s in self.stmts[b]:
                    if s[0] == 'icall':
                        sites.append('bisl $r%d at %X (in %X)' % (s[1], s[2] - 4, e))
                    if s[0] == 'ijump':
                        sites.append('bi $r%d in %X (in %X)%s' % (s[1], b, e, ': table %X' % self.tables[b][0] if b in self.tables else ''))
                    if s[0] == 'xjump':
                        sites.append('branch to %X in %X (in %X)' % (s[1], b, e))
                    if s[0] == 'stop':
                        sites.append('stop 0x%X in %X (in %X)%s' % (s[1], b, e, ', continued' if s[2] else ''))
        lines.append('indirect branches and stops: ' + '; '.join(sites))
        return '\n'.join(lines) + '\n'


def main():
    args = sys.argv[1:]
    report = None
    if '--report' in args:
        i = args.index('--report')
        report = args[i + 1]
        del args[i:i + 2]
    cfgpath, lift, elf, template, output = args
    cfg = load_config(cfgpath)
    funcs = parse(lift, cfg.PREFIX)
    image, image_entry = load_image(elf)
    if image_entry != cfg.ENTRY:
        raise SystemExit('the ELF entry is 0x%X, the configuration says 0x%X' % (image_entry, cfg.ENTRY))
    g = Gen(cfg, funcs, image, image_entry)
    gen_blocks = sorted(set(b for e in g.entries for b in g.blocks[e]))
    block_index = {a: i for i, a in enumerate(gen_blocks)}
    out = []
    for e in sorted(g.entries):
        g.gen_function(e, block_index, out)
    stray = [a for a in cfg.BLOCK_NOTES if a not in block_index]
    if stray:
        raise SystemExit('notes for addresses that start no generated block: ' + ' '.join('%X' % a for a in stray))
    decls = ['void %s(Run& c);' % g.fname(e) for e in sorted(g.entries)]
    table = ['    { 0x%05Xu, 0x%05Xu },' % (a, g.owner[a][0]) for a in gen_blocks]
    cases = ['    case 0x%05Xu: %s(c); break;' % (e, g.fname(e)) for e in sorted(g.entries)]
    entries = ['    0x%05Xu,' % e for e in sorted(g.entries)]
    t = open(template, encoding='utf-8').read()
    t = t.replace('// @DECLS@', '\n'.join(decls))
    t = t.replace('// @FUNCTIONS@', '\n'.join(out).rstrip('\n'))
    t = t.replace('// @BLOCKS@', '\n'.join(table))
    t = t.replace('// @DISPATCH@', '\n'.join(cases))
    t = t.replace('// @JUMPS@', '\n'.join('    case 0x%05Xu: TAIL %s(c);' % (e, g.fname(e)) for e in sorted(g.entries)))
    t = t.replace('// @ENTRIES@', '\n'.join(entries))
    t = t.replace('@BLOCK_COUNT@', str(len(gen_blocks)))
    t = t.replace('@ENTRY_COUNT@', str(len(g.entries)))
    t = t.replace('@CODE_BEGIN@', '0x%05Xu' % cfg.CODE_BEGIN)
    t = t.replace('@CODE_END@', '0x%05Xu' % cfg.CODE_END)
    with open(output, 'w', encoding='utf-8', newline='\n') as f:
        f.write(t)
    rep = g.report()
    if report:
        with open(report, 'w', encoding='utf-8', newline='\n') as f:
            f.write(rep)
    print('%d lifted functions: %d blocks in %d functions; %d lines' % (len(funcs), len(gen_blocks), len(g.entries), t.count('\n')))
    print(rep.split('\n')[0])
    print(rep.split('\n')[1])


if __name__ == '__main__':
    main()

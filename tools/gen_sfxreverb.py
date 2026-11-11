#!/usr/bin/env python3
"""Generates fmod_sfxreverb.cpp from the lifted sfxreverb overlay.

    py -3 gen_sfxreverb.py <lifted spu_recomp.c> <template .cpp.in> <output .cpp>

The lift (build/gen/spu/spu_0024_at_00FC7180/spu_recomp.c) splits the image's
nine functions into 158 lifted functions, one per branch target, that reach
each other through the runtime (SPU_DIRECT tail calls, the drain loop, ctx).
This puts them back together: one C++ function per SPU function, each lifted
function a labelled block in it, and rewrites each statement for the native
interface (fmod_sfxreverb.h):

  ctx->gpr[N]                    -> rN, a local of the function, loaded from
                                    the register file at entry and after each
                                    call, stored back before each call, return
                                    and halt (so the file is, at every exit,
                                    what the lift leaves in ctx->gpr)
  spu_ls_read128 / write128      -> lsr / lsw (the same address masking)
  SPU_RELOC(x)                   -> (d + x), d the relocation delta
  spu_wrch / spu_rdch            -> wrch / rdch (the caller's channels)
  SPU_DIRECT to a block of the   -> goto L_<address>
    same function
  SPU_DIRECT to another function -> a tail call
  brsl $r0 (the lift's host call) -> a call, then `if (c.halt) return`
  SPU_RET                        -> return
  the HALT-ASSERT (heqi)         -> c.halt = <its pc>; return
  `loc_` labels, gotos           -> L_<address>

Every other statement (the instruction helpers of spu_helpers.h) is kept as
it is, in the lift's order. Fails on any statement it does not recognise.
"""
import re
import sys

PREFIX = 'spu_0024_at_00FC7180_spu_func_'

# The nine SPU functions: entry -> (name, one-line description). Their blocks
# are what each reaches through SPU_DIRECT without passing another entry.
FUNCS = {
    0x0090: ('f_0090', 'downmix(R, length, channels, src): the input to mono at R[0x410]'),
    0x0820: ('f_0820', 'the reverb: process(R, in, out, channels, length, float, dry, speaker mask)'),
    0x2C70: ('f_2C70', 'the descriptor body: desc+0x34 = read; returns the descriptor'),
    0x2CA8: ('f_2CA8', 'the ELF entry: a branch to the descriptor body'),
    0x2CB0: ('f_2CB0', 'static initialisation (initialize, priority): a list head at 0x2FEC'),
    0x2D20: ('f_2D20', 'the .ctors entry: (1, 0xFFFF) into 0x2CB0 (the mixer never runs it)'),
    0x2D30: ('f_2D30', 'read on `this`: the arguments rearranged for 0x820'),
    0x2D88: ('f_2D88', 'the read callback: the DSP state -> this (state - 28), then 0x2D30'),
    0x2D98: ('f_2D98', 'dma(lsa, {eah, eal}, size, tag, command): one MFC command, waits for its tag'),
}

# What some blocks do (the lift's split points; [C] read in the code, [H] the
# names). A comment before the block's label.
BLOCK_NOTES = {
    0x0098: 'r79 = the relocation delta (the brsl $r79 idiom); the mode is the channel count',
    0x0158: '1 channel: memmove(R[0x410], src, length * 4) -- forwards, 16 bytes at a time when both are aligned',
    0x01F8: '6 channels: each frame summed (4 frames a pass)',
    0x0428: '8 channels: each frame summed (4 frames a pass)',
    0x06DC: 'memmove backwards, a byte at a time (the destination inside the source)',
    0x0718: 'memmove, unaligned: through the 16 byte-shift patterns at .rodata 0x2E00',
    0x0818: 'length 0 (any other channel count)',
    0x0820: 'the prologue: an 18,736-byte frame (the line buffers), r80-r127 saved; R[0x410] = R rounded up to 16 '
            '(the mono buffer; its old value restored at the end); R+0x2A30 and R+0x2A20 = R + 0x2920 and R + 0x29C0 '
            '(whole quadwords); r110 = 1 / length; 8 channels: 0x2A80',
    0x09B8: 'out = in + 1.0e-15 or - (0x26901D7C, the sign alternating with the frame): a guard against denormals, '
            'channel by channel',
    0x0A88: 'downmix the guarded input (in `out`) to mono',
    0x0AD8: 'the 9 early taps (R+0x2D00 new, R+0x2D30 old): read positions in the main ring (mask R[0x2D80], '
            'write position R[0x2D90]); a changed tap -> R[0x2D60 + i] = 1 and the old position kept for a crossfade',
    0x0B8C: 'the 8 late lines (R+0x2B80 + 4j delay, R+0x2BA0 old, R+0x2BC0 second tap, R+0x2C10 mask, R+0x2C30 write '
            'position): read positions; a changed delay -> R[0x2C00 + j] = 1 (its old position read by DMA each sample); '
            'the per-line values compared with their targets (R+0x2A40 / 0x2A80, R+0x2870 / 0x2900)',
    0x0DA8: 'three smoothed values compared with their targets (R+0x2AD8 / 0x2AE8, R+0x2AD4 / 0x2AF8, R+0x2834 / '
            '0x2844): a change sets a flag and the block interpolates (0x2A38, 0x29F0, 0x29A8)',
    0x0E98: 'the LS buffers cleared: two 8,336-byte areas and 1,040 bytes of the frame, 9,232 bytes at R+0x420',
    0x0F60: 'the buffer tables on the stack: r1+128, r1+64 (8 x 1,040 bytes in the frame), r1+160 (9 x 1 KB at R+0x420)',
    0x1070: 'GETs, group A: each early tap\'s `length` samples from the main ring (EA R[0x2CB0]) into its 1 KB buffer; '
            'two GETs where the read wraps; tags from 0; halts on a tag past 31, an unaligned LSA / EA / size, '
            'or a size above 16 KB',
    0x1298: 'GETs, group B: the late lines in use (R[0x2B14]), `length` + 4 samples each from the line\'s ring '
            '(EA R[0x2B40 + 4j])',
    0x1550: 'wait for groups A and B; then GETs, group C (tags from 0 again): each output channel\'s second tap on '
            'its line',
    0x1808: 'wait for group C; length 0: straight to the PUTs (0x26F0)',
    0x18B0: 'the sample loop (r97 = the sample): the mono input through a one-pole lowpass twice (coefficient '
            'smoothed from R[0x2840] to R[0x283C]; states R+0x2848, R+0x284C)',
    0x1918: 'the second pole; then (0x197C) a biquad when R[0x2864] is not 0 (coefficients R+0x2850..0x2860, states '
            'R+0x2868, R+0x286C)',
    0x1A00: 'the filtered input into the main ring\'s new samples (the PUT buffer at r1+208, written backwards); the '
            'ring\'s write position and the taps\' positions move down a sample',
    0x1B40: 'early tap 0 times a gain (smoothed: R+0x2AF0..0x2AF8) through two allpasses (LS rings R+0x6E90 and '
            'R+0x7690, coefficient R[0x2B10], masks R+0x7EC0 / 0x7EC4, positions R+0x7ED0..0x7EE4) into a 4,096-float '
            'LS ring (R+0x2E00, mask R[0x6E50], position R[0x6E60]); its seven taps (R+0x6E70) onto the stack',
    0x1F10: 'each late line: input = early tap j+1 times a gain (smoothed: R+0x2AE0..0x2AE8); x = the line\'s output '
            'times R[0x2A60 + 4j] plus the input; times R[0x2AFC]; a one-pole lowpass per line (states at R[0x2A20] '
            '-> R+0x29C0, coefficient R+0x28A0 / 0x28E0)',
    0x2008: 'a line whose delay is changing: its old position\'s sample read from main memory, a 4-byte GET per '
            'sample (through 0x2D98), crossfaded with the new one',
    0x2150: 'a tap whose position is changing: the old position\'s sample by a 4-byte GET',
    0x21E8: 'the 8 lowpassed lines through three butterfly stages (coefficient R[0x2AC0], shuffles from .rodata '
            '0x2DE0 / 0x2DF0) into R+0x2920 and into the lines\' new samples (the group A buffers 0-7, backwards)',
    0x2388: 'each output channel\'s line tap: copied from this block\'s new samples where it falls inside them',
    0x2420: 'the same for the 8 lines\' main taps',
    0x24D0: 'the output, channel by channel: a channel not in the speaker mask keeps `in`; else the sum of the early '
            'taps on it plus its line taps (clamped to [-1, 1]) plus dry x in (dry = this[0x8074]; dry 0: no input); '
            '8 / channels lines a channel',
    0x26B0: 'the next sample',
    0x26F0: 'PUTs: the filtered input into the main ring at its write position (tag 0), each line\'s new samples into '
            'its ring (tags 1-8); wait; R[0x410] restored',
    0x2854: 'the epilogue',
    0x2930: 'a channel not in the speaker mask: out = in',
    0x295C: 'dry 0: out = the reverb only',
    0x2A80: '8 channels: the guard offset, 4 frames (32 floats) a pass; the group C flag at r1+17952 set',
    0x2B40: 'R+0x420 not 16-byte aligned: the clear through masks (0x2C44 / 0x2C50, for a length under 16, cannot run: '
            'the length is 9,232)',
    0x2BC8: 'r1+208 not 16-byte aligned: the clear through masks',
    0x2C70: 'r9 - r8 = the relocation delta',
    0x2CB0: 'only for (1, 0xFFFF): 0x2FEC = 0x2FF0 = 0x2FEC + delta, 0x2FF4 = 0 [H: an empty list]',
}


def parse(path):
    src = open(path, encoding='utf-8').read()
    funcs = {}
    pat = re.compile(r'void ' + PREFIX + r'([0-9A-F]+)\(spu_context\* ctx\) \{\n(.*?)\n\}\n', re.S)
    for m in pat.finditer(src):
        funcs[int(m.group(1), 16)] = m.group(2).split('\n')
    return funcs


def jumps_of(lines):
    out = set()
    for l in lines:
        for x in re.findall(r'SPU_DIRECT\(ctx, SPU_RELOC\(0x([0-9A-F]+)\)', l):
            out.add(int(x, 16))
    return out


RE_REG = re.compile(r'ctx->gpr\[(\d+)\]')
RE_RELOC = re.compile(r'SPU_RELOC\((0x[0-9A-F]+)\)')


def regs_of(lines):
    s = set()
    for l in lines:
        for x in RE_REG.findall(l):
            s.add(int(x))
    return s


def expr(e):
    """An expression of the lift, rewritten."""
    e = RE_REG.sub(lambda m: 'r' + m.group(1), e)
    e = RE_RELOC.sub(lambda m: '(d + %su)' % m.group(1), e)
    e = e.replace('spu_ls_read128(ctx, ', 'lsr(c, ')
    e = e.replace('spu_rdch(ctx, ', 'rdch(c, ')
    if 'ctx' in e or 'SPU_' in e:
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


def gen_function(entry, funcs, entries, block_index, final_out):
    out = []
    blocks = gen_body(entry, funcs, entries, block_index, out)
    # a label no goto names (a function's entry) is left as a comment
    text = '\n'.join(out)
    for i, l in enumerate(out):
        m = re.fullmatch(r'L_([0-9A-F]{4}):', l)
        if m and ('goto L_%s;' % m.group(1)) not in text:
            out[i] = '    // L_%s' % m.group(1)
    final_out.extend(out)
    return blocks


def gen_body(entry, funcs, entries, block_index, out):
    name, desc = FUNCS[entry]
    # the blocks: the closure of SPU_DIRECT from the entry, other entries excluded
    blocks = {entry}
    stack = [entry]
    while stack:
        x = stack.pop()
        for y in jumps_of(funcs[x]):
            if y in entries and y != entry:
                continue
            if y not in blocks:
                blocks.add(y)
                stack.append(y)
    blocks = sorted(blocks)
    regs = sorted(set().union(*(regs_of(funcs[b]) for b in blocks)))
    save = ' '.join('g[%d] = r%d;' % (r, r) for r in regs)
    load = ' '.join('r%d = g[%d];' % (r, r) for r in regs)
    out.append('// ---- 0x%04X %s ----' % (entry, desc))
    out.append('void %s(Run& c)' % name)
    out.append('{')
    out.append('    u128* const g = c.g;')
    out.append('    const uint32_t d = c.d;')
    out.append('    (void)g;')
    out.append('    (void)d;')
    # the registers, in groups of eight per line
    decl = ['r%d = g[%d]' % (r, r) for r in regs]
    for i in range(0, len(decl), 8):
        out.append('    Q ' + ', '.join(decl[i:i + 8]) + ';')
    out.append('#define SAVE() do { %s } while (0)' % save)
    out.append('#define LOAD() do { %s } while (0)' % load)
    if blocks[0] != entry:
        out.append('    goto L_%04X;' % entry)
    for b in blocks:
        if b in BLOCK_NOTES:
            out.extend(wrap_comment(BLOCK_NOTES[b], '    // '))
        out.append('L_%04X:' % b)
        out.append('    COV(%d);' % block_index[b])
        for raw in funcs[b]:
            l = raw.strip()
            if not l or l.startswith('const uint32_t _rl = ctx->ls_reloc'):
                continue
            if re.fullmatch(r'/\*.*\*/;?', l):
                continue                                   # nop, lnop, branch hints
            m = re.fullmatch(r'loc_([0-9A-F]+):', l)
            if m:
                a = int(m.group(1), 16)
                if a != b:
                    out.append('L_%04X:' % a)
                continue
            m = re.fullmatch(r'(if \(.*\)) goto loc_([0-9A-F]+);', l)
            if m:
                out.append('    %s goto L_%04X;' % (expr(m.group(1)), int(m.group(2), 16)))
                continue
            m = re.fullmatch(r'goto loc_([0-9A-F]+);', l)
            if m:
                out.append('    goto L_%04X;' % int(m.group(1), 16))
                continue
            # a call: brsl $r0 lifted as a nested host call
            m = re.fullmatch(r'ctx->gpr\[0\] = spu_link\(SPU_RELOC\((0x[0-9A-F]+)\)\); \{ int32_t _si = \(int32_t\)ctx->image_id; '
                             r'ctx->host_depth\+\+; ' + PREFIX + r'([0-9A-F]+)\(ctx\); spu_drain_call\(ctx, SPU_RELOC\((0x[0-9A-F]+)\)\); '
                             r'ctx->host_depth--; spu_img_restore\(ctx, _si\); ctx->ls_reloc = _rl; \}', l)
            if m:
                tgt = int(m.group(2), 16)
                assert m.group(1) == m.group(3) and tgt in entries
                out.append('    r0 = spu_link(d + %su);' % m.group(1))
                out.append('    SAVE(); %s(c); if (c.halt) return; LOAD();   // brsl $r0, 0x%04X' % (FUNCS[tgt][0], tgt))
                continue
            # brsl $rN (N != 0): a PC-relative idiom the lift treats as a jump with a link
            m = re.fullmatch(r'ctx->gpr\[(\d+)\] = spu_link\(SPU_RELOC\((0x[0-9A-F]+)\)\); \{ SPU_DIRECT\(ctx, SPU_RELOC\((0x[0-9A-F]+)\), '
                             + PREFIX + r'([0-9A-F]+)\); \}', l)
            if m:
                out.append('    r%s = spu_link(d + %su);' % (m.group(1), m.group(2)))
                l = '{ SPU_DIRECT(ctx, SPU_RELOC(%s), %s%s); }' % (m.group(3), PREFIX, m.group(4))
            m = re.fullmatch(r'(if \(.*\) )?\{ SPU_DIRECT\(ctx, SPU_RELOC\((0x[0-9A-F]+)\), ' + PREFIX + r'([0-9A-F]+)\); \}', l)
            if m:
                tgt = int(m.group(3), 16)
                assert tgt == int(m.group(2), 16)
                cond = expr(m.group(1)) if m.group(1) else ''
                if not cond and out[-1] == '    goto L_%04X;' % tgt:
                    continue                               # the lift's brsl $rN repeats its jump
                if tgt in blocks:
                    stmt = 'goto L_%04X;' % tgt
                else:
                    assert tgt in entries
                    stmt = '{ SAVE(); %s(c); return; }   // a tail jump' % FUNCS[tgt][0]
                out.append('    ' + cond + stmt)
                continue
            m = re.fullmatch(r'(if \(.*\) )?\{? ?SPU_RET\(ctx\); ?\}?', l)
            if m:
                cond = expr(m.group(1)) if m.group(1) else ''
                out.append('    %s{ SAVE(); return; }' % cond)
                continue
            m = re.fullmatch(r'if \(\(int32_t\)ctx->gpr\[(\d+)\]\._u32\[0\] == \(int32_t\)\(0\)\) \{ extern void spu_halt\(spu_context\*\); '
                             r'fprintf\(stderr, "\[spu\] HALT-ASSERT heqi pc=0x([0-9A-F]+) img=%d\\n", ctx->image_id\); '
                             r'ctx->status = SPU_STATUS_STOPPED_BY_HALT; spu_halt\(ctx\); return; \}', l)
            if m:
                out.append('    if ((int32_t)r%s._u32[0] == 0) { SAVE(); halt(c, 0x%s); return; }   // heqi' % (m.group(1), m.group(2)))
                continue
            m = re.fullmatch(r'spu_ls_write128\(ctx, (.*), (ctx->gpr\[\d+\])\);', l)
            if m:
                out.append('    lsw(c, %s, %s);' % (expr(m.group(1)), expr(m.group(2))))
                continue
            m = re.fullmatch(r'spu_wrch\(ctx, (\w+), (ctx->gpr\[\d+\])\);', l)
            if m:
                out.append('    wrch(c, %s, %s);' % (m.group(1), expr(m.group(2))))
                continue
            m = re.fullmatch(r'(ctx->gpr\[\d+\]) = (spu_\w+\(.*\));', l)
            if m:
                out.append('    %s = %s;' % (expr(m.group(1)), expr(m.group(2))))
                continue
            raise SystemExit('0x%X: not recognised: %s' % (b, l))
    out.append('#undef SAVE')
    out.append('#undef LOAD')
    out.append('}')
    out.append('')
    return blocks


def main():
    lift, template, output = sys.argv[1:4]
    funcs = parse(lift)
    entries = set(FUNCS)
    code_blocks = sorted(a for a in funcs if a < 0x2DE0)     # 0x2E00: .rodata lifted as code
    block_index = {a: i for i, a in enumerate(code_blocks)}
    out = []
    owner = {}
    for e in sorted(FUNCS):
        for b in gen_function(e, funcs, entries, block_index, out):
            assert b not in owner, hex(b)
            owner[b] = e
    stray = [a for a in BLOCK_NOTES if a not in owner]
    if stray:
        raise SystemExit('notes for addresses that start no block: ' + ' '.join('%X' % a for a in stray))
    missing = [a for a in code_blocks if a not in owner]
    if missing:
        raise SystemExit('blocks in no function: ' + ' '.join('%X' % a for a in missing))
    decls = ['void %s(Run& c);' % FUNCS[e][0] for e in sorted(FUNCS)]
    table = ['    { 0x%04Xu, 0x%04Xu },' % (a, owner[a]) for a in code_blocks]
    t = open(template, encoding='utf-8').read()
    t = t.replace('// @DECLS@', '\n'.join(decls))
    t = t.replace('// @FUNCTIONS@', '\n'.join(out).rstrip('\n'))
    t = t.replace('// @BLOCKS@', '\n'.join(table))
    t = t.replace('@BLOCK_COUNT@', str(len(code_blocks)))
    with open(output, 'w', encoding='utf-8', newline='\n') as f:
        f.write(t)
    print('%d lifted functions: %d blocks in %d functions; %d lines' % (len(funcs), len(code_blocks), len(FUNCS), t.count('\n')))


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Which state other work's draws take from PPFX (docs/NATIVE_R3B.md section 5).

From a frame trace with writers (RSX_FRAME_TRACE with PS3_ATTRIB_SHADOW=1):
for every draw that is not PPFX's, the state its derivation reads -- the
surface, viewport, scissor, both programs and their controls, the enabled
vertex arrays and the values of disabled ones, the enabled texture units,
the render state with what its enables make irrelevant left out, the
vertex-program constants its program reads -- and, of that, what PPFX wrote
last. Those items are what removing PPFX's commands (R4) would change for
the draw unless the native side hands them over.

    tools/r3_reverse_deps.py <frametrace.txt> [--frames A-B]

PPFX's own writes: a writer called from PPFX's code (0x007497E0-0x007D3E38),
or a libgcm function called by a libgcm function inside a PPFX span (from
PPFX's first direct call in a frame to its last); the latter are reported
apart as "nested".
"""
import collections
import sys

PPFX = (0x007497E0, 0x007D3E38)
GCM = (0x0069C000, 0x006B0000)


def in_(a, r):
    return r[0] <= a < r[1]


def vp_const_reads(words):
    """The constants a vertex program reads (D1 const_src, for each source
    whose register type is a constant): (the ones read directly, the bases
    of relative reads -- an address register's offset from them, which a
    static look cannot bound)."""
    direct, relative = set(), set()
    for i in range(0, len(words) - 3, 4):
        d1, d2, d3 = words[i + 1], words[i + 2], words[i + 3]
        src = [((d2 >> 23) & 0x1FF) | ((d1 & 0xFF) << 9), (d2 >> 6) & 0x1FFFF,
               ((d3 >> 21) & 0x7FF) | ((d2 & 0x3F) << 11)]
        c = (d1 >> 12) & 0x1FF
        for s in src:
            if s & 3 == 3:
                (relative if d3 >> 1 & 1 else direct).add(c)
        if d3 & 1:   # END
            break
    return direct, relative


def main():
    path = sys.argv[1]
    fr = None
    if '--frames' in sys.argv:
        a, b = sys.argv[sys.argv.index('--frames') + 1].split('-')
        fr = (int(a), int(b))
    last = {}          # method -> (value, who, fn, lr)
    const = {}         # constant index -> (who, fn, lr)
    vpmem = [0] * (544 * 4)
    vp_load = 0
    const_load = 0
    frame = 0
    ppfx_span = False
    out = collections.defaultdict(collections.Counter)    # item -> (who, draw writer) -> draws
    draws = collections.Counter()
    # the render state's enables: an item counts only with its enable on
    gated = {0x0308: 0x0304, 0x030C: 0x0304,                       # alpha func, ref
             0x0314: 0x0310, 0x0318: 0x0310, 0x031C: 0x0310, 0x0320: 0x0310, 0x037C: 0x0310,
             0x032C: 0x0328, 0x0330: 0x0328, 0x0334: 0x0328, 0x0338: 0x0328, 0x033C: 0x0328,
             0x0340: 0x0328, 0x0344: 0x0328,
             0x0A6C: 0x0A74, 0x0A70: 0x0A74,
             0x1830: 0x183C, 0x1834: 0x183C,
             0x0A78: 0x0A68, 0x0A7C: 0x0A68}
    always = [0x0194, 0x018C, 0x01B4, 0x01B8, 0x0198, 0x0208, 0x020C, 0x0210, 0x0214, 0x0218, 0x021C, 0x022C,
              0x0220, 0x0200, 0x0204, 0x02B8, 0x1D88,
              0x0A00, 0x0A04, 0x0394, 0x0398] + list(range(0x0A20, 0x0A40, 4)) + [0x08C0, 0x08C4,
              0x08E4, 0x1D60, 0x1FF4, 0x1FF0, 0x1EA0, 0x1EF8, 0x1FEC, 0x1478, 0x1D7C, 0x1738, 0x173C, 0x1FC0,
              0x0300, 0x0304, 0x0310, 0x0324, 0x0328, 0x036C, 0x0370, 0x0374, 0x0380, 0x0A68, 0x0A74,
              0x183C, 0x1828, 0x182C, 0x1838, 0x147C, 0x03BC, 0x1DB4, 0x1FF8] + list(gated)
    for line in open(path, errors='replace'):
        if line.startswith('#'):
            continue
        p = line.split()
        if len(p) >= 3 and p[2] == 'FLIP':
            frame += 1
            ppfx_span = False
            continue
        if ' M ' not in line:
            continue
        fn = int(p[2], 16)
        lr = 0
        for t in p:
            if t.startswith('lr='):
                lr = int(t[3:], 16)
        i = p.index('M')
        sc = int(p[i + 1])
        meth = int(p[i + 2], 16)
        cnt = int(p[i + 3])
        ni = len(p) > i + 4 and p[i + 4] == 'ni'
        vals = [int(x, 16) for x in p[i + 4:] if x not in ('...', 'ni')][:min(cnt, 8)]
        if sc != 0:
            continue
        direct = in_(lr, PPFX)
        if direct:
            ppfx_span = True
        nested = in_(fn, GCM) and in_(lr, GCM) and ppfx_span
        who = 'ppfx' if direct else 'ppfx-nested' if nested else 'other'
        for k, v in enumerate(vals):
            m = meth if ni else meth + 4 * k
            last[m] = (v, who, fn, lr)
            if m == 0x1E9C:
                vp_load = v
            elif 0x0B80 <= m < 0x0C00:
                if vp_load * 4 + (m - 0x0B80) // 4 < len(vpmem):
                    vpmem[vp_load * 4 + (m - 0x0B80) // 4] = v
                if m == 0x0BFC or k == len(vals) - 1:
                    pass
            elif m == 0x1EFC:
                const_load = v
            elif 0x1F00 <= m < 0x1F80:
                ci = const_load + (m - 0x1F00) // 16
                const[ci] = (who, fn, lr)
        if meth == 0x0B80 and cnt:
            vp_load += cnt // 4          # the load slot advances by whole instructions
        if meth == 0x1F00 and cnt:
            const_load += cnt // 4
        # a draw: BEGIN_END with a primitive
        if meth != 0x1808 or not vals or vals[0] == 0:
            continue
        if direct or nested:
            continue
        if fr and not (fr[0] <= frame <= fr[1]):
            continue
        dw = 'asp:%08X' % fn if 0x006C0000 <= fn < 0x006D6000 else '%08X<-%08X' % (fn, lr)
        draws[dw] += 1
        items = list(always)
        # enabled vertex arrays (size != 0), the values of the others the program may read
        for a in range(16):
            fmt = last.get(0x1740 + 4 * a, (0,))[0]
            if fmt >> 4 & 0xF:
                items += [0x1740 + 4 * a, 0x1680 + 4 * a]
            else:
                items += [0x1C00 + 16 * a + 4 * c for c in range(4)]
        # enabled texture units
        for u in range(16):
            if last.get(0x1A0C + 0x20 * u, (0,))[0] >> 31:
                items += [0x1A00 + 0x20 * u + 4 * k for k in range(8)] + [0x1840 + 4 * u]
        for m in items:
            if m in gated and not (last.get(gated[m], (0,))[0] & 1):
                continue
            v, w, f, l = last.get(m, (None, 'reset', 0, 0))
            if w.startswith('ppfx'):
                out['%04X' % m][(w, dw, '%08X' % (v or 0))] += 1
        # the constants this draw's vertex program reads
        start = last.get(0x1EA0, (0,))[0]
        direct, relative = vp_const_reads(vpmem[start * 4:])
        for c in sorted(direct):
            w = const.get(c, ('reset', 0, 0))[0]
            if w.startswith('ppfx'):
                out['const c%d' % c][(w, dw, 'read directly')] += 1
        for c in sorted(relative):
            out['relative from c%d' % c][('-', dw, 'base of an indexed read')] += 1
    print('# other work\'s draws: %d, by writer: %s' % (sum(draws.values()), dict(draws.most_common(12))))
    if not out:
        print('no state of other work\'s draws was last set by PPFX')
    for item in sorted(out):
        for (w, dw, v), n in out[item].most_common():
            print('%-10s set by %-12s used by %-24s %s  %d draws' % (item, w, dw, v, n))


if __name__ == '__main__':
    main()

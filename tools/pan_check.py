#!/usr/bin/env python3
"""pan_check.py: positioned effects at mirrored positions, measured in a captured mix (docs/OPENAL_MIGRATION_PLAN.md
section 4.6). The plays are the sound lab's (tests/scenarios/sound_pan.drive: DOTHEROES_DEV_SOUND, `sepos` and
`se` steps); the capture is the final mix of either audio path (cellAudio's AUDIO_WAV, raw f32le stereo 48 kHz, or
OpenAL Soft's wave writer, a float WAV of 2 to 8 channels). Pure Python (no numpy).

  pan_check.py <capture> <drive> [--channels N]

Each play is found by its onset (the mix is silent between them: the music and loops faded out first), matched
to the drive's steps in order, and measured over the effect's length: each channel's level (RMS, dBFS, the noise
before the onset subtracted), the total power over all channels, and the balance. For each mirrored pair of
offsets (x and -x, or z and -z): the near channel against its mirror, the far channel against its mirror, and the
total -- averaged over the repeats, with their spread.
"""
import math
import os
import re
import struct
import sys
import array

RATE = 48000
HOP = 240   # 5 ms


def read_capture(path, channels):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:4] == b'RIFF' and data[8:12] == b'WAVE':
        pos = 12
        while pos + 8 <= len(data):
            cid, size = data[pos:pos + 4], struct.unpack_from('<I', data, pos + 4)[0]
            if cid == b'fmt ':
                channels = struct.unpack_from('<H', data, pos + 10)[0]
                rate = struct.unpack_from('<I', data, pos + 12)[0]
                if rate != RATE:
                    sys.exit('pan_check: %d Hz, expected %d' % (rate, RATE))
            if cid == b'data':
                end = len(data) if size in (0, 0xFFFFFFFF) else min(len(data), pos + 8 + size)
                data = data[pos + 8:end]
                break
            pos += 8 + size + (size & 1)
    data = data[:len(data) // (4 * channels) * 4 * channels]
    a = array.array('f')
    a.frombytes(data)
    if sys.byteorder != 'little':
        a.byteswap()
    return a, channels


def envelopes(a, ch):
    """per channel, the sum of squares in each 5 ms hop"""
    n = len(a) // ch
    out = []
    for c in range(ch):
        x = a[c::ch]
        e = []
        for i in range(0, n - HOP + 1, HOP):
            s = x[i:i + HOP]
            e.append(sum(v * v for v in s))
        out.append(e)
    return out


def drive_steps(path):
    for line in open(path, encoding='utf-8'):
        m = re.match(r'#!\s*env\s+.*DOTHEROES_DEV_SOUND=(\S+)', line)
        if m:
            spec = m.group(1)
            break
    else:
        sys.exit('pan_check: no DOTHEROES_DEV_SOUND in ' + path)
    plays = []
    for one in spec.split(','):
        p = one.split(':')
        if p[1] == 'sepos':
            plays.append((float(p[0]), int(p[2]), (float(p[3]), float(p[4]), float(p[5]))))
        elif p[1] == 'se':
            plays.append((float(p[0]), int(p[2]), None))
    return plays


def db(x):
    return 10 * math.log10(x) if x > 0 else -200.0


def main():
    args = sys.argv[1:]
    ch = 2
    if '--channels' in args:
        i = args.index('--channels')
        ch = int(args[i + 1])
        del args[i:i + 2]
    if len(args) != 2:
        sys.exit(__doc__)
    a, ch = read_capture(args[0], ch)
    plays = drive_steps(args[1])
    env = envelopes(a, ch)
    tot = [sum(env[c][i] for c in range(ch)) for i in range(len(env[0]))]
    # the noise floor: the quietest 10% of hops after the first play's expected neighbourhood
    srt = sorted(tot)
    floor = srt[len(srt) // 10] / HOP + 1e-20
    thresh = floor * 10 ** 1.5   # 15 dB over the floor (an ambience may stay under the plays)
    onsets = []
    quiet = 0
    for i, v in enumerate(tot):
        if v / HOP > thresh:
            if quiet >= 40:   # 200 ms below first
                onsets.append(i)
            quiet = 0
        else:
            quiet += 1
    # the schedule's place in the capture: of the offsets that put a play on an onset, the one with the most
    # energy at all the plays' times together (the plays are evenly spaced: counting matched onsets alone can
    # lock one step early, onto an ambience's peak)
    sched = [t for t, _, _ in plays]
    pre = [0.0]
    for v in tot:
        pre.append(pre[-1] + v)

    def energy_at(t, span=0.1):
        i = int(round(t * RATE / HOP))
        j = i + int(span * RATE / HOP)
        if i < 0 or j >= len(tot):
            return 0.0
        return (pre[j] - pre[i]) / ((j - i) * HOP)

    def contrast(t):   # a play's start: much more energy just after than just before (music has none)
        r = math.log10((energy_at(t) + floor) / (energy_at(t - 0.3, 0.25) + floor))
        return min(max(r, 0.0), 3.0)

    best = None
    for o in onsets:
        for t_first in sched[:4]:
            t0 = o * HOP / RATE - t_first
            score = sum(contrast(t + t0) for t in sched)
            if not best or score > best[0]:
                best = (score, t0)
    t0 = best[1]
    hits = []
    for t in sched:
        want = (t + t0) * RATE / HOP
        near = [o for o in onsets if abs(o - want) <= 0.15 * RATE / HOP]
        hits.append(min(near, key=lambda o: abs(o - want)) if near else None)
    nfound = sum(1 for h in hits if h is not None)
    # a play with no onset of its own (too quiet for the threshold, or masked) is measured where the aligned
    # schedule puts it (marked ~)
    guessed = set()
    for k, (t, _, _) in enumerate(plays):
        if hits[k] is None:
            hits[k] = int(round((t + t0) * RATE / HOP))
            guessed.add(k)
    print('capture: %s, %d channels, %.1f s; noise floor %.1f dBFS; %d of %d plays found by their onsets, the rest '
          'measured where the schedule puts them (~); the schedule at %+.2f s in the capture' %
          (os.path.basename(args[0]), ch, len(a) / ch / RATE, db(floor), nfound, len(plays), t0))
    length = {202: 0.40, 302: 0.42, 148: 0.14}
    rows = []
    for (t, sid, off), h in zip(plays, hits):
        if h is None or h >= len(env[0]):
            rows.append(None)
            continue
        i0 = max(0, h - 1)
        i1 = h + int(length.get(sid, 0.4) * RATE / HOP) + 1
        b0, b1 = max(0, h - 70), max(0, h - 10)   # the noise before: 300 ms
        lv = []
        for c in range(ch):
            e = sum(env[c][i0:i1]) / ((i1 - i0) * HOP)
            nz = sum(env[c][b0:b1]) / max(1, (b1 - b0) * HOP)
            lv.append(max(e - nz, 1e-20))
        rows.append(lv)
    names = ['L', 'R', 'C', 'LFE', 'SL', 'SR', 'BL', 'BR'] if ch == 8 else (['L', 'R'] if ch == 2 else ['c%d' % c for c in range(ch)])
    if ch == 8:
        names = ['FL', 'FR', 'FC', 'LFE', 'BL', 'BR', 'SL', 'SR']   # WAVE_FORMAT_EXTENSIBLE 7.1 order
    print('\n%-7s %4s %-22s %s  %8s %8s' % ('t', 'id', 'offset', ' '.join('%7s' % n for n in names), 'total', 'L-R'))
    for k, ((t, sid, off), lv) in enumerate(zip(plays, rows)):
        if lv is None:
            print('%-7.1f %4d %-22s (outside the capture)' % (t, sid, off))
            continue
        L = sum(lv[c] for c in range(ch) if names[c] in ('L', 'FL', 'SL', 'BL'))
        R = sum(lv[c] for c in range(ch) if names[c] in ('R', 'FR', 'SR', 'BR'))
        print('%-7.1f %4d %-22s %s  %8.2f %8.2f%s' % (t, sid, off if off else 'none', ' '.join('%7.2f' % db(v) for v in lv),
                                                      db(sum(lv)), db(L) - db(R), ' ~' if k in guessed else ''))
    # mirrored pairs: group by (id, |offset| with x or z sign folded)
    groups = {}
    for (t, sid, off), lv in zip(plays, rows):
        if lv is None:
            continue
        if off is None:
            key, side = (sid, 'none'), 0
        elif off[0] != 0:
            key, side = (sid, 'x', abs(off[0]), off[1], off[2]), (1 if off[0] > 0 else -1)
        else:
            key, side = (sid, 'z', abs(off[2]), off[1], 0.0), (1 if off[2] > 0 else -1)
        groups.setdefault(key, {}).setdefault(side, []).append(lv)

    def mean_db(lvs, c):
        v = [db(x[c]) for x in lvs]
        return sum(v) / len(v), (max(v) - min(v)) if len(v) > 1 else 0.0

    def mean_tot(lvs):
        v = [db(sum(x)) for x in lvs]
        return sum(v) / len(v), (max(v) - min(v)) if len(v) > 1 else 0.0

    print('\nmirrored pairs (dB; spread = max - min over the repeats):')
    li, ri = 0, 1
    if ch == 8:
        pairs = [(0, 1), (6, 7), (4, 5)]
    else:
        pairs = [(0, 1)]
    for key, sides in groups.items():
        if key[1] == 'none':
            s = sides[0]
            t, sp = mean_tot(s)
            bal = sum(db(x[0]) - db(x[1]) for x in s) / len(s)
            print('  id %d, no position: total %.2f (spread %.2f), L-R %.2f, n=%d' % (key[0], t, sp, bal, len(s)))
            continue
        if -1 not in sides or 1 not in sides:
            continue
        axis = key[1]
        # x: the left (-x) first; z: ahead (+z) first
        neg, pos = (sides[-1], sides[1]) if axis == 'x' else (sides[1], sides[-1])
        tn, sn = mean_tot(neg)
        tp, sp = mean_tot(pos)
        lab = 'x -%g / +%g' % (key[2], key[2]) if axis == 'x' else 'z +%g / -%g' % (key[2], key[2])
        line = '  id %d, %s (y %g%s): total %.2f / %.2f -> difference %+.2f (spreads %.2f, %.2f), n=%d/%d' % (
            key[0], lab, key[3], ', z %g' % key[4] if axis == 'x' and key[4] else '', tn, tp, tn - tp, sn, sp, len(neg), len(pos))
        print(line)
        if axis == 'x':
            for a_, b_ in pairs:
                nl, _ = mean_db(neg, a_)
                nr, _ = mean_db(neg, b_)
                pl, _ = mean_db(pos, a_)
                pr, _ = mean_db(pos, b_)
                print('      %s/%s: on the left %.2f / %.2f, on the right %.2f / %.2f -> near %+.2f, far %+.2f' % (
                    names[a_], names[b_], nl, nr, pl, pr, nl - pr, nr - pl))
        else:
            for a_, b_ in pairs:
                nl, _ = mean_db(neg, a_)
                nr, _ = mean_db(neg, b_)
                pl, _ = mean_db(pos, a_)
                pr, _ = mean_db(pos, b_)
                print('      %s/%s: ahead %.2f / %.2f (L-R %+.2f), behind %.2f / %.2f (L-R %+.2f)' % (
                    names[a_], names[b_], nl, nr, nl - nr, pl, pr, pl - pr))


if __name__ == '__main__':
    main()

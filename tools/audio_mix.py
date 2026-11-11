#!/usr/bin/env python3
"""audio_mix.py: the game's captured output (cellAudio's AUDIO_WAV, or the OpenAL path's: raw f32le, 48 kHz,
stereo) looked at (docs/OPENAL_MIGRATION_PLAN.md section 4). Pure Python (no numpy): everything is computed on
10 ms energy envelopes.

  audio_mix.py summary <mix.f32>
      length, level (RMS dBFS) and left/right balance per 10 s, silent stretches, the loudest moments.
  audio_mix.py find <mix.f32> <file.wav> [<from s> [<to s>]]
      where in the mix the file starts (the envelope of its first 20 s matched along the mix): the offset and
      how well it matches (correlation 0..1).
  audio_mix.py loop <mix.f32> <file.wav> <start in the mix, s> [<loop start s> <loop end s>]
      after the file's first pass (to its loop end, or its end), what plays next: the mix's envelope there
      against the file's at the loop start and at the file's start (and, for a play-once file, against
      silence): which one it is, with the correlation of each.
"""
import array
import math
import struct
import sys

RATE = 48000
HOP = 0.01   # s


def read_mix(path):
    a = array.array('f')
    with open(path, 'rb') as f:
        a.frombytes(f.read())
    if sys.byteorder != 'little':
        a.byteswap()
    return a   # interleaved L R


def mix_envelope(a, start=0.0, end=None):
    """per-10 ms mean square of (L + R) / 2, from start to end (s)"""
    hop = int(RATE * HOP)
    n = len(a) // 2
    i0 = int(start * RATE)
    i1 = n if end is None else min(n, int(end * RATE))
    out = []
    for i in range(i0, i1 - hop + 1, hop):
        s = 0.0
        seg = a[2 * i:2 * (i + hop)]
        for k in range(0, len(seg), 2):
            m = (seg[k] + seg[k + 1]) * 0.5
            s += m * m
        out.append(s / hop)
    return out


def wav_envelope(path, start=0.0, length=20.0):
    d = open(path, 'rb').read()
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(d):
        cid, n = d[pos:pos + 4], struct.unpack('<I', d[pos + 4:pos + 8])[0]
        if cid == b'fmt ':
            fmt = struct.unpack('<HHIIHH', d[pos + 8:pos + 24])
        elif cid == b'data':
            data = (pos + 8, n)
        pos += 8 + n + (n & 1)
    _, ch, rate, _, align, bits = fmt
    hop = int(rate * HOP)
    off, n = data
    frames = n // align
    f0 = int(start * rate)
    f1 = min(frames, f0 + int(length * rate))
    out = []
    for i in range(f0, f1 - hop + 1, hop):
        raw = d[off + i * align: off + (i + hop) * align]
        v = array.array('h', raw)
        if sys.byteorder != 'little':
            v.byteswap()
        s = 0.0
        if ch == 1:
            for x in v:
                m = x / 32768.0
                s += m * m
        else:
            for k in range(0, len(v), 2):
                m = (v[k] + v[k + 1]) / 65536.0
                s += m * m
        out.append(s / hop)
    return out, frames / rate


def corr(a, b):
    n = min(len(a), len(b))
    if n < 10:
        return 0.0
    a, b = a[:n], b[:n]
    ma, mb = sum(a) / n, sum(b) / n
    sa = math.sqrt(sum((x - ma) ** 2 for x in a))
    sb = math.sqrt(sum((x - mb) ** 2 for x in b))
    if sa == 0 or sb == 0:
        return 0.0
    return sum((x - ma) * (y - mb) for x, y in zip(a, b)) / (sa * sb)


def db(x):
    return 10 * math.log10(x) if x > 1e-12 else -120.0


def summary(path):
    a = read_mix(path)
    n = len(a) // 2
    print('%s: %.1f s' % (path, n / RATE))
    win = 10 * RATE
    for w in range(0, n, win):
        sl = sr = 0.0
        seg = a[2 * w:2 * min(n, w + win)]
        for k in range(0, len(seg), 2):
            sl += seg[k] * seg[k]
            sr += seg[k + 1] * seg[k + 1]
        m = len(seg) // 2 or 1
        print('  %6.0f s  level %6.1f dBFS  balance (L-R) %+5.1f dB' % (w / RATE, db((sl + sr) / (2 * m)), db(sl / m) - db(sr / m)))


def find(path, wav, t0=0.0, t1=None):
    env = mix_envelope(read_mix(path), t0, t1)
    ref, _ = wav_envelope(wav, 0.0, 20.0)
    best = (-1.0, 0)
    for lag in range(0, max(1, len(env) - len(ref))):
        c = corr(env[lag:lag + len(ref)], ref)
        if c > best[0]:
            best = (c, lag)
    print('%s starts at %.2f s in the mix (correlation %.3f)' % (wav, t0 + best[1] * HOP, best[0]))
    return t0 + best[1] * HOP, best[0]


def loop(path, wav, start, ls=None, le=None):
    _, length = wav_envelope(wav, 0.0, 0.01)
    end = le if le is not None else length
    at = start + end   # where the first pass ends in the mix
    span = 8.0
    mix = mix_envelope(read_mix(path), at - 1.0, at + span + 1.0)
    cands = {'the file start (0 s)': wav_envelope(wav, 0.0, span)[0]}
    if ls is not None:
        cands['the loop start (%.2f s)' % ls] = wav_envelope(wav, ls, span)[0]
    print('%s: the first pass ends at %.2f s in the mix' % (wav, at))
    level = sum(mix[100:100 + int(span / HOP)]) / max(1, int(span / HOP))
    print('  after it: level %.1f dBFS' % db(level))
    for name, ref in cands.items():
        best = max((corr(mix[k:k + len(ref)], ref), k) for k in range(0, 200))
        print('  against %s: correlation %.3f (lag %+.2f s)' % (name, best[0], (best[1] - 100) * HOP))


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    cmd, args = sys.argv[1], sys.argv[2:]
    if cmd == 'summary':
        summary(args[0])
    elif cmd == 'find':
        find(args[0], args[1], *(float(x) for x in args[2:4]))
    elif cmd == 'loop':
        loop(args[0], args[1], float(args[2]), *(float(x) for x in args[3:5]))
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
"""M8.2's sound comparison (docs/FFMPEG_MIGRATION_PLAN.md section 5): a movie's sound as the lifted Bink played it
(cellAudio's AUDIO_WAV: raw f32le, stereo, 48 kHz -- only the movies use cellAudio since M5) against the same movie on
the native player (OpenAL Soft's wave writer, ALSOFT_CONF: float32, stereo, 48 kHz, a WAV file -- the whole mix).
Pure Python, on 10 ms energy envelopes.

  tools/m8_sound_compare.py <bink.f32> <native.wav> [<seconds>] [<occurrence>]

The movie's sound is found in Bink's capture (its occurrence-th start after silence, default the first), the native
capture aligned to it by the envelopes' correlation (100 ms steps over the whole capture, then 10 ms), then over
<seconds> (default 115.5: the demo) per 10 s: each capture's level (RMS dBFS), the difference, the left/right
balance, and the correlation of the two 10 ms envelopes.
"""
import array
import math
import struct
import sys

RATE = 48000
HOP = 480   # 10 ms


def read(path):
    data = open(path, "rb").read()
    off = 0
    if data[:4] == b"RIFF":
        pos, fmt_ok = 12, False
        while pos + 8 <= len(data):
            cid, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
            if cid == b"fmt ":
                tag, ch, rate = struct.unpack("<HHI", data[pos + 8:pos + 16])
                bits = struct.unpack("<H", data[pos + 22:pos + 24])[0]
                if tag == 0xFFFE:
                    tag = struct.unpack("<H", data[pos + 32:pos + 34])[0]
                fmt_ok = tag == 3 and ch == 2 and rate == RATE and bits == 32
                if not fmt_ok:
                    sys.exit("%s: not float32 stereo 48 kHz (tag %d, %d channels, %d Hz, %d bits)" % (path, tag, ch, rate, bits))
            if cid == b"data":
                off = pos + 8
                break
            pos += 8 + size + (size & 1)
    a = array.array("f")
    n = (len(data) - off) // 8 * 8
    a.frombytes(data[off:off + n])
    if sys.byteorder != "little":
        a.byteswap()
    return a


def envelope(a):
    """per 10 ms: (mean square both channels, left's, right's)"""
    out = []
    n = len(a) // 2
    for w in range(n // HOP):
        s = a[w * HOP * 2:(w + 1) * HOP * 2]
        l = sum(x * x for x in s[0::2]) / HOP
        r = sum(x * x for x in s[1::2]) / HOP
        out.append(((l + r) / 2, l, r))
    return out


def db(x):
    return 10 * math.log10(x) if x > 1e-20 else -200.0


def corr(a, b):
    n = min(len(a), len(b))
    if n < 2:
        return 0.0
    ma, mb = sum(a[:n]) / n, sum(b[:n]) / n
    sa = math.sqrt(sum((x - ma) ** 2 for x in a[:n]))
    sb = math.sqrt(sum((x - mb) ** 2 for x in b[:n]))
    return sum((a[i] - ma) * (b[i] - mb) for i in range(n)) / (sa * sb) if sa and sb else 0.0


def onset(env, occurrence):
    """the occurrence-th window after which 1 s holds sound (-60 dBFS) after 1 s of near silence"""
    lv = [db(e[0]) for e in env]
    found = 0
    for i in range(100, len(lv) - 100):
        if lv[i] > -50 and max(lv[i - 100:i]) < -70 and sum(1 for x in lv[i:i + 100] if x > -60) > 80:
            found += 1
            if found == occurrence:
                return i
    return None


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    secs = float(sys.argv[3]) if len(sys.argv) > 3 else 115.5
    occ = int(sys.argv[4]) if len(sys.argv) > 4 else 1
    A, B = envelope(read(sys.argv[1])), envelope(read(sys.argv[2]))
    a0 = onset(A, occ)
    if a0 is None:
        sys.exit("no sound found in %s" % sys.argv[1])
    span = int(secs * 100)
    ref = [math.sqrt(e[0]) for e in A[a0:a0 + min(span, 3000)]]
    lb = [math.sqrt(e[0]) for e in B]
    # coarse: 100 ms steps over the whole native capture, on 100 ms means
    ref10 = [sum(ref[i:i + 10]) for i in range(0, len(ref) - 9, 10)]
    lb10 = [sum(lb[i:i + 10]) for i in range(0, len(lb) - 9, 10)]
    # both captures start with the game: the same movie lies within a minute of the same time in each (a later
    # occurrence of the same movie would match as well)
    lo, hi = max(0, a0 // 10 - 600), min(max(1, len(lb10) - len(ref10)), a0 // 10 + 600)
    best = max(range(lo, max(lo + 1, hi)), key=lambda k: corr(ref10, lb10[k:k + len(ref10)]))
    b0 = max(0, best * 10 - 20)
    b0 = max(range(b0, min(len(lb) - len(ref), b0 + 40)), key=lambda k: corr(ref, lb[k:k + len(ref)]))
    print("the movie's sound: Bink's capture from %.2f s, the native capture from %.2f s (aligned by the envelopes)"
          % (a0 / 100, b0 / 100))
    print("  %-10s %9s %9s %7s %16s %6s" % ("window", "Bink dB", "native dB", "diff", "balance B / n dB", "corr"))
    diffs = []
    for w in range(0, span, 1000):
        sa, sb = A[a0 + w:a0 + min(span, w + 1000)], B[b0 + w:b0 + min(span, w + 1000)]
        if len(sa) < 100 or len(sb) < 100:
            break
        la, lbv = db(sum(e[0] for e in sa) / len(sa)), db(sum(e[0] for e in sb) / len(sb))
        bal_a = db(sum(e[1] for e in sa)) - db(sum(e[2] for e in sa))
        bal_b = db(sum(e[1] for e in sb)) - db(sum(e[2] for e in sb))
        ea = [math.sqrt(e[0]) for e in sa]
        c = corr(ea, [math.sqrt(e[0]) for e in sb])
        # the window's own best lag (within 1.5 s): a drift or a gap between the two shows here
        lag = max(range(-150, 151), key=lambda k: corr(ea, [math.sqrt(e[0]) for e in B[b0 + w + k:b0 + w + k + len(ea)]])
                  if b0 + w + k >= 0 else -2)
        cl = corr(ea, [math.sqrt(e[0]) for e in B[b0 + w + lag:b0 + w + lag + len(ea)]])
        diffs.append(lbv - la)
        print("  %4d-%-5d %9.2f %9.2f %+7.2f %+7.2f / %+6.2f %6.3f   lag %+5d ms (corr %.3f)" % (
            w // 100, min(span, w + 1000) // 100, la, lbv, lbv - la, bal_a, bal_b, c, lag * 10, cl))
    whole_a = db(sum(e[0] for e in A[a0:a0 + span]) / span)
    whole_b = db(sum(e[0] for e in B[b0:b0 + span]) / span)
    print("  whole: Bink %.2f dB, native %.2f dB (%+.2f); the envelopes' correlation %.3f" % (
        whole_a, whole_b, whole_b - whole_a, corr([math.sqrt(e[0]) for e in A[a0:a0 + span]],
                                                  [math.sqrt(e[0]) for e in B[b0:b0 + span]])))


if __name__ == "__main__":
    main()

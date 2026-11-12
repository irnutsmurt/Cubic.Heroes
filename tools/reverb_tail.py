#!/usr/bin/env python3
"""The reverb a mix put on an effect, measured from a capture and its audio trace (M4: the dungeon reverb on FMOD's
path against OpenAL Soft's; tests/scenarios/reverb_lab.drive plays one dry effect at a time, the reverb on, then
off). For each play of the effect: its dry sound's energy (from the play to the file's length), the tail's energy
after it (from 30 ms past the end to 1.2 s past it) less the noise before the play, and their ratio in dB -- the
reverb's level relative to the sound; with the reverb off the tail is only what the mix holds after a sound
(nothing, or a mixing block). The capture starts at the trace's SM.init (the device opened there), as the game's
captures and tests/native/sound_replay --wav do.

    tools/reverb_tail.py <mix: .f32/.raw or .wav> <trace> <effect id> <the effect's file, for its length>
                         [--until <s>] [-v]
--until: only the plays up to so many seconds after SM.init (e.g. before a movie's sound joins the capture);
-v: each play.
"""
import math
import os
import sys
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import audio_mix as am  # noqa: E402
import audio_trace  # noqa: E402


def energy(a, i0, i1):
    i0, i1 = max(0, i0), min(len(a) // 2, i1)
    if i1 <= i0:
        return 0.0
    return sum(a[2 * i] ** 2 + a[2 * i + 1] ** 2 for i in range(i0, i1)) / (2 * (i1 - i0))


def main():
    mix, trace, sid, sfile = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
    until = float(sys.argv[sys.argv.index('--until') + 1]) if '--until' in sys.argv else 1e30
    verbose = '-v' in sys.argv
    with wave.open(sfile) as w:
        length = w.getnframes() / w.getframerate()
    a = am.read_mix(mix)
    calls = list(audio_trace.parse(trace))
    t_init = next(c['t'] for c in calls if c['name'] == 'SM.init')
    reverb = False
    rows = []
    R = am.RATE
    for c in calls:
        if c['name'] == 'SM.setReverb' and c['args']:
            reverb = c['args'][0] == '1'
        if c['name'] != 'SM.playSE' or not c['args'] or c['args'][0] != sid:
            continue
        if c['t'] - t_init > until:
            continue
        s = int((c['t'] - t_init) * R)
        # the onset: the strongest 20 ms rise within 0.15 s of the call, either way (a capture can start a little
        # after the trace's SM.init -- the device opening -- or lag it)
        best, onset = None, s
        for off in range(-int(0.15 * R), int(0.15 * R), int(0.002 * R)):
            d = energy(a, s + off, s + off + int(0.02 * R)) - energy(a, s + off - int(0.02 * R), s + off)
            if best is None or d > best:
                best, onset = d, s + off
        L = int(length * R)
        noise = energy(a, onset - int(0.6 * R), onset - int(0.05 * R))
        dry = energy(a, onset, onset + L) - noise
        tail = energy(a, onset + L + int(0.03 * R), onset + L + int(1.2 * R)) - noise
        db = 10 * math.log10(max(tail, 1e-14) / max(dry, 1e-14))
        rows.append((reverb, (onset - s) / R, dry, tail, db))
        if verbose:
            print(f'  {c["t"] - t_init:7.2f} s  reverb {"on " if reverb else "off"}  tail/dry {db:+.2f} dB')
    for on in (True, False):
        rr = [r for r in rows if r[0] == on]
        if not rr:
            continue
        dbs = [r[4] for r in rr]
        print(f'{os.path.basename(mix)}: reverb {"on " if on else "off"}: {len(rr)} plays, tail/dry '
              f'{sum(dbs) / len(dbs):+.2f} dB (range {min(dbs):+.2f} to {max(dbs):+.2f}); dry level '
              f'{10 * math.log10(sum(r[2] for r in rr) / len(rr)):.1f} dBFS; onset lag {sum(r[1] for r in rr) / len(rr) * 1000:.0f} ms')


if __name__ == '__main__':
    main()

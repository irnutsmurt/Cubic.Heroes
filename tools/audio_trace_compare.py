#!/usr/bin/env python3
"""audio_trace_compare.py <rel27 trace> <native trace> [--json out.json]: the game's sound-manager calls in two runs
of the same scene (port/audio_hooks.cpp traces; docs/OPENAL_MIGRATION_PLAN.md, M3) -- rel27's on FMOD and one on
the native sound manager (DOTHEROES_NATIVE_AUDIO=1) -- side by side:
  - calls by method;
  - the music: each playBGM (id, loop) in order with the game frame it came at, and each fadeOutBGM;
  - the reverb's switches in order, with their frames;
  - effects played, by id (plays, plays that got no handle);
  - the answers the game read: isPlaying, isBGMActive, currentBGMId, isLoaded -- how often each value;
  - in the native trace: FMOD API calls (there must be none).
Two runs of a scene differ a little by themselves (frame pacing moves an enemy, a hit, a sound): the report gives
the differences; the music, the reverb and the method set must agree.
"""
import json
import os
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import audio_trace  # noqa: E402


def summary(path):
    s = {'calls': Counter(), 'bgm': [], 'fades': [], 'reverb': [], 'plays': Counter(), 'no_handle': Counter(),
         'answers': {}, 'fmod': 0, 'frames': 0}
    t0 = None
    for c in audio_trace.parse(path):
        n = c['name']
        if t0 is None:
            t0 = c['t']
        if n.startswith('FMOD.'):
            s['fmod'] += 1
            continue
        if not n.startswith('SM.'):
            continue
        s['calls'][n] += 1
        s['frames'] = max(s['frames'], c['frame'])
        a = c['args']
        if n == 'SM.playBGM':
            s['bgm'].append((int(a[0]), int(a[2]), c['frame']))
        elif n == 'SM.fadeOutBGM':
            s['fades'].append((float(a[0]), c['frame']))
        elif n == 'SM.setReverb':
            s['reverb'].append((int(a[0]), c['frame']))
        elif n in ('SM.playSE', 'SM.playSEFadeIn'):
            s['plays'][int(a[0])] += 1
            if not c['ret']:
                s['no_handle'][int(a[0])] += 1
        elif n in ('SM.isPlaying', 'SM.isBGMActive', 'SM.currentBGMId', 'SM.isLoaded'):
            s['answers'].setdefault(n, Counter())[c['ret']] += 1
    return s


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    a, b = summary(sys.argv[1]), summary(sys.argv[2])
    print(f'rel27: {sys.argv[1]}\nnative: {sys.argv[2]}')
    print('calls by method (rel27 / native):')
    for n in sorted(set(a['calls']) | set(b['calls'])):
        print(f'  {n:24} {a["calls"][n]:8} {b["calls"][n]:8}')
    same_bgm = [(i, l) for i, l, _ in a['bgm']] == [(i, l) for i, l, _ in b['bgm']]
    print(f'music: {"the same tracks in the same order" if same_bgm else "DIFFERENT"}')
    for k in range(max(len(a['bgm']), len(b['bgm']))):
        x = a['bgm'][k] if k < len(a['bgm']) else None
        y = b['bgm'][k] if k < len(b['bgm']) else None
        print(f'  {k}: rel27 {x}  native {y}')
    same_rev = [r for r, _ in a['reverb']] == [r for r, _ in b['reverb']]
    print(f'reverb switches: {"the same" if same_rev else "DIFFERENT"}: rel27 {a["reverb"]}  native {b["reverb"]}')
    diff = {i: (a['plays'][i], b['plays'][i]) for i in set(a['plays']) | set(b['plays']) if a['plays'][i] != b['plays'][i]}
    print(f'effects: rel27 {sum(a["plays"].values())} plays of {len(a["plays"])} ids, native '
          f'{sum(b["plays"].values())} of {len(b["plays"])}; ids played a different number of times: {len(diff)} '
          f'{dict(sorted(diff.items())[:20])}')
    print(f'  plays with no handle: rel27 {dict(a["no_handle"])}, native {dict(b["no_handle"])}')
    for n in sorted(set(a['answers']) | set(b['answers'])):
        print(f'  answers {n}: rel27 {dict(a["answers"].get(n, {}))}  native {dict(b["answers"].get(n, {}))}')
    print(f'FMOD API calls in the native run: {b["fmod"]}')
    # FMOD's stream-open callback is the old library's: the native manager has none
    ok = same_bgm and same_rev and b['fmod'] == 0 and set(a['calls']) - {'SM.openCallback'} == set(b['calls'])
    print('result:', 'agree' if ok else 'DIFFER')
    if '--json' in sys.argv:
        with open(sys.argv[sys.argv.index('--json') + 1], 'w') as f:
            json.dump({'same_music': same_bgm, 'same_reverb': same_rev, 'fmod_calls': b['fmod'],
                       'effects_diff': {str(k): v for k, v in diff.items()}, 'ok': ok}, f, indent=1)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())

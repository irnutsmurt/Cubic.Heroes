#!/usr/bin/env python3
"""audio_trace.py <trace> [--sounds <sound_inventory.json>] [--json out.json]: what the game's audio did in a run,
from a DOTHEROES_AUDIO_TRACE file (port/audio_hooks.cpp; docs/OPENAL_MIGRATION_PLAN.md, M0).

Reports, from the sound manager's calls and the FMOD calls they made:
  - calls by function;
  - music: each playBGM (the id, its file, loop or once), its stream's open and callback, the channel it got,
    fades, the channel's volume over time, how long it played;
  - effects: per id, the plays, with or without a position, and how each channel ended -- at about the
    file's length (once), past it (looped), or stopped by the game -- against the file's length and loop
    points (tools/sound_inventory.py --json);
  - reverb on/off, ducking, pause; the listener's positions;
  - the most channels playing at once.
A channel's end is the first FMOD isPlaying that answers 0 (or an error: a handle no longer valid), or its
stop; how long it played is up to the last isPlaying that answered 1 (a stop can come long after the sound
ended by itself). FMOD's state moves at the game's 10 Hz System::update, so ends are known to about 0.1 s.
"""
import json
import re
import sys
from collections import Counter, defaultdict

LINE = re.compile(r'^(\d+) (\d+) (\d+) (\S+)(.*?) -> ([0-9A-F]+)(.*)$')


def parse(path):
    for raw in open(path, encoding='utf-8', errors='replace'):
        if raw.startswith('#'):
            continue
        m = LINE.match(raw.rstrip('\n'))
        if not m:
            continue
        us, frame, thread, name, args, ret, outs = m.groups()
        this = None
        a = args.strip()
        if a.startswith('this='):
            this, _, a = a[5:].partition(' ')
        out = re.findall(r'out=(\S+)', outs)
        yield {'t': int(us) / 1e6, 'frame': int(frame), 'thread': int(thread), 'name': name, 'this': this,
               'args': split_args(a), 'ret': int(ret, 16), 'out': out}


def split_args(a):
    out, cur, depth, quote = [], '', 0, False
    for ch in a:
        if ch == '"':
            quote = not quote
        if ch == '(' and not quote:
            depth += 1
        if ch == ')' and not quote:
            depth -= 1
        if ch == ' ' and not depth and not quote:
            if cur:
                out.append(cur)
            cur = ''
        else:
            cur += ch
    if cur:
        out.append(cur)
    return out


def main():
    args = sys.argv[1:]
    path = args[0]
    sounds = None
    if '--sounds' in args:
        sounds = json.load(open(args[args.index('--sounds') + 1], encoding='utf-8'))
    jout = args[args.index('--json') + 1] if '--json' in args else None
    ev = list(parse(path))
    ev.sort(key=lambda e: e['t'])
    entries = {e['id']: e for e in sounds['entries']} if sounds else {}
    files = sounds['files'] if sounds else {}

    def fname(i):
        e = entries.get(i)
        return e['file'] if e else '?'

    def flen(i):
        f = files.get(fname(i))
        return f.get('seconds') if f else None

    def floop(i):
        f = files.get(fname(i))
        if not f or not f.get('smpl_loops'):
            return None
        lp = f['smpl_loops'][0]
        return (lp['start'] / f['rate'], (lp['end'] + 1) / f['rate'])

    counts = Counter(e['name'] for e in ev)
    t0 = ev[0]['t'] if ev else 0
    print('%s: %d calls over %.1f s, %d game frames' % (path, len(ev), (ev[-1]['t'] - t0) if ev else 0,
                                                       (ev[-1]['frame'] - ev[0]['frame']) if ev else 0))
    print('calls: ' + ', '.join('%s %d' % kv for kv in counts.most_common()))

    # channels: start (playSound's out), the sound, the volume set over time, the end
    chan = {}
    sound_of_stream = {}
    for e in ev:
        n = e['name']
        if n == 'FMOD.System.createStream' and e['out']:
            sound_of_stream[e['out'][0]] = e['args'][0].strip('"').rsplit('/', 1)[-1]
        if n == 'FMOD.System.playSound' and e['ret'] == 0 and e['out']:
            ch = e['out'][0]
            chan[ch] = {'start': e['t'], 'sound': e['args'][1], 'end': None, 'how': None, 'vol': [], 'alive': e['t'],
                        'kind': 'bgm' if e['args'][1] in sound_of_stream else 'se', 'id': None, 'pos': None}
        if n == 'FMOD.Channel.setVolume' and e['this'] in chan:
            chan[e['this']]['vol'].append((e['t'], float(e['args'][0])))
        if n == 'FMOD.Channel.stop' and e['this'] in chan and chan[e['this']]['end'] is None:
            chan[e['this']]['end'], chan[e['this']]['how'] = e['t'], 'stopped'
        if n == 'FMOD.Channel.isPlaying' and e['this'] in chan and chan[e['this']]['end'] is None:
            if e['ret'] == 0 and e['out'] and e['out'][0] != '0':
                chan[e['this']]['alive'] = e['t']
            if e['ret'] != 0 or (e['out'] and e['out'][0] == '0'):
                chan[e['this']]['end'], chan[e['this']]['how'] = e['t'], 'ended'
    # the effects' ids and positions (SM.playSE returns the channel)
    for e in ev:
        if e['name'] in ('SM.playSE', 'SM.playSEFadeIn'):
            ch = '%X' % e['ret']
            if ch in chan:
                chan[ch]['id'] = int(e['args'][0])
                chan[ch]['pos'] = e['args'][2] if e['name'] == 'SM.playSE' and len(e['args']) > 2 else None
    # music
    print('\nmusic:')
    for e in ev:
        n = e['name']
        if n == 'SM.playBGM':
            i = int(e['args'][0])
            print('  %8.2f  playBGM %d %s (%s)' % (e['t'] - t0, i, fname(i), 'loop' if e['args'][2] != '0' else 'once'))
        elif n == 'SM.fadeOutBGM':
            print('  %8.2f  fadeOutBGM %s -> %d' % (e['t'] - t0, e['args'][0], e['ret']))
        elif n in ('SM.duckBGM', 'SM.unduckBGM', 'SM.setBGMMaster'):
            print('  %8.2f  %s %s' % (e['t'] - t0, n[3:], ' '.join(e['args'])))
        elif n == 'SM.openCallback':
            print('  %8.2f  stream ready (callback, thread %d)' % (e['t'] - t0, e['thread']))
    for ch, c in sorted(chan.items(), key=lambda kv: kv[1]['start']):
        if c['kind'] != 'bgm':
            continue
        f = sound_of_stream.get(c['sound'], '?')
        v = c['vol']
        lifetime = (c['end'] or ev[-1]['t']) - c['start']
        print('  channel %s %s: from %.2f, %s %.2f s; volume %s' % (
            ch, f, c['start'] - t0, c['how'] or 'still playing after', lifetime,
            ('%.3f .. %.3f (%d sets; last %.3f at %.2f)' % (min(x for _, x in v), max(x for _, x in v), len(v), v[-1][1], v[-1][0] - t0)) if v else '-'))
    # effects by id
    print('\neffects:')
    byid = defaultdict(list)
    for ch, c in chan.items():
        if c['kind'] == 'se':
            byid[c['id']].append(c)
    rows = []
    for i, cs in sorted(byid.items(), key=lambda kv: (kv[0] is None, kv[0] or 0)):
        length = flen(i) if i is not None else None
        loop = floop(i) if i is not None else None
        ends = Counter()
        lt = []
        for c in cs:
            life = c['alive'] - c['start']   # up to the last "still playing"
            lt.append(life)
            if length is not None and life > length + 0.35:
                ends['played past its length'] += 1
            elif c['how'] == 'stopped':
                ends['stopped'] += 1
            elif c['end'] is None:
                ends['playing at the end'] += 1
            else:
                ends['at about its length'] += 1
        pos = sum(1 for c in cs if c['pos'] not in (None, 'null'))
        rows.append({'id': i, 'file': fname(i) if i is not None else None, 'plays': len(cs), 'with_position': pos,
                     'length': length, 'loop': loop, 'ends': dict(ends), 'lifetime_max': max(lt)})
        print('  %4s %-30s plays %4d (positioned %4d)  file %s%s  ends: %s  longest %.2f s' % (
            i, fname(i) if i is not None else '?', len(cs), pos, ('%.2f s' % length) if length else '?',
            (', loop %.2f-%.2f' % loop) if loop else '', ', '.join('%s %d' % kv for kv in ends.items()), max(lt)))
    # reverb, pause, listener
    print('\nreverb, pause, listener:')
    for e in ev:
        if e['name'] in ('SM.setReverb', 'FMOD.DSP.setBypass', 'SM.pauseById', 'SM.duckSE', 'SM.unduckSE',
                         'SM.fadeOutLoopSEs', 'SM.fadeOutSE', 'SM.stopSE'):
            print('  %8.2f  %s %s' % (e['t'] - t0, e['name'], ' '.join(e['args'])))
    lis = [e for e in ev if e['name'] == 'FMOD.System.set3DListenerAttributes']
    if lis:
        print('  listener: %d updates; first %s, last %s' % (len(lis), lis[0]['args'][1], lis[-1]['args'][1]))
    # concurrency
    marks = []
    for c in chan.values():
        marks.append((c['start'], 1))
        marks.append(((c['end'] or ev[-1]['t']), -1))
    live = peak = 0
    for _, d in sorted(marks):
        live += d
        peak = max(peak, live)
    print('\nchannels: %d started, at most %d playing at once' % (len(chan), peak))
    if jout:
        json.dump({'calls': counts, 'effects': rows,
                   'music': [{'channel': ch, 'file': sound_of_stream.get(c['sound']), 'start': c['start'] - t0,
                              'end': (c['end'] - t0) if c['end'] else None, 'how': c['how'], 'volume': c['vol']}
                             for ch, c in chan.items() if c['kind'] == 'bgm'],
                   'peak_channels': peak}, open(jout, 'w', encoding='utf-8', newline='\n'), indent=1)


if __name__ == '__main__':
    main()

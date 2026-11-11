#!/usr/bin/env python3
"""sound_inventory.py [<data/sound dir>] [--json out.json]: the game's sound list against its sound files.

Reads the player's own `sound.lua.eu` (the list the game's sound manager loads: {file, volume, 3D, min, max}
per id, ids in list order) and every .wav beside it, and reports what the OpenAL Soft path has to load: the
list's ids by kind (music = the ids before the first effect, as the game numbers them; effects; the empty
last entry, the game's "no sound"), files referenced more than once, listed files missing from the disc,
files on the disc no id names, and each file's format (RIFF chunks, PCM format, channels, rate, bits,
length). Nothing is written unless --json is given (the report as data, for the loader's tests).

Default directory: build/disc/PS3_GAME/USRDIR/data/sound (the player's extracted disc).
"""
import json
import os
import re
import struct
import sys

ENTRY = re.compile(r'\{\s*"([^"]*)"\s*,\s*([-0-9.]+)\s*,\s*([01])\s*,\s*([-0-9.]+)\s*,\s*([-0-9.]+)\s*\}')


def parse_list(path):
    text = open(path, 'rb').read().decode('utf-8', errors='replace')
    fade = re.search(r'BGM_FadeTime\s*=\s*([-0-9.]+)', text)
    body = text[text.index('List'):]
    # comments out: a commented-out entry is not in the list
    body = '\n'.join(line.split('--', 1)[0] for line in body.splitlines())
    out = []
    for m in ENTRY.finditer(body):
        out.append({'id': len(out), 'file': m.group(1), 'volume': float(m.group(2)), 'is3d': m.group(3) == '1',
                    'min': float(m.group(4)), 'max': float(m.group(5))})
    return out, (float(fade.group(1)) if fade else None)


def wav_info(path):
    d = open(path, 'rb').read()
    if d[:4] != b'RIFF' or d[8:12] != b'WAVE':
        return {'error': 'not RIFF/WAVE'}
    pos, chunks, info = 12, [], {}
    while pos + 8 <= len(d):
        cid = d[pos:pos + 4].decode('latin-1')
        size, = struct.unpack('<I', d[pos + 4:pos + 8])
        chunks.append(cid)
        if cid == 'fmt ':
            fmt, ch, rate, _, align, bits = struct.unpack('<HHIIHH', d[pos + 8:pos + 24])
            info.update(format=fmt, channels=ch, rate=rate, bits=bits, align=align)
        elif cid == 'data':
            info['data_bytes'] = size
        elif cid == 'smpl' and size >= 36:
            n, = struct.unpack('<I', d[pos + 8 + 28:pos + 8 + 32])
            loops = []
            for k in range(n):
                q = pos + 8 + 36 + 24 * k
                if q + 24 <= pos + 8 + size:
                    cue, typ, start, end, frac, count = struct.unpack('<6I', d[q:q + 24])
                    loops.append({'type': typ, 'start': start, 'end': end, 'count': count})
            info['smpl_loops'] = loops
        elif cid == 'cue ' and size >= 4:
            info['cue_points'], = struct.unpack('<I', d[pos + 8:pos + 12])
        pos += 8 + size + (size & 1)
    info['chunks'] = chunks
    if 'data_bytes' in info and info.get('align'):
        info['frames'] = info['data_bytes'] // info['align']
        info['seconds'] = round(info['frames'] / info['rate'], 3)
    return info


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    jpath = sys.argv[sys.argv.index('--json') + 1] if '--json' in sys.argv else None
    if jpath in args:
        args.remove(jpath)
    root = args[0] if args else os.path.join('build', 'disc', 'PS3_GAME', 'USRDIR', 'data', 'sound')
    entries, fade = parse_list(os.path.join(root, 'sound.lua.eu'))
    files = sorted(f for f in os.listdir(root) if f.lower().endswith('.wav'))
    fmt = {f: wav_info(os.path.join(root, f)) for f in files}
    # the game's music ids: the list's first section, up to the first effect (an empty entry inside it is
    # still a music id with no file)
    first_se = next(e['id'] for e in entries if e['file'] and not e['file'].startswith('bgm_'))
    named = [e for e in entries if e['file']]
    refs = {}
    for e in named:
        refs.setdefault(e['file'], []).append(e['id'])
    dup = {f: ids for f, ids in refs.items() if len(ids) > 1}
    missing = [e for e in named if e['file'] not in fmt]
    unlisted = [f for f in files if f not in refs]
    music = [e for e in named if e['id'] < first_se]
    effects = [e for e in named if e['id'] >= first_se]
    empty = [e['id'] for e in entries if not e['file']]

    print('list: %d entries (ids 0-%d), BGM_FadeTime %s' % (len(entries), len(entries) - 1, fade))
    print('  music ids 0-%d: %d named; effect ids %d-%d: %d named; empty entries (no file): %s' % (
        first_se - 1, len(music), first_se, len(entries) - 1, len(effects), empty))
    print('  3D: %d, 2D: %d (of the named)' % (sum(e['is3d'] for e in named), sum(not e['is3d'] for e in named)))
    print('files: %d .wav; distinct files named by the list: %d' % (len(files), len(refs)))
    print('  named by more than one id: %s' % (dup or 'none'))
    print('  named but missing: %s' % ([(e['id'], e['file']) for e in missing] or 'none'))
    print('  on the disc, named by no id: %s' % (unlisted or 'none'))
    kinds = {}
    for f in files:
        i = fmt[f]
        k = '%s, %d ch, %d Hz, %d bit, chunks %s' % ('PCM' if i.get('format') == 1 else 'format %s' % i.get('format'),
                                                     i.get('channels', 0), i.get('rate', 0), i.get('bits', 0),
                                                     '+'.join(c.strip() for c in i.get('chunks', [])))
        kinds.setdefault(k, []).append(f)
    print('formats:')
    for k, fs in sorted(kinds.items(), key=lambda kv: -len(kv[1])):
        print('  %4d  %s' % (len(fs), k))
    def total(es):
        seen = {e['file'] for e in es if e['file'] in fmt}
        return sum(fmt[f]['data_bytes'] for f in seen), len(seen)
    mb, mn = total(music)
    eb, en = total(effects)
    print('music: %d distinct files, %.1f MB of samples; effects: %d distinct files, %.1f MB' % (mn, mb / 1e6, en, eb / 1e6))
    print('effects not mono 44.1 kHz: %s' % ([(e['id'], e['file'], fmt[e['file']]['channels'], fmt[e['file']]['rate'], e['is3d'])
                                              for e in effects if e['file'] in fmt and (fmt[e['file']]['channels'] != 1 or fmt[e['file']]['rate'] != 44100)]))
    print('music not stereo 44.1 kHz: %s' % ([(e['id'], e['file'], fmt[e['file']]['channels'], fmt[e['file']]['rate'])
                                             for e in music if e['file'] in fmt and (fmt[e['file']]['channels'] != 2 or fmt[e['file']]['rate'] != 44100)]))
    looped = [(f, fmt[f]['smpl_loops']) for f in files if fmt[f].get('smpl_loops')]
    print('smpl chunks: %d files; with loops: %d' % (sum('smpl' in fmt[f]['chunks'] for f in files), len(looped)))
    for f, loops in looped:
        ids = refs.get(f, ['unlisted'])
        print('  %-34s ids %-8s frames %9d  loops %s' % (f, ids, fmt[f].get('frames', 0),
              ', '.join('type %d %d-%d x%d' % (l['type'], l['start'], l['end'], l['count']) for l in loops)))
    if jpath:
        json.dump({'fade_time': fade, 'first_effect_id': first_se, 'entries': entries,
                   'files': fmt, 'duplicates': dup, 'missing': [e['id'] for e in missing], 'unlisted': unlisted},
                  open(jpath, 'w', encoding='utf-8', newline='\n'), indent=1)


if __name__ == '__main__':
    main()

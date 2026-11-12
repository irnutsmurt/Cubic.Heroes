#!/usr/bin/env python3
"""save_diff.py: which saves a run wrote (docs/SAVE_UI.md, "Tests"): every save folder of <before> (a checkpoint, the
copies a drive started from) against the same folder in <after> (the run's savedata), file by file (SHA-256):
the folders changed, added and removed, and for each changed one its files.

  save_diff.py <before dir> <after dir> [--expect <folder>[,<folder>...]] [--ignore <folder>[,...]]

With --expect: exit 1 unless exactly those folders changed or were added (the ignored ones -- the title's own
system data, say -- are reported but not counted)."""
import hashlib
import os
import sys


def tree(root):
    out = {}
    if not os.path.isdir(root):
        return out
    for d in sorted(os.listdir(root)):
        p = os.path.join(root, d)
        if not os.path.isdir(p):
            continue
        files = {}
        for f in sorted(os.listdir(p)):
            fp = os.path.join(p, f)
            if os.path.isfile(fp):
                with open(fp, 'rb') as h:
                    files[f] = hashlib.sha256(h.read()).hexdigest()
        out[d] = files
    return out


def main():
    a = sys.argv[1:]
    expect = ignore = None
    for opt in ('--expect', '--ignore'):
        if opt in a:
            i = a.index(opt)
            v = set(x for x in a[i + 1].split(',') if x)
            del a[i:i + 2]
            if opt == '--expect':
                expect = v
            else:
                ignore = v
    if len(a) != 2:
        sys.exit(__doc__)
    ignore = ignore or set()
    before, after = tree(a[0]), tree(a[1])
    changed = []
    for d in sorted(set(before) | set(after)):
        if d not in after:
            print('removed  %s' % d)
            changed.append(d)
        elif d not in before:
            print('added    %s (%d files)' % (d, len(after[d])))
            changed.append(d)
        elif before[d] != after[d]:
            files = sorted(f for f in set(before[d]) | set(after[d]) if before[d].get(f) != after[d].get(f))
            print('changed  %s: %s%s' % (d, ', '.join(files), ' (ignored)' if d in ignore else ''))
            changed.append(d)
    same = [d for d in before if d in after and before[d] == after[d]]
    print('unchanged: %d folder(s)' % len(same))
    if expect is not None:
        counted = set(d for d in changed if d not in ignore)
        ok = counted == expect
        print('expected written: %s -> %s' % (', '.join(sorted(expect)) or 'none', 'yes' if ok else 'NO (%s)' % ', '.join(sorted(counted)) or 'none'))
        sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()

"""Per-function block coverage from the checker's MIXER_COVERAGE_FILE files (any number, merged).

    py -3 tools/fmod_mixer_coverage.py <cov.txt> [<cov.txt> ...]
"""
import collections
import sys

hits = collections.Counter()
owner = {}
for path in sys.argv[1:]:
    for line in open(path):
        a, f, n = line.split()
        a, f, n = int(a, 16), int(f, 16), int(n)
        owner[a] = f
        hits[a] += n
byf = collections.defaultdict(lambda: [0, 0, []])
for a, f in owner.items():
    byf[f][1] += 1
    if hits[a]:
        byf[f][0] += 1
    else:
        byf[f][2].append(a)
taken = sum(v[0] for v in byf.values())
total = sum(v[1] for v in byf.values())
print('blocks taken: %d of %d; functions entered: %d of %d; fully covered: %d' % (
    taken, total, sum(1 for v in byf.values() if v[0]), len(byf), sum(1 for v in byf.values() if v[0] == v[1])))
print('functions with blocks never taken (function: taken/total, untaken):')
for f, (t, n, un) in sorted(byf.items(), key=lambda kv: -(kv[1][1] - kv[1][0])):
    if t < n:
        print('  %05X: %d/%d  %s' % (f, t, n, ' '.join('%X' % a for a in un[:12]) + (' ...' if len(un) > 12 else '')))

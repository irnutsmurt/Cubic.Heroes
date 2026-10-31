#!/usr/bin/env python3
"""Which draws keep making new resident-geometry keys, and what changes.

  tests/geom_newkeys.py <stderr.log> [--from-frame N] [--top 12]

Reads the [geom-new] lines RSX_GEOM_CACHE_LOG=<n> writes (rsx_draw_engine.c,
eng_geom_log), groups them by what the draw is -- pipeline, first texture,
primitive, attribute formats -- and for each group says how many new keys it
made, over how many frames, and which key fields took more than one value
(index/array ranges, index array offset, vertex array offsets, base offset).
Frames before --from-frame (loading, the first sighting of everything) are
skipped.
"""
import argparse
import collections
import re

ap = argparse.ArgumentParser()
ap.add_argument("log")
ap.add_argument("--from-frame", type=int, default=0)
ap.add_argument("--top", type=int, default=12)
a = ap.parse_args()

rx = re.compile(r"^\[geom-new\] f(\d+) (\w+) k(\w+) prim (\d+) pipe (\d+) tex0 (\d+) nv (\d+) nd (\d+) "
                r"arr(.*?) idx(.*?)(?: ia (\S+))? bo (\S+) bi (\d+)(.*)$")
groups = collections.defaultdict(list)
for line in open(a.log, encoding="utf-8", errors="replace"):
    m = rx.match(line.strip())
    if not m or int(m.group(1)) < a.from_frame:
        continue
    f, what, key, prim, pipe, tex0, nv, nd, arr, idx, ia, bo, bi, attrs = m.groups()
    al = re.findall(r"a(\d+)=(\d+):(\w+)/(\d+)/(\d+)\.(\d+)", attrs)
    fmt = tuple((x[0], x[1], x[3], x[4], x[5]) for x in al)         # attr, location, stride, type, size
    groups[(pipe, tex0, prim, fmt)].append({
        "frame": int(f), "what": what, "key": key, "nv": nv, "nd": nd, "arr": arr.strip(), "idx": idx.strip(),
        "ia": ia or "", "bo": bo, "bi": bi, "offs": tuple(x[2] for x in al)})

total = sum(len(v) for v in groups.values())
print(f"{total} new keys in {len(groups)} kinds of draw (from frame {a.from_frame})")
for g, rows in sorted(groups.items(), key=lambda kv: -len(kv[1]))[:a.top]:
    frames = sorted(r["frame"] for r in rows)
    keys = len({r["key"] for r in rows})
    varying = [k for k in ("nv", "nd", "arr", "idx", "ia", "bo", "bi", "offs") if len({r[k] for r in rows}) > 1]
    whats = collections.Counter(r["what"] for r in rows)
    print(f"\n{len(rows)} new ({keys} distinct keys) over frames {frames[0]}-{frames[-1]}: pipe {g[0]} tex0 {g[1]} "
          f"prim {g[2]} attrs {len(g[3])} {dict(whats)}")
    print(f"  varying: {', '.join(varying) or 'nothing -- the same key again (evicted or rebuilt)'}")
    for k in varying[:3]:
        vals = collections.Counter(r[k] if k != "offs" else " ".join(r[k]) for r in rows)
        print(f"    {k}: {len(vals)} values, e.g. {[v for v, _ in vals.most_common(4)]}")

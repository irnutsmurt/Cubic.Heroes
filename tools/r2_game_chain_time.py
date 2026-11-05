"""R2: the existing renderer's GPU time for the PPFX chain, from its own timestamps.

r2_game_chain_time.py <capture dir> <passes.txt> [<passes.txt> ...]

passes.txt is what VK_GPU_PASSES dumps (rsx_vulkan_backend.c): every 5 s one
submission's render passes, each with its target size, draw count, start
and duration (a TOP_OF_PIPE timestamp after the pass's barrier, just before
it begins; BOTTOM_OF_PIPE when it ends), and the time before it since the
previous pass ended. The chain's passes 3-48 are found in a submission by
their target sizes in order (the capture's draws after pass 1, one draw
each); for each such submission:
  span     pass 3's start to pass 48's end: the chain as the GPU ran it,
           its barriers, the feedback copy and the gaps between passes
           included -- what the executor's segment of passes 3-48 times;
  passes   the passes' own durations, summed;
  between  span less passes.
Pass 1 (drawn before asp's scene, into the scene target) is the 1280x720
one-draw pass just before the run of asp's passes, and is reported when one
such pass stands alone; the engine may merge it with asp's first draws into
the same render pass, and then it is not separable. Prints medians and ranges
per file; writes nothing.
"""
import json
import os
import statistics
import sys


def chain_sizes(cap):
    chain = json.load(open(os.path.join(cap, "chain.json")))
    sizes = []
    for o in chain["frames"][0]["ops"]:
        if o["op"] != "draw":
            continue
        t = json.load(open(os.path.join(cap, o["dir"], "desc.json")))["target"]
        sizes.append((t["width"], t["height"]))
    return sizes


def submissions(path):
    subs, cur = [], None
    for ln in open(path, errors="replace"):
        t = ln.split()
        if not t:
            continue
        if t[0] == "cb":
            cur = []
            subs.append(cur)
        elif cur is not None and len(t) >= 11:
            w, h = (int(x) for x in t[1].split("x"))
            cur.append({"w": w, "h": h, "draws": int(t[3]), "start": float(t[5]), "dur": float(t[7]),
                        "before": float(t[9])})
    return subs


def main():
    cap = sys.argv[1]
    sizes = chain_sizes(cap)
    tail = sizes[1:]                  # passes 3-48
    for path in sys.argv[2:]:
        spans, ins, p1 = [], [], []
        for sub in submissions(path):
            n = len(tail)
            for i in range(len(sub) - n + 1):
                win = sub[i:i + n]
                if all((p["w"], p["h"]) == s and p["draws"] == 1 for p, s in zip(win, tail)):
                    span = win[-1]["start"] + win[-1]["dur"] - win[0]["start"]
                    spans.append(span)
                    ins.append(sum(p["dur"] for p in win))
                    # pass 1: alone in its render pass, just before asp's scene pass
                    if i >= 2 and (sub[i - 2]["w"], sub[i - 2]["h"]) == sizes[0] and sub[i - 2]["draws"] == 1 \
                            and (sub[i - 1]["w"], sub[i - 1]["h"]) == sizes[0] and sub[i - 1]["draws"] > 1:
                        p1.append(sub[i - 2]["dur"])
                    break
        if not spans:
            print(f"{path}: the chain's passes 3-48 not found")
            continue
        med = statistics.median
        print(f"{path}: {len(spans)} submissions with passes 3-48")
        print(f"  span (pass 3 start to pass 48 end): median {med(spans):.1f} us, range {min(spans):.1f}-{max(spans):.1f}")
        print(f"  in passes: median {med(ins):.1f} us; between: median {med([s - i for s, i in zip(spans, ins)]):.1f} us")
        if p1:
            print(f"  pass 1: {len(p1)} submissions, median {med(p1):.1f} us, range {min(p1):.1f}-{max(p1):.1f}")


if __name__ == "__main__":
    main()

"""R2: a captured PPFX chain as the executor's chain description.

r2_chain.py <capture dir>

Reads every f<frame>/ops.jsonl and the draws' desc.json of a chain capture
(tools/r2_capture.sh) and writes <capture dir>/chain.json:
  resources  each image the chain touches, named by the capture's resource
             id: size, format, levels, layers, whether a draw renders into
             it ("drawn"), and its role --
               external      read before the chain writes it in a frame, or
                             after work outside the chain rewrote it within
                             the frame, and re-supplied from outside the
                             chain each time (asp's scene target, a static
                             texture): seeded from that frame's capture,
                             every frame, where it arrives;
               history       read before the chain writes it in a frame, and
                             last written by the chain itself in an earlier
                             frame: seeded once, at the first frame, then
                             carried from the executor's own work;
               snapshot      a feedback copy the chain makes (pass 11): the
                             executor copies its source, as the game's
                             renderer does;
               intermediate  first touched by a write in each frame: produced
                             by the executor, its previous content poisoned
                             in checks;
  frames     per frame its operations in order -- copy, draw, outside (work
             outside the chain wrote these resources here: asp's scene
             between PPFX's first pass and the rest) and seed (a resource
             rewritten from outside, read next: its captured content goes in
             here) -- what to seed at the frame's start, and where the
             references are.
Reads count as: the textures a draw binds, a snapshot's source, and a draw's
target when it blends (its old content is then an input). Also checked: no
draw samples its own target (a feedback read must go through a copy), every
read-before-write has its captured content, and resources whose guest
memory overlaps are listed (the executor, like the renderer, keeps them
apart). Prints a summary.
"""
import glob
import json
import os
import sys


def load_frame(fdir):
    ops = [json.loads(l) for l in open(os.path.join(fdir, "ops.jsonl")) if l.strip()]
    head = next(o for o in ops if o["op"] == "frame")
    outside = next((o for o in ops if o["op"] == "outside_written" and "since_previous_frame" in o),
                   {"resources": [], "since_previous_frame": False})
    body = [o for o in ops if o["op"] in ("draw", "snapshot") or (o["op"] == "outside_written" and o.get("within_frame"))]
    return head, outside, body


def main():
    cap = sys.argv[1]
    fdirs = sorted(glob.glob(os.path.join(cap, "f[0-9][0-9][0-9]")))
    resources = {}
    frames = []
    written_ever = set()          # by chain draws or copies, in earlier frames
    roles = {}
    problems = []
    overlaps = set()
    programs = {}
    drawn = set()                 # resources some draw renders into: they need attachment use
    mid_external = set()          # re-supplied from outside the chain within a frame
    for fi, fdir in enumerate(fdirs):
        head, outside, body = load_frame(fdir)
        out_w = set(outside["resources"]) if outside.get("since_previous_frame") else None
        written = set()
        rbw = {}                  # resource -> the op index that reads it first, unwritten
        ops = []
        guest = {}
        gen = {}                  # resource -> times work outside the chain rewrote it in this frame
        seeded_gen = {}           # resource -> the generation already seeded mid-frame

        def read(r, k):
            """A read of r by op k: unwritten in this frame, it is an input --
            at the frame's start, or, rewritten from outside the chain since,
            seeded right here (a seed op before this one)."""
            if r in written:
                return
            g = gen.get(r, 0)
            if g == 0:
                rbw.setdefault(r, k)
                return
            if seeded_gen.get(r) == g:
                return
            f_in = os.path.join(fdir, f"input_r{r}_g{g}.bin")
            if not os.path.exists(f_in):
                problems.append(f"{os.path.basename(fdir)}: resource {r} rewritten from outside and read, but not captured")
            ops.append({"op": "seed", "resource": r, "file": os.path.relpath(f_in, cap).replace("\\", "/"),
                        "generation": g})
            seeded_gen[r] = g
            mid_external.add(r)

        for k, o in enumerate(body):
            if o["op"] == "outside_written":
                for r in o["resources"]:
                    gen[r] = gen.get(r, 0) + 1
                    written.discard(r)
                ops.append({"op": "outside", "resources": o["resources"]})
                continue
            if o["op"] == "snapshot":
                src, dst = o["src"], o["dst"]
                read(src, k)
                ops.append({"op": "copy", "src": src, "dst": dst, "for_draw": o["for_draw"]})
                written.add(dst)
                roles[dst] = "snapshot"
                continue
            ddir = os.path.join(fdir, o["dir"])
            desc = json.load(open(os.path.join(ddir, "desc.json")))
            t = desc["target"]
            tr = desc["target_resource"]
            drawn.add(tr)
            resources.setdefault(tr, {k2: t[k2] for k2 in ("width", "height", "format", "levels", "layers",
                                                           "bytes_per_texel", "bytes_per_block")})
            guest[tr] = desc["provenance"]["rt_offset"]
            tex = {}
            for x in desc["textures"]:
                r = x["resource"]
                im = x["image"]
                resources.setdefault(r, {k2: im[k2] for k2 in ("width", "height", "format", "levels", "layers",
                                                              "bytes_per_texel", "bytes_per_block")})
                guest.setdefault(r, x.get("guest_offset"))
                tex[str(x["unit"])] = r
                if r == tr:
                    problems.append(f"{o['dir']} of {os.path.basename(fdir)} samples its own target {r}")
                read(r, k)
            blend = any(a["enable"] for a in desc["blend"]["attachments"])
            if blend:
                read(tr, k)
            written.add(tr)
            key = (os.path.getsize(os.path.join(ddir, "fp.ucode")), open(os.path.join(ddir, "ps.hlsl"), "rb").read())
            programs.setdefault(key, []).append(f"{os.path.basename(fdir)}/{o['dir']}")
            ops.append({"op": "draw", "dir": os.path.relpath(ddir, cap).replace("\\", "/"), "target": tr,
                        "textures": tex, "blend": blend})
        seed = {}
        carried = []
        for r in sorted(rbw):
            f_in = os.path.join(fdir, f"input_r{r}.bin")
            if roles.get(r) == "snapshot":
                continue
            external = fi == 0 or r not in written_ever or (out_w is not None and r in out_w)
            if external or fi == 0:
                if not os.path.exists(f_in):
                    problems.append(f"{os.path.basename(fdir)}: resource {r} is read before written, but not captured")
                seed[str(r)] = os.path.relpath(f_in, cap).replace("\\", "/")
            if fi > 0 and not external:
                carried.append(r)
                roles[r] = "history"
            elif r not in written_ever and roles.get(r) not in ("history",):
                roles.setdefault(r, "external")
            elif out_w is not None and r in out_w:
                roles[r] = "external"
        for r in written:
            roles.setdefault(r, "intermediate")
        # overlapping guest memory among distinct resources of this frame
        spans = []
        for r, g in guest.items():
            if g is None:
                continue
            spec = resources[r]
            size = spec["width"] * spec["height"] * max(spec["bytes_per_texel"], 1)
            spans.append((g, g + size, r))
        spans.sort()
        for i in range(len(spans)):
            for j in range(i + 1, len(spans)):
                if spans[j][0] >= spans[i][1]:
                    break
                overlaps.add((spans[i][2], spans[j][2]))
        frames.append({"index": fi, "dir": os.path.basename(fdir), "engine_frame": head["engine_frame"],
                       "outside_written": sorted(out_w) if out_w is not None else None,
                       "seed": seed, "carried": carried, "read_before_write": sorted(rbw), "ops": ops})
        written_ever |= written
    for r in mid_external:
        roles[r] = "external"
    for r, spec in resources.items():
        spec["role"] = roles.get(r, "external")
        spec["drawn"] = r in drawn
    chain = {"resources": {str(r): s for r, s in sorted(resources.items())}, "frames": frames,
             "distinct_programs": len(programs), "overlapping_guest_memory": sorted(overlaps), "problems": problems}
    with open(os.path.join(cap, "chain.json"), "w") as f:
        json.dump(chain, f, indent=1)
    by_role = {}
    for r, s in resources.items():
        by_role.setdefault(s["role"], []).append(r)
    print(f"{cap}: {len(frames)} frame(s), {len(resources)} resources, "
          f"{sum(1 for o in frames[0]['ops'] if o['op'] == 'draw')} draws and "
          f"{sum(1 for o in frames[0]['ops'] if o['op'] == 'copy')} copies a frame, {len(programs)} distinct programs")
    for role in ("external", "history", "snapshot", "intermediate"):
        print(f"  {role}: {sorted(by_role.get(role, []))}")
    for fr in frames:
        mid = [(k, o["resource"]) for k, o in enumerate(fr["ops"]) if o["op"] == "seed"]
        outs = [(k, o["resources"]) for k, o in enumerate(fr["ops"]) if o["op"] == "outside"]
        print(f"  frame {fr['index']} (engine {fr['engine_frame']}): seeds {sorted(int(x) for x in fr['seed'])}, "
              f"carried {fr['carried']}, outside-written {fr['outside_written']}")
        if mid or outs:
            print(f"    within the frame: outside writes at {outs}, seeds at {mid}")
    print(f"  overlapping guest memory: {sorted(overlaps) or 'none'}")
    print(f"  problems: {problems or 'none'}")


if __name__ == "__main__":
    main()

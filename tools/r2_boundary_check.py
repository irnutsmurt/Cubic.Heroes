"""R2: every pass of a captured chain, what PPFX asked for against what the GPU consumed.

r2_boundary_check.py <capture dir>

tools/r1_boundary_check.py's check, over every draw of every frame of a chain
capture (tools/r2_chain.py's chain.json), with the GCM state followed
through the boundary log rather than read from one pass's calls alone -- a
pass inherits what an earlier one set, so state is the last observed call
that set it, by any caller, up to the draw:
  surface        PPFX's surface set before the draw: colour offset, size
  textures       per unit the draw samples, the last texture set on that
                 unit: format, size and offset, against the consumed image
                 and its guest offset and format
  fragment prog  the last bind's offset
  patches        every inline transfer PPFX queued into the program's bytes
                 (in this pass or an earlier one: a patch stays in local
                 memory), the latest at each offset, against the consumed
                 program at that offset
  vp constants   every register the vertex program reads, its last
                 observed write against the consumed constant block (a
                 register read but never observed written is reported)
  viewport       the last viewport set: size and depth range
  scissor        the last scissor set, clipped to the target
  draw           the vertex count
and which state no call in the pass's own window set (inherited). A chain
frame's draws are found by ring position; the log frame whose candidates
pass for every draw of the chain frame is the one the frame came from; the
chain's frames being consecutive, their log frames are too (the ring layout
repeats every 3 frames and the display buffers every 2, so one candidate
alone can be a lap off).
Writes <capture dir>/boundary_chain.json and prints a summary.
"""
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from r1_boundary_check import IO_BASE, PPFX, be32, load, surface, texture  # noqa: E402

DRAW, SURF, TEX, FPBIND, XFER, VPC = 0x006A1E28, 0x0069F944, 0x0069F2B8, 0x0069E99C, 0x0069FC38, 0x006A39EC
VIEWPORT, SCISSOR = 0x0069E7EC, 0x006A674C
CG_COMPONENTS = {1045: 1, 1046: 2, 1047: 3, 1048: 4}   # CG_FLOAT, CG_FLOAT2, CG_FLOAT3, CG_FLOAT4
SET_BY = {0x006A5EC0: "colour mask", 0x006A6E30: "blend enable", 0x006A7124: "depth test",
          0x006A7004: "cull enable", 0x006A6DA0: "alpha test", 0x006A7244: "stencil test",
          0x006A7674: "texture filter", 0x006A7744: "texture address"}


class State:
    """The observed GCM state, call by call: textures per unit, the fragment
    program bound, the patches by local offset, vp constants, viewport and
    scissor -- the last call that set each."""

    def __init__(self):
        self.tex, self.patch, self.vpc = {}, {}, {}
        self.fp = self.view = self.sc = None

    def apply(self, c):
        a = c["addr"]
        if a == VIEWPORT:
            self.view = c
        elif a == SCISSOR:
            self.sc = c
        elif a == TEX:
            t = texture(c)
            if t:
                self.tex[t["unit"]] = t
        elif a == FPBIND:
            self.fp = c["r"][2]
        elif a == XFER:
            dst, words = c["r"][1], c["r"][3]
            src = c["blob"].get(5, (0, b""))[1][:words * 4]
            for i in range(0, len(src), 4):
                self.patch[dst + i] = src[i:i + 4]
        elif a == VPC:
            # cellGcmSetVertexProgramParameter(ctx, CgBinaryParameter*, values):
            # the parameter's type (+0) and register (+12); a CG_FLOAT..CG_FLOAT4
            # (1045..1048) writes that many components of one register
            pb = c["blob"].get(4, (0, b""))[1]
            db = c["blob"].get(5, (0, b""))[1]
            if len(pb) >= 16 and len(db) >= 16:
                ty = be32(pb, 0)
                self.vpc[be32(pb, 12)] = (db[:16], CG_COMPONENTS.get(ty), ty)


def check_draw(calls, di, ddir, desc, st):
    frame = calls[di]["frame"]
    si = di
    while si >= 0 and not (calls[si]["addr"] == SURF and PPFX[0] <= calls[si]["lr"] < PPFX[1]
                           and calls[si]["frame"] == frame):
        si -= 1
    win = calls[si:di + 1]
    tex, fp_bound, patch, vpc, view, sc = st.tex, st.fp, st.patch, st.vpc, st.view, st.sc
    ck = {}
    s = surface(calls[si]) if si >= 0 else None
    t = desc["target"]
    ck["surface"] = bool(s) and s["width"] == t["width"] and s["height"] == t["height"] \
        and s["color_offset"] == desc["provenance"]["rt_offset"]
    bad_tex = []
    for x in desc["textures"]:
        u, im = x["unit"], x["image"]
        b = tex.get(u)
        if not b or b["width"] != im["width"] or b["height"] != im["height"] or b["offset"] != x.get("guest_offset") \
                or (b["format"] & 0x9F) != (x.get("guest_format", 0) & 0x9F):
            bad_tex.append(u)
    ck["textures"] = not bad_tex
    fp_off = desc["provenance"]["fp_offset"]
    ck["fragment_program"] = fp_bound == fp_off
    fp = open(os.path.join(ddir, "fp.ucode"), "rb").read()
    n_patch = n_patch_win = 0
    patch_bad = []
    win_dsts = set()
    for c in win:
        if c["addr"] == XFER:
            for i in range(c["r"][3]):
                win_dsts.add(c["r"][1] + 4 * i)
    for off, word in sorted(patch.items()):
        o = off - fp_off
        if 0 <= o < len(fp):
            n_patch += 1
            n_patch_win += off in win_dsts
            if fp[o:o + 4] != word:
                patch_bad.append(off)
    ck["patches"] = not patch_bad
    vsc = open(os.path.join(ddir, "vs_constants.bin"), "rb").read()
    vs = open(os.path.join(ddir, "vs.hlsl")).read().replace("float4 vp_c[512];", "")
    reads = sorted({int(m) for m in re.findall(r"vp_c\[(\d+)\]", vs)})
    vbad, vunobs, vtypes = [], [], set()
    for reg in reads:
        consumed = vsc[reg * 16:reg * 16 + 16]
        if reg not in vpc:
            vunobs.append(reg)
            continue
        data, n, ty = vpc[reg]
        vtypes.add(ty)
        if n is None:   # a type whose layout is not handled: reported, not passed
            vbad.append(reg)
        elif struct.pack(f"<{n}f", *struct.unpack(f">{n}f", data[:4 * n])) != consumed[:4 * n]:
            vbad.append(reg)
    ck["vertex_constants"] = not vbad and not vunobs and bool(reads)
    vpj, scj = desc["viewport"], desc["scissor"]
    ck["viewport"] = view is not None and [view["r"][3], view["r"][4]] == [int(vpj[2]), int(vpj[3])]         and [view["f"][0], view["f"][1]] == [vpj[4], vpj[5]] and [view["r"][1], view["r"][2]] == [int(vpj[0]), int(vpj[1])]
    if sc is not None:
        x, y, w, h = sc["r"][1], sc["r"][2], sc["r"][3], sc["r"][4]
        want = [x, y, min(w, t["width"] - x), min(h, t["height"] - y)]
        ck["scissor"] = want == [int(v) for v in scj]
    else:
        ck["scissor"] = False
    ck["draw"] = calls[di]["r"][3] == desc["draw"]["vertex_count"]
    called = {SET_BY[c["addr"]] for c in win if c["addr"] in SET_BY}
    return {"log_frame": frame, "checks": ck, "ok": all(ck.values()),
            "detail": {"textures_differing": bad_tex, "patched_words_in_program": n_patch,
                       "of_them_queued_in_this_pass": n_patch_win, "patched_words_differing": patch_bad,
                       "vp_registers_read": reads, "vp_registers_differing": vbad,
                       "vp_registers_unobserved": vunobs, "vp_parameter_types": sorted(vtypes),
                       "inherited": sorted(set(SET_BY.values()) - called)}}


def main():
    cap = sys.argv[1]
    calls = load(os.path.join(cap, "boundary.txt"))
    chain = json.load(open(os.path.join(cap, "chain.json")))
    out = {"capture": os.path.basename(os.path.normpath(cap)), "frames": []}
    all_ok = True
    # every chain draw, by the ring position its draw-arrays call has
    jobs, by_ring = [], {}
    for k, fr in enumerate(chain["frames"]):
        for o in fr["ops"]:
            if o["op"] != "draw":
                continue
            ddir = os.path.join(cap, o["dir"])
            desc = json.load(open(os.path.join(ddir, "desc.json")))
            by_ring.setdefault(desc["provenance"]["fifo_get"] - 0x20, []).append(len(jobs))
            jobs.append((k, o, ddir, desc))
    # one pass over the log: each candidate checked against the state just before it
    per = [{} for _ in chain["frames"]]
    st = State()
    for di, c in enumerate(calls):
        if c["addr"] == DRAW and PPFX[0] <= c["lr"] < PPFX[1]:
            for j in by_ring.get(c["ring"] - IO_BASE, ()):
                k, o, ddir, desc = jobs[j]
                r = check_draw(calls, di, ddir, desc, st)
                r["draw"] = o["dir"]
                per[k].setdefault(r["log_frame"], []).append(r)
        st.apply(c)
    found = []
    for k, fr in enumerate(chain["frames"]):
        draws = [o for o in fr["ops"] if o["op"] == "draw"]
        per_lf = per[k]
        whole = sorted(lf for lf, rs in per_lf.items() if len(rs) == len(draws) and all(r["ok"] for r in rs))
        found.append((fr, draws, per_lf, whole))
    # consecutive chain frames are consecutive log frames: the offset every frame agrees on
    offs = None
    for k, (fr, _, _, whole) in enumerate(found):
        o = {lf - k for lf in whole}
        offs = o if offs is None else offs & o
    base = min(offs) if offs and len(offs) == 1 else None
    for k, (fr, draws, per_lf, whole) in enumerate(found):
        best = base + k if base is not None else max(per_lf, key=lambda lf: (len(per_lf[lf]),
                                                                            sum(r["ok"] for r in per_lf[lf])),
                                                     default=None)
        rs = per_lf.get(best, [])
        ok = base is not None and best in whole
        all_ok &= ok
        inherited = sorted({k for r in rs for k in r["detail"]["inherited"]})
        out["frames"].append({"frame": fr["index"], "draws": len(draws), "log_frame": best,
                              "log_frames_passing_all": whole, "unique_consecutive_match": base is not None, "ok": ok,
                              "checks_passing": {k: sum(r["checks"][k] for r in rs) for k in (rs[0]["checks"] if rs else {})},
                              "inherited_somewhere": inherited, "per_draw": rs})
        print(f"{cap} frame {fr['index']}: {len(draws)} draws, log frame {best}"
              f"{'' if ok else ' (NOT unique or not all passing: ' + str(whole) + ')'}: "
              + ", ".join(f"{k} {v}/{len(rs)}" for k, v in out["frames"][-1]["checks_passing"].items()))
        bad = [r for r in rs if not r["ok"]]
        for r in bad[:6]:
            print(f"    {r['draw']}: " + ", ".join(k for k, v in r["checks"].items() if not v) + f" {r['detail']}")
    out["ok"] = all_ok
    with open(os.path.join(cap, "boundary_chain.json"), "w") as f:
        json.dump(out, f, indent=1)
    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()

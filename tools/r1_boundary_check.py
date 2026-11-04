"""R1: what PPFX asked for at its libgcm calls, against what the GPU consumed.

r1_boundary_check.py <capture dir> <pass name>

<capture dir>/boundary.txt is the observers' log (port/native/render/
r1_observe.cpp); <capture dir>/<pass>/ the consumption capture
(rsx_vulkan_backend.c). The draw is found in the log by where its commands
went: the draw-arrays call (0x006A1E28) whose ring position is just before
the walker's offset at the draw. The pass is the calls from PPFX's surface
set (0x0069F944, from 0x007B7424) up to it. Then, call by call, in order:
  surface        colour format, offset, pitch, size (CellGcmSurface)
  texture        per unit: format, size, pitch, offset (CellGcmTexture)
  fragment prog  the offset it is bound at
  inline xfers   the bytes each queues into the program (0x0069FC38: the
                 destination offset, the source words) -- applied over the
                 consumed program at their offsets, they must equal it
  vp constants   each register's four floats (0x006A39EC), the last write
                 winning -- against the consumed constant block
  draw           mode, first, count (0x006A1E28)
and the state the consumed draw has that no call of the pass set (inherited).
Writes <pass>/boundary_check.json. The frame the walker numbers N is not the
log's N (the two count from different starts), so a match is found by ring
position; candidates a ring lap apart are each checked, and every one has to
agree with the consumption capture for the check to pass.
"""
import json
import os
import struct
import sys

IO_BASE = 0x11100000          # the command ring's EA - IO offset (cellGcmInit ioAddr)
PPFX = (0x007497E0, 0x007D3E38)


def load(path):
    calls = []
    cur = None
    for ln in open(path, errors="replace"):
        t = ln.split()
        if not t or t[0] == "#":
            continue
        if t[0] == "C":
            cur = {"frame": int(t[1]), "addr": int(t[2], 16), "lr": int(t[3], 16), "tid": int(t[4]),
                   "ring": int(t[5], 16), "r": [int(x, 16) for x in t[6:14]],
                   "f": [float(x) for x in t[14:18]], "blob": {}}
            calls.append(cur)
        elif t[0] == "B" and cur is not None:
            cur["blob"][int(t[1])] = (int(t[2], 16), bytes.fromhex(t[3]))
    return calls


def be32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def be16(b, o):
    return struct.unpack_from(">H", b, o)[0]


def surface(c):
    b = c["blob"].get(4, (0, b""))[1]
    if len(b) < 64:
        return None
    # CellGcmSurface: type, antialias, colorFormat, colorTarget (u8), colorLocation[4] (u8),
    # colorOffset[4], colorPitch[4], depthFormat, depthLocation, _pad[2] (u8), depthOffset,
    # depthPitch (u32), width, height, x, y (u16): width at byte 52
    return {"color_format": b[2], "color_target": b[3], "color_offset": be32(b, 8), "color_pitch": be32(b, 24),
            "width": be16(b, 52), "height": be16(b, 54)}


def texture(c):
    b = c["blob"].get(5, (0, b""))[1]
    if len(b) < 24:
        return None
    # CellGcmTexture: format, mipmap, dimension, cubemap (u8), remap (u32), width, height,
    # depth (u16), location, _padding (u8), pitch, offset (u32)
    return {"unit": c["r"][1] & 0xF, "format": b[0], "mipmap": b[1], "dimension": b[2], "cubemap": b[3],
            "remap": be32(b, 4), "width": be16(b, 8), "height": be16(b, 10), "location": b[14],
            "pitch": be32(b, 16), "offset": be32(b, 20)}


def check(cap_dir, name):
    calls = load(os.path.join(cap_dir, "boundary.txt"))
    pdir = os.path.join(cap_dir, name)
    desc = json.load(open(os.path.join(pdir, "desc.json")))
    get = desc["provenance"]["fifo_get"]
    fp_off = desc["provenance"]["fp_offset"]
    fp = open(os.path.join(pdir, "fp.ucode"), "rb").read()
    vsc = open(os.path.join(pdir, "vs_constants.bin"), "rb").read()
    # The call writes the vertex-cache word (16 bytes), BEGIN, DRAW_ARRAYS and
    # END; the engine takes the draw at END, 0x20 past the call's position.
    draws = [i for i, c in enumerate(calls)
             if c["addr"] == 0x006A1E28 and PPFX[0] <= c["lr"] < PPFX[1] and get - (c["ring"] - IO_BASE) == 0x20]
    res = {"pass": name, "walker_fifo_get": get, "candidates": []}
    for di in draws:
        frame = calls[di]["frame"]
        si = di
        while si >= 0 and not (calls[si]["addr"] == 0x0069F944 and calls[si]["lr"] == 0x007B7424
                               and calls[si]["frame"] == frame):
            si -= 1
        win = calls[si:di + 1]
        out = {"log_frame": frame, "draw_ring_io": calls[di]["ring"] - IO_BASE, "calls": len(win), "checks": {}}
        ck = out["checks"]
        # surface
        s = surface(win[0])
        t = desc["target"]
        ck["surface"] = {"boundary": s, "consumed": {"width": t["width"], "height": t["height"], "format": t["format"]},
                         "rt_offset_consumed": desc["provenance"]["rt_offset"],
                         "ok": bool(s) and s["width"] == t["width"] and s["height"] == t["height"]
                               and s["color_offset"] == desc["provenance"]["rt_offset"]}
        # textures
        texs = {}
        for c in win:
            if c["addr"] == 0x0069F2B8:
                tx = texture(c)
                if tx:
                    texs[tx["unit"]] = tx
        cons = {x["unit"]: x["image"] for x in desc["textures"]}
        tok = all(u in cons and texs[u]["width"] == cons[u]["width"] and texs[u]["height"] == cons[u]["height"]
                  for u in texs) and set(texs) == set(cons)
        ck["textures"] = {"boundary": texs, "consumed_units": sorted(cons), "ok": tok}
        # fragment program bind
        binds = [c for c in win if c["addr"] == 0x0069E99C]
        bofs = binds[-1]["r"][2] if binds else None
        ck["fragment_program"] = {"bound_offset": bofs, "consumed_offset": fp_off, "ok": bofs == fp_off}
        # inline transfers into the program
        xf = []
        for c in win:
            if c["addr"] != 0x0069FC38:
                continue
            dst, words, base = c["r"][1], c["r"][3], c["r"][4]
            src = c["blob"].get(5, (0, b""))[1][:words * 4]
            xf.append({"dst": dst, "words": words, "base": base, "bytes": src.hex()})
        patched_ok = True
        for x in xf:
            o = x["dst"] - fp_off
            x["consumed_bytes"] = fp[o:o + x["words"] * 4].hex() if 0 <= o < len(fp) else None
            x["equal"] = x["consumed_bytes"] == x["bytes"]
            patched_ok &= x["equal"]
        ck["inline_transfers"] = {"transfers": xf, "ok": patched_ok and len(xf) > 0,
                                  "note": "each queued patch's words, against the program bytes the GPU consumed at that offset"}
        # vertex program constants, in order, last write winning
        vc = {}
        for c in win:
            if c["addr"] != 0x006A39EC:
                continue
            pb = c["blob"].get(4, (0, b""))[1]
            db = c["blob"].get(5, (0, b""))[1]
            if len(pb) >= 16 and len(db) >= 16:
                vc[be32(pb, 12)] = db[:16]
        vok = True
        vlist = []
        for idx, dbytes in sorted(vc.items()):
            consumed = vsc[idx * 16:idx * 16 + 16]
            want = struct.unpack(">4f", dbytes)
            got = struct.unpack("<4f", consumed) if len(consumed) == 16 else None
            eq = got is not None and struct.pack("<4f", *want) == consumed
            vok &= eq
            vlist.append({"register": idx, "boundary": want, "consumed": got, "equal": eq})
        ck["vertex_constants"] = {"registers": vlist, "ok": vok and len(vlist) > 0}
        # draw
        d = calls[di]
        ck["draw"] = {"mode": d["r"][1], "first": d["r"][2], "count": d["r"][3],
                      "consumed_vertex_count": desc["draw"]["vertex_count"],
                      "ok": d["r"][3] == desc["draw"]["vertex_count"]}
        # state no call of the pass set: inherited
        set_by = {0x006A5EC0: "colour mask", 0x006A6E30: "blend enable", 0x006A7124: "depth test",
                  0x006A7004: "cull enable", 0x006A6DA0: "alpha test", 0x006A7244: "stencil test",
                  0x006A7674: "texture filter", 0x006A7744: "texture address"}
        called = {set_by[c["addr"]] for c in win if c["addr"] in set_by}
        blend = desc["blend"]["attachments"][0]
        out["inherited"] = {k: ("set in the pass" if k in called else "inherited") for k in set_by.values()}
        out["consumed_inherited_values"] = {
            "colour write mask": blend["write_mask"],
            "texture address (unit: u, v, w)": {str(x["unit"]): x["sampler"]["address"] for x in desc["textures"]},
            "texture filter (unit: mag, min)": {str(x["unit"]): [x["sampler"]["mag_filter"], x["sampler"]["min_filter"]]
                                                for x in desc["textures"]}}
        out["ok"] = all(v["ok"] for v in ck.values())
        res["candidates"].append(out)
    res["ok"] = bool(res["candidates"]) and all(c["ok"] for c in res["candidates"])
    with open(os.path.join(pdir, "boundary_check.json"), "w") as f:
        json.dump(res, f, indent=1)
    return res


def main():
    """r1_boundary_check.py <capture dir> <pass> [<pass> ...]: each pass, then
    the log frames where every check of every pass holds -- the frame the
    capture came from, when it is one."""
    cap = sys.argv[1]
    ok_frames = None
    for name in sys.argv[2:]:
        r = check(cap, name)
        print(f"{r['pass']}: {len(r['candidates'])} candidate frame(s) by ring position")
        for c in r["candidates"]:
            print(f"  log frame {c['log_frame']}: " + ", ".join(f"{k} {'ok' if v['ok'] else 'DIFFERS'}"
                                                             for k, v in c["checks"].items()))
            print("    inherited: " + ", ".join(k for k, v in c["inherited"].items() if v == "inherited"))
        frames = {c["log_frame"] for c in r["candidates"] if c["ok"]}
        ok_frames = frames if ok_frames is None else ok_frames & frames
    print(f"frames where every check of every pass holds: {sorted(ok_frames or [])}")
    with open(os.path.join(cap, "boundary_frames.json"), "w") as f:
        json.dump({"passes": sys.argv[2:], "frames_all_checks_hold": sorted(ok_frames or [])}, f)
    sys.exit(0 if ok_frames and len(ok_frames) == 1 else 1)


if __name__ == "__main__":
    main()

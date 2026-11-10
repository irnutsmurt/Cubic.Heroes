#!/usr/bin/env python3
"""Reference captures from RPCS3, and a check of the native renderer against them (docs/NATIVE_R8.md 9.4-9.5).

The four scenes of the 2026-11-09 comparison -- the first dungeon's lamp-lit door (the hero's sparkle), the castle's
entry hall and corridor (the floor reflections, the pillars' shadows) and the king's close-up (bloom, the shadow
map) -- each at the camera RPCS3 had, with the render targets RPCS3 wrote back read from its memory (its GDB
server: tests/rpcs3_capture.py boots it). The captures themselves are game output and stay local
(build/reference/rpcs3/, git-ignored); tests/rpcs3_reference.json, committed, says what they are and the figures the
validated build reached.

  tests/rpcs3_reference.py pack <scene> <rpcs3 dump dir> [<save dir>]
      keep the scene's RPCS3 targets (gzipped) and its save in build/reference/rpcs3/<scene>/
  tests/rpcs3_reference.py run <scene|all> --bin <dotheroes.exe> [--tag T] [NAME=value ...]
      play the scene's drive with the build (windowed, real audio, can't-die), the frame described and its targets
      dumped at the drive's trigger, then check them
  tests/rpcs3_reference.py check <scene> <our dump dir>
      compare a dump with the reference and with the accepted figures; exit 1 when a figure is worse than
      accepted (x1.5, plus a floor for noise)
  tests/rpcs3_reference.py accept <scene> <our dump dir> <build name>
      record a validated build's figures as the accepted ones (tests/rpcs3_reference.json)

Metrics: fp16 targets, the mean |difference| a channel and the share of pixels any channel of which is more than
0.004 apart (and negative components, the sparkle's sign); 8-bit targets (the frame, A8R8G8B8 in the guest), the mean
|difference| of 255 and the share more than 8 apart; the R5G6B5 reflection, the same in 8-bit units and the share
more than 16 apart; the D24 shadow map, the mean |difference| over the rows RPCS3's copy holds (512 on: its first
half is stale memory) and the share of texels within two steps of 2^-24.
"""
import glob
import gzip
import json
import os
import re
import shutil
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = os.path.join(ROOT, "tests", "rpcs3_reference.json")
REFDIR = os.path.join(ROOT, "build", "reference", "rpcs3")


def manifest():
    with open(MANIFEST, encoding="utf-8") as f:
        return json.load(f)


def load_ref(scene, name):
    with gzip.open(os.path.join(REFDIR, scene, name + ".bin.gz"), "rb") as f:
        return f.read()


def ours_file(dump, t):
    pat = "dep_0_%s_*" % t["offset"] if t["kind"] == "d24" else "tgt_0_%s_*" % t["offset"]
    fs = sorted(glob.glob(os.path.join(dump, pat)), key=lambda p: int(re.search(r"_f(\d+)\.bin$", p).group(1)))
    return fs[-1] if fs else None


def f16_rows(raw, w, h, pitch, endian):
    fmt = endian + str(w * 4) + "e"
    for y in range(h):
        yield struct.unpack(fmt, raw[y * pitch:y * pitch + w * 8])


def cmp_f16(a_raw, b_raw, t):
    w, h = t["w"], t["h"]
    tot = [0.0, 0.0, 0.0]
    far = negA = negB = 0
    for ra, rb in zip(f16_rows(a_raw, w, h, w * 8, "<"), f16_rows(b_raw, w, h, t["pitch"], ">")):
        for x in range(0, w * 4, 4):
            m = 0.0
            for k in range(3):
                a, b = ra[x + k], rb[x + k]
                d = 10.0 if (a != a or b != b) else abs(a - b)
                tot[k] += d
                m = max(m, d)
                negA += a < 0
                negB += b < 0
            far += m > 0.004
    n = w * h
    return {"mean": [round(v / n, 5) for v in tot], "far_pct": round(100.0 * far / n, 3), "neg_ours": negA, "neg_rpcs3": negB}


def cmp_argb8(a_raw, b_raw, t):
    w, h, pitch = t["w"], t["h"], t["pitch"]
    tot = [0, 0, 0]
    far = 0
    for y in range(h):
        ra = a_raw[y * w * 4:(y + 1) * w * 4]
        rb = b_raw[y * pitch:y * pitch + w * 4]
        for x in range(0, w * 4, 4):
            d0, d1, d2 = abs(ra[x] - rb[x + 1]), abs(ra[x + 1] - rb[x + 2]), abs(ra[x + 2] - rb[x + 3])
            tot[0] += d0; tot[1] += d1; tot[2] += d2
            far += max(d0, d1, d2) > 8
    n = w * h
    return {"mean": [round(v / n, 3) for v in tot], "far_pct": round(100.0 * far / n, 3)}


def rgb565(v):
    return (((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)


def cmp_r565(a_raw, b_raw, t, ours_name):
    w, h, pitch = t["w"], t["h"], t["pitch"]
    is565 = "_4_2_" in os.path.basename(ours_name)   # ours as R5G6B5_UNORM_PACK16, else RGBA8 (before 2026-11-09)
    tot = [0, 0, 0]
    far = 0
    for y in range(h):
        rb = struct.unpack(">" + str(w) + "H", b_raw[y * pitch:y * pitch + w * 2])
        ra = struct.unpack("<" + str(w) + "H", a_raw[y * w * 2:(y + 1) * w * 2]) if is565 else None
        for x in range(w):
            b = rgb565(rb[x])
            a = rgb565(ra[x]) if is565 else tuple(a_raw[(y * w + x) * 4:(y * w + x) * 4 + 3])
            d = [abs(a[k] - b[k]) for k in range(3)]
            for k in range(3):
                tot[k] += d[k]
            far += max(d) > 16
    n = w * h
    return {"mean": [round(v / n, 3) for v in tot], "far_pct": round(100.0 * far / n, 3)}


def cmp_d24(a_raw, b_raw, t, ours_name):
    w, h, pitch, y0 = t["w"], t["h"], t["pitch"], t.get("rows_from", 0)
    d24 = "_129_" in os.path.basename(ours_name)   # D24_UNORM_S8_UINT (the 24 bits low), else D32_SFLOAT
    tot = 0.0
    near = 0
    for y in range(y0, h):
        rb = struct.unpack(">" + str(w) + "I", b_raw[y * pitch:y * pitch + w * 4])
        if d24:
            ra = [(v & 0xFFFFFF) / 16777215.0 for v in struct.unpack("<" + str(w) + "I", a_raw[y * w * 4:(y + 1) * w * 4])]
        else:
            ra = struct.unpack("<" + str(w) + "f", a_raw[y * w * 4:(y + 1) * w * 4])
        for x in range(w):
            d = abs(ra[x] - (rb[x] >> 8) / 16777215.0)
            tot += d
            near += d <= 2.5 / 16777215.0   # within two steps of 2^-24
    n = w * (h - y0)
    return {"mean": round(tot / n, 9), "within_2steps_pct": round(100.0 * near / n, 3)}


def check(scene, dump, quiet=False):
    m = manifest()["scenes"][scene]
    out = {}
    for t in m["targets"]:
        f = ours_file(dump, t)
        if not f:
            out[t["name"]] = {"missing": True}
            continue
        a = open(f, "rb").read()
        b = load_ref(scene, t["name"])
        kind = t["kind"]
        if kind == "f16":
            r = cmp_f16(a, b, t)
        elif kind == "argb8":
            r = cmp_argb8(a, b, t)
        elif kind == "r565":
            r = cmp_r565(a, b, t, f)
        else:
            r = cmp_d24(a, b, t, f)
        out[t["name"]] = r
    worse = []
    acc = m.get("accepted", {})
    kinds = {t["name"]: t["kind"] for t in m["targets"]}
    # a scene's own multiple where its targets vary from run to run of one build ("tolerance": {target: x}), else 1.5
    mean_floor = {"f16": 0.002, "d24": 1e-7, "argb8": 0.25, "r565": 0.25}
    for name, r in out.items():
        a = acc.get(name)
        if r.get("missing"):
            worse.append("%s: no dump" % name)
            continue
        if not a:
            continue
        for key, val in r.items():
            adv = m.get("advisory", {}).get(name)   # a scene's measures that vary run to run with what moves in it
            if key not in a or key.startswith("neg_rpcs3") or adv == "*" or (isinstance(adv, list) and key in adv):
                continue
            ref = a[key]
            vals = val if isinstance(val, list) else [val]
            refs = ref if isinstance(ref, list) else [ref]
            for v, rv in zip(vals, refs):
                floor = {"mean": mean_floor[kinds[name]], "far_pct": 0.5, "neg_ours": 0}.get(key, 0.0)
                if key == "within_2steps_pct":
                    if v < rv - 1.0:
                        worse.append("%s %s %s < accepted %s" % (name, key, v, rv))
                elif v > rv * m.get("tolerance", {}).get(name, 1.5) + floor:
                    worse.append("%s %s %s > accepted %s" % (name, key, v, rv))
    if not quiet:
        print("scene %s (%s): ours %s" % (scene, m["about"], dump))
        for name, r in out.items():
            print("  %-9s %s" % (name, json.dumps(r)))
        print("  " + ("worse than accepted: " + "; ".join(worse) if worse else "within the accepted figures"))
    return out, worse


def pack(scene, src, save=None):
    m = manifest()["scenes"][scene]
    d = os.path.join(REFDIR, scene)
    os.makedirs(d, exist_ok=True)
    for t in m["targets"]:
        with open(os.path.join(src, t["rpcs3_file"]), "rb") as f, gzip.open(os.path.join(d, t["name"] + ".bin.gz"), "wb") as g:
            g.write(f.read())
    for extra in ("cam.bin", "area.bin"):
        if os.path.exists(os.path.join(src, extra)):
            shutil.copy(os.path.join(src, extra), d)
    if save:
        sd = os.path.join(d, "save")
        if os.path.isdir(sd):
            shutil.rmtree(sd)
        shutil.copytree(save, sd)
    print("packed", scene, "->", d)


def run(scene, binary, tag, extra):
    m = manifest()["scenes"][scene]
    dump = os.path.join(ROOT, "build", "reference", "runs", "%s-%s" % (scene, tag), "dump")
    if os.path.isdir(os.path.dirname(dump)):
        shutil.rmtree(os.path.dirname(dump))
    os.makedirs(dump)
    env = dict(os.environ)
    env.update({
        "BIN": os.path.abspath(binary), "DRIVE": os.path.join(ROOT, m["drive"]), "SAVES": os.path.join(REFDIR, scene, "save"),
        "WINDOW": "1", "VISIBLE": "1", "TOPMOST": "1", "AUDIO": "1", "KEEP_RUNS": "60", "RUN_TAG": "ref-%s-%s" % (scene, tag),
        "DOTHEROES_DEV_STATE": "1", "DOTHEROES_DEV_KEYS": "1", "DOTHEROES_DEV_CANT_DIE": "1", "DOTHEROES_PPFX_NATIVE": "1",
        "DOTHEROES_NATIVE_CD": "1", "DOTHEROES_NATIVE_SOLVER": "4,5,6", "GCM_WRAP_LAP": "1", "DOTHEROES_PRESENT_ONCE": "1",
        "PS3RECOMP_SPIRV_CACHE": "build/play-route/cache", "DOTHEROES_ASP_DESCRIBE_FRAME": "trigger", "DOTHEROES_PPFX_DUMP": dump,
    })
    for kv in extra:
        k, _, v = kv.partition("=")
        env[k] = v
    subprocess.call([sys.executable, os.path.join(ROOT, "tests", "boot_smoke.py"), str(m.get("secs", 150))], cwd=ROOT, env=env)
    runs = sorted(glob.glob(os.path.join(ROOT, "build", "test", "boot-*-ref-%s-%s" % (scene, tag))))
    if runs:
        dst = os.path.join(os.path.dirname(dump), "run")
        shutil.move(runs[-1], dst)
        st = open(os.path.join(dst, "state.txt")).read().split("\n")
        last = [ln for ln in st if ln.strip()][-1].split() if st else []
        if len(last) > 3:
            print("camera %s %s (reference %s %s)" % (last[2], last[3], m["camera"][0], m["camera"][1]))
    return check(scene, dump)


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        return 2
    if a[0] == "pack":
        pack(a[1], a[2], a[3] if len(a) > 3 else None)
        return 0
    if a[0] == "check":
        return 1 if check(a[1], a[2])[1] else 0
    if a[0] == "accept":   # accept <scene> <dump dir> <build name>: its figures become the accepted ones
        out, _ = check(a[1], a[2], quiet=True)
        m = manifest()
        m["scenes"][a[1]]["accepted"] = {k: {kk: vv for kk, vv in v.items() if kk != "neg_rpcs3"} for k, v in out.items()}
        m["scenes"][a[1]]["accepted_from"] = a[3]
        with open(MANIFEST, "w", encoding="utf-8", newline="\n") as f:
            json.dump(m, f, indent=1)
            f.write("\n")
        print("accepted", a[1], "from", a[3])
        return 0
    if a[0] == "run":
        scenes = list(manifest()["scenes"]) if a[1] == "all" else [a[1]]
        binary = a[a.index("--bin") + 1]
        tag = a[a.index("--tag") + 1] if "--tag" in a else "check"
        extra = [x for x in a[2:] if "=" in x and not x.startswith("--")]
        bad = 0
        for s in scenes:
            bad += bool(run(s, binary, tag, extra)[1])
        return 1 if bad else 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())

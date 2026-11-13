#!/usr/bin/env python3
"""spu_census_static.py: where the game's own code can start SPU work, read in the developer's lift (M6's static
census, docs/OPENAL_MIGRATION_PLAN.md section 10). A scene that never starts an SPU program shows what that scene
does, not what the rest of the game can do; this lists every call site of the SPU-related imports (cellSpurs,
cellSync, cellAudio) and syscalls (the SPU thread groups, images, raw SPUs, the SPU event ports) in the whole
lifted executable, with each call site's callers up to a depth, each function marked: in excluded code
(config/excluded_code.json: a trap, it cannot run), answered by a hook (an entry hook declining or not,
config/*.json), or plain lifted code (it runs as the game wrote it).

  spu_census_static.py <lift dir> <imports.json> <ppu_hle_nids.cpp> [--depth N] [--config DIR]

Reads the lift (game-derived, local); prints addresses and import names only.
"""
import argparse
import collections
import glob
import json
import os
import re

SPU_LIBS = ("cellSpurs", "cellSpursJq", "cellSync", "cellSync2", "cellAudio")
# lv2 syscalls of the SPU environment (numbers as lv2 has them)
SPU_SYSCALLS = {
    156: "sys_spu_image_open", 155: "sys_spu_image_get_info", 157: "sys_spu_image_import",
    158: "sys_spu_image_close", 159: "sys_spu_image_get_modules", 160: "sys_raw_spu_create",
    161: "sys_raw_spu_destroy", 169: "sys_spu_initialize", 170: "sys_spu_thread_group_create",
    171: "sys_spu_thread_group_destroy", 172: "sys_spu_thread_initialize", 173: "sys_spu_thread_group_start",
    174: "sys_spu_thread_group_suspend", 175: "sys_spu_thread_group_resume", 176: "sys_spu_thread_group_yield",
    177: "sys_spu_thread_group_terminate", 178: "sys_spu_thread_group_join", 179: "sys_spu_thread_group_set_priority",
    181: "sys_spu_thread_write_ls", 182: "sys_spu_thread_read_ls", 184: "sys_spu_thread_write_snr",
    185: "sys_spu_thread_group_connect_event", 186: "sys_spu_thread_group_disconnect_event",
    187: "sys_spu_thread_set_spu_cfg", 188: "sys_spu_thread_get_spu_cfg", 190: "sys_spu_thread_write_spu_mb",
    191: "sys_spu_thread_connect_event", 192: "sys_spu_thread_disconnect_event",
    193: "sys_spu_thread_bind_queue", 194: "sys_spu_thread_unbind_queue",
    251: "sys_spu_thread_group_connect_event_all_threads", 252: "sys_spu_thread_group_disconnect_event_all_threads",
}
FUNC_RE = re.compile(r'^void (?:__attribute__\(\(weak\)\) )?func_([0-9A-F]{8})\(ppu_context\* ctx\) \{(.*?)^\}', re.S | re.M)
CALL_RE = re.compile(r'\bfunc_([0-9A-F]{8})\b')
SC_RE = re.compile(r'ctx->gpr\[11\] = \(int64_t\)\(int32_t\)\((0x[0-9A-F]+|\d+)\);\s*\n\s*lv2_syscall\(ctx\);')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("lift")
    ap.add_argument("imports")
    ap.add_argument("nids")
    ap.add_argument("--depth", type=int, default=3)
    ap.add_argument("--config", default=os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "config"))
    a = ap.parse_args()
    names = {int(m.group(1), 16): m.group(2) for m in re.finditer(r'ps3_hle_register\(0x([0-9A-Fa-f]+)u, "([^"]+)"',
                                                                  open(a.nids, encoding="utf-8").read())}
    stubs = {}   # stub address -> (library, name)
    for i in json.load(open(a.imports)):
        if i["library"] in SPU_LIBS:
            nid = int(i["nid"], 16)
            stubs[int(i["stub"], 16)] = (i["library"], names.get(nid, "nid_%08X" % nid))
    excl = json.load(open(os.path.join(a.config, "excluded_code.json")))
    ranges = [(int(t["lo"], 0), int(t["hi"], 0), t["what"]) for t in excl.get("traps", [])]
    inert = {int(e["addr"], 0) for e in excl.get("inert", [])}
    hooked = {}
    for path in glob.glob(os.path.join(a.config, "*.json")):
        try:
            spec = json.load(open(path))
        except (ValueError, OSError):
            continue
        for h in spec.get("entry_hooks", []) if isinstance(spec, dict) else []:
            if "addr" in h:
                hooked[int(str(h["addr"]), 0)] = os.path.basename(path) + (" (may decline)" if h.get("decline") else "")
    # the lift: each function's direct callees, its SPU syscalls
    callers = collections.defaultdict(set)
    sc_sites = collections.defaultdict(set)   # syscall -> functions
    for path in sorted(glob.glob(os.path.join(a.lift, "*.cpp"))):
        txt = open(path, encoding="utf-8", errors="replace").read()
        for m in FUNC_RE.finditer(txt):
            f, body = int(m.group(1), 16), m.group(2)
            for c in set(CALL_RE.findall(body)):
                callers[int(c, 16)].add(f)
            for s in SC_RE.findall(body):
                n = int(s, 0)
                if n in SPU_SYSCALLS:
                    sc_sites[n].add(f)

    def mark(f):
        for lo, hi, what in ranges:
            if lo <= f < hi:
                return "excluded (%s%s)" % (what, ", inert" if f in inert else "")
        if f in hooked:
            return "hooked: " + hooked[f]
        return "lifted"

    def chains(f, depth):
        """the callers' tree as lines"""
        out = []

        def walk(g, d, seen):
            for c in sorted(callers.get(g, ())):
                if c in seen:
                    continue
                out.append("    " * d + "  <- func_%08X  [%s]" % (c, mark(c)))
                if d + 1 < depth and not mark(c).startswith(("excluded", "hooked")):
                    walk(c, d + 1, seen | {c})
        walk(f, 1, {f})
        return out

    roots = collections.Counter()
    print("SPU-related imports called by the game's lifted code")
    for stub, (lib, name) in sorted(stubs.items(), key=lambda kv: kv[1]):
        direct = sorted(callers.get(stub, ()))
        live = [c for c in direct if mark(c) == "lifted" or mark(c).startswith("hooked")]
        print("%-12s %-44s stub func_%08X: %d call site(s), %d outside excluded code" % (lib, name, stub, len(direct), len(live)))
        for c in direct:
            print("  <- func_%08X  [%s]" % (c, mark(c)))
            if mark(c) == "lifted":
                for line in chains(c, a.depth):
                    print("  " + line)
            roots[mark(c).split(" ")[0]] += 1
    print("\nSPU syscalls in the game's lifted code")
    for n in sorted(sc_sites):
        fs = sorted(sc_sites[n])
        print("%3d %-44s %d function(s)" % (n, SPU_SYSCALLS[n], len(fs)))
        for f in fs:
            print("  in func_%08X  [%s]" % (f, mark(f)))
            if mark(f) == "lifted":
                for line in chains(f, a.depth):
                    print("  " + line)
    print("\ncall sites of SPU imports by the calling function's state: %s" % dict(roots))


if __name__ == "__main__":
    main()

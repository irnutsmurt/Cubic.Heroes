"""R2: a checked chain capture reduced to compact evidence and reproducible inputs.

r2_prune.py <capture dir> [--dry-run]

Only when <capture dir>/compare_chain.json (tools/r2_compare.py, the carry
run) says every output of every frame was bit-exact: each draw's reference
image (target_after.bin) is replaced by its SHA-256 in references.json, and
what the executor regenerates on every run (replay_*.hlsl, replay_*.spv,
replay_dummy.bin) and any native output left over are deleted. Kept: the
chain description (chain.json), each frame's ops.jsonl, the draws'
desc.json, programs (microcode, HLSL, SPIR-V, shadow.bin), constants,
vertices and indices, the seeds (the input_r*.bin files chain.json seeds:
what the chain reads from outside it; any other input file goes), the
boundary log (gzipped), the inventory, the run's logs, every result
(replay_chain*.json, compare_chain*.json, boundary_chain.json,
validation*.txt) and the final_sheet PNGs. r2_compare.py then checks the
executor's outputs against references.json. Prints what it removes.
"""
import gzip
import hashlib
import json
import os
import shutil
import sys

REGEN = ("replay_vs.hlsl", "replay_ps.hlsl", "replay_vs.spv", "replay_fs.spv", "replay_dummy.bin")


def main():
    cap = sys.argv[1]
    dry = "--dry-run" in sys.argv[2:]
    cmp_path = os.path.join(cap, "compare_chain.json")
    if not os.path.exists(cmp_path):
        sys.exit(f"{cap}: no compare_chain.json -- compare first")
    cmp = json.load(open(cmp_path))
    chain = json.load(open(os.path.join(cap, "chain.json")))
    if cmp["compared"] != cmp["bit_exact"] or len(cmp["frames"]) != len(chain["frames"]):
        sys.exit(f"{cap}: {cmp['bit_exact']}/{cmp['compared']} bit-exact over {len(cmp['frames'])} of "
                 f"{len(chain['frames'])} frames -- not pruned")
    refs_path = os.path.join(cap, "references.json")
    refs = json.load(open(refs_path)) if os.path.exists(refs_path) else {}
    freed = 0
    removed = 0
    for fr in chain["frames"]:
        for o in fr["ops"]:
            if o["op"] != "draw":
                continue
            ddir = os.path.join(cap, o["dir"])
            ref = os.path.join(ddir, "target_after.bin")
            if os.path.exists(ref):
                b = open(ref, "rb").read()
                refs[o["dir"] + "/target_after.bin"] = hashlib.sha256(b).hexdigest()
                freed += len(b)
                removed += 1
                if not dry:
                    os.remove(ref)
            for name in os.listdir(ddir):
                if name in REGEN or name.startswith("native_"):
                    p = os.path.join(ddir, name)
                    freed += os.path.getsize(p)
                    removed += 1
                    if not dry:
                        os.remove(p)
        fdir = os.path.join(cap, fr["dir"])
        seeded = {os.path.basename(p) for p in fr["seed"].values()} | \
                 {os.path.basename(o["file"]) for o in fr["ops"] if o["op"] == "seed"}
        for name in os.listdir(fdir):
            # an input the chain never seeds: a target's earlier content (taken
            # by captures before DOTHEROES_R2_TARGET_INPUTS), not an input
            if name.startswith("native_") or (name.startswith("input_r") and name not in seeded):
                p = os.path.join(fdir, name)
                freed += os.path.getsize(p)
                removed += 1
                if not dry:
                    os.remove(p)
    for name in os.listdir(cap):
        if name in REGEN:
            freed += os.path.getsize(os.path.join(cap, name))
            removed += 1
            if not dry:
                os.remove(os.path.join(cap, name))
    log = os.path.join(cap, "boundary.txt")   # read back through gzip (r1_boundary_check.load)
    if os.path.exists(log):
        n = os.path.getsize(log)
        if not dry:
            with open(log, "rb") as src, gzip.open(log + ".gz", "wb", compresslevel=6) as dst:
                shutil.copyfileobj(src, dst, 1 << 20)
            os.remove(log)
            n -= os.path.getsize(log + ".gz")
        freed += n
    if not dry:
        with open(refs_path, "w") as f:
            json.dump(refs, f, indent=0, sort_keys=True)
    print(f"{cap}: {'would remove' if dry else 'removed'} {removed} files, {freed / 1e9:.2f} GB "
          f"(the boundary log gzipped); {len(refs)} reference hashes in references.json")


if __name__ == "__main__":
    main()

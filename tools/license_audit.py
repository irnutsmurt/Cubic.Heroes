#!/usr/bin/env python3
"""license_audit.py [<out.md>]: every tracked file of the repository with its origin class and what is done with
it, as docs/LICENSE_AUDIT.md's table (docs/OPENAL_MIGRATION_PLAN.md section 5). The rules are by path, each
checked by reading the files' own headers; a file no rule names is the project's own (INDEP). Classes:
  BULLET     an altered version of Bullet 2.74's algorithms (zlib licence; its notice in the file), or code
             written against Bullet's own sources, which build_deps.py fetches
  THIRD      a third-party asset or text kept with its licence file
  SPUGEN     independently written generation of native code from the player's own lift: the generator, its
             template and its per-image configuration (where the image's code is, its entry, its jump tables;
             no implementation) -- the generated code exists only on the player's machine
  DOC-GAME   documentation of the game's behaviour (addresses, data layouts, what a task computes): facts read
             in the player's lift, no code
  INDEP      this project's own code, configuration, tests and documentation
Not allowed at all: code translated or generated from FMOD, code transcribed from the game, code from Sony's SDK
or firmware. The paths of the FMOD-derived files removed in M1 are kept below so that they cannot come back
unnoticed (the tool fails if one is tracked); the rest is the classification's reading of each file.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

FORBIDDEN = [
    (("port/native/fmod_", "tools/fmod_", "tools/gen_sfxreverb.py", "tests/native/fmod_"), "FMOD-IMPL"),
]
RULES = [
    (("port/native/bullet_",), "BULLET", "keep: Bullet's zlib notice in the altered files (bullet_cd.cpp, bullet_solver.cpp); "
                                          "listed in the README"),
    (("port/assets/ui/", "port/LICENSE.ps3recomp"), "THIRD", "keep: licence file beside it; listed in the README"),
    (("tools/gen_spu_native.py", "tools/spu_native_task.cpp.in", "tools/spu_native/", "port/native/spu_native"),
     "SPUGEN", "keep: generates the water's and the waver's tasks from the player's lift; nothing of an image in it"),
    (("docs/SPU_NON_AUDIO.md", "docs/BULLET_NATIVE.md", "docs/NATIVE_R", "docs/NATIVE_EVIDENCE.md"), "DOC-GAME",
     "keep: facts about the game's behaviour and the project's results"),
]


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else None
    files = [f for f in subprocess.run(["git", "ls-files"], capture_output=True, text=True, cwd=ROOT).stdout.split("\n") if f]
    bad = [(f, c) for f in files for prefixes, c in FORBIDDEN if f.startswith(prefixes)]
    rows, counts = [], {}
    for f in sorted(files):
        cls, act = "INDEP", "keep"
        for prefixes, c, a in RULES:
            if f.startswith(prefixes):
                cls, act = c, a
                break
        counts[cls] = counts.get(cls, 0) + 1
        rows.append("| `%s` | %s | %s |" % (f, cls, act))
    summary = "%d files: %s" % (len(files), ", ".join("%s %d" % kv for kv in sorted(counts.items())))
    print(summary)
    if bad:
        print("FORBIDDEN files tracked:", ", ".join("%s (%s)" % b for b in bad))
    if out_path:
        with open(out_path, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(["| File | Class | Action |", "|---|---|---|"] + rows) + "\n")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""M11.2g: the game's button prompts -- a regression check over finished runs of tests/scripts/prompts.drive
(offscreen, one run per style: LAUNCHER_PROMPTS=<style>; docs/M11_PLAN.md section 35).

  tests/prompts_check.py <run dir>[=<style>] [...]

Each run is checked from its log (stderr.log) and pictures (drive/*.png):
  - every prompt texture of the game recognised from the player's files ("40 recognised, 0 not found");
  - the words' hooks installed (the title's, the messages');
  - PlayStation: no texture redrawn (the game's own art); any other style: textures redrawn, in that style only;
  - the drive finished (no STOPPED or FAILED).
The style is taken from the run's tag (pr-<style>) unless given. Exit 0 when every run passes.
"""
import os
import re
import sys

STYLES = ("keyboard", "playstation", "xbox", "nintendo", "steam", "generic")


def check(run, style):
    bad = []
    err = open(os.path.join(run, "stderr.log"), encoding="utf-8", errors="replace").read()
    drive = open(os.path.join(run, "drive.log"), encoding="utf-8", errors="replace").read()
    m = re.search(r"\[prompts\] the game's prompt textures: (\d+) recognised from its files, (\d+) not found", err)
    if not m:
        bad.append("the prompt textures were not read")
    elif m.group(2) != "0" or int(m.group(1)) < 40:
        bad.append(f"textures: {m.group(1)} recognised, {m.group(2)} not found")
    if "hooks title, messages" not in err:
        bad.append("the words' hooks not both installed")
    redrawn = re.findall(r"\[prompts\] redrawn: (\S+) \d+x\d+ in the (\w+) style", err)
    if style == "playstation":
        if redrawn:
            bad.append(f"{len(redrawn)} textures redrawn in PlayStation's style (its own art expected)")
    else:
        if not redrawn:
            bad.append("no texture redrawn")
        other = {s for _, s in redrawn if s != style}
        if other:
            bad.append(f"redrawn in other styles: {sorted(other)}")
    if "[drive] STOPPED" in drive or "FAILED" in drive:
        bad.append("the drive did not finish")
    shots = [f for f in os.listdir(os.path.join(run, "drive")) if f.endswith(".png")] if os.path.isdir(os.path.join(run, "drive")) else []
    if len(shots) < 8:
        bad.append(f"{len(shots)} pictures")
    return bad, len(redrawn)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    failed = 0
    for a in argv[1:]:
        run, _, style = a.partition("=")
        if not style:
            m = re.search(r"pr-(\w+)$", run.rstrip("/\\"))
            style = m.group(1) if m else ""
        if style not in STYLES:
            print(f"{run}: no style (give <run>=<style>)")
            failed += 1
            continue
        bad, n = check(run, style)
        print(f"{'PASS' if not bad else 'FAIL'} {style:12s} {os.path.basename(run.rstrip('/\\'))}: {n} textures redrawn"
              + ("" if not bad else " -- " + "; ".join(bad)))
        failed += bool(bad)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

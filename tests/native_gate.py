"""The native-architecture gate on a game run (the owner's acceptance #7 for beta.6, 2026-11-25: legacy PS3 paths must
never come back): from the run's exit census (tests/boot_smoke.py result.json "census"), whether anything ran through
the machinery the native release replaced -- a SPURS task, a native replacement declining its work, an SPU program
started lifted -- and, for a run meant to fight (a drive's `#! fights`), whether the game's physics actually ran,
natively: a run left at a menu has a clean census too ("it played" is not enough; 2026-11-25, twice).

    native_gate(census, fights) -> {"ok": bool, "why": [reasons]}
"""


def native_gate(census, fights=False):
    why = []
    if not census:
        why.append("no census (the game didn't end in order)")
    else:
        for k in ("spurs_task", "native_decline", "lifted_spu_start"):
            v = census.get(k, 0)
            if v:
                why.append(f"{k} {v}")
        if fights:
            if not census.get("phys_native"):
                why.append("a fighting run with no native physics task (it never fought)")
            if census.get("phys_lifted"):
                why.append(f"phys_lifted {census['phys_lifted']}")
    return {"ok": not why, "why": why}

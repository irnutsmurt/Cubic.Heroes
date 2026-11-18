# M7: the repository's history -- the cleanup plan, prepared for the owner's approval

**Status: done 2026-11-18** (the owner's approval of this plan, the AUDIO.md and SPU_NON_AUDIO.md versions included;
the force push approved after the checks' report). Results in section 5. The SHAs this file names are the commits as
they were before the rewrite.

## 1. The issue

Between 2026-11-10 and 2026-11-11 the public history received code that does not belong in it:

- **FMOD-derived code** (ports of the middleware's mixer, codec and reverb, with their generators and checks): commits
  542794b, c2526e8, b5f5a14, e6f2912, and 3b7b168 and b3d71fc (the mixer's native glue file -- found by the checks of
  2026-11-18: the first list named four commits);
- **game-derived code** (hand transcriptions of the game's water and waver SPU tasks): commits
  9a71d05, 38a0c9b, a719cef, and the versions of those files touched by e932d0b.

4fa1a04 (2026-11-11, "Audio M1") took all of it out of the tree: the FMOD paths are gone, and the water and waver are
since generated on the player's machine from the player's own lift (the files of the same names in today's tree are
independently written hosts, classed INDEP in docs/LICENSE_AUDIT.md). `tools/boundary_check.py` keeps any of it from
coming back. But the 30 commits from 9a71d05 to 4fa1a04 still hold those versions, and so does every clone.

No release or download ever carried it (none was published before 0.2.0-beta.1, whose package is built from today's
tree and checked by the boundary and private-information checks).

## 2. What would change

- **Removed**: the file versions (git blobs) of the paths below that exist only in the commits from 9a71d05 to the
  parent of 4fa1a04 -- each such version disappears from the commits that held it. Nothing else changes: every other
  file, every commit message, author and date is kept, and **the final tree is byte for byte today's**. (A stripped
  version of a file that existed before is not replaced by nothing: the file keeps its previous version in that commit,
  as git-filter-repo drops the change. And git-filter-repo points a commit message's references to rewritten commits
  -- three messages name one, as "(3270978)" -- at their new SHAs, so they stay valid when the old ones are gone.)
  - FMOD-derived (all versions): `port/native/fmod_codec_native.cpp`, `fmod_codec_raw.cpp/.h`, `fmod_mixer.cpp/.h`,
    `fmod_sfxreverb.cpp/.h`, `fmod_mixer_native.cpp`; `tests/native/fmod_codec_raw_check.cpp`, `fmod_mixer_check.cpp`,
    `fmod_sfxreverb_check.cpp`; `tools/fmod_mixer.cpp.in`, `fmod_mixer_batch.sh`, `fmod_mixer_check.sh`,
    `fmod_mixer_config.py`, `fmod_mixer_coverage.py`, `fmod_mixer_profile.py`, `fmod_sfxreverb.cpp.in`,
    `gen_sfxreverb.py`;
  - game-derived (only their versions from that window, never today's): `port/native/water_task.cpp/.h`,
    `waver_game.cpp/.h`, `waver_task.cpp/.h`; `tests/native/water_task_check.cpp`, `waver_task_check.cpp` (the
    inflate's files, added in the same window, are independent code unchanged since: not touched);
  - for the owner's choice: the versions of `docs/AUDIO.md` and `docs/SPU_NON_AUDIO.md` from that window, which
    described the middleware's internals (recommended: include them).
- **Rewritten**: every commit from 9a71d05 to today's main -- **123 commits on 2026-11-18, more by the time it runs**;
  each gets a new SHA. The three tags (stable-2026-11-02, checkpoint-2026-11-04, r4-checkpoint-2026-11-05) are
  ancestors of 9a71d05 (checked): **they do not move.** A release tag made before the cleanup (v0.2.0-beta.1) points
  into the rewritten range and **would be moved** to its rewritten commit (its release, assets and notes stay; the
  package itself is unaffected, it was built from today's tree).
- Commit SHAs written in the docs (milestone records name many) would name the old commits: a follow-up commit
  rewrites them through the rewrite's commit map (`filter-repo` writes one).

## 3. The steps

1. **Backups** (kept until the owner says otherwise): `git clone --mirror` of the GitHub repository to
   `F:\3d dot game heroes\m7\backup-<date>.git`, and `git bundle create ... --all` beside it; the working checkout's
   local branches and stashes noted.
2. **The blob list**, computed, not typed: for each commit from 9a71d05 to 4fa1a04's parent, the blob of every path
   above; minus every blob that also appears in 4fa1a04 or any later commit (so nothing of today's tree is touched).
   Saved as `blobs.txt` and reviewed against section 2 (paths and counts) before going on.
3. **The rewrite**, in a fresh clone (never the working checkout): `git filter-repo --strip-blobs-with-ids blobs.txt
   --refs main` (git-filter-repo: installed into the checkout's venv with pip -- a download, asked for first), keeping
   its commit map.
4. **Checks** (all must pass, else nothing is pushed):
   - the rewritten main's tree is identical to today's main's tree (`git diff --stat` empty);
   - `tools/boundary_check.py` passes on the tree of every rewritten commit;
   - none of the blob IDs is reachable from any ref; `git log --all -- <the FMOD paths>` empty;
   - the commit count, messages, authors and dates unchanged (compared through the commit map);
   - the three old tags at their old SHAs;
   - `tools/push_guard.py range` on the rewritten range (nothing private).
5. **Publishing the rewrite** -- only on the owner's explicit go-ahead at this point: a force push of main (and, if it
   exists, the moved release tag), with `--force-with-lease` against the SHA checked in step 1.
6. **GitHub**: the old commits stay reachable on GitHub by their SHAs (cached views, the network graph) until GitHub
   removes them. **The owner's decision (2026-11-18): no GitHub Support request** -- the rewrite takes them out of every
   branch and tag; whoever has an old SHA may still reach it on GitHub until GitHub collects it on its own schedule.
   What keeps this from happening again is the checks that run before anything is pushed: tools/boundary_check.py in
   the pre-push hook (game-derived content), tools/license_audit.py (fails if a removed path is tracked again) and
   tools/push_guard.py (private information) -- none of which existed when the code was first pushed.
7. **Afterwards**: every clone (this PC's checkout included) is replaced by a fresh clone or reset to the rewritten main;
   the docs' SHAs updated through the commit map (a normal commit).

## 4. What the owner needs to do

1. **Approve this plan** (or say what to change: the docs' versions in or out, when to run it) -- the rewrite and its
   checks are then run and reported, and **stop before the force push**.
2. **Approve the force push** after reading the checks' report (and the moved release tag, if any).
3. ~~File the GitHub Support request~~ -- not filed, the owner's decision of 2026-11-18 (step 6).

Recommended: run it after 0.2.0-beta.1 is published (moving that one tag) or, if the owner prefers no moved tag, before
the release is tagged. Either way the published package is unaffected.

## 5. Results (2026-11-18)

- **Backups** (kept): `F:\3d dot game heroes\m7\backup-2026-11-18.git` (a mirror of GitHub before the push) and
  `backup-2026-11-18.bundle` (all refs; `git bundle verify`: okay).
- **Stripped**: 57 file versions, computed by build/m7/blobs.py (the plan's paths, their versions in the window, less any
  version also present outside it -- three kept so). The checks found one FMOD-derived file the first list missed,
  `port/native/fmod_mixer_native.cpp` (commits 3b7b168, b3d71fc); every file added in the window and absent from the
  final tree is now either stripped or confirmed independent.
- **Rewritten**: 127 commits, 9a71d05 to the previous tip; git-filter-repo `--strip-blobs-with-ids`, refs main and
  v0.2.0-beta.1 only. Three commit messages' references to other commits point at the rewritten commits.
- **Checks** (build/m7/verify.py, against the backup): the final tree identical (5a84fd970eb0); 331 commits, their
  messages, authors and dates the same; the three tags unmoved; the beta tag on its rewritten twin; none of the 57
  versions reachable; no FMOD-derived path in any commit; each rewritten commit differs only at the plan's paths; the
  boundary check on every rewritten tree; the private-information scan of the range -- all pass.
- **Pushed** with `--force-with-lease` against the backed-up values, through the pre-push hook (boundary and private
  information: nothing found): main 78a7df4 -> 8558bd2, v0.2.0-beta.1 c00feaa -> 5b395eb.
- **The remote checked** from a new clone: the same refs (the three tags and rename-cubic-heroes untouched), the tree
  identical, 331 commits, none of the 57 versions present, no FMOD-derived path.
- The docs' commit references pointed at the rewritten commits (build/m7/docs_shas.py: 5 references); the working
  checkout reset to the rewritten main.

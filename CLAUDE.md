# Project instructions

## Public material: what never goes in

Commits, commit messages, pull requests, issues, release notes, documentation, comments and any other
GitHub post are public. None of them may contain:

- links to Claude sessions or conversations (`claude.ai/code/session_...`, `claude.ai/share/...`, a
  `Claude-Session:` trailer, a PR-body session link) -- even when the session is private;
- personal email addresses (the only addresses allowed are noreply ones);
- account names, beyond the commit identity's noreply address and the repository's own name;
- local home-directory paths (`C:\Users\<name>`, `/home/<name>`, `/Users/<name>`);
- the conversation itself, or personal details.

Technical updates describe the change and the relevant test results, nothing else.

## How that is enforced

- **Attribution**: `attribution.sessionUrl` is `false` (with the commit trailer
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` and nothing more) in `.claude/settings.json`
  and the user settings. Never add a session link by hand.
- **Before every push**: the pre-push hook (`tools/githooks/pre-push`, enabled with
  `git config core.hooksPath tools/githooks`) runs `tools/push_guard.py`, which scans every outgoing
  commit's message and added lines and every tag message, and refuses the push on any finding. Never
  bypass it (`--no-verify`). Personal strings it also refuses live only in `.git/push-guard.local`, never
  in the repository. In a new clone, enable the hook before the first push.
- **Before posting text** (a PR body, an issue, release notes, a comment): write it to a file and run
  `python tools/push_guard.py text <file>` first.
- **Before pushing**, also read the outgoing commit messages (`git log origin/main..HEAD`).

## Commits

- Commit with the project's noreply identity (kept in local configuration and memory, not here); never a
  personal email address.
- Do not rewrite pushed history or move the tags (`stable-2026-11-02`, `checkpoint-2026-11-04`,
  `r4-checkpoint-2026-11-05`) without the owner's explicit approval of the specific cleanup.
- Never commit game data, extracted or generated files, keys or firmware, or the build directory.

## Architecture requirements

The final PC recompilation: native Vulkan graphics, OpenAL Soft audio, FFmpeg-based video decoding; no
proprietary-middleware implementation (FMOD, Bink) or code translated from the game, Sony's SDK or firmware in
public source or distributed artifacts -- game-derived code is generated on the player's machine from their own
files and keys; no unnecessary PS3 machinery; a stable 60 fps with minimal spikes; correct behaviour throughout.
Flag any decision that conflicts with these to the owner instead of changing the plan silently
(docs/NATIVE_PORT.md section 1, docs/OPENAL_MIGRATION_PLAN.md).

## When work is blocked on the owner

If development is blocked by a decision, an authorization, a manual test or any input from the owner, ask for
it explicitly and at once -- never only state that something "needs the owner's input". Each request says:

- what is blocked;
- why it is blocked;
- exactly what is needed from the owner (the decision, the command to run, the test to do);
- the recommended decision, where there is one.

Ask as soon as the question is identified -- never batched into an end-of-session report unless it truly depends
on the result of unfinished work. Keep working on everything that does not depend on the answer: a decision that
affects one part of the project does not stop the rest. This applies to every milestone.

## Tests stay off the desktop

Automated tests never open anything on the owner's desktop. `tests/desktop_guard.py` decides it for every runner:
without an approval in force, requests for a window (WINDOW, VISIBLE, TOPMOST, GRAB_EVERY, a scenario's `#! env`
line) are overridden, the game renders offscreen, the launcher's `--nav-test` stays hidden, a window that appears
anyway ends the run, and on-screen-only tools (RPCS3 captures) refuse to start. Ask the owner before any on-screen
run; only after their yes, for that run: `py -3 tests/desktop_guard.py allow MINUTES "WHAT"`, then `revoke`.
New runners that start the game or the launcher go through the guard (`game_env`, `launcher_env`, `Watch`).

## The recompilation boundary

The player-side recompilation boundary (README, "This repository contains no game data") is checked by
`tools/boundary_check.py`: the pre-push hook runs it on every pushed tree, `tools/make_bundle.py` on the setup's
download; run `tools/boundary_check.py tree` before staging with `git add -A`. Never weaken a rule to make a
finding go away: a finding means something game-derived is about to be published.

## Development artifacts and disk space

Cleaning up what development and testing generate is part of the work, never left to the owner (no reliance on a
reboot or a manual clean-up).

- Temporary artifacts go to disk-backed, git-ignored directories: `build/tmp` (with `TMPDIR` pointing there for
  build and test processes), never the system's RAM-backed `/tmp`; the system's `/tmp` configuration is not changed.
- Delete, once no longer needed: temporary screenshots, frame dumps, video captures, memory dumps, traces and
  diagnostic recordings; temporary test scenarios, extracted AppImages, staging directories and obsolete test packages.
- Keep only the logs, screenshots and benchmark results that document findings or reproduce unresolved bugs;
  compress large diagnostic logs that are kept.
- Check the free disk space before large compilations, packaging and long test runs.
- Clean up after each completed test batch and each milestone; check for abandoned temporary artifacts at the start
  of every session.
- Performance benchmarks run without continuous screenshot captures (`DUMP=0`, no `shot` steps) unless capture itself
  is what is tested.
- Never delete: the owner's saves or settings; original game files; active builds or designated rollback builds;
  valid shader and compilation caches; evidence needed for unresolved bugs; files of other projects or sessions.
- Milestone summaries report significant disk use and what the clean-up removed.

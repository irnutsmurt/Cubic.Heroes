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

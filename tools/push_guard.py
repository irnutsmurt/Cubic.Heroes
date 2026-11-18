#!/usr/bin/env python3
"""Keep private material out of what is pushed: Claude session links, personal email addresses, account
names, local home-directory paths.

  tools/push_guard.py pre-push <remote> <url>     (as .git/hooks/pre-push: the ref updates on stdin)
  tools/push_guard.py range <rev-range>           (scan commits, e.g. origin/main..HEAD)
  tools/push_guard.py files                       (scan the tracked files of the working tree)
  tools/push_guard.py text <file>                 (scan a text, e.g. a PR body or release notes, before posting)
  tools/push_guard.py package <dir>               (scan a package before it goes anywhere: every file, binaries as
                                                  bytes -- home-directory paths and the private literals; email
                                                  addresses are third parties' in licences and sources, not scanned)

What is scanned: every outgoing commit's message, the lines its diff adds, and annotated tags' messages.
Not scanned: the commit's author and committer headers (the project's commit identity lives there).
Blocked:
  - Claude session and share links (claude.ai/code/session_..., claude.ai/share/..., claude.ai/chat/...);
  - email addresses, except noreply ones (users.noreply.github.com, noreply@anthropic.com);
  - home-directory paths with a user name (C:\\Users\\<name>, C:/Users/<name>, /home/<name>, /Users/<name>);
  - the literal strings in .git/push-guard.local (personal names, account names: kept out of the repository).
    Lines there starting "allow " name exact strings that are public by design (the commit identity's noreply
    address, the repository's own name) and are taken out of the text before it is scanned.
Exit 1 with each finding listed (commit, where, the matched text) when anything is found."""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PATTERNS = [
    # a link names a session or a conversation: an ID follows (the rule's own text, "claude.ai/share/...", has none)
    ("Claude session link", re.compile(r"claude\.ai/code/session_[A-Za-z0-9]{6,}|claude\.ai/(?:share|chat)/[A-Za-z0-9-]{6,}", re.I)),
    ("Claude session trailer", re.compile(r"^Claude-Session:\s*\S", re.I | re.M)),
    ("email address", re.compile(r"[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}")),
    # a user name follows (the PS3's own /home/00000001 is not one)
    ("home-directory path", re.compile(r"[A-Za-z]:[\\/]+Users[\\/]+(?!Public\b|Default\b)[A-Za-z0-9._-]+"
                                       r"|/home/(?!\d{8}\b)[A-Za-z0-9._-]+|/Users/(?!Shared\b)[A-Za-z0-9._-]+", re.I)),
]
ALLOWED_EMAIL = re.compile(r"@users\.noreply\.github\.com$|^noreply@anthropic\.com$|@example\.(?:com|org)$", re.I)


def git(*a):
    return subprocess.run(["git", "-C", ROOT] + list(a), capture_output=True).stdout.decode("utf-8", "replace")


def local_lists():
    path = os.path.join(git("rev-parse", "--git-dir").strip() or ".git", "push-guard.local")
    if not os.path.isabs(path):
        path = os.path.join(ROOT, path)
    block, allow = [], []
    try:
        for ln in open(path, encoding="utf-8"):
            ln = ln.strip()
            if not ln or ln.startswith("#"):
                continue
            if ln.startswith("allow "):
                allow.append(ln[6:].strip())
            else:
                block.append(ln)
    except OSError:
        pass
    return block, allow


LITERALS, ALLOW = local_lists()


def scan_text(text, where, findings):
    for a in ALLOW:   # public by design: removed before scanning (case-insensitive)
        text = re.sub(re.escape(a), " ", text, flags=re.I)
    for name, rx in PATTERNS:
        for m in rx.finditer(text):
            s = m.group(0)
            if name == "email address" and ALLOWED_EMAIL.search(s):
                continue
            findings.append((where, name, s))
    for lit in LITERALS:
        for m in re.finditer(re.escape(lit), text, re.I):
            i = m.start()
            findings.append((where, "a private literal (.git/push-guard.local)", text[max(0, i - 20):i + len(lit) + 20].strip()))


def scan_commit(sha, findings):
    msg = git("log", "-1", "--format=%B", sha)
    scan_text(msg, f"{sha[:10]} message", findings)
    diff = git("show", "--format=", "--no-color", "-U0", "--no-ext-diff", sha)
    path = "?"
    added = []
    for ln in diff.splitlines():
        if ln.startswith("+++ "):
            path = ln[6:] if ln.startswith("+++ b/") else ln[4:]
        elif ln.startswith("+") and not ln.startswith("+++"):
            added.append((path, ln[1:]))
    by_path = {}
    for p, l in added:
        by_path.setdefault(p, []).append(l)
    for p, ls in by_path.items():
        scan_text("\n".join(ls), f"{sha[:10]} {p}", findings)


def report(findings, what):
    if not findings:
        print(f"[push-guard] {what}: nothing private found")
        return 0
    print(f"[push-guard] {what}: {len(findings)} finding(s) -- nothing pushed")
    seen = set()
    for where, name, s in findings:
        k = (where, name, s)
        if k in seen:
            continue
        seen.add(k)
        print(f"  {where}: {name}: {s[:120]}")
    print("[push-guard] remove them (amend or rewrite the unpushed commits) and push again")
    return 1


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    mode = sys.argv[1]
    findings = []
    if mode == "pre-push":
        zero = "0" * 40
        for line in sys.stdin.read().splitlines():
            parts = line.split()
            if len(parts) != 4:
                continue
            local_ref, local_sha, remote_ref, remote_sha = parts
            if local_sha == zero:
                continue   # a deletion
            if git("cat-file", "-t", local_sha).strip() == "tag":
                scan_text(git("cat-file", "-p", local_sha), f"tag {local_ref}", findings)
            rng = [local_sha, "--not", "--remotes"] if remote_sha == zero else [f"{remote_sha}..{local_sha}"]
            for sha in git("rev-list", *rng).split():
                scan_commit(sha, findings)
        return report(findings, "outgoing commits")
    if mode == "range":
        for sha in git("rev-list", sys.argv[2]).split():
            scan_commit(sha, findings)
        return report(findings, f"commits {sys.argv[2]}")
    if mode == "files":
        for p in git("ls-files", "-z").split("\0"):
            if not p:
                continue
            try:
                data = open(os.path.join(ROOT, p), "rb").read()
            except OSError:
                continue
            if b"\0" in data[:8192]:
                continue   # binary
            scan_text(data.decode("utf-8", "replace"), p, findings)
        return report(findings, "tracked files")
    if mode == "package":
        names = {"home-directory path"}
        if not LITERALS:
            print("[push-guard] package: no private literals (.git/push-guard.local): nothing to look for")
            return 2
        for d, dirs, files in os.walk(sys.argv[2]):
            for f in files:
                path = os.path.join(d, f)
                try:
                    data = open(path, "rb").read()
                except OSError:
                    continue
                rel = os.path.relpath(path, sys.argv[2])
                found = []
                # as bytes (latin-1 keeps every byte), and as UTF-16 (Windows programs keep paths that way)
                for text in (data.decode("latin-1"), data.decode("utf-16-le", "replace"),
                             data[1:].decode("utf-16-le", "replace")):
                    scan_text(text, rel, found)
                # a package carries third parties' files (their documentation's example paths, their build machines'
                # paths): what counts is ours -- a home path whose user is one of the private literals, and a private
                # literal standing as a word (not inside a longer one, as random bytes of a program can spell it)
                for where, name, hit in found:
                    if name in names and any(re.search(r"(?:Users|home)[\\/]+" + re.escape(lit) + r"\b", hit, re.I)
                                             for lit in LITERALS):
                        findings.append((where, name, hit))
                    elif name.startswith("a private literal") and any(
                            re.search(r"(?<![A-Za-z])" + re.escape(lit) + r"(?![A-Za-z])", hit, re.I)
                            for lit in LITERALS):
                        findings.append((where, name, hit))
        return report(findings, f"package {sys.argv[2]}")
    if mode == "text":
        scan_text(open(sys.argv[2], encoding="utf-8", errors="replace").read(), sys.argv[2], findings)
        return report(findings, sys.argv[2])
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())

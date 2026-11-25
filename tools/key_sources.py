#!/usr/bin/env python3
"""Where the player's keys are, and the game program they open (docs/INSTALLING.md, "Your keys").

Cubic.Heroes ships no key: the program on the player's disc is opened with what the player already has. Looked at, in this order, and nowhere else:

1. the source the player chose now (a keys file, a folder or ZIP of key files, or an EBOOT.elf they decrypted);
2. the sources they chose before (the launcher remembers where, never what);
3. the explicitly relevant places beside the disc they chose: the folder holding the disc image (for a disc folder,
   that folder and the one holding it), not searched any deeper than its own entries -- files whose names say they
   hold keys (keys, *.keys, *keys*.txt, *keys*.zip, ...) or a decrypted EBOOT.elf, and folders whose names say so;
4. an RPCS3 the player has and pointed the launcher at (optional; never required): its own `--decrypt`, run on a
   private copy of the program, without a window. Nothing of RPCS3 is part of Cubic.Heroes.

Whatever opens the program, the result is accepted only when it is exactly the pinned program (its SHA-256), so a
wrong or damaged key, file or tool can never produce a game. Keys are read in memory, never written, logged,
returned or put in an error -- results name the source's file and what was missing.
"""
import hashlib
import os
import re
import shutil
import subprocess
import tempfile

import player_keys
import self_decrypt

RPCS3_TIMEOUT_S = 180
ELF_MAX = 64 << 20
_KEYISH = re.compile(r"(?i)keys?")
_DISC_KEY_EXT = (".dkey", ".key", ".ird")   # a disc key unlocks the image, not the program: never a key source here
_SOURCE_EXT = (".txt", ".keys", ".zip", ".ini", ".cfg", "")


class NoProgram(Exception):
    """nothing given or found opens the program. code: keys_missing (no source at all), wrong_key (sources, but none
    opens it to the pinned program); need: what the program still needs (names only); tried: [{source, kind, how,
    result}]"""

    def __init__(self, message, code, need=(), tried=()):
        super().__init__(message)
        self.code, self.need, self.tried = code, list(need), list(tried)


def beside(disc):
    """the explicitly relevant places beside the disc: [path], in name order"""
    disc = os.path.abspath(disc)
    folders = [os.path.dirname(disc)] + ([disc] if os.path.isdir(disc) else [])
    out = []
    for d in folders:
        try:
            names = sorted(os.listdir(d))
        except OSError:
            continue
        for n in names:
            p = os.path.join(d, n)
            low = n.lower()
            if low.endswith(_DISC_KEY_EXT) or p == disc:
                continue
            if os.path.isfile(p):
                ext = os.path.splitext(low)[1]
                if low == "eboot.elf" or (_KEYISH.search(low) and ext in _SOURCE_EXT):
                    out.append(p)
            elif os.path.isdir(p) and (_KEYISH.search(low) or low == ".ps3"):
                out.append(p)
    return out


class Sources:
    """the key sources of one install or check: chosen (a path or None), remembered ([path]), the disc's (for the
    places beside it), rpcs3 (a path or None)"""

    def __init__(self, chosen=None, remembered=(), disc=None, rpcs3=None):
        self.chosen, self.remembered, self.disc, self.rpcs3 = chosen, list(remembered or ()), disc, rpcs3

    def __bool__(self):
        return bool(self.chosen or self.remembered or self.disc or self.rpcs3)

    def ordered(self):
        """[(path, how)] -- each path once, the chosen first"""
        seen, out = set(), []
        for p, how in ([(self.chosen, "chosen")] if self.chosen else []) + \
                [(p, "remembered") for p in self.remembered] + \
                ([(p, "beside the disc") for p in beside(self.disc)] if self.disc else []):
            k = os.path.normcase(os.path.abspath(p))
            if k not in seen and os.path.exists(p):
                seen.add(k)
                out.append((os.path.abspath(p), how))
        return out


def _try_ring(eboot, ring, want):
    elf = self_decrypt.decrypt_self(eboot, ring)
    return elf if hashlib.sha256(elf).hexdigest() == want else None


def rpcs3_decrypt(rpcs3, eboot, work_dir=None, timeout=RPCS3_TIMEOUT_S):
    """the program decrypted by the player's own RPCS3 (`rpcs3 --decrypt`, headless) from a private copy in a
    temporary folder of ours, which is removed afterwards; its output is neither shown nor kept. bytes or None."""
    d = tempfile.mkdtemp(prefix="cubic-rpcs3-", dir=work_dir)
    try:
        src = os.path.join(d, "EBOOT.BIN")
        with open(src, "wb") as f:
            f.write(eboot)
        flags = 0x08000000 if os.name == "nt" else 0   # CREATE_NO_WINDOW
        try:
            subprocess.run([rpcs3, "--decrypt", src], cwd=d, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=timeout, creationflags=flags)
        except (OSError, subprocess.SubprocessError):
            return None
        out = os.path.join(d, "EBOOT.elf")
        if not os.path.isfile(out) or os.path.getsize(out) > ELF_MAX:
            return None
        with open(out, "rb") as f:
            return f.read()
    finally:
        shutil.rmtree(d, ignore_errors=True)


def open_program(eboot, want, sources, work_dir=None):
    """the pinned program from the encrypted one (eboot: its bytes; want: its SHA-256), with the first source that
    opens it: each source alone, then all the key sources together (keys split over several), then RPCS3.
    -> (elf bytes, {source, kind, how}). Raises NoProgram."""
    tried, merged, need = [], player_keys.Keyring(), []
    for path, how in sources.ordered():
        kind = player_keys.kind_of(path)
        rec = {"source": path, "kind": kind, "how": how}
        try:
            if kind == "elf":
                if os.path.getsize(path) <= ELF_MAX:
                    with open(path, "rb") as f:
                        data = f.read()
                    if hashlib.sha256(data).hexdigest() == want:
                        return data, rec
                rec["result"], rec["status"] = "not this game's program", "no_match"
            elif kind in ("text", "folder", "zip", "keyfile"):
                ring = player_keys.load(path)      # reading errors: the source's (below)
                if not ring:
                    rec["result"], rec["status"] = "no keys in it", "empty"
                else:
                    merged.merge(ring)
                    try:
                        elf = _try_ring(eboot, ring, want)
                    except self_decrypt.MissingKeys:
                        raise
                    except ValueError:             # the program's own problem, not the keys file's
                        elf = None
                    if elf is not None:
                        return elf, rec
                    rec["result"], rec["status"] = "its keys don't open this program", "no_match"
            elif kind == "rpcs3" and not sources.rpcs3:
                sources.rpcs3 = path           # RPCS3 chosen as the source: tried last, below
                rec["result"], rec["status"] = "RPCS3 (tried last)", "rpcs3"
            else:
                rec["result"], rec["status"] = {
                    "self": ("the game's program itself, still encrypted -- not its keys", "encrypted_program"),
                    "pkg": ("a PS3 package, not keys", "package"),
                }.get(kind, ("not a keys file (not text, a ZIP, a folder of key files or an EBOOT.elf)", "not_text"))
        except self_decrypt.MissingKeys as e:
            need = e.need
            rec["result"], rec["status"] = "its keys don't open this program (no " + ", ".join(e.need) + ")", "no_match"
        except player_keys.KeysError as e:
            rec["result"], rec["status"], rec["line"] = f"can't be read: {e}", "malformed", e.line
        except UnicodeDecodeError:
            rec["result"], rec["status"] = "not a keys file (not text, a ZIP, a folder of key files or an EBOOT.elf)", \
                "not_text"
        except (OSError, ValueError) as e:
            rec["result"], rec["status"] = f"can't be read ({type(e).__name__})", "unreadable"
        tried.append(rec)
    if merged and sum(1 for t in tried if t["kind"] in ("text", "folder", "zip")) > 1:
        try:
            elf = _try_ring(eboot, merged, want)
            if elf is not None:
                return elf, {"source": "+".join(t["source"] for t in tried if t["kind"] in ("text", "folder", "zip")),
                             "kind": "keys", "how": "together"}
        except self_decrypt.MissingKeys as e:
            need = e.need
        except ValueError:
            pass
    if sources.rpcs3:
        elf = rpcs3_decrypt(sources.rpcs3, eboot, work_dir)
        rec = {"source": os.path.abspath(sources.rpcs3), "kind": "rpcs3", "how": "RPCS3"}
        if elf is not None and hashlib.sha256(elf).hexdigest() == want:
            return elf, rec
        rec["result"] = "RPCS3 didn't decrypt it" if elf is None else "RPCS3's result isn't this game's program"
        rec["status"] = "rpcs3_failed"
        tried.append(rec)
    if not need:
        try:   # what the program needs, by name, when nothing said it
            self_decrypt.decrypt_self(eboot, player_keys.Keyring())
        except self_decrypt.MissingKeys as e:
            need = e.need
        except ValueError:
            pass
    if not tried:
        raise NoProgram("No keys were given or found.", "keys_missing", need, tried)
    raise NoProgram("None of the keys given or found opens this game's program.", "wrong_key", need, tried)


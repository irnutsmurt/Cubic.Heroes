#!/usr/bin/env python3
"""The player's PS3 save key (tools/ps3_save.py), kept on their own computer by the operating system -- never in a
plain file, never logged, never in diagnostics or packages.

  Windows  Credential Manager (a generic credential "Cubic.Heroes/ps3-save-key" for the current user; Windows
           protects it with DPAPI).
  Linux    the Secret Service (GNOME Keyring, KWallet...) through libsecret's `secret-tool`.
Where neither is available, the key can only be used for the current session (kept in memory) and the player is
told so; it is never written in the clear instead.
"""
import os
import subprocess
import sys

TARGET = "Cubic.Heroes/ps3-save-key"
ATTRS = ("application", "cubic.heroes", "secret", "ps3-save-key")

_session = {}     # the current process only, for a player who chose not to keep the key


class StoreUnavailable(Exception):
    pass


# ---- Windows: Credential Manager ---------------------------------------------------------------------------

def _win():
    import ctypes
    from ctypes import wintypes

    class FILETIME(ctypes.Structure):
        _fields_ = [("dwLowDateTime", wintypes.DWORD), ("dwHighDateTime", wintypes.DWORD)]

    class CREDENTIALW(ctypes.Structure):
        _fields_ = [("Flags", wintypes.DWORD), ("Type", wintypes.DWORD), ("TargetName", wintypes.LPWSTR),
                    ("Comment", wintypes.LPWSTR), ("LastWritten", FILETIME),
                    ("CredentialBlobSize", wintypes.DWORD), ("CredentialBlob", ctypes.POINTER(ctypes.c_ubyte)),
                    ("Persist", wintypes.DWORD), ("AttributeCount", wintypes.DWORD), ("Attributes", ctypes.c_void_p),
                    ("TargetAlias", wintypes.LPWSTR), ("UserName", wintypes.LPWSTR)]
    adv = ctypes.WinDLL("advapi32", use_last_error=True)
    adv.CredWriteW.argtypes = [ctypes.POINTER(CREDENTIALW), wintypes.DWORD]
    adv.CredReadW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.POINTER(ctypes.POINTER(CREDENTIALW))]
    adv.CredDeleteW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD]
    adv.CredFree.argtypes = [ctypes.c_void_p]
    return ctypes, CREDENTIALW, adv


CRED_TYPE_GENERIC, CRED_PERSIST_LOCAL_MACHINE = 1, 2
ERROR_NOT_FOUND = 1168


def _win_save(key):
    ctypes, CREDENTIALW, adv = _win()
    blob = (ctypes.c_ubyte * len(key)).from_buffer_copy(key)
    c = CREDENTIALW()
    c.Type = CRED_TYPE_GENERIC
    c.TargetName = TARGET
    c.Comment = "Cubic.Heroes: the PS3 save key for importing characters saved on a PS3"
    c.CredentialBlobSize = len(key)
    c.CredentialBlob = blob
    c.Persist = CRED_PERSIST_LOCAL_MACHINE
    c.UserName = "Cubic.Heroes"
    if not adv.CredWriteW(ctypes.byref(c), 0):
        raise StoreUnavailable(f"Windows Credential Manager refused (error {ctypes.get_last_error()})")


def _win_load():
    ctypes, CREDENTIALW, adv = _win()
    p = ctypes.POINTER(CREDENTIALW)()
    if not adv.CredReadW(TARGET, CRED_TYPE_GENERIC, 0, ctypes.byref(p)):
        if ctypes.get_last_error() == ERROR_NOT_FOUND:
            return None
        raise StoreUnavailable(f"Windows Credential Manager unreadable (error {ctypes.get_last_error()})")
    try:
        c = p.contents
        return bytes(c.CredentialBlob[:c.CredentialBlobSize])
    finally:
        adv.CredFree(p)


def _win_forget():
    ctypes, _C, adv = _win()
    if not adv.CredDeleteW(TARGET, CRED_TYPE_GENERIC, 0) and ctypes.get_last_error() != ERROR_NOT_FOUND:
        raise StoreUnavailable(f"Windows Credential Manager refused (error {ctypes.get_last_error()})")


# ---- Linux: the Secret Service through secret-tool ----------------------------------------------------------

def _secret_tool():
    for d in os.environ.get("PATH", "").split(os.pathsep):
        p = os.path.join(d, "secret-tool")
        if os.access(p, os.X_OK):
            return p
    return None


def _lx_run(args, stdin=None):
    st = _secret_tool()
    if not st:
        raise StoreUnavailable("no Secret Service tool (secret-tool, from libsecret) on this system")
    try:
        import hostenv     # the system's own libraries, not the frozen worker's
        env = hostenv.host_env()
    except Exception:
        env = dict(os.environ)
    return subprocess.run([st] + args, input=stdin, capture_output=True, env=env, timeout=30)


def _lx_save(key):
    r = _lx_run(["store", "--label=Cubic.Heroes PS3 save key", *ATTRS], stdin=key.hex().encode())
    if r.returncode != 0:
        raise StoreUnavailable("the Secret Service refused to keep the key (is a keyring unlocked?)")


def _lx_load():
    r = _lx_run(["lookup", *ATTRS])
    if r.returncode != 0 or not r.stdout.strip():
        return None
    try:
        return bytes.fromhex(r.stdout.decode().strip())
    except ValueError:
        return None


def _lx_forget():
    _lx_run(["clear", *ATTRS])


# ---- the interface -------------------------------------------------------------------------------------------

def available():
    """(True, where) when a protected store exists here; (False, why) otherwise."""
    if sys.platform == "win32":
        return True, "Windows Credential Manager"
    if _secret_tool():
        return True, "the Secret Service (keyring)"
    return False, "no protected key store on this system: the key can be used for this session only"


def save(key, keep=True):
    """Keep the (already validated) key: in the protected store when keep, else for this session only."""
    if len(key) != 16:
        raise ValueError("a PS3 save key is 16 bytes")
    _session["key"] = bytes(key)
    if not keep:
        return "this session"
    if sys.platform == "win32":
        _win_save(key)
        return "Windows Credential Manager"
    _lx_save(key)
    return "the Secret Service (keyring)"


def load():
    """The key, or None: this session's, else the protected store's."""
    if "key" in _session:
        return _session["key"]
    try:
        k = _win_load() if sys.platform == "win32" else _lx_load()
    except StoreUnavailable:
        return None
    return k if k and len(k) == 16 else None


def forget():
    _session.pop("key", None)
    try:
        if sys.platform == "win32":
            _win_forget()
        else:
            _lx_forget()
    except StoreUnavailable:
        pass

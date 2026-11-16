#!/usr/bin/env python3
"""The developer tools' test channel (M11.0, docs/M11_PLAN.md section 3.3): commands to a running game in developer
mode (DOTHEROES_DEVELOPER_MODE=1), answers back.

The game reads <run>/dev.cmd every 50 ms and removes it; each line is a command, "@ID COMMAND" naming its answer.
Answers are JSON lines appended to <run>/dev.out: {"id": ID, "ok": true|false, "out": text or an object}. This client
writes dev.cmd whole (a temporary file renamed) only once the game has taken the previous one, and waits for its
answer by id.

    tests/devtools.py RUN_DIR COMMAND [ARGS...]      one command; its answer printed; exit 1 when refused
    tests/devtools.py RUN_DIR --wait-ready [SECS]    until `status` answers with the game's scripts loaded
"""
import itertools
import json
import os
import sys
import time

_ids = itertools.count(1)


class Refused(Exception):
    pass


class Channel:
    def __init__(self, run_dir):
        self.run = run_dir
        self.cmd = os.path.join(run_dir, "dev.cmd")
        self.out = os.path.join(run_dir, "dev.out")
        self.pos = 0
        self.prefix = f"t{os.getpid()}-"

    def _answers(self):
        try:
            with open(self.out, "rb") as f:
                f.seek(self.pos)
                data = f.read()
        except OSError:
            return []
        if not data:
            return []
        end = data.rfind(b"\n") + 1   # whole lines only
        self.pos += end
        res = []
        for ln in data[:end].decode("utf-8", "replace").splitlines():
            try:
                res.append(json.loads(ln))
            except ValueError:
                pass
        return res

    def send(self, line, timeout=30.0, alive=None):
        """the answer to one command (a dict); raises Refused when the game refuses it, TimeoutError when none comes"""
        cid = f"{self.prefix}{next(_ids)}"
        t0 = time.monotonic()
        while os.path.exists(self.cmd):   # the game has not taken the previous batch yet
            if time.monotonic() - t0 > timeout:
                raise TimeoutError(f"the game did not take dev.cmd within {timeout:g} s (developer mode on?)")
            if alive and not alive():
                raise TimeoutError("the game exited")
            time.sleep(0.05)
        tmp = self.cmd + ".tmp"
        with open(tmp, "w", encoding="utf-8", newline="\n") as f:
            f.write(f"@{cid} {line}\n")
        os.replace(tmp, self.cmd)
        while True:
            for a in self._answers():
                if a.get("id") == cid:
                    if not a.get("ok"):
                        raise Refused(a.get("out"))
                    return a
            if time.monotonic() - t0 > timeout:
                raise TimeoutError(f"no answer to {line!r} within {timeout:g} s")
            if alive and not alive():
                raise TimeoutError("the game exited")
            time.sleep(0.05)

    def status(self, timeout=30.0, alive=None):
        out = self.send("status", timeout, alive)["out"]
        return out if isinstance(out, dict) else json.loads(out)

    def lua(self, chunk, timeout=30.0, alive=None):
        return self.send("lua " + chunk, timeout, alive)["out"]

    def wait_ready(self, timeout=180.0, alive=None):
        """until the game's scripts are loaded (its Lua state up, the scripts' Event object set)"""
        t0 = time.monotonic()
        while True:
            try:
                s = self.status(timeout=max(1.0, timeout - (time.monotonic() - t0)), alive=alive)
                if s.get("lua_state") not in (None, "0x00000000") and s.get("event_object") not in (None, "0x00000000"):
                    return s
            except Refused:
                pass
            if time.monotonic() - t0 > timeout:
                raise TimeoutError(f"the game's scripts were not ready within {timeout:g} s")
            time.sleep(0.5)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    ch = Channel(sys.argv[1])
    if sys.argv[2] == "--wait-ready":
        print(json.dumps(ch.wait_ready(float(sys.argv[3]) if len(sys.argv) > 3 else 180.0)))
        return 0
    try:
        a = ch.send(" ".join(sys.argv[2:]))
    except Refused as e:
        print(f"refused: {e}")
        return 1
    print(json.dumps(a["out"]) if not isinstance(a["out"], str) else a["out"])
    return 0


if __name__ == "__main__":
    sys.exit(main())

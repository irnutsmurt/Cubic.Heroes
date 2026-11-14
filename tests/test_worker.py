#!/usr/bin/env python3
"""The pipeline worker's protocol (tools/worker.py, docs/M9_WORKER_PROTOCOL.md):
    build/venv/Scripts/python.exe -m unittest tests.test_worker
The event stream's framing, the result and exit codes, cancellation (asked for, the launcher gone, a blocked
step ended by force), the disc and key probes (made-up discs and keys; the real disc image and keys file are
used when they are present locally, and never printed). Nothing here converts a disc."""
import io
import json
import os
import struct
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import setup_pipeline as pipeline  # noqa: E402
import worker  # noqa: E402

WORKER = os.path.join(ROOT, "tools", "worker.py")
ISO = os.path.join(ROOT, "3D DOT GAME HEROES [BLUS-30490].iso")
KEYS = os.path.join(ROOT, "build", "keys", "self_keys.txt")
ELF = os.path.join(ROOT, "build", "EBOOT.elf")


def put(path, data):
    with open(path, "wb" if isinstance(data, bytes) else "w") as f:
        f.write(data)


def run(*args, stdin=None, timeout=60):
    p = subprocess.run([sys.executable, WORKER, *args], input=stdin, capture_output=True, text=True,
                       timeout=timeout)
    events = [json.loads(ln) for ln in p.stdout.splitlines()]
    return p.returncode, events, p.stderr


def sfo(entries):
    """a PARAM.SFO of string entries"""
    keys = b"".join(k.encode() + b"\0" for k in entries)
    keys += b"\0" * (-len(keys) % 4)
    data, table, ko, do = b"", b"", 0, 0
    for k, v in entries.items():
        b = v.encode() + b"\0"
        b += b"\0" * (-len(b) % 4)
        table += struct.pack("<HHIII", ko, 0x0204, len(v) + 1, len(b), do)
        data += b
        ko += len(k) + 1
        do += len(b)
    key_tab = 20 + len(table)
    return b"\0PSF" + struct.pack("<IIII", 0x101, key_tab, key_tab + len(keys), len(entries)) + table + keys + data


class Framing(unittest.TestCase):
    def test_version(self):
        rc, ev, _ = run("version")
        self.assertEqual(rc, worker.EXIT_OK)
        self.assertEqual([e["ev"] for e in ev], ["hello", "result"])
        self.assertEqual(ev[0]["protocol"], worker.PROTOCOL)
        self.assertTrue(ev[-1]["ok"])
        self.assertEqual(ev[-1]["data"]["game"], "BLUS30490")
        for e in ev:
            self.assertIsInstance(e["t"], float)

    def test_usage(self):
        rc, ev, _ = run("probe-disc")
        self.assertEqual(rc, worker.EXIT_USAGE)
        self.assertEqual(ev[-1]["code"], "usage")

    def test_selftest_progress(self):
        rc, ev, _ = run("selftest", "--seconds", "0.6")
        self.assertEqual(rc, worker.EXIT_OK)
        kinds = [e["ev"] for e in ev]
        self.assertEqual(kinds[:2], ["hello", "plan"])
        self.assertEqual(kinds[-1], "result")
        stages = [(e["id"], e["state"]) for e in ev if e["ev"] == "stage"]
        self.assertEqual(stages, [("verify", "start"), ("verify", "done"), ("build", "start"), ("build", "done"),
                                  ("finish", "start"), ("finish", "done")])
        overall = [e["overall"] for e in ev if e["ev"] == "progress"]
        self.assertEqual(overall, sorted(overall), "the overall fraction never goes back")
        self.assertAlmostEqual(overall[-1], 1.0, places=3)
        self.assertTrue(any(e["eta"] is not None for e in ev if e["ev"] == "progress"))

    def test_log_file(self):
        with tempfile.TemporaryDirectory() as d:
            log = os.path.join(d, "logs", "worker.log")
            rc, ev, _ = run("version", "--log", log)
            with open(log, encoding="utf-8") as f:
                self.assertEqual([json.loads(ln) for ln in f], ev)


class Cancel(unittest.TestCase):
    def start(self, *args):
        return subprocess.Popen([sys.executable, WORKER, *args, "--control"], stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, text=True)

    def finish(self, p):
        out, _ = p.communicate(timeout=30)
        return p.returncode, [json.loads(ln) for ln in out.splitlines()]

    def test_cancel_asked(self):
        p = self.start("selftest", "--seconds", "20")
        time.sleep(1.0)
        p.stdin.write("cancel\n")
        p.stdin.flush()
        rc, ev = self.finish(p)
        self.assertEqual(rc, worker.EXIT_CANCELLED)
        self.assertIn("cancelling", [e["ev"] for e in ev])
        self.assertEqual(ev[-1]["code"], "cancelled")
        self.assertNotIn("forced", ev[-1].get("data") or {})

    def test_launcher_gone(self):
        p = self.start("selftest", "--seconds", "20")
        time.sleep(1.0)
        p.stdin.close()   # the launcher's end of the pipe closed: it is gone
        out = p.stdout.read()
        p.wait(timeout=30)
        ev = [json.loads(ln) for ln in out.splitlines()]
        self.assertEqual(p.returncode, worker.EXIT_CANCELLED)
        self.assertEqual([e for e in ev if e["ev"] == "cancelling"][0]["by"], "the launcher closed")

    def test_blocked_step_forced(self):
        p = self.start("selftest", "--seconds", "1", "--block")
        time.sleep(1.0)
        t0 = time.monotonic()
        p.stdin.write("cancel\n")
        p.stdin.flush()
        rc, ev = self.finish(p)
        self.assertEqual(rc, worker.EXIT_CANCELLED)
        self.assertTrue(ev[-1]["data"]["forced"])
        self.assertLess(time.monotonic() - t0, worker.CANCEL_GRACE_S + 3)
        self.assertEqual(sum(1 for e in ev if e["ev"] == "result"), 1)


class ProgressMath(unittest.TestCase):
    def test_skipped_weights_leave_the_total(self):
        ev = worker.Events(io.StringIO())
        p = worker.Progress(ev, worker.Control(ev), ["recomp", "spu", "build"])
        p.skip("recomp")
        p.skip("spu")
        p.progress("build", 0.5, "x")
        self.assertAlmostEqual(p.overall(), 0.5)

    def run_at(self, speed, steps=("verify", "extract", "recomp", "spu", "build", "finish")):
        """a pretend install on a PC `speed` times slower than the reference in its compute-bound steps (the
        disc's steps at the reference's pace), each step's fraction linear in time: [(t, eta, basis, actual)]"""
        clock = {"t": 0.0}
        ev = worker.Events(io.StringIO())
        p = worker.Progress(ev, worker.Control(ev), list(steps), clock=lambda: clock["t"])
        took = {s: worker.STAGE_SECONDS[s] * (speed if s in worker.CPU_STAGES else 1.0) for s in steps}
        end = sum(took.values())
        rows, t0 = [], 0.0
        for s in steps:
            for i in range(51):
                clock["t"] = t0 + took[s] * i / 50
                p.progress(s, i / 50, "x")
                eta, basis = p.eta()
                rows.append((clock["t"], eta, basis, end - clock["t"]))
            t0 += took[s]
        return rows

    def test_eta_on_the_reference_pc(self):
        for t, eta, basis, actual in self.run_at(1.0):
            self.assertIsNotNone(eta)
            self.assertLess(abs(eta - actual), max(0.1 * actual, 5.0), (t, eta, basis, actual))

    def test_eta_on_a_slower_pc(self):
        rows = self.run_at(2.0)
        first = rows[0]
        self.assertEqual(first[2], "reference", "before a compute-bound step is done: the reference PC's")
        measured = [r for r in rows if r[2] in ("measured", "rate") and r[0] > rows[-1][0] * 0.25]
        self.assertTrue(measured)
        for t, eta, basis, actual in measured:   # once the recompile is done: this PC's pace
            self.assertLess(abs(eta - actual), max(0.15 * actual, 5.0), (t, eta, basis, actual))


class Probes(unittest.TestCase):
    def disc_folder(self, d, title_id="BLUS30490", app_ver="01.00", eboot=b"not the game"):
        os.makedirs(os.path.join(d, "PS3_GAME", "USRDIR"))
        put(os.path.join(d, "PS3_GAME", "PARAM.SFO"),
            sfo({"APP_VER": app_ver, "TITLE": "SOME GAME", "TITLE_ID": title_id}))
        put(os.path.join(d, "PS3_GAME", "USRDIR", "EBOOT.BIN"), eboot)
        return d

    def code(self, *args):
        rc, ev, _ = run(*args)
        self.assertEqual(rc, worker.EXIT_FAILED if not ev[-1]["ok"] else worker.EXIT_OK)
        return ev[-1].get("code"), ev[-1]

    def test_not_found(self):
        self.assertEqual(self.code("probe-disc", "--disc", os.path.join(ROOT, "no such.iso"))[0], "not_found")

    def test_not_a_disc(self):
        with tempfile.TemporaryDirectory() as d:
            f = os.path.join(d, "x.iso")
            put(f, os.urandom(64 * 1024))
            self.assertEqual(self.code("probe-disc", "--disc", f)[0], "not_a_disc")
            self.assertEqual(self.code("probe-disc", "--disc", d)[0], "not_a_disc")

    def test_other_game_and_version(self):
        with tempfile.TemporaryDirectory() as d:
            code, res = self.code("probe-disc", "--disc", self.disc_folder(os.path.join(d, "a"), "BLES00841"))
            self.assertEqual(code, "unsupported_version")
            self.assertIn("BLES00841", res["message"])
            code, res = self.code("probe-disc", "--disc", self.disc_folder(os.path.join(d, "b"), app_ver="01.01"))
            self.assertEqual(code, "unsupported_version")
            self.assertIn("01.01", res["message"])
            code, _ = self.code("probe-disc", "--disc", self.disc_folder(os.path.join(d, "c")))
            self.assertEqual(code, "damaged_disc")

    def test_keys_files(self):
        with tempfile.TemporaryDirectory() as d:
            disc = self.disc_folder(os.path.join(d, "disc"))
            empty = os.path.join(d, "empty.txt")
            put(empty, "# nothing\n")
            self.assertEqual(self.code("probe-keys", "--disc", disc, "--keys", empty)[0], "keys_empty")
            self.assertEqual(self.code("probe-keys", "--disc", disc, "--keys", os.path.join(d, "none"))[0],
                             "keys_unreadable")
            bad = os.path.join(d, "bad.txt")
            put(bad, "APP 0001 zz 00\n")
            self.assertEqual(self.code("probe-keys", "--disc", disc, "--keys", bad)[0], "keys_unreadable")

    @unittest.skipUnless(os.path.exists(ISO), "the disc image is local only")
    def test_real_disc(self):
        code, res = self.code("probe-disc", "--disc", ISO)
        self.assertIsNone(code, res)
        self.assertEqual((res["data"]["title_id"], res["data"]["app_ver"], res["data"]["kind"]),
                         ("BLUS30490", "01.00", "iso"))

    @unittest.skipUnless(os.path.exists(ISO), "the disc image is local only")
    def test_wrong_key(self):
        with tempfile.TemporaryDirectory() as d:
            wrong = os.path.join(d, "wrong.txt")
            put(wrong, f"APP 0001 {os.urandom(32).hex()} {os.urandom(16).hex()}\n")
            code, res = self.code("probe-keys", "--disc", ISO, "--keys", wrong)
            self.assertEqual(code, "wrong_key")

    @unittest.skipUnless(os.path.exists(ISO) and os.path.exists(KEYS), "the disc image and keys are local only")
    def test_matching_keys(self):
        rc, ev, err = run("probe-keys", "--disc", ISO, "--keys", KEYS)
        self.assertEqual(rc, worker.EXIT_OK, ev[-1])
        self.assertEqual(ev[-1]["data"], {"match": True, "via": "keys"})
        # nothing of the keys file in anything the worker said
        with open(KEYS) as f:
            secret = [ln.split()[2] for ln in f if ln.split()[:1] == ["APP"]]
        text = json.dumps(ev) + err
        self.assertFalse(any(s.lower() in text.lower() for s in secret))

    @unittest.skipUnless(os.path.exists(ISO) and os.path.exists(ELF), "the disc image and EBOOT.elf are local only")
    def test_elf_in_place_of_keys(self):
        code, res = self.code("probe-keys", "--disc", ISO, "--keys", ELF)
        self.assertIsNone(code, res)
        self.assertEqual(res["data"]["via"], "elf")


class Space(unittest.TestCase):
    def test_space(self):
        with tempfile.TemporaryDirectory() as d:
            rc, ev, _ = run("space", "--install", os.path.join(d, "not", "made", "yet"))
            self.assertEqual(rc, worker.EXIT_OK)
            data = ev[-1]["data"]
            self.assertEqual(data["needed"], worker.NEEDED["install"])
            self.assertEqual(data["enough"], data["free"] >= data["needed"])
            self.assertTrue(os.path.isdir(data["checked_at"]))


if __name__ == "__main__":
    unittest.main()

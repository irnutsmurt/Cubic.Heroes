#!/usr/bin/env python3
"""The portable root (the install's root is the launcher's own folder): the worker's side.
    build/venv/Scripts/python.exe -m unittest tests.test_portable_root
In temporary folders only, most of them named with spaces and characters beyond ASCII ("Cubic Heroes ünïcode 測試"):
Zig's caches in the install (the environment every Zig run gets, the subprocess stood in for); the worker's
temporary folder in the install (its children's too; a gone worker's removed, nothing else); an existing install
copied into a new root (migrate-root: the source left exactly as it was, every copied file checked, refused when the
roots overlap, the new one holds an install, a game runs, or there is no room or no right to write; a cancel or a
damaged copy leaves nothing); the old installations found, read only (find-legacy); a lifting tool's output in any
characters; read-only roots and too little space as results, never traces; the packaged worker started on its own
opens no window. Nothing here starts the game or the launcher, and nothing opens a window."""
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, ROOT)
import install_layout as layout  # noqa: E402
import root_migration  # noqa: E402
import setup_pipeline as pipeline  # noqa: E402
import worker  # noqa: E402
import zig_build  # noqa: E402

WORKER = os.path.join(ROOT, "tools", "worker.py")
NAME = "Cubic Heroes ünïcode 測試"
EXE = layout.exe_name()
DEAD_PID = 999999   # never a process: Windows' are multiples of 4, and nothing else runs that high here


def put(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb" if isinstance(data, bytes) else "w", **({} if isinstance(data, bytes) else
                                                                    {"encoding": "utf-8", "newline": "\n"})) as f:
        f.write(data)


def get(path):
    with open(path, "rb") as f:
        return f.read()


def snapshot(root):
    """every file and folder under root (links as their target, never followed): bytes and modification times"""
    out = {}
    for d, dirs, fs in os.walk(root):
        keep = []
        for x in dirs:
            p = os.path.join(d, x)
            if layout.is_link(p):
                out[os.path.relpath(p, root)] = ("link", os.path.normcase(os.path.realpath(p)))
            else:
                keep.append(x)
                out[os.path.relpath(p, root)] = ("dir", os.stat(p).st_mtime_ns)
        dirs[:] = keep
        for f in fs:
            p = os.path.join(d, f)
            out[os.path.relpath(p, root)] = (get(p), os.stat(p).st_mtime_ns)
    return out


def run(*args, env=None, timeout=60):
    p = subprocess.run([sys.executable, WORKER, *args], capture_output=True, text=True, encoding="utf-8",
                       timeout=timeout, env=env)
    return p.returncode, [json.loads(ln) for ln in p.stdout.splitlines()]


def slots_install(root, game=b"game 1"):
    """a made-up install with one active build (its game a few bytes), saves, settings, caches and logs"""
    lay = layout.Layout(root)
    lay.ensure_dirs()
    staging = lay.stage()
    put(os.path.join(staging, EXE), game)
    info = lay.record(staging, {"lift": "lift-a", "objects": ["a.o"], "shader_cache": "sh-1", "version": "test"})
    bid = lay.promote(staging, info)
    lay.activate(bid)
    put(os.path.join(lay.gamedata, "dev_hdd0", "home", "00000001", "savedata", "SLOT", "DATA.BIN"), b"the save")
    put(os.path.join(lay.data, "devmode", "gamedata", "TEST.BIN"), b"a test save")
    put(os.path.join(lay.disc, "PS3_GAME", "USRDIR", "EBOOT.elf"), os.urandom(70000))
    put(lay.install_json, json.dumps({"version": "test", "stamps": {}}))
    put(os.path.join(lay.objects, "a.o"), b"object")
    put(os.path.join(lay.shaders, "sh-1", "x.spv"), b"spirv")
    put(lay.cfg, "DOTHEROES_FPS=60\n")
    put(os.path.join(lay.backups_dir, "20261117-manual", "gamedata", "DATA.BIN"), b"a backup")
    put(os.path.join(lay.backups_dir, "20261117-manual", "backup.json"), "{}")
    put(os.path.join(lay.logs, "game.log"), "the old folder's log")
    put(os.path.join(lay.downloads, "0.1.0", "pkg.zip"), b"a download")
    put(os.path.join(lay.cache, "prepare-part.bin"), b"a preparation's part")
    put(os.path.join(lay.tmp, "worker-12", "x.tmp"), b"a worker's")
    return lay, bid


def unlink_all(root):
    """the links a test made (removed as links before the folder goes)"""
    for d, dirs, _ in os.walk(root):
        for x in list(dirs):
            if layout.is_link(os.path.join(d, x)):
                layout.remove_link(os.path.join(d, x))
                dirs.remove(x)


class Temp(unittest.TestCase):
    def setUp(self):
        self.tmpd = tempfile.TemporaryDirectory(prefix="portable-test-")
        self.tmp = self.tmpd.name
        self.root = os.path.join(self.tmp, NAME)

    def tearDown(self):
        worker.release_install_temp()
        zig_build.use_install(None)
        unlink_all(self.tmp)
        self.tmpd.cleanup()


# ---- the folder moved: game/ (an absolute junction) repaired, the build run directly (M11.3) ----

class MovedFolder(Temp):
    def test_the_game_link_follows_the_folder(self):
        lay, bid = slots_install(self.root)
        moved = self.root + " moved"
        os.replace(self.root, moved)
        new = layout.Layout(moved)
        self.assertEqual(new.active_exe(), os.path.join(moved, "builds", bid, EXE))
        self.assertTrue(os.path.isfile(new.active_exe()))
        # before the repair game/ still names the old place (gone): the build's own program is what runs
        self.assertFalse(os.path.isfile(os.path.join(new.game_link, EXE)))
        self.assertTrue(new.repair_game_link())
        self.assertTrue(os.path.samefile(new.game_link, new.build_dir(bid)))
        self.assertTrue(os.path.isfile(os.path.join(new.game_link, EXE)))
        self.assertTrue(new.repair_game_link())   # right already: nothing to do, still true

    def test_the_worker_repairs_it_when_it_opens_the_install(self):
        lay, bid = slots_install(self.root)
        moved = self.root + " moved"
        os.replace(self.root, moved)
        code, out = run("builds", "--install", moved)
        self.assertEqual(code, 0, out)
        self.assertTrue(os.path.samefile(os.path.join(moved, "game"), os.path.join(moved, "builds", bid)))

    def test_no_install_no_link(self):
        os.makedirs(self.root)
        self.assertFalse(layout.Layout(self.root).repair_game_link())
        self.assertFalse(os.path.lexists(os.path.join(self.root, "game")))


# ---- 4: Zig's caches in the install ----

class ZigCaches(Temp):
    def caches(self):
        return zig_build.cache_dirs(self.root)

    def test_run_names_the_installs_caches(self):
        calls = []
        with mock.patch.object(zig_build.subprocess, "run",
                               lambda cmd, **kw: calls.append(kw) or mock.Mock(returncode=0, stdout="", stderr="")):
            zig_build._run(["zig", "version"], install=self.root, capture_output=True, text=True)
            zig_build.use_install(self.root)
            zig_build._run(["zig", "cc"], capture_output=True, text=True)
            zig_build._run(["zig", "cc"], env={"PATH": "x"})
        g, loc = self.caches()
        for kw in calls:
            self.assertEqual((kw["env"]["ZIG_GLOBAL_CACHE_DIR"], kw["env"]["ZIG_LOCAL_CACHE_DIR"]), (g, loc))
        self.assertEqual(calls[0]["encoding"], "utf-8")
        self.assertEqual(calls[2]["env"]["PATH"], "x", "an environment of the caller's kept, the caches added")
        self.assertTrue(os.path.isdir(g) and os.path.isdir(loc))
        self.assertTrue(g.startswith(os.path.join(self.root, "data", "cache")))

    def test_no_install_known_no_change(self):
        calls = []
        with mock.patch.object(zig_build.subprocess, "run", lambda cmd, **kw: calls.append(kw) or mock.Mock()):
            zig_build._run(["zig", "version"])
        self.assertNotIn("env", calls[0])

    def test_compiles_link_and_cpu_probe(self):
        g, loc = self.caches()
        src = os.path.join(self.root, "src", "a.cpp")
        put(src, "int x;")
        envs = []

        def compile_(cmd, **kw):
            envs.append(kw.get("env"))
            if "-o" in cmd:
                put(cmd[cmd.index("-o") + 1], b"o")
            return mock.Mock(returncode=0, stdout="#define __SSE4_2__ 1\n", stderr="")
        with mock.patch.object(zig_build.subprocess, "run", compile_), \
                mock.patch.object(zig_build, "jobs_for_memory", lambda: 1):
            zig_build._compile_all("zig", [(src, os.path.join(self.root, "obj", "a-1.o"), "c++", [])], "windows",
                                   install=self.root)
            saved, zig_build._host_cpu = zig_build._host_cpu, None
            try:
                with mock.patch.dict(os.environ, {"DOTHEROES_CPU": ""}):
                    zig_build.use_install(self.root)
                    zig_build.host_cpu("zig")
            finally:
                zig_build._host_cpu = saved
                zig_build.use_install(None)

        class Popen:
            def __init__(self, cmd, **kw):
                envs.append(kw.get("env"))
                put(cmd[cmd.index("-o") + 1], b"exe")
                self.returncode = 0

            def communicate(self, timeout=None):
                return "", ""
        lib = os.path.join(self.root, "lib")
        put(os.path.join(lib, zig_build.RUNTIME_LIB), b"lib")
        with mock.patch.object(zig_build, "Sources", lambda platform=None: mock.Mock(lib_dir=lib)), \
                mock.patch.object(zig_build, "game_jobs", lambda *a, **k: []), \
                mock.patch.object(zig_build, "link_args", lambda *a, **k: []), \
                mock.patch.object(zig_build.subprocess, "Popen", Popen):
            zig_build.build_game(os.path.join(self.root, "gen"), os.path.join(self.root, "out", EXE),
                                 os.path.join(self.root, "objects"), zig="zig", install=self.root)
        self.assertEqual(len(envs), 3)   # a compile, the CPU probe, the link
        for env in envs:
            self.assertEqual((env["ZIG_GLOBAL_CACHE_DIR"], env["ZIG_LOCAL_CACHE_DIR"]), (g, loc))

    def test_the_pipeline_names_its_install(self):
        os.makedirs(self.root)
        with self.assertRaises(pipeline.SetupError):   # no disc copy: it stops there, after naming the install
            pipeline.run(self.root)
        self.assertEqual(zig_build._cache_install, os.path.abspath(self.root))


# ---- 5 and 7: the worker's temporary folder in the install; any characters in a path ----

class InstallTemp(Temp):
    def test_in_process(self):
        base = layout.Layout(self.root).tmp
        stale = os.path.join(base, f"worker-{DEAD_PID}")
        live = os.path.join(base, f"worker-{os.getppid()}")   # the process that started this test: alive
        put(os.path.join(stale, "left.tmp"), b"x")
        put(os.path.join(live, "in-use.tmp"), b"x")
        put(os.path.join(base, "keep.txt"), "not a worker's")
        before = {k: os.environ.get(k) for k in worker.TEMP_VARS}
        mine = worker.use_install_temp(self.root)
        try:
            self.assertEqual(mine, os.path.join(base, f"worker-{os.getpid()}"))
            self.assertEqual(tempfile.gettempdir(), mine)
            for k in worker.TEMP_VARS:
                self.assertEqual(os.environ[k], mine)
            with tempfile.NamedTemporaryFile() as f:
                self.assertEqual(os.path.dirname(f.name), mine)
            self.assertFalse(os.path.exists(stale), "a gone worker's folder removed")
            self.assertTrue(os.path.exists(os.path.join(live, "in-use.tmp")), "a live worker's kept")
            self.assertTrue(os.path.exists(os.path.join(base, "keep.txt")), "nothing else touched")
        finally:
            worker.release_install_temp()
        self.assertFalse(os.path.exists(mine))
        self.assertEqual({k: os.environ.get(k) for k in worker.TEMP_VARS}, before)
        self.assertNotEqual(tempfile.gettempdir(), mine)

    def test_never_made_for_a_folder_not_there(self):
        self.assertIsNone(worker.use_install_temp(self.root))
        self.assertFalse(os.path.exists(self.root))

    def test_the_workers_children(self):
        os.makedirs(self.root)
        put(os.path.join(self.root, "data", "tmp", f"worker-{DEAD_PID}", "x"), b"x")
        rc, ev = run("selftest", "--seconds", "0.1", "--child", "--install", self.root)
        self.assertEqual(rc, 0, ev[-1])
        logs = [e["text"] for e in ev if e["ev"] == "log"]
        self.assertIn("child ran", logs)
        temp = [t for t in logs if t.startswith("child temp ")][0][len("child temp "):]
        self.assertEqual(os.path.dirname(temp), os.path.join(self.root, "data", "tmp"))
        self.assertTrue(os.path.basename(temp).startswith("worker-"))
        g, loc = zig_build.cache_dirs(self.root)
        self.assertIn(f"child zig {g} | {loc}", logs)
        self.assertEqual(os.listdir(os.path.join(self.root, "data", "tmp")), [], "its own folder and the gone one's")

    def test_a_tools_output_in_any_characters(self):
        os.makedirs(self.root)
        script = os.path.join(self.root, "say path.py")
        put(script, "import sys\nprint(sys.argv[1])\nprint('\\u00c1\\u6e2c')\n")
        lines = []
        pipeline.run_tool(script, [self.root], lines.append)
        self.assertEqual(lines, [self.root, "Á測"])


# ---- 6: migrate-root ----

class MigrateRoot(Temp):
    def setUp(self):
        super().setUp()
        self.old = os.path.join(self.tmp, "old install ü")
        self.lay, self.bid = slots_install(self.old)
        self.outside = os.path.join(self.tmp, "elsewhere")
        put(os.path.join(self.outside, "precious.txt"), "never copied")
        layout.make_link(os.path.join(self.lay.gen_root, "outlink"), self.outside)
        # the new root: the launcher's folder, its package and its own logs already there
        put(os.path.join(self.root, "CubicHeroes.exe"), b"launcher")
        put(os.path.join(self.root, "worker", "DotHeroes-Setup.exe"), b"worker")
        put(os.path.join(self.root, "logs", "worker-find-legacy.jsonl"), "the new root's log")
        self.cb = worker.Callbacks()

    def test_dry_run_writes_nothing(self):
        before_old, before_new = snapshot(self.old), snapshot(self.root)
        p = worker.op_migrate_root(self.old, self.root, self.cb, dry_run=True)
        self.assertEqual(p["kind"], "slots")
        self.assertEqual(p["active"], self.bid)
        self.assertTrue(p["enough"])
        self.assertIn("data/tmp", p["skipped"])
        self.assertIn("data/gen/outlink", p["links"])
        self.assertEqual((snapshot(self.old), snapshot(self.root)), (before_old, before_new))

    def test_copy(self):
        before = snapshot(self.old)
        out = worker.op_migrate_root(self.old, self.root, self.cb)
        self.assertEqual(snapshot(self.old), before, "the source exactly as it was")
        new = layout.Layout(self.root)
        self.assertEqual(new.kind(), "slots")
        self.assertEqual(new.read_active()["active"], self.bid)
        self.assertIsNone(new.check_build(self.bid, full=True))
        self.assertTrue(layout.is_link(new.game_link))
        self.assertEqual(get(os.path.join(new.game_link, EXE)), b"game 1")
        self.assertTrue(out["game_link"])
        for rel in (os.path.join("data", "gamedata", "dev_hdd0", "home", "00000001", "savedata", "SLOT", "DATA.BIN"),
                    os.path.join("data", "devmode", "gamedata", "TEST.BIN"),
                    os.path.join("data", "disc", "PS3_GAME", "USRDIR", "EBOOT.elf"), os.path.join("data", "install.json"),
                    os.path.join("data", "cache", "objects", "a.o"), os.path.join("data", "cache", "shaders", "sh-1",
                                                                                  "x.spv"),
                    os.path.join("config", "dotheroes.cfg"),
                    os.path.join("backups", "saves", "20261117-manual", "gamedata", "DATA.BIN")):
            self.assertEqual(get(os.path.join(self.root, rel)), get(os.path.join(self.old, rel)), rel)
        for absent in (os.path.join("data", "tmp", "worker-12"), os.path.join("data", "cache", "prepare-part.bin"),
                       os.path.join("data", "gen", "outlink"), "downloads", root_migration.PART):
            self.assertFalse(os.path.lexists(os.path.join(self.root, absent)), absent)
        self.assertEqual(os.listdir(os.path.join(self.root, "logs")), ["worker-find-legacy.jsonl"],
                         "the old logs not copied, the new root's kept")
        self.assertEqual(get(os.path.join(self.root, "CubicHeroes.exe")), b"launcher")
        self.assertEqual(out["verified"]["builds"], [self.bid])
        self.assertGreaterEqual(out["verified"]["hashed"], 5)

    def test_refusals(self):
        before = snapshot(self.old)
        cases = [(self.old, "root_overlap"), (os.path.join(self.old, "inner"), "root_overlap"),
                 (os.path.dirname(self.old), "root_overlap")]
        for new, code in cases:
            with self.assertRaises(pipeline.SetupError) as e:
                worker.op_migrate_root(self.old, new, self.cb)
            self.assertEqual(e.exception.code, code, new)
        taken = os.path.join(self.tmp, "taken")
        put(os.path.join(taken, "active.json"), "{}")
        with self.assertRaises(pipeline.SetupError) as e:
            worker.op_migrate_root(self.old, taken, self.cb)
        self.assertEqual(e.exception.code, "install_exists")
        with self.assertRaises(pipeline.SetupError) as e:
            worker.op_migrate_root(os.path.join(self.tmp, "nothing"), self.root, self.cb)
        self.assertEqual(e.exception.code, "no_install")
        setup_app = os.path.join(self.tmp, "setup app")
        put(os.path.join(setup_app, EXE), b"old game")
        put(os.path.join(setup_app, "data", "install.json"), "{}")
        with self.assertRaises(pipeline.SetupError) as e:
            worker.op_migrate_root(setup_app, self.root, self.cb)
        self.assertEqual(e.exception.code, "needs_migration")
        self.assertEqual(snapshot(self.old), before)

    def test_no_room_or_no_right_to_write(self):
        before = snapshot(self.root)
        with mock.patch.object(worker, "free_bytes", lambda p: (1024, p)):
            with self.assertRaises(pipeline.SetupError) as e:
                worker.op_migrate_root(self.old, self.root, self.cb)
        self.assertEqual(e.exception.code, "disk_space")
        with mock.patch.object(worker, "_write_probe", side_effect=PermissionError(13, "Access is denied")):
            with self.assertRaises(pipeline.SetupError) as e:
                worker.op_migrate_root(self.old, self.root, self.cb)
        self.assertEqual(e.exception.code, "root_not_writable")
        self.assertEqual(snapshot(self.root), before, "nothing written")

    def test_cancel_leaves_nothing(self):
        cancel = threading.Event()
        cb = worker.Callbacks(progress=lambda s, f, m: cancel.set(), cancel=cancel)
        with self.assertRaises(pipeline.Cancelled):
            worker.op_migrate_root(self.old, self.root, cb)
        new = layout.Layout(self.root)
        for absent in (root_migration.PART, "builds", "active.json", "config"):
            self.assertFalse(os.path.lexists(os.path.join(self.root, absent)), absent)
        self.assertEqual(new.kind(), "none")

    def test_a_damaged_copy_is_never_put_in_place(self):
        real = root_migration._copy_file

        def damaging(src, dst, on_bytes):
            real(src, dst, on_bytes)
            if src.endswith("DATA.BIN") and "savedata" in src:
                with open(dst, "r+b") as f:
                    f.write(b"X")   # the same size, another byte
        with mock.patch.object(root_migration, "_copy_file", damaging):
            with self.assertRaises(pipeline.SetupError) as e:
                worker.op_migrate_root(self.old, self.root, self.cb)
        self.assertEqual(e.exception.code, "copy_failed")
        self.assertFalse(os.path.lexists(os.path.join(self.root, root_migration.PART)))
        self.assertFalse(os.path.lexists(os.path.join(self.root, "active.json")))

    def test_a_settings_file_already_there_is_kept_aside(self):
        put(os.path.join(self.root, "config", "dotheroes.cfg"), "DOTHEROES_FPS=30\n")
        out = worker.op_migrate_root(self.old, self.root, self.cb)
        self.assertEqual(get(os.path.join(self.root, "config", "dotheroes.cfg")), b"DOTHEROES_FPS=60\n")
        self.assertEqual(get(os.path.join(self.root, "config", "dotheroes.cfg.before-migrate-root")),
                         b"DOTHEROES_FPS=30\n")
        self.assertEqual(out["replaced"], [{"path": "config/dotheroes.cfg",
                                            "kept_as": "dotheroes.cfg.before-migrate-root"}])

    def test_the_workers_command(self):
        before = snapshot(self.old)
        rc, ev = run("migrate-root", "--from", self.old, "--install", self.root, "--dry-run")
        self.assertEqual((rc, ev[-1]["ok"]), (0, True), ev[-1])
        self.assertFalse(os.path.exists(os.path.join(self.root, "active.json")))
        rc, ev = run("migrate-root", "--from", self.old, "--install", self.root)
        self.assertEqual(rc, 0, ev[-1])
        plan = [e for e in ev if e["ev"] == "plan"][0]
        self.assertEqual([s["id"] for s in plan["stages"]], ["copy", "check-copy", "place"])
        self.assertTrue(any(e["ev"] == "progress" and e["stage"] == "copy" for e in ev))
        self.assertEqual(ev[-1]["data"]["active"], self.bid)
        self.assertEqual(snapshot(self.old), before)
        self.assertEqual(os.listdir(os.path.join(self.root, "data", "tmp")), [], "the worker's temporary folder gone")
        rc, ev = run("migrate-root", "--install", self.root)
        self.assertEqual((rc, ev[-1]["code"], ev[-1]["message"]), (64, "usage", "migrate-root needs --from"))


# ---- 6: the old installations found ----

@unittest.skipUnless(os.name == "nt", "the Windows folders (APPDATA, LOCALAPPDATA)")
class FindLegacy(Temp):
    def setUp(self):
        super().setUp()
        self.env = {"APPDATA": os.path.join(self.tmp, "roaming"), "LOCALAPPDATA": os.path.join(self.tmp, "local")}
        self.old = os.path.join(self.tmp, "D drive", "Games", "Cubic.Heroes ü")
        slots_install(self.old)
        put(os.path.join(self.env["APPDATA"], "CubicHeroes", "launcher.json"),
            json.dumps({"install": self.old, "disc": "", "keys": ""}))
        self.default = os.path.join(self.env["LOCALAPPDATA"], "Cubic.Heroes")
        put(os.path.join(self.default, "data", "install.json"), "{}")   # an install interrupted
        os.makedirs(self.root)

    def test_read_only_report(self):
        before = snapshot(self.tmp)
        r = root_migration.find_legacy(worker.install_report, current=self.root, env=self.env, home=self.tmp)
        self.assertEqual(snapshot(self.tmp), before, "nothing written anywhere")
        launcher = [p for p in r["prefs"] if p["source"] == "launcher"][0]
        self.assertTrue(launcher["present"] and launcher["readable"])
        self.assertEqual(launcher["install"], self.old)
        self.assertEqual(launcher["holds"]["kind"], "slots")
        self.assertTrue(launcher["holds"]["migratable"])
        setup = [p for p in r["prefs"] if p["source"] == "setup-app"][0]
        self.assertFalse(setup["present"])
        d = {x["source"]: x for x in r["defaults"]}
        self.assertEqual(d["launcher-default"]["holds"]["kind"], "partial")
        self.assertFalse(d["setup-app-default"]["holds"]["exists"])
        self.assertEqual(sorted(r["migratable"]), sorted([self.old, self.default]))

    def test_the_workers_command(self):
        before = snapshot(self.tmp)
        rc, ev = run("find-legacy", "--install", self.root, env=dict(os.environ, **self.env))
        self.assertEqual(rc, 0, ev[-1])
        self.assertIn(self.old, ev[-1]["data"]["migratable"])
        self.assertEqual(snapshot(self.tmp), before)


# ---- 8: read-only roots and too little space: results, not traces ----

class RootChecks(Temp):
    def never(self, *a, **k):
        raise AssertionError("the conversion was started")

    def code(self, fn, *a, **k):
        with self.assertRaises(pipeline.SetupError) as e:
            fn(*a, **k)
        return e.exception.code

    def test_install(self):
        with mock.patch.object(pipeline, "run", self.never):
            with mock.patch.object(worker.os, "access", lambda p, m: False):
                self.assertEqual(self.code(worker.op_install, self.root, "d.iso", "k", None, worker.Callbacks()),
                                 "root_not_writable")
            with mock.patch.object(worker, "_write_probe", side_effect=PermissionError(13, "Access is denied")):
                self.assertEqual(self.code(worker.op_install, self.root, "d.iso", "k", None, worker.Callbacks()),
                                 "root_not_writable")
            with mock.patch.object(worker, "free_bytes", lambda p: (1024, p)):
                self.assertEqual(self.code(worker.op_install, self.root, "d.iso", "k", None, worker.Callbacks()),
                                 "disk_space")

            def unreachable(p):
                raise OSError(21, "The device is not ready")
            with mock.patch.object(worker, "free_bytes", unreachable):
                self.assertEqual(self.code(worker.op_install, self.root, "d.iso", "k", None, worker.Callbacks()),
                                 "root_not_writable")

    def test_update_and_prepare(self):
        slots_install(self.root)
        with mock.patch.object(pipeline, "run", self.never), \
                mock.patch.object(worker, "_write_probe", side_effect=PermissionError(13, "Access is denied")):
            self.assertEqual(self.code(worker.op_update, self.root, worker.Callbacks()), "root_not_writable")
            self.assertEqual(self.code(worker.op_prepare, self.root, worker.Callbacks()), "root_not_writable")
        with mock.patch.object(pipeline, "run", self.never), \
                mock.patch.object(worker, "free_bytes", lambda p: (1024, p)):
            self.assertEqual(self.code(worker.op_update, self.root, worker.Callbacks()), "disk_space")

    def test_the_workers_result(self):
        afile = os.path.join(self.tmp, "a file")
        put(afile, "not a folder")
        rc, ev = run("install", "--disc", "d.iso", "--keys", "k.txt", "--install", os.path.join(afile, NAME))
        self.assertEqual((rc, ev[-1]["code"]), (worker.EXIT_FAILED, "root_not_writable"), ev[-1])
        self.assertNotIn("Traceback", ev[-1].get("details") or "")


# ---- 9: the packaged worker started on its own ----

class SetupAppNotice(unittest.TestCase):
    def setUp(self):
        import setup_app
        self.app = setup_app

    def start(self, *argv):
        out = io.StringIO()
        with mock.patch.object(sys, "frozen", True, create=True), \
                mock.patch.object(sys, "argv", ["DotHeroes-Setup.exe", *argv]), \
                mock.patch.object(sys, "stdout", out), \
                mock.patch.object(self.app, "_load_tk", side_effect=AssertionError("the window was opened")):
            with self.assertRaises(SystemExit) as e:
                self.app.main()
        return e.exception.code, out.getvalue()

    def test_no_window(self):
        for argv in ((), ("--after-update", "old", "install"), ("--update-failed",), ("--something",)):
            code, text = self.start(*argv)
            self.assertEqual(code, 2, argv)
            self.assertIn("CubicHeroes", text)

    def test_the_worker_still_runs(self):
        with mock.patch.object(worker, "main", return_value=0) as m:
            code, _ = self.start("--worker", "version")
        self.assertEqual(code, 0)
        m.assert_called_once_with(["version"])


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""RecompLauncher support (0.2.0-beta.5; docs/RECOMPLAUNCHER.md): the SDK manifest and the updater's rule.
    build/venv/Scripts/python.exe -m unittest tests.test_recomplauncher

- launcher/recomplauncher.json is valid against RecompLauncher's own JSON Schema (tests/fixtures/
  recomplauncher.schema.json: the SDK's schema/recomplauncher.json, MIT, copied 2026-11-21 from its commit c06e372,
  SHA-256 a9d4ffca7c0f92579bbe0395006910b6d6968b5c7a66825ba92e98a68f4c7af1), checked by a draft-07 validator for
  exactly the keywords that schema uses (an unknown keyword fails the test: it never passes by ignoring one);
- what the manifest asks of Cubic.Heroes exists: its programs are the launcher's (both systems), its start is
  `--play --or-launcher`, every settings argument is an option of our command line with a value it accepts (the
  prompt style from config/settings_schema.json), its saves are the installation's save folder;
- the packages carry it (install_layout.LAUNCHER_PACKAGE), and RecompLauncher's mark is the installation's own item
  (never a package item the updater would move);
- the worker's own updater refuses for an installation RecompLauncher manages (RECOMPLAUNCHER=1, or its mark), and
  answers a check with "managed": nothing is downloaded, staged or swapped; with neither, nothing changes."""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import install_layout as layout  # noqa: E402
import updates  # noqa: E402

MANIFEST = os.path.join(ROOT, "launcher", "recomplauncher.json")
SCHEMA = os.path.join(ROOT, "tests", "fixtures", "recomplauncher.schema.json")


def load(p):
    with open(p, encoding="utf-8") as f:
        return json.load(f)


class Draft07:
    """a JSON Schema draft-07 validator for the keywords RecompLauncher's schema uses -- and no others: a keyword
    this doesn't know is an error, so a schema update that needs more is noticed, never silently passed"""
    KNOWN = {"$id", "$ref", "$schema", "additionalProperties", "allOf", "anyOf", "const", "default", "definitions",
             "description", "enum", "examples", "exclusiveMinimum", "if", "items", "maxItems", "maximum", "minItems",
             "minLength", "minimum", "not", "pattern", "properties", "propertyNames", "required", "then", "title",
             "type", "uniqueItems"}
    TYPES = {"object": dict, "array": list, "string": str, "boolean": bool, "null": type(None)}

    def __init__(self, root):
        self.root = root

    def resolve(self, ref):
        if not ref.startswith("#/"):
            raise ValueError(f"unsupported $ref {ref}")
        node = self.root
        for part in ref[2:].split("/"):
            node = node[part]
        return node

    def type_ok(self, t, v):
        if t == "integer":
            return isinstance(v, int) and not isinstance(v, bool)
        if t == "number":
            return isinstance(v, (int, float)) and not isinstance(v, bool)
        return isinstance(v, self.TYPES[t])

    def errors(self, s, v, path="$"):
        if s is True or s == {}:
            return []
        if s is False:
            return [f"{path}: not allowed"]
        unknown = set(s) - self.KNOWN
        if unknown:
            raise ValueError(f"{path}: schema keywords this validator doesn't know: {sorted(unknown)}")
        out = []
        if "$ref" in s:
            out += self.errors(self.resolve(s["$ref"]), v, path)
        if "type" in s:
            ts = s["type"] if isinstance(s["type"], list) else [s["type"]]
            if not any(self.type_ok(t, v) for t in ts):
                return out + [f"{path}: not a {s['type']}"]
        if "const" in s and v != s["const"]:
            out.append(f"{path}: not {s['const']!r}")
        if "enum" in s and v not in s["enum"]:
            out.append(f"{path}: {v!r} not one of {s['enum']}")
        if isinstance(v, str):
            if "pattern" in s and not re.search(s["pattern"], v):
                out.append(f"{path}: {v!r} doesn't match {s['pattern']}")
            if "minLength" in s and len(v) < s["minLength"]:
                out.append(f"{path}: shorter than {s['minLength']}")
        if isinstance(v, (int, float)) and not isinstance(v, bool):
            if "minimum" in s and v < s["minimum"]:
                out.append(f"{path}: below {s['minimum']}")
            if "maximum" in s and v > s["maximum"]:
                out.append(f"{path}: above {s['maximum']}")
            if "exclusiveMinimum" in s and v <= s["exclusiveMinimum"]:
                out.append(f"{path}: not above {s['exclusiveMinimum']}")
        if isinstance(v, list):
            if "minItems" in s and len(v) < s["minItems"]:
                out.append(f"{path}: fewer than {s['minItems']} items")
            if "maxItems" in s and len(v) > s["maxItems"]:
                out.append(f"{path}: more than {s['maxItems']} items")
            if s.get("uniqueItems") and len({json.dumps(x, sort_keys=True) for x in v}) != len(v):
                out.append(f"{path}: items not unique")
            if "items" in s:
                for i, x in enumerate(v):
                    out += self.errors(s["items"], x, f"{path}[{i}]")
        if isinstance(v, dict):
            for k in s.get("required", []):
                if k not in v:
                    out.append(f"{path}: {k} is required")
            props = s.get("properties", {})
            for k, x in v.items():
                if "propertyNames" in s:
                    out += self.errors(s["propertyNames"], k, f"{path}.<{k}>")
                if k in props:
                    out += self.errors(props[k], x, f"{path}.{k}")
                elif "additionalProperties" in s:
                    out += self.errors(s["additionalProperties"], x, f"{path}.{k}")
        for sub in s.get("allOf", []):
            out += self.errors(sub, v, path)
        if "anyOf" in s and not any(not self.errors(sub, v, path) for sub in s["anyOf"]):
            out.append(f"{path}: none of anyOf")
        if "not" in s and not self.errors(s["not"], v, path):
            out.append(f"{path}: matches a 'not'")
        if "if" in s and not self.errors(s["if"], v, path) and "then" in s:
            out += self.errors(s["then"], v, path)
        return out


class SchemaTests(unittest.TestCase):
    def test_the_manifest_is_valid_against_the_official_schema(self):
        schema = load(SCHEMA)
        self.assertEqual(Draft07(schema).errors(schema, load(MANIFEST)), [])

    def test_the_validator_catches_mistakes(self):
        schema = load(SCHEMA)
        v = Draft07(schema)
        m = load(MANIFEST)
        for bad in (dict(m, format=2), dict(m, unknown=1), {k: x for k, x in m.items() if k != "format"},
                    dict(m, start={"args": ["--play"]}), dict(m, settings=[{"id": "x", "label": "y", "choices": [{}]}]),
                    dict(m, system={"windows": {"version": "ten"}}), dict(m, requires=["start", "start"])):
            self.assertTrue(v.errors(schema, bad), bad)
        with self.assertRaises(ValueError):
            v.errors({"type": "object", "dependentRequired": {}}, {})


class ManifestTests(unittest.TestCase):
    def setUp(self):
        self.m = load(MANIFEST)
        with open(os.path.join(ROOT, "launcher", "cli.h"), encoding="utf-8") as f:
            self.options = set(re.findall(r'o == "(--[a-z-]+)"', f.read()))

    def test_start(self):
        self.assertEqual(self.m["start"]["program"], [layout.LAUNCHER_PACKAGE["windows"][0],
                                                      layout.LAUNCHER_PACKAGE["linux"][0]])
        self.assertEqual(self.m["start"]["args"], ["--play", "--or-launcher"])
        for o in self.m["start"]["args"]:
            self.assertIn(o, self.options)

    def test_settings_are_our_command_line(self):
        prompts = next(s for s in load(os.path.join(ROOT, "config", "settings_schema.json"))["settings"]
                       if s["key"] == "LAUNCHER_PROMPTS")["values"]
        for s in self.m["settings"]:
            self.assertEqual(s.get("in"), "args", s["id"])
            self.assertEqual(s["choices"][0]["args"], [], f"{s['id']}: the first choice leaves the game's own setting")
            for c in s["choices"]:
                a = c["args"]
                i = 0
                while i < len(a):
                    o = a[i]
                    self.assertIn(o, self.options, f"{s['id']}: {o}")
                    if o in ("--resolution", "--fps", "--vsync", "--set"):
                        val = a[i + 1]
                        if o == "--resolution":
                            self.assertRegex(val, r"^(native|\d{3,4}x\d{3,4})$")
                        elif o == "--fps":
                            self.assertTrue(val == "display" or val == "0" or 30 <= int(val) <= 1000, val)
                        elif o == "--vsync":
                            self.assertIn(val, ("on", "off"))
                        else:
                            k, _, v = val.partition("=")
                            self.assertEqual(k, "LAUNCHER_PROMPTS")
                            self.assertIn(v, prompts)
                        i += 2
                    else:
                        i += 1

    def test_saves_and_logs_are_the_installation_s(self):
        self.assertEqual(self.m["saves"], ["{install}/data/gamedata/dev_hdd0/home/00000001/savedata"])
        self.assertEqual(self.m["folders"]["logs"], [{"path": "{install}/logs"}])

    def test_packaged_and_kept_apart(self):
        for platform in ("windows", "linux"):
            self.assertIn("recomplauncher.json", layout.launcher_package(platform))
        self.assertIn(layout.RECOMP_MARK, layout.INSTALL_ITEMS)
        self.assertTrue(updates._not_package(layout.RECOMP_MARK), "the mark is never a package item")
        self.assertFalse(updates._not_package("recomplauncher.json"))
        # RecompLauncher unpacks a new version over the folder: no package item may carry the name of one of the
        # installation's own items (the game's builds, the saves, settings, caches, logs, downloads/, the marks)
        own = set(layout.INSTALL_ITEMS) | {layout.ROOT_MARKER}
        for platform in ("windows", "linux"):
            self.assertFalse(own & set(layout.launcher_package(platform)), platform)


class WorkerRuleTests(unittest.TestCase):
    """the worker's own refusal (the launcher refuses first, with exit 8)"""

    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="recomp-", dir=os.environ.get("TMPDIR"))
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)

    def worker(self, *args, env=None):
        e = {k: v for k, v in os.environ.items() if not k.startswith("RECOMPLAUNCHER")}
        e.update(env or {}, HTTP_PROXY="http://127.0.0.1:9", HTTPS_PROXY="http://127.0.0.1:9", NO_PROXY="")
        r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "worker.py"), *args], env=e,
                           capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120)
        lines = [json.loads(x) for x in r.stdout.splitlines() if x.startswith("{")]
        return next((x for x in reversed(lines) if x.get("ev") == "result"), {})

    def test_managed_refuses_and_says_so(self):
        for env, mark in (({"RECOMPLAUNCHER": "1"}, False), ({}, True)):
            mp = os.path.join(self.root, layout.RECOMP_MARK)
            if mark:
                with open(mp, "w") as f:
                    f.write("RecompLauncher 0.90.0-beta.67\n")
            c = self.worker("check-update", "--install", self.root, "--launcher", self.root, env=env)
            self.assertTrue(c.get("ok"), c)
            self.assertEqual(c["data"].get("managed"), "RecompLauncher")
            self.assertFalse(c["data"].get("available"))
            for cmd in (["download-update", "--channel", "prerelease", "--version", "9.9.9"],
                        ["apply-update", "--version", "9.9.9"], ["rollback-launcher"]):
                r = self.worker(cmd[0], "--install", self.root, "--launcher", self.root, *cmd[1:], env=env)
                self.assertEqual(r.get("code"), "managed_elsewhere", (cmd, r))
            files = sorted(os.path.relpath(os.path.join(d, n), self.root).replace("\\", "/")
                           for d, _, ns in os.walk(self.root) for n in ns)
            self.assertEqual(files, [layout.RECOMP_MARK] if mark else [], "nothing downloaded, staged or kept")
            self.assertFalse(os.path.exists(os.path.join(self.root, "downloads")), "no downloads/ (the updater's)")
            if mark:
                os.remove(mp)

    def test_standalone_unchanged(self):
        c = self.worker("check-update", "--install", self.root, "--launcher", self.root)
        self.assertNotIn("managed", c.get("data", {}), c)


if __name__ == "__main__":
    unittest.main()

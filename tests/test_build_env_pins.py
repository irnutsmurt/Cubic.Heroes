#!/usr/bin/env python3
"""The Linux build environment fetches nothing unpinned (M11.4, the review's L-4): tools/linux_build_env/Dockerfile's
every download is checked by SHA-256, pip comes from its own pinned wheel (no get-pip.py), and requirements.txt
pins every package by version and by its wheel's SHA-256 (pip --require-hashes)."""
import os
import re
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENV = os.path.join(ROOT, "tools", "linux_build_env")


class BuildEnvPins(unittest.TestCase):
    def setUp(self):
        with open(os.path.join(ENV, "Dockerfile"), encoding="utf-8") as f:
            self.docker = f.read()

    def test_no_get_pip(self):
        code = "\n".join(ln for ln in self.docker.splitlines() if not ln.lstrip().startswith("#"))
        self.assertNotIn("bootstrap.pypa.io", code)
        self.assertNotRegex(code, r"get-pip\.py")

    def test_every_download_is_checked(self):
        runs = re.split(r"\n(?=RUN |COPY |ENV |ARG |FROM |WORKDIR )", self.docker)
        for run in runs:
            for out in re.findall(r"curl [^\n]*?-o (\S+)", run):
                self.assertRegex(run, re.escape(out.replace('"', "")) + r'"?\s*\|\s*sha256sum -c|sha256sum -c[^\n]*' +
                                 re.escape(os.path.basename(out)), f"{out}: downloaded without a SHA-256 check")

    def test_pip_installs_by_hash(self):
        for ln in self.docker.splitlines():
            if "pip install" in ln and "--no-index" not in ln:
                self.assertIn("--require-hashes", ln, ln)

    def test_requirements_pinned_by_version_and_hash(self):
        with open(os.path.join(ENV, "requirements.txt"), encoding="utf-8") as f:
            reqs = [ln.strip() for ln in f if ln.strip() and not ln.startswith("#")]
        self.assertTrue(reqs)
        for r in reqs:
            self.assertRegex(r, r"^[A-Za-z0-9_.-]+==[^\s]+ --hash=sha256:[0-9a-f]{64}$", r)


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""The Windows package's licences (the Windows review's W-7): the notices of the software CPython incorporates shipped
beside its licence, and the package licence audit's Windows rules (tests/package_license_audit.py audit_windows)."""
import io
import os
import shutil
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "tests"))
import package_license_audit as audit  # noqa: E402


class HtmlText(unittest.TestCase):
    def test_a_sphinx_page_as_text(self):
        import make_bundle
        page = ('<html><body><nav>skip me</nav><div class="body" role="main"><section><h2>expat<a class="headerlink" '
                'href="#x">¶</a></h2><p>The <code>pyexpat</code> extension is built with:</p>'
                '<pre>Copyright (c) 1998 Thai Open Source Software Center Ltd\n    and Clark Cooper</pre>'
                '<ul><li>one</li><li>two</li></ul></section></div><script>no()</script></body></html>')
        t = make_bundle.html_text(page)
        self.assertIn("## expat\n", t)
        self.assertNotIn("¶", t)
        self.assertIn("The pyexpat extension is built with:", t)
        self.assertIn("Copyright (c) 1998 Thai Open Source Software Center Ltd\n    and Clark Cooper", t)
        self.assertIn("- one", t)
        self.assertNotIn("skip me", t)
        self.assertNotIn("no()", t)

    @unittest.skipUnless(os.name == "nt", "the python.org Windows build's own documentation")
    def test_the_windows_worker_carries_cpythons_incorporated_software(self):
        # this Python's own pages (a Python installed without its documentation can't be checked here; make_bundle
        # refuses to package with one)
        import make_bundle
        if not os.path.isfile(os.path.join(sys.base_prefix, "Doc", "html", "license.html")):
            self.skipTest("this Python was installed without its documentation")
        tmp = tempfile.mkdtemp()
        try:
            make_bundle.cpython_incorporated_software(tmp, sys.base_prefix)
            with open(os.path.join(tmp, "Python-incorporated-software.txt"), encoding="utf-8") as f:
                text = f.read()
            for w in ("expat", "libmpdec", "Mersenne Twister", "SipHash", "Clark Cooper"):
                self.assertIn(w, text)
        finally:
            shutil.rmtree(tmp)

    @unittest.skipUnless(os.name == "nt", "the Windows worker's runtime")
    def test_the_windows_runtime_licences_together(self):
        # the whole set, where the package is made: PyInstaller and pycryptodome installed beside this Python (CI's
        # unit tests run without them)
        import make_bundle
        import sysconfig
        sites = [sysconfig.get_paths()["purelib"]]
        if not (make_bundle.dist_files(sites, "pyinstaller-*", "COPYING*")
                and make_bundle.dist_files(sites, "pycryptodome*", "LICENSE*")):
            self.skipTest("PyInstaller and pycryptodome not installed here (only the packaging needs them)")
        tmp = tempfile.mkdtemp()
        try:
            make_bundle.python_runtime_licences(tmp, "windows")
            names = set(os.listdir(tmp))
            self.assertTrue({"Python.txt", "Python-incorporated-software.txt", "pycryptodome.txt",
                             "PyInstaller.txt"} <= names, names)
            with open(os.path.join(tmp, "Python.txt"), encoding="utf-8") as f:
                self.assertIn("Microsoft Distributable Code", f.read())
        finally:
            shutil.rmtree(tmp)


class WindowsAudit(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.root = os.path.join(self.tmp, "Cubic.Heroes")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def put(self, rel, data=b"MZ\x90\x00"):
        p = os.path.join(self.root, *rel.split("/"))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb" if isinstance(data, bytes) else "w") as f:
            f.write(data)

    def package(self, incorporated=True, pillow=False):
        for rel in ("CubicHeroes.exe", "SDL2.dll", "worker/DotHeroes-Setup.exe", "worker/_internal/python312.dll",
                    "worker/_internal/pyexpat.pyd", "worker/_internal/_decimal.pyd", "worker/_internal/libffi-8.dll",
                    "worker/_internal/libssl-3.dll", "worker/_internal/VCRUNTIME140.dll",
                    "worker/_internal/Crypto/Cipher/_raw_aes.pyd"):
            self.put(rel)
        if pillow:
            self.put("worker/_internal/PIL/_imaging.cp312-win_amd64.pyd")
        for n, text in (("LICENSE.txt", "x"), ("licenses/Dear-ImGui.txt", "x"), ("licenses/SDL2.txt", "x"),
                        ("worker/licenses/pycryptodome.txt", "x"), ("worker/licenses/PyInstaller.txt", "x"),
                        ("worker/licenses/Python.txt", "PYTHON SOFTWARE FOUNDATION LICENSE bzip2 libffi Apache License "
                                                       "Microsoft Distributable Code")):
            self.put(n, text)
        if incorporated:
            self.put("worker/licenses/Python-incorporated-software.txt", "expat ... libmpdec ...")

    def run_audit(self):
        out = io.StringIO()
        with redirect_stdout(out):
            rc = audit.audit_windows(self.root, "test")
        return rc, out.getvalue()

    def test_a_complete_package_passes(self):
        self.package()
        rc, out = self.run_audit()
        self.assertEqual(rc, 0, out)

    def test_cpythons_incorporated_notices_are_required(self):
        self.package(incorporated=False)
        rc, out = self.run_audit()
        self.assertEqual(rc, 1)
        self.assertIn("Python-incorporated-software.txt is missing", out)
        self.put("worker/licenses/Python-incorporated-software.txt", "the PSF licence only")
        rc, out = self.run_audit()
        self.assertEqual(rc, 1)
        self.assertIn("does not hold its notice ('expat')", out)

    def test_an_unknown_module_fails_even_in_a_known_folder(self):
        """Pillow's modules under _internal/PIL are not CPython's (fnmatch's * crossed the folder before)"""
        self.package(pillow=True)
        rc, out = self.run_audit()
        self.assertEqual(rc, 1)
        self.assertIn("worker/_internal/PIL/_imaging.cp312-win_amd64.pyd: no component known", out)

    def test_path_rules(self):
        m = audit.path_match
        self.assertTrue(m("worker/_internal/_ssl.pyd", "worker/_internal/*.pyd"))
        self.assertFalse(m("worker/_internal/PIL/_imaging.pyd", "worker/_internal/*.pyd"))
        self.assertTrue(m("worker/_internal/Crypto/Cipher/_raw_aes.pyd", "worker/_internal/Crypto/**"))
        self.assertFalse(m("worker/_internal/CryptoX/a.pyd", "worker/_internal/Crypto/**"))


if __name__ == "__main__":
    unittest.main()

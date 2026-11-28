"""The launcher's prompt glyphs (beta.6, the owner's Store check by mouse): every label a keycap or a controller button
can show is drawn by the pixel font -- port/ui_pixel_font.h, shared with the game's overlay, plus the launcher's own
kLauncherGlyphs -- never by its "?" stand-in. The keyboard's "[" and "]" (the Character Store's tab keys) were drawn
"?" on Windows and Linux alike. Read from the sources: no build needed.

    py -3 -m unittest tests.test_launcher_glyphs
"""
import os
import re
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read(*p):
    with open(os.path.join(ROOT, *p), encoding="utf-8") as f:
        return f.read()


def glyph_chars(src):
    """the characters a table of PixGlyph entries defines: { 'A', { ... } }, with '\\'' and '\\\\' escaped"""
    out = set()
    for m in re.finditer(r"\{ '(\\.|.)', \{", src):
        c = m.group(1)
        out.add(c[1] if len(c) == 2 else c)
    return out


class Glyphs(unittest.TestCase):
    def setUp(self):
        self.main = read("launcher", "main.cpp")
        self.font = glyph_chars(read("port", "ui_pixel_font.h"))
        i = self.main.find("static const PixGlyph kLauncherGlyphs[]")   # none before beta.6's fix
        self.own = glyph_chars(self.main[i:self.main.index("};", i)]) if i >= 0 else set()
        i = self.main.index("static float glyph(ImDrawList* dl, ImVec2 p, Act a, float h)")
        self.body = self.main[i:self.main.index("\nstruct Prompt", i)]

    def labels(self):
        """every string the glyph function draws as a label: its t = "..." choices and its letters"""
        found = set(re.findall(r'"([^"]*)"', self.body))
        return {t for t in found if t and len(t) <= 8 and " " not in t}

    def test_every_label_is_drawn(self):
        labels = self.labels()
        for need in ("[", "]", "Q", "E", "Enter", "Esc", "F10", "LB", "RB", "LT", "RT", "L2", "R2", "MENU", "VIEW"):
            self.assertIn(need, labels)   # the parse sees the labels it checks
        have = self.font | self.own
        missing = {t: sorted({c.upper() for c in t} - have) for t in labels if {c.upper() for c in t} - have}
        self.assertEqual(missing, {}, "labels with characters the pixel font draws as '?'")

    def test_brackets_are_the_launchers_own(self):
        """the keyboard's tab keys: drawn by the launcher's glyphs; the game's shared font stays as it is"""
        self.assertEqual({"[", "]"} - self.own, set())
        self.assertNotIn("[", self.font)
        self.assertIn("lpix_glyph(*s)", self.main)
        self.assertNotIn("pix_glyph(*s)->rows", self.main.replace("lpix_glyph(*s)->rows", ""))


if __name__ == "__main__":
    unittest.main()

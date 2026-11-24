#!/usr/bin/env python3
"""Preview pictures of a player-made character, from its own voxels (tools/edt_format.py) and nothing else: no game
art, no hand-made mock-up. The same code makes them for the launcher's library, for imports and exports, for the
Hall of Heroes recovery and for the community store's GitHub workflow.

  thumbnail  one picture for lists: the Stand pose seen a little from the front-left and above, framed tightly.
  views      front | side | back of the Stand pose, side by side, at ONE zoom for every character (16 voxels -- the
             editor's whole grid -- always fit), so characters compare at their real sizes.

Deterministic: the same character always gives the same pixels. The camera angles have exact rational sines and
cosines (Pythagorean triples), so only +, -, *, / and comparisons are used -- no trigonometry, whose last bits can
differ between C libraries; drawing is painter's order with a total, stable sort. Orthographic, fixed lighting, a
neutral background. Each voxel is drawn as the game draws one: a cube, its faces lit by their direction, with a
darker edge.

Pure Python (zlib for the PNG): it runs in the launcher's worker and in a GitHub Actions job without extra packages.

  python tools/char_preview.py <DATA.EDT or .chchar> <out-dir>      writes thumb.png and views.png
"""
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import edt_format  # noqa: E402

RENDERER = "char_preview 1"          # recorded with every preview; bump when the pictures change
BACKGROUND = (59, 63, 70)            # neutral slate
EDGE = 0.70                          # an edge's brightness relative to its face
INSET = 0.12                         # the edge's width, a fraction of the face

# pitch: the camera 16.26 degrees above the horizon (sin 7/25, cos 24/25)
PITCH_SIN, PITCH_COS = 7 / 25, 24 / 25
# yaws as (cos, sin): front, side (the character's left), back; the thumbnail's 22.62 degrees (cos 12/13, sin 5/13)
YAW_FRONT, YAW_SIDE, YAW_BACK = (1.0, 0.0), (0.0, 1.0), (-1.0, 0.0)
YAW_THUMB = (12 / 13, -5 / 13)
# light, in the camera's frame (x right, y up, z away from the viewer): the direction it travels -- from the
# viewer's upper left into the scene, so faces toward the viewer show close to their own colour, as in the game
LIGHT = (9 / 25, -12 / 25, 20 / 25)
AMBIENT, DIFFUSE = 0.68, 0.38
# the editor's x runs to the character's right as seen from the front (a mirror flag kept in one place)
MIRROR_X = False       # checked in the editor: from its default camera (the character's front), Right = +x

FACES = (  # (normal, the four corners of the unit cube's face, in order)
    ((1, 0, 0), ((1, 0, 0), (1, 1, 0), (1, 1, 1), (1, 0, 1))),
    ((-1, 0, 0), ((0, 0, 0), (0, 0, 1), (0, 1, 1), (0, 1, 0))),
    ((0, 1, 0), ((0, 1, 0), (0, 1, 1), (1, 1, 1), (1, 1, 0))),
    ((0, -1, 0), ((0, 0, 0), (1, 0, 0), (1, 0, 1), (0, 0, 1))),
    ((0, 0, 1), ((0, 0, 1), (1, 0, 1), (1, 1, 1), (0, 1, 1))),
    ((0, 0, -1), ((0, 0, 0), (0, 1, 0), (1, 1, 0), (1, 0, 0))),
)


def _camera(yaw):
    """world (x, y, z) -> camera (right, up, depth); the character faces -z, the front camera stands at -z."""
    c, s = yaw
    mx = -1.0 if MIRROR_X else 1.0

    def to_cam(x, y, z):
        x = x * mx
        # yaw about y: the camera circles the character
        xr = c * x + s * z
        zr = -s * x + c * z
        # pitch: the camera above, looking down a little (view direction (0, -sin, cos), its up (0, cos, sin))
        up = y * PITCH_COS + zr * PITCH_SIN
        depth = zr * PITCH_COS - y * PITCH_SIN
        return xr, up, depth
    return to_cam


def _shade(rgb, n_cam):
    d = n_cam[0] * LIGHT[0] + n_cam[1] * LIGHT[1] + n_cam[2] * LIGHT[2]
    k = AMBIENT + DIFFUSE * max(0.0, -d)       # lit by the light coming toward it
    return tuple(min(255, int(ch * 255 * k + 0.5)) for ch in rgb)


class Canvas:
    def __init__(self, w, h, bg=BACKGROUND):
        self.w, self.h = w, h
        row = bytes(bg) * w
        self.rows = [bytearray(row) for _ in range(h)]

    def polygon(self, pts, rgb):
        """Fill a convex polygon (pixel coordinates); a pixel is in when its centre is."""
        ys = [p[1] for p in pts]
        y0 = max(0, int(min(ys) - 0.5) + 0)
        y1 = min(self.h - 1, int(max(ys) + 0.5))
        col = bytes(rgb)
        n = len(pts)
        for py in range(y0, y1 + 1):
            yc = py + 0.5
            xs = []
            for i in range(n):
                ax, ay = pts[i]
                bx, by = pts[(i + 1) % n]
                if (ay <= yc < by) or (by <= yc < ay):
                    xs.append(ax + (yc - ay) * (bx - ax) / (by - ay))
            if len(xs) < 2:
                continue
            xl, xr = min(xs), max(xs)
            a = max(0, int(xl + 0.5))
            b = min(self.w, int(xr + 0.5))
            if b > a:
                self.rows[py][a * 3:b * 3] = col * (b - a)

    def paste(self, other, ox, oy):
        for y in range(other.h):
            if 0 <= oy + y < self.h:
                self.rows[oy + y][ox * 3:(ox + other.w) * 3] = other.rows[y]

    def downsample(self, f):
        """f x f boxes averaged: the edges smoothed (supersampling)."""
        out = Canvas(self.w // f, self.h // f)
        for y in range(out.h):
            src = self.rows[y * f:(y + 1) * f]
            dst = out.rows[y]
            for x in range(out.w):
                r = g = b = 0
                for row in src:
                    seg = row[x * f * 3:(x + 1) * f * 3]
                    r += sum(seg[0::3])
                    g += sum(seg[1::3])
                    b += sum(seg[2::3])
                n = f * f
                dst[x * 3:x * 3 + 3] = bytes(((r + n // 2) // n, (g + n // 2) // n, (b + n // 2) // n))
        return out

    def png(self):
        raw = b"".join(b"\x00" + bytes(r) for r in self.rows)

        def chunk(t, d):
            return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
        return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", self.w, self.h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def _faces(voxels, yaw):
    """The visible faces, far to near: [(depth key, 2D corners in camera units, shaded colour, edge colour)]."""
    to_cam = _camera(yaw)
    occ = {(v.x, v.y, v.z) for v in voxels}
    normals = {}
    for n, _ in FACES:
        a = to_cam(*n)
        normals[n] = a
    out = []
    for v in voxels:
        cx, cy, cz = to_cam(v.x + 0.5, v.y + 0.5, v.z + 0.5)
        for n, corners in FACES:
            if (v.x + n[0], v.y + n[1], v.z + n[2]) in occ:
                continue                       # hidden by its neighbour
            nc = normals[n]
            if nc[2] >= 0:
                continue                       # facing away from the camera
            pts = [to_cam(v.x + a, v.y + b, v.z + c) for a, b, c in corners]
            col = _shade(v.rgb, nc)
            edge = tuple(int(ch * EDGE + 0.5) for ch in col)
            out.append(((-cz, -cy, v.x, v.y, v.z, n), [(p[0], p[1]) for p in pts], col, edge))
    out.sort(key=lambda f: f[0])               # far first; ties broken by position: a total order
    return out


def _draw(faces, canvas, ox, oy, scale):
    for _, pts, col, edge in faces:
        px = [(ox + x * scale, oy - y * scale) for x, y in pts]
        canvas.polygon(px, edge)
        cxm = sum(p[0] for p in px) / 4
        cym = sum(p[1] for p in px) / 4
        inner = [(cxm + (p[0] - cxm) * (1 - INSET), cym + (p[1] - cym) * (1 - INSET)) for p in px]
        canvas.polygon(inner, col)


def _bounds(faces):
    xs = [x for f in faces for x, _ in f[1]]
    ys = [y for f in faces for _, y in f[1]]
    return min(xs), max(xs), min(ys), max(ys)


SS = 3   # supersampling


def thumbnail(c, size=256, pose=0):
    """The list picture: size x size, the model framed tightly (8% margin)."""
    faces = _faces(c.poses[pose].voxels, YAW_THUMB) if c.poses and c.poses[pose].voxels else []
    if not faces:
        return placeholder(size, size)
    x0, x1, y0, y1 = _bounds(faces)
    big = size * SS
    scale = (big * 0.84) / max(x1 - x0, y1 - y0)
    canvas = Canvas(big, big)
    ox = big / 2 - (x0 + x1) / 2 * scale
    oy = big / 2 + (y0 + y1) / 2 * scale
    _draw(faces, canvas, ox, oy, scale)
    return canvas.downsample(SS)


def views(c, panel_w=300, panel_h=380, pose=0, gap=4):
    """Front | side | back at one fixed zoom: the editor's 16-voxel grid fits any panel's height, so every character
    is shown at the same scale; each view centred on the model."""
    vox = c.poses[pose].voxels if c.poses else []
    big_w, big_h = panel_w * SS, panel_h * SS
    # the tallest a 16-voxel model can be on screen at this pitch: 16 cos + 16 sin
    scale = big_h * 0.92 / (16 * (PITCH_COS + PITCH_SIN))
    out = Canvas(panel_w * 3 + gap * 2, panel_h, bg=(44, 47, 53))
    for i, yaw in enumerate((YAW_FRONT, YAW_SIDE, YAW_BACK)):
        faces = _faces(vox, yaw)
        if not faces:
            panel = placeholder(panel_w, panel_h)
        else:
            x0, x1, y0, y1 = _bounds(faces)
            canvas = Canvas(big_w, big_h)
            _draw(faces, canvas, big_w / 2 - (x0 + x1) / 2 * scale, big_h / 2 + (y0 + y1) / 2 * scale, scale)
            panel = canvas.downsample(SS)
        out.paste(panel, i * (panel_w + gap), 0)
    return out


_QMARK = ("01110", "10001", "00001", "00110", "00100", "00000", "00100")


def placeholder(w, h):
    """No preview could be made: a neutral picture with a question mark of blocks."""
    c = Canvas(w, h)
    cell = max(2, min(w, h) // 14)
    ox, oy = (w - 5 * cell) // 2, (h - 7 * cell) // 2
    for r, line in enumerate(_QMARK):
        for k, ch in enumerate(line):
            if ch == "1":
                c.polygon([(ox + k * cell + 1, oy + r * cell + 1), (ox + (k + 1) * cell - 1, oy + r * cell + 1),
                           (ox + (k + 1) * cell - 1, oy + (r + 1) * cell - 1), (ox + k * cell + 1, oy + (r + 1) * cell - 1)],
                          (140, 146, 156))
    return c


def png_to_rgb(png):
    """(w, h, RGB bytes) of a PNG this module wrote (8-bit RGB, one IDAT, filter 0 on every row)."""
    w, h, depth, colour = struct.unpack(">IIBB", png[16:26])
    if png[:8] != b"\x89PNG\r\n\x1a\n" or depth != 8 or colour != 2:
        raise ValueError("not a preview this module wrote")
    o, idat = 8, b""
    while o < len(png):
        n, t = struct.unpack(">I4s", png[o:o + 8])
        if t == b"IDAT":
            idat += png[o + 8:o + 8 + n]
        o += 12 + n
    raw = zlib.decompress(idat)
    stride = 1 + 3 * w
    if len(raw) != stride * h or any(raw[y * stride] for y in range(h)):
        raise ValueError("not a preview this module wrote")
    return w, h, b"".join(raw[y * stride + 1:(y + 1) * stride] for y in range(h))


def make(edt_bytes):
    """(thumb.png bytes, views.png bytes, ok): placeholders and ok False when the data is not a character."""
    try:
        c = edt_format.parse(edt_bytes)
    except edt_format.EdtError:
        return placeholder(256, 256).png(), placeholder(300 * 3 + 8, 380).png(), False
    return thumbnail(c).png(), views(c).png(), True


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, out = sys.argv[1], sys.argv[2]
    data = open(src, "rb").read()
    if data[:2] == b"PK":
        import zipfile
        with zipfile.ZipFile(src) as z:
            data = z.read("character.edt")
    os.makedirs(out, exist_ok=True)
    t, v, ok = make(data)
    open(os.path.join(out, "thumb.png"), "wb").write(t)
    open(os.path.join(out, "views.png"), "wb").write(v)
    print(f"{out}: thumb.png {len(t)} bytes, views.png {len(v)} bytes{'' if ok else ' (placeholders: not a character)'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

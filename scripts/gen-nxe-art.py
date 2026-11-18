#!/usr/bin/env python3
"""Bakes the NXE layout's built-in textures into assets/UI/nxe/.

Like gen-blades-art.py, all of it is drawn here from formulas - nothing is
traced or sampled from another picture - so the files are sLaunch's own.

  background.png  the whole screen: a lime sky misting to grey-white on the
                  right, out-of-focus rings of light, and a grey stage floor
                  curving up from the left with rings of light in it.
  tile.png        a panel's face, lime glass, stretched to every panel.
  panel.png       nine-slice (border 16): the dark glass sheet lists and
                  pop-ups sit on, lit along its top edge.
  highlight.png   nine-slice (border 16): the selection, a lime bar.
  frame.png       nine-slice (border 12): over every panel in a row, a sheen
                  across its top and a pale rim.
  icon_*.png      a panel's glyph: a white silhouette glossing to lime at its
                  foot, holes cut through, a soft shadow and its reflection on
                  the floor under it. Drawn here shape by shape; a skin's own
                  icon_<name> replaces any of them.
"""
import os
import numpy as np
from PIL import Image, ImageDraw, ImageFilter

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "UI", "nxe")


def smooth(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def save(rgb, a, name):
    img = np.dstack([np.clip(rgb, 0, 1), np.clip(a, 0, 1)[..., None]]) if a is not None else np.clip(rgb, 0, 1)
    Image.fromarray((img * 255 + 0.5).astype(np.uint8)).save(os.path.join(OUT, name), optimize=True)


def bokeh(w, h, n, seed, rmin, rmax, alpha):
    """Soft rings and discs of light, the out-of-focus kind."""
    rng = np.random.default_rng(seed)
    x, y = np.meshgrid(np.arange(w) + 0.5, np.arange(h) + 0.5)
    out = np.zeros((h, w))
    for _ in range(n):
        cx, cy, r = rng.uniform(0, w), rng.uniform(0, h), rng.uniform(rmin, rmax)
        d = np.hypot(x - cx, y - cy)
        ring = np.exp(-((d - r) / (0.12 * r + 1.2)) ** 2) * 0.6 + smooth(r, r * 0.5, d) * 0.35
        out = np.maximum(out, ring * rng.uniform(0.3, 1.0))
    return out * alpha


def background():
    w, h = 1280, 720
    x, y = np.meshgrid(np.arange(w) + 0.5, np.arange(h) + 0.5)
    lime, deep, mist = np.array([0.52, 0.72, 0.16]), np.array([0.30, 0.48, 0.06]), np.array([0.86, 0.90, 0.88])
    # The sky: lime, deeper toward the top left, misting to grey-white on the right.
    k = smooth(0.0, 1.0, (x / w) * 0.8 + (1.0 - y / 400.0) * -0.25 + 0.15)[..., None]
    sky = deep + (lime - deep) * smooth(0.0, 0.7, (x / w * 0.6 + y / 500.0))[..., None]
    sky = sky + (mist - sky) * (k ** 2.2) * 0.85
    sky = sky + bokeh(w, h, 22, 3, 8, 40, 0.035)[..., None]
    # The floor: a grey stage curving up from the left, lit toward the viewer.
    horizon = 440 - 52 * smooth(0.0, 1.0, x / 760.0)
    floor_k = smooth(horizon - 1.5, horizon + 1.5, y)[..., None]
    fl = 0.56 + 0.10 * smooth(horizon, horizon + 300, y) + 0.06 * (x / w) + 0.08 * np.exp(-((x - 900) / 400.0) ** 2)
    floor = np.dstack([fl, fl * 1.01, fl * 1.03])
    floor = floor + 0.10 * np.exp(-((y - horizon) / 6.0) ** 2)[..., None]       # a lit rim along the edge
    # Rings in the floor, bottom right, as a stage light would leave.
    d = np.hypot((x - 1050) / 1.0, (y - 640) * 2.6)
    rings = (0.5 + 0.5 * np.cos(d / 9.0)) * smooth(260, 60, d) * 0.06
    floor = floor + rings[..., None]
    rgb = sky * (1 - floor_k) + floor * floor_k
    rgb += (np.random.default_rng(7).random((h, w, 1)) - 0.5) / 255.0
    save(rgb, None, "background.png")


def tile():
    """A panel's face: lime glass, brighter at the top, with faint bokeh."""
    w, h = 336, 256
    x, y, d = rrect(w, h, 4, 0)
    cov = np.clip(0.5 - d, 0, 1)
    top, bot = np.array([0.72, 0.86, 0.20]), np.array([0.36, 0.56, 0.08])
    rgb = top + (bot - top) * smooth(0, h, y)[..., None]
    rgb = rgb + 0.10 * np.exp(-((x - w * 0.35) / 120.0) ** 2 - ((y - 40) / 70.0) ** 2)[..., None]
    rgb = rgb + bokeh(w, h, 12, 11, 10, 34, 0.05)[..., None]
    rgb = rgb + (np.exp(-((y - 1.5) / 1.2) ** 2) * 0.25)[..., None]               # lit top edge
    save(rgb, cov, "tile.png")


def rrect(w, h, rad, inset=0.0):
    x, y = np.meshgrid(np.arange(w) + 0.5, np.arange(h) + 0.5)
    qx = np.abs(x - w / 2) - (w / 2 - inset - rad)
    qy = np.abs(y - h / 2) - (h / 2 - inset - rad)
    d = np.hypot(np.maximum(qx, 0), np.maximum(qy, 0)) + np.minimum(np.maximum(qx, qy), 0) - rad
    return x, y, d                                   # d < 0 inside


def panel():
    w = h = 64
    x, y, d = rrect(w, h, 8, 1)
    cov = np.clip(0.5 - d, 0, 1)
    v = 0.36 - 0.12 * smooth(0, 64, y)                 # mid grey, darker below
    rgb = np.dstack([v, v * 1.02, v * 1.05])
    rim = np.exp(-((d + 1.0) / 0.9) ** 2) * cov
    rgb = rgb + (rim * 0.18)[..., None]
    save(rgb, cov * 0.94, "panel.png")


def highlight():
    w = h = 64
    x, y, d = rrect(w, h, 3, 1)
    cov = np.clip(0.5 - d, 0, 1)
    top, bot = np.array([0.62, 0.80, 0.14]), np.array([0.36, 0.60, 0.06])
    rgb = top + (bot - top) * smooth(0, 64, y)[..., None]
    rgb = rgb + (np.exp(-((y - 2.0) / 1.5) ** 2) * 0.22)[..., None]
    save(rgb, cov, "highlight.png")


def frame():
    w = h = 64
    x, y, d = rrect(w, h, 9, 0)
    cov = np.clip(0.5 - d, 0, 1)
    rim = np.exp(-((d + 0.8) / 0.8) ** 2) * cov * 0.45
    sheen = smooth(30, 2, y) * 0.16 * cov
    a = np.maximum(rim, sheen)
    save(np.ones((h, w, 3)), a, "frame.png")


def front():
    w = h = 64
    x, y, d = rrect(w, h, 9, 0)
    cov = np.clip(0.5 - d, 0, 1)
    a = (np.exp(-((d + 1.5) / 1.4) ** 2) * 0.95 + np.exp(-((d + 4.0) / 4.0) ** 2) * 0.30) * cov
    save(np.ones((h, w, 3)), np.clip(a, 0, 1), "front.png")


# --- icons -------------------------------------------------------------------
SS, IW, IH = 4, 220, 180          # supersampling; the glyph's box, reflection goes below


class Pen:
    """Draws one silhouette at SS x: fill() adds, cut() takes away."""
    def __init__(self):
        self.im = Image.new("L", (IW * SS, IH * SS), 0)
        self.d = ImageDraw.Draw(self.im)

    def _b(self, b):
        return [v * SS for v in b]

    def fill(self, kind, *a, v=255, **k):
        if kind == "poly":
            self.d.polygon([(x * SS, y * SS) for x, y in a[0]], fill=v)
        elif kind == "rr":
            self.d.rounded_rectangle(self._b(a[0]), radius=a[1] * SS, fill=v)
        elif kind == "arc":   # an annulus sector: box centre, r_out, r_in, start, end (degrees, PIL's)
            cx, cy, ro, ri, s0, s1 = a
            self.d.pieslice(self._b([cx - ro, cy - ro, cx + ro, cy + ro]), s0, s1, fill=v)
            self.d.pieslice(self._b([cx - ri, cy - ri, cx + ri, cy + ri]), s0 - 1, s1 + 1, fill=255 - v)
        elif kind == "line":
            self.d.line([(x * SS, y * SS) for x, y in a[0]], fill=v, width=int(a[1] * SS), joint="curve")
        else:
            getattr(self.d, kind)(self._b(a[0]), fill=v)

    def cut(self, kind, *a):
        self.fill(kind, *a, v=0)

    def circle(self, cx, cy, r, cut=False):
        (self.cut if cut else self.fill)("ellipse", [cx - r, cy - r, cx + r, cy + r])

    def mask(self, angle=0.0):
        im = self.im.rotate(angle, resample=Image.BICUBIC, center=(IW * SS / 2, IH * SS / 2)) if angle else self.im
        return np.asarray(im.resize((IW, IH), Image.LANCZOS), dtype=np.float64) / 255.0


def blur(a, r):
    return np.asarray(Image.fromarray((a * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(r)),
                      dtype=np.float64) / 255.0


def glyph(m, name):
    ys, xs = np.nonzero(m > 0.5)
    top, bot = ys.min(), ys.max()
    x0, x1 = xs.min(), xs.max()
    x, y = np.meshgrid(np.arange(IW), np.arange(IH))
    t = np.clip((y - top) / max(1, bot - top), 0, 1)
    # White, a grey sheen toward the middle, then past a gently sloped gloss
    # line pale lime brightening to a saturated lime-yellow at the foot.
    white, sheen = np.array([1.0, 1.0, 1.0]), np.array([0.80, 0.81, 0.82])
    pale, lime = np.array([0.90, 0.95, 0.70]), np.array([0.82, 0.96, 0.04])
    split = 0.55 + 0.07 * (x - (x0 + x1) / 2) / max(1, x1 - x0)
    up = white + (sheen - white) * (smooth(0.0, 1.0, t / split) ** 1.4)[..., None]
    lo = pale + (lime - pale) * smooth(0.0, 1.0, (t - split) / (1 - split + 1e-6))[..., None]
    rgb = np.where((t < split)[..., None], up, lo)
    # A bevel: the rim a touch darker, the top edge lit.
    rim = np.clip(m - blur(m, 2.5), 0, 1)
    rgb = rgb * (1 - 0.30 * rim[..., None])
    a = m
    # The shadow, down and right, under the glyph.
    sh = blur(np.roll(np.roll(m, 4, 0), 3, 1), 4) * 0.45
    out_a = a + sh * (1 - a)
    out_rgb = (rgb * a[..., None]) / np.maximum(out_a, 1e-6)[..., None]
    # The reflection on the floor: flipped about the foot, faded out fast.
    rh = 96
    H = IH + rh
    full_rgb, full_a = np.zeros((H, IW, 3)), np.zeros((H, IW))
    full_rgb[:IH], full_a[:IH] = out_rgb, out_a
    foot = bot + 3
    fl = a[::-1][IH - 1 - bot:][: H - foot]
    frgb = rgb[::-1][IH - 1 - bot:][: H - foot]
    n = fl.shape[0]
    fade = (0.30 * (1 - np.arange(n) / 70.0).clip(0, 1) ** 1.6)[:, None]
    ra = fl * fade
    seg_a = full_a[foot:foot + n]
    seg_rgb = full_rgb[foot:foot + n]
    na = ra + seg_a * (1 - ra)
    seg_rgb[:] = (frgb * ra[..., None] + seg_rgb * (seg_a * (1 - ra))[..., None]) / np.maximum(na, 1e-6)[..., None]
    seg_a[:] = na
    save(full_rgb, full_a, "icon_%s.png" % name)


def icons():
    def games():
        p = Pen()
        p.fill("rr", [34, 44, 186, 112], 34)
        p.fill("ellipse", [16, 58, 96, 168]); p.fill("ellipse", [124, 58, 204, 168])
        p.cut("rr", [52, 68, 84, 80], 3); p.cut("rr", [62, 58, 74, 90], 3)        # the cross
        for cx, cy in ((154, 60), (154, 88), (140, 74), (168, 74)):
            p.circle(cx, cy, 6.5, cut=True)
        return p.mask(-4)

    def music():
        p = Pen()
        p.fill("ellipse", [36, 128, 92, 168]); p.fill("ellipse", [128, 116, 184, 156])
        p.fill("rr", [80, 30, 94, 148], 4); p.fill("rr", [172, 18, 186, 136], 4)
        p.fill("poly", [(80, 30), (186, 18), (186, 44), (80, 56)])
        return p.mask(4)

    def photos():
        p = Pen()
        p.fill("rr", [22, 52, 198, 156], 14); p.fill("rr", [60, 38, 112, 60], 6)
        p.circle(110, 104, 38, cut=True); p.circle(110, 104, 29); p.circle(110, 104, 18, cut=True)
        p.cut("rr", [154, 64, 184, 78], 5)
        return p.mask(8)

    def settings():
        p = Pen()
        cx, cy = 110, 92
        for k in range(8):
            ang = k * np.pi / 4
            pts = []
            for dx, dy in ((-13, 0), (13, 0), (10, 74), (-10, 74)):
                pts.append((cx + dx * np.cos(ang) - dy * np.sin(ang), cy + dx * np.sin(ang) + dy * np.cos(ang)))
            p.fill("poly", pts)
        p.circle(cx, cy, 62); p.circle(cx, cy, 24, cut=True)
        return p.mask(-6)

    def bust(eyes):
        p = Pen()
        p.circle(110, 66, 44)
        p.fill("ellipse", [42, 116, 178, 236])
        p.cut("rectangle", [0, 172, IW, IH])
        p.fill("rr", [42, 150, 178, 172], 8)
        if eyes:
            p.cut("ellipse", [90, 54, 102, 74]); p.cut("ellipse", [118, 54, 130, 74])
        p.cut("rectangle", [0, 108, IW, 114])   # the neck's gap
        return p

    def mii():
        return bust(True).mask(0)

    def profile():
        return bust(False).mask(0)

    def network():
        p = Pen()
        for ro in (118, 86, 54):
            p.fill("arc", 110, 172, ro, ro - 20, 225, 315)
        p.circle(110, 160, 14)
        return p.mask(0)

    def bluetooth():   # the audio devices it pairs: headphones
        p = Pen()
        p.fill("arc", 110, 112, 84, 68, 180, 360)
        p.fill("rr", [20, 104, 66, 170], 16); p.fill("rr", [154, 104, 200, 170], 16)
        return p.mask(0)

    def power():
        p = Pen()
        p.fill("arc", 110, 100, 70, 50, 0, 360)
        p.cut("poly", [(110, 100), (78, 0), (142, 0)])
        p.fill("rr", [100, 14, 120, 98], 9)
        return p.mask(0)

    def videos():   # a screen with a play mark
        p = Pen()
        p.fill("rr", [18, 30, 202, 150], 16)
        p.cut("poly", [(92, 60), (92, 120), (144, 90)])
        p.fill("rr", [80, 152, 140, 166], 6)
        return p.mask(-6)

    def theming():   # a palette
        p = Pen()
        p.fill("ellipse", [16, 26, 204, 166])
        p.circle(78, 124, 17, cut=True)
        p.cut("ellipse", [36, 128, 72, 176])
        for cx, cy in ((60, 76), (98, 52), (142, 56), (172, 92), (156, 132)):
            p.circle(cx, cy, 11, cut=True)
        return p.mask(-8)

    def fonts():   # a capital A
        p = Pen()
        p.fill("poly", [(92, 18), (128, 18), (196, 166), (160, 166), (110, 50), (60, 166), (24, 166)])
        p.fill("rr", [62, 108, 158, 128], 2)
        return p.mask(0)

    def homebrew():   # a package
        p = Pen()
        p.fill("poly", [(110, 14), (192, 52), (192, 134), (110, 172), (28, 134), (28, 52)])
        p.fill("line", [(28, 52), (110, 90), (192, 52)], 7, v=0)
        p.fill("line", [(110, 90), (110, 176)], 7, v=0)
        p.fill("line", [(70, 33), (150, 71)], 6, v=0)
        return p.mask(0)

    def files():   # a folder
        p = Pen()
        p.fill("rr", [22, 30, 96, 60], 8)
        p.fill("rr", [22, 46, 198, 164], 12)
        p.fill("line", [(22, 70), (198, 70)], 6, v=0)
        return p.mask(-5)

    def web():   # a globe
        p = Pen()
        p.circle(110, 92, 76)
        p.fill("line", [(34, 92), (186, 92)], 6, v=0)
        p.fill("line", [(48, 54), (172, 54)], 5, v=0); p.fill("line", [(48, 130), (172, 130)], 5, v=0)
        p.fill("line", [(110, 16), (110, 168)], 6, v=0)
        p.d.ellipse([(110 - 36) * SS, 18 * SS, (110 + 36) * SS, 166 * SS], outline=0, width=5 * SS)
        return p.mask(-10)

    def chat():
        p = Pen()
        p.fill("ellipse", [18, 24, 202, 140])
        p.fill("poly", [(58, 118), (40, 170), (100, 132)])
        for cx in (70, 110, 150):
            p.circle(cx, 82, 11, cut=True)
        return p.mask(0)

    def random():   # a die
        p = Pen()
        p.fill("rr", [44, 26, 176, 158], 24)
        for cx, cy in ((76, 58), (144, 58), (110, 92), (76, 126), (144, 126)):
            p.circle(cx, cy, 11, cut=True)
        return p.mask(-12)

    for f in (games, music, photos, settings, mii, profile, network, bluetooth, power, videos,
              theming, fonts, homebrew, files, web, chat, random):
        glyph(f(), f.__name__)


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    background(); tile(); panel(); highlight(); frame(); icons()
    print("wrote", sorted(os.listdir(OUT)))

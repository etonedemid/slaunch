#!/usr/bin/env python3
"""Bakes the Blades layout's built-in textures into assets/UI/blades/.

All of it is drawn here from formulas - nothing is traced or sampled from
another picture - so the files are sLaunch's own and ship with it.

  spine_right.png  one blade as the right-hand stack shows it: its leading edge
                   on the left, its name tab standing out to the right. Straight;
                   the menu bends it row by row.
  spine.png        the same, mirrored, for the left-hand stack.
  light.png        what lies over an open blade's flat colour: the glow behind
                   its middle, the dark corners, the rings, the two glass bands.
  light_wide.png   the same without the bands, for a blade that fills the
                   screen: those screens keep their own header and legend
                   lines, and the menu draws slimmer bands to suit.
  info.png         the glossy black sheet a game's details sit on, as the
                   360's game pages had: grey glass on top, black below, a
                   sweep of shine across it and a pale rim. Stretched to fit.
  star.png         a lit rating star, gold, and star_off.png an unlit one.
"""
import os
import numpy as np
from PIL import Image, ImageDraw

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "UI", "blades")


def smooth(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def blur(a, sigma):
    r = int(sigma * 3) + 1
    k = np.exp(-(np.arange(-r, r + 1) ** 2) / (2.0 * sigma * sigma))
    k /= k.sum()
    a = np.apply_along_axis(lambda v: np.convolve(np.pad(v, r, mode="edge"), k, mode="valid"), 0, a)
    return np.apply_along_axis(lambda v: np.convolve(np.pad(v, r, mode="edge"), k, mode="valid"), 1, a)


def spine(scale=2):
    w, h = 56, 720                       # in menu pixels; baked at twice that
    x, y = np.meshgrid((np.arange(w * scale) + 0.5) / scale, (np.arange(h * scale) + 0.5) / scale)
    band = 11.0                          # the blade's own edge, a rounded rim
    t0, t1, tr, rad = 150.0, 560.0, 50.0, 11.0

    # The tab: its right side runs straight, rounds over at the top, and
    # sweeps back into the edge over its last stretch.
    sweep = smooth(t1 - 86.0, t1, y)
    right = band + (tr - band) * (1.0 - sweep)
    d = np.maximum(x - right, t0 - y)                                   # > 0 outside
    cx, cy = tr - rad, t0 + rad
    corner = (x > cx) & (y < cy)
    d = np.where(corner, np.hypot(x - cx, y - cy) - rad, d)
    tab = np.clip(0.5 - d, 0.0, 1.0) * (x >= band)

    v = 0.60 + 0.17 * np.clip((x - band) / (w - band), 0, 1)           # the blade behind, in shadow
    v *= 1.0 - 0.30 * blur(tab, 3.0) * (1.0 - tab)                      # ...darker where the tab stands over it
    tabv = 0.945 - 0.05 * (x - band) / (tr - band)
    tabv += 0.07 * np.exp(-(d / 1.3) ** 2) * (d < 0)                    # a lit rim just inside its outline
    v = v * (1.0 - tab) + tabv * tab
    v *= 1.0 - 0.22 * np.exp(-((d - 0.6) / 0.9) ** 2) * (d > -0.2) * (x >= band)   # the outline itself
    edge = 0.80 + 0.20 * np.exp(-((x - 4.2) / 2.8) ** 2)
    edge *= 1.0 - 0.30 * smooth(band - 2.2, band, x)                    # falls away into the join
    v = np.where(x < band, edge, v)
    v *= 1.0 - 0.28 * smooth(w - 4.5, w, x)                             # the gap before the next blade
    v *= 0.90 + 0.10 * np.clip(1.0 - np.abs(y / h - 0.42) * 1.5, 0.0, 1.0)   # catches the light mid-way
    a = smooth(0.0, 1.3, x)                                             # only the leading edge is soft
    rgb = np.stack([v * 0.985, v * 0.992, v], axis=-1)
    img = np.concatenate([rgb, a[..., None]], axis=-1)
    return Image.fromarray((np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8), "RGBA")


def over(dst, color, alpha):
    """Lay a flat colour with an alpha map over an RGBA float picture."""
    a0 = dst[..., 3]
    a = alpha + a0 * (1.0 - alpha)
    for i in range(3):
        dst[..., i] = np.where(a > 1e-6, (color[i] * alpha + dst[..., i] * a0 * (1.0 - alpha)) / np.maximum(a, 1e-6), 0)
    dst[..., 3] = a


def rrect(x, y, x0, y0, x1, y1, r):
    qx = np.abs(x - (x0 + x1) / 2) - ((x1 - x0) / 2 - r)
    qy = np.abs(y - (y0 + y1) / 2) - ((y1 - y0) / 2 - r)
    return np.hypot(np.maximum(qx, 0), np.maximum(qy, 0)) + np.minimum(np.maximum(qx, qy), 0) - r


def light(bands=True):
    w, h = 1024, 720
    x, y = np.meshgrid(np.arange(w) + 0.5, np.arange(h) + 0.5)
    u, v = x / w, y / h
    img = np.zeros((h, w, 4))
    white, black = (1.0, 1.0, 1.0), (0.0, 0.0, 0.0)
    r2 = ((u - 0.5) / 0.62) ** 2 + ((v - 0.47) / 0.66) ** 2
    over(img, black, 0.46 * smooth(0.30, 1.25, r2))                     # the corners fall off
    over(img, white, 0.50 * np.exp(-(((u - 0.47) / 0.30) ** 2 + ((v - 0.40) / 0.34) ** 2)))   # the glow
    for rad, a in ((300.0, 0.050), (430.0, 0.045), (560.0, 0.040), (690.0, 0.030)):   # rings, off the corner
        dist = np.abs(np.hypot(x - 0.86 * w, y - 1.22 * h) - rad)
        over(img, white, a * np.clip(1.6 - dist, 0.0, 1.0) / 1.6 * 1.6)
    for y0, y1 in ((30.0, 112.0), (600.0, 692.0)) if bands else ():     # the title's band, the legend's
        d = rrect(x, y, 0.035 * w, y0, 0.965 * w, y1, 18.0)
        inside = np.clip(0.5 - d, 0.0, 1.0)
        over(img, black, 0.16 * inside)
        over(img, white, inside * (0.20 - 0.17 * (y - y0) / (y1 - y0)))   # glass: lit along its top
        over(img, white, 0.30 * np.clip(1.0 - np.abs(d + 0.6), 0.0, 1.0) * smooth(y1, y0, y) ** 0.5)
    return Image.fromarray((np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8), "RGBA")


def info_panel():
    w, h = 480, 440
    x, y = np.meshgrid(np.arange(w) + 0.5, np.arange(h) + 0.5)
    u, v = x / w, y / h
    d = rrect(x, y, 3.0, 3.0, w - 3.0, h - 3.0, 14.0)
    inside = np.clip(0.5 - d, 0.0, 1.0)
    img = np.zeros((h, w, 4))
    top = v < 0.45                                                       # grey glass, then black
    grey = np.where(top, 0.27 - 0.17 * v / 0.45, 0.05 - 0.05 * (v - 0.45) / 0.55)
    for i in range(3):
        img[..., i] = grey
    img[..., 3] = 0.94 * inside
    shine = np.hypot(x - 1.30 * w, y - 1.45 * h) > 1.20 * h              # the sweep, upper left of an arc
    over(img, (1.0, 1.0, 1.0), inside * shine * 0.07 * smooth(1.0, 0.0, v))
    over(img, (1.0, 1.0, 1.0), 0.55 * np.clip(1.0 - np.abs(d + 1.0), 0.0, 1.0) * (0.45 + 0.55 * smooth(1.0, 0.0, v)))
    return Image.fromarray((np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8), "RGBA")


def star(lit=True, size=64, ss=4):
    n = size * ss
    c, ro, ri = n / 2, n * 0.47, n * 0.47 * 0.42
    pts = lambda k: [(c + r * np.sin(np.pi * i / 5), c + 0.04 * n - r * np.cos(np.pi * i / 5))
                     for i, r in enumerate([ro * k, ri * k] * 5)]
    rim, body = Image.new("L", (n, n)), Image.new("L", (n, n))
    ImageDraw.Draw(rim).polygon(pts(1.0), fill=255)
    ImageDraw.Draw(body).polygon(pts(0.86), fill=255)
    rim = np.asarray(rim, float) / 255
    body = np.asarray(body, float) / 255
    v = np.linspace(0.0, 1.0, n)[:, None] * np.ones((1, n))
    img = np.zeros((n, n, 4))
    if lit:
        over(img, (0.36, 0.24, 0.02), rim)                               # dark gold edge
        top, bot = np.array([1.0, 0.93, 0.45]), np.array([0.93, 0.62, 0.08])
        col = top[None, None, :] * (1 - v[..., None]) + bot[None, None, :] * v[..., None]
        a = body
        for i in range(3):
            img[..., i] = col[..., i] * a + img[..., i] * (1 - a)
        over(img, (1.0, 1.0, 0.85), body * 0.35 * smooth(0.55, 0.30, v))   # shine on the upper half
    else:
        over(img, (0.55, 0.55, 0.55), rim * 0.85)
        over(img, (0.16, 0.16, 0.16), body * 0.9)
    out = Image.fromarray((np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8), "RGBA")
    return out.resize((size, size), Image.LANCZOS)


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    s = spine()
    s.save(os.path.join(OUT, "spine_right.png"), optimize=True)
    s.transpose(Image.FLIP_LEFT_RIGHT).save(os.path.join(OUT, "spine.png"), optimize=True)
    light().save(os.path.join(OUT, "light.png"), optimize=True)
    light(bands=False).save(os.path.join(OUT, "light_wide.png"), optimize=True)
    p = info_panel()
    p.save(os.path.join(OUT, "info.png"), optimize=True)
    on, off = star(), star(lit=False)
    on.save(os.path.join(OUT, "star.png"), optimize=True)
    off.save(os.path.join(OUT, "star_off.png"), optimize=True)
    assert s.size == (112, 1440) and s.getpixel((0, 700))[3] < 40 and s.getpixel((60, 700))[3] == 255
    assert p.getpixel((0, 0))[3] == 0 and p.getpixel((240, 300))[3] > 200 and p.getpixel((240, 300))[0] < 30
    assert on.getpixel((32, 34))[3] == 255 and on.getpixel((32, 34))[0] > 200 and on.getpixel((2, 62))[3] == 0
    print("wrote", sorted(os.listdir(OUT)))

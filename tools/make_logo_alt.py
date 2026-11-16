"""Draws an alternative project logo that nods to the game's box logo without copying it.

A red tile with "TCS" in slanted, rounded white letters, outlined black and yellow; under it "NX" in wide
outlined gold letters; a thin gold rule between them. Every shape is drawn here: no font and no game artwork.

Writes assets/logo-alt-512.png (and assets/logo-alt-256.png for small previews).
Needs Pillow, NumPy and SciPy. Usage: python tools/make_logo_alt.py
"""
import math
import os
import random

import numpy as np
from PIL import Image, ImageDraw, ImageFilter
from scipy import ndimage

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = 512
SS = 4                      # supersampling
N = OUT * SS


def px(v):
    return int(round(v * SS))


# ---------- helpers ----------

def blank():
    return Image.new("L", (N, N), 0)


def stroke(draw, pts, width):
    """A thick polyline with round joins and caps."""
    w = px(width)
    p = [(px(x), px(y)) for x, y in pts]
    draw.line(p, fill=255, width=w)
    r = w / 2
    for x, y in p:
        draw.ellipse((x - r, y - r, x + r, y + r), fill=255)


def arc_pts(cx, cy, rx, ry, a0, a1, n=48):
    """Points on an ellipse, angles in degrees, y up (converted by the caller)."""
    return [(cx + rx * math.cos(math.radians(a0 + (a1 - a0) * i / n)),
             cy + ry * math.sin(math.radians(a0 + (a1 - a0) * i / n))) for i in range(n + 1)]


def dist_out(mask):
    """Distance (output pixels) from each outside pixel to the shape."""
    return ndimage.distance_transform_edt(mask < 128) / SS


def dist_in(mask):
    """Distance (output pixels) from each inside pixel to the edge."""
    return ndimage.distance_transform_edt(mask >= 128) / SS


def vgrad(top, bottom, y0, y1):
    """An N x N x 3 vertical gradient between y0 and y1 (output pixels)."""
    t = np.clip((np.arange(N) / SS - y0) / max(y1 - y0, 1e-6), 0, 1)[:, None, None]
    top, bottom = np.array(top, float), np.array(bottom, float)
    return np.broadcast_to(top + (bottom - top) * t, (N, N, 3))


def multi_grad(stops, y0, y1):
    """Vertical gradient through several (position, colour) stops."""
    t = np.clip((np.arange(N) / SS - y0) / max(y1 - y0, 1e-6), 0, 1)
    pos = np.array([s[0] for s in stops])
    cols = np.array([s[1] for s in stops], float)
    out = np.stack([np.interp(t, pos, cols[:, c]) for c in range(3)], axis=1)
    return np.broadcast_to(out[:, None, :], (N, N, 3))


def paint(canvas, colour, alpha):
    """canvas: N x N x 4 float; colour: N x N x 3 or a tuple; alpha: N x N in 0..1."""
    a = np.clip(alpha, 0, 1)[..., None]
    col = np.broadcast_to(np.array(colour, float), (N, N, 3)) if not isinstance(colour, np.ndarray) else colour
    canvas[..., :3] = canvas[..., :3] * (1 - a) + col * a
    canvas[..., 3:] = canvas[..., 3:] * (1 - a) + 255 * a


def soft(d, edge):
    """Antialiased coverage for 'distance <= edge' (d in output pixels)."""
    return np.clip(edge - d + 0.5, 0, 1)


# ---------- shapes ----------

def tcs_mask(x0, y0, u, slant):
    """Slanted rounded TCS. Letter cells are 4 x 6 units with 1.2 units between letters."""
    img = blank()
    d = ImageDraw.Draw(img)
    w = 1.55 * u
    h = 6.0

    def place(pts, gx):
        out = []
        for x, y in pts:                         # y up inside the letter, 0..6
            ys = h - y
            out.append((x0 + (gx + x + (y - h / 2) * slant) * u, y0 + ys * u))
        return out

    gx = 0.0                                     # T
    stroke(d, place([(0.0, 6.0), (4.0, 6.0)], gx), w)
    stroke(d, place([(2.0, 6.0), (2.0, 0.0)], gx), w)
    gx += 5.2                                    # C
    stroke(d, place(arc_pts(2.2, 3.0, 2.0, 3.0, 50, 310), gx), w)
    gx += 5.0                                    # S
    upper = arc_pts(2.0, 4.5, 1.85, 1.5, 15, 270)
    lower = arc_pts(2.0, 1.5, 1.85, 1.5, 90, -165)
    stroke(d, place(upper + lower, gx), w)
    return img


def nx_mask(x0, y0, u):
    """Wide geometric N and X with flat ends. Each letter is 5.4 x 5 units."""
    img = blank()
    d = ImageDraw.Draw(img)
    t = 1.35                                     # stroke thickness in units
    H = 5.0
    W = 5.4

    def poly(pts, gx):
        d.polygon([(px(x0 + (gx + x) * u), px(y0 + y * u)) for x, y in pts], fill=255)

    gx = 0.0                                     # N
    poly([(0, 0), (t, 0), (t, H), (0, H)], gx)
    poly([(W - t, 0), (W, 0), (W, H), (W - t, H)], gx)
    poly([(0, 0), (t * 1.15, 0), (W, H), (W - t * 1.15, H)], gx)
    gx += W + 0.9                                # X
    poly([(0, 0), (t * 1.1, 0), (W, H), (W - t * 1.1, H)], gx)
    poly([(W - t * 1.1, 0), (W, 0), (t * 1.1, H), (0, H)], gx)
    return img, (2 * W + 0.9) * u, H * u


def rounded_rect_mask(x0, y0, x1, y1, r):
    img = blank()
    ImageDraw.Draw(img).rounded_rectangle((px(x0), px(y0), px(x1), px(y1)), radius=px(r), fill=255)
    return img


# ---------- composition ----------

def build():
    canvas = np.zeros((N, N, 4), float)

    # Background: a rounded night-sky tile with a few stars.
    bg = np.asarray(rounded_rect_mask(0, 0, OUT - 1, OUT - 1, 84), float) / 255
    paint(canvas, multi_grad([(0, (24, 30, 48)), (0.55, (12, 15, 26)), (1, (5, 6, 11))], 0, OUT), bg)
    stars = blank()
    sd = ImageDraw.Draw(stars)
    rng = random.Random(1977)
    for _ in range(70):
        x, y = rng.uniform(24, OUT - 24), rng.uniform(24, OUT - 24)
        r = rng.choice([0.6, 0.8, 1.0, 1.3])
        sd.ellipse((px(x - r), px(y - r), px(x + r), px(y + r)), fill=rng.randint(90, 230))
    paint(canvas, (235, 240, 255), np.asarray(stars, float) / 255 * bg)

    # Red tile with TCS.
    tx0, ty0, tx1, ty1 = 112, 30, 400, 252
    tile = rounded_rect_mask(tx0, ty0, tx1, ty1, 20)
    tm = np.asarray(tile, float) / 255
    shadow = np.asarray(tile.filter(ImageFilter.GaussianBlur(px(7))), float) / 255
    shadow = np.roll(shadow, px(5), axis=0)
    paint(canvas, (0, 0, 0), shadow * 0.55 * bg)
    paint(canvas, multi_grad([(0, (238, 52, 40)), (0.5, (214, 22, 22)), (1, (168, 8, 14))], ty0, ty1), tm)
    rim = np.clip(dist_in(np.asarray(tile)) < 2.2, 0, 1) * tm
    paint(canvas, (120, 0, 6), rim * 0.65)

    u = 16.0
    word_w = (4 + 5.2 + 5.0 + 0.3) * u
    letters = tcs_mask((tx0 + tx1) / 2 - word_w / 2 - 0.4 * u, (ty0 + ty1) / 2 - 3.0 * u, u, 0.20)
    lm = np.asarray(letters)
    do = dist_out(lm)
    paint(canvas, (255, 214, 0), soft(do, 9.0))                 # yellow outer outline
    paint(canvas, (16, 16, 18), soft(do, 5.0))                  # black outline
    white = multi_grad([(0, (255, 255, 255)), (1, (226, 230, 236))], ty0 + 40, ty1 - 40)
    paint(canvas, white, np.asarray(letters.filter(ImageFilter.GaussianBlur(SS * 0.35)), float) / 255)
    # A small highlight on the top of each letter, like a printed brick.
    hi = np.asarray(letters, float) / 255 * np.clip(1 - dist_in(lm) / 3.0, 0, 1)
    hi *= (np.arange(N)[:, None] / SS < (ty0 + ty1) / 2 - 1.2 * u)
    paint(canvas, (255, 255, 255), hi * 0.35)

    # Gold rule between the tile and NX, fading at both ends.
    ry = 282
    xs = np.arange(N) / SS
    fade = np.clip(1 - np.abs(xs - OUT / 2) / 190, 0, 1) ** 0.6
    ys = np.arange(N)[:, None] / SS
    for off, thick in ((-3.0, 1.1), (3.0, 1.1)):
        line = np.clip(thick - np.abs(ys - (ry + off)) + 0.5, 0, 1) * fade[None, :]
        paint(canvas, (226, 182, 86), line * bg)
    diamond = blank()
    ImageDraw.Draw(diamond).polygon([(px(OUT / 2), px(ry - 9)), (px(OUT / 2 + 9), px(ry)),
                                     (px(OUT / 2), px(ry + 9)), (px(OUT / 2 - 9), px(ry))], fill=255)
    dm = np.asarray(diamond, float) / 255
    paint(canvas, (12, 15, 26), np.clip(dist_out(np.asarray(diamond)) < 3.5, 0, 1) * 1.0)
    paint(canvas, vgrad((255, 240, 180), (176, 120, 30), ry - 9, ry + 9), dm)

    # Gold NX: a rich gold border around a pale, polished centre, with a thin dark edge.
    gu = 26.0
    tmp, w_nx, h_nx = nx_mask(0, 0, gu)
    nx, _, _ = nx_mask(OUT / 2 - w_nx / 2, 310, gu)
    gm = np.asarray(nx)
    gy0, gy1 = 310, 310 + h_nx
    do = dist_out(gm)
    di = dist_in(gm)
    inside = np.asarray(nx.filter(ImageFilter.GaussianBlur(SS * 0.35)), float) / 255
    paint(canvas, (34, 22, 4), soft(do, 2.6))                                       # dark edge
    border = multi_grad([(0, (255, 226, 120)), (0.35, (196, 140, 34)), (0.55, (250, 210, 104)),
                         (0.8, (150, 100, 18)), (1, (220, 170, 60))], gy0, gy1)
    paint(canvas, border, inside)                                                  # gold band
    core = np.clip((di - 6.0) * 1.0 + 0.5, 0, 1)
    centre = multi_grad([(0, (255, 252, 236)), (0.5, (248, 232, 184)), (1, (226, 196, 120))], gy0, gy1)
    paint(canvas, centre, core)                                                    # pale centre
    inner_line = np.clip(1 - np.abs(di - 6.0) / 0.9, 0, 1) * (di > 0)
    paint(canvas, (120, 80, 12), inner_line * 0.8)                                 # engraved line

    img = Image.fromarray(np.clip(canvas, 0, 255).astype(np.uint8), "RGBA")
    return img.resize((OUT, OUT), Image.LANCZOS)


def main():
    img = build()
    os.makedirs(os.path.join(ROOT, "assets"), exist_ok=True)
    img.save(os.path.join(ROOT, "assets", "logo-alt-512.png"), optimize=True)
    img.resize((256, 256), Image.LANCZOS).save(os.path.join(ROOT, "assets", "logo-alt-256.png"), optimize=True)
    print("assets/logo-alt-512.png, assets/logo-alt-256.png written")


if __name__ == "__main__":
    main()

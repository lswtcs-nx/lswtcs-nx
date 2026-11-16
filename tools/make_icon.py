"""Makes the NRO icon (source/switch/icon.jpg, 256x256) from the game's cover art.

The HOME menu, sphaira and any forwarder made from the NRO show this icon. The cover is not in the repository:
pass your own image. The script skips a white store banner at the top if there is one, takes the largest square
from the top of the artwork (the logo and the characters), and scales it to 256x256.

Usage: python tools/make_icon.py <cover image> [--top ROW]
"""
import argparse
import os

import numpy as np
from PIL import Image, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def art_top(img):
    """First row below a mostly white banner, or 0 when the image starts with artwork."""
    a = np.asarray(img).astype(int)
    white = (a.min(axis=2) > 240).mean(axis=1)
    if white[: img.height // 4].max() < 0.05:
        return 0
    for y in range(1, img.height // 2):
        if white[y - 1] >= 0.05 and white[y:y + 8].max() < 0.05:
            return y
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("cover")
    ap.add_argument("--top", type=int, help="first row of the artwork (found automatically by default)")
    ap.add_argument("--out", default=os.path.join(ROOT, "source", "switch", "icon.jpg"))
    args = ap.parse_args()

    img = Image.open(args.cover).convert("RGB")
    top = art_top(img) if args.top is None else args.top
    side = min(img.width, img.height - top)
    x0 = (img.width - side) // 2
    icon = img.crop((x0, top, x0 + side, top + side)).resize((256, 256), Image.LANCZOS)
    icon = icon.filter(ImageFilter.UnsharpMask(radius=0.8, percent=40, threshold=1))
    icon.save(args.out, quality=95, subsampling=0)
    print("%s written from rows %d-%d of %s" % (args.out, top, top + side, os.path.basename(args.cover)))


if __name__ == "__main__":
    main()

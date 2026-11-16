"""Draws the project's own logo: "TCS" over "NX" in block letters, Joy-Con blue and red, no font and no game artwork.

Writes, from one geometry:
  assets/logo.svg            for the README and the repository
  assets/logo-512.png        for an organisation or repository avatar
  source/switch/icon.jpg     256x256, the NRO's icon (HOME menu, sphaira, forwarders made from the NRO)
Needs Pillow for the PNG and the JPG. Usage: python tools/make_logo.py
"""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIZE = 256
BG = (27, 30, 36)
WHITE = (242, 244, 247)
BLUE = (10, 185, 230)    # Joy-Con neon blue
RED = (255, 60, 40)      # Joy-Con neon red

GLYPHS = {   # 5 x 7 cells
    'T': ["#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."],
    'C': [".####", "#....", "#....", "#....", "#....", "#....", ".####"],
    'S': [".####", "#....", "#....", ".###.", "....#", "....#", "####."],
    'N': ["#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#"],
    'X': ["#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"],
}


def word_rects(text, cell, x0, y0, colours):
    """Rectangles (x, y, w, h, colour) for a word; one colour per letter."""
    out = []
    for i, ch in enumerate(text):
        gx = x0 + i * 6 * cell
        for row, line in enumerate(GLYPHS[ch]):
            for col, c in enumerate(line):
                if c == '#':
                    out.append((gx + col * cell, y0 + row * cell, cell, cell, colours[i]))
    return out


def geometry():
    rects = []
    big = 12                                   # TCS: 3 letters, 17 cells wide
    w_big = (3 * 6 - 1) * big
    x_big = (SIZE - w_big) // 2
    rects += word_rects("TCS", big, x_big, 40, [WHITE, WHITE, WHITE])
    half = w_big // 2                          # the split bar: blue then red, as a pair of Joy-Cons
    rects.append((x_big, 136, half - 4, 6, BLUE))
    rects.append((x_big + half + 4, 136, w_big - half - 4, 6, RED))
    small = 8                                  # NX: 2 letters, 11 cells wide
    w_small = (2 * 6 - 1) * small
    rects += word_rects("NX", small, (SIZE - w_small) // 2, 160, [BLUE, RED])
    return rects


def hexc(c):
    return '#%02x%02x%02x' % c


def write_svg(path, rects):
    lines = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%d" height="%d">' % (SIZE, SIZE, SIZE, SIZE),
             '  <title>TCS NX</title>',
             '  <rect width="%d" height="%d" rx="40" fill="%s"/>' % (SIZE, SIZE, hexc(BG))]
    for x, y, w, h, c in rects:
        lines.append('  <rect x="%d" y="%d" width="%d" height="%d" fill="%s"/>' % (x, y, w, h, hexc(c)))
    lines.append('</svg>')
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')


def raster(rects, scale, rounded):
    from PIL import Image, ImageDraw
    n = SIZE * scale
    img = Image.new('RGB', (n, n), BG if not rounded else (0, 0, 0))
    d = ImageDraw.Draw(img)
    if rounded:
        img = Image.new('RGBA', (n, n), (0, 0, 0, 0))
        d = ImageDraw.Draw(img)
        d.rounded_rectangle((0, 0, n - 1, n - 1), radius=40 * scale, fill=BG + (255,))
    for x, y, w, h, c in rects:
        d.rectangle((x * scale, y * scale, (x + w) * scale - 1, (y + h) * scale - 1), fill=c if not rounded else c + (255,))
    return img


def main():
    rects = geometry()
    os.makedirs(os.path.join(ROOT, 'assets'), exist_ok=True)
    write_svg(os.path.join(ROOT, 'assets', 'logo.svg'), rects)
    raster(rects, 2, True).save(os.path.join(ROOT, 'assets', 'logo-512.png'))
    raster(rects, 1, False).save(os.path.join(ROOT, 'source', 'switch', 'icon.jpg'), quality=95)   # square: the HOME menu rounds it
    print('assets/logo.svg, assets/logo-512.png, source/switch/icon.jpg written')


if __name__ == '__main__':
    main()

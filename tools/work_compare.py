"""Compares the work two sessions did, per level, from their [WORK] lines (log111).

[WORK] lines (gl_backend.cpp, every 60 replays with the testing profile's diagnostics) count work that is the same on any
host: nvc0 draws, vertices, the bytes of vertex and pixel program code bound per draw, and guest calls per replay. So two
emulator runs of the same script can tell whether a build does less work, while their frame times cannot (the emulator's
speed is the PC's). Pixel-program bytes x draws stands in for shading work; it does not count pixels.

Each [WORK] window is put under the level named by the last [LEVEL] line before it. Windows with under 150 nvc0 draws
per replay (menus, loading) go under "<level> (light)", so they do not dilute the play windows.

Usage: python tools/work_compare.py <run A folder or stdout.log> <run B folder or stdout.log>
       python tools/work_compare.py <run>          (one run: its table only)
"""
import re
import sys
from collections import defaultdict
from pathlib import Path

WORK = re.compile(r"\[WORK\] per replay \((\d+)\): nvc0 draws ([\d.]+), vertices ([\d.]+) k, vertex program ([\d.]+) KB, "
                  r"pixel program ([\d.]+) KB .*\| guest calls ([\d.]+) k")
LEVEL = re.compile(r"^\[LEVEL\] (.+?)(?: \(frame \d+\))?\s*$")
FIELDS = ["draws", "vertices k", "VS KB", "PS KB", "guest calls k"]


def load(arg):
    p = Path(arg)
    if p.is_dir():
        p = p / "stdout.log"
    sums = defaultdict(lambda: [0.0] * (len(FIELDS) + 1))   # level -> weighted sums + replays
    level = "?"
    for raw in p.read_bytes().decode("utf-8", "replace").splitlines():
        m = LEVEL.match(raw)
        if m:
            level = m.group(1).split("\\")[-1]
            continue
        m = WORK.search(raw)
        if not m:
            continue
        n = int(m.group(1))
        vals = [float(m.group(i)) for i in range(2, 7)]
        key = level + (" (light)" if vals[0] < 150 else "")
        s = sums[key]
        for i, v in enumerate(vals):
            s[i] += v * n
        s[-1] += n
    return {k: ([v / s[-1] for v in s[:-1]], int(s[-1])) for k, s in sums.items() if s[-1]}


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    a = load(sys.argv[1])
    b = load(sys.argv[2]) if len(sys.argv) == 3 else None
    if not a:
        sys.exit("no [WORK] lines in the first run (a build before log111, or diagnostics off)")
    levels = sorted(set(a) | set(b or {}))
    for lv in levels:
        ra, rb = a.get(lv), (b or {}).get(lv)
        head = f"{lv}: A {ra[1] if ra else 0} replays" + (f", B {rb[1] if rb else 0} replays" if b is not None else "")
        print(head)
        for i, f in enumerate(FIELDS):
            va = ra[0][i] if ra else None
            if b is None:
                print(f"  {f:14} {va:10.1f}")
                continue
            vb = rb[0][i] if rb else None
            if va is None or vb is None:
                print(f"  {f:14} {'-' if va is None else f'{va:10.1f}':>10} {'-' if vb is None else f'{vb:10.1f}':>10}")
                continue
            delta = (vb - va) / va * 100 if va else 0.0
            print(f"  {f:14} {va:10.1f} {vb:10.1f} {delta:+7.1f} %")
    if b is not None:
        print("\nB against A. The same script on the same build gives a few % between runs (load timing shifts which frames "
              "fall in each window); look for changes well beyond that, and in the field the change was meant to move.")


if __name__ == "__main__":
    main()

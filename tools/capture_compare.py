"""Compares the screenshots two scripted sessions took at the same script steps (visual regressions).

An input script's 'capture' step makes the render thread save shot_<replay>.png in the session's log folder and print
[GLSHOT]. This pairs each capture with the script line that asked for it ("[SCRIPT] line N capture"), so captures of
two sessions of the same script match by step, not by replay number.

For each pair it prints:
  - the share of pixels that differ by more than --threshold (0-255, any channel) and the mean difference;
  - for each image, the share of pixels still at the scene's clear colour (13, 13, 20) and of near-black pixels, which
    flags blank or missing frames even without a reference;
and writes <out>/step<N>.png: A | B | the difference x4.

Two sessions never show exactly the same frame (animation, camera, load timing), so small differences are normal. What
this catches is the gross kind: a black or empty frame, missing geometry or textures, wrong colours across the screen.
Without a second session it only reports the blank-frame shares.

Needs Pillow and NumPy on the PC (not part of the NRO).
Usage: python tools/capture_compare.py <run A folder> [<run B folder>] [--out folder] [--threshold 24]
"""
import argparse
import re
import sys
from pathlib import Path

try:
    import numpy as np
    from PIL import Image
except ImportError:
    sys.exit("needs Pillow and NumPy: pip install pillow numpy")

CLEAR = (13, 13, 20)
STEP = re.compile(r"^\[SCRIPT\] line (\d+) capture")
SHOT = re.compile(r"^\[GLSHOT\] (\S+) ")


def captures(run):
    """{script line: png path} for one session, plus captures not tied to a script step under negative keys."""
    run = Path(run)
    out, pending, loose = {}, [], 0
    for raw in (run / "stdout.log").read_bytes().decode("utf-8", "replace").splitlines():
        m = STEP.match(raw)
        if m:
            pending.append(int(m.group(1)))
            continue
        m = SHOT.match(raw)
        if m:
            path = run / Path(m.group(1).replace("\\", "/")).name
            if pending:
                out[pending.pop(0)] = path
            else:
                loose -= 1
                out[loose] = path
    return out


def load(p):
    return np.asarray(Image.open(p).convert("RGB"), dtype=np.int16)


def shares(img):
    clear = np.all(img == np.array(CLEAR, dtype=np.int16), axis=2).mean()
    black = (img.max(axis=2) < 16).mean()
    return clear, black


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("a")
    ap.add_argument("b", nargs="?")
    ap.add_argument("--out", help="folder for the side-by-side images (default: <run B>/compare or <run A>/compare)")
    ap.add_argument("--threshold", type=int, default=24)
    args = ap.parse_args()
    ca = captures(args.a)
    cb = captures(args.b) if args.b else {}
    if not ca:
        sys.exit("no [GLSHOT] captures in the first session")
    out = Path(args.out) if args.out else Path(args.b or args.a) / "compare"
    flagged = 0
    for step in sorted(set(ca) | set(cb)):
        label = f"line {step}" if step > 0 else f"capture {-step} (no script step)"
        pa, pb = ca.get(step), cb.get(step)
        if pa is not None and not pa.exists():
            print(f"{label}: {pa.name} missing (still queued for the card when the session ended?)")
            pa = None
        if pb is not None and not pb.exists():
            print(f"{label}: {pb.name} missing in B")
            pb = None
        imgs = {k: load(p) for k, p in (("A", pa), ("B", pb)) if p is not None}
        notes = []
        for k, img in imgs.items():
            c, bl = shares(img)
            notes.append(f"{k} clear colour {c:.0%}, near-black {bl:.0%}")
            if c > 0.9 or bl > 0.9:
                notes[-1] += " <- blank?"
                flagged += 1
        line = f"{label}: " + "; ".join(notes)
        if len(imgs) == 2:
            a, b = imgs["A"], imgs["B"]
            if a.shape != b.shape:
                line += f"; sizes differ {a.shape[1]}x{a.shape[0]} vs {b.shape[1]}x{b.shape[0]}"
            else:
                d = np.abs(a - b)
                differ = (d.max(axis=2) > args.threshold).mean()
                line += f"; {differ:.1%} of pixels differ by more than {args.threshold}, mean {d.mean():.1f}"
                out.mkdir(parents=True, exist_ok=True)
                side = np.concatenate([a, b, np.clip(d * 4, 0, 255)], axis=1).astype(np.uint8)
                Image.fromarray(side).save(out / f"step{step}.png")
        elif args.b:
            line += "; only in " + ("A" if "A" in imgs else "B")
        print(line)
    if out.exists():
        print("side by side (A | B | difference x4):", out)
    return 1 if flagged else 0


if __name__ == "__main__":
    sys.exit(main())

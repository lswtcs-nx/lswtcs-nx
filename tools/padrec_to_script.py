"""Turns a played route into an input script (LSWTCS_INPUTSCRIPT).

Reads the [PADREC] lines a session with LSWTCS_INPUTRECORD=1 wrote to stdout.log and writes a script for
source/switch/include/lsw_input_script.h:
  - each change of buttons becomes a hold of that length; sticks become 'stick' lines;
  - idle time becomes 'wait';
  - a change of level becomes 'waitlevel', and the first play frames after light ones (a load or a menu ending)
    become 'waitplay 500', so a replay waits for the game's loads instead of trusting the recorded clock.
The result is a starting point: read it, trim the start (title, user selection) and add 'mark' or 'log' steps.

--frames (log113; the recording must come from a build that logs "frame N", and both the recording and the replay
should run with LSWTCS_FIXEDDT=1): the script follows guest frames instead of milliseconds. Each pad change becomes
'waitframe K' then 'pad BUTTONS' / 'stick', with frame 0 reset ('frameset') at every switch between light frames
(menus, loading) and play frames, after 'waitplay 0' or 'waitlight 0'. A load that takes longer then shifts nothing
after it.

Usage: python tools/padrec_to_script.py <stdout.log> [-o script.txt] [--from-ms N] [--to-ms N] [--frames]
       [--from-level TEXT]   start at the first play frame of the first level whose path contains TEXT
"""
import argparse
import re
import sys
from pathlib import Path

LINE = re.compile(r"\[PADREC\] (\d+) ms buttons 0x([0-9A-Fa-f]+) lt (\d) rt (\d) l (-?\d+) (-?\d+) r (-?\d+) (-?\d+) (light|play) "
                  r"level (\S+)(?: frame (\d+))?\s*$")
NAMES = [(0x0001, "UP"), (0x0002, "DOWN"), (0x0004, "LEFT"), (0x0008, "RIGHT"), (0x0010, "START"), (0x0020, "BACK"),
         (0x0040, "LS"), (0x0080, "RS"), (0x0100, "LB"), (0x0200, "RB"), (0x1000, "A"), (0x2000, "B"), (0x4000, "X"), (0x8000, "Y")]


def button_names(bits, lt, rt):
    names = [n for b, n in NAMES if bits & b]
    if lt:
        names.append("LT")
    if rt:
        names.append("RT")
    return "+".join(names)


def axis(v):
    return f"{max(-1.0, min(1.0, v / 16.0)):.2f}".rstrip("0").rstrip(".") if v else "0"


def convert(records):
    out = []
    t_prev = records[0][0]
    level_prev = records[0][9]
    light_prev = records[0][8] == "light"
    held = None   # (names, since)
    sticks = {"L": (0, 0), "R": (0, 0)}

    def flush_wait(t):
        nonlocal t_prev
        if t - t_prev >= 20:
            out.append(f"wait {t - t_prev}")
        t_prev = t

    for t, bits, lt, rt, lx, ly, rx, ry, frames, level, _frame in records:
        light = frames == "light"
        names = button_names(bits, lt, rt)
        if held and names != held[0]:
            out.append(f"hold {held[0]} {max(1, t - held[1])}")
            t_prev = t
            held = None
        if level != level_prev and level != "?":
            out.append(f"waitlevel {level.split(chr(92))[-1]}")
            t_prev = t
        elif light_prev and not light and not names:
            out.append("waitplay 500")
            t_prev = t
        level_prev, light_prev = level, light
        for side, (x, y) in (("L", (lx, ly)), ("R", (rx, ry))):
            if sticks[side] != (x, y):
                if not held:
                    flush_wait(t)
                out.append(f"stick {side} {axis(x)} {axis(y)}")
                sticks[side] = (x, y)
        if names and not held:
            flush_wait(t)
            held = (names, t)
    if held:
        out.append(f"hold {held[0]} 150")
    for side in ("L", "R"):
        if sticks[side] != (0, 0):
            out.append(f"stick {side} 0 0")
    out.append("done")
    return out


def convert_frames(records):
    """Frame-exact script: waitframe K + pad / stick, frame 0 reset at every light/play switch."""
    out = []
    base = records[0][10]
    out.append("frameset")
    light_prev = records[0][8] == "light"
    level_prev = records[0][9]
    pad_prev = None
    sticks = {"L": (0, 0), "R": (0, 0)}
    for t, bits, lt, rt, lx, ly, rx, ry, frames, level, frame in records:
        light = frames == "light"
        if level != level_prev and level != "?":
            out.append(f"waitlevel {level.split(chr(92))[-1]}")
        if light != light_prev:
            out.append("waitlight 0" if light else "waitplay 0")
            out.append("frameset")
            base = frame
        level_prev, light_prev = level, light
        changes = []
        names = button_names(bits, lt, rt) or "-"
        if names != pad_prev:
            changes.append(f"pad {names}")
            pad_prev = names
        for side, (x, y) in (("L", (lx, ly)), ("R", (rx, ry))):
            if sticks[side] != (x, y):
                changes.append(f"stick {side} {axis(x)} {axis(y)}")
                sticks[side] = (x, y)
        if changes:
            if frame > base:
                out.append(f"waitframe {frame - base}")
            out.extend(changes)
    out.append("pad -")
    for side in ("L", "R"):
        if sticks[side] != (0, 0):
            out.append(f"stick {side} 0 0")
    out.append("done")
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("log")
    ap.add_argument("-o", "--output")
    ap.add_argument("--from-ms", type=int, default=0)
    ap.add_argument("--to-ms", type=int, default=1 << 62)
    ap.add_argument("--frames", action="store_true", help="frame-exact script (see above)")
    ap.add_argument("--from-level", help="start at the first play frame of this level (path contains the text)")
    args = ap.parse_args()
    records = []
    for raw in Path(args.log).read_bytes().decode("utf-8", "replace").splitlines():
        m = LINE.search(raw)
        if not m:
            continue
        t = int(m.group(1))
        if args.from_ms <= t <= args.to_ms:
            g = m.groups()
            records.append((t, int(g[1], 16), g[2] == "1", g[3] == "1", int(g[4]), int(g[5]), int(g[6]), int(g[7]), g[8], g[9].strip(),
                            int(g[10]) if g[10] else None))
    if not records:
        sys.exit("no [PADREC] lines (was the session run with LSWTCS_INPUTRECORD=1?)")
    if args.from_level:
        k = next((i for i, r in enumerate(records) if args.from_level.lower() in r[9].lower() and r[8] == "play"), None)
        if k is None:
            sys.exit(f"no play frames in a level containing '{args.from_level}'")
        records = records[k:]
    if args.frames and records[0][10] is None:
        sys.exit("--frames needs a recording with frame numbers (log113 or later)")
    body = convert_frames(records) if args.frames else convert(records)
    head = [f"# from {Path(args.log).name}, {records[0][0]}-{records[-1][0]} ms, {len(records)} pad changes"]
    if args.from_level and args.frames:
        head.append(f"waitlevel {args.from_level} 180")
        head.append("waitplay 0 180")
    lines = head + body
    text = "\n".join(lines) + "\n"
    if args.output:
        Path(args.output).write_bytes(text.encode())
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()

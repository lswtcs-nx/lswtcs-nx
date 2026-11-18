"""Finds the runtime's known TCS functions in another game's image by their code (for porting another game).

The functions the runtime overrides, hooks or waits on (tools/port_inventory.py) are mostly SDK libraries (D3D, XAudio,
XMedia, the C runtime, the network library) and TT engine code. Another game built with the same SDK or engine has
the same machine code at other addresses, with different branch targets and address constants. This makes a masked
signature of each function from TCS's image and searches the other image for it.

Mask, per PowerPC instruction (only what differs between builds is hidden):
  - b / bl (opcode 18): the target; bc (16): the target;
  - addi, addis, ori, oris and the D-form loads and stores (opcodes 14, 15, 24, 25, 32-55): the 16-bit immediate,
    which carries address halves and offsets into globals;
  - everything else (registers, opcodes, other fields) must match.
Signatures are the first --words instructions of each function (default 32, fewer for short functions; under 6 is
too short to trust, so those read on 12 words into what follows: "(spans)"). A match is reported as unique, ambiguous
(several places) or missing. The same SDK version matches nearly word for word; another version or a recompiled
engine function may not match at all.

Self-check on TCS (17 November 2026): of the 246 runtime-named addresses that are function starts, 212 are found
only where they are; 34 are ambiguous (tiny chunks such as padding-like stubs). Not yet tried on another game.

Inputs are flat images loaded at 0x82000000: TCS's image.bin (or the one tests/xex_image_check.py builds from
default.xex) and the other game's, made the same way. Function starts come from the recompiler's
ppc_func_mapping.cpp for TCS.

Usage: python tools/find_functions.py --a <tcs image.bin> --mapping <ppc_func_mapping.cpp> --b <other image.bin>
           [--words 32] [--csv out.csv]
       With --b the same as --a, it checks itself: every function should be found where it is.
"""
import argparse
import csv
import re
import sys
from pathlib import Path

try:
    import numpy as np
except ImportError:
    sys.exit("needs NumPy: pip install numpy")

BASE = 0x82000000
sys.path.insert(0, str(Path(__file__).resolve().parent))
import port_inventory  # noqa: E402


def masked_words(raw):
    w = np.frombuffer(raw[: len(raw) // 4 * 4], dtype=">u4").astype(np.uint32)
    op = w >> 26
    m = w.copy()
    m[op == 18] &= np.uint32(0xFC000003)
    m[op == 16] &= np.uint32(0xFFFF0003)
    imm = np.isin(op, [14, 15, 24, 25] + list(range(32, 56)))
    m[imm] &= np.uint32(0xFFFF0000)
    return m


def starts_from_mapping(path):
    pat = re.compile(r"\{\s*0x([0-9A-Fa-f]+),\s*sub_")
    starts = set()
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = pat.search(line)
            if m:
                a = int(m.group(1), 16)
                if a >= BASE:
                    starts.add(a)
    return sorted(starts)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--a", required=True, help="TCS image (flat, at 0x82000000)")
    ap.add_argument("--mapping", required=True, help="TCS's ppc_func_mapping.cpp")
    ap.add_argument("--b", required=True, help="the other game's image (flat, at 0x82000000)")
    ap.add_argument("--words", type=int, default=32)
    ap.add_argument("--csv")
    args = ap.parse_args()

    raw_a, raw_b = Path(args.a).read_bytes(), Path(args.b).read_bytes()
    ma, mb = masked_words(raw_a), masked_words(raw_b)
    starts = starts_from_mapping(args.mapping)
    nxt = {s: e for s, e in zip(starts, starts[1:] + [BASE + len(raw_a)])}
    uses = port_inventory.scan()
    wanted = [a for a in sorted(uses) if a in nxt]   # the inventory's addresses that are function starts
    print(f"{len(starts)} functions in the mapping, {len(wanted)} of them named by the runtime")

    rows, counts = [], {"unique": 0, "same place": 0, "ambiguous": 0, "missing": 0, "too short": 0}
    for a in wanted:
        n = min(args.words, (nxt[a] - a) // 4)
        reg = port_inventory.region(a)[0]
        spans = n < 6   # a tiny entry (or a chunk the recompiler split off): read on into what follows it
        if spans:
            n = 12
        if (a - BASE) // 4 + n > len(ma):
            counts["too short"] += 1
            rows.append((a, reg, n, "too short", ""))
            continue
        i0 = (a - BASE) // 4
        sig = ma[i0:i0 + n]
        cand = np.nonzero((mb[:-n] == sig[0]) & (mb[1:len(mb) - n + 1] == sig[1]))[0]
        hits = [int(c) for c in cand if np.array_equal(mb[c:c + n], sig)]
        found = [BASE + h * 4 for h in hits]
        if not found:
            status = "missing"
        elif len(found) == 1:
            status = "same place" if found[0] == a else "unique"
        else:
            status = "ambiguous"
        counts[status] += 1
        if spans:
            status += " (spans)"
        rows.append((a, reg, n, status, " ".join(f"{f:08X}" for f in found[:6]) + (" ..." if len(found) > 6 else "")))

    for a, reg, n, status, found in rows:
        if status not in ("same place",):
            print(f"  sub_{a:08X}  {reg:22} {n:3} words  {status:10} {found}")
    print("\n" + ", ".join(f"{k} {v}" for k, v in counts.items()))
    if args.csv:
        with open(args.csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["tcs_address", "region", "words", "status", "found"])
            for a, reg, n, status, found in rows:
                w.writerow([f"{a:08X}", reg, n, status, found])
    return 0


if __name__ == "__main__":
    sys.exit(main())

"""Lists every guest address the runtime names, by the part of the game it belongs to (for porting another game).

The runtime (source/LSWTCSRuntime, source/switch, the workspace tools) refers to TCS's own code and data by address:
patches, function overrides, hooks, waits it understands. Another game built from the same SDK and engine has the same
pieces at other addresses. This scans the sources for guest addresses (sub_8XXXXXXX, 0x82000000-0x83FFFFFF), puts
each one in a region of TCS's image, and prints, per region, how many there are and where they are used.

The regions are read from the code's own comments and from tests in the runtime (which addresses belong to D3D, XAudio,
XMedia, the C runtime, the network library); their edges are approximate. Data addresses (0x82E70000 and up: .data,
.bss) are game globals and must be found again by what reads them.

Usage: python tools/port_inventory.py [--csv out.csv] [--detail REGION]
"""
import argparse
import csv
import re
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCES = [ROOT / "source/LSWTCSRuntime/source", ROOT / "source/LSWTCSRuntime/include", ROOT / "source/switch/source",
           ROOT / "source/switch/include", ROOT / "workspace-tools", ROOT / "source/switch/tools"]
ADDR = re.compile(r"\b(?:sub_|0x)(8[23][0-9A-Fa-f]{6})\b")

# (start, end, region, how it ports). Ordered; first match wins.
REGIONS = [
    (0x82000000, 0x82130000, "headers/rodata", "XEX headers and read-only data: found from the XEX itself"),
    (0x8229E000, 0x822A4000, "xboxkrnl/xapi glue", "SDK startup and Xapi wrappers (thread attach, _xstart): SDK signature"),
    (0x822A4000, 0x822E0000, "D3D runtime", "Xbox D3D (ring buffer, fences, ISR, tiling, shaders): SDK signature"),
    (0x824F0000, 0x82520000, "engine I/O + CRT", "TT file system, streaming, job system, and the C runtime (memcpy, "
                                                  "sprintf, save/restore helpers): engine or CRT signature"),
    (0x82520000, 0x82540000, "engine heap/sync", "TT heap and sync wrappers: engine signature"),
    (0x82540000, 0x827C0000, "HLSL compiler (D3DX)", "the shader compiler the game carries: SDK signature, if it is used"),
    (0x827C0000, 0x82800000, "XAudio", "XAudio (voices, render thread, locks): SDK signature"),
    (0x82800000, 0x829A0000, "XMedia (movies)", "WMV player and decoder: SDK signature"),
    (0x829A0000, 0x82AF0000, "network/XAM libs", "QNet session library, voice, XAM wrappers: SDK signature"),
    (0x82AF0000, 0x82E70000, "other code", "late code (SDK and middleware): look it up case by case"),
    (0x82E70000, 0x84000000, "game data/bss", "globals: find what reads them in the new game"),
    (0x82130000, 0x8229E000, "TT engine/game (low)", "game and engine code: engine signature, or by behaviour"),
    (0x822E0000, 0x824F0000, "TT engine/game (mid)", "game and engine code: engine signature, or by behaviour"),
]


def region(a):
    for lo, hi, name, how in REGIONS:
        if lo <= a < hi:
            return name, how
    return "?", ""


def scan():
    uses = defaultdict(list)   # addr -> [(file, line)]
    for d in SOURCES:
        if not d.exists():
            continue
        for p in sorted(d.rglob("*")):
            if p.suffix not in (".cpp", ".cc", ".h", ".inc", ".py") or "build" in p.parts:
                continue
            for i, line in enumerate(p.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
                for m in ADDR.finditer(line):
                    uses[int(m.group(1), 16)].append((p.relative_to(ROOT).as_posix(), i))
    return uses


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--csv", help="write address, region, file:line list")
    ap.add_argument("--detail", help="list every address of one region (a name from the table, e.g. 'D3D runtime')")
    args = ap.parse_args()
    uses = scan()
    by_region = defaultdict(list)
    for a in sorted(uses):
        by_region[region(a)[0]].append(a)
    print(f"{len(uses)} distinct guest addresses in the runtime sources\n")
    print(f"{'region':24} {'addrs':>5}  files (uses)  / how it ports")
    for lo, hi, name, how in sorted(REGIONS):
        addrs = by_region.get(name, [])
        if not addrs:
            continue
        files = defaultdict(int)
        for a in addrs:
            for f, _ in uses[a]:
                files[Path(f).name] += 1
        top = ", ".join(f"{f} ({n})" for f, n in sorted(files.items(), key=lambda x: -x[1])[:4])
        print(f"{name:24} {len(addrs):5}  {top}\n{'':31}{how}")
    if args.detail:
        print(f"\n{args.detail}:")
        for a in by_region.get(args.detail, []):
            where = ", ".join(f"{f}:{n}" for f, n in uses[a][:3])
            print(f"  {a:08X}  {where}{' ...' if len(uses[a]) > 3 else ''}")
    if args.csv:
        with open(args.csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["address", "region", "uses"])
            for a in sorted(uses):
                w.writerow([f"{a:08X}", region(a)[0], " ".join(f"{f}:{n}" for f, n in uses[a])])
    return 0


if __name__ == "__main__":
    sys.exit(main())

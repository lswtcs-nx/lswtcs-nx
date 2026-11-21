#!/usr/bin/env python3
"""log135: widen guest pointer range checks to the log135 reservation range.

Hand-written patches in the generated game code and checks in the runtime accept guest pointers only in
0x80000000/0x82000000-0x94000000. log135 maps virtual reservations the 512 MB cannot hold at 0x60000000-0x70000000
(LSWTCSRuntime/include/lsw_guest_ptr.h). This rewrites the two check shapes, only when the same expression is on both
sides:

    X < 0x80000000u || X >= 0x94000000u   ->  !lsw_gptr80(X)      (also 0x82000000u -> lsw_gptr82)
    X >= 0x80000000u && X < 0x94000000u   ->  lsw_gptr80(X)

and adds #include "lsw_guest_ptr.h" to each file it changes. Checks with only the upper bound already accept the range.
Idempotent. Usage: widen_guest_ptr_checks.py FILE_OR_DIR... [--check]  (--check: report, change nothing; exit 1 if any
check is left to rewrite).
"""
import pathlib
import re
import sys

X = r"([A-Za-z_][\w]*(?:\.[A-Za-z_]\w*)*)"
REJECT = re.compile(X + r"\s*<\s*0x8([02])000000u?\s*\|\|\s*\1\s*>=\s*0x94000000u?")
ACCEPT = re.compile(X + r"\s*>=\s*0x8([02])000000u?\s*&&\s*\1\s*<\s*0x94000000u?")
INCLUDE = '#include "lsw_guest_ptr.h"   // log135 (workspace-tools/widen_guest_ptr_checks.py)\n'


# Kernel-object checks up to 0xC0000000/0xE0000000 (spin locks, waits, handles): wrapped so the range passes too.
REJECT_HI = re.compile(X + r"\s*<\s*0x80000000u\s*\|\|\s*\1\s*>=\s*(0x[CE]0000000u)")
ACCEPT_HI = re.compile(X + r"\s*>=\s*0x80000000u\s*&&\s*\1\s*<\s*(0x[CE]0000000u)")


def rewrite(text):
    text, n1 = REJECT.subn(lambda m: f"!lsw_gptr8{m.group(2)}({m.group(1)})", text)
    text, n2 = ACCEPT.subn(lambda m: f"lsw_gptr8{m.group(2)}({m.group(1)})", text)
    text, n3 = REJECT_HI.subn(lambda m: f"(!lsw_vext_ptr({m.group(1)}) && ({m.group(1)} < 0x80000000u || {m.group(1)} >= {m.group(2)}))", text)
    text, n4 = ACCEPT_HI.subn(lambda m: f"(lsw_vext_ptr({m.group(1)}) || ({m.group(1)} >= 0x80000000u && {m.group(1)} < {m.group(2)}))", text)
    return text, n1 + n2 + n3 + n4


def main(argv):
    check = "--check" in argv
    paths = [pathlib.Path(a) for a in argv if a != "--check"]
    files = []
    for p in paths:
        files += sorted(p.glob("ppc_recomp.*.cpp")) if p.is_dir() else [p]
    total = 0
    for f in files:
        raw = f.read_bytes()
        text = raw.decode("utf-8", errors="surrogateescape")
        new, n = rewrite(text)
        if not n:
            continue
        total += n
        print(f"{f}: {n} checks")
        if check:
            continue
        if "lsw_guest_ptr.h" not in new:
            lines = new.split("\n")
            # after the first #include: later ones may sit inside #ifdef blocks
            at = next((i for i, l in enumerate(lines[:60]) if l.startswith("#include")), -1) + 1
            nl = "\r\n" if "\r\n" in new else "\n"
            lines.insert(at, INCLUDE.rstrip("\n") + ("\r" if nl == "\r\n" else ""))
            new = "\n".join(lines)
        f.write_bytes(new.encode("utf-8", errors="surrogateescape"))
    print(f"{total} checks {'to rewrite' if check else 'rewritten'}")
    return 1 if check and total else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

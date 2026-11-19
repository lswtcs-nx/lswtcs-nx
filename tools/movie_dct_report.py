"""Reads [MOVIEDCT] lines (LSWTCS_MOVIEDCT=1, kernel_stubs.cpp) and prints each probe's 8x8 output as signed 16-bit values,
with a verdict per v69/v72 candidate: a DC-only block must come out flat, row 0 col 1 must vary along x only, row 1 col 0
along y only. Usage: python tools/movie_dct_report.py <run folder or stdout.log> [--matrices]"""
import re, sys, struct
from pathlib import Path

def rows(hexblocks):
    out = []
    for blk in hexblocks.split()[:8]:
        out.append(list(struct.unpack('>8h', bytes.fromhex(blk))))
    return out

def shape(m):
    flat = all(v == m[0][0] for r in m for v in r)
    xonly = all(r == m[0] for r in m) and not flat
    yonly = all(len(set(r)) == 1 for r in m) and not flat
    return 'flat' if flat else 'x only' if xonly else 'y only' if yonly else 'mixed'

def main():
    p = Path(sys.argv[1]); log = p / 'stdout.log' if p.is_dir() else p
    pat = re.compile(r'^\[MOVIEDCT\] (\S+) call \d+: perm (-?\d+) pattern (\S+) .*? out ((?:[0-9A-F]{32} ?){8})')
    want = {'dc': 'flat', 'r0c1': 'x only', 'r1c0': 'y only', 'r0c2': 'x only', 'r2c0': 'y only'}
    res = {}
    for line in log.read_text(errors='replace').splitlines():
        m = pat.match(line)
        if not m: continue
        fn, cand, what, hx = m.group(1), int(m.group(2)), m.group(3), m.group(4)
        mat = rows(hx)
        res.setdefault((fn, cand), []).append((what, shape(mat), mat))
    for (fn, cand), items in sorted(res.items()):
        ok = all(s == want[w] for w, s, _ in items)
        print(f'{fn} perm {cand:2d}: ' + ', '.join(f'{w}={s}' for w, s, _ in items) + ('   <- all as expected' if ok else ''))
        if '--matrices' in sys.argv:
            for w, s, mat in items:
                print(f'   {w}:'); [print('     ', ' '.join(f'{v:6d}' for v in r)) for r in mat]

if __name__ == '__main__':
    main()

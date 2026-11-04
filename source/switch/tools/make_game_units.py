"""Compiler units for the recompiled game, grouped so hot calls can be inlined (CMake option LSW_GAME_INLINE).

Every guest call is a call through a weak alias (`PPC_WEAK_FUNC`, weak and noinline), and GameCode builds with -fPIC, so GCC
never inlines one guest function into another. lsw_guest_linkage.h keeps weak linkage only for the functions the runtime
replaces, and CMake adds -fno-semantic-interposition; then a call can be inlined when the callee is in the same unit. This tool
writes the two inputs:

- game_units.txt: generated files per unit, at most CAP files each. Files are merged greedily along the call sites of the
  sampled hot functions (hot_order.ld, hottest first, weighted by rank), heaviest edges first. The other files fill units in
  numeric order. Hot call sites in the same unit: about 50 % at 20 files, against 19 % for the old alphabetical batches of 5.
- include/lsw_guest_overrides.h: the guest functions the runtime defines strongly, read from a linked ELF (a C++ symbol
  `sub_X` of type T instead of the generated weak alias). A new override missing here fails the link with a multiple
  definition; run this tool again then.

Writes only file numbers and function names. Usage:
  python make_game_units.py <generated output dir> <linked ELF> <nm> [--cap 20]"""
import argparse, collections, glob, os, re, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
SWITCH = os.path.dirname(HERE)
ap = argparse.ArgumentParser()
ap.add_argument('output'); ap.add_argument('elf'); ap.add_argument('nm')
ap.add_argument('--cap', type=int, default=20)
a = ap.parse_args()

nm = subprocess.run([a.nm, a.elf], capture_output=True, text=True, check=True).stdout
over = sorted({m.group(1) for m in re.finditer(r'^[0-9a-f]+ T _Z12(sub_[0-9A-F]{8})R10PPCContextPh$', nm, re.M)})
if not over:
    raise SystemExit('no overridden guest functions found: wrong ELF?')

defre = re.compile(r'^PPC_FUNC_IMPL\(__imp__(sub_[0-9A-F]{8})\) \{', re.M)
callre = re.compile(r'\b(sub_[0-9A-F]{8})\(ctx, base\)')
fdef, calls = {}, {}
for p in glob.glob(os.path.join(a.output, 'ppc_recomp.*.cpp')):
    n = int(os.path.basename(p).split('.')[1])
    t = open(p, encoding='latin1').read()
    ms = list(defre.finditer(t))
    for i, m in enumerate(ms):
        fdef[m.group(1)] = n
        calls[m.group(1)] = callre.findall(t[m.end(): ms[i + 1].start() if i + 1 < len(ms) else len(t)])
hot = [l.split('__imp__')[1].split(')')[0] for l in open(os.path.join(SWITCH, 'hot_order.ld')) if '__imp__' in l]
overs = set(over)
edges = collections.Counter()
for r, f in enumerate(hot):
    for c in calls.get(f, []):
        if c in overs or c not in fdef or fdef[c] == fdef.get(f, -1) or f not in fdef:
            continue
        x, y = fdef[f], fdef[c]
        edges[(min(x, y), max(x, y))] += 1.0 / (1 + r / 200)
nums = sorted(set(fdef.values()))
parent = {n: n for n in nums}
members = {n: [n] for n in nums}
def find(x):
    while parent[x] != x:
        parent[x] = parent[parent[x]]
        x = parent[x]
    return x
for (x, y), w in edges.most_common():
    rx, ry = find(x), find(y)
    if rx != ry and len(members[rx]) + len(members[ry]) <= a.cap:
        parent[ry] = rx
        members[rx] += members.pop(ry)
units = sorted(sorted(m) for m in members.values() if len(m) > 1)
singles = sorted(m[0] for m in members.values() if len(m) == 1)
units += [singles[i:i + a.cap] for i in range(0, len(singles), a.cap)]
unit = {n: i for i, u in enumerate(units) for n in u}
tot = same = 0
for f in hot:
    for c in calls.get(f, []):
        if c in fdef and c not in overs and f in fdef:
            tot += 1
            same += unit[fdef[c]] == unit[fdef[f]]

with open(os.path.join(SWITCH, 'game_units.txt'), 'w', newline='\n') as o:
    o.write('# tools/make_game_units.py: ppc_recomp.<n>.cpp numbers per GameCode unit (LSW_GAME_INLINE). %d units of at most %d files;\n'
            '# %d of %d hot call sites (hot_order.ld) to non-overridden callees fall in the caller\'s unit.\n'
            % (len(units), a.cap, same, tot))
    for u in units:
        o.write(' '.join(map(str, u)) + '\n')
with open(os.path.join(SWITCH, 'include', 'lsw_guest_overrides.h'), 'w', newline='\n') as o:
    o.write('// tools/make_game_units.py, from %s: guest functions the runtime defines strongly. They keep weak, noinline\n'
            '// linkage (lsw_guest_linkage.h). A new override missing here fails the link with a multiple definition.\n'
            '#pragma once\n' % os.path.basename(a.elf))
    for f in over:
        o.write('#define LSW_OVR_%s ~, 1\n' % f)
print('units', len(units), 'overrides', len(over), 'hot call sites in the caller\'s unit', same, 'of', tot)

"""Makes a copy of the generated guest code whose plain loads and stores are not volatile (log77 A/B).

Usage: python make_nonvolatile_output.py <output dir> <new dir>
The generated output itself is never changed. The copy differs in three ways:

1. ppc_context.h: the scalar PPC_LOAD_* / PPC_STORE_* macros use LSW_GV instead of volatile. LSW_GV is empty only when
   LSW_GUEST_NONVOLATILE is defined (CMake option LSW_GUEST_NONVOLATILE, GameCode target only), so the runtime, which
   includes the same header and polls guest memory in host loops, keeps volatile. The MMIO store macros
   (PPC_MM_STORE_*, 17 sites) keep volatile in every case.
2. A compiler barrier (LSW_GUEST_BARRIER, an empty asm with a memory clobber) after every sync, lwsync, eieio and isync.
   The generator emits these as comments only; volatile alone kept the order of stores around them.
3. The same barrier right after every label that a later goto in the same function jumps to: every loop's header.
   Without it GCC may keep a value read from guest memory in a register for the whole loop, and a loop that waits for
   another thread or the GPU to change memory never ends (mk9-nx notes, masseffect-nx: required). A loop with a call in
   it reloads anyway; the barrier costs nothing in code that has no memory access to move.

Technique from mk9-nx / masseffect-nx (GPL-3.0, technique only; no code). Their measurement: +15-20 % fps.
"""
import os, re, shutil, sys

src, dst = sys.argv[1], sys.argv[2]
if os.path.exists(dst):
    shutil.rmtree(dst)
os.makedirs(dst)

CTX_HEAD = '''// ---- log77 (switch/tools/make_nonvolatile_output.py) ----
#ifdef LSW_GUEST_NONVOLATILE
#define LSW_GV
#else
#define LSW_GV volatile
#endif
#define LSW_GUEST_BARRIER() __asm__ __volatile__("" ::: "memory")
'''
MM = '''// log77: MMIO stores stay volatile whatever LSW_GV is.
#ifndef PPC_MM_STORE_U8
#define PPC_MM_STORE_U8(x, y)   *(volatile uint8_t*)PPC_HOST(x) = (y)
#endif
#ifndef PPC_MM_STORE_U16
#define PPC_MM_STORE_U16(x, y)  *(volatile uint16_t*)PPC_HOST(x) = __builtin_bswap16(y)
#endif
#ifndef PPC_MM_STORE_U32
#define PPC_MM_STORE_U32(x, y)  *(volatile uint32_t*)PPC_HOST(x) = __builtin_bswap32(y)
#endif
#ifndef PPC_MM_STORE_U64
#define PPC_MM_STORE_U64(x, y)  *(volatile uint64_t*)PPC_HOST(x) = __builtin_bswap64(y)
#endif
'''

def fix_context(text):
    anchor = '#ifndef PPC_LOAD_U8\n'
    assert text.count(anchor) == 1
    text = text.replace(anchor, CTX_HEAD + MM + anchor)
    for kind in ('LOAD', 'STORE'):
        for bits, t in ((8, 'uint8_t'), (16, 'uint16_t'), (32, 'uint32_t'), (64, 'uint64_t')):
            m = re.search(r'#define PPC_%s_U%d\(.*\n' % (kind, bits), text)
            assert m, (kind, bits)
            line = m.group(0)
            assert line.count('(volatile %s*)' % t) == 1, line
            text = text.replace(line, line.replace('(volatile %s*)' % t, '(LSW_GV %s*)' % t))
    assert text.count('(volatile ') == 4   # only the four MMIO stores
    return text

LABEL = re.compile(r'^(loc_[0-9A-F]+):\s*$')
GOTO = re.compile(r'goto (loc_[0-9A-F]+);')
FENCE = re.compile(r'^\s*// (sync|lwsync|eieio|isync)\b')
FUNC = re.compile(r'^PPC_FUNC_IMPL\(')

def fix_unit(text):
    lines = text.split('\n')
    # function ranges
    starts = [i for i, l in enumerate(lines) if FUNC.match(l)] + [len(lines)]
    loop_heads = set()   # line numbers of labels some later goto in the same function targets
    for a, b in zip(starts, starts[1:]):
        where = {}
        for i in range(a, b):
            m = LABEL.match(lines[i])
            if m:
                where[m.group(1)] = i
        for i in range(a, b):
            for g in GOTO.findall(lines[i]):
                j = where.get(g)
                if j is not None and j <= i:
                    loop_heads.add(j)
    out, fences = [], 0
    for i, l in enumerate(lines):
        out.append(l)
        if i in loop_heads:
            out.append('\tLSW_GUEST_BARRIER();')
        elif FENCE.match(l):
            out.append('\tLSW_GUEST_BARRIER();')
            fences += 1
    return '\n'.join(out), len(loop_heads), fences

total_loops = total_fences = units = 0
for name in sorted(os.listdir(src)):
    p, q = os.path.join(src, name), os.path.join(dst, name)
    if os.path.isdir(p):
        shutil.copytree(p, q)
        continue
    if name == 'ppc_context.h' or (name.startswith('ppc_recomp.') and name.endswith('.cpp')):
        raw = open(p, newline='', encoding='latin-1').read()
        crlf = '\r\n' in raw
        text = raw.replace('\r\n', '\n')
        if name == 'ppc_context.h':
            new = fix_context(text)
        else:
            new, loops, fences = fix_unit(text)
            total_loops += loops; total_fences += fences; units += 1
        open(q, 'w', newline='', encoding='latin-1').write(new.replace('\n', '\r\n') if crlf else new)
    else:
        shutil.copy2(p, q)
print(f'{units} units: barriers at {total_loops} loop headers and {total_fences} fences -> {dst}')

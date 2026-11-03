"""LSWTCS: keep Mesa's NIR shader path on the Switch build of switch-mesa 20.1.0-rc3, so NV50_PROG_USE_NIR=1 (mesa_nir_edits.py)
really selects nouveau's NIR front end. devkitPro's patch drops that path with `#ifndef __SWITCH__` around every NIR use in the
state tracker and in nvc0; with them in place the variable changed only nouveau_screen.c and shaders stayed on TGSI (checked on the
log60 ELF: no NIR code linked at all). Here each such guard whose block is about NIR becomes `#if 1`, which keeps the non-Switch
branch and drops any `#else`. Guards about the draw module, glthread, SPIR-V and the rest stay. Default stays TGSI.
NOT APPLIED (tools/build_mesa.sh): run 60's NIR session drew wrong shadows and darker light, ran 21 % slower (load-matched, 90 %
-26 to -16) and crashed in nv50_ir's SSA pass compiling a vertex shader. Using it again also needs libnir linked: libEGL.a does not
merge it, meson builds it as a thin archive, and its SPIR-V objects are named ".._spirv_*" (see git history of CMakeLists.txt).
Usage: python mesa_nir_full_edits.py <mesa-src-dir>"""
import os, re, sys
root = sys.argv[1]
MARK = '#if 1 /* LSWTCS mesa_nir_full_edits.py: NIR path kept on the Switch */'
FILES = {   # file: number of NIR guards expected
    'src/mesa/state_tracker/st_program.c': 10,
    'src/mesa/state_tracker/st_context.c': 2,
    'src/mesa/state_tracker/st_shader_cache.c': 4,
    'src/mesa/state_tracker/st_glsl_to_ir.cpp': 1,
    'src/gallium/drivers/nouveau/nvc0/nvc0_screen.c': 3,
    'src/gallium/drivers/nouveau/nvc0/nvc0_program.c': 2,
    'src/gallium/drivers/nouveau/nvc0/nvc0_state.c': 3,
    'src/gallium/drivers/nouveau/codegen/nv50_ir.cpp': 1,
}
NIRISH = re.compile(r'(\bnir\b|_nir\b|\bnir_|_nir_|\bNIR\b|_NIR\b|PREFERRED_IR)')

def block_end(lines, i):
    depth = 0
    for j in range(i, len(lines)):
        s = lines[j].strip()
        if s.startswith('#if'):
            depth += 1
        elif s.startswith('#endif'):
            depth -= 1
            if depth == 0:
                return j
    raise SystemExit('unbalanced #if at line %d' % (i + 1))

for rel, expected in FILES.items():
    p = os.path.join(root, rel)
    text = open(p, newline='').read()
    nl = '\r\n' if '\r\n' in text else '\n'
    lines = text.split(nl)
    done = sum(1 for l in lines if l.strip() == MARK)
    if done:
        assert done == expected, (rel, done, expected)
        continue
    hits = []
    for i, l in enumerate(lines):
        if l.strip() != '#ifndef __SWITCH__':
            continue
        body = nl.join(lines[i:block_end(lines, i) + 1])
        if NIRISH.search(body) and 'spirv' not in body.lower():
            hits.append(i)
    assert len(hits) == expected, (rel, len(hits), expected, [h + 1 for h in hits])
    for i in hits:
        lines[i] = MARK
    text = nl.join(lines)
    if rel.endswith('st_program.c'):
        # The draw module is not built on the Switch, and a draw shader never comes up there (feedback and raster-pos are off).
        old = '(!key->is_draw_shader || draw_has_llvm())'
        if old in text:
            text = text.replace(old, '!key->is_draw_shader')
    open(p, 'w', newline='').write(text)
print('nir full edits applied')

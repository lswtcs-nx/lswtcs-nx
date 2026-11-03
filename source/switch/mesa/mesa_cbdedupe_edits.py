"""LSWTCS: nouveau skips a constant buffer bind that changes nothing, when lsw_nvc0_cb_dedupe is set (state B of the A/B cycle).

Any glBindBufferRange on a uniform buffer makes the state tracker set every uniform block of every stage again
(st_bind_ubos), and nvc0_set_constant_buffer marks each one dirty without comparing. So one changed block rebinds them all
on the GPU. With the option, a block with the same buffer, offset and size returns early. It stays bound: its bufctx entry
and cb_bindings bit are kept, and paths that need a rebind (buffer storage replaced, blits, context switch) set
constbuf_dirty themselves.

Counters for [NVC0CB]: blocks set, of those skipped, constant cache barriers, serializes on bind.
Applied after mesa_cbflush_edits.py. Usage: python mesa_cbdedupe_edits.py <mesa-src-dir>"""
import sys, os
root = sys.argv[1]

def edit(rel, pairs):
    p = os.path.join(root, rel)
    s = open(p, newline='').read()
    for old, new in pairs:
        if s.count(new) == 1:
            continue   # already applied
        assert s.count(old) == 1, (rel, old[:70])
        s = s.replace(old, new)
    open(p, 'w', newline='').write(s)

N = 'src/gallium/drivers/nouveau/nvc0/'

edit(N + 'nvc0_state.c', [
    ('static void\nnvc0_set_constant_buffer(struct pipe_context *pipe,',
     '/* LSWTCS (mesa_cbdedupe_edits.py) */\n'
     'volatile int lsw_nvc0_cb_dedupe = 0;\n'
     'unsigned long long lsw_nvc0_cb_sets = 0, lsw_nvc0_cb_same = 0, lsw_nvc0_cb_barriers = 0, lsw_nvc0_cb_serializes = 0;\n\n'
     'static void\nnvc0_set_constant_buffer(struct pipe_context *pipe,'),
    ('   const unsigned i = index;\n\n   if (unlikely(shader == PIPE_SHADER_COMPUTE)) {\n      if (nvc0->constbuf[s][i].user)\n',
     '   const unsigned i = index;\n\n'
     '   if (shader != PIPE_SHADER_COMPUTE) {   /* LSWTCS: mesa_cbdedupe_edits.py */\n'
     '      ++lsw_nvc0_cb_sets;\n'
     '      if (lsw_nvc0_cb_dedupe && res && !cb->user_buffer && !nvc0->constbuf[s][i].user &&\n'
     '          nvc0->constbuf[s][i].u.buf == res && (nvc0->constbuf_valid[s] & (1 << i)) &&\n'
     '          nvc0->constbuf[s][i].offset == cb->buffer_offset &&\n'
     '          nvc0->constbuf[s][i].size == MIN2(align(cb->buffer_size, 0x100), 0x10000)) {\n'
     '         ++lsw_nvc0_cb_same;\n'
     '         return;\n'
     '      }\n'
     '   }\n\n'
     '   if (unlikely(shader == PIPE_SHADER_COMPUTE)) {\n      if (nvc0->constbuf[s][i].user)\n'),
])

edit(N + 'nvc0_vbo.c', [
    ('      IMMED_NVC0(push, NVC0_3D(MEM_BARRIER), 0x1011);\n      nvc0->cb_dirty = false;\n',
     '      IMMED_NVC0(push, NVC0_3D(MEM_BARRIER), 0x1011);\n      nvc0->cb_dirty = false;\n'
     '      { extern unsigned long long lsw_nvc0_cb_barriers; ++lsw_nvc0_cb_barriers; }   /* LSWTCS: mesa_cbdedupe_edits.py */\n'),
])

edit(N + 'nvc0_screen.c', [
    ('         IMMED_NVC0(push, NVC0_3D(SERIALIZE), 0);\n         if (can_serialize)\n',
     '         IMMED_NVC0(push, NVC0_3D(SERIALIZE), 0);\n'
     '         { extern unsigned long long lsw_nvc0_cb_serializes; ++lsw_nvc0_cb_serializes; }   /* LSWTCS: mesa_cbdedupe_edits.py */\n'
     '         if (can_serialize)\n'),
])
print('cbdedupe edits applied')

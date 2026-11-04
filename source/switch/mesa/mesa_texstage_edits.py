"""LSWTCS: NVC0_TEX_STAGE_MM=1 makes nvc0's texture uploads take their staging memory from the screen's GART
suballocator (nouveau_mm, as buffer uploads already do) instead of a new buffer object per call.

nvc0_miptree_transfer_map made one nouveau_bo_new per glTexSubImage call, i.e. per mip level: on the Switch that is a
memalign, a memset of the whole size, nvMapCreate and nvAddressSpaceMap, and after the copy's fence nvAddressSpaceUnmap,
nvMapClose and free. Run 76 ([TEXSLOW], console 2026-11-04): about 8 ms per new texture whatever its size (a 5 KB
64x64 with 7 mips 19 ms, a 2 MB 2048x2048 with one mip 6 ms), 150-200 textures at each area entry and cutscene.

With the option the staging range comes from screen->mm_GART (slabs of up to 4 MB, chunks up to 2 MB; larger requests
still get their own buffer object inside nouveau_mm_allocate), and the write path frees it with nouveau_mm_free_work on
the current fence, as nouveau_buffer.c does. Slabs stay allocated once made (the most staging alive at once); the port
flushes during its texture pre-pass to keep that small. Mesa's default stays off; the port sets it in egl_init
(gl_backend.cpp, LSWTCS_GLTEXSTAGE=0 to opt out).
Applied after the other edits. Usage: python mesa_texstage_edits.py <mesa-src-dir>"""
import sys, os
root = sys.argv[1]

def edit(rel, pairs):
    p = os.path.join(root, rel)
    s = open(p, newline='').read()
    if 'mesa_texstage_edits.py' in s:   # already applied (some new texts contain their old text)
        return
    for old, new in pairs:
        if s.count(new) == 1 and s.count(old) == 0:
            continue   # already applied
        assert s.count(old) == 1, (rel, old[:70])
        s = s.replace(old, new)
    open(p, 'w', newline='').write(s)

N = 'src/gallium/drivers/nouveau/'
TAG = ' /* LSWTCS: mesa_texstage_edits.py */'

edit(N + 'nvc0/nvc0_transfer.c', [
    # the allocation token and the staging range's start
    ('struct nvc0_transfer {\n   struct pipe_transfer base;\n   struct nv50_m2mf_rect rect[2];\n',
     'struct nvc0_transfer {\n   struct pipe_transfer base;\n   struct nv50_m2mf_rect rect[2];\n'
     '   struct nouveau_mm_allocation *mm;' + TAG + '\n'
     '   uint32_t stage_base;\n'),
    # the option, read once
    ('void *\nnvc0_miptree_transfer_map(struct pipe_context *pctx,\n',
     'static bool\nnvc0_tex_stage_mm(void)' + TAG + '\n{\n'
     '   static int on = -1;\n'
     '   if (on < 0)\n'
     '      on = debug_get_bool_option("NVC0_TEX_STAGE_MM", false);\n'
     '   return on;\n}\n\n'
     'void *\nnvc0_miptree_transfer_map(struct pipe_context *pctx,\n'),
    # staging from the suballocator
    ('   ret = nouveau_bo_new(dev, NOUVEAU_BO_GART | NOUVEAU_BO_MAP, 0,\n'
     '                        size * tx->nlayers, NULL, &tx->rect[1].bo);\n'
     '   if (ret) {\n',
     '   ret = 0;\n'
     '   if (nvc0_tex_stage_mm())' + TAG + '\n'
     '      tx->mm = nouveau_mm_allocate(nvc0->screen->base.mm_GART, size * tx->nlayers,\n'
     '                                   &tx->rect[1].bo, &tx->rect[1].base);\n'
     '   else\n'
     '      ret = nouveau_bo_new(dev, NOUVEAU_BO_GART | NOUVEAU_BO_MAP, 0,\n'
     '                           size * tx->nlayers, NULL, &tx->rect[1].bo);\n'
     '   tx->stage_base = tx->rect[1].base;\n'
     '   if (ret || !tx->rect[1].bo) {\n'
     '      if (tx->mm)\n'
     '         nouveau_mm_free(tx->mm);\n'),
    # the read path rewinds to the range's start, not 0
    ('      tx->rect[0].z = z;\n      tx->rect[0].base = base;\n      tx->rect[1].base = 0;\n',
     '      tx->rect[0].z = z;\n      tx->rect[0].base = base;\n      tx->rect[1].base = tx->stage_base;' + TAG + '\n'),
    # pointers into the range
    ('   if (tx->rect[1].bo->map) {\n      *ptransfer = &tx->base;\n      return tx->rect[1].bo->map;\n   }\n',
     '   if (tx->rect[1].bo->map) {\n      *ptransfer = &tx->base;\n'
     '      return (uint8_t *)tx->rect[1].bo->map + tx->stage_base;' + TAG + '\n   }\n'),
    ('   ret = nouveau_bo_map(tx->rect[1].bo, flags, nvc0->screen->base.client);\n'
     '   if (ret) {\n'
     '      pipe_resource_reference(&tx->base.resource, NULL);\n'
     '      nouveau_bo_ref(NULL, &tx->rect[1].bo);\n'
     '      FREE(tx);\n'
     '      return NULL;\n'
     '   }\n\n'
     '   *ptransfer = &tx->base;\n'
     '   return tx->rect[1].bo->map;\n',
     '   ret = nouveau_bo_map(tx->rect[1].bo, flags, nvc0->screen->base.client);\n'
     '   if (ret) {\n'
     '      pipe_resource_reference(&tx->base.resource, NULL);\n'
     '      nouveau_bo_ref(NULL, &tx->rect[1].bo);\n'
     '      if (tx->mm)' + TAG + '\n'
     '         nouveau_mm_free(tx->mm);\n'
     '      FREE(tx);\n'
     '      return NULL;\n'
     '   }\n\n'
     '   *ptransfer = &tx->base;\n'
     '   return (uint8_t *)tx->rect[1].bo->map + tx->stage_base;\n'),
    # release: the range after the copies' fence, as nouveau_buffer.c release_allocation
    ('      /* Allow the copies above to finish executing before freeing the source */\n'
     '      nouveau_fence_work(nvc0->screen->base.fence.current,\n'
     '                         nouveau_fence_unref_bo, tx->rect[1].bo);\n'
     '   } else {\n'
     '      nouveau_bo_ref(NULL, &tx->rect[1].bo);\n'
     '   }\n',
     '      /* Allow the copies above to finish executing before freeing the source */\n'
     '      if (tx->mm) {' + TAG + '\n'
     '         nouveau_fence_work(nvc0->screen->base.fence.current, nouveau_mm_free_work, tx->mm);\n'
     '         nouveau_bo_ref(NULL, &tx->rect[1].bo);\n'
     '      } else\n'
     '      nouveau_fence_work(nvc0->screen->base.fence.current,\n'
     '                         nouveau_fence_unref_bo, tx->rect[1].bo);\n'
     '   } else {\n'
     '      nouveau_bo_ref(NULL, &tx->rect[1].bo);\n'
     '      if (tx->mm)\n'
     '         nouveau_mm_free(tx->mm);\n'
     '   }\n'),
])
print('texstage edits applied')

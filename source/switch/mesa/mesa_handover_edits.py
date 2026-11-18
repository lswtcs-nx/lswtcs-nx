"""LSWTCS (log123): a sampling height per texture object, for the resolve hand-over (gl_rt.inc, LSWTCS_GLHANDOVER).

A colour resolve that copies a whole render target and then clears it can hand the target's storage to the destination instead
of copying it. Render targets are 2048 rows tall (kRtH) and the destinations 704, so the destination samples the storage through a
GL texture view whose sampler sees only the top rows: lsw_gl_texture_sample_height(view, rows) stores the row count in the view's
texture object, the state tracker passes it in the sampler-view template, and the Maxwell/Kepler TIC uses it as the height. In
block-linear memory the address of a row does not depend on the surface's height (one layer, one level), so the top rows of the
2048-row storage are exactly a 704-row texture. Normalised coordinates and clamping then behave as for the copy, with no change
in any shader. Templates are not cleared by the state tracker, so the height counts only with its magic word. Applied after
mesa_nvc0disk_edits.py. Usage: python mesa_handover_edits.py <mesa-src-dir>"""
import sys, os
root = sys.argv[1]

def edit(rel, pairs):
    p = os.path.join(root, rel)
    s = open(p, newline='', encoding='utf-8').read()
    for old, new in pairs:
        if s.count(new) == 1:
            continue   # already applied
        assert s.count(old) == 1, (rel, old[:70])
        s = s.replace(old, new)
    open(p, 'w', newline='', encoding='utf-8').write(s)

MAGIC = '0x4C534856u'

edit('src/gallium/include/pipe/p_state.h', [
    ('''         unsigned size;     /**< size of the readable sub-range in bytes */
      } buf;
   } u;
};
''', '''         unsigned size;     /**< size of the readable sub-range in bytes */
      } buf;
   } u;
   unsigned lsw_magic;    /**< LSWTCS: lsw_height counts only when this is LSW_SAMPLE_HEIGHT_MAGIC */
   unsigned lsw_height;   /**< LSWTCS: rows the sampler sees (mesa_handover_edits.py) */
};
#define LSW_SAMPLE_HEIGHT_MAGIC ''' + MAGIC + '''
'''),
])

edit('src/mesa/main/mtypes.h', [
    ('''   GLboolean Immutable;        /**< GL_ARB_texture_storage */
   GLboolean _IsFloat;         /**< GL_OES_float_texture */
''', '''   GLboolean Immutable;        /**< GL_ARB_texture_storage */
   GLuint LswSampleHeight;     /**< LSWTCS: rows the sampler sees, 0 = all (mesa_handover_edits.py) */
   GLboolean _IsFloat;         /**< GL_OES_float_texture */
'''),
])

edit('src/mesa/main/texobj.c', [
    ('''   return;
}

/*@}*/''', '''   return;
}

/* LSWTCS (mesa_handover_edits.py): rows the sampler of texture `id` sees. Called on the GL thread (lsw_glthread_call) right
 * after the texture view is made, before it is sampled, so no sampler view exists yet. */
void lsw_gl_texture_sample_height(GLuint id, GLuint rows);
void
lsw_gl_texture_sample_height(GLuint id, GLuint rows)
{
   GET_CURRENT_CONTEXT(ctx);
   if (!ctx)
      return;
   struct gl_texture_object *obj = _mesa_lookup_texture(ctx, id);
   if (obj)
      obj->LswSampleHeight = rows;
}

/*@}*/'''),
])

edit('src/mesa/state_tracker/st_sampler_view.c', [
    ('''   templ.swizzle_a = GET_SWZ(swizzle, 3);

   return st->pipe->create_sampler_view(st->pipe, stObj->pt, &templ);
''', '''   templ.swizzle_a = GET_SWZ(swizzle, 3);
   /* LSWTCS (mesa_handover_edits.py) */
   templ.lsw_magic = stObj->base.LswSampleHeight ? LSW_SAMPLE_HEIGHT_MAGIC : 0;
   templ.lsw_height = stObj->base.LswSampleHeight;

   return st->pipe->create_sampler_view(st->pipe, stObj->pt, &templ);
'''),
])

edit('src/gallium/drivers/nouveau/nvc0/nvc0_tex.c', [
    ('''   } else {
      width = mt->base.base.width0;
      height = mt->base.base.height0;
   }

   tic[4] |= width - 1;
''', '''   } else {
      width = mt->base.base.width0;
      height = mt->base.base.height0;
      /* LSWTCS (mesa_handover_edits.py): a view that samples only the top rows */
      if (view->pipe.lsw_magic == LSW_SAMPLE_HEIGHT_MAGIC && view->pipe.lsw_height && view->pipe.lsw_height < height)
         height = view->pipe.lsw_height;
   }

   tic[4] |= width - 1;
'''),
])
print('handover edits applied')

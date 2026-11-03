"""LSWTCS: ZCULL, the GPU's coarse depth culling, in nouveau when lsw_nvc0_zcull is set (state c of the A/B cycle).

This nouveau leaves ZCULL off: screen init writes ZCULL_REGION 0x3f and nvc0_validate_zcull is under #if 0. On the Switch the
rest is already in place: libdrm_nouveau 1.0.1 binds a ZCULL context on Mesa's channel (pushbuf.c, nvGpuChannelZcullBind), and
nvc0 writes the per-shader ZCULL_TEST_MASK (0x11, off, for shaders that write depth). What is missing is the 3D engine's setup.
It follows deko3d (ZLIB; technique and register values, our own code): gpu_3d_base.cpp (init, depth target, clear hint) and
gpu_3d_zcull.cpp (sizes from nvGpuGetZcullInfo()).

The ZCULL data belongs to one depth target. It is invalidated when that target changes (address, size or layer), after draws
whose shader wrote depth (ZCULL does not follow those), and after depth is written outside draws: blits, copies and clears of a
surface other than the bound one. Colour-only passes keep it. Multisampled or layered depth targets run without ZCULL.

The game's render targets are 2048 rows tall (gl_rt.inc kRtH). At 1280 wide that is more than the ZCULL storage covers
(aliquot_total * pixel_squares_by_aliquots, about 2 M pixels), so the region covers the top rows that fit (1632 at 1280).
The game draws in the top rows only. What the GPU does with depth below the region is not known; the pixel check
(LSWTCS_ZCULLCHECK) compares whole frames.

Counters for [ZCULL]: framebuffer validations with ZCULL set up, invalidations on a target change and after depth-writing
shaders, forgets (blit, copy, foreign clear), targets that cannot use it, regions cut to fit.
Usage: python mesa_zcull_edits.py <mesa-src-dir>"""
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

HELPERS = r'''/* LSWTCS (mesa_zcull_edits.py): ZCULL when lsw_nvc0_zcull is set. See the script for the design. */
struct lsw_zcull_info {   /* libnx nvioctl_zcull_info (switch/nvidia/ioctl.h) */
   uint32_t width_align_pixels, height_align_pixels, pixel_squares_by_aliquots, aliquot_total;
   uint32_t region_byte_multiplier, region_header_size, subregion_header_size;
   uint32_t subregion_width_align_pixels, subregion_height_align_pixels, subregion_count;
};
extern const struct lsw_zcull_info *nvGpuGetZcullInfo(void);   /* libnx, filled when libdrm_nouveau opened the GPU */
volatile int lsw_nvc0_zcull = 0;
unsigned long long lsw_nvc0_zcull_binds = 0, lsw_nvc0_zcull_inval = 0, lsw_nvc0_zcull_inval_fp = 0, lsw_nvc0_zcull_forgets = 0;
unsigned long long lsw_nvc0_zcull_unsupported = 0, lsw_nvc0_zcull_clamped = 0;
static int lsw_zcull_on, lsw_zcull_inited, lsw_zcull_owned;   /* region on; init registers written; data has an owner */
static uint64_t lsw_zcull_addr;   /* the depth target the data belongs to */
static uint32_t lsw_zcull_w, lsw_zcull_h, lsw_zcull_layer;
void lsw_nvc0_zcull_forget(struct nouveau_pushbuf *push);
void lsw_nvc0_zcull_fp(struct nouveau_pushbuf *push, uint32_t test_mask);

static void
lsw_zcull_invalidate(struct nouveau_pushbuf *push)
{
   IMMED_NVC0(push, SUBC_3D(NVC0_3D_UNK12C8), 0);   /* deko3d's InvalidateZcullNoWfi, method 0x4b2 */
}

static void
lsw_zcull_region_off(struct nouveau_pushbuf *push)
{
   if (lsw_zcull_on)
      IMMED_NVC0(push, NVC0_3D(ZCULL_REGION), 0x3f);
   lsw_zcull_on = 0;
   lsw_zcull_owned = 0;
}

/* Depth was written where ZCULL did not see it. */
void
lsw_nvc0_zcull_forget(struct nouveau_pushbuf *push)
{
   if (!lsw_zcull_on)
      return;
   lsw_zcull_invalidate(push);
   lsw_zcull_owned = 0;   /* and again at the next framebuffer validation */
   ++lsw_nvc0_zcull_forgets;
}

/* From nvc0_fragprog_validate: after draws whose shader wrote depth (test mask 0x11), the data no longer matches. */
void
lsw_nvc0_zcull_fp(struct nouveau_pushbuf *push, uint32_t test_mask)
{
   static uint32_t last;
   if (lsw_zcull_on && last == 0x11 && test_mask != 0x11) {
      lsw_zcull_invalidate(push);
      ++lsw_nvc0_zcull_inval_fp;
   }
   last = test_mask;
}

static void
lsw_zcull_validate(struct nouveau_pushbuf *push, struct pipe_framebuffer_state *fb)
{
   const struct lsw_zcull_info *zi;
   struct nv50_miptree *mt;
   struct nv50_surface *sf;
   uint32_t w, h, max_h, aliquots;
   uint64_t addr;

   if (!lsw_nvc0_zcull) {
      lsw_zcull_region_off(push);
      return;
   }
   if (!fb->zsbuf)
      return;   /* colour only: the data stays with the last depth target, as in deko3d */
   mt = nv50_miptree(fb->zsbuf->texture);
   sf = nv50_surface(fb->zsbuf);
   zi = nvGpuGetZcullInfo();
   if (!zi || !zi->width_align_pixels || !zi->height_align_pixels || !zi->pixel_squares_by_aliquots ||
       mt->ms_mode != NVC0_3D_MULTISAMPLE_MODE_MS1 || sf->depth != 1) {
      lsw_zcull_region_off(push);
      ++lsw_nvc0_zcull_unsupported;
      return;
   }
   w = align(sf->width, zi->width_align_pixels);
   h = align(sf->height, zi->height_align_pixels);
   max_h = (uint32_t)((uint64_t)zi->aliquot_total * zi->pixel_squares_by_aliquots / w) /
           zi->height_align_pixels * zi->height_align_pixels;
   if (h > max_h) {   /* 2048-row render targets: cover the top rows that fit */
      h = max_h;
      ++lsw_nvc0_zcull_clamped;
   }
   if (!h) {
      lsw_zcull_region_off(push);
      ++lsw_nvc0_zcull_unsupported;
      return;
   }
   aliquots = (w * h + zi->pixel_squares_by_aliquots - 1) / zi->pixel_squares_by_aliquots;
   if (!lsw_zcull_inited) {
      BEGIN_NVC0(push, NVC0_3D(ZCULL_UNK1968), 1);
      PUSH_DATA (push, 0x11);
      BEGIN_NVC0(push, NVC0_3D(ZCULL_UNK0DD8), 1);   /* stencil cull criteria: NOTEQUAL, reference 0, mask 0xff */
      PUSH_DATA (push, 0xff000005);
      lsw_zcull_inited = 1;
   }
   if (!lsw_zcull_on) {
      IMMED_NVC0(push, NVC0_3D(ZCULL_REGION), 0);
      lsw_zcull_on = 1;
      lsw_zcull_owned = 0;
   }
   BEGIN_NVC0(push, SUBC_3D(NVC0_3D_UNK07E0), 2);   /* image size (<< 16) and layer size, in aliquots */
   PUSH_DATA (push, aliquots << 16);
   PUSH_DATA (push, aliquots);
   IMMED_NVC0(push, SUBC_3D(NVC0_3D_UNK15C8), 2);   /* zeta type: depth only */
   BEGIN_NVC0(push, NVC0_3D(ZCULL_WIDTH), 4);
   PUSH_DATA (push, w);
   PUSH_DATA (push, h);
   PUSH_DATA (push, 1);
   PUSH_DATA (push, 0);
   BEGIN_NVC0(push, NVC0_3D(ZCULL_WINDOW_OFFSET_X), 2);
   PUSH_DATA (push, 0);
   PUSH_DATA (push, 0);
   IMMED_NVC0(push, SUBC_3D(NVC8_3D_UNK02E8), 0);
   addr = mt->base.address + sf->offset;
   if (!lsw_zcull_owned || addr != lsw_zcull_addr || w != lsw_zcull_w || h != lsw_zcull_h ||
       sf->base.u.tex.first_layer != lsw_zcull_layer) {
      lsw_zcull_invalidate(push);
      lsw_zcull_owned = 1;
      lsw_zcull_addr = addr;
      lsw_zcull_w = w;
      lsw_zcull_h = h;
      lsw_zcull_layer = sf->base.u.tex.first_layer;
      ++lsw_nvc0_zcull_inval;
   }
   ++lsw_nvc0_zcull_binds;
}

static void
nvc0_validate_fb(struct nvc0_context *nvc0)
'''

edit(N + 'nvc0_state_validate.c', [
    ('static void\nnvc0_validate_fb(struct nvc0_context *nvc0)\n', HELPERS),
    ('   if (nr_cbufs == 0 && !fb->zsbuf) {\n      assert(util_is_power_of_two_or_zero(fb->samples));\n',
     '   lsw_zcull_validate(push, fb);   /* LSWTCS: mesa_zcull_edits.py */\n\n'
     '   if (nr_cbufs == 0 && !fb->zsbuf) {\n      assert(util_is_power_of_two_or_zero(fb->samples));\n'),
])

edit(N + 'nvc0_shader_state.c', [
    ('   BEGIN_NVC0(push, NVC0_3D(ZCULL_TEST_MASK), 1);\n   PUSH_DATA (push, fp->flags[0]);\n',
     '   BEGIN_NVC0(push, NVC0_3D(ZCULL_TEST_MASK), 1);\n   PUSH_DATA (push, fp->flags[0]);\n'
     '   { extern void lsw_nvc0_zcull_fp(struct nouveau_pushbuf *, uint32_t);   /* LSWTCS: mesa_zcull_edits.py */\n'
     '     lsw_nvc0_zcull_fp(push, fp->flags[0]); }\n'),
])

HINT = ('      if (lsw_nvc0_zcull) {   /* LSWTCS (mesa_zcull_edits.py): ZCULL\'s clear depth hint, as deko3d */\n'
        '         BEGIN_NVC0(push, NVC0_3D(ZCULL_UNK0DBC), 1);\n'
        '         PUSH_DATA (push, (depth < 0.5) | ((depth == 0.0 || depth == 1.0) << 16));\n'
        '      }\n')

edit(N + 'nvc0_surface.c', [
    ('#include "nv50/nv50_blit.h"\n',
     '#include "nv50/nv50_blit.h"\n\n'
     '/* LSWTCS: mesa_zcull_edits.py (nvc0_state_validate.c) */\n'
     'extern volatile int lsw_nvc0_zcull;\n'
     'void lsw_nvc0_zcull_forget(struct nouveau_pushbuf *push);\n'),
    ('   if (dst->target == PIPE_BUFFER && src->target == PIPE_BUFFER) {\n      nouveau_copy_buffer(&nvc0->base,\n',
     '   if (util_format_is_depth_or_stencil(dst->format))   /* LSWTCS: mesa_zcull_edits.py */\n'
     '      lsw_nvc0_zcull_forget(nvc0->base.pushbuf);\n\n'
     '   if (dst->target == PIPE_BUFFER && src->target == PIPE_BUFFER) {\n      nouveau_copy_buffer(&nvc0->base,\n'),
    ('   if (clear_flags & PIPE_CLEAR_DEPTH) {\n      BEGIN_NVC0(push, NVC0_3D(CLEAR_DEPTH), 1);\n      PUSH_DATAf(push, depth);\n',
     '   if (clear_flags & PIPE_CLEAR_DEPTH) {\n' + HINT +
     '      BEGIN_NVC0(push, NVC0_3D(CLEAR_DEPTH), 1);\n      PUSH_DATAf(push, depth);\n'),
    ('   if (!render_condition_enabled)\n      IMMED_NVC0(push, NVC0_3D(COND_MODE), nvc0->cond_condmode);\n\n'
     '   nvc0->dirty_3d |= NVC0_NEW_3D_FRAMEBUFFER;\n}\n\nvoid\nnvc0_clear(',
     '   if (!render_condition_enabled)\n      IMMED_NVC0(push, NVC0_3D(COND_MODE), nvc0->cond_condmode);\n\n'
     '   lsw_nvc0_zcull_forget(push);   /* LSWTCS: a surface other than the bound target may have been cleared */\n'
     '   nvc0->dirty_3d |= NVC0_NEW_3D_FRAMEBUFFER;\n}\n\nvoid\nnvc0_clear('),
    ('   if (buffers & PIPE_CLEAR_DEPTH) {\n      BEGIN_NVC0(push, NVC0_3D(CLEAR_DEPTH), 1);\n      PUSH_DATA (push, fui(depth));\n',
     '   if (buffers & PIPE_CLEAR_DEPTH) {\n' + HINT +
     '      BEGIN_NVC0(push, NVC0_3D(CLEAR_DEPTH), 1);\n      PUSH_DATA (push, fui(depth));\n'),
    ('   if (nvc0->screen->num_occlusion_queries_active)\n      IMMED_NVC0(push, NVC0_3D(SAMPLECNT_ENABLE), 1);\n\n'
     '   NOUVEAU_DRV_STAT(&nvc0->screen->base, tex_blit_count, 1);\n',
     '   if (nvc0->screen->num_occlusion_queries_active)\n      IMMED_NVC0(push, NVC0_3D(SAMPLECNT_ENABLE), 1);\n\n'
     '   if (info->mask & PIPE_MASK_ZS)   /* LSWTCS: mesa_zcull_edits.py */\n'
     '      lsw_nvc0_zcull_forget(push);\n\n'
     '   NOUVEAU_DRV_STAT(&nvc0->screen->base, tex_blit_count, 1);\n'),
])
print('zcull edits applied')

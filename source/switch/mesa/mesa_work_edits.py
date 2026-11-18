"""LSWTCS: work counters in nvc0's draw path, for comparing builds on any host (log111).

Counts, per draw that reaches nvc0_draw_vbo: the draw, its vertices (count x instances), and the code size of the vertex and
pixel programs bound for it (nvc0 machine code, the bytes the GPU runs). The sums are the same on the console and in an
emulator, because the NRO's own Mesa compiles the shaders. Pixel-program bytes x draws is a proxy for shading work, not a
pixel count: the number of pixels each draw covers is not measured.

The counters are plain integers written by the thread that runs nvc0 (Mesa's glthread worker) and read now and then by the render
thread through lsw_nvc0_work_stats(); a torn read can only skew one report. Applied after the other edits.
Usage: python mesa_work_edits.py <mesa-src-dir>"""
import os
import sys

root = sys.argv[1]
TAG = 'mesa_work_edits.py'


def edit(rel, pairs):
    p = os.path.join(root, rel)
    s = open(p, newline='').read()
    if TAG in s:   # already applied
        print('already applied', rel)
        return
    for old, new in pairs:
        assert s.count(old) == 1, (rel, old[:70])
        s = s.replace(old, new)
    open(p, 'w', newline='').write(s)
    print('ok', rel)


edit('src/gallium/drivers/nouveau/nvc0/nvc0_vbo.c', [
("""void
nvc0_draw_vbo(struct pipe_context *pipe, const struct pipe_draw_info *info)
{""",
"""/* LSWTCS mesa_work_edits.py: work counters (draws, vertices, vertex and pixel program bytes per draw). */
static unsigned long long lsw_work_draws, lsw_work_vertices, lsw_work_vp_bytes, lsw_work_fp_bytes;
void lsw_nvc0_work_stats(unsigned long long *draws, unsigned long long *vertices, unsigned long long *vp_bytes,
                         unsigned long long *fp_bytes) __attribute__((visibility("default")));
void lsw_nvc0_work_stats(unsigned long long *draws, unsigned long long *vertices, unsigned long long *vp_bytes,
                         unsigned long long *fp_bytes)
{
   *draws = lsw_work_draws;
   *vertices = lsw_work_vertices;
   *vp_bytes = lsw_work_vp_bytes;
   *fp_bytes = lsw_work_fp_bytes;
}

void
nvc0_draw_vbo(struct pipe_context *pipe, const struct pipe_draw_info *info)
{"""),
("""cleanup:
   push->kick_notify = nvc0_default_kick_notify;
""",
"""cleanup:
   push->kick_notify = nvc0_default_kick_notify;
   ++lsw_work_draws;   /* LSWTCS: mesa_work_edits.py */
   lsw_work_vertices += (unsigned long long)info->count * (info->instance_count ? info->instance_count : 1);
   if (nvc0->vertprog)
      lsw_work_vp_bytes += nvc0->vertprog->code_size;
   if (nvc0->fragprog)
      lsw_work_fp_bytes += nvc0->fragprog->code_size;
"""),
])

"""LSWTCS: reuse nvc0's translation of a shader stage that was already translated in this session (in memory only).

Mesa links each GL program on its own: the state tracker makes a TGSI shader per stage per linked program, and nvc0 runs the whole
nv50_ir back end (TGSI conversion, SSA, optimisation, register allocation, emission) for each at create time. The port's programs
share their stages heavily: the saved list of 1082 programs (2026-11-04) has 114 different vertex shaders (about 25 KB of GLSL
each) and 804 different pixel shaders. Run 109's compile threads spent 55 % of their samples in nv50_ir; one program costs
130-230 ms of worker time.

nvc0_program_translate is a pure function of (shader type, TGSI tokens, user clip planes, chipset): it reads nothing else of the
program or the context. Its result is now kept, keyed by the SHA-1 of the tokens, the type, the clip planes and the chipset: a
snapshot of the program right after translation, with its own copies of the code and the relocation and fixup tables (relocation
and fixups patch the code in place at upload, so every program gets fresh copies of the untouched ones). Programs with stream
output and compute programs are not cached. At most 16 MB of snapshots (code and tables).

Every 64th reuse (LSWTCS_SHADERDEDUPCHECK, 0 = never; every 8th until log104) the stage is also translated again and compared word for word (code, header,
registers, tables); a difference is logged as [NVC0DEDUP] and the fresh translation is used. LSWTCS_SHADERDEDUP=0 turns the reuse
off. lsw_nvc0_dedup_stats() reports hits, misses, checks and mismatches for [GLPROG].
Run 110 (log103dd): 1805 reuses against 1360 translations, worker time per program 278 -> 153 ms, no render stall of 250 ms or
more. 36 of 225 checks "differed", all pixel shaders with the same code size: the check compared the tables with memcmp, and
FixupEntry (8-byte pointer + 4-byte field) and RelocEntry (int8 then enum) carry padding bytes nv50_ir never writes. From log104
the tables are compared field by field and the log says which part differed (bit mask: 1 code size, 2 code, 4 header, 8 registers
/ flags, 16 vertex fields, 32 pixel fields, 64 relocations, 128 fixups) and how many code words.
Run 111 (log104dd): 225 checks, 0 differences, so the padding was the whole story. Each check is a full translation (about 9 % of
the worker time at one in 8), so from log105 one reuse in 64 is checked.
Applied after the other edits. Usage: python mesa_dedup_edits.py <mesa-src-dir>"""
import sys, os
root = sys.argv[1]
TAG = 'mesa_dedup_edits.py'


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


# Sizes of the relocation and fixup tables (their entry types are C++).
edit('src/gallium/drivers/nouveau/codegen/nv50_ir_target.cpp', [
("""void
nv50_ir_get_target_library(uint32_t chipset,
""",
"""/* LSWTCS mesa_dedup_edits.py: byte sizes of the tables, so a translation can be copied. */
size_t
nv50_ir_reloc_bytes(const void *relocData)
{
   const nv50_ir::RelocInfo *info = reinterpret_cast<const nv50_ir::RelocInfo *>(relocData);
   return sizeof(*info) + info->count * sizeof(nv50_ir::RelocEntry);
}

size_t
nv50_ir_fixup_bytes(const void *fixupData)
{
   const nv50_ir::FixupInfo *info = reinterpret_cast<const nv50_ir::FixupInfo *>(fixupData);
   return sizeof(*info) + info->count * sizeof(nv50_ir::FixupEntry);
}

/* Field by field: the entries have padding bytes that are never written. */
int
nv50_ir_reloc_equal(const void *a, const void *b)
{
   const nv50_ir::RelocInfo *x = reinterpret_cast<const nv50_ir::RelocInfo *>(a);
   const nv50_ir::RelocInfo *y = reinterpret_cast<const nv50_ir::RelocInfo *>(b);
   if (x->count != y->count || x->codePos != y->codePos || x->libPos != y->libPos || x->dataPos != y->dataPos)
      return 0;
   for (unsigned i = 0; i < x->count; ++i) {
      const nv50_ir::RelocEntry &e = x->entry[i], &f = y->entry[i];
      if (e.data != f.data || e.mask != f.mask || e.offset != f.offset || e.bitPos != f.bitPos || e.type != f.type)
         return 0;
   }
   return 1;
}

int
nv50_ir_fixup_equal(const void *a, const void *b)
{
   const nv50_ir::FixupInfo *x = reinterpret_cast<const nv50_ir::FixupInfo *>(a);
   const nv50_ir::FixupInfo *y = reinterpret_cast<const nv50_ir::FixupInfo *>(b);
   if (x->count != y->count)
      return 0;
   for (unsigned i = 0; i < x->count; ++i)
      if (x->entry[i].apply != y->entry[i].apply || x->entry[i].val != y->entry[i].val)
         return 0;
   return 1;
}

void
nv50_ir_get_target_library(uint32_t chipset,
"""),
])

edit('src/gallium/drivers/nouveau/nvc0/nvc0_program.c', [
("""#include "codegen/nv50_ir_driver.h"
""",
"""#include "codegen/nv50_ir_driver.h"
#include "tgsi/tgsi_parse.h"
#include "util/mesa-sha1.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
"""),
("""bool
nvc0_program_translate(struct nvc0_program *prog, uint16_t chipset,
                       struct pipe_debug_callback *debug)
{
   struct nv50_ir_prog_info *info;
""",
"""static bool
nvc0_program_translate_uncached(struct nvc0_program *prog, uint16_t chipset,
                                struct pipe_debug_callback *debug)
{
   struct nv50_ir_prog_info *info;
"""),
("""uint32_t
nvc0_program_symbol_offset(const struct nvc0_program *prog, uint32_t label)
""",
"""/* LSWTCS mesa_dedup_edits.py: translations kept for identical stages (see the script's header). */
size_t nv50_ir_reloc_bytes(const void *relocData);
size_t nv50_ir_fixup_bytes(const void *fixupData);
int nv50_ir_reloc_equal(const void *a, const void *b);
int nv50_ir_fixup_equal(const void *a, const void *b);

struct lsw_dedup_entry {
   struct lsw_dedup_entry *next;
   unsigned char sha[20];
   ubyte type;
   uint8_t num_ucps;
   uint16_t chipset;
   struct nvc0_program snap;   /* code / relocs / fixups owned here; pipe zeroed */
   size_t bytes;
};
void lsw_nvc0_dedup_stats(unsigned *hits, unsigned *misses, unsigned *checks, unsigned *mismatches, unsigned *kb)
   __attribute__((visibility("default")));
#define LSW_DEDUP_BUCKETS 4096
static struct lsw_dedup_entry *lsw_dedup_table[LSW_DEDUP_BUCKETS];
static pthread_mutex_t lsw_dedup_lock = PTHREAD_MUTEX_INITIALIZER;
static size_t lsw_dedup_bytes;
static unsigned lsw_dedup_hits, lsw_dedup_misses, lsw_dedup_checks, lsw_dedup_mismatches, lsw_dedup_full;

void
lsw_nvc0_dedup_stats(unsigned *hits, unsigned *misses, unsigned *checks, unsigned *mismatches, unsigned *kb)
{
   pthread_mutex_lock(&lsw_dedup_lock);
   *hits = lsw_dedup_hits; *misses = lsw_dedup_misses; *checks = lsw_dedup_checks;
   *mismatches = lsw_dedup_mismatches; *kb = (unsigned)(lsw_dedup_bytes >> 10);
   pthread_mutex_unlock(&lsw_dedup_lock);
}

static int
lsw_dedup_env(const char *name, int def)
{
   const char *e = getenv(name);
   return e && *e ? atoi(e) : def;
}

static void *
lsw_dedup_dup(const void *p, size_t n)
{
   void *q;
   if (!p || !n)
      return NULL;
   q = MALLOC(n);
   if (q)
      memcpy(q, p, n);
   return q;
}

/* Copies a translated program's results; false when out of memory. */
static bool
lsw_dedup_copy(struct nvc0_program *dst, const struct nvc0_program *src)
{
   const struct pipe_shader_state pipe = dst->pipe;
   *dst = *src;
   dst->pipe = pipe;
   dst->mem = NULL;
   dst->tfb = NULL;
   dst->code_base = 0;
   dst->code = lsw_dedup_dup(src->code, src->code_size);
   dst->relocs = src->relocs ? lsw_dedup_dup(src->relocs, nv50_ir_reloc_bytes(src->relocs)) : NULL;
   dst->fixups = src->fixups ? lsw_dedup_dup(src->fixups, nv50_ir_fixup_bytes(src->fixups)) : NULL;
   if ((src->code_size && !dst->code) || (src->relocs && !dst->relocs) || (src->fixups && !dst->fixups)) {
      FREE(dst->code); FREE(dst->relocs); FREE(dst->fixups);
      dst->code = NULL; dst->relocs = NULL; dst->fixups = NULL;
      return false;
   }
   return true;
}

/* 0 when the two translations are the same; else a mask of what differs (see the script's header). */
static unsigned
lsw_dedup_diff(const struct nvc0_program *a, const struct nvc0_program *b, unsigned *code_words)
{
   unsigned m = 0, i;
   *code_words = 0;
   if (a->code_size != b->code_size)
      m |= 1;
   else
      for (i = 0; i < a->code_size / 4; ++i)
         if (a->code[i] != b->code[i])
            ++*code_words;
   if (*code_words)
      m |= 2;
   if (memcmp(a->hdr, b->hdr, sizeof(a->hdr)))
      m |= 4;
   if (a->num_gprs != b->num_gprs || a->need_tls != b->need_tls || memcmp(a->flags, b->flags, sizeof(a->flags)) ||
       a->num_barriers != b->num_barriers)
      m |= 8;
   if (a->vp.clip_mode != b->vp.clip_mode || a->vp.clip_enable != b->vp.clip_enable || a->vp.cull_enable != b->vp.cull_enable ||
       a->vp.num_ucps != b->vp.num_ucps || a->vp.edgeflag != b->vp.edgeflag || a->vp.need_vertex_id != b->vp.need_vertex_id ||
       a->vp.need_draw_parameters != b->vp.need_draw_parameters || a->vp.layer_viewport_relative != b->vp.layer_viewport_relative)
      m |= 16;
   if (a->fp.early_z != b->fp.early_z || a->fp.colors != b->fp.colors || a->fp.color_interp[0] != b->fp.color_interp[0] ||
       a->fp.color_interp[1] != b->fp.color_interp[1] || a->fp.sample_mask_in != b->fp.sample_mask_in ||
       a->fp.force_persample_interp != b->fp.force_persample_interp || a->fp.flatshade != b->fp.flatshade ||
       a->fp.reads_framebuffer != b->fp.reads_framebuffer || a->fp.post_depth_coverage != b->fp.post_depth_coverage)
      m |= 32;
   if (!a->relocs != !b->relocs || (a->relocs && !nv50_ir_reloc_equal(a->relocs, b->relocs)))
      m |= 64;
   if (!a->fixups != !b->fixups || (a->fixups && !nv50_ir_fixup_equal(a->fixups, b->fixups)))
      m |= 128;
   return m;
}

bool
nvc0_program_translate(struct nvc0_program *prog, uint16_t chipset,
                       struct pipe_debug_callback *debug)
{
   static int on = -1, check_every = 64;
   unsigned char sha[20];
   unsigned bucket, n_hit = 0;
   struct lsw_dedup_entry *e;
   bool ok = false;

   if (on < 0) {
      check_every = lsw_dedup_env("LSWTCS_SHADERDEDUPCHECK", 64);
      on = lsw_dedup_env("LSWTCS_SHADERDEDUP", 1) != 0;
   }
   if (!on || prog->pipe.type != PIPE_SHADER_IR_TGSI || !prog->pipe.tokens || prog->type == PIPE_SHADER_COMPUTE ||
       prog->pipe.stream_output.num_outputs)
      return nvc0_program_translate_uncached(prog, chipset, debug);

   _mesa_sha1_compute(prog->pipe.tokens, tgsi_num_tokens(prog->pipe.tokens) * sizeof(struct tgsi_token), sha);
   bucket = (sha[0] | (sha[1] << 8)) % LSW_DEDUP_BUCKETS;

   pthread_mutex_lock(&lsw_dedup_lock);
   for (e = lsw_dedup_table[bucket]; e; e = e->next)
      if (e->type == prog->type && e->num_ucps == prog->vp.num_ucps && e->chipset == chipset && !memcmp(e->sha, sha, 20))
         break;
   if (e) {
      ok = lsw_dedup_copy(prog, &e->snap);   /* entries are never freed: copying under the lock is safe and short */
      if (ok)
         n_hit = ++lsw_dedup_hits;
   }
   pthread_mutex_unlock(&lsw_dedup_lock);

   if (e && ok) {
      if (check_every > 0 && (n_hit % (unsigned)check_every) == 0) {
         struct nvc0_program fresh;
         memset(&fresh, 0, sizeof(fresh));
         fresh.pipe = prog->pipe;
         fresh.type = prog->type;
         fresh.vp.num_ucps = prog->vp.num_ucps;
         if (nvc0_program_translate_uncached(&fresh, chipset, debug)) {
            unsigned words = 0;
            const unsigned diff = lsw_dedup_diff(prog, &fresh, &words);
            const bool same = diff == 0;
            pthread_mutex_lock(&lsw_dedup_lock);
            ++lsw_dedup_checks;
            if (!same)
               ++lsw_dedup_mismatches;
            pthread_mutex_unlock(&lsw_dedup_lock);
            if (!same) {
               printf("[NVC0DEDUP] type %u: reused translation differs from a fresh one (what 0x%X, %u of %u code words); "
                      "fresh one used\\n", prog->type, diff, words, prog->code_size / 4);
               fflush(stdout);
               FREE(prog->code); FREE(prog->relocs); FREE(prog->fixups);
               *prog = fresh;
               return true;
            }
            FREE(fresh.code); FREE(fresh.relocs); FREE(fresh.fixups);
            if (fresh.tfb) FREE(fresh.tfb);
         }
      }
      return true;
   }

   if (!nvc0_program_translate_uncached(prog, chipset, debug))
      return false;

   pthread_mutex_lock(&lsw_dedup_lock);
   ++lsw_dedup_misses;
   for (e = lsw_dedup_table[bucket]; e; e = e->next)   /* another thread may have added it meanwhile */
      if (e->type == prog->type && e->num_ucps == prog->vp.num_ucps && e->chipset == chipset && !memcmp(e->sha, sha, 20))
         break;
   if (!e) {
      const size_t bytes = prog->code_size + (prog->relocs ? nv50_ir_reloc_bytes(prog->relocs) : 0) +
                           (prog->fixups ? nv50_ir_fixup_bytes(prog->fixups) : 0) + sizeof(struct lsw_dedup_entry);
      if (lsw_dedup_bytes + bytes > ((size_t)16 << 20)) {
         if (!lsw_dedup_full++) {
            printf("[NVC0DEDUP] 16 MB of kept translations: new ones are no longer kept\\n");
            fflush(stdout);
         }
      } else if ((e = CALLOC_STRUCT(lsw_dedup_entry))) {
         memset(&e->snap, 0, sizeof(e->snap));
         if (lsw_dedup_copy(&e->snap, prog)) {
            memset(&e->snap.pipe, 0, sizeof(e->snap.pipe));
            memcpy(e->sha, sha, 20);
            e->type = prog->type;
            e->num_ucps = prog->vp.num_ucps;
            e->chipset = chipset;
            e->bytes = bytes;
            e->next = lsw_dedup_table[bucket];
            lsw_dedup_table[bucket] = e;
            lsw_dedup_bytes += bytes;
         } else {
            FREE(e);
         }
      }
   }
   pthread_mutex_unlock(&lsw_dedup_lock);
   return true;
}

uint32_t
nvc0_program_symbol_offset(const struct nvc0_program *prog, uint32_t label)
"""),
])
print('dedup edits applied')

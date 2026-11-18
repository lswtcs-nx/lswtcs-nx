"""LSWTCS: keep nvc0's stage translations on the SD card between sessions (log116).

mesa_dedup_edits.py keeps each stage's nv50_ir translation in memory for the session, keyed by the SHA-1 of its TGSI tokens, the
type, the clip planes and the chipset. Mesa's own disk cache only saves the GLSL front end (the linked program and its TGSI), so
every session still runs the whole nv50_ir back end once per stage: about 95 ms of worker time per program on the Switch even
with that cache (runs 56-60), and run 118's area entries waited 670-1673 ms for 10-18 programs while the saved-program list was
still being built (1042 queued).

Now each translation the session makes is also appended to <MESA_GLSL_CACHE_DIR>/lsw_nvc0.bin (sdmc:/lswtcs/mesacache), and the
next session loads the file into the in-memory table before its first translation:
- A record is the program struct right after translation (its pointers zeroed), the code, the relocation table and the fixup
  table, with an FNV-1a sum. The fixup entries hold a function pointer, which is not the same from one launch to the next: on
  the card it is a number, the function's place among the GM107 emitter's two fixup functions (interpApply, selpFlip). A
  translation with any other fixup function is not saved.
- The file starts with an identity: a version string, the codegen environment variables (NV50_MUL_ZERO_WINS, NV50_PROG_*) and
  the struct and table entry sizes. Another identity starts the file over. Bump kIdent when an edit changes nv50_ir's output.
- A record cut short (the game closed from the HOME menu while writing) and anything after it are dropped (ftruncate).
- The first 8 loaded translations a session reuses (LSWTCS_NVC0DISKCHECK) are also translated again and compared, as the
  in-memory reuse is checked one time in 64. A difference drops every loaded translation for the session and starts the
  file over.
- LSWTCS_NVC0DISK=0 turns it off; MESA_GLSL_CACHE_DISABLE=true (a simulated first session) turns it off too.
- Loaded translations count against the in-memory table's 16 MB.
Applied after mesa_dedup_edits.py. Usage: python mesa_nvc0disk_edits.py <mesa-src-dir>"""
import sys, os
root = sys.argv[1]
TAG = 'mesa_nvc0disk_edits.py'


def edit(rel, pairs):
    p = os.path.join(root, rel)
    s = open(p, newline='').read()
    if TAG in s:   # already applied
        print('already applied', rel)
        return
    for old, new, n in pairs:
        assert s.count(old) == n, (rel, old[:70], s.count(old))
        s = s.replace(old, new)
    open(p, 'w', newline='').write(s)


# 1. The GM107 emitter's fixup functions, by number.
edit('src/gallium/drivers/nouveau/codegen/nv50_ir_emit_gm107.cpp', [
    ('\n} // namespace nv50_ir\n',
     '\n} // namespace nv50_ir\n\n'
     '/* LSWTCS mesa_nvc0disk_edits.py: this emitter\'s fixup functions, so a fixup table on the SD card can name them by number. */\n'
     'extern "C" void\n'
     'lsw_gm107_fixup_fns(void **fns)\n'
     '{\n'
     '   fns[0] = reinterpret_cast<void *>(nv50_ir::interpApply);\n'
     '   fns[1] = reinterpret_cast<void *>(nv50_ir::selpFlip);\n'
     '}\n', 1),
])

# 2. Fixup tables to and from numbers; entry sizes for the identity.
edit('src/gallium/drivers/nouveau/codegen/nv50_ir_target.cpp', [
    ('void\nnv50_ir_get_target_library(uint32_t chipset,',
     '/* LSWTCS mesa_nvc0disk_edits.py: a fixup table\'s functions as numbers (place in fns + 1), and back, in place. 0 when a\n'
     '   function is not in fns or a number is out of range (the table is then half converted: the caller drops it). */\n'
     'int\n'
     'nv50_ir_fixup_to_ids(void *fixupData, void *const *fns, int nfns)\n'
     '{\n'
     '   nv50_ir::FixupInfo *info = reinterpret_cast<nv50_ir::FixupInfo *>(fixupData);\n'
     '   for (unsigned i = 0; i < info->count; ++i) {\n'
     '      uintptr_t id = 0;\n'
     '      for (int k = 0; k < nfns; ++k)\n'
     '         if (reinterpret_cast<void *>(info->entry[i].apply) == fns[k])\n'
     '            id = k + 1;\n'
     '      if (!id)\n'
     '         return 0;\n'
     '      info->entry[i].apply = reinterpret_cast<nv50_ir::FixupApply>(id);\n'
     '   }\n'
     '   return 1;\n'
     '}\n\n'
     'int\n'
     'nv50_ir_fixup_from_ids(void *fixupData, void *const *fns, int nfns)\n'
     '{\n'
     '   nv50_ir::FixupInfo *info = reinterpret_cast<nv50_ir::FixupInfo *>(fixupData);\n'
     '   for (unsigned i = 0; i < info->count; ++i) {\n'
     '      const uintptr_t id = reinterpret_cast<uintptr_t>(info->entry[i].apply);\n'
     '      if (id < 1 || id > (uintptr_t)nfns || !fns[id - 1])\n'
     '         return 0;\n'
     '      info->entry[i].apply = reinterpret_cast<nv50_ir::FixupApply>(fns[id - 1]);\n'
     '   }\n'
     '   return 1;\n'
     '}\n\n'
     'size_t\n'
     'nv50_ir_fixup_entry_size(void)\n'
     '{\n'
     '   return sizeof(nv50_ir::FixupEntry);\n'
     '}\n\n'
     'size_t\n'
     'nv50_ir_reloc_entry_size(void)\n'
     '{\n'
     '   return sizeof(nv50_ir::RelocEntry);\n'
     '}\n\n'
     'void\nnv50_ir_get_target_library(uint32_t chipset,', 1),
])

DISK_C = r'''
/* ---- LSWTCS mesa_nvc0disk_edits.py: the kept translations on the SD card (see the script's header) ---- */
void lsw_gm107_fixup_fns(void **fns);
int nv50_ir_fixup_to_ids(void *fixupData, void *const *fns, int nfns);
int nv50_ir_fixup_from_ids(void *fixupData, void *const *fns, int nfns);
size_t nv50_ir_fixup_entry_size(void);
size_t nv50_ir_reloc_entry_size(void);
void lsw_nvc0_disk_stats(unsigned *loaded, unsigned *reused, unsigned *written, unsigned *checked, unsigned *differed)
   __attribute__((visibility("default")));

#define LSW_DISK_MAGIC 0x3044564Eu   /* "NVD0" */
#define LSW_DISK_MAX_BYTES ((long)48 << 20)
static const char kIdent[] = "switch-mesa-20.1.0-rc3-5+lswtcs nvc0 translations 1";
struct lsw_disk_head { uint32_t magic, zero; unsigned char id[20]; uint32_t zero2; };
struct lsw_disk_rec {
   uint32_t bytes;   /* the whole record, this header included */
   uint32_t sum;     /* FNV-1a of the rest */
   unsigned char sha[20];
   uint8_t type, num_ucps;
   uint16_t chipset;
   uint32_t code_size, reloc_bytes, fixup_bytes;
};
static pthread_once_t lsw_disk_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t lsw_disk_lock = PTHREAD_MUTEX_INITIALIZER;   /* the file and the counts; never taken inside the table's */
static FILE *lsw_disk_out;
static char lsw_disk_path[256];
static unsigned char lsw_disk_id[20];
static void *lsw_disk_fns[2];
static unsigned lsw_disk_loaded, lsw_disk_reused, lsw_disk_written, lsw_disk_checked, lsw_disk_differed, lsw_disk_checks_left;

void
lsw_nvc0_disk_stats(unsigned *loaded, unsigned *reused, unsigned *written, unsigned *checked, unsigned *differed)
{
   pthread_mutex_lock(&lsw_disk_lock);
   *loaded = lsw_disk_loaded; *reused = lsw_disk_reused; *written = lsw_disk_written;
   *checked = lsw_disk_checked; *differed = lsw_disk_differed;
   pthread_mutex_unlock(&lsw_disk_lock);
}

static uint32_t
lsw_disk_fnv(const unsigned char *p, size_t n)
{
   uint32_t h = 2166136261u;
   size_t i;
   for (i = 0; i < n; ++i) {
      h ^= p[i];
      h *= 16777619u;
   }
   return h;
}

/* Under lsw_disk_lock or before anyone else uses the file. */
static FILE *
lsw_disk_start_over(void)
{
   struct lsw_disk_head h;
   FILE *f = fopen(lsw_disk_path, "wb");
   if (!f)
      return NULL;
   setvbuf(f, NULL, _IOFBF, 256 << 10);
   memset(&h, 0, sizeof(h));
   h.magic = LSW_DISK_MAGIC;
   memcpy(h.id, lsw_disk_id, 20);
   if (fwrite(&h, sizeof(h), 1, f) != 1) {
      fclose(f);
      return NULL;
   }
   fflush(f);
   return f;
}

/* One record of the file into the table (as a reuse would find it). False if it cannot be used. */
static bool
lsw_disk_add(const struct lsw_disk_rec *r, const unsigned char *buf)
{
   const size_t bytes = r->code_size + r->reloc_bytes + r->fixup_bytes + sizeof(struct lsw_dedup_entry);
   const unsigned bucket = (r->sha[0] | (r->sha[1] << 8)) % LSW_DEDUP_BUCKETS;
   const unsigned char *p = buf + sizeof(struct nvc0_program);
   struct lsw_dedup_entry *e, *o;
   struct nvc0_program *s;

   e = CALLOC_STRUCT(lsw_dedup_entry);
   if (!e)
      return false;
   s = &e->snap;
   memcpy(s, buf, sizeof(*s));
   memset(&s->pipe, 0, sizeof(s->pipe));
   s->code = NULL; s->relocs = NULL; s->fixups = NULL; s->tfb = NULL; s->mem = NULL; s->cp.syms = NULL;
   s->code = lsw_dedup_dup(p, r->code_size);
   s->relocs = lsw_dedup_dup(p + r->code_size, r->reloc_bytes);
   s->fixups = lsw_dedup_dup(p + r->code_size + r->reloc_bytes, r->fixup_bytes);
   if ((r->code_size && !s->code) || (r->reloc_bytes && !s->relocs) || (r->fixup_bytes && !s->fixups) ||
       s->code_size != r->code_size ||
       (s->fixups && !nv50_ir_fixup_from_ids(s->fixups, lsw_disk_fns, 2))) {
      FREE(s->code); FREE(s->relocs); FREE(s->fixups); FREE(e);
      return false;
   }
   memcpy(e->sha, r->sha, 20);
   e->type = r->type;
   e->num_ucps = r->num_ucps;
   e->chipset = r->chipset;
   e->from_disk = 1;
   e->bytes = bytes;
   pthread_mutex_lock(&lsw_dedup_lock);
   for (o = lsw_dedup_table[bucket]; o; o = o->next)
      if (o->type == e->type && o->num_ucps == e->num_ucps && o->chipset == e->chipset && !memcmp(o->sha, e->sha, 20))
         break;
   if (o || lsw_dedup_bytes + bytes > ((size_t)16 << 20)) {
      pthread_mutex_unlock(&lsw_dedup_lock);
      FREE(s->code); FREE(s->relocs); FREE(s->fixups); FREE(e);
      return false;
   }
   e->next = lsw_dedup_table[bucket];
   lsw_dedup_table[bucket] = e;
   lsw_dedup_bytes += bytes;
   pthread_mutex_unlock(&lsw_dedup_lock);
   return true;
}

static void
lsw_disk_load(void)
{
   static const char *const envs[] = {"NV50_MUL_ZERO_WINS", "NV50_PROG_OPTIMIZE", "NV50_PROG_USE_NIR", "NV50_PROG_SCHED",
                                      "NV50_PROG_DEBUG", "NV50_PROG_CHIPSET"};
   const char *dir = getenv("MESA_GLSL_CACHE_DIR");
   const char *dis = getenv("MESA_GLSL_CACHE_DISABLE");
   const int64_t t0 = os_time_get_nano();
   struct mesa_sha1 ctx;
   uint32_t sizes[3];
   unsigned i, kb = 0, dropped = 0;
   long good = 0;
   bool cut = false;
   const char *why = "none yet";
   FILE *f;

   lsw_disk_checks_left = (unsigned)lsw_dedup_env("LSWTCS_NVC0DISKCHECK", 8);
   if (!lsw_dedup_env("LSWTCS_NVC0DISK", 1) || !dir || (dis && (!strcmp(dis, "true") || !strcmp(dis, "1")))) {
      printf("[NVC0DISK] off (%s)\n", !dir ? "no MESA_GLSL_CACHE_DIR" : dis ? "MESA_GLSL_CACHE_DISABLE" : "LSWTCS_NVC0DISK=0");
      fflush(stdout);
      return;
   }
   snprintf(lsw_disk_path, sizeof(lsw_disk_path), "%s/lsw_nvc0.bin", dir);
   lsw_gm107_fixup_fns(lsw_disk_fns);
   _mesa_sha1_init(&ctx);
   _mesa_sha1_update(&ctx, kIdent, sizeof(kIdent));
   for (i = 0; i < sizeof(envs) / sizeof(envs[0]); ++i) {
      const char *v = getenv(envs[i]);
      _mesa_sha1_update(&ctx, envs[i], strlen(envs[i]) + 1);
      _mesa_sha1_update(&ctx, v ? v : "-", strlen(v ? v : "-") + 1);
   }
   sizes[0] = sizeof(struct nvc0_program);
   sizes[1] = (uint32_t)nv50_ir_fixup_entry_size();
   sizes[2] = (uint32_t)nv50_ir_reloc_entry_size();
   _mesa_sha1_update(&ctx, sizes, sizeof(sizes));
   _mesa_sha1_final(&ctx, lsw_disk_id);

   f = fopen(lsw_disk_path, "rb");
   if (f) {
      struct lsw_disk_head h;
      if (fread(&h, sizeof(h), 1, f) != 1 || h.magic != LSW_DISK_MAGIC) {
         why = "unreadable header";
      } else if (memcmp(h.id, lsw_disk_id, 20)) {
         why = "another Mesa or other codegen settings";
      } else {
         why = NULL;
         good = sizeof(h);
         for (;;) {
            struct lsw_disk_rec r;
            unsigned char *buf;
            size_t pay;
            size_t got = fread(&r, 1, sizeof(r), f);
            if (got == 0)
               break;
            if (got != sizeof(r) || r.bytes < sizeof(r) + sizeof(struct nvc0_program) || r.bytes > (16u << 20) ||
                r.bytes - sizeof(r) != sizeof(struct nvc0_program) + r.code_size + r.reloc_bytes + r.fixup_bytes) {
               cut = true;
               break;
            }
            pay = r.bytes - sizeof(r);
            buf = MALLOC(pay);
            if (!buf)
               break;
            if (fread(buf, 1, pay, f) != pay || lsw_disk_fnv(buf, pay) != r.sum) {
               FREE(buf);
               cut = true;
               break;
            }
            good += r.bytes;
            if (lsw_disk_add(&r, buf)) {
               ++lsw_disk_loaded;
               kb += r.bytes >> 10;
            } else {
               ++dropped;
            }
            FREE(buf);
         }
      }
      fclose(f);
   }
   if (why) {
      lsw_disk_out = lsw_disk_start_over();
   } else {
      if (cut) {   /* drop the cut record and what follows, so appends come after the last good one */
         FILE *t = fopen(lsw_disk_path, "r+b");
         if (!t || ftruncate(fileno(t), good) != 0) {
            why = "a cut record could not be dropped";
            lsw_disk_loaded = 0;   /* still in the table and good; only the file starts over */
         }
         if (t)
            fclose(t);
      }
      lsw_disk_out = why ? lsw_disk_start_over() : fopen(lsw_disk_path, "ab");
      if (lsw_disk_out && !why)
         setvbuf(lsw_disk_out, NULL, _IOFBF, 256 << 10);
   }
   printf("[NVC0DISK] %s: %u translations loaded (%u KB, %u not kept) in %.1f ms%s%s%s%s\n", lsw_disk_path, lsw_disk_loaded,
          kb, dropped, (os_time_get_nano() - t0) / 1e6, cut ? "; a cut record at the end dropped" : "",
          why ? "; started over (" : "", why ? why : "", why ? ")" : "");
   if (!lsw_disk_out)
      printf("[NVC0DISK] cannot write %s: translations are not saved this session\n", lsw_disk_path);
   fflush(stdout);
}

/* A translation made this session, appended to the file (outside the table's lock). */
static void
lsw_disk_write(const struct lsw_dedup_entry *e)
{
   const struct nvc0_program *src = &e->snap;
   const size_t rb = src->relocs ? nv50_ir_reloc_bytes(src->relocs) : 0;
   const size_t fb = src->fixups ? nv50_ir_fixup_bytes(src->fixups) : 0;
   const size_t pay = sizeof(struct nvc0_program) + src->code_size + rb + fb;
   struct lsw_disk_rec r;
   struct nvc0_program *c;
   unsigned char *buf;

   if (!lsw_disk_out)
      return;
   buf = MALLOC(pay);
   if (!buf)
      return;
   c = (struct nvc0_program *)buf;
   memcpy(c, src, sizeof(*c));
   memset(&c->pipe, 0, sizeof(c->pipe));
   c->code = NULL; c->relocs = NULL; c->fixups = NULL; c->tfb = NULL; c->mem = NULL; c->cp.syms = NULL;
   if (src->code_size)
      memcpy(buf + sizeof(*c), src->code, src->code_size);
   if (rb)
      memcpy(buf + sizeof(*c) + src->code_size, src->relocs, rb);
   if (fb) {
      unsigned char *fx = buf + sizeof(*c) + src->code_size + rb;
      memcpy(fx, src->fixups, fb);
      if (!nv50_ir_fixup_to_ids(fx, lsw_disk_fns, 2)) {   /* a fixup function of another emitter: not saved */
         FREE(buf);
         return;
      }
   }
   memset(&r, 0, sizeof(r));
   r.bytes = (uint32_t)(sizeof(r) + pay);
   r.sum = lsw_disk_fnv(buf, pay);
   memcpy(r.sha, e->sha, 20);
   r.type = e->type;
   r.num_ucps = e->num_ucps;
   r.chipset = e->chipset;
   r.code_size = src->code_size;
   r.reloc_bytes = (uint32_t)rb;
   r.fixup_bytes = (uint32_t)fb;
   pthread_mutex_lock(&lsw_disk_lock);
   if (lsw_disk_out && ftell(lsw_disk_out) + (long)r.bytes <= LSW_DISK_MAX_BYTES &&
       fwrite(&r, sizeof(r), 1, lsw_disk_out) == 1 && fwrite(buf, 1, pay, lsw_disk_out) == pay) {
      ++lsw_disk_written;   /* reaches the card when the 256 KB buffer fills (log117; a flush per record cost run 119) */
   }
   pthread_mutex_unlock(&lsw_disk_lock);
   FREE(buf);
}

/* A reuse of a loaded translation: counted once per entry; true while the first ones are still to be checked. */
static bool
lsw_disk_reuse(struct lsw_dedup_entry *e)
{
   bool check = false;
   if (!e->from_disk)
      return false;
   pthread_mutex_lock(&lsw_disk_lock);
   if (!e->used) {
      e->used = 1;
      ++lsw_disk_reused;
   }
   if (lsw_disk_checks_left) {
      --lsw_disk_checks_left;
      check = true;
   }
   pthread_mutex_unlock(&lsw_disk_lock);
   return check;
}

/* A loaded translation differed from a fresh one: none of the loaded ones is used again, and the file starts over. */
static void
lsw_disk_check_result(bool same)
{
   unsigned b;
   struct lsw_dedup_entry *e;
   pthread_mutex_lock(&lsw_disk_lock);
   ++lsw_disk_checked;
   if (!same)
      ++lsw_disk_differed;
   pthread_mutex_unlock(&lsw_disk_lock);
   if (same)
      return;
   pthread_mutex_lock(&lsw_dedup_lock);
   for (b = 0; b < LSW_DEDUP_BUCKETS; ++b)
      for (e = lsw_dedup_table[b]; e; e = e->next)
         if (e->from_disk)
            e->stale = 1;
   pthread_mutex_unlock(&lsw_dedup_lock);
   pthread_mutex_lock(&lsw_disk_lock);
   if (lsw_disk_out)
      fclose(lsw_disk_out);
   lsw_disk_out = lsw_disk_start_over();
   pthread_mutex_unlock(&lsw_disk_lock);
   printf("[NVC0DISK] a loaded translation differs from a fresh one: the loaded ones are dropped, the file starts over\n");
   fflush(stdout);
}

bool
nvc0_program_translate(struct nvc0_program *prog, uint16_t chipset,'''

MATCH = 'if (e->type == prog->type && e->num_ucps == prog->vp.num_ucps && e->chipset == chipset && !memcmp(e->sha, sha, 20))'

edit('src/gallium/drivers/nouveau/nvc0/nvc0_program.c', [
    ('#include "util/mesa-sha1.h"\n', '#include "util/mesa-sha1.h"\n#include "util/os_time.h"\n#include <string.h>\n#include <unistd.h>\n', 1),
    ('   uint16_t chipset;\n   struct nvc0_program snap;',
     '   uint16_t chipset;\n   uint8_t from_disk, stale, used;   /* mesa_nvc0disk_edits.py */\n   struct nvc0_program snap;', 1),
    ('\nbool\nnvc0_program_translate(struct nvc0_program *prog, uint16_t chipset,', DISK_C, 1),
    ('   struct lsw_dedup_entry *e;\n   bool ok = false;\n',
     '   struct lsw_dedup_entry *e, *added = NULL;\n   bool ok = false;\n', 1),
    ('   _mesa_sha1_compute(prog->pipe.tokens,',
     '   pthread_once(&lsw_disk_once, lsw_disk_load);\n   _mesa_sha1_compute(prog->pipe.tokens,', 1),
    (MATCH, MATCH.replace('!memcmp(e->sha, sha, 20))', '!memcmp(e->sha, sha, 20) && !e->stale)'), 2),
    ('      if (check_every > 0 && (n_hit % (unsigned)check_every) == 0) {',
     '      if (lsw_disk_reuse(e) || (check_every > 0 && (n_hit % (unsigned)check_every) == 0)) {', 1),
    ('            const bool same = diff == 0;\n',
     '            const bool same = diff == 0;\n            if (e->from_disk)\n               lsw_disk_check_result(same);\n', 1),
    ('            lsw_dedup_table[bucket] = e;\n            lsw_dedup_bytes += bytes;\n',
     '            lsw_dedup_table[bucket] = e;\n            lsw_dedup_bytes += bytes;\n            added = e;\n', 1),
    ('   pthread_mutex_unlock(&lsw_dedup_lock);\n   return true;\n}\n\nuint32_t\nnvc0_program_symbol_offset(',
     '   pthread_mutex_unlock(&lsw_dedup_lock);\n   if (added)\n      lsw_disk_write(added);\n   return true;\n}\n\n'
     'uint32_t\nnvc0_program_symbol_offset(', 1),
])
print('mesa_nvc0disk_edits.py applied')

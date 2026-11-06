"""Exact speed changes to the generated output's ppc_context.h (log91).

1. Float compares (fcmpu/fcmpo into a CR field, 11,660 sites). The generator's PPCCRRegister::compare(double, double)
   computes `un = isnan(l) || isnan(r)` and then `!un && l < r` and so on. devkitA64 GCC turns that into up to four
   compares and three branches: fcmp l,l; b.vs; fcmp r,r; b.vs; then a signalling fcmpe l,r for `<` and `>`, and another
   fcmp for `==`. In run 91's console profile, compares were 9-15 % of the samples inside the main thread's top functions
   (sub_8231ED30, sub_8249DB20). The quiet forms below (isunordered, isless, isgreater, ==) give the same four bits for
   every input, NaNs and signed zeros included (tests/cr_compare_check.py checks this exhaustively over special values and
   randomly over the rest), and compile to one fcmp and four csets. The host's FP exception flags are the only thing that
   differs (fcmpe raises Invalid on a quiet NaN); nothing reads them: the guest's FPSCR is emulated from the rounding bits
   only (PPCFPSCRRegister::loadFromHost).

2. --fold-fast: guest addresses without the region table. Default since run 93 (152 ns per guest call against 158;
   first built as a separate B NRO from a copy of the output).
   PPC_HOST was ppc_view_base[x >> 29] + x: before every guest load and store, a load from the table (lsr #29, then
   ldr [table, idx, lsl #3]), so each guest access is two dependent loads on the A57's single load pipe. In run 91's
   console profile 16.6 % of the main thread's samples in generated code sat on the access right after such a table
   load, and 3.6 % one instruction later (the loads are 3.9 % of the code). The table folds the 0xA0000000 and 0xC0000000
   physical views onto 0x80000000 and is identity elsewhere. The fast form: if bit 31 is set, clear bits 30-29 (tst, and,
   csel). It differs from the table only in region 0xE0000000, which it also folds onto 0x80000000; that region is mapped
   only with the heap bypass on (lsw_guest_layout.h, "small-alloc pool"), so main.cpp refuses to start such a build with
   LSWTCS_HEAPBYPASS on. With the bypass off (env.txt), an access there faults today, so no running guest code reaches it.

Like workspace-tools/fill_tcs360_movie_ops.py, this edits the generated output in place; tools/generated_output.py check
reports ppc_context.h as changed while it is applied. The original text is kept in a comment so --revert can restore it.
Usage: python workspace-tools/tune_tcs360_context.py [--output-dir DIR] [--check | --revert]
"""
from pathlib import Path
import argparse
import sys

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUT = ROOT / "source" / "Convert 360" / "LSWTCS" / "output"

OLD = """    inline void compare(double left, double right) noexcept
    {
        un = __builtin_isnan(left) || __builtin_isnan(right);
        lt = !un && (left < right);
        gt = !un && (left > right);
        eq = !un && (left == right);
    }
"""

NEW = """    inline void compare(double left, double right) noexcept
    {
        // tune_tcs360_context.py (log91): quiet compares, one fcmp; the same bits as the generator's form, which was:
        //   un = __builtin_isnan(left) || __builtin_isnan(right);
        //   lt = !un && (left < right); gt = !un && (left > right); eq = !un && (left == right);
        un = __builtin_isunordered(left, right);
        lt = __builtin_isless(left, right);
        gt = __builtin_isgreater(left, right);
        eq = left == right;
    }
"""


HOST_OLD = """#if defined(LSW_ADDR_FOLD_CMOV) && LSW_ADDR_FOLD_CMOV
#define PPC_HOST(x) (base + ppc_fold((uint32_t)(x)))
#else
#define PPC_HOST(x) (ppc_view_base[(uint32_t)(x) >> 29] + (uint32_t)(x))
#endif
"""

HOST_NEW = """// tune_tcs360_context.py --fold-fast (log91 B): no region table. Bit 31 set: clear bits 30-29, so 0xA0000000 and
// 0xC0000000 fold onto 0x80000000 as the table does, and 0xE0000000 too (mapped only with the heap bypass, which main.cpp
// refuses in this build). The table form was: ppc_view_base[(uint32_t)(x) >> 29] + (uint32_t)(x).
#define LSW_ADDR_FOLD_FAST 1
static inline __attribute__((always_inline)) uint32_t lsw_fold_fast(uint32_t a) { return (int32_t)a < 0 ? (a & 0x9FFFFFFFu) : a; }
#define PPC_HOST(x) (base + lsw_fold_fast((uint32_t)(x)))
"""


VREV_ANCHOR = "};\n\ninline uint8_t VectorShiftTableL[] =\n"
VREV_HELPER = """};

// tune_tcs360_context.py --vec-rev (log92): the guest vector byte reversal as a shuffle by a constant. The generated form,
// simde_mm_shuffle_epi8(x, simde_mm_load_si128((simde__m128i*)VectorMaskL)), reloads the mask from the non-const table at
// every site (stores may alias it), ANDs it with 0x8F, then TBL; the constant {15..0} is the table's first row, so the
// result is the same 16 bytes, and GCC keeps the mask in a register: one TBL. Other compilers keep the generated form.
static inline __attribute__((always_inline)) simde__m128i lsw_vrev16(simde__m128i a)
{
#if defined(__GNUC__) && !defined(__clang__)
    typedef unsigned char lsw_v16u8 __attribute__((vector_size(16)));
    lsw_v16u8 v;
    __builtin_memcpy(&v, &a, 16);
    v = __builtin_shuffle(v, (lsw_v16u8){15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0});
    simde__m128i r;
    __builtin_memcpy(&r, &v, 16);
    return r;
#else
    return simde_mm_shuffle_epi8(a, simde_mm_load_si128((simde__m128i*)VectorMaskL));
#endif
}

inline uint8_t VectorShiftTableL[] =
"""
VREV_MASK_ARG = ", simde_mm_load_si128((simde__m128i*)VectorMaskL))"
VREV_CALL = "simde_mm_shuffle_epi8("


def call_end(text, start):
    """Index just past the ')' closing the call whose '(' is at text[start - 1]."""
    depth = 1
    i = start
    while depth:
        c = text[i]
        if c == "(": depth += 1
        elif c == ")": depth -= 1
        i += 1
    return i


def vec_rev_source(text, revert):
    out, n, pos = [], 0, 0
    call = "lsw_vrev16(" if revert else VREV_CALL
    while True:
        i = text.find(call, pos)
        if i < 0: break
        j = call_end(text, i + len(call))
        whole = text[i:j]
        if revert:
            out.append(text[pos:i] + VREV_CALL + whole[len(call):-1] + VREV_MASK_ARG); n += 1
        elif whole.endswith(VREV_MASK_ARG):
            out.append(text[pos:i] + "lsw_vrev16(" + whole[len(call):-len(VREV_MASK_ARG)] + ")"); n += 1
        else:
            out.append(text[pos:j])
        pos = j
    out.append(text[pos:])
    return "".join(out), n


def vec_rev(a):
    hdr = a.output_dir / "ppc_context.h"
    raw = hdr.read_bytes(); crlf = b"\r\n" in raw
    h = raw.decode("utf-8").replace("\r\n", "\n")
    has = h.count(VREV_HELPER)
    total = 0
    for p in sorted(a.output_dir.glob("ppc_recomp.*.cpp")):
        with open(p, encoding="utf-8", errors="surrogateescape", newline="") as f: t = f.read()
        if a.check:
            total += t.count("lsw_vrev16(")
            continue
        new, n = vec_rev_source(t, a.revert)
        if n:
            p.write_text(new, encoding="utf-8", errors="surrogateescape", newline="")
            total += n
    if a.check:
        print(f"vector reversal: helper {'present' if has else 'absent'}, {total} lsw_vrev16 sites"); return
    if a.revert and has: h = h.replace(VREV_HELPER, VREV_ANCHOR)
    if not a.revert and not has:
        if h.count(VREV_ANCHOR) != 1: sys.exit("ppc_context.h: VectorShiftTableL anchor not found once")
        h = h.replace(VREV_ANCHOR, VREV_HELPER)
    hdr.write_bytes((h.replace("\n", "\r\n") if crlf else h).encode("utf-8"))
    print(f"vector reversal: {'restored' if a.revert else 'constant shuffle'} at {total} sites")


def fold_fast(a, text):
    n_old, n_new = text.count(HOST_OLD), text.count(HOST_NEW)
    if n_old + n_new != 1:
        sys.exit(f"expected exactly one PPC_HOST block, found {n_old} original and {n_new} fast")
    if a.check: print(f"PPC_HOST: {'fast fold' if n_new else 'region table'}"); return None
    if a.revert: return text.replace(HOST_NEW, HOST_OLD) if n_new else None
    return text.replace(HOST_OLD, HOST_NEW) if n_old else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--output-dir", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--fold-fast", action="store_true", help="item 2 instead of item 1")
    ap.add_argument("--vec-rev", action="store_true", help="item 3: vector byte reversal by a constant shuffle (all .cpp)")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--check", action="store_true", help="report whether the change is applied; change nothing")
    g.add_argument("--revert", action="store_true", help="put the generator's form back")
    a = ap.parse_args()
    if a.vec_rev:
        vec_rev(a)
        return
    path = a.output_dir / "ppc_context.h"
    raw = path.read_bytes()
    crlf = b"\r\n" in raw
    text = raw.decode("utf-8").replace("\r\n", "\n")
    if a.fold_fast:
        out = fold_fast(a, text)
        if out is not None:
            path.write_bytes((out.replace("\n", "\r\n") if crlf else out).encode("utf-8"))
            print(f"{path}: PPC_HOST {'restored' if a.revert else 'fast fold'}")
        return
    has_old, has_new = text.count(OLD), text.count(NEW)
    if has_old + has_new != 1:
        sys.exit(f"{path}: expected exactly one compare(double, double), found {has_old} original and {has_new} tuned")
    if a.check:
        print(f"{path.name}: float compare {'tuned' if has_new else 'original'}")
        return
    if a.revert:
        if has_old: print("already original"); return
        text = text.replace(NEW, OLD)
    else:
        if has_new: print("already tuned"); return
        text = text.replace(OLD, NEW)
    out = text.replace("\n", "\r\n") if crlf else text
    path.write_bytes(out.encode("utf-8"))
    print(f"{path.name}: float compare {'restored' if a.revert else 'tuned'}")


if __name__ == "__main__":
    main()

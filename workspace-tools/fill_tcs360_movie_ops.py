"""Fill PowerPC/VMX operations that the generator left empty in the movie decoder.

XenonRecomp has no case for some VMX operations (vslh, vsrah, vsubshs, vpkswss,
vctuxs and others): it writes the assembly comment and no code, so the operation
does nothing. patch_tcs360_movie_vmx.py filled twelve WMV decoder functions.
Run 86 (log83) played the title-idle movie for the first time past its first
frame and showed green/magenta blocks: the decoder still runs through other
functions with empty operations (motion-compensation variants in 0x82D9-0x82DF,
the colour output in 0x8288B6D0, the sound output in 0x82866770).

This fills every empty operation of the listed kinds in:
  - functions in the movie libraries' address ranges, 0x82814000-0x829A0000 and
    0x82D90000-0x82E00000 (run 84's gameplay profile and run 86's menu profile
    show no game thread in those functions outside 0x8281xxxx system calls,
    which have none of these gaps);
  - functions the movie threads reach outside them (EXTRA, from run 86's
    profile and the direct-call closure of the three movie thread entries).

Conditional traps (twllei, tdlgei, ...) stay empty on purpose.
The assembly comments stay as anchors; each inserted line ends in "// fill".
No game bytes are included. --revert removes the inserted lines.

Vector registers are stored with their bytes reversed (element 0 at the high
end), so element-wise operations work lane by lane and two-source packs take
their sources in the opposite order, as the generator's own vpkshus does.
"""
from pathlib import Path
import argparse
import re
import sys

RANGES = [(0x82814000, 0x829A0000), (0x82D90000, 0x82E00000)]
# log127: four more reached through function pointers (no direct caller), each with 8 empty eqv in a branchless median of
# three (sign masks), likely the motion-vector predictor: found by scanning for empty operations after run 121's movie dumps
# showed the decoded Y plane as residue only.
EXTRA = {"82E043C0", "82E0B540", "82D7E1A8", "82E03038", "82E05D90", "82E09D58"}
TAG = " // fill"
TRAPS = {"twllei", "twlgei", "tdllei", "tdlgei", "twi", "tdi", "tw", "td"}
KNOWN = {"vslh", "vsrh", "vsrah", "vrlh", "vsrab", "vsubshs", "vaddshs", "vsubsbs", "vaddsbs", "vsububm", "vsubuhs",
         "vmaxsh", "vminsh", "vmaxuh", "vminuh", "vavguh", "vcmpgtsh", "vcmpequh", "vcmpgtsw", "vandc",
         "vpkswss", "vpkshss", "vpkswus", "vpkuhus", "vaddsws"}


class Fn:
    def __init__(self, body):
        self.decl = set(re.findall(r"\tPPC(?:V|CR)?Register (\w+)\{\};", body))
        self.decl |= set(re.findall(r"\tPPCCRRegister (\w+)\{\};", body))
        self.new = []

    def reg(self, r):
        if r in self.decl:
            return r
        kind, n = r[0], int(r[1:]) if r[1:].isdigit() else -1
        if (kind == "v" and n >= 14) or (kind == "r" and n >= 14 and n != 13):
            # A non-volatile register used only by omitted operations: a fresh
            # local, as the generator makes for the other non-volatile registers.
            self.decl.add(r)
            self.new.append(r)
            return r
        return f"ctx.{r}"


def L(v):
    return f"simde_mm_load_si128((simde__m128i*){v}.u8)"


def S(d, expr):
    return [f"simde_mm_store_si128((simde__m128i*){d}.u8, {expr});"]


def op_code(fn, op, args):
    rec = op.endswith(".")
    op = op.rstrip(".")
    a = [x.strip() for x in args]
    if op in ("eqv", "mulhd"):
        d, x, y = (fn.reg(r) for r in a)
        if op == "eqv":
            out = [f"{d}.u64 = ~({x}.u64 ^ {y}.u64);"]
        else:
            out = [f"{d}.s64 = int64_t((__int128({x}.s64) * __int128({y}.s64)) >> 64);"]
        if rec:
            return None
        return out
    if op == "vspltish":
        d = fn.reg(a[0])
        imm = ((int(a[1]) & 31) ^ 16) - 16
        out = S(d, f"simde_mm_set1_epi16(short({imm}))")
    elif op == "vctuxs":
        d, b, u = fn.reg(a[0]), fn.reg(a[1]), int(a[2])
        out = ["{ uint32_t t_[4];",
               f"for (int i_ = 0; i_ < 4; i_++) {{ const float x_ = {b}.f32[i_] * {float(1 << u)}f; "
               "t_[i_] = !(x_ > 0.0f) ? 0u : (x_ >= 4294967296.0f ? 0xFFFFFFFFu : uint32_t(x_)); }",
               f"for (int i_ = 0; i_ < 4; i_++) {d}.u32[i_] = t_[i_]; }}"]
    else:
        if op not in KNOWN or len(a) != 3:
            return None
        d, x, y = (fn.reg(r) for r in a[:3])
        lane16 = {"vslh": ("u16", "uint16_t({x}.u16[{i}] << ({y}.u16[{i}] & 15))"),
                  "vsrh": ("u16", "uint16_t({x}.u16[{i}] >> ({y}.u16[{i}] & 15))"),
                  "vsrah": ("s16", "int16_t({x}.s16[{i}] >> ({y}.u16[{i}] & 15))"),
                  "vrlh": ("u16", "uint16_t(({x}.u16[{i}] << ({y}.u16[{i}] & 15)) | ({x}.u16[{i}] >> ((16 - ({y}.u16[{i}] & 15)) & 15)))")}
        simple = {"vsubshs": "simde_mm_subs_epi16({x}, {y})", "vaddshs": "simde_mm_adds_epi16({x}, {y})",
                  "vsubsbs": "simde_mm_subs_epi8({x}, {y})", "vaddsbs": "simde_mm_adds_epi8({x}, {y})",
                  "vsububm": "simde_mm_sub_epi8({x}, {y})", "vsubuhs": "simde_mm_subs_epu16({x}, {y})",
                  "vmaxsh": "simde_mm_max_epi16({x}, {y})", "vminsh": "simde_mm_min_epi16({x}, {y})",
                  "vmaxuh": "simde_mm_max_epu16({x}, {y})", "vminuh": "simde_mm_min_epu16({x}, {y})",
                  "vavguh": "simde_mm_avg_epu16({x}, {y})",
                  "vcmpgtsh": "simde_mm_cmpgt_epi16({x}, {y})", "vcmpequh": "simde_mm_cmpeq_epi16({x}, {y})",
                  "vcmpgtsw": "simde_mm_cmpgt_epi32({x}, {y})",
                  "vaddsws": "simde_mm_vaddsws({x}, {y})",
                  "vandc": "simde_mm_andnot_si128({y}, {x})",
                  # packs: host lanes 0-3 hold PPC elements 7-4, which come from the second source
                  "vpkswss": "simde_mm_packs_epi32({y}, {x})", "vpkshss": "simde_mm_packs_epi16({y}, {x})",
                  "vpkswus": "simde_mm_packus_epi32({y}, {x})",
                  "vpkuhus": "simde_mm_packus_epi16(simde_mm_min_epu16({y}, simde_mm_set1_epi16(0xFF)), simde_mm_min_epu16({x}, simde_mm_set1_epi16(0xFF)))"}
        if op in lane16:
            kind, e = lane16[op]
            # into a temporary first: the destination may also be a source
            out = ["{ PPCVRegister t_;"] + [f"t_.{kind}[{i}] = " + e.format(x=x, y=y, i=i) + ";" for i in range(8)] + \
                  [f"{d} = t_; }}"]
        elif op == "vsrab":
            out = ["{ PPCVRegister t_;"] + [f"t_.s8[{i}] = int8_t({x}.s8[{i}] >> ({y}.u8[{i}] & 7));" for i in range(16)] + [f"{d} = t_; }}"]
        elif op == "vaddsws":
            out = ["{ PPCVRegister t_;"] + [f"{{ const int64_t s_ = int64_t({x}.s32[{i}]) + int64_t({y}.s32[{i}]); "
                                            f"t_.s32[{i}] = int32_t(s_ > INT32_MAX ? INT32_MAX : s_ < INT32_MIN ? INT32_MIN : s_); }}" for i in range(4)] + \
                  [f"{d} = t_; }}"]
        elif op in simple:
            out = S(d, simple[op].format(x=L(x), y=L(y)))
        else:
            return None
    if rec:
        if not op.startswith("vcmp"):
            return None
        out.append(f"{fn.reg('cr6')}.setFromMask({L(d)}, 0xFFFF);")
    return out


def selected(addr):
    v = int(addr, 16)
    return addr in EXTRA or any(lo <= v < hi for lo, hi in RANGES)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("output", type=Path)
    p.add_argument("--revert", action="store_true")
    args = p.parse_args()
    total = funcs = 0
    unhandled = {}
    for path in sorted(args.output.glob("ppc_recomp.*.cpp")):
        text = path.read_text(encoding="utf-8")
        if args.revert:
            new = "".join(l for l in text.splitlines(keepends=True) if not l.rstrip("\n").endswith(TAG))
            if new != text:
                path.write_text(new, encoding="utf-8", newline="\n")
            continue
        parts = re.split(r"(?=^PPC_FUNC_IMPL\(__imp__sub_[0-9A-F]{8}\) \{$)", text, flags=re.M)
        changed = False
        for k, part in enumerate(parts):
            m = re.match(r"PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]{8})\)", part)
            if not m or not selected(m[1]):
                continue
            fn = Fn(part)
            lines = part.split("\n")
            n = 0
            for i in range(len(lines) - 2, -1, -1):
                c = re.fullmatch(r"\t// (\w+\.?) +([^\n]+)", lines[i])
                if not c:
                    continue
                nxt = lines[i + 1]
                if nxt.endswith(TAG):
                    continue   # filled already
                if not (nxt.startswith("\t// ") or nxt.startswith("loc_") or nxt == "}"):
                    continue
                op = c[1]
                if op.rstrip(".") in TRAPS or op.startswith(("b", "nop", "sync", "lwsync", "eieio", "isync", "dcb", "icbi", "db16", "cctp", ".long")):
                    continue
                code = op_code(fn, op, c[2].split(","))
                if code is None:
                    unhandled[op] = unhandled.get(op, 0) + 1
                    continue
                lines[i + 1:i + 1] = ["\t" + s + TAG for s in code]
                n += 1
            if n:
                body = "\n".join(lines)
                if fn.new:
                    decl = "".join(f"\n\tPPC{'V' if r[0] == 'v' else ''}Register {r}{{}};{TAG}" for r in sorted(fn.new))
                    body = body.replace("\tPPC_FUNC_PROLOGUE();", "\tPPC_FUNC_PROLOGUE();" + decl, 1)
                parts[k] = body
                total += n
                funcs += 1
                changed = True
        if changed:
            path.write_text("".join(parts), encoding="utf-8", newline="\n")
    if args.revert:
        print("reverted")
        return
    print(f"filled {total} operations in {funcs} functions")
    if unhandled:
        print("left empty (no implementation here):", ", ".join(f"{k} x{v}" for k, v in sorted(unhandled.items())))


if __name__ == "__main__":
    sys.exit(main())

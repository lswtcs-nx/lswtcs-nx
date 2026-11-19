"""Check the generator's own C++ for the VMX operations the movie decoder uses against PowerPC semantics, on the PC.

tests/fill_ops_check.py checks the operations workspace-tools/fill_tcs360_movie_ops.py adds; this one checks those
XenonRecomp emitted itself. For each operation it takes one emitted site from the generated output (a site with three
distinct registers), renames its registers to v20/v21/v22/v23, builds a host program with clang, runs it on random and
edge inputs, and compares with a reference written in PowerPC element order (element 0 = most significant).

log127: written after the title-idle movie's decoded Y plane (LSWTCS_TEXPNG) showed blocks whose rows are each one value
(the horizontal detail of the inverse transform lost), with the empty-operation fills already in place.
Usage: python tests/vmx_gen_ops_check.py --compiler <clang++> --output-dir "source/Convert 360/LSWTCS/output_nv"
       [--ops vmrghh,vperm,...] [--keep DIR]
"""
import argparse, os, pathlib, random, re, struct, subprocess, sys, tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
FUNC = re.compile(r'^PPC_FUNC_IMPL\(__imp__sub_[0-9A-F]{8}\)')

# op -> (operand kinds, reference). Kinds: 'v' vector register, 'i' immediate. The first operand is the destination.
def H(x, s): return list(struct.unpack(">8h" if s else ">8H", x))
def W(x, s): return list(struct.unpack(">4i" if s else ">4I", x))
def Bt(x, s): return list(struct.unpack(">16b" if s else ">16B", x))
def F(x): return list(struct.unpack(">4f", x))
def sat(v, lo, hi): return max(lo, min(hi, v))
def ph(v, s=True): return struct.pack(">8h" if s else ">8H", *v)
def pw(v, s=True): return struct.pack(">4i" if s else ">4I", *v)
def pb(v, s=False): return struct.pack(">16b" if s else ">16B", *v)
def f32(v): return struct.unpack(">f", struct.pack(">f", v))[0]

def vctsxs(a, u):
    out = []
    for x in F(a):
        if x != x: out.append(0); continue
        x = x * (1 << u)
        out.append(sat(int(x) if abs(x) < 2.0**40 else (2**31 if x > 0 else -2**31 - 1), -2**31, 2**31 - 1))
    return pw(out)

REF = {
    "vupkhsh": ("vv", lambda a, b, c, i: pw(H(a, 1)[0:4])),
    "vupklsh": ("vv", lambda a, b, c, i: pw(H(a, 1)[4:8])),
    "vupkhsb": ("vv", lambda a, b, c, i: ph(Bt(a, 1)[0:8])),
    "vupklsb": ("vv", lambda a, b, c, i: ph(Bt(a, 1)[8:16])),
    "vmrghb": ("vvv", lambda a, b, c, i: pb([v for k in range(8) for v in (Bt(a, 0)[k], Bt(b, 0)[k])])),
    "vmrglb": ("vvv", lambda a, b, c, i: pb([v for k in range(8, 16) for v in (Bt(a, 0)[k], Bt(b, 0)[k])])),
    "vmrghh": ("vvv", lambda a, b, c, i: ph([v for k in range(4) for v in (H(a, 0)[k], H(b, 0)[k])], False)),
    "vmrglh": ("vvv", lambda a, b, c, i: ph([v for k in range(4, 8) for v in (H(a, 0)[k], H(b, 0)[k])], False)),
    "vmrghw": ("vvv", lambda a, b, c, i: pw([v for k in range(2) for v in (W(a, 0)[k], W(b, 0)[k])], False)),
    "vmrglw": ("vvv", lambda a, b, c, i: pw([v for k in range(2, 4) for v in (W(a, 0)[k], W(b, 0)[k])], False)),
    "vperm": ("vvvv", lambda a, b, c, i: bytes((a + b)[x & 31] for x in c)),
    "vsldoi": ("vvvi", lambda a, b, c, i: (a + b)[i:i + 16]),
    "vpkshus": ("vvv", lambda a, b, c, i: pb([sat(v, 0, 255) for v in H(a, 1) + H(b, 1)])),
    "vpkswss": ("vvv", lambda a, b, c, i: ph([sat(v, -32768, 32767) for v in W(a, 1) + W(b, 1)])),
    "vadduhm": ("vvv", lambda a, b, c, i: ph([(p + q) & 0xFFFF for p, q in zip(H(a, 0), H(b, 0))], False)),
    "vsubuhm": ("vvv", lambda a, b, c, i: ph([(p - q) & 0xFFFF for p, q in zip(H(a, 0), H(b, 0))], False)),
    "vaddubm": ("vvv", lambda a, b, c, i: pb([(p + q) & 0xFF for p, q in zip(Bt(a, 0), Bt(b, 0))])),
    "vaddubs": ("vvv", lambda a, b, c, i: pb([min(255, p + q) for p, q in zip(Bt(a, 0), Bt(b, 0))])),
    "vaddshs": ("vvv", lambda a, b, c, i: ph([sat(p + q, -32768, 32767) for p, q in zip(H(a, 1), H(b, 1))])),
    "vsubshs": ("vvv", lambda a, b, c, i: ph([sat(p - q, -32768, 32767) for p, q in zip(H(a, 1), H(b, 1))])),
    "vavgub": ("vvv", lambda a, b, c, i: pb([(p + q + 1) >> 1 for p, q in zip(Bt(a, 0), Bt(b, 0))])),
    "vavgsh": ("vvv", lambda a, b, c, i: ph([(p + q + 1) >> 1 for p, q in zip(H(a, 1), H(b, 1))])),
    "vcmpgtuh": ("vvv", lambda a, b, c, i: ph([0xFFFF if p > q else 0 for p, q in zip(H(a, 0), H(b, 0))], False)),
    "vcmpgtsh": ("vvv", lambda a, b, c, i: ph([0xFFFF if p > q else 0 for p, q in zip(H(a, 1), H(b, 1))], False)),
    "vcmpequh": ("vvv", lambda a, b, c, i: ph([0xFFFF if p == q else 0 for p, q in zip(H(a, 0), H(b, 0))], False)),
    "vcmpgtub": ("vvv", lambda a, b, c, i: pb([0xFF if p > q else 0 for p, q in zip(Bt(a, 0), Bt(b, 0))])),
    "vcmpequb": ("vvv", lambda a, b, c, i: pb([0xFF if p == q else 0 for p, q in zip(Bt(a, 0), Bt(b, 0))])),
    "vmaxsh": ("vvv", lambda a, b, c, i: ph([max(p, q) for p, q in zip(H(a, 1), H(b, 1))])),
    "vminsh": ("vvv", lambda a, b, c, i: ph([min(p, q) for p, q in zip(H(a, 1), H(b, 1))])),
    "vsel": ("vvvv", lambda a, b, c, i: bytes((p & ~r | q & r) & 0xFF for p, q, r in zip(a, b, c))),
    "vsplth": ("vvi", lambda a, b, c, i: ph([H(a, 0)[i & 7]] * 8, False)),
    "vspltb": ("vvi", lambda a, b, c, i: pb([Bt(a, 0)[i & 15]] * 16)),
    "vspltw": ("vvi", lambda a, b, c, i: pw([W(a, 0)[i & 3]] * 4, False)),
    "vslw": ("vvv", lambda a, b, c, i: pw([(p << (q & 31)) & 0xFFFFFFFF for p, q in zip(W(a, 0), W(b, 0))], False)),
    "vsraw": ("vvv", lambda a, b, c, i: pw([p >> (q & 31) for p, q in zip(W(a, 1), W(b, 0))])),
    "vslb": ("vvv", lambda a, b, c, i: pb([(p << (q & 7)) & 0xFF for p, q in zip(Bt(a, 0), Bt(b, 0))])),
    "vslh": ("vvv", lambda a, b, c, i: ph([(p << (q & 15)) & 0xFFFF for p, q in zip(H(a, 0), H(b, 0))], False)),
    "vsrah": ("vvv", lambda a, b, c, i: ph([p >> (q & 15) for p, q in zip(H(a, 1), H(b, 0))])),
    "vor": ("vvv", lambda a, b, c, i: bytes(p | q for p, q in zip(a, b))),
    "vand": ("vvv", lambda a, b, c, i: bytes(p & q for p, q in zip(a, b))),
    "vxor": ("vvv", lambda a, b, c, i: bytes(p ^ q for p, q in zip(a, b))),
    "vandc": ("vvv", lambda a, b, c, i: bytes(p & ~q & 0xFF for p, q in zip(a, b))),
    "vsubsws": ("vvv", lambda a, b, c, i: pw([sat(p - q, -2**31, 2**31 - 1) for p, q in zip(W(a, 1), W(b, 1))])),
    "vaddsws": ("vvv", lambda a, b, c, i: pw([sat(p + q, -2**31, 2**31 - 1) for p, q in zip(W(a, 1), W(b, 1))])),
    "vcfsx": ("vvi", lambda a, b, c, i: struct.pack(">4f", *[f32(float(x) / (1 << i)) for x in W(a, 1)])),
    "vctsxs": ("vvi", lambda a, b, c, i: vctsxs(a, i)),
}
# Float inputs (vctsxs) are built as floats so the reference is not about NaN payloads or denormal flushing.
FLOAT_IN = {"vctsxs"}

def find_site(outdir, op, kinds):
    """First emitted site of `op` whose register operands are distinct plain vN registers, as (operands, code lines)."""
    pat = re.compile(r"^\t// " + op + r" (\S.*)$")
    for f in sorted(outdir.glob("ppc_recomp.*.cpp")):
        lines = f.read_text(errors="replace").splitlines()
        for k, line in enumerate(lines):
            m = pat.match(line)
            if not m:
                continue
            ops = [x.strip() for x in m.group(1).split(",")]
            if len(ops) != len(kinds):
                continue
            regs = [o for o, t in zip(ops, kinds) if t == "v"]
            if not all(re.fullmatch(r"v\d+", r) for r in regs) or len(set(regs)) != len(regs):
                continue
            code = []
            for nxt in lines[k + 1:]:
                if nxt.startswith("\t//") or nxt.startswith("loc_") or nxt.startswith("}"):
                    break
                code.append(nxt)
            if code and not any("// fill" in c for c in code):
                return ops, code
    return None

def rename(code, ops, kinds):
    names = ["v20", "v21", "v22", "v23"]
    mapping = {}
    k = 0
    for o, t in zip(ops, kinds):
        if t == "v":
            mapping[o] = names[k]; k += 1
    tmp = {}
    out = []
    for line in code:
        for j, (old, new) in enumerate(mapping.items()):
            line = re.sub(r"\bctx\." + old + r"\b", f"@@{j}@@", line)
            line = re.sub(r"(?<![\w.])" + old + r"\b", f"@@{j}@@", line)
        for j, (old, new) in enumerate(mapping.items()):
            line = line.replace(f"@@{j}@@", new)
        out.append(line)
    return out

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--compiler", required=True)
    ap.add_argument("--output-dir", required=True, type=pathlib.Path)
    ap.add_argument("--ops", help="comma-separated subset")
    ap.add_argument("--keep", type=pathlib.Path)
    ap.add_argument("--inputs", type=int, default=400)
    args = ap.parse_args()
    ops = [o for o in (args.ops.split(",") if args.ops else REF)]
    sites = []
    for op in ops:
        kinds = REF[op][0]
        s = find_site(args.output_dir, op, kinds)
        if not s:
            print(f"{op}: no plain site found, skipped")
            continue
        sites.append((op, kinds, s[0], rename(s[1], s[0], kinds)))
    prog = ['#include "ppc_context.h"', "#include <cstdio>", "#include <cstring>",
            "#ifdef _WIN32", "#include <fcntl.h>", "#include <io.h>", "#endif",
            "static void to_host(PPCVRegister& v, const uint8_t* be) { for (int i = 0; i < 16; i++) v.u8[i] = be[15 - i]; }",
            "static void from_host(uint8_t* be, const PPCVRegister& v) { for (int i = 0; i < 16; i++) be[i] = v.u8[15 - i]; }",
            "int main() { uint8_t a[16], b[16], c[16], o[16];",
            "#ifdef _WIN32", "_setmode(_fileno(stdin), _O_BINARY); _setmode(_fileno(stdout), _O_BINARY);", "#endif",
            "while (fread(a, 1, 16, stdin) == 16 && fread(b, 1, 16, stdin) == 16 && fread(c, 1, 16, stdin) == 16) {"]
    for op, kinds, opnds, code in sites:
        prog.append("{ PPCContext ctx{}; PPCVRegister v20{}, v21{}, v22{}, v23{}; PPCRegister r0{}, temp{}; uint32_t ea{}; (void)ctx; (void)r0; (void)temp; (void)ea;")
        prog.append("ctx.fpscr.csr = 0x1F80; ctx.fpscr.disableFlushMode();")   # as the runtime starts each thread (exceptions masked)
        prog.append("to_host(v21, a); to_host(v22, b); to_host(v23, c);")
        prog.extend(code)
        prog.append("from_host(o, v20); fwrite(o, 1, 16, stdout); }")
    prog.append("} return 0; }")
    tmp = args.keep or pathlib.Path(tempfile.mkdtemp())
    tmp.mkdir(parents=True, exist_ok=True)
    src = tmp / "vmx_gen_ops.cpp"; exe = tmp / ("vmx_gen_ops" + (".exe" if os.name == "nt" else ""))
    src.write_text("\n".join(prog))
    simde = ROOT / "source" / "simde"
    cc = [args.compiler, "-std=c++20", "-O1", "-w", "-msse4.1", f"-I{args.output_dir}", f"-I{ROOT / 'source' / 'LSWTCSRuntime' / 'include'}",
          f"-I{simde}", f"-I{simde / 'simde'}", str(src), "-o", str(exe)]
    r = subprocess.run(cc, capture_output=True, text=True)
    if r.returncode:
        print(r.stderr[-3000:]); sys.exit(2)
    rnd = random.Random(127)
    edge16 = [0, 1, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF]
    inputs = []
    for n in range(args.inputs):
        trip = []
        for _ in range(3):
            if n % 4 == 0:
                trip.append(struct.pack(">8H", *[rnd.choice(edge16) for _ in range(8)]))
            elif n % 4 == 1:
                trip.append(struct.pack(">4f", *[rnd.uniform(-70000, 70000) for _ in range(4)]))
            else:
                trip.append(bytes(rnd.randrange(256) for _ in range(16)))
        inputs.append(trip)
    data = b"".join(b"".join(t) for t in inputs)
    out = subprocess.run([str(exe)], input=data, capture_output=True).stdout
    per = 16 * len(sites)
    bad = {}
    for n, (A, B, C) in enumerate(inputs):
        for k, (op, kinds, opnds, code) in enumerate(sites):
            imm = int(opnds[-1], 0) if kinds[-1] == "i" else 0
            if op in FLOAT_IN and n % 4 != 1:
                continue
            want = REF[op][1](A, B, C, imm)
            got = out[n * per + k * 16: n * per + k * 16 + 16]
            if got != want and op not in bad:
                bad[op] = (A, B, C, imm, got, want)
    for op, kinds, opnds, code in sites:
        if op in bad:
            A, B, C, imm, got, want = bad[op]
            print(f"MISMATCH {op} (site {', '.join(opnds)}): a={A.hex()} b={B.hex()} c={C.hex()} imm={imm}\n   got  {got.hex()}\n   want {want.hex()}\n   code: {' | '.join(c.strip() for c in code)}")
        else:
            print(f"ok {op}")
    print(f"{len(sites)} operations x {len(inputs)} inputs: {len(bad)} mismatched")
    sys.exit(1 if bad else 0)

if __name__ == "__main__":
    main()

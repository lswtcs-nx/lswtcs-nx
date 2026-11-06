"""Check workspace-tools/fill_tcs360_movie_ops.py's emitted C++ against PowerPC semantics on the PC.

Builds a small host program from the exact lines the tool emits, runs every operation on random and edge
inputs, and compares with a reference written in PowerPC element order (element 0 = most significant).
Usage: python tests/fill_ops_check.py --compiler <clang++> --output-dir "source/Convert 360/LSWTCS/output"
"""
import argparse, importlib.util, pathlib, subprocess, sys, tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("fill", ROOT / "workspace-tools" / "fill_tcs360_movie_ops.py")
fill = importlib.util.module_from_spec(spec); spec.loader.exec_module(fill)

class FakeFn(fill.Fn):
    def __init__(self):
        self.decl = {"v20", "v21", "v22", "cr6"}; self.new = []

VOPS = sorted(fill.KNOWN)
cases = []
for op in VOPS:
    cases.append((op, ["v20", "v21", "v22"]))
    cases.append((op, ["v21", "v21", "v22"]))   # destination is a source
for op in ("vcmpgtsh.", "vcmpequh.", "vcmpgtsw."):
    cases.append((op, ["v20", "v21", "v22"]))
for imm in ("0", "4", "15", "16", "31"):
    cases.append(("vspltish", ["v20", imm]))
for u in ("0", "8"):
    cases.append(("vctuxs", ["v20", "v21", u]))

def body(op, args):
    code = fill.op_code(FakeFn(), op, args)
    assert code is not None, op
    return "\n".join("\t" + c for c in code)

def program():
    out = ['#include "ppc_context.h"', "#include <cstdio>", "#include <cstring>",
           "static void to_host(PPCVRegister& v, const uint8_t* be) { for (int i = 0; i < 16; i++) v.u8[i] = be[15 - i]; }",
           "static void from_host(uint8_t* be, const PPCVRegister& v) { for (int i = 0; i < 16; i++) be[i] = v.u8[15 - i]; }",
           "#ifdef _WIN32", "#include <fcntl.h>", "#include <io.h>", "#endif",
           "int main() { uint8_t a[16], b[16], c[16], o[16]; unsigned crl, cre;",
           "#ifdef _WIN32", "_setmode(_fileno(stdin), _O_BINARY); _setmode(_fileno(stdout), _O_BINARY);", "#endif",
           "while (fread(a, 1, 16, stdin) == 16 && fread(b, 1, 16, stdin) == 16 && fread(c, 1, 16, stdin) == 16) {"]
    for k, (op, args) in enumerate(cases):
        out.append("{ PPCContext ctx{}; PPCVRegister v20{}, v21{}, v22{}; PPCCRRegister cr6{}; (void)ctx;")
        out.append("to_host(v20, c); to_host(v21, a); to_host(v22, b);")
        out.append(body(op, args))
        out.append(f"from_host(o, {args[0]}); crl = cr6.lt; cre = cr6.eq; fwrite(o, 1, 16, stdout); fputc(crl, stdout); fputc(cre, stdout); }}")
    out.append("} return 0; }")
    return "\n".join(out)

def ref(op, args, A, B, C):
    """PowerPC reference on big-endian byte strings; returns (16 bytes, lt, eq) or None for cr."""
    import struct
    rec = op.endswith("."); op = op.rstrip(".")
    h = lambda x, s: list(struct.unpack(">8h" if s else ">8H", x))
    w = lambda x, s: list(struct.unpack(">4i" if s else ">4I", x))
    by = lambda x, s: list(struct.unpack(">16b" if s else ">16B", x))
    sat = lambda v, lo, hi: max(lo, min(hi, v))
    if op == "vspltish":
        imm = int(args[1]) & 31; imm = imm - 32 if imm & 16 else imm
        return struct.pack(">8h", *[imm] * 8), 0, 0
    if op == "vctuxs":
        f = struct.unpack(">4f", A); r = []
        for x in f:
            x = x * (1 << int(args[2]))
            r.append(0 if not (x > 0) else 0xFFFFFFFF if x >= 4294967296.0 else int(x))
        return struct.pack(">4I", *r), 0, 0
    x, y = (A, B) if args[1] != args[0] else (A, B)
    if op == "vslh": r = struct.pack(">8H", *[(p << (q & 15)) & 0xFFFF for p, q in zip(h(x, 0), h(y, 0))])
    elif op == "vsrh": r = struct.pack(">8H", *[p >> (q & 15) for p, q in zip(h(x, 0), h(y, 0))])
    elif op == "vsrah": r = struct.pack(">8h", *[p >> (q & 15) for p, q in zip(h(x, 1), h(y, 0))])
    elif op == "vrlh": r = struct.pack(">8H", *[((p << (q & 15)) | (p >> (16 - (q & 15)))) & 0xFFFF for p, q in zip(h(x, 0), h(y, 0))])
    elif op == "vsrab": r = struct.pack(">16b", *[p >> (q & 7) for p, q in zip(by(x, 1), by(y, 0))])
    elif op == "vsubshs": r = struct.pack(">8h", *[sat(p - q, -32768, 32767) for p, q in zip(h(x, 1), h(y, 1))])
    elif op == "vaddshs": r = struct.pack(">8h", *[sat(p + q, -32768, 32767) for p, q in zip(h(x, 1), h(y, 1))])
    elif op == "vsubsbs": r = struct.pack(">16b", *[sat(p - q, -128, 127) for p, q in zip(by(x, 1), by(y, 1))])
    elif op == "vaddsbs": r = struct.pack(">16b", *[sat(p + q, -128, 127) for p, q in zip(by(x, 1), by(y, 1))])
    elif op == "vsububm": r = struct.pack(">16B", *[(p - q) & 0xFF for p, q in zip(by(x, 0), by(y, 0))])
    elif op == "vsubuhs": r = struct.pack(">8H", *[max(0, p - q) for p, q in zip(h(x, 0), h(y, 0))])
    elif op == "vmaxsh": r = struct.pack(">8h", *[max(p, q) for p, q in zip(h(x, 1), h(y, 1))])
    elif op == "vminsh": r = struct.pack(">8h", *[min(p, q) for p, q in zip(h(x, 1), h(y, 1))])
    elif op == "vmaxuh": r = struct.pack(">8H", *[max(p, q) for p, q in zip(h(x, 0), h(y, 0))])
    elif op == "vminuh": r = struct.pack(">8H", *[min(p, q) for p, q in zip(h(x, 0), h(y, 0))])
    elif op == "vavguh": r = struct.pack(">8H", *[(p + q + 1) >> 1 for p, q in zip(h(x, 0), h(y, 0))])
    elif op == "vcmpgtsh": r = struct.pack(">8H", *[0xFFFF if p > q else 0 for p, q in zip(h(x, 1), h(y, 1))])
    elif op == "vcmpequh": r = struct.pack(">8H", *[0xFFFF if p == q else 0 for p, q in zip(h(x, 0), h(y, 0))])
    elif op == "vcmpgtsw": r = struct.pack(">4I", *[0xFFFFFFFF if p > q else 0 for p, q in zip(w(x, 1), w(y, 1))])
    elif op == "vandc": r = bytes(p & ~q & 0xFF for p, q in zip(x, y))
    elif op == "vaddsws": r = struct.pack(">4i", *[sat(p + q, -2**31, 2**31 - 1) for p, q in zip(w(x, 1), w(y, 1))])
    elif op == "vpkswss": r = struct.pack(">8h", *[sat(v, -32768, 32767) for v in w(x, 1) + w(y, 1)])
    elif op == "vpkshss": r = struct.pack(">16b", *[sat(v, -128, 127) for v in h(x, 1) + h(y, 1)])
    elif op == "vpkswus": r = struct.pack(">8H", *[sat(v, 0, 65535) for v in w(x, 1) + w(y, 1)])
    elif op == "vpkuhus": r = struct.pack(">16B", *[min(v, 255) for v in h(x, 0) + h(y, 0)])
    else: raise KeyError(op)
    if rec:
        all1 = all(b == 0xFF for b in r); none = all(b == 0 for b in r)
        return r, int(all1), int(none)
    return r, None, None

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--compiler", required=True); ap.add_argument("--output-dir", required=True)
    a = ap.parse_args()
    import random, struct
    rnd = random.Random(86)
    inputs = []
    edges = [b"\x00" * 16, b"\xff" * 16, bytes(range(16)), bytes([0x7F, 0xFF, 0x80, 0x00] * 4), bytes([0x80, 0x00, 0x00, 0x01] * 4)]
    for _ in range(400):
        inputs.append([rnd.randbytes(16) for _ in range(3)])
    for e in edges:
        for f in edges:
            inputs.append([e, f, rnd.randbytes(16)])
    fl = [struct.pack(">4f", *[rnd.choice([-1.5, 0.0, 0.4, 1.0, 254.6, 255.0, 65535.9, 3e9, 5e9, float("nan"), 1e-40]) for _ in range(4)]) for _ in range(60)]
    for f in fl:
        inputs.append([f, rnd.randbytes(16), rnd.randbytes(16)])
    with tempfile.TemporaryDirectory() as d:
        src = pathlib.Path(d) / "t.cpp"; exe = pathlib.Path(d) / "t.exe"
        src.write_text(program())
        simde = ROOT / "source" / "simde"
        cmd = [a.compiler, "-std=c++20", "-O1", "-w", "-msse4.1", f"-I{a.output_dir}", f"-I{ROOT / 'source' / 'LSWTCSRuntime' / 'include'}", f"-I{simde}", f"-I{simde / 'simde'}", str(src), "-o", str(exe)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            print(r.stderr[:3000]); return 1
        data = b"".join(b"".join(t) for t in inputs)
        out = subprocess.run([str(exe)], input=data, capture_output=True).stdout
    per = 18; bad = {}
    pos = 0
    for A, B, C in inputs:
        for op, args in cases:
            got = out[pos:pos + 16]; lt, eq = out[pos + 16], out[pos + 17]; pos += per
            src_a = A; src_b = B
            want, wl, we = ref(op, args, src_a, src_b, C)
            if got != want or (wl is not None and (lt, eq) != (wl, we)):
                bad.setdefault(op, (args, A.hex(), B.hex(), want.hex(), got.hex(), (wl, we), (lt, eq)))
    if pos != len(out):
        print("output length", len(out), "expected", pos); return 1
    print(f"{len(cases)} cases x {len(inputs)} inputs")
    for k, v in bad.items(): print("MISMATCH", k, v)
    print("ok" if not bad else "FAILED")
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main())

"""Fill omitted VMX operations in reached WMV decoder routines and direct callees.

This narrow patch is intentionally separate from a whole-game opcode expansion.
The original assembly comments remain the anchors; no game bytes are included.
"""
from pathlib import Path
import argparse
import re

OPS = {
    "vandc": "andnot", "vcmpgtsh": "cmpgt_epi16", "vcmpgtsw": "cmpgt_epi32",
    "vmaxsh": "max_epi16", "vminsh": "min_epi16", "vsubshs": "subs_epi16",
    "vslh": "left", "vsrah": "right", "vspltish": "splat", "vaddsbs": "adds_epi8", "vcmpequh":"cmpeq_epi16",
}


def operation(op, args):
    record = op.endswith(".")
    op = op.rstrip(".")
    regs = [f"ctx.{a}" if a.startswith("v") and int(a[1:]) < 14 else a for a in args]
    d = regs[0]
    load = lambda v: f"simde_mm_load_si128((simde__m128i*){v}.u8)"
    if op == "vspltish":
        imm = (int(args[1]) & 31) ^ 16
        expr = f"simde_mm_set1_epi16({imm - 16})"
    elif op in ("vslh", "vsrah"):
        kind, shift = ("u", "<<") if op == "vslh" else ("s", ">>")
        return "".join(f"\t{d}.{kind}16[{i}] = {regs[1]}.{kind}16[{i}] {shift} ({regs[2]}.u16[{i}] & 15);\n" for i in range(8))
    else:
        a, b = regs[1:]
        if op == "vandc":
            a, b = b, a
            intrinsic = "andnot_si128"
        else:
            intrinsic = OPS[op]
        expr = f"simde_mm_{intrinsic}({load(a)}, {load(b)})"
    code = f"\tsimde_mm_store_si128((simde__m128i*){d}.u8, {expr});\n"
    if record:
        code += f"\tcr6.setFromMask({load(d)}, 0xFFFF);\n"
    return code


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("output", type=Path)
    args = p.parse_args()
    targets = {516: {"82947210":448,"829450E8":292,"829466E8":147,"82948DE0":226,"82944B98":25}, 512:{"828E02F0":6},
               517:{"8296CE78":9,"8296D260":14,"8296D758":37,"8296E290":68,"8296F7D8":79,"82970028":75}}
    updates = {}
    for file_id, functions in targets.items():
        path = args.output / f"ppc_recomp.{file_id}.cpp"
        text = path.read_text(encoding="utf-8")
        for function, expected_count in functions.items():
            start = text.index(f"PPC_FUNC_IMPL(__imp__sub_{function}) {{")
            end = text.index('\n__attribute__((alias(', start)
            body = text[start:end]
            lines = body.splitlines(keepends=True)
            count = 0
            for i in range(len(lines) - 1, -1, -1):
                m = re.fullmatch(r"\t// (\w+\.?)[ ]+([^\n]+)\n", lines[i])
                if not m or m[1].rstrip(".") not in OPS:
                    continue
                code = operation(m[1], m[2].split(","))
                existing = "".join(lines[i + 1:i + 1 + code.count('\n')])
                if existing == code:
                    count += 1
                    continue
                assert i + 1 == len(lines) or lines[i + 1].startswith(("\t// ", "loc_")), (function, i, lines[i + 1])
                lines.insert(i + 1, code)
                count += 1
            assert count == expected_count, (function, count, expected_count)
            body = "".join(lines)
            if function == "82948DE0" and "PPCVRegister v14{};" not in body:
                # v14 was used exclusively by omitted operations. Its first use
                # is a splat definition, so this is a temporary, not a live-in.
                first = re.search(r"\t// [^\n]*\bv14\b[^\n]*", body)
                assert first and first[0] == "\t// vspltish v14,1"
                body = body.replace("\tPPC_FUNC_PROLOGUE();", "\tPPC_FUNC_PROLOGUE();\n\tPPCVRegister v14{};", 1)
            for v in set(re.findall(r"\bv(\d+)\.", body)):
                if int(v) >= 14:
                    assert f"PPCVRegister v{v}{{}};" in body, (function, v)
            text = text[:start] + body + text[end:]
        updates[path] = text
    for path, text in updates.items():
        if path.read_text(encoding="utf-8") != text:
            path.write_text(text, encoding="utf-8", newline="\n")
    print("WMV decoder: 1426 omitted VMX operations implemented in twelve functions")


if __name__ == "__main__":
    main()

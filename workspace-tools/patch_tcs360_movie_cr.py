"""Repair four omitted CR logical operations in the owned Rev 1 WMV decoder.

Run after the existing generated-output patch. Does not regenerate game code or
contain game data. Rejects unexpected source rather than guessing replacements.
"""
from pathlib import Path
import argparse

p = argparse.ArgumentParser()
p.add_argument("output", type=Path)
args = p.parse_args()
path = args.output / "ppc_recomp.2794.cpp"
text = path.read_text(encoding="utf-8")
for name, relation in [("82D82A58", "lt"), ("82D82E18", "gt")]:
    begin = text.index(f"PPC_FUNC_IMPL(__imp__sub_{name}) {{")
    end = text.index('\n__attribute__((alias(', begin)
    body = text[begin:end]
    if "\tPPCCRRegister cr1{};" not in body:
        assert body.count("\tPPCCRRegister cr0{};") == 1
        body = body.replace("\tPPCCRRegister cr0{};", "\tPPCCRRegister cr0{};\n\tPPCCRRegister cr1{};")
    for old, new in [
        (f"\t// cror 4*cr1+eq,{relation},4*cr6+eq\n", f"\tcr1.eq = cr0.{relation} || cr6.eq;\n"),
        ("\t// crorc eq,4*cr1+eq,4*cr5+lt\n", "\tcr0.eq = cr1.eq || !cr5.lt;\n"),
    ]:
        assert body.count(old) == 1, (name, old)
        if old + new not in body:
            body = body.replace(old, old + new)
    text = text[:begin] + body + text[end:]
path.write_text(text, encoding="utf-8", newline="\n")
print("WMV decoder: four CR operations implemented in two functions")

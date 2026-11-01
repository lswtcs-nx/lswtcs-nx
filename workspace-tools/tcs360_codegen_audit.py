"""Read-only inventory of a TCS 360 generation attempt. Does not validate gameplay."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re


def inventory(root):
    files = sorted(p for p in root.iterdir() if p.is_file())
    tree = hashlib.sha256()
    traps = 0
    size = 0
    for path in files:
        data = path.read_bytes()
        size += len(data)
        digest = hashlib.sha256(data).hexdigest()
        tree.update((path.name + "\0" + digest + "\n").encode())
        if path.suffix == ".cpp":
            traps += data.count(b"__builtin_debugtrap();")
    return {"files": len(files), "cpp_files": sum(p.suffix == ".cpp" for p in files),
            "bytes": size, "tree_sha256": tree.hexdigest(),
            "explicit_debugtrap_calls": traps}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--folded", type=Path, required=True)
    parser.add_argument("--patched", type=Path, required=True)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--patch", type=Path, required=True)
    args = parser.parse_args()
    log = args.log.read_text(encoding="utf-8", errors="replace")
    unsupported = [(int(a, 16), op) for a, op in re.findall(
        r"Unrecognized instruction at (0x[0-9A-Fa-f]+): ([^\r\n]+)", log)]
    patch = args.patch.read_bytes().replace(b"\r\n", b"\n")
    result = {
        "scope": "generation inventory only; no compilation or execution implied",
        "folded_baseline": inventory(args.folded),
        "patched": inventory(args.patched),
        "patch_lf_sha256": hashlib.sha256(patch).hexdigest(),
        "patch_file_sections": len(re.findall(rb"^diff ", patch, re.M)),
        "patch_hunks": len(re.findall(rb"^@@ ", patch, re.M)),
        "generator_reached_100_percent": "Recompiling functions... 100%" in log,
        "unsupported_total": len(unsupported),
        "unsupported_unique_address_opcode_pairs": len(set(unsupported)),
        "unsupported_by_opcode": dict(sorted(Counter(op for _, op in unsupported).items())),
        "unsupported_in_main_text": sum(0x8213E600 <= a < 0x82D5162C for a, _ in unsupported),
        "unsupported_in_embedded_code": sum(0x82D51800 <= a < 0x82E70000 for a, _ in unsupported),
        "undecodable_diagnostics": log.count("Unable to decode instruction"),
        "missing_switch_table_diagnostics": log.count("with no switch table entry present"),
        "warning": "Unsupported instructions need a reachability/correctness audit; trap count is not opcode coverage.",
    }
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()

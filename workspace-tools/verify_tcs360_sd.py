"""Read-only verification of a staged or copied SD package against its SHA-256 manifest."""
import argparse
import hashlib
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sd-root", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    args = parser.parse_args()
    root = args.sd_root.resolve(strict=True)
    inventory = json.loads(args.manifest.read_text(encoding="utf-8"))
    checked = 0
    for item in inventory["files"]:
        path = (root / item["path"]).resolve()
        if not path.is_relative_to(root):
            raise ValueError("Manifest entry escapes the chosen SD root")
        if not path.is_file() or path.stat().st_size != item["bytes"]:
            raise ValueError(f"Missing file or wrong size: {item['path']}")
        with path.open("rb") as stream:
            actual = hashlib.file_digest(stream, "sha256").hexdigest()
        if actual != item["sha256"]:
            raise ValueError(f"Hash mismatch: {item['path']}")
        checked += 1
    print(json.dumps({"verified_files": checked, "verified_bytes":
                      sum(item["bytes"] for item in inventory["files"]),
                      "scope": "package bytes match manifest; not a hardware execution test"}, indent=2))


if __name__ == "__main__":
    main()

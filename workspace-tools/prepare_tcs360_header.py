"""Extract the owner's XEX header for native module metadata; refuses overwrite."""
import argparse
import hashlib
from pathlib import Path
import struct

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--xex", required=True, type=Path)
parser.add_argument("--output", required=True, type=Path)
args = parser.parse_args()
with args.xex.open("rb") as stream:
    start = stream.read(24)
    if start[:4] != b"XEX2":
        raise ValueError("Expected XEX2")
    size = struct.unpack_from(">I", start, 8)[0]
    if not 24 <= size <= 0xFE00:
        raise ValueError("Unsupported header size")
    stream.seek(0)
    header = stream.read(size)
if len(header) != size:
    raise ValueError("Truncated XEX header")
with args.output.open("xb") as target:
    target.write(header)
print(f"{size} bytes; SHA-256 {hashlib.sha256(header).hexdigest()}")

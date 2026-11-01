"""Validate the full ARM64 ELF, NRO segments, embedded NACP and Mesa exports.

Uses libnx include/switch/nro.h layouts. These checks establish artifact integrity,
not execution or frame rate on Switch hardware.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def validate(build, nm):
    elf, nro = build / "lswtcs.elf", build / "lswtcs.nro"
    with elf.open("rb") as stream:
        header = stream.read(64)
    require(header[:6] == b"\x7fELF\x02\x01", "Expected little-endian ELF64")
    require(struct.unpack_from("<H", header, 18)[0] == 183, "ELF must target AArch64")
    phoff = struct.unpack_from("<Q", header, 32)[0]
    phsize, phnum = struct.unpack_from("<HH", header, 54)
    loads = []
    with elf.open("rb") as stream:
        for index in range(phnum):
            stream.seek(phoff + index * phsize)
            ptype, flags, offset, va, _, filesz, memsz, align = struct.unpack("<II6Q", stream.read(56))
            if ptype == 1:
                require(offset + filesz <= elf.stat().st_size and filesz <= memsz,
                        "ELF load segment outside file or larger than memory")
                loads.append({"virtual_address": va, "file_bytes": filesz,
                              "memory_bytes": memsz, "flags": flags, "alignment": align})
    require(loads and any(p["flags"] & 1 for p in loads), "ELF has no executable load segment")
    size = nro.stat().st_size
    with nro.open("rb") as stream:
        header = stream.read(0x80)
        require(header[0x10:0x14] == b"NRO0", "Missing NRO0 header")
        image_size = struct.unpack_from("<I", header, 0x18)[0]
        require(0x80 <= image_size <= size, "Invalid NRO image size")
        segments = []
        end = 0
        for index, name in enumerate(("text", "rodata", "data")):
            offset, length = struct.unpack_from("<II", header, 0x20 + index * 8)
            require(offset % 0x1000 == 0 and length % 0x1000 == 0,
                    f"NRO {name} must be page aligned")
            require(offset >= end and offset + length <= image_size,
                    f"NRO {name} overlaps or exceeds image")
            segments.append({"name": name, "offset": offset, "bytes": length})
            end = offset + length
        require(end == image_size, "NRO segment end does not match image size")
        stream.seek(image_size)
        assets = stream.read(0x38)
        require(assets[:4] == b"ASET" and struct.unpack_from("<I", assets, 4)[0] == 0,
                "Expected version-zero NRO asset header")
        for index, name in enumerate(("icon", "nacp", "romfs")):
            offset, length = struct.unpack_from("<QQ", assets, 8 + index * 16)
            require(not length or (offset >= 0x38 and image_size + offset + length <= size),
                    f"Embedded {name} exceeds NRO")
        nacp_offset, nacp_size = struct.unpack_from("<QQ", assets, 0x18)
        require(nacp_size == 0x4000, "Expected complete 16 KiB NACP")
        stream.seek(image_size + nacp_offset)
        nacp = stream.read(nacp_size)
    require(nacp == (build / "lswtcs.nacp").read_bytes(), "Embedded NACP differs from generated NACP")
    name = nacp[:0x200].split(b"\0")[0].decode("utf-8")
    version = nacp[0x3060:0x3070].split(b"\0")[0].decode("utf-8")
    require(name == "LSWTCS", "Unexpected application name")
    symbols = subprocess.run([str(nm), "--defined-only", str(elf)],
                             capture_output=True, text=True, check=True).stdout
    defined = {line.split()[-1] for line in symbols.splitlines() if line.split()}
    required = {"lsw_glthread_call", "lsw_egl_swap_buffers_worker", "lsw_glthread_sync_hook"}
    require(required <= defined, "Missing patched Mesa definitions")
    link_command = (build / "CMakeFiles/lswtcs.dir/link.txt").read_text()
    require("--unresolved-symbols=ignore-all" not in link_command,
            "Full game target must not ignore unresolved symbols")
    require("mesa/build/install/" in link_command.replace("\\", "/"),
            "Expected project-local modified Mesa on link command")
    return {"scope": "static artifact validation; hardware execution pending",
            "elf": {"bytes": elf.stat().st_size, "sha256": digest(elf),
                    "machine": "AArch64", "load_segments": loads},
            "nro": {"bytes": size, "sha256": digest(nro), "image_bytes": image_size,
                    "bss_bytes": struct.unpack_from("<I", header, 0x38)[0],
                    "build_id": header[0x40:0x60].hex(), "segments": segments},
            "nacp": {"name": name, "version": version, "bytes": nacp_size},
            "patched_mesa_symbols": sorted(required), "strict_link": True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--nm", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    result = validate(args.build_dir, args.nm)
    args.output.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()

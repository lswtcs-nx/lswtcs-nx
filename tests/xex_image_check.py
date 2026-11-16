"""Checks the runtime's XEX image builder (log107) against the reference files the port was developed on.

Needs your own default.xex, image.bin and pristine.bin, and a host clang++ (tiny-AES-c comes from the pinned XenonRecomp
checkout). Builds tests/xex_image_check.cpp with source/LSWTCSRuntime/source/lsw_xex_image.cpp, runs it on default.xex,
and checks that the image it makes gives the guest the same memory at boot as image.bin did:
  - outside .data and the 13 kernel-variable records (bound again from the header at boot) it equals image.bin;
  - in .data it equals pristine.bin (what LSWTCS_PRISTINE=1 put there);
  - the header it keeps equals xex_header.bin.
Prints the SHA-256 of the built image, which the runtime logs as [XEX] image sha256.

Usage: python tests/xex_image_check.py --xex default.xex --data-dir <folder with image.bin, pristine.bin, xex_header.bin>
"""
import argparse
import hashlib
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASE = 0x82000000
DATA_LO, DATA_HI = 0x82E70000, 0x82E70000 + 0x41491C          # main.cpp, LSWTCS_PRISTINE overlay
KERNEL_VARIABLE_SLOTS = {0x83290A3C, 0x83290A6C, 0x83290A78, 0x83290A88, 0x83290AF0, 0x83290B04, 0x83290B4C,
                         0x83290B58, 0x83290B90, 0x83290BC8, 0x83290BD8, 0x83290BDC, 0x83290BF4}   # lsw_kernel bindings


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--xex", required=True, type=Path)
    ap.add_argument("--data-dir", required=True, type=Path)
    ap.add_argument("--clang", default=os.environ.get("CLANGXX", r"C:\Program Files\LLVM\bin\clang++.exe"))
    args = ap.parse_args()

    aes = ROOT / "XenonRecomp" / "thirdparty" / "tiny-AES-c"
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        exe = tmp / "xex_image_check.exe"
        defines = ["-DAES128=1", "-DCBC=1", "-DECB=0", "-DCTR=0", "-D_CRT_SECURE_NO_WARNINGS"]
        aes_obj = tmp / "aes.o"
        subprocess.run([args.clang, "-x", "c", "-O2", *defines, "-c", str(aes / "aes.c"), "-o", str(aes_obj)], check=True)
        subprocess.run([args.clang, "-std=c++17", "-O2", *defines,
                        f"-I{ROOT / 'source/LSWTCSRuntime/include'}", f"-I{aes}",
                        str(ROOT / "tests/xex_image_check.cpp"), str(ROOT / "source/LSWTCSRuntime/source/lsw_xex_image.cpp"),
                        str(aes_obj), "-o", str(exe)], check=True)
        out_image, out_header = tmp / "image.out", tmp / "header.out"
        subprocess.run([str(exe), str(args.xex), str(out_image), str(out_header)], check=True)
        built, header = out_image.read_bytes(), out_header.read_bytes()

    image = (args.data_dir / "image.bin").read_bytes()
    pristine = (args.data_dir / "pristine.bin").read_bytes()
    ref_header = (args.data_dir / "xex_header.bin").read_bytes()
    failures = []
    if len(built) != len(image):
        failures.append(f"size 0x{len(built):X} against image.bin 0x{len(image):X}")
    outside = [BASE + k for k in range(0, min(len(built), len(image)), 4)
               if built[k:k + 4] != image[k:k + 4] and not DATA_LO <= BASE + k < DATA_HI
               and BASE + k not in KERNEL_VARIABLE_SLOTS]
    if outside:
        failures.append(f"{len(outside)} words differ from image.bin outside .data, first at 0x{outside[0]:08X}")
    if built[DATA_LO - BASE:DATA_HI - BASE] != pristine[DATA_LO - BASE:DATA_HI - BASE]:
        failures.append(".data differs from pristine.bin")
    if header != ref_header:
        failures.append("header differs from xex_header.bin")
    print(f"built image sha256 {hashlib.sha256(built).hexdigest()}")
    if failures:
        print("FAIL: " + "; ".join(failures))
        return 1
    print("PASS: same guest memory at boot as image.bin with LSWTCS_PRISTINE=1; header equals xex_header.bin")
    return 0


if __name__ == "__main__":
    sys.exit(main())

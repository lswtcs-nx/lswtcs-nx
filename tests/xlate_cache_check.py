"""Checks the translation cache's file handling (source/switch/source/gl_xlate_cache.cc, log109) on a PC.

Builds tests/xlate_cache_check.cpp with gl_xlate_cache.cc using a host clang++ (xxHash from the pinned xenia-canary
checkout) and runs "sessions" in a temporary folder:
  - a first session saves two translations, the next one finds them with every field intact;
  - the first LSWTCS_GLXLATECHECK hits are flagged for checking; a check that differs turns the cache off for the
    session and starts the file over;
  - a record cut short at the end (the game closed from the HOME menu mid-write) is dropped, the others kept;
  - another build ID or another translation setting starts the file over; LSWTCS_GLXLATE=0 turns the cache off.

Usage: python tests/xlate_cache_check.py [--clang path\\to\\clang++.exe]
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path("glprogs2") / "xlate.bin"
HEADER = 16


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--clang", default=os.environ.get("CLANGXX", r"C:\Program Files\LLVM\bin\clang++.exe"))
    args = ap.parse_args()
    failures = []

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        shim = tmp / "posix_shim.h"
        shim.write_text("#pragma once\n#ifdef _WIN32\n#include <direct.h>\n#define mkdir(path, mode) _mkdir(path)\n#endif\n")
        exe = tmp / "xlate_cache_check.exe"
        subprocess.run([args.clang, "-std=c++17", "-O2", "-D__SWITCH__", "-D_CRT_SECURE_NO_WARNINGS", "-include", str(shim),
                        f"-I{ROOT / 'source/switch/include'}", f"-I{ROOT / 'source/LSWTCSRuntime/include'}",
                        f"-I{ROOT / 'source/xenia-canary'}",
                        str(ROOT / "tests/xlate_cache_check.cpp"), str(ROOT / "source/switch/source/gl_xlate_cache.cc"),
                        "-o", str(exe)], check=True)
        work = tmp / "sd"
        work.mkdir()
        cache = work / CACHE

        def session(build, mode, expect_log, **env):
            e = dict(os.environ, LSWTCS_GLXLATECHECK="2", **env)
            r = subprocess.run([str(exe), str(build), mode], cwd=work, env=e, capture_output=True, text=True)
            out = r.stdout.strip().replace("\n", " | ")
            ok = r.returncode == 0 and expect_log in r.stdout
            print(f"{'ok  ' if ok else 'FAIL'} {mode:4} build {build}: {out}")
            if not ok:
                failures.append(f"{mode} (expected '{expect_log}')")

        def size():
            return cache.stat().st_size if cache.exists() else -1

        session(1, "put", "started over (none yet)")
        saved = size()
        if saved <= HEADER:
            failures.append(f"nothing written after put ({saved} bytes)")
        session(1, "find", "2 translations")
        if size() != HEADER:
            failures.append(f"a differing check left {size()} bytes, not just the header")
        session(1, "put", "0 translations")
        with open(cache, "ab") as f:   # a record cut short: header says 1000 bytes, 100 follow
            f.write(struct.pack("<IIQ", 0x3152584C, 1000, 0) + b"x" * 100)
        session(1, "find", "a cut record at the end dropped")
        session(1, "put", "0 translations")
        session(1, "none", "another build or other translation settings", LSWTCS_GLFMZ="0")
        session(1, "put", "another build or other translation settings")
        session(2, "none", "another build or other translation settings")
        session(1, "none", "off (LSWTCS_GLXLATE=0)", LSWTCS_GLXLATE="0")

    if failures:
        print("FAIL: " + "; ".join(failures))
        return 1
    print("PASS: saved, found intact, checked, cut record dropped, started over on another build or setting")
    return 0


if __name__ == "__main__":
    sys.exit(main())

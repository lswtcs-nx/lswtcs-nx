"""Checks the scripted input (source/switch/source/lsw_input_script.cpp, LSWTCS_INPUTSCRIPT) on a PC.

Builds tests/input_script_check.cpp with the script player using a host clang++ and runs it against a simulated game:
a full route (level waits, light and play waits, presses, a repeat, a stick, a mark, a capture), presses slower than the
polls, a wait that times out and hands the pad back, a script without 'done', and parse errors.
Also parses every script in tests/scripts/ so a broken script is caught before it reaches a console or an emulator.

Usage: python tests/input_script_check.py [--clang path\\to\\clang++.exe]
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--clang", default=os.environ.get("CLANGXX", r"C:\Program Files\LLVM\bin\clang++.exe"))
    args = ap.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / "input_script_check.exe"
        subprocess.run([args.clang, "-std=c++17", "-O2", "-D_CRT_SECURE_NO_WARNINGS", f"-I{ROOT / 'source/switch/include'}",
                        str(ROOT / "tests/input_script_check.cpp"), str(ROOT / "source/switch/source/lsw_input_script.cpp"),
                        "-o", str(exe)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        print(r.stdout, end="")
        ok = r.returncode == 0
        scripts = sorted((ROOT / "tests/scripts").glob("*.txt"))
        if scripts:
            r2 = subprocess.run([str(exe), "--parse", *map(str, scripts)], capture_output=True, text=True)
            print(r2.stdout, end="")
            ok = ok and r2.returncode == 0
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

"""Packages a release from the built NRO: the SD-card zip, SHA256SUMS.txt and the release notes (docs/RELEASING.md).

Refuses an NRO whose NACP version is not the release version (build with -DLSW_RELEASE_VERSION=X.Y.Z). Writes to
private/releases/vX.Y.Z/ and prints the `gh release create --draft` command; it publishes nothing itself.

Usage: python tools/make_release.py --version 0.1.0 [--build-dir source/switch/build-nvpgo]
"""
import argparse
import hashlib
import re
import shutil
import struct
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

GAME_FILES_NOTE = """Copy these from your own Xbox 360 disc of LEGO Star Wars: The Complete Saga
(USA/Europe Rev 1 release) into this folder:

  default.xex
  game.DAT
  game1.DAT
  Movies/
  audio/

Then make a forwarder in sphaira for the app (CPU Cores: 4, Address Space: 39-bit) and start the game from it.

env-testing.txt turns on detailed logs for reporting a problem: rename it to env.txt, play, then rename it back.
"""

NOTES_TEMPLATE = """{changes}

## Install

1. Extract `lswtcs-v{version}.zip` to the root of your SD card.
2. Copy from your own disc of the game (USA/Europe Rev 1) into `sdmc:/lswtcs/`: `default.xex`, `game.DAT`,
   `game1.DAT`, `Movies/` and `audio/`.
3. In sphaira, highlight *LEGO Star Wars: The Complete Saga*, press **X**, choose **Install Forwarder**, set
   **CPU Cores** to **4** and **Address Space** to **39-bit**, then choose **Create Forwarder**.
4. Start the game from its icon on the HOME menu.

## Update

Replace `sdmc:/switch/lswtcs/lswtcs.nro` with the one in this release (or extract the zip again). Saves and the game
files are kept, and the forwarder starts the new version.

## Known issues

See [docs/known-issues.md](https://github.com/lswtcs-nx/lswtcs-nx/blob/v{version}/docs/known-issues.md).

No game files are included. You need your own copy of the game.
"""


def nacp_version(nro: bytes) -> str:
    nro_size = struct.unpack_from("<I", nro, 0x18)[0]
    aset = nro[nro_size:]
    if aset[:4] != b"ASET":
        raise SystemExit("the NRO has no asset section")
    nacp_off, nacp_size = struct.unpack_from("<QQ", aset, 0x18)
    nacp = aset[nacp_off:nacp_off + nacp_size]
    return nacp[0x3060:0x3070].split(b"\0")[0].decode()


def personal_paths(nro: bytes) -> list:
    """Absolute paths of this machine that a compiler left in the binary (__FILE__ and the like)."""
    found = []
    for base in {Path.home(), ROOT}:
        win = str(base).replace("\\", "/")                      # C:/Users/name/...
        posix = "/" + win[0].lower() + win[2:] if win[1:2] == ":" else win   # /c/Users/name/...
        for form in {win, posix, str(base)}:
            if form.encode() in nro:
                found.append(form)
    return found


def changelog_section(version: str) -> str:
    text = (ROOT / "CHANGELOG.md").read_text(encoding="utf-8")
    m = re.search(rf"^## v{re.escape(version)}\b.*?$(.*?)(?=^## |\Z)", text, re.M | re.S)
    if not m:
        raise SystemExit(f"CHANGELOG.md has no '## v{version}' section; move 'Unreleased' there first")
    return m.group(1).strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--version", required=True, help="X.Y.Z")
    ap.add_argument("--build-dir", default=str(ROOT / "source/switch/build-nvpgo"))
    args = ap.parse_args()
    if not re.fullmatch(r"\d+\.\d+\.\d+", args.version):
        raise SystemExit("version must be X.Y.Z")
    build = Path(args.build_dir)
    nro_path, elf_path = build / "lswtcs.nro", build / "lswtcs.elf"
    nro = nro_path.read_bytes()
    found = nacp_version(nro)
    if found != args.version:
        raise SystemExit(f"the NRO's version is {found!r}, not {args.version!r}: build with -DLSW_RELEASE_VERSION={args.version}")
    leaks = personal_paths(nro)
    if leaks:
        raise SystemExit(f"the NRO contains this machine's paths ({', '.join(sorted(set(leaks)))}): map them with "
                         "-ffile-prefix-map (CMakeLists.txt, LSW_PREFIX_MAP) and rebuild")
    changes = changelog_section(args.version)

    out = ROOT / "private" / "releases" / f"v{args.version}"
    out.mkdir(parents=True, exist_ok=True)
    zip_path = out / f"lswtcs-v{args.version}.zip"
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("switch/lswtcs/lswtcs.nro", nro)
        z.writestr("lswtcs/PUT-YOUR-GAME-FILES-HERE.txt", GAME_FILES_NOTE)
        z.write(ROOT / "profiles" / "hardware-ab.env", "lswtcs/env-testing.txt")
    shutil.copyfile(nro_path, out / "lswtcs.nro")
    if elf_path.exists():
        shutil.copyfile(elf_path, out / f"lswtcs-v{args.version}.elf")   # private: for crash reports, never uploaded
    sums = "".join(f"{hashlib.sha256((out / n).read_bytes()).hexdigest()}  {n}\n"
                   for n in (zip_path.name, "lswtcs.nro"))
    (out / "SHA256SUMS.txt").write_text(sums, encoding="utf-8", newline="\n")
    notes = out / "notes.md"
    notes.write_text(NOTES_TEMPLATE.format(changes=changes, version=args.version), encoding="utf-8", newline="\n")
    print(f"wrote {out}")
    print(sums, end="")
    print(f'gh release create v{args.version} --draft --title "v{args.version}" --notes-file "{notes}" '
          f'"{zip_path}" "{out / "lswtcs.nro"}" "{out / "SHA256SUMS.txt"}"')
    return 0


if __name__ == "__main__":
    sys.exit(main())

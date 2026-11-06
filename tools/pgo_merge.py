"""Turn the console's PGO count streams into .gcda files for the -DLSW_PGO=use build (log93).

The instrumented NRO writes sdmc:/lswtcs/pgo/lswtcs_<run>.stream (source/switch/source/lsw_pgo.cpp), one per launch. Copy those
files to a folder on the PC and run:

    python tools/pgo_merge.py <folder with the .stream files> [--gcov-tool <aarch64-none-elf-gcov-tool>]

Each stream goes through `gcov-tool merge-stream`, which writes (or adds to) the .gcda files at the names the instrumented build
recorded: LSW_PGO_DIR (private/pgo/profile by default). Streams are added together, so run it once per new set of launches, on an
empty or deliberately reused profile folder. Afterwards each .gcda's object summary is checked: sa2-nx's notes say the stream
path leaves runs = 0, from which GCC derives its hot and cold thresholds; such files are set to runs = <number of streams>.
The counts are derived from the game: keep them under private/.
"""
from pathlib import Path
import argparse, struct, subprocess, sys

GCDA_MAGIC = 0x67636461            # "gcda"
TAG_OBJECT_SUMMARY = 0xA1000000


def fix_summary(path, runs):
    data = bytearray(path.read_bytes())
    if len(data) < 16 or struct.unpack_from("<I", data, 0)[0] != GCDA_MAGIC:
        return "not a gcda"
    pos = 16                        # magic, version, stamp, checksum (GCC 12+)
    while pos + 8 <= len(data):
        tag, length = struct.unpack_from("<II", data, pos)
        if tag == TAG_OBJECT_SUMMARY:
            old_runs, sum_max = struct.unpack_from("<II", data, pos + 8)
            if old_runs == 0:
                struct.pack_into("<I", data, pos + 8, runs)
                path.write_bytes(bytes(data))
                return f"runs 0 -> {runs} (sum_max {sum_max})"
            return f"runs {old_runs} (sum_max {sum_max})"
        if tag == 0 or length > len(data):
            break
        pos += 8 + length           # GCC 12+: lengths in bytes
    return "no object summary"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("streams", type=Path)
    ap.add_argument("--gcov-tool", default=r"C:\devkitPro\msys2\opt\devkitpro\devkitA64\bin\aarch64-none-elf-gcov-tool.exe")
    ap.add_argument("--profile-dir", type=Path,
                    default=Path(__file__).resolve().parents[1] / "private" / "pgo" / "profile")
    a = ap.parse_args()
    streams = sorted(a.streams.glob("*.stream"))
    if not streams:
        sys.exit(f"no .stream files in {a.streams}")
    a.profile_dir.mkdir(parents=True, exist_ok=True)
    for s in streams:
        r = subprocess.run([a.gcov_tool, "merge-stream", str(s)], capture_output=True, text=True)
        print(f"{s.name}: {s.stat().st_size / 1048576:.1f} MB, merge-stream exit {r.returncode}")
        if r.returncode:
            print(r.stdout[-2000:], r.stderr[-2000:])
            sys.exit(1)
    gcdas = sorted(a.profile_dir.rglob("*.gcda"))
    results = {}
    for g in gcdas:
        results.setdefault(fix_summary(g, len(streams)).split(" (")[0], []).append(g.name)
    print(f"{len(gcdas)} .gcda files in {a.profile_dir}")
    for k, v in results.items():
        print(f"  {k}: {len(v)} files (e.g. {v[0]})")
    if not gcdas:
        sys.exit("no .gcda written where expected: check the paths the instrumented build recorded (gcov-dump -l)")


if __name__ == "__main__":
    main()

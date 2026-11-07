"""Turn the console's PGO count streams into .gcda files for the -DLSW_PGO=use build (log93; retargeting since run 96).

The instrumented NRO writes sdmc:/lswtcs/pgo/lswtcs_<run>.stream (source/switch/source/lsw_pgo.cpp), one per launch. Copy those
files to a folder on the PC and run:

    python tools/pgo_merge.py <folder with the .stream files> [--profile-dir DIR] [--gcov-tool <aarch64-none-elf-gcov-tool>]

Each stream holds, per object, a "gcfn" record with the .gcda name the instrumented build computed (its LSW_PGO_DIR, then
build-pgo's own path, then the object's mangled path) followed by that object's .gcda data. The names are rewritten to start
with --profile-dir (default private/pp, which must equal the use build's LSW_PGO_DIR), into a copy of the stream in
<profile-dir>/../pp-streams, which then goes through `gcov-tool merge-stream`. Retargeting matters on Windows: paths past 259
characters are not written (run 96: 9 of 657 objects under private/pgo/profile), and the compiler would not read them either.

Streams are added together, so run it once per new set of launches, on an empty or deliberately reused profile folder.
Afterwards each .gcda's object summary is fixed: the stream path leaves runs = 0 and sum_max = 0 (sa2-nx's notes name runs),
and GCC derives its hot threshold from sum_max. They become runs = <number of streams> and sum_max = the largest arc count of
all objects. The counts are derived from the game: keep them under private/.
"""
from pathlib import Path
import argparse, struct, subprocess, sys

GCDA_MAGIC = 0x67636461            # "gcda"
GCFN_MAGIC = 0x6763666E            # "gcfn": a file name record in a stream
TAG_OBJECT_SUMMARY = 0xA1000000
TAG_ARC_COUNTERS = 0x01A10000
MAX_PATH = 259                     # longest path Windows' C runtime opens without long-path support
ROOT = Path(__file__).resolve().parents[1]


def read_u32(data, pos):
    return struct.unpack_from("<I", data, pos)[0]


def gcfn_at(data, p, version):
    """The name of a gcfn record at p, or None: magic, the stream's version, a length, a NUL-ended .gcda name, then gcda data."""
    if read_u32(data, p) != GCFN_MAGIC or data[p + 4:p + 8] != version:
        return None
    n = read_u32(data, p + 8)
    raw = data[p + 12:p + 12 + n]
    if n < 6 or len(raw) != n or not raw.endswith(b".gcda\0") or read_u32(data, p + 12 + n) != GCDA_MAGIC:
        return None
    return raw[:-1].decode()


def split_stream(data):
    """Yield (name, gcda bytes) per object: each object's data runs to the next gcfn record (checked as one, not just its magic)."""
    version = data[4:8]
    magic = struct.pack("<I", GCFN_MAGIC)
    marks, p = [], data.find(magic)
    while p >= 0:
        name = gcfn_at(data, p, version)
        if name is not None:
            marks.append((p, name))
        p = data.find(magic, p + 1)
    if not marks or marks[0][0] != 0:
        raise ValueError("the stream does not start with a gcfn record")
    for i, (p, name) in enumerate(marks):
        end = marks[i + 1][0] if i + 1 < len(marks) else len(data)
        yield name, data[p + 12 + len(name) + 1:end]


def retarget(stream, out, profile_dir):
    """Copy a stream with every .gcda name moved under profile_dir; returns (objects, longest new name)."""
    data = stream.read_bytes()
    version = data[4:8]
    parts, longest = [], 0
    prefix = str(profile_dir).replace("\\", "/")
    for name, gcda in split_stream(data):
        rest = name[name.index("\\"):] if "\\" in name else "/" + name.rsplit("/", 1)[1]
        new = (prefix + rest).encode() + b"\0"
        longest = max(longest, len(new) - 1)
        parts += [struct.pack("<I", GCFN_MAGIC), version, struct.pack("<I", len(new)), new, gcda]
    out.write_bytes(b"".join(parts))
    return len(parts) // 5, longest


def records(data):
    """(tag, offset of the payload, payload length) of a .gcda. GCC 12+: lengths in bytes; an all-zero counter block has a
    negative length and no payload."""
    pos = 16                        # magic, version, stamp, checksum
    while pos + 8 <= len(data):
        tag, length = struct.unpack_from("<Ii", data, pos)
        if tag == 0:
            return
        size = max(length, 0)
        yield tag, pos + 8, size
        pos += 8 + size


def max_arc_count(path):
    data = path.read_bytes()
    best = 0
    for tag, at, size in records(data):
        if tag == TAG_ARC_COUNTERS and size:
            best = max(best, max(struct.unpack_from(f"<{size // 8}q", data, at)))
    return best


def fix_summary(path, runs, sum_max):
    """The stream path writes the object summary as zeros (no program summary): runs = 0 and sum_max = 0. GCC sets its hot
    threshold from sum_max (sum_max / hot-bb-count-fraction), so with 0 every block counts as hot. Set runs to the number of
    streams and sum_max to the largest arc count over all objects, as libgcov's exit path does (it is 32 bits in the file)."""
    data = bytearray(path.read_bytes())
    if len(data) < 16 or read_u32(data, 0) != GCDA_MAGIC:
        return "not a gcda"
    for tag, at, size in records(data):
        if tag == TAG_OBJECT_SUMMARY:
            old_runs, old_max = struct.unpack_from("<II", data, at)
            if old_runs == 0 and old_max == 0:
                struct.pack_into("<II", data, at, runs, min(sum_max, 0xFFFFFFFF))
                path.write_bytes(bytes(data))
                return "summary set"
            return f"summary kept (runs {old_runs}, sum_max {old_max})"
    return "no object summary"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("streams", type=Path)
    ap.add_argument("--gcov-tool", default=r"C:\devkitPro\msys2\opt\devkitpro\devkitA64\bin\aarch64-none-elf-gcov-tool.exe")
    ap.add_argument("--profile-dir", type=Path, default=ROOT / "private" / "pp")
    a = ap.parse_args()
    streams = sorted(a.streams.glob("*.stream"))
    if not streams:
        sys.exit(f"no .stream files in {a.streams}")
    profile_dir = a.profile_dir.resolve()
    profile_dir.mkdir(parents=True, exist_ok=True)
    work = profile_dir.parent / (profile_dir.name + "-streams")
    work.mkdir(exist_ok=True)
    objects = None
    for s in streams:
        moved = work / s.name
        objects, longest = retarget(s, moved, profile_dir)
        print(f"{s.name}: {s.stat().st_size / 1048576:.1f} MB, {objects} objects, longest name {longest} characters")
        if longest > MAX_PATH:
            sys.exit(f"names past {MAX_PATH} characters: use a shorter --profile-dir (and the same LSW_PGO_DIR)")
        r = subprocess.run([a.gcov_tool, "merge-stream", str(moved)], capture_output=True, text=True)
        print(f"  merge-stream exit {r.returncode}")
        if r.returncode:
            print(r.stdout[-2000:], r.stderr[-2000:])
            sys.exit(1)
    gcdas = sorted(profile_dir.rglob("*.gcda"))
    sum_max = max((max_arc_count(g) for g in gcdas), default=0)
    print(f"largest arc count {sum_max}" + (" (clamped to 32 bits)" if sum_max > 0xFFFFFFFF else ""))
    results = {}
    for g in gcdas:
        results.setdefault(fix_summary(g, len(streams), sum_max).split(" (")[0], []).append(g.name)
    print(f"{len(gcdas)} .gcda files in {profile_dir}")
    for k, v in results.items():
        print(f"  {k}: {len(v)} files (e.g. {v[0]})")
    if objects is not None and len(gcdas) < objects:
        sys.exit(f"only {len(gcdas)} of {objects} objects written: check the names (gcov-dump -l)")


if __name__ == "__main__":
    main()

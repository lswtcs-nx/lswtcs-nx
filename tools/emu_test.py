"""One-command emulator test: replays a recorded route on an NRO and reports against a baseline (log113).

Runs tools/emu_run.py with everything a reproducible run needs:
  - the route script (tests/scripts/emu_gungan_quick.txt by default, about 3 minutes; --route full for the whole
    hub / Gungan A / Gungan B / hub route, about 6);
  - --fixed-dt (the game's frame time fixed at 1/60 s) and --fresh-save console_gabriel (the same save every run);
  - the emulator profile (profiles/emulator.env), a GDB thread dump on a stall, the ELF beside the NRO.
Then it reports:
  - the result: done, failed (with the step), stall, crash;
  - the guest frames between level changes ([LEVEL] ... (frame N)): inside levels these must match the baseline
    exactly, or the run did not replay the same game;
  - work per level ([WORK]: nvc0 draws, vertices, vertex and pixel program bytes, guest calls per replay) against the
    baseline. Two runs of one build differ by under 3 % in Gungan_B (A/A, 18 November 2026; hub up to 5 %, hub vertices 11 %), so only
    differences beyond --noise (default 5 %; vertices 12 %) are flagged. The verdict fails only when the run did not
    finish or did not play the same game; work changes are listed beside it.

What it cannot tell: speed on the Switch. Frame times here are the PC's. Use it to see that a build still plays the
route the same way (no crash, no hang, same game) and whether it does more or less work; measure speed on the console.

Usage:
  python tools/emu_test.py --save-baseline main            # run the current build and keep it as baseline "main"
  python tools/emu_test.py --baseline main                 # run the current build and compare with "main"
  python tools/emu_test.py --compare <run folder> --baseline main   # report on a finished run, no new run
  python tools/emu_test.py --cold --baseline main          # a player's first session (no shader caches)
Options: --nro (default source/switch/build-nvpgo/lswtcs.nro), --elf (default beside it), --route quick|full,
         --noise PCT, --env KEY=VALUE (repeatable, e.g. an A/B switch), --keep-going (no stop on the first failure)
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
import work_compare  # noqa: E402

EMU = ROOT / "_work/emu/ryubing-1.3.3/Ryujinx.exe"
SD = ROOT / "_work/emu/ryubing-1.3.3/portable/sdcard"
BASELINES = ROOT / "private/emu-baselines"
# A/A on the full route moved hub vertices by 11 % (windows there mix in load frames); other fields stay under 4 %.
FIELD_NOISE = {"vertices k": 12.0}
ROUTES = {"quick": "emu_gungan_quick.txt", "full": "emu_gungan.txt"}
LEVEL = re.compile(r"^\[LEVEL\] (.+?) \(frame (\d+)\)")
XLATE = re.compile(r"^\[XLATE\] \S+ \S+ \d+ dw: (.+?) ([\d.]+) ms")
XLATEWAIT = re.compile(r"^\[XLATEWAIT\] replay \d+: ([\d.]+) ms")
FRAMESET = re.compile(r"^\[SCRIPT\] line \d+ frameset at .*?\(level (\S+), (\w+) frames?, .*frame (\d+)\)")


def summarize(run):
    log = (Path(run) / "stdout.log").read_bytes().decode("utf-8", "replace")
    levels = [(m.group(1).split("\\")[-1], int(m.group(2))) for m in map(LEVEL.match, log.splitlines()) if m]
    # Frames from one frameset to the next inside one level: the script re-anchors when the game turns light or play
    # (a door, a cutscene), so these say when the game changed state. Frame-exact replay makes them equal run to run
    # (hub 795, Gungan_B 4252 in the A/A runs). Spans that cross a level change include a load, whose length is real
    # time, so they are not compared.
    gaps, anchor = [], None
    for line in log.splitlines():
        m = FRAMESET.match(line)
        if m:
            where, state, frame = m.group(1).split(chr(92))[-1], m.group(2), int(m.group(3))
            if anchor and anchor[0] == where:
                gaps.append(f"{where} {anchor[1]}: {frame - anchor[2]} frames")
            anchor = (where, state, frame)
        elif LEVEL.match(line):
            anchor = None
    result = "unknown"
    for line in log.splitlines():
        if line.startswith("[SCRIPT] done"):
            result = "done"
        elif line.startswith("[SCRIPT] failed"):
            result = "failed: " + line[len("[SCRIPT] failed"):].strip()
    crash = [l for l in log.splitlines() if l.startswith(("[CRASH]", "[FATAL]"))]
    work = {lv: {"replays": n, **dict(zip(work_compare.FIELDS, vals))} for lv, (vals, n) in work_compare.load(run).items()}
    return {"run": str(run), "result": result, "crash": crash[:5], "levels": levels, "gaps": gaps, "work": work,
            "stutter": stutter(run, log)}


def stutter(run, log):
    """Long play frames (frames.log: real time between swaps, frames with 150 draws or more) and what new shaders cost.
    The times are the PC's: compare runs on this PC with each other, never with the console."""
    long_frames, hold = [], 0.0
    fl = Path(run) / "frames.log"
    if fl.exists():
        for line in fl.read_bytes().decode("utf-8", "replace").splitlines():
            if line.startswith("#"):
                continue
            c = line.split()
            try:
                replay, real, hold_ms, draws = int(c[0]), float(c[4]), float(c[7]), int(c[8])
            except (ValueError, IndexError):
                continue
            hold += hold_ms
            if draws >= 150 and real > 100:
                long_frames.append((real, replay))
    kinds, xlate_ms, wait_ms = {}, 0.0, 0.0
    for line in log.splitlines():
        m = XLATE.match(line)
        if m:
            kinds[m.group(1)] = kinds.get(m.group(1), 0) + 1
            xlate_ms += float(m.group(2))
            continue
        m = XLATEWAIT.match(line)
        if m:
            wait_ms += float(m.group(1))
    long_frames.sort(reverse=True)
    return {"long_frames": len(long_frames), "long_ms": round(sum(r for r, _ in long_frames), 1),
            "worst": [[round(r, 1), n] for r, n in long_frames[:5]], "program_hold_ms": round(hold, 1),
            "translations": kinds, "translation_ms": round(xlate_ms, 1), "translation_wait_ms": round(wait_ms, 1)}


def stutter_line(st):
    return (f"{st['long_frames']} play frames over 100 ms ({st['long_ms']:.0f} ms in all; worst "
            + ", ".join(f"{r:.0f} ms at replay {n}" for r, n in st["worst"][:3]) + f"); frames held for programs "
            f"{st['program_hold_ms']:.0f} ms; translations " + (", ".join(f"{v} {k}" for k, v in sorted(st["translations"].items()))
            or "none") + f" ({st['translation_ms']:.0f} ms), render thread waited for them {st['translation_wait_ms']:.0f} ms")


def report(cur, base, noise):
    ok = cur["result"] == "done" and not cur["crash"]
    moved = []
    print(f"result: {cur['result']}" + (f"; crash lines: {cur['crash']}" if cur["crash"] else ""))
    print("frames between state changes: " + ("; ".join(cur["gaps"]) or "none"))
    if base:
        same = cur["gaps"] == base["gaps"]
        print(f"  baseline: {'; '.join(base['gaps'])}  -> {'same game' if same else 'DIFFERENT (the run did not replay the same)'}")
        ok = ok and same
    print("work per level, per replay" + (" (against the baseline):" if base else ":"))
    for lv in sorted(cur["work"]):
        if "(light)" in lv:
            continue
        w = cur["work"][lv]
        b = (base or {}).get("work", {}).get(lv)
        cells = []
        for f in work_compare.FIELDS:
            if b and b.get(f):
                d = (w[f] - b[f]) / b[f] * 100
                flag = " <-" if abs(d) > max(noise, FIELD_NOISE.get(f, 0)) else ""
                if flag:
                    moved.append(f"{lv} {f} {d:+.0f} %")
                cells.append(f"{f} {w[f]:.1f} ({d:+.1f} %{flag})")
            else:
                cells.append(f"{f} {w[f]:.1f}")
        print(f"  {lv} ({w['replays']} replays): " + ", ".join(cells))
    if cur.get("stutter"):
        print("stutter (PC times, compare only with runs on this PC): " + stutter_line(cur["stutter"]))
        if base and base.get("stutter"):
            print("  baseline: " + stutter_line(base["stutter"]))
    if base:
        print(f"  (differences beyond {noise:.0f} % (vertices 12 %) are marked <-; two runs of one build: Gungan_B within 3 %, hub up to 5 %, hub vertices 11 %)")
    return ok, moved


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--nro", default=str(ROOT / "source/switch/build-nvpgo/lswtcs.nro"))
    ap.add_argument("--elf")
    ap.add_argument("--route", choices=sorted(ROUTES), default="quick")
    ap.add_argument("--baseline", help="compare with private/emu-baselines/<route>-<NAME>.json")
    ap.add_argument("--save-baseline", metavar="NAME", help="keep this run as the baseline NAME")
    ap.add_argument("--compare", metavar="RUN", help="report on a finished run folder instead of running")
    ap.add_argument("--noise", type=float, default=5.0)
    ap.add_argument("--boot-retries", type=int, default=2, help="runs again when the emulator fails before the first level")
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--cold", action="store_true", help="a player's first session: no saved shader caches (emu_run "
                    "--cold-caches); baselines of cold runs are kept apart")
    args = ap.parse_args()

    if args.compare:
        run = Path(args.compare)
    else:
        nro = Path(args.nro)
        elf = Path(args.elf) if args.elf else nro.with_suffix(".elf")
        cmd = [sys.executable, str(ROOT / "tools/emu_run.py"), "--emu", str(EMU), "--sd", str(SD), "--nro", str(nro),
               "--script", ROUTES[args.route], "--fixed-dt", "--fresh-save", "console_gabriel",
               "--timeout", "600" if args.route == "full" else "360", "--gdb-on-stall", "60", "--elf", str(elf)]
        for e in args.env:
            cmd += ["--env", e]
        if args.cold:
            cmd.append("--cold-caches")
        # The emulator dies or hangs at boot now and then (before the first level: GL start-up, the guest scheduler), with
        # builds before and after log114 alike (18 November 2026: 5 of about 30 runs). Such a run says nothing about the
        # build, so it is run again, up to --boot-retries times.
        for attempt in range(args.boot_retries + 1):
            r = subprocess.run(cmd, capture_output=True, text=True)
            m = re.search(r"^logs: (.+)$", r.stdout, re.M)
            print("\n".join(l for l in r.stdout.splitlines() if l.startswith(("result:", "last step:", "free memory", "guest threads"))))
            if not m:
                print(r.stdout[-2000:], r.stderr[-2000:])
                sys.exit("no log folder: the NRO did not start")
            run = Path(m.group(1).strip())
            print(f"run: {run}")
            log = (run / "stdout.log").read_bytes().decode("utf-8", "replace")
            if re.search(r"^\[LEVEL\] ", log, re.M):   # reached a level: a real result, good or bad
                break
            if attempt < args.boot_retries:
                print(f"the run ended before the first level (emulator boot failure): running again ({attempt + 1} of "
                      f"{args.boot_retries})")

    cur = summarize(run)
    base = None
    if args.baseline:
        bp = BASELINES / f"{args.route}{'-cold' if args.cold else ''}-{args.baseline}.json"
        if not bp.exists():
            sys.exit(f"no baseline {bp} (make one with --save-baseline {args.baseline})")
        base = json.loads(bp.read_text())
    ok, moved = report(cur, base, args.noise)
    if args.save_baseline and cur["result"] != "done":
        print("not saved as a baseline: the run did not finish")
    elif args.save_baseline:
        BASELINES.mkdir(parents=True, exist_ok=True)
        bp = BASELINES / f"{args.route}{'-cold' if args.cold else ''}-{args.save_baseline}.json"
        bp.write_text(json.dumps(cur, indent=1))
        print(f"baseline saved: {bp}")
    # OK: the build played the route the same way. Work changes are reported, not failed: an optimization should move them.
    print("VERDICT: " + ("OK" if ok else "CHECK") + (f"; work changed: {', '.join(moved)}" if moved else ""))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

"""Runs the NRO in a Switch emulator with an input script and collects the session's logs.

Not yet run against an emulator: no emulator is installed on this machine. It is written for Ryubing (the Ryujinx fork,
https://ryujinx.app), which loads a homebrew NRO given on its command line and keeps the emulated SD card in a folder.
The emulator command is a template (--emu-args), so another emulator only needs another template.

What it does:
  1. Checks that the emulated SD card has the game files (sdmc:/lswtcs/default.xex and the rest; see docs/playing.md).
  2. Writes sdmc:/lswtcs/env.txt from a profile (default profiles/hardware-ab.env) plus LSWTCS_INPUTSCRIPT=<script>,
     and copies tests/scripts/*.txt to sdmc:/lswtcs/tests/. An env.txt already there is kept as env.txt.emu-backup
     and put back at the end.
  3. Starts the emulator, finds the session's new log folder (sdmc:/lswtcs/logs/<date>/) and follows stdout.log until
     "[SCRIPT] done", "[SCRIPT] failed", a crash report, the emulator exiting, or the time limit.
  4. Stops only the emulator process it started (its own child), copies the log folder to --out and prints a summary:
     the script's result, the last step reached, crash lines, and analyze_run.py's frame summary when frames.log exists.
Exit status: 0 when the script finished with "done" and no crash line was seen, 1 otherwise.

Usage:
  python tools/emu_run.py --emu "C:/path/Ryujinx.exe" --sd "%APPDATA%/Ryujinx/sdcard" \
      --nro source/switch/build-nvpgo/lswtcs.nro --script boot_to_hub.txt [--timeout 900] [--out private/emu-runs]
"""
import argparse
import datetime
import os
import shlex
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GAME_FILES = ["default.xex", "game.DAT", "game1.DAT"]
CRASH_MARKS = ["[CRASH]", "[FATAL]", "std::bad_alloc", "terminate called", "[EXCEPTION]"]


def newest_log_dir(logs, after):
    if not logs.is_dir():
        return None
    dirs = [d for d in logs.iterdir() if d.is_dir() and d.stat().st_mtime >= after - 2]
    return max(dirs, key=lambda d: d.stat().st_mtime) if dirs else None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--emu", required=True, help="the emulator's executable")
    ap.add_argument("--emu-args", default="{nro}", help="arguments template; {nro} is the NRO's path (Ryubing headless: '--no-gui {nro}')")
    ap.add_argument("--sd", required=True, help="the emulator's SD card folder")
    ap.add_argument("--nro", required=True)
    ap.add_argument("--script", required=True, help="a file in tests/scripts/ (copied to sdmc:/lswtcs/tests/)")
    ap.add_argument("--profile", default=str(ROOT / "profiles/hardware-ab.env"))
    ap.add_argument("--env", action="append", default=[], help="extra KEY=VALUE lines for env.txt")
    ap.add_argument("--timeout", type=int, default=900, help="seconds")
    ap.add_argument("--out", default=str(ROOT / "private/emu-runs"))
    args = ap.parse_args()

    sd = Path(os.path.expandvars(args.sd))
    game = sd / "lswtcs"
    missing = [f for f in GAME_FILES if not (game / f).exists()]
    if missing:
        sys.exit(f"the emulated SD card lacks {', '.join(missing)} in {game} (copy the disc's files there, as docs/playing.md says)")
    script = ROOT / "tests/scripts" / args.script
    if not script.exists():
        sys.exit(f"no script {script}")
    nro = Path(args.nro).resolve()
    if not nro.exists():
        sys.exit(f"no NRO {nro}")

    # Settings and scripts on the emulated card.
    (game / "tests").mkdir(exist_ok=True)
    for s in (ROOT / "tests/scripts").glob("*.txt"):
        shutil.copyfile(s, game / "tests" / s.name)
    env_path, backup = game / "env.txt", game / "env.txt.emu-backup"
    had_env = env_path.exists()
    if had_env and not backup.exists():
        shutil.copyfile(env_path, backup)
    profile = Path(args.profile).read_bytes().decode("utf-8")
    extra = [f"LSWTCS_INPUTSCRIPT={args.script}", *args.env]
    env_path.write_bytes((profile.rstrip("\n") + "\n# emu_run.py\n" + "\n".join(extra) + "\n").encode())

    cmd = [args.emu, *[a.replace("{nro}", str(nro)) for a in shlex.split(args.emu_args, posix=False)]]
    print("starting:", " ".join(cmd))
    t0 = time.time()
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    result, reason, log_dir, last_step, crash = "timeout", f"no result in {args.timeout} s", None, "", []
    pos = 0
    try:
        while time.time() - t0 < args.timeout:
            time.sleep(2)
            if log_dir is None:
                log_dir = newest_log_dir(game / "logs", t0)
            if log_dir is not None and (log_dir / "stdout.log").exists():
                with open(log_dir / "stdout.log", "rb") as f:
                    f.seek(pos)
                    chunk = f.read()
                cut = chunk.rfind(b"\n") + 1   # whole lines only
                pos += cut
                for raw in chunk[:cut].decode("utf-8", "replace").splitlines():
                    if raw.startswith("[SCRIPT] line"):
                        last_step = raw
                        print(raw)
                    elif raw.startswith("[SCRIPT] done"):
                        result, reason = "done", raw
                    elif raw.startswith("[SCRIPT] failed"):
                        result, reason = "failed", raw
                    elif any(m in raw for m in CRASH_MARKS):
                        crash.append(raw)
                if result != "timeout" or crash:
                    if crash and result == "timeout":
                        result, reason = "crash", crash[0]
                    time.sleep(3)   # let the last log writes land
                    break
            if proc.poll() is not None:
                result, reason = "exited", f"the emulator exited with {proc.returncode}"
                break
    finally:
        if proc.poll() is None:   # only the process this run started
            proc.terminate()
            try:
                proc.wait(15)
            except subprocess.TimeoutExpired:
                proc.kill()
        if had_env and backup.exists():
            shutil.copyfile(backup, env_path)
            backup.unlink()
        elif not had_env:
            env_path.unlink(missing_ok=True)

    print(f"\nresult: {result}: {reason}")
    if last_step:
        print("last step:", last_step)
    for c in crash[:10]:
        print("crash line:", c)
    if log_dir is not None:
        stamp = datetime.datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
        dest = Path(args.out) / f"{stamp}-{Path(args.script).stem}-{result}"
        shutil.copytree(log_dir, dest)
        print("logs:", dest)
        if (dest / "frames.log").exists() and (ROOT / "tools/analyze_run.py").exists():
            subprocess.run([sys.executable, str(ROOT / "tools/analyze_run.py"), str(dest)])
    else:
        print("no log folder appeared: the NRO did not start (check the emulator's own log)")
    return 0 if result == "done" and not crash else 1


if __name__ == "__main__":
    sys.exit(main())

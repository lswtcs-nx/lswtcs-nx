"""Runs the NRO in a Switch emulator with an input script and collects the session's logs.

Written for Ryubing 1.3.3 (the Ryujinx fork, https://ryujinx.app) in portable mode: it loads a homebrew NRO given on its
command line and keeps the emulated SD card in portable/sdcard. First runs (17 November 2026): the NRO boots and draws the
boot logos, then the emulator stops responding about 30 s in (docs/AUTOMATED_TESTS.md).
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
  python tools/emu_run.py --emu _work/emu/ryubing-1.3.3/Ryujinx.exe --sd _work/emu/ryubing-1.3.3/portable/sdcard \
      --nro source/switch/build-nvpgo/lswtcs.nro --script boot_to_hub.txt [--timeout 900] [--out private/emu-runs]
"""
import argparse
import datetime
import os
import re
import shlex
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GAME_FILES = ["default.xex", "game.DAT", "game1.DAT"]
# Ryubing 1.3.3 (17 November 2026): headless, handheld, no profile picker or controller screen, unimplemented services
# answered instead of stopping (pcv), and its OpenGL backend: with Vulkan the NRO stopped at its first shader.
RYUBING_ARGS = ("--no-gui --disable-docked-mode --skip-user-profiles-manager --ignore-controller-applet "
                "--ignore-missing-services --graphics-backend OpenGl --vsync-mode Unbounded {nro}")
# The clock monitor reads the GPU load with an nvhost-ctrl-gpu ioctl (0x80044715) the emulator does not have.
# The sampling profiler (LSWTCS_HWPROF) pauses threads with svcSetThreadActivity; in emulator run 9 a paused thread
# never came back and the boot deadlocked.
EMU_ENV = ["LSWTCS_CLOCKLOG=0", "LSWTCS_HWPROF=0"]
GDB = Path("C:/devkitPro/msys2/opt/devkitpro/devkitA64/bin/aarch64-none-elf-gdb.exe")
QUIET_OK = ("[THREADS]", "[CLOCK]", "[HEAP]", "[HEAPGL]", "[CPULOAD]", "[AUDIO]", "[AUDIOQ]", "[AUDIOOFF]", "[OFFCORE]",
            "[GLSTEP]", "[GLPROG]", "[NVC0DEDUP]", "[LONGWAIT]", "[KWAITLONG]")
CRASH_MARKS = ["[CRASH]", "[FATAL]", "std::bad_alloc", "terminate called", "[EXCEPTION]"]


def newest_log_dir(logs, after):
    if not logs.is_dir():
        return None
    dirs = [d for d in logs.iterdir() if d.is_dir() and d.stat().st_mtime >= after - 2]
    return max(dirs, key=lambda d: d.stat().st_mtime) if dirs else None


def free_ram_mb():
    """Free physical memory in MB on Windows, or None. The emulator holds the guest's 4 GB plus its own (about 3.3 GB in
    use in the first runs), so with little free memory it pages and looks frozen."""
    if os.name != "nt":
        return None
    import ctypes

    class MemoryStatus(ctypes.Structure):
        _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong), ("ullTotalPhys", ctypes.c_ulonglong),
                    ("ullAvailPhys", ctypes.c_ulonglong), ("ullTotalPageFile", ctypes.c_ulonglong),
                    ("ullAvailPageFile", ctypes.c_ulonglong), ("ullTotalVirtual", ctypes.c_ulonglong),
                    ("ullAvailVirtual", ctypes.c_ulonglong), ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]
    st = MemoryStatus()
    st.dwLength = ctypes.sizeof(MemoryStatus)
    return st.ullAvailPhys // (1 << 20) if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(st)) else None


GDB_EXTRA = []   # --gdb-ex: more commands after the stacks (e.g. "print g_sched_turn")


def gdb_dump(gdb, elf, port, base, out):
    """Attaches to the emulator's GDB stub and writes every guest thread's stack to `out`, then detaches (the emulator
    keeps running). The NRO is position-independent and loaded at `base` (Ryubing logs it: "Loading image 0 at ...")."""
    cmds = ["set pagination off", "set confirm off", "set width 0", f"add-symbol-file {elf} -o {base}",
            f"target remote 127.0.0.1:{port}", "info threads", "thread apply all bt 12", *GDB_EXTRA, "detach"]
    argv = [str(gdb), "-nx", "-batch", *sum((["-ex", c] for c in cmds), [])]
    try:
        r = subprocess.run(argv, capture_output=True, text=True, timeout=180)
        Path(out).write_text(r.stdout + "\n--- stderr ---\n" + r.stderr, encoding="utf-8")
        return True
    except (OSError, subprocess.TimeoutExpired) as e:
        Path(out).write_text(f"gdb failed: {e}\n", encoding="utf-8")
        return False


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--emu", required=True, help="the emulator's executable")
    ap.add_argument("--emu-args", default=RYUBING_ARGS, help="arguments template; {nro} is the NRO's path (default: Ryubing 1.3.3 headless)")
    ap.add_argument("--sd", required=True, help="the emulator's SD card folder")
    ap.add_argument("--nro", required=True)
    ap.add_argument("--script", help="a file in tests/scripts/ (copied to sdmc:/lswtcs/tests/)")
    ap.add_argument("--record", metavar="NAME",
                    help="record instead of replaying: the emulator's window opens, you play with your controller, and when "
                         "you close the window the pad changes ([PADREC]) become tests/scripts/NAME.txt")
    ap.add_argument("--input-id", help="the emulator's input id for player 1 (list them: Ryujinx.exe --no-gui "
                                       "--list-input-ids x.nro); without it the emulator's default device is used")
    ap.add_argument("--profile", default=str(ROOT / "profiles/emulator.env"))
    ap.add_argument("--env", action="append", default=[], help="extra KEY=VALUE lines for env.txt")
    ap.add_argument("--timeout", type=int, default=900, help="seconds")
    ap.add_argument("--out", default=str(ROOT / "private/emu-runs"))
    ap.add_argument("--gdb-on-stall", type=int, default=0, metavar="S",
                    help="when stdout.log gets nothing new for S seconds, dump the guest threads' stacks through the emulator's "
                         "GDB stub (needs --elf; adds --enable-gdb-stub to the emulator's arguments) and stop")
    ap.add_argument("--elf", help="the ELF the NRO was made from, for --gdb-on-stall")
    ap.add_argument("--gdb", default=str(GDB))
    ap.add_argument("--gdb-port", type=int, default=55555)
    ap.add_argument("--nro-base", default="0x8500000", help="where the emulator loads the NRO (Ryubing 1.3.3: 0x8500000)")
    ap.add_argument("--gdb-ex", action="append", default=[], help="a GDB command to run after the stacks (repeatable)")
    ap.add_argument("--fresh-save", metavar="FOLDER",
                    help="start from this save every run: empty the EMULATOR's save of the port (portable/bis/user/save/"
                         "<id>/0 and 1, beside the SD folder) and import FOLDER (under sdmc:/lswtcs/save_export/) at boot. "
                         "Only the emulator's own files are touched")
    ap.add_argument("--fixed-dt", action="store_true",
                    help="LSWTCS_FIXEDDT=1: the game's frame time fixed at 1/60 s, for frame-exact recording and replay "
                         "(a --record then writes a frame script, padrec_to_script.py --frames)")
    ap.add_argument("--save-id", default="0000000000000001", help="the emulator's save folder of the port (Ryubing: "
                                                                   "0000000000000001, emulator run 13)")
    args = ap.parse_args()
    GDB_EXTRA.extend(args.gdb_ex)
    if bool(args.script) == bool(args.record):
        sys.exit("give --script (replay) or --record NAME (record), one of them")
    if args.record and args.timeout == 900:
        args.timeout = 3600   # recording ends when the window is closed
    if args.gdb_on_stall and not args.elf:
        sys.exit("--gdb-on-stall needs --elf")

    sd = Path(os.path.expandvars(args.sd))
    game = sd / "lswtcs"
    missing = [f for f in GAME_FILES if not (game / f).exists()]
    if missing:
        sys.exit(f"the emulated SD card lacks {', '.join(missing)} in {game} (copy the disc's files there, as docs/playing.md says)")
    if args.script:
        script = ROOT / "tests/scripts" / args.script
        if not script.exists():
            sys.exit(f"no script {script}")
    nro = Path(args.nro).resolve()
    if not nro.exists():
        sys.exit(f"no NRO {nro}")

    if args.fixed_dt:
        args.env.append("LSWTCS_FIXEDDT=1")
    if args.fresh_save:
        if not (game / "save_export" / args.fresh_save).is_dir():
            sys.exit(f"no save folder {game / 'save_export' / args.fresh_save}")
        save_dir = sd.parent / "bis" / "user" / "save" / args.save_id   # portable/sdcard -> portable/bis/user/save/<id>
        if not save_dir.is_dir():
            sys.exit(f"no emulator save folder {save_dir} (boot the NRO once in the emulator first)")
        for sub in ("0", "1"):
            d = save_dir / sub
            if d.is_dir():
                for child in d.iterdir():
                    shutil.rmtree(child) if child.is_dir() else child.unlink()
        args.env.append(f"LSWTCS_SAVEIMPORT={args.fresh_save}")
        print(f"emulator save emptied ({save_dir}); {args.fresh_save} is imported at boot")

    # Settings and scripts on the emulated card.
    (game / "tests").mkdir(exist_ok=True)
    for s in (ROOT / "tests/scripts").glob("*.txt"):
        shutil.copyfile(s, game / "tests" / s.name)
    env_path, backup = game / "env.txt", game / "env.txt.emu-backup"
    had_env = env_path.exists()
    if had_env and not backup.exists():
        shutil.copyfile(env_path, backup)
    profile = Path(args.profile).read_bytes().decode("utf-8")
    extra = [f"LSWTCS_INPUTSCRIPT={args.script}" if args.script else "LSWTCS_INPUTRECORD=1", *EMU_ENV, *args.env]
    env_path.write_bytes((profile.rstrip("\n") + "\n# emu_run.py\n" + "\n".join(extra) + "\n").encode())

    free = free_ram_mb()
    if free is not None:
        print(f"free memory before start: {free} MB" + ("  (WARNING: under 6000 MB, the emulator may page and stall)" if free < 6000 else ""))
    emu = Path(args.emu).resolve()
    emu_args = args.emu_args
    if args.gdb_on_stall and "--enable-gdb-stub" not in emu_args:
        # Ryubing takes headless mode only when --no-gui is the first argument (with it later, the window opens).
        stub = f"--enable-gdb-stub --gdb-stub-port {args.gdb_port}"
        emu_args = (emu_args.replace("--no-gui", "--no-gui " + stub, 1) if emu_args.startswith("--no-gui")
                    else stub + " " + emu_args)
    if args.record and not args.input_id:
        # The first connected gamepad. (--use-main-config would take the window's controller, but also its other settings:
        # with them, ignore_missing_services was false and the emulator died on a missing service, 18 November 2026.)
        # The listing prints the ids and then goes on running, so read until the list is out and stop the process we started.
        import tempfile
        with tempfile.TemporaryFile() as out:
            lister = subprocess.Popen([str(emu), "--no-gui", "--list-input-ids", "x.nro"], stdout=out,
                                      stderr=subprocess.STDOUT, cwd=str(emu.parent))
            text, seen = "", 0
            for _ in range(60):   # at most 30 s
                time.sleep(0.5)
                out.seek(0)
                text = out.read().decode("utf-8", "replace")
                if "Input Ids:" in text:
                    seen += 1
                if seen >= 3 or lister.poll() is not None:   # 1 s after the list began: all its lines are out
                    break
            if lister.poll() is None:
                lister.terminate()
                try:
                    lister.wait(10)
                except subprocess.TimeoutExpired:
                    lister.kill()
        pads = re.findall(r"- (\S+) \(\"(?!All keyboards)([^\"]+)\"\)", text)
        if not pads:
            sys.exit("no controller found by the emulator (connect it, or pass --input-id; 0 is the keyboard)")
        args.input_id = pads[0][0]
        print(f"controller: {pads[0][1]} ({args.input_id})")
    if not args.input_id:
        # A replay needs a controller connected for player 1, or the game sees none ([PADS] no controller, 18 November
        # 2026: the script ran and the game ignored it). The keyboard ("0") is one that nobody touches by accident.
        args.input_id = "0"
    if args.input_id:
        emu_args = emu_args.replace("{nro}", f"--input-id-1 {args.input_id} {{nro}}")
    cmd = [str(emu), *[a.replace("{nro}", str(nro)) for a in shlex.split(emu_args, posix=False)]]
    if args.record:
        print("recording: play in the emulator's window, then close it to stop")
    print("starting:", " ".join(cmd))
    t0 = time.time()
    result, reason, log_dir, last_step, crash = "timeout", f"no result in {args.timeout} s", None, "", []
    pos = 0
    proc = None
    last_growth = time.time()
    thread_dump = None
    try:
        proc = subprocess.Popen(cmd, cwd=str(emu.parent), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
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
                    # Timer-driven lines keep coming while the game is stuck: they do not count as progress.
                    if not raw.startswith(QUIET_OK):
                        last_growth = time.time()
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
            if args.gdb_on_stall and log_dir is not None and time.time() - last_growth >= args.gdb_on_stall:
                thread_dump = Path(args.out) / "threads-pending.txt"
                Path(args.out).mkdir(parents=True, exist_ok=True)
                print(f"stdout.log quiet for {args.gdb_on_stall} s: dumping the guest threads through GDB")
                gdb_dump(args.gdb, Path(args.elf).resolve().as_posix(), args.gdb_port, args.nro_base, thread_dump)
                result, reason = "stall", f"stdout.log quiet for {args.gdb_on_stall} s (threads dumped)"
                break
    finally:
        if proc is not None and proc.poll() is None:   # only the process this run started
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
        dest = Path(args.out) / f"{stamp}-{Path(args.script).stem if args.script else 'record-' + args.record}-{result}"
        shutil.copytree(log_dir, dest)
        print("logs:", dest)
        if thread_dump is not None and thread_dump.exists():
            shutil.move(str(thread_dump), str(dest / "threads.txt"))
            print("guest threads:", dest / "threads.txt")
        if (dest / "frames.log").exists() and (ROOT / "tools/analyze_run.py").exists():
            subprocess.run([sys.executable, str(ROOT / "tools/analyze_run.py"), str(dest)])
    else:
        print("no log folder appeared: the NRO did not start (check the emulator's own log)")
    if args.record and log_dir is not None:
        out_script = ROOT / "tests/scripts" / f"{args.record}.txt"
        conv = [sys.executable, str(ROOT / "tools/padrec_to_script.py"), str(dest / "stdout.log"), "-o", str(out_script)]
        r = subprocess.run(conv + (["--frames"] if args.fixed_dt else []))
        if r.returncode == 0:
            print(f"script: {out_script} (read it, trim the start, then replay it with --script {out_script.name})")
        return r.returncode
    return 0 if result == "done" and not crash else 1


if __name__ == "__main__":
    sys.exit(main())

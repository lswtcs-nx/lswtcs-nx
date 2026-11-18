# Automated runs

Scripted input lets a session run without anyone playing: on the console (launch it and leave it) or in a Switch
emulator on a PC. The same script and the same NRO serve both.

**Status (18 November 2026):**
- The script player, the recorder and the converter pass their PC check (`python tests/input_script_check.py`).
- Ryubing 1.3.3 runs the NRO from boot to the hub and through `Gungan_A` and `Gungan_B`, from a save copied off the
  console. The tester's recorded route replays frame for frame (`--fixed-dt`).
- `python tools/emu_test.py` tests a build in one command (about 3 minutes): same game or not, and the work it did
  against a baseline. See "Testing a build in the emulator".
- The console route `tests/scripts/console_gungan.txt` is recorded but has not been replayed on the console yet.

## Testing a build in the emulator

```
python tools/emu_test.py --save-baseline main      # once: the reference build
python tools/emu_test.py --baseline main           # each change: build the NRO, then this
python tools/emu_test.py --baseline main --env LSWTCS_X=1    # an A/B switch, without a rebuild
```

It runs `emu_run.py` with the quick route (`tests/scripts/emu_gungan_quick.txt`: menus, hub, `Gungan_A`, the first
1500 frames of `Gungan_B`), `--fixed-dt`, `--fresh-save console_gabriel`, `profiles/emulator.env` and a GDB dump on a
stall. `--route full` replays the whole route back to the hub (about 6 minutes). Baselines are kept in
`private/emu-baselines/`. `--compare <run folder>` reports on a finished run without running again.

It prints:
- **result:** `done`, the failed step, a stall (with the threads' stacks) or a crash.
- **frames between state changes:** the guest frames between the script's framesets inside one level (the game going
  light or play: a door, a cutscene). With the frame time fixed these do not change between runs (hub 796, `Gungan_B`
  4252 in every run so far). If they differ from the baseline, the build did not play the same game.
- **work per level:** `[WORK]` per replay against the baseline. Fields beyond 5 % (vertices 12 %) are marked.
- **VERDICT:** `OK` when the route finished and played the same game; `CHECK` otherwise. Work changes are listed beside
  it and do not fail it, since an optimization is meant to move them.

**Noise and sensitivity (18 November 2026, build `0.0.99-log113`):**
- A/A, the same build (three quick runs, two full): in `Gungan_B` every field within 2.6 %. The hub is noisier, up to
  4.8 % (vertices 11 %): its windows mix in load frames, whose number depends on real time. Judge a change in
  `Gungan_B`, the heavy level.
- Sensitivity: the same build with `LSWTCS_ABSTATES` minus `e` and `d` (two of the state filters) showed pixel-program
  bytes +191 % in `Gungan_B` and +75 % in the hub, nothing else beyond 5 %.

**Speed:** the script step `speed max` lifts the port's frame limiter, and `emu_run.py` passes `--vsync-mode Unbounded`.
The full route went from about 340 s to 333 s: the emulator itself runs the game at about 35 frames per second on this
PC, so there is little to gain. The menus stay at the normal rate, since with the limiter off the title sat until
its attract movie, which hangs in the emulator. The run is already headless (`--no-gui`).

What this does not replace: speed. The emulator cannot tell how many milliseconds a frame costs on the X1, so a change
that passes here and lowers work still needs a console run to claim a gain.

## What an emulator run can and cannot tell

| Use it for | Do not use it for |
| --- | --- |
| Does the build boot, load levels, save, and not crash or hang? | FPS or frame milliseconds: the emulator's speed is the PC's, not the X1's |
| Visual regressions, by comparing captures on a fixed route | GPU cost: a desktop GPU hides the handheld's pixel-shader limit |
| Work counts that are the same on any host: draws, resolves, uploads, guest calls per frame, the size of the nvc0 shader code | CPU cost per guest call: the ARM64 code is translated again for x86 |
| Many boots in a row (a new port reaching its menu) | Clocks: emulators ignore the clock profile; there is no model of the X1's timing |
|  | Driver and Horizon behaviour: nvmap, the fourth core, thread priorities |

Performance claims still come from the console. A scripted console run helps there too: every session takes the
same route, which hand play never does (part of the ±10 % between sessions).

## Scripts

Set `LSWTCS_INPUTSCRIPT=<file>` in `env.txt`. A bare name is read from `sdmc:/lswtcs/tests/`. While the script runs it
replaces player 1's pad; when it ends (`done`) or fails, the pad is the player's again. The format is at the top of
`source/switch/include/lsw_input_script.h`. In short:

```
waitlevel Map 120      # until the level path contains "Map" (at most 120 s)
waitplay 5000          # until 5 s of play frames in a row (not menus or loading)
press START            # 150 ms down, 150 ms up, each at least 2 polls
stick L 0.5 -1         # stays until set again
repeat 3
  press A 100
end
capture                # as the L + R + right stick combo
done
```

Waits follow what the game shows, not the clock: the level path (`[LEVEL]`), light frames (title, menus, loading;
the flag the pad layout "auto" uses) and finished load episodes (`waitloads N`, the `[LOADEP]` count since boot). That
keeps a script in step when loads take longer, as they do in an emulator.

`tests/scripts/boot_to_hub.txt` follows the menu path the tester took in every session of runs 112-117 ([PAD] lines):
A, START, DOWN + A, A, UP + A, then A again and the hub loads. It has not been replayed yet.

## Work counters and the frame timeline

With the testing profile's diagnostics (log111):
- `[WORK]`, every 60 replays: nvc0 draws, vertices, the bytes of vertex and pixel program code bound per draw (summed
  over draws), and guest calls per replay. These are the same on any host. `python tools/work_compare.py <run A> <run B>`
  compares two sessions per level. Pixel-program bytes x draws stands in for shading work; pixels are not counted.
- `[CHAIN]`, every 60 frames of 1300+ draws: p50/p90 of each step from the game's `VdSwap` to the present (walk, the
  wait for the previous frame, the render thread's pickup, replay, present and swap). It is a console measurement: in
  an emulator the steps take the PC's time. `LSWTCS_CHAIN=0` turns it off, `LSWTCS_CHAINDRAWS` sets the draw floor.

How to read `[CHAIN]` on the console: if "wait for previous frame" is long, the game waits on the render thread; if
"render pickup" is long and "render idle when posted" is low, the render thread was still busy with the previous frame;
if both are short and frames are still slow, the time is in the game's own work ("game") or in the GPU (`[GLSPAN]`).

`stdout.log` gets `[SCRIPT]` lines: each step with its time, level, frame kind and load count, then
`[SCRIPT] done in N s` or `[SCRIPT] failed at N s: <reason>`.

Pressing any button on the real pad while a script runs ends it ("the player took over") and hands the pad back.
Sticks are not counted, since they drift. On the console this is the way out if a menu path goes wrong.

## Comparing captures

`python tools/capture_compare.py <run A> [<run B>]` pairs the captures two sessions of the same script took at each
`capture` step. For each step it reports:
- how much of the picture differs;
- per image, the share of pixels still at the clear colour and of near-black pixels, which flags a blank or missing
  frame even without a reference;
- `compare/step<N>.png`: A | B | the difference.

Two sessions never show the same frame exactly, so only gross faults stand out: black frames, missing geometry, wrong
colours. Checked on made-up images only so far. It needs Pillow and NumPy on the PC.

## Recording a route

The easiest script is a played one. With `LSWTCS_INPUTRECORD=1`, `stdout.log` gets a `[PADREC]` line whenever player
1's pad changes (sticks in steps of 1/16). Then:

```
python tools/padrec_to_script.py <run>/stdout.log -o tests/scripts/<name>.txt [--from-ms N] [--to-ms N]
```

Held buttons become `hold`, idle time `wait`, a new level `waitlevel` and the end of a load `waitplay 500`. Trim the
start (title, user selection) and add `mark`, `log` or `capture` where wanted, then check it with
`python tests/input_script_check.py`, which also parses every script in `tests/scripts/`.

Long routes with combat and jumps may drift from the recording; short, fixed scenes (stand in a heavy area, turn the
camera) replay best. How far a route holds is not known yet.

## Moving a save between the console and the emulator

Save managers do not list the port's save (its ID, `054C535443530000`, is not an installed title), so the NRO copies it
itself (log112):
- `LSWTCS_SAVEEXPORT=1`: at launch, the selected user's save is copied to
  `sdmc:/lswtcs/save_export/<date>_<user>/`. It only reads the save: nothing in it is changed, deleted or committed.
- `LSWTCS_SAVEIMPORT=<folder>` (a bare name is under `sdmc:/lswtcs/save_export/`): at launch, the folder's files are
  copied into the save only if the save has no files at all, then committed. A save with anything in it is never
  replaced.

Checked in the emulator on 18 November 2026:
- a test folder was imported into the empty save, then exported back identical, byte for byte;
- a second import over the now non-empty save was refused.

The emulator keeps the port's save in `portable/bis/user/save/0000000000000001/0/`.

## Recording a route in the emulator

Menu timing differs between the console and the emulator. In the emulator, the legal screen and the opening crawl ran
longer, and the title came about 30 s in (emulator probe, 18 November 2026). A route recorded on one does not replay on
the other, so record each where it will be replayed. In the emulator:

```
_work/emu/ryubing-1.3.3/Ryujinx.exe --no-gui --list-input-ids x.nro      # with the controller connected: its id
python tools/emu_run.py --emu _work/emu/ryubing-1.3.3/Ryujinx.exe --sd _work/emu/ryubing-1.3.3/portable/sdcard \
    --nro source/switch/build-nvpgo/lswtcs.nro --record to_hub --input-id <id>
```

The window opens; play, then close it. The pad changes become `tests/scripts/to_hub.txt`, which replays with
`--script to_hub.txt`.

A route replayed by the clock drifts: the tester's emulator route reached the hub but never entered `Gungan_A`. For
routes inside levels, record and replay frame by frame with a fixed frame time (log113), from the same save each time:

```
python tools/emu_run.py ... --record emu_gungan --fixed-dt --fresh-save console_gabriel
python tools/padrec_to_script.py <run>/stdout.log --frames --from-level Map -o tests/scripts/emu_gungan_frames.txt
```

Then put `emu_to_hub.txt`'s boot and menu steps in front, and replay with `--fixed-dt --fresh-save console_gabriel`.

**Captures in the emulator are not faithful yet:** the probe's captures show only the left quarter of the picture.
The emulator draws the port's frame wrongly, so comparing captures there flags this, not the build. This was not
investigated further.

## Running in an emulator

`tools/emu_run.py` writes `env.txt` on the emulated SD card (a profile plus the script), copies `tests/scripts/`,
starts the emulator, follows `stdout.log` until the script ends, a crash line appears or the time runs out, stops the
emulator it started, copies the logs to `private/emu-runs/` and prints a summary.

**Setup on the owner's PC:** Ryubing 1.3.3 (`ryujinx-1.3.3-win_x64.zip`, sha256 `baa93f48…`, from the project's own
download link) unpacked in `_work/emu/ryubing-1.3.3/` with a `portable/` folder, so its settings and the emulated SD
card (`portable/sdcard/`) stay there. The disc's files are copied to `portable/sdcard/lswtcs/`. `_work/` is not in Git.

```
python tools/emu_run.py --emu _work/emu/ryubing-1.3.3/Ryujinx.exe --sd _work/emu/ryubing-1.3.3/portable/sdcard \
    --nro source/switch/build-nvpgo/lswtcs.nro --script boot_to_hub.txt --timeout 300
```

**What the first runs needed** (the harness's defaults now):
- `--ignore-missing-services`: the CPU clock read (`pcv` command 3) is not implemented and stopped the emulator.
- `LSWTCS_CLOCKLOG=0`: the clock monitor's GPU-load ioctl (`nvhost-ctrl-gpu` 0x80044715) is not implemented.
- `--graphics-backend OpenGl`: with Vulkan the NRO stopped at its first shader; with OpenGL it drew about 100 frames.
- `--skip-user-profiles-manager`, `--ignore-controller-applet`, `--disable-docked-mode`, `--no-gui`.

No keys or firmware were needed to boot the homebrew NRO. Without the firmware's time-zone archive, save times are in
UTC. Keys and firmware, if they turn out to be needed, come from the owner's own console and never go into the
repository.

**Open:** the stop about 30 s in. The likeliest cause is memory: during those runs the PC had about 0.7-1.1 GB of its
15 GB free (another program held 7.5 GB), and the emulator had 3.3 GB in use and the guest's 4 GB to map, so it was
probably paging. The emulator's log stopping is weak evidence of a freeze, since it logs little once a game runs.
`emu_run.py` now prints the free memory at start and warns under 6 GB. First retry with the PC free. If it still
stops, `--gdb-on-stall 30 --elf private/lswtcs-logN.elf` starts the emulator with its GDB stub and, after 30 s without a
new `stdout.log` line, writes every guest thread's stack (with the ELF's function names) to `threads.txt` in the run's
log folder, using devkitPro's `aarch64-none-elf-gdb`. Not tried yet. Otherwise: run with the window (without `--no-gui`) to see the screen when it stops, and
find whether the emulator itself is stuck (its log stops too) or the NRO waits on the emulated GPU.

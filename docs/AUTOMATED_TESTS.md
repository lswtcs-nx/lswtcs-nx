# Automated runs

Scripted input lets a session run without anyone playing: on the console (launch it and leave it) or in a Switch
emulator on a PC. The same script and the same NRO serve both.

**Status (17 November 2026):** the script player, the recorder and the converter pass their PC check
(`python tests/input_script_check.py`). None of it has run on a console or in an emulator yet, and no emulator is
installed. The two scripts in `tests/scripts/` are drafts: the menu presses in `boot_to_hub.txt` are a guess.

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

Waits follow what the game shows, not the clock: the level path (`[LEVEL]`) and light frames (title, menus, loading;
the flag the pad layout "auto" uses). That keeps a script in step when loads take longer, as they do in an emulator.

`stdout.log` gets `[SCRIPT]` lines: each step with its time, level and frame kind, then `[SCRIPT] done in N s` or
`[SCRIPT] failed at N s: <reason>`.

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

## Running in an emulator

`tools/emu_run.py` writes `env.txt` on the emulated SD card (a profile plus the script), copies `tests/scripts/`,
starts the emulator, follows `stdout.log` until the script ends, a crash line appears or the time runs out, stops the
emulator it started, copies the logs to `private/emu-runs/` and prints a summary. It is written for Ryubing
(https://ryujinx.app) and untested.

Setting one up needs, on the PC that runs it:
- the emulator, installed with the owner's approval (name, size and folder first; see `AGENTS.md`);
- keys and firmware dumped from the owner's own console;
- the disc's files in the emulated SD card's `lswtcs/` folder, as on the console.

None of these go into the repository.

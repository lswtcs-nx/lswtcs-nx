# Findings from the first GPU attribution run, 1 November 2026

Fresh Mesa012 build plus logging (`0.0.12-log1`), `profiles/hardware-gpu.env`, about 24
minutes with 7 save commits, no `crash.log`, empty stderr. The tester reported no visual
errors, though that profile deliberately draws some frames wrong. Clock mode was not recorded
in the logs. The tester's usual route is handheld at stock clocks.

## Likely Mesa012 regression: tile groups never merge

In both 012 sessions, **0 of 163 sampled heavy frames merged a tile group**, and
`[GLUNTILE]` counted 398,001 walk-time copies drawn outside a group. Heavy frames carry
the 320 / 320 / 128 scissor pattern, for example `1280x320 x928` and `1280x128 x464`, so the
world is drawn about three times.

The cause found by source reading: the 012 key hashes viewport height (`viewport[3]`).
`GetHostViewportInfo` clamps y to 0 and shrinks the extent by the tile's offset, and
`plan_untile` already derives that offset from the viewport bottom. So a clamped later tile
cannot match tile 0. The local fixture missed it because its later tile used y `-288` with
an unchanged height, which real viewports do not appear to do.

Fix on this branch: height leaves the key, like y, while x, width and depth stay.
`bottom_of` still rejects a tile whose draws disagree on their bottom.

- Regression test: new `clamped-height` case fails before the fix and passes after, and a
  new `viewport-height-mixed` case still rejects. The whole test passes.
- **Not yet shown on hardware.** A later key difference could still block merging.
  `profiles/hardware-untile.env` enables the existing `LSWTCS_GLUNTILEDBG` report for that.
- **Open risk:** visual faults might have stopped because merging stopped, not because
  012's stricter comparisons fixed them. Merging is now back, so this needs a real check.
  If faults return, the walk-time copy and resident-input limits in `ARCHITECTURE.md` are
  the first place to look. Do not drop the new key checks to gain speed.

## GPU cost in heavy frames

Medians over samples with at least 300 plain draws, in ms. Each variant is a different frame.

| Variant | Samples | Total | Plain | Rects | Quads | Resolves | Texsrc | Present |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Normal | 18 | 55.7 | 27.3 | 10.8 | 3.4 | 8.9 | 4.4 | 1.2 |
| Pixels off | 18 | 20.9 | 6.7 | 2.5 | 1.8 | 4.3 | 4.3 | 1.2 |
| Pixels and vertices off | 19 | 21.1 | 4.7 | 2.2 | 2.4 | 4.3 | 4.3 | 1.2 |

- GPU total is close to the frame time, so these frames are mostly GPU-limited.
- **Pixel work is about 35 ms, or 60 % of the frame.** Vertex work is too small to separate
  here, since the variants are not the same frames.
- About 21 ms stays with pixels off. Part of it, such as the 4.3 ms of texture-source
  blits, is blit work the scissor does not clip.
- This argues against expecting Deko alone to fix pacing: pixel work stays on any API,
  unless Deko's compiled shaders turn out cheaper, which is unmeasured.
- Two pixel shaders stand out per draw: `A2D183873C322D09` at about 4.6 ms and
  `4477E1B1AAA62B09` at about 3.1 ms. What they draw is not yet identified.

## Audio

Most windows produce audio in real time. The device queue empties mainly in two
situations, shader holds and loading aside:

- Windows over 300 draws: median production 95 %, 59 s of device-empty time in total.
- Windows up to 300 draws: median 100 %, 17 s in total, mostly around transitions.
- The worst sustained gameplay windows reach 72–85 % production, usually above about 1,500
  draws, with hundreds of gaps of about 12 ms. Submit gaps there are only 30–50 ms.

With 6 device buffers holding about 32 ms, a 30–50 ms production gap is enough to drain
the queue. That fits audible jitter, but scheduler admission of the audio callback is still
unmeasured. Whether merging reduces these gaps is the next question to answer before any
buffering change.

## Shader preparation

The warm session shows 15 holds and 8.4 s over about 24 minutes, against 45 holds and 30 s
over 6.5 minutes on the cold first run. The saved program list is working, though the two
sessions did not play identical routes.

## Run 3: `0.0.12-log2`, height fix applied

About 6.4 minutes with `profiles/hardware-untile.env`, 10 save commits, no `crash.log`,
empty stderr. **The tile merge works on hardware: 30 of 33 sampled heavy frames merged.**
Records fell from a median near 700 to near 360 drawn, for example 2,765 recorded and
1,021 drawn. Two samples with a key difference were still seen, so some groups stay rejected.

Non-hold frames compared with run 2, binned by recorded draws. Routes and lengths differ,
so treat these as indications, not a controlled comparison.

| Recorded draws | Replay ms, before → after | Median frame ms, before → after | FPS, before → after |
| --- | --- | --- | --- |
| 300–800 | 9.1 → 5.7 | 48 → 45 | 19.8 → 21.4 |
| 800–1300 | 18.1 → 10.7 | 58 → 55 | 17.4 → 16.8 |
| 1300–1800 | 25.0 → 14.1 | 67 → 55 | 14.5 → 17.7 |
| 1800+ | 32.8 → 19.2 | 70 → 64 | 13.9 → 14.5 |

Replay CPU fell by about 40 %, but the GPU wait stayed near 30–35 ms, so pacing improved
only a little, which fits the pixel-dominated GPU cost above.

Audio did not improve: median production stayed at 98 %, and device-empty time did not fall.
Recorded draw merging is therefore not the main audio cause.

Visual acceptance of merging still depends on the tester's report and a longer run.

The Load Game screen shows the save date as 12/31/1969 21:00 (`shot_1141`), the existing
register item. That is the Unix epoch at UTC-3, which suggests the time reaching the game
is zero. `st_mtime` from the native save file system is the suspect, not yet confirmed.

## Early depth testing: next candidate

None of the 136 saved pixel shaders uses `early_fragment_tests`, and nearly all contain
`discard` from the translator's runtime alpha test, which is emitted unless the early hint
is set (`spirv_shader_translator_rb.cc:473`). `nvc0_program.c:443` then marks the program as
killing pixels, even in draws with alpha test off. The earlier source comment assumed
Maxwell already tested early in those cases, but the shaders never qualified as non-killing.

`0.0.12-log3` sets the hint under the same conditions as Xenia's Vulkan path, with
`LSWTCS_GLEARLYZ=0` as the switch back. Compiled and statically validated only: the real
gain depends on how much overdraw these scenes have, which has not been measured. A shader
can now have two programs, so the first session may show more shader holds.

## Run 4: `0.0.12-log3`, early hint on

About 4 minutes on a cold shader set for the new variants, with `profiles/hardware-earlyz.env`,
2 save commits, no `crash.log`, empty stderr. **88–92 % of draws took the hint**, and 19 of 23
sampled heavy frames merged tiles. No hint-related compile or link failure was logged.

Non-hold frames by recorded draws. Routes and lengths differ, and run 4 is short, so these
are indications. The 1300–1800 bin has only 49 frames in run 4.

| Recorded draws | FPS: run 2 → run 3 → run 4 | GPU wait ms: run 3 → run 4 |
| --- | --- | --- |
| 300–800 | 19.8 → 21.4 → 22.6 | 30.9 → 22.3 |
| 800–1300 | 17.4 → 16.8 → 19.1 | 33.3 → 22.9 |
| 1300–1800 | 14.5 → 17.7 → 19.3 | 32.5 → 19.1 |

The GPU wait fell by about 10 ms, which is the expected direction for early rejection.

The cost shows up in shader preparation: 35 holds and 23.9 s, against 2 holds and 1.2 s in
run 3, and 239 programs against 149. A warm session has to confirm that this settles. The audio
queue also emptied more, which these holds and this cold start may explain. Audio peaks above
1.0, which the output clamp clips, appear in every session (highest 2.3 here, 2.1 in run 2),
so they are not new with this change. Whether they are audible has not been established.

Most expensive per draw in this run: `EB7221B1AAA62B09` at 2.9 ms, `0FA48AD390D6CBB9` at
3.6 ms and `0E809EDDFB5D4D0D` at 1.3 ms. `capglsl/` from a capture holds GLSL under these keys,
so capturing a heavy frame can identify them.

## Run 5: warm session with captures

About 5 minutes on `0.0.12-log3`, 2 save commits, no `crash.log`. The tester reported no
visual or audio faults so far. Merging: 14 of 15 sampled heavy frames.

Shader holds settled as expected: **1 hold and 0.6 s**, against 35 and 23.9 s on the cold run.
Excluding the first 120 s, FPS by recorded draws was 26.9 (300-800), 21.9 (800-1300) and
17.6 (1300-1800). The sample is short and has a different mix of scenes.

Captures took **66 s and 72 s**. The replay itself took 12-15 s, consistent with about 700 KB
of draw log in a small stdio buffer, and the rest went to reading back and writing about 30
PNG targets. `0.0.12-log4` makes the combo write only the screenshot, draw log and GLSL, moves
targets to a separate combo (also holding L + R, from `0.0.12-log5`) and uses larger stdio buffers. The gain is not yet measured.
Logs and captures now go to `sdmc:/lswtcs/logs/<date_time>/`.

The two screenshots look correct, and the normal-buffer composite shows no seam at the tile
rows. That is one frame of evidence for merging, not a general proof.

Shaders identified by capture: `0FA48AD390D6CBB9` is a full-screen grid (10,800 indices) reading
the 1280x704 scene, a 160x88 target and more. `EB7221B1AAA62B09` is a 640x704 half-resolution
pass with 4 texture fetches and a loop, run twice. Both look like post-processing, which is an
inference from their inputs. Together they cost about 9 ms per frame.

A next candidate is per-pixel work on fetch constants: each fetch re-derives LOD bias, offset
and exponent with `bitfieldExtract` although they are constant for a draw.

The GPU sample at replay 1800 (980 ms, right after a level load) is excluded from cost rankings.

## Run 6: long session, `0.0.12-log5`, 1 November 2026

About 16.6 minutes, 20,879 frames, 6 save commits, no `crash.log`, no skipped draws (0 "no program"). The
tester reported more brief visual faults than before, without being able to catch them on screen, and
no effect on play. No marks were used.

**Captures missed their moments.** The shots came 10, 29, 26 and 17 replays after the press, up to about
1.5 s, because the trigger files were polled every 30 replays. The combo's file access on the input thread
also held the game for 1.1, 1.1, 0.9 and 2.6 s, two swaps after each press. So these four shots do not
show the faults, and their look says nothing about them. `0.0.12-log6` takes the request at the next replay
and does the logging on the render thread. A press still arrives after a fault of one or two frames.

**Light capture is still 12.7-15.3 s** and the full one about 61-76 s. So the larger stdio buffers did not
bring the replay part down as run 5's note expected; the cause is unmeasured.

**Combos from `0.0.12-log6`:** ZL + ZR + right stick is the full capture, as in builds before log4.
L + R + right stick is the light one. ZL + ZR + left stick stays the mark.

**Audio falls behind real time in heavy scenes.** 1875 buffers are 10 s of audio, and `[AUDIOQ]` windows
give how long each took. Audio speed by mean draws in the window, windows over 30 s excluded:

| Run, build | 700-1300 | 1300-1900 | 1900+ |
|---|---|---|---|
| 2, `log1`, no merging or early-Z | 94 % (39) | 90 % (20) | 79 % (11) |
| 3, `log2` | 71 % (7) | | 51 % (1) |
| 4, `log3` | 61 % (8) | | |
| 5, `log3` | 67 % (10) | | |
| 6, `log5` | 73 % (15) | 81 % (17) | 58 % (8) |

Window counts in brackets. This is the owner's audio jitter on a number, and it looks worse since merging or
early-Z. **That is not established**: the runs differ in scene and FPS is up, so an A/B at one spot is needed.
Guest threads were still about 30 % idle on average in these scenes (`[SCHEDPROF]`), so plain CPU saturation
does not explain it.

**Merging:** 76 of 99 sampled heavy frames, and 25,001 walk-time copies were drawn outside a group. Merged and
unmerged frames therefore mix during play. That could flicker if the two paths differ, but nothing here shows it.
`LSWTCS_GLUNTILE=0` and `LSWTCS_GLEARLYZ=0` remain the A/B switches.

FPS by recorded draws, frames over 300 ms excluded: 25.4 (300-800), 19.6 (800-1300), 18.6 (1300-1800), 18.1
(1800-2400). Replay and GPU wait are now about equal in the heavy bands, 24 and 20 ms at 1800-2400.

## What run 6 led to: `0.0.13-log7`, not yet run on hardware

Compiled and validated; the planner has host tests. Nothing below has run on a console, so every expected effect
is unconfirmed.

**Audio: a likely mechanism.** One audio frame (5.33 ms) needs turns for the XAudio client worker and the render thread,
and in heavy scenes a scheduler round holds a main-thread slice of about 2 ms and a PM4 walk slice of 3.6-5.8 ms. About
8 ms a round against a 5.33 ms frame is about 67 % speed, close to what was measured. This fits; it is not proved.
`LSWTCS_AUDIOPRIO` (audio threads first) already existed but was never in the run settings, nor was `LSWTCS_WALKOFF`
(walk off the rotation, on core 2). Both can now change during play. `sched_next_locked` also gained a limit of 8 audio
turns in a row, so two spinning audio threads cannot starve the others.

**A/B during one session.** `LSWTCS_ABCYCLE=20` with `LSWTCS_ABSTATES=MA,MAW,M,A,MAV` changes settings every 20 s (profile
`profiles/hardware-ab.env`). Defaults are unchanged without it, so nothing has been switched on for the owner's build.
`tools/analyze_run.py` compares the states by draw band: FPS, audio speed, empty device time, replay, GPU wait.

**Merged against unmerged, automatically.** In `V` states the frame is drawn merged, unmerged and merged again every 60
replays and compared. The second merged image gives the noise floor, and flagged frames save all three images. This does
not depend on anyone seeing a fault, and it does not cover early-Z, which needs `LSWTCS_GLEARLYZ=0`.

**Captures.** `LSWTCS_GLHISTORY=40` keeps the last 40 presented frames at half size, saved with a combo and named by whether
the frame merged. `dump_capture_glsl` called `stat` on the SD card for each of three shaders per draw, the likely bulk of
the 13-15 s, so it now remembers what it wrote. PNG and GLSL writes moved to a background thread, bounded at 192 MB.
`[CAPTIME]` reports the hold.

**Logging off the producing threads.** `stdout.log`, `stderr.log` and `frames.log` go through a RAM ring (`log_async.cpp`).
`[SCHEDPROF]` flushed to the SD card under the scheduler's mutex, and the audio output thread flushed every 10 s. Whether that
caused any measured gap is unknown. On a crash the ring is drained, waiting at most 200 ms for the writer.

**Clocks.** `[CLOCK]` every 5 s (`hw_monitor.cpp`, `LSWTCS_CLOCKLOG`): real CPU, GPU and memory clocks, GPU load,
temperatures, battery, operation mode and apm configuration. Read-only. It answers the clock question left open in
`PORT_RESEARCH.md`, including whether a sysmodule changes clocks.

**Plan cost.** `[GLPROF]` showed `plan` up to 8 ms per replay. Walk-time copies now reuse their source's base key, only
candidate segments are hashed, and the periodic `[GLDRAW]` repeat count reuses the plan's keys. Merge decisions should be the
same: `tests/run_draw_identity.py` has three new cases, including that a reused key equals a full hash. The render thread is
not the limit in heavy frames, so this should free CPU time rather than raise FPS.

**Hitches still unexplained.** Of 28 stretches over 250 ms outside captures, 9 follow new shaders. Caching translations was
therefore dropped. `LSWTCS_SPIKEMS=250` now also prints `[GSPIKE]`, splitting the time into guest threads, walk, file reads and
render wait.

Not attempted: stitching resolves directly (about 5 of 11.8 Mpx of blits per frame are second copies of tile pieces), colour
compression and fetch-constant folding. They need the data above first and carry more risk.

## Run 7: first hardware run of `0.0.13-log7`, 1 November 2026

About 11 minutes, 11,969 frames, 2 save commits, no `crash.log`, in a much heavier jungle level: 3,000-5,800 records against
at most about 2,800 before. The tester reported large faults at the end and could not finish the level. Archived as
`private/runs/run7-log7`.

**Clocks, now measured.** CPU 1020, GPU 307.2 and memory 1331.2 MHz, apm configuration `0x00020003`, handheld. SoC at most
39.5 C. The only other reading, CPU 1785 and GPU 76.8 in 3 samples, is at the very end of the session; the cause was not checked.
GPU load read 96-99 % or 0 %, and the zeros come also from heavy stretches, so single samples are not reliable.

**`W` (walk off the rotation) is rejected.** All in `MAW` phases:
- 5 of 40 history frames show one whole tile band black (`hist_8598`), and 0 of the 116 frames of the other three histories
  that were scanned. Bands match the tile rows, in both merged and unmerged frames. The scan only looks for black rows.
- A 20.6 s freeze: `[GSPIKE]` frame 20,557 ms, walk 20,542, prepare 9,554, guest idle 17,550.
- FPS 3.2-5.1 against 5.8-10.9 in the next `MA` phases, and shader holds of 1.8-4.4 s per phase.
- Merged frames 38-40 % in two phases where the next phases had 100 %.

The picture stopped for good 25 s after that freeze: from replay 9574 every replay repeats serial 9811 (498 records, only tile 0
resolved, rows below it stale, hence the seam at row 320 in `shot_10267`), for 87 s until the session ended, while audio and
guest threads ran and guest swaps kept counting. No earlier session did this, and it began right after `W`. One occurrence: very
likely, not proved. The mechanism was not found. `W` also gave the best audio at 1900+ draws (92 %), which does not matter now.
It is gone from the profile, so the owner's faults are not explained by it: no earlier run used it.

**Merging did not change the picture in the checks.** 12 checks on frames of 1,425-5,476 records: at most 2 pixels differ,
noise 0, none flagged. A small sample, in this level only. It does not cover early-Z, and run 6's faults remain unexplained.

**Audio priority works, and seems to cost FPS.** Same draw bands, `M` (off) against `MA` and `MAV` (on):

| Draws | Audio off | Audio on | FPS off | FPS on |
|---|---|---|---|---|
| 1300-1900 | 43 % | 98-99 % | 16.3 | 5.8-7.2 |
| 1900+ | 47 % | 60-65 % | 11.8 | 8.6-9.1 |

The cycle order was fixed and the scene moves, so the FPS cost is an indication. With `M` the highest in its neighbourhood in each
cycle it looks real. So this is a trade-off for the owner rather than a plain fix, and even with priority audio stays under
real time above 1900 draws.

**This level is limited by the guest CPU, not the GPU.** Heavy frames: swap to swap 99 ms, of which GPU 54 ms and GPU idle 47 ms.
The turn is almost always held (idle 0.4-1.3 %), yet the main thread holds it for 31-44 % of the time while using only 17-18 % CPU.
The walk thread is at 26-27 % turn, 20-23 % CPU. What the main thread waits on while holding the turn is unknown, and is the next
question. 460.8 MHz would not by itself fix such a level; run 6's lighter levels were GPU-limited.

**Early-Z:** 35 % of draws hinted, against about 90 % before. Likely alpha-tested foliage; not checked per shader.

**Captures.** Hold: light 0.3 s (was 13-15), full 2.4 s (was 61-76). The background writer is too slow, though: when the session
ended, `hist_10290` and `hist_12020` were empty, `hist_10267` had 36 of 40 frames, and `res_12020` was partial. About a minute
was not enough for one full capture. Its priority is the first suspect.

**Plan:** 4.3 ms per replay in heavy frames, on frames with about twice the records of run 6's 4.95 ms sample.

**"texture unit with an unknown binding":** 22 `[GLBRIDGE]` lines, new in this level; none in runs 2, 5 and 6. Each drops a
texture unit from a shader. Not yet examined, and a candidate for visible faults there.

**Black Ops 2:** see `PORT_RESEARCH.md`. The tester meant bo2-360-nx, a real 360 static recompilation.

## What run 7 led to: `0.0.14-log8`, not yet run on hardware

Compiled and validated. The planner tests still pass, and the report tool was run on synthetic samples against the real ELF.
Nothing below has run on a console.

**A real texture bug, found by reading the code.** `SpirvShaderTranslator::PostTranslation` stores a shader's texture and sampler
bindings only once, from its first translation. Upstream that holds, because bindings do not depend on the modification. The
port's `kLswTexturesUnsigned` does change them: an unsigned translation has no signed bindings. So when a shader was translated
in both forms, the second had binding numbers that no longer fit the stored lists, and `get_glsl` dropped its texture units with
"texture unit with an unknown binding". Those draws then sampled whatever the units last held. The keys logged in run 7 match
this: `82B78D7C67978A69` and `8E809EDDFB5D4D0D` are the shaders `02B78D7C67978A69` and `0E809EDDFB5D4D0D` of run 6's `capglsl`
with bit 63 flipped, which is what that modification does to the key. `get_glsl` now reads the bindings of the translation it just
made, through accessors added to the translator (`source/patches/xenia-canary.patch`, checked to apply on a clean checkout at the
pin). Expected on hardware: no such `[GLBRIDGE]` line. Whether this caused any fault the tester saw is unproved. The black
characters in `shot_10267` are a candidate, as they are below the seam in a frame from right after those lines.

The replay binds one texture to the unsigned and signed units. That is fine for gamma and biased components and wrong for truly
signed ones. `[GLSIGNS]` now reports what reaches these translations, to tell whether that needs work.

**Where the guest CPU goes: `LSWTCS_HWPROF`** (`hw_profiler.cpp`). Every 2 ms the turn holder, and every 8 ms every thread, is paused
for a few microseconds, and its pc, lr and likely callers are recorded with its band and whether it held the turn.
`tools/hwprof_report.py` names them. This answers what the main thread waits on while holding the turn. The chain is a stack scan,
so only pc and lr are exact. It costs some CPU on core 2, unmeasured. Nothing allocates or locks while a target is paused.

**A/B.** States now run in a new random order each round, 15 s each, because run 7's fixed order confounded state with scene.

**Capture saves** moved to core 1 under the render thread, and `[CAPSAVE]` reports MB and seconds.

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

## Run 8: first hardware run of `0.0.14-log8`, 1 November 2026

About 13 minutes, 11,579 frames, 6 save commits, no `crash.log`. Episode I chapter 2, the heavy level, and this time the tester finished
it, reporting stutters and freezes but only one visual fault. Clocks stayed at CPU 1020, GPU 307.2 and memory 1331.2 MHz; SoC at most
41.2 C. Archived as `private/runs/run8-log8`, with its ELF.

**Most of the long freezes came from the diagnostics, not the game.** 21 gaps over 1 s add up to 227 s, 29 % of the session; eight over
5 s make up about 200 s of that.

- Two (14.5 s and 58.7 s, `[CAPTIME]` "targets") were PNG writes on the render thread, after three full captures close together filled the
  192 MB background queue.
- Six began between -1.1 and +5.5 s from the end of a profiler window, only while capture files were still being saved, and lost
  profiler samples match their length. So they lasted as long as `hwprof.txt` was being written: about 680 KB per window through a
  small FILE buffer, competing with the PNG writes at only 0.85-0.93 MB/s. During them GPU load was 0, guests about 98 % idle, while audio
  and the clock log kept running. How that write blocked the render thread is not proved. Renderer file polls waiting on the same
  SD card are one suspect.

**The texture fix held on hardware.** No "unknown binding" or other `[GLBRIDGE]` line in the session. `[GLSIGNS]` reported only format 38,
`k_32_32_32_32_FLOAT`, with signs 55. Sign handling does not apply to float data, so this does not call for work.

**Merging: 35 checks, 0 flagged**, at most 2 pixels differing, noise 0. At 1900+ draws replay costs a median of about 25 ms merged against
46 ms unmerged. 2,091 merges were refused for differing content; why was not examined.

Frames over 1 s left out; guest idle and thread shares from 5 s `[SCHEDPROF]` windows without a freeze:

| Recorded draws | FPS | Replay ms | GPU wait ms | Guest idle | Main | Walker | Audio |
| --- | --- | --- | --- | --- | --- | --- | --- |
| under 300 | 39.8 | 0.5 | 14.3 | 60 % | 14 % | 2 % | 3 % |
| 300-800 | 21.5 | 7.6 | 25.3 | 42 % | 27 % | 18 % | 6 % |
| 800-1300 | 18.4 | 15.5 | 25.4 | 34 % | 30 % | 27 % | 8 % |
| 1300-1900 | 17.5 | 23.0 | 19.9 | 14 % | 30 % | 33 % | 9 % |
| 1900+ | 14.1 | 30.5 | 7.9 | 10 % | 31 % | 41 % | 10 % |

So the GPU limits the middle bands and the guest's turn limits the heaviest. Above 1300 draws almost no turn time is left, and audio falls
below real time. These windows were not split by capture work, so treat the shares as approximate.

**Where the turn goes in the heavy band (`hwprof.txt`).** No single lock dominates, which answers run 7's question for this level: only about
10 % of the holders' samples are blocked in a system call, nearly all of it the next item.

| Turn share | Thread | Leaf functions |
| --- | --- | --- |
| 45 % | Walker `822D2AB0` | PM4 walk 21 %, page hash 13 %, file queries 12 %, `memcmp` 9 %, `memmove` 4.5 %, `memset` 3.7 %, clock reads 3.4 % |
| 35 % | Main | File queries 9 %, page hash 7 %, then diffuse guest code: `sub_8231ED30` 3.3 %, `sub_82320328` 2.5 % |
| 11 % | Audio `827DB318` | Guest mixer `sub_827E51D0` 15 %, `sub_827F5990` 14 %, XMA decode about 18 % |

The file queries are this port's own: trigger-file polls, limited to one per 250 ms per call site on the belief that a query takes 10-20 us.
That figure came from a PC. Here they appear to take milliseconds. `press_now`, through `XamInputGetState`, is 6 % of the main thread alone.
Call chains are a stack scan, and the 2 ms sampling pauses the holder, so the percentages are approximate.

**A/B, random order, gaps over 300 ms left out.** At 1900+ draws: M 15.8 FPS with audio at 41 %, MA 14.6 and 52 %, A (unmerged) 12.6 and 58 %,
MAV 13.2 and 71 %. Run 7's reading of a large FPS cost from audio priority does not hold up without its fixed order: it is about 1 FPS
here. No setting keeps audio at real time in this band, and MAV's 71 % against MA's 52 % is larger than V should cause, so scene mix
still weighs on these numbers.

**The visual fault was not identified.** The tester placed it at roughly 20:39 or just before. Captures
`6338` (about 20:38:50) and `6631` (about 20:39:09) are the candidates; frame times were matched to the clock only to within about 2 s. An
automated scan of all five histories for black rows and sudden changes found nothing, and looking at both found no clear fault. In `6631` the
characters beside the blue saber turn cyan and return. Qui-Gon goes green beside his own saber in `6338`, so this is probably saber light, but no
reference capture was compared. The grey moving block's flat black and white bands are also unchecked.

## What run 8 led to: `0.0.15-log9`, never run on hardware by itself

The tester chose to skip this build and test it together with the next one, `0.0.16-log10`.

Compiled and validated; the draw-identity test passes. The new PNG writer's output was checked on PC: pixels, CRCs and Adler-32 at six sizes.
The level table was tried on synthetic frames. Nothing below has run on a console.

**Diagnostics no longer write from the game's way.**

- File triggers are off on the console unless `LSWTCS_TRIGGERS=1`, in guest and renderer. The capture combos replaced them.
- The profiler hands a full table to the background thread and keeps sampling into a second one, so its file and the capture files never
  compete. A window waiting behind capture files just runs longer.
- A new profiler band, "no frame for over 1 s", samples any remaining freeze: `hwprof_report.py --band 3` says what each thread waits on.
- PNG writing is much cheaper: CRC by the CPU's instruction, row copies, Adler-32 reduced every 5552 bytes. It used 4 divisions per byte.
- A full queue now skips files with a `[CAPSAVE]` line, never writing on the render thread. A full capture with over 60 MB still queued keeps
  its screenshot, draw log and history and skips its targets, with a `[RESDUMP]` line.

**Turn time saved in the record path, all with unchanged output.** Register blocks are byte-swapped with NEON and tested against
the hook ranges inline (5 % of the walker was calling those hooks), the clock is read directly on the console, a shader at the same address is not compared
again within a memory epoch, and tile copies make one copy of the 1760-byte record instead of two. The shader case relies on what the page hash
already relies on: no guest code runs within one epoch.

Rough expectation, from samples and not measured: about 10 ms of a 71 ms frame at 1900+ draws (file queries about 5.7 ms, the rest about 4.5 ms), so
perhaps 14 to 16 FPS there, and more turn time for audio. Lighter bands are GPU-limited and should change little. The profiler still runs and costs
something itself.

**Level names in the logs.** GAME.DAT hides file names from the host, but the game formats its level paths with sprintf (`sub_8250F7E0`). Its output
is read after the call for five known formats, found by following those strings in the generated code: `levels\%s\%s.gin`, `.giz`, and
`%sLevels\%s\%s` with its `.ai2` and `ai.pak`. `[LEVEL]` in stdout and `# level` in frames.log, and `tools/analyze_run.py` prints a table per level.
Whether all of these run at every level load is unverified; a level that uses none would keep the previous name.

**Profile.** `hardware-ab.env` drops the unmerged state, already answered. `LSWTCS_GPUPROFLOG=1` logs pages hashed, re-hashes and epochs per frame (`[GPUPAGES]`).

**Next, not started.** The page hash is 8.6 % of the turn in heavy scenes and needs `[GPUPAGES]` first: if re-hashes within a frame dominate, there is a cheaper rule to look for,
carefully, because that check guards against missed updates. After it, the PM4 walk itself and the guest mixers (`sub_827E51D0`, `sub_827F5990`) as native rewrites
under the method in `BO2_OPTIMIZATION_PATHS.md`.

The next run should repeat Episode I chapter 2. It decides whether the freezes are gone (`--band 3` shows any left), what FPS and audio gained at the same
draw counts, and whether `[LEVEL]` names the levels. Captures are best kept for moments with a fault, since each full one writes about 130 MB.

## `0.0.16-log10`: planned from run 8's profile, first run on hardware is run 10 below

Compiled and validated. The draw-identity test passes. The fetch-constant test passes, also over the 181 distinct shaders captured so far, and those shaders still
compile as rewritten on a PC's OpenGL driver (NVIDIA), which says nothing about Mesa. This section was written before any of it ran on a console.

Run 8 changed the order of `BO2_OPTIMIZATION_PATHS.md`. In the heavy band 69 % of the turn is host code and 31 % the game's, and the game's top 100 functions are only 22 %.
So work on this port's own record path comes first, and compiler flags or native rewrites of guest code have less to offer for now.

**State letters.** `hardware-ab.env` runs MAPXS and one state without each of P, X and S, plus one with H and one with V, so each compares with MAPXS on its own.

**P: reuse of draw preparation** (`gl_bridge.cc`, `lsw_gpu_prepare`). Every register write now goes through a generation count (`LswRegGens`), which ignores a value written again
unchanged. While no state register has changed and the shaders are the same, the previous draw's translations, viewport, scissor and system constants stand, and only what
changed is computed again: texture state and vertex ranges on a fetch change, floats on a float change. Identical blocks then skip the comparison in `put_block`, through a token.
Every 256th reuse also runs the full path and compares; the first difference prints `[PREPMEMO] MISMATCH` and turns reuse off for the session. How often draws qualify is unknown until `[PREPMEMO]` reports it.

**X: XMA and LZ2K off the turn.** A kick wakes the XMA worker, now on core 2, instead of decoding on the audio thread, as the 360's decoder runs beside the CPU. Blocks of 16 KB and up decompress
without the turn, on core 1 below the render thread. If the game relied on a kick finishing at once, audio would show it in this state.

**S: resolves straight into stitched targets** (`gl_rt.inc`). About 5 of 11.8 Mpx of blits per frame were second copies of tile pieces. Once a region's composite has been built twice, resolves into it go
straight to their row, which keeps the rule that the newest resolve of a row wins. A part read on its own, or a composite of another size, gets its rows copied back first. State V's comparison now also resolves and
composes the old way in its unmerged pass, so it checks S too.

**H: page hash per frame.** The hash is 8.6 % of the turn. The source comment rejects page age as proof that a page has not changed, for a good reason, so this is not on by default and needs the owner's judgement even if it
measures well. Only in this state, a page unchanged for 8 frames and already hashed this frame skips later epochs, except one in 16, and `[GPUPAGES]` counts what those samples would have missed.

**Always on:**

- Fetch constants folded (`lsw_glsl_fold.h`, `LSWTCS_GLFETCHFOLD=0` turns it off): offset, LOD bias and exponent scale come from a uniform block computed per draw instead of bitfield extracts, divisions and `ldexp` per pixel.
  It rewrites the GLSL text, so the program list on the SD card is rewritten too at boot and stays useful. That also leaves no state to compare within a session: it can only be compared against run 8. It changes rounding offsets by at most
  a unit in the last place. Mass Effect's similar change measured nothing, so expect little.
- Blits use one program per mode instead of branching per pixel.
- `lsw_guest` scopes compile to nothing in the Mesa build, where no observer exists; they were a call per access.
- `LOAD_ALU_CONSTANT` takes the same block copy as type 0 writes.

**Considered and left out.**

- Not walking repeated tile IBs. With the block copy and the generation count little of the gain is left, perhaps 2 % of the turn. Its draws still need the viewport registers set inside the IB, so skipping writes needs a replay script
  of them, and an error would show as a wrong viewport in a later tile.
- `bridge()` costs a static guard check, 0.7 % of the turn, not the `call_once` assumed when this was planned. Passing it as an argument removes only a part of that.

**Reading the next run.** `tools/analyze_run.py` compares the states by draw band and prints `[PREPMEMO]`, `[GPUPAGES]`, `[GLSTITCH]` and, from `LSWTCS_GLGPUPROF=600`, GPU time by kind of work with merging on, which run 2's table predates. A
`MISMATCH` or a flagged `[UNTILECHECK]` decides against that change whatever its FPS. `tests/run_fetch_fold.py` covers the rewrite.

## Run 10: first hardware run of `0.0.16-log10`, 1 November 2026

Handheld at stock clocks, 16,739 frames in 647 s, no crash, stderr empty, two full captures (replay 1957 in the hub, 12860 in `Gungan_C`). Levels seen: `Map\Map`, `Episode_I\Gungan\Gungan_A/B/C`. Archived in `private/runs/run10-log10`. The tester saw no visual fault and neither do
the two screenshots, which says little about the frames not captured. Real heavy gameplay was short, about 180 s (t≈360 to 540), and t≈180 to 360 sat at 64 draws. The state slices are 15 s long and land on different scenes, so the per-state numbers below are indications, not comparisons of equals.

**Long freezes are mostly gone.** Gaps of 1 s or more: 27 totalling 50.7 s (7.8 % of the session), against 21 totalling 227 s (29 %) in run 8. Of the time in gaps of 500 ms or more, 81 s falls in the two windows of about 100 s while a capture saves, and 16.8 s outside them.
Saving took 99.6 s for 99 MB and 99.7 s for 105 MB, about 1 MB/s, so the SD card and not the PNG encoder sets the pace.
During those gaps the render thread spends 35 % of its samples in `nvioctlChannel_KickoffPb` under `pushbuf_submit`, and 11 % in `nvFenceWait` under `nwindowDequeueBuffer`. A hypothesis not proved here: file system and nvservices share the system core, so busy SD writes starve the GPU kick. That would also fit run 8's freezes with GPU load 0.

**FPS by recorded draws, outside capture saves and gaps of 1 s or more.** Run 8 is its MA and MAV states over the whole session, with gaps of 1 s or more removed but its capture saves still in. Run 10's 300-800 row is only 6 s and its 4000-5000 row 13 s.

| Draws | Run 8 FPS | Run 8 median ms | Run 10 FPS | Run 10 median ms | Run 8 audio | Run 10 audio |
|---|---:|---:|---:|---:|---:|---:|
| 300-800 | 21.7 | 42 | 25.1 | 36 | 97 % | 80 % |
| 800-1300 | 18.0 | 50 | 20.0 | 46 | 95 % | 76 % |
| 1300-1900 | 17.8 | 51 | 20.2 | 46 | 82 % | 82 % |
| 2500-3100 | 14.9 | 61 | 16.6 | 56 | 59 % | 57 % |
| 4000-5000 | 11.6 | 71 | 11.9 | 78 | 66 % | 69 % |
| 5000+ | 10.9 | 85 | 9.5 | 100 | 45 % | 59 % |

So 11 to 16 % better up to about 3100 draws, even in the 4000-5000 band by FPS though not by median, and 13 % worse in the heaviest band, with the regression below in force in all of them. Audio is the fraction of real time played at that band. Its queue was empty for 5.25 % of the session against 9.54 % in run 8, but the longest empty stretch grew from 512 to 851 ms.
The light bands' drop to 76-80 % is not explained. Part of it is t≈600 to 630, at about 950 draws and 64 %. Audio also falls at 64 draws, t≈270 to 360, to 63-89 % with 7 s of empty queue, and what the game was doing there is unknown.

**A regression from this version: the XMA kick.** `svcArbitrateLock` is 22.2 % of the heavy band's turn, against 0.7 % in run 8, and 20.5 points of it are the audio thread in `pthread_mutex_lock < SetEvent < lswtcs_xma_mmio_store`. The audio thread's share of the turn went from 10.5 to 30.8 %.
`SetEvent` takes the one mutex shared by all wait objects (`win32_switch.cpp`) and notifies every waiter, and now that the XMA worker runs on core 2 it contends on that mutex truly in parallel. The worker is pinned there in every state, so the X state says little about it. Fix: a mutex, condition variable and pending flag of the worker's own, with `notify_one`.
How much FPS this returns is not known until it runs.

**P turned itself off at t≈98 s.** `[PREPMEMO] MISMATCH in vertex floats after 326400 reuses`, in the hub, so all real gameplay ran without P and P against no P cannot be read from this run. `memo_differs` compares `std::vector<float>` by value, so a NaN constant differs from itself, which makes a false alarm likely. This is not proved. Compare bits and print the first
differing index. Until then the mismatch stands. Before it, 67 to 79 % of draws reused, about half of them with new fetch or float data.

**Polls and walk.** `svcSendSyncRequest` fell from 12.4 to 1.6 % of the heavy turn. Audio thread CPU from 35 to 15 %. Walker from 44 to 34.5 % and main from 35 to 30 % of the turn. Host against guest is still 69 to 31. Left in the record path: `pm4_extract_ib` 7.1, XXH3 6.7, `memcmp` 3.3, `prepare_full` 1.7, `fill_fetch_derived` 1.6, the last one new.

**S works.** Heavy frames went from 28.9 to about 15.6 blits and 11.79 to about 7 Mpx, close to the 5 Mpx estimated. `[GLGPU]` texsrc blits fell from about 2.5 ms to 0.00 with S. `[UNTILECHECK]`: 19 checks, none flagged, at most 4 pixels different. A comparison only covers the frames it sampled.

**H looks effective, and is still the owner's decision.** Heavy frames hashed 1312 pages against 2168, re-hashed 105 against 1140, walk 15.8 against 18 ms, record 12 against 14 ms. Sampled misses: 0 in the session, a small sample at one in 16. Other pages still change mid-frame about 0.9 times per frame. Stays off by default for the reason in the source comment.

**Fetch fold: compiles, cost one recompile.** No compile or link failure on Mesa, 352 programs. Worker time was 196 s against 49 s, which is not the fold being slower to compile: run 8 hit Mesa's disk cache (`shader_cache_read_program`), and the rewritten text missed it. Front-end samples (GLSL IR and TGSI) rose from 661 to 7918, while the nv50 backend, which runs either way, only went from 2458 to 2753.
Disk cache writes rose from 310 to 3848 samples. So a second session of this build should fall back near 49 s; that is an inference, not measured. Also one-time: about 20 % of the render thread's freeze samples were file operations in `program()`, from `program_list_save` rewriting the list, which runs on the render thread and should move to the background writer.
The queue drained around replay 2068, right after the first capture, so capture 1957's 5.9 s gap had both writers competing. The fold's own FPS effect cannot be separated from the rest here.

**GPU.** `[GLGPU]`, few samples per band: under 300 draws 3.8 ms; 300 to 1300 draws 32.6 ms (plain 18.5, rects 4.9, resolves 3.0, quads 2.6); 1300 to 2500 draws 37.4 ms (plain 21.8, rects 5.9, resolves 5.7). One sample at about 5000 draws, replay 10800: 94 ms, of which 62.95 ms quads and 61.08 ms a single pixel shader, `CEF661992D489E4B`, over 83 draws. It has one texture and alpha test branches, which suggests smoke or fog overdraw. So the heaviest spot is limited by the GPU too, while `gpuwait` reads 0.
One sample decides nothing, and any reduction in that effect is a quality decision for the owner.

**Levels.** `[LEVEL]` works; `note_level` leaves a trailing `\ai` that should go.

## Smoke in Episode I chapter 2: what is known, and options for the owner

**Corrected by run 11, below: the 61 ms was not this shader's steady cost.** It costs about 4 to 7 ms in settled frames, and the heaviest band is limited by the CPU. The fill estimate and the table here stay as the record of what was assumed, and no reduction is needed for now.

The tester sees it in `Gungan_C`, as fog and smoke sheets near the waterfall and in the forest. The shader is `CEF661992D489E4B`.

**Measured.**

- One timed frame, replay 10800 in run 10: 61.08 of 94 ms of GPU over 83 draws. It is the only `[GLGPU]` sample that fell on such a frame out of 27, so how typical it is, is unknown. If it is, the heaviest band cannot pass about 10 FPS whatever the CPU does, which fits its 100 ms median, although the GPU wait column reads 0 (the wait does not show there).
- Captured frames: 131 draws in run 8's replay 3787 and 167 in run 7's replay 4242, a few draws in other frames. Each draw is usually 36 vertices through the quad geometry shader, so 9 quads, about 1,240 quads in replay 3787.
- One DXT5 texture per draw (1024x1024 and 512x512 seen), in the main scene pass at 1280x704. The guest's 2x MSAA is already drawn at one sample per pixel here (`LSWTCS_MSAA1X`), so no gain is left there. Blend `00000706` is source alpha and one minus source alpha. `RB_DEPTHCONTROL` is `00700736`: depth test and depth write on. Two colour targets are written.
- The second target is not wasted: it is resolved and sampled later in the frame, so it cannot simply be left out.
- Most of those draws come early in the pass, before other opaque geometry. In replay 3787, 121 of the 131 come within the first 165 of 1,949 records. Pixels covered later were shaded for nothing, and the game's order cannot be changed from here without knowing why it draws so.

**From the shader text, not measured.** The translated shader is small: a texture read, a few multiplies, then the translator's run-time checks for alpha test, alpha to mask and gamma, each read from a uniform per pixel. Depth writing with a coverage-dependent alpha test rules out the early hint (Xenia's condition, `gl_bridge.cc`), so if alpha test is on in these draws, as depth writing suggests, pixels behind geometry already drawn are shaded too. The capture does not record the alpha test.

**Rough scale, an estimate.** 61 ms at the GPU's likely fill of 0.5 to 1.2 Gpixel/s would be 30 to 75 Mpx, or 30 to 80 layers of the whole screen. That is why it is cheap on the 360, whose blending happens in eDRAM, and expensive here. What limits it (shader, texture cache, blending or memory) is unknown. `0.0.17-log11` measures pixels per program in those frames (`LSWTCS_GLGPUPIX`) and samples frames of 4000 draws or more every 120 frames instead of every 600, which should answer the first question: nanoseconds per pixel of this shader against costlier ones.

**Options, none implemented.** Each depends on that measurement, and all but the first change or may change the picture, so they are the owner's decision. A rule keyed to this shader would also be the kind of per-content exception `AGENTS.md` rules out unless the owner accepts it.

| Option | Expected effect | Visual risk | Effort, problem |
|---|---|---|---|
| Specialise the translator's run-time checks per program (alpha test, alpha to mask, gamma) | Fewer instructions on every shader, not just smoke. Helps only if shader time is the limit | None intended | Medium. More program variants, and run 4 showed variants cost shader holds in a cold session |
| Positive LOD bias on large blended sprites | Less texture memory traffic | Softer smoke, small on already soft textures | Small, but needs a rule that picks these draws |
| Skip quads that are fully transparent | Depends on how many are fading out, unknown | None if alpha really is zero | Needs vertex data the capture lacks, and geometry shader work |
| Dynamic resolution in heavy frames | The largest and most general, all fill falls with pixels | Softer picture while it lasts | Large: every target and blit scales, and it is a quality decision |
| Lower resolution for smoke only (off-screen particles) | Up to a quarter of the fill | Soft edges against geometry | Large. These draws write depth and a second target, which a lower-resolution pass cannot simply reproduce |
| Draw fewer smoke quads | Direct | Visible thinning | Small, but a change of content; last resort |

## `0.0.17-log11`: fixes from run 10, first run on hardware is run 11 below

Run 10's analysis leads to this batch. This section was written before it ran on a console.

- **XMA wakeup** (`lsw_xma.cc`). The worker has its own mutex, condition variable and flags, and a kick takes the lock only if the worker is asleep. Expected: `svcArbitrateLock` back near run 8's 0.7 % of the heavy turn. What FPS that returns is the main question of the next run.
- **P's comparison** (`gl_bridge.cc`). Floats compare by bits. A real difference now prints which value, both bit patterns and the shader keys, and still turns reuse off. If no mismatch appears, this run gives the first real reading of P in gameplay.
- **Program list** (`gl_backend.cpp`). Written from the background thread. A full queue skips the entry, and the program is listed next session.
- **Capture files** (`lsw_png_write.h`, `log_async.cpp`). PNGs are compressed: filter Up, zlib level 1, tried on run 10's own images on a PC, about 40 % of the bytes overall (43 of 105 MB). The file size is set first, then cut to what was written, and 256 KB goes to the card per write, against 64 KB through a `FILE`. Every other capture also pauses after each write for as long as the write took, to test whether SD writes starve the GPU's kicks. `[CAPSAVE]` now says bytes on the card, writes, time writing, longest write, time in open, size and close, and time paused. Unknown: how zlib's CPU time on the background thread compares, and whether presetting the size helps on this card.
- **Profiler.** `xma`, `bg_files` and `log_writer` are now sampled, so the save rate of about 1 MB/s can be split into CPU starvation of that thread at priority 0x3A and waiting for the card.
- **GPU sampling.** As above. The counter around each draw may stretch the timings of sampled frames a little.
- **Levels.** A trailing `\ai` is stripped.
- **A/B.** MAPX is dropped, since S looked clean: `MAPXS,MAXS,MAPS,MAPXSH,MAPXSV`. V still checks S.

**Considered and left out.** `fill_fetch_derived` (1.6 % of the heavy turn in run 10): all of run 10's gameplay ran it per draw because P was off, and P's reuse avoids it on many draws. A dirty mask per fetch slot could remove the rest, but a missed write would show as a wrong LOD and P's check could not catch it unless it recomputed everything, which is not worth about 1 %.

**Reading the next run.** A warm shader cache is expected, so compile time should be near 49 s; if not, the inference in run 10 was wrong. `tools/analyze_run.py` prints milliseconds per Mpx for the costliest programs.

## Run 11: first hardware run of `0.0.17-log11`, 2 November 2026

Handheld at stock clocks, 9,180 frames in 375 s, no crash, stderr empty, six full captures, all taken for material and none for a fault. Levels: `Map\Map`, `Gungan_A`, `Gungan_B`. Archived in `private/runs/run11-log11`. About 60 s at 5000 draws or more. State slices are short and land on different scenes, so per-state numbers are indications.

**The lock is still there, and the cause is now exact.** `svcArbitrateLock` is 20.0 % of the heavy turn (22.2 in run 10), 19.6 points of it on the audio thread. All sampled call sites of the mapped return address, offset 0x224 in `lswtcs_xma_mmio_store`, are the empty `lock_guard` on `g_wake_mx` in `wake_worker`, by disassembly. So this version's fix moved the wait instead of removing it. A likely reason, from how the kernel's condition variable works and not measured: a waking thread owns the mutex as soon as it is signalled or times out, before it gets a core, so the kick waits until the worker is scheduled on busy core 2. Run 10's stack named `SetEvent`, and the same effect there would explain it.
Fix: notify without the mutex. A wakeup lost in the small window before the wait costs at most the 4 ms timeout.

**The heaviest band is limited by the CPU.** At 4000 draws or more: frame 91 ms, replay 36 ms, GPU wait 0, and the scheduler's idle share is near 0 % during those stretches. `[GLSPAN]` shows draws to present of about 31 ms at the median. So the 20 % lock acts directly on that band, though how much FPS it returns stays to be seen.

**The smoke was misread.** `CEF661992D489E4B` over 14 samples: 84 and 39 ms in the two frames beside a texture load (`[GLSTALL]` replay 3735: 1,410 ms, of it 422 decode and 893 upload), then 4 to 7 ms in settled heavy frames, with 0.2 to 0.8 Mpx passing. Run 10's sample, replay 10800 at t≈350, came just as the game went from 64 draws to about 5000, so it probably was the same kind of frame; its texture load was not checked. Other programs show the same pattern: 45.84 ms over 54 draws with 0.00 Mpx (`8C2CA6604C9FBA06`), 26.27 ms over 6 draws with 0.00 Mpx (`5F9BA8D0CD24F9D6`). So a large per-program figure in `[GLGPU]` can be a wait charged to whichever draw follows it, and is not fill. Passed samples leave out discarded and depth-failed pixels, so this does not count overdraw, but it bounds what the smoke costs. Whole frames pass 10 to 14 Mpx in levels.

**P works, and the alarm was probably false.** 7.9 M calls, 5.37 M reuses (68 %), 20,983 checks, no mismatch, against one after 326,400 reuses before comparing by bits. Heavy frames, medians of 13 to 21 samples: walk 15.6 ms and record 12.0 ms with P (MAPXS), against 19.6 and 15.6 without (MAXS). `prepare_full` 1.7 → 0.7 % and `fill_fetch_derived` 1.6 → 1.0 % of the heavy turn.

**Captures.** The save windows are gone: 99 to 104 MB of images in 13.9 to 15.6 s (23 to 31 MB on the card), against about 100 s. Writing takes 2.1 to 3.4 s and opening, sizing and closing 3.6 to 5.3 s; the rest is mostly encoding. The session's frames of 500 ms or more: 12, against 101 in run 10. What remains is one freeze of 2 to 4.4 s at the moment of each capture, on the render thread's read-backs, which is the tool's own cost.
The pause between writes shows nothing: frames of 200 ms or more in the following 16 s were 6, 4 and 2, 3, 6 across paced and unpaced, on different scenes, and the two kinds overlap. Since the kick waits above no longer point to the file system, the starvation hypothesis has lost its evidence, though this does not disprove it. The pause should go. Only 5 `[CAPSAVE]` lines for 6 captures: the session ended inside the last save or it merged with another; not checked.

**H.** Hashed 1276 against 2107 pages, re-hashed 99 against 1090, sampled misses 0 again. Walk and record did not clearly improve over MAPXS here (16.3 and 11.3 against 15.6 and 12.0), while XXH3 is 8.9 % of the heavy turn. Still the owner's decision.

**Shader cache.** 32 s of worker time against 196 s, as inferred from run 10, and no compile failure.

**FPS by recorded draws**, gaps of 1 s or more removed, run 10 outside its capture saves:

| Draws | Run 10 FPS | Run 10 median ms | Run 11 FPS | Run 11 median ms | Run 10 audio | Run 11 audio |
|---|---:|---:|---:|---:|---:|---:|
| 300-800 | 25.1 | 36 | 31.2 | 31 | 80 % | 84 % |
| 800-1300 | 20.0 | 46 | 21.6 | 42 | 76 % | 79 % |
| 1300-1900 | 20.2 | 46 | 20.8 | 40 | 82 % | 87 % |
| 2500-3100 | 16.6 | 56 | 19.1 | 50 | 57 % | 71 % |
| 3100-4000 | 14.3 | 70 | 15.6 | 58 | 49 % | 69 % |
| 4000-5000 | 11.9 | 78 | 12.3 | 75 | 69 % | 61 % |
| 5000+ | 9.5 | 100 | 10.1 | 96 | 59 % | 52 % |

Some rows are short (6 to 16 s). The audio queue was empty 6.67 % of the time, longest 819 ms, against 5.25 % and 851 ms, in a session with more heavy play for its length. Audio in heavy scenes is still far from real time.

**Next:** notify without the mutex, remove the pause, and look at the remaining record path: XXH3 8.9, `pm4_extract_ib` 7.5 %.

## CPU plan after run 11

The limit in heavy scenes is one thing: guest threads run one at a time on core 0, and that core has no idle time. Cores 1 and 2 have room (render 29 %, XMA 14 %, Mesa's worker 14 %, averaged over the session). So a faster Deko3D renderer would not lift these scenes until this does.

**Where the turn goes, heaviest band, run 11.** At about 91 ms per frame, by `[SCHEDPROF]`, roughly a third each:

| Thread | Share of turn | What it does |
|---|---:|---|
| Main (`guest_00000000/0`) | 32.7 % | 78 % guest code, spread thin; 14 % of it is also PM4 walking |
| Command thread (`822D2AB0`) | 32.7 % | 99 % host: the PM4 walk and draw records. XXH3 20 %, `pm4_extract_ib` 21 % of this thread |
| Audio (`827DB318`) | 28.6 % | 68 % of it waiting on the lock; the rest is mostly guest mixing |

So the game's own code is about a third of the turn. The rest is this port's overhead, which is good news: most of it does not depend on the generated code.

**Options, by expected gain.** Percentages are of the heavy turn, from samples. None is measured as an FPS gain, and they do not simply add.

| Option | Turn it can free | Risk | Status |
|---|---:|---|---|
| XMA kick without a lock | up to 20 % | Low | `0.0.18-log12` |
| PM4 walk off the turn (W) | up to about 30 % | High: rejected in run 7, cause unknown | Second, separate session in `0.0.18-log12` |
| Not re-walking repeated tile IBs | perhaps 5 % | Medium: a wrong viewport in a later tile | `0.0.18-log12` measures it (`repeat tiles` in `[GPUPAGES]`); replaces the 2 % guessed for `0.0.16-log10` |
| Page hash per frame (H) | up to about 4 % | Owner's: can miss a page changed mid-frame | In A/B, sampled misses 0 in two runs |
| Page hashes on a helper core, valid for the epoch they ran in | up to about 5 % | Medium, complex: most epochs are short | Not started. Sound only with the walk on the turn |
| Native audio mixers `sub_827E51D0`, `sub_827F5990` | 2 to 3 % | Low with a compare mode; `tools/gen_native_reverb.py` is the pattern | Next batch, if audio stays heavy |
| LTO, then direct calls, PGO | about 3 % of the frame, if bo2-360-nx's 10 % holds on the guest third | Build memory, a retrain per change | After the host overhead |
| Plain guest memory instead of volatile | unknown, bo2-360-nx's 5.8 % was shared with other changes | Medium to high | After an audit |
| Guest threads truly in parallel | largest | Largest: the scheduler keeps this game stable | Owner's decision, not routine work |
| Higher CPU clock | direct | None technical; run conditions change | Owner's decision |

**One caution on the lock.** Audio runs at about 52 % of real time in the heaviest band. Its work is per second of sound, so as it catches up it takes back part of what the lock frees: better audio, and less FPS than the 20 % suggests.

**Why W is worth one more look, and why apart.** Run 7's trial ran on core 2 with shader compiles and Mesa's worker on a cold cache, and the walk assumed memory cannot change inside an epoch, which is false off the turn. Both may explain its faults; neither is shown. `profiles/hardware-walk.env` cycles MAPXS and MAPXSW, on core 1 with a new epoch per IB. It remains a test, not a recommendation, and running guest code beside the walk still needs the owner's view before it could ever be default.

**`0.0.18-log12`:** the kick notifies without the mutex, the capture pause is gone, `LSWTCS_WALKOFF_CORE` and the repeat-tile timer are added. Next run: lock share, FPS and audio by band against run 11, repeat-tile time; then W's session: black rows in history frames, `[GSPIKE]`, FPS against MAPXS.

## Run 12: `0.0.18-log12`, two sessions, 2 November 2026

Handheld at stock clocks. Archived in `private/runs/run12-log12` (main) and `private/runs/run12w-log12` (W). No crash, stderr empty in both.

### Main session: the lock is gone

276 s, 8,370 frames, levels `Map\Map` and `Gungan_A/B/C`, three full captures taken for material, no visual fault reported.

- `svcArbitrateLock` no longer appears on the audio thread; the audio thread's share of the heavy turn fell from 28.6 to 13.8 %. Its leaves are now its own mixing: `sub_827E51D0` 18.8 % and `sub_827F5990` 16.9 % of that thread, about 5 % of the turn together.
- Audio queue empty 2.8 % of the session (6.67 % in run 11), longest 604 ms. The heaviest band plays 86 % of real time against 52 %.

| Draws | Run 11 FPS | Run 12 FPS | Run 11 median ms | Run 12 median ms | Run 11 audio | Run 12 audio |
|---|---:|---:|---:|---:|---:|---:|
| 800-1300 | 21.6 | 26.8 | 42 | 33 | 79 % | 94 % |
| 1300-1900 | 20.8 | 26.2 | 40 | 36 | 87 % | 93 % |
| 2500-3100 | 19.1 | 22.5 | 50 | 44 | 71 % | 73 % |
| 3100-4000 | 15.6 | 20.7 | 58 | 47 | 69 % | 77 % |
| 4000-5000 | 12.3 | 13.7 | 75 | 64 | 61 % | 85 % |
| 5000+ | 10.1 | 12.1 | 96 | 75 | 52 % | 86 % |

Rows of run 12 with under 10 s (2500-4000) are indications. As expected, part of the freed turn went to audio, which now keeps up.

- Heavy turn now: main thread 40.5 %, command thread (walk and record) 39.5 %, audio 13.8 %. The scheduler's idle share is near 0 again in those stretches, so the heaviest band is still CPU-limited.
- `repeat tiles` read 0.00 ms in every `[GPUPAGES]` line, heavy frames included. So walk-time tile skipping (`tile_skip_begin`) did not start for the ring-level IBs of this level, while the replay still merged 3,400 or so repeated draws per heavy frame. The repeats are then walked and recorded in full, which may be most of the command thread's time. Not proved: the reason for each rejection goes to `dbg_ram`, which no session keeps. Next build: count and print why.
- Captures: 12 to 16 s per capture, as in run 11.

### W session: rejected again, and the mechanism is narrower now

141 s in the hub, cycling MAPXS and MAPXSW, walk on core 1. The tester saw a black band, then a picture that stopped.

- From the first W phase (t ≈ 45 s), the published frame stays serial 2499 until the end: every later replay repeats it (`2499R` in `frames.log`, 2,332 times), as in run 7. Audio stayed at 100 % and guest threads ran.
- `[GSPIKE]` after that shows walk 0.0 ms: the ring was no longer walked at all. The game's command thread (`822D2AB0`) spent 95-96 % of its samples in every later window in `KeWaitForSingleObject` called from `sub_822D2AB0`. It waits for something that never comes, so it never kicks the ring again.
- So W does not just draw wrongly: once the walk has run beside guest threads, an event the command thread waits for is lost or never set. Candidates, none checked: a GPU interrupt or a fence write (`pm4_gpu_write`) racing a guest thread that tests the fence and then waits, or the order of `walk_wait_other` and the walker's rejoin.
- The capture, taken after the stop, shows the stuck hub frame without a band; the band itself was not captured.

W stays out. Moving the walk off the turn would need the waits and interrupts of the command thread understood first, which is a scheduler change for the owner to weigh, not a setting to try.

### Next

1. Print why walk-time tile skipping rejects ring-level IBs in this level, with counters per frame. If the repeats can be skipped safely, it is the largest CPU gain left that keeps one guest thread at a time.
2. Native audio mixers `sub_827E51D0` and `sub_827F5990`, with a compare mode: about 5 % of the heavy turn now.
3. H remains the owner's decision.

## `0.0.19-log13`: repeated tiles checked draw by draw (state T), not yet run on hardware

**Why.** In the heavy band the command thread (PM4 walk and draw records) is about 40 % of the guest turn. The game draws its world once per EDRAM tile, three times in Episode I chapter 2, and the replay merges the tiles and draws tile 0's records only, so recording tiles 1 and 2 serves mostly to prove they match. Walk-time tile skipping exists for that, but `repeat tiles` read 0.00 ms in run 12: its entry check refused the repeats. Exact pc samples (`private/session-tools/hot_lines.py`) put the refusal path itself among the walker's hot lines. The refused register is unknown because the reason went to `dbg_ram`, which no session keeps.

**What changes.**

- A hash of the whole register file is kept current on every write of the walk (`reg_set`, `pm4_regs_copy`): XOR of a 64-bit mix of register and value, zero values contributing nothing. The per-tile registers and the two written directly are left out.
- State T: a repeat starts without the entry check. Each draw is copied from tile 0's only if the register hash, the shader addresses and sizes and the draw packet equal tile 0's at the same draw. From the first draw that differs, the rest of the IB is recorded in full, so a copy is never taken against different registers short of a 64-bit collision.
- One repeat in 16 under T is recorded in full anyway, and each draw whose hash matched is compared with tile 0's record: targets, blend, stencil, shaders, constants (vertex, pixel, bool and loop, fetch, derived), vertex ranges, indices, textures and samplers. The viewport-derived system constants are not compared, since tile copies patch them. A difference prints `[TILESKIP] FALSE MATCH` and turns T off for the session.
- `[TILESKIP]` beside `[GPUPAGES]`: repeats by the entry check, refusals and the first differing register of each, repeats under T and how many a hash stopped, draws copied, and the full checks.

**What it does not cover.** Guest memory read through the copies (vertex data, indices, shader microcode) is tile 0's. It could change between tiles only when guest code runs between them, as the existing skip already assumed; the full checks would see it in the sampled repeats.

**A/B.** `MAPXS,MAPXST,MAPXSTV,MAPXSH,MAPXSTH`. MAPXST against MAPXS measures the gain; MAPXSTV checks the picture with T on; MAPXSTH is both CPU savings together.

**Reading the next run.** `repeat tiles` and `walk`/`record` per frame by state; `[TILESKIP]` false matches must stay 0; FPS and audio by band against run 12. If T holds, the next candidates are the native audio mixers (about 5 % of the turn) and, larger and harder, page hashing (about 10 %): either H, which is the owner's decision, or tracking guest writes to vertex pages with memory protection, which would replace hashing by proof but is a large change.

## Run 13: `0.0.19-log13`, 2 November 2026

Handheld at stock clocks, 8,610 frames in 326 s, levels `Map\Map`, `Gungan_A/B/C`, five full captures for material, no visual fault reported. Archived in `private/runs/run13-log13`.

**T ran for about 20 s and then turned itself off.**

- Under T, 1,328 repeats began and the hash stopped 1,324 of them at their first draw, so almost nothing was copied under T.
- One full check compared 725 draws: 724 equal, one `FALSE MATCH` at draw 723 (VS `2E84CD15F17C7538`, PS `A57505D45FC84D0D`), at t ≈ 100 s, inside the load from `Gungan_A` to `Gungan_B` with new shaders arriving. T switched off there, so the T states after it ran without T and the state table says nothing about T.
- Which field differed was not printed. A level load is when guest memory changes between the tiles of a frame, so stale vertex, index or constant memory is the likely cause, not the register hash. That staleness applies equally to the copies of the entry check, which ran all along. In merged frames it would not show, since the replay then draws tile 0's records for the whole screen.

**The entry check's refusals now have a name.** With T off, 4,212 repeats passed it and 8,562 were refused. Every refusal was on register `0x0A2F`, next to `COHER_STATUS_HOST` (`0x0A31`): Xenia's `COHER_SIZE_HOST`, set by the game's cache-coherency packets around resolves. No draw reads it. It also explains T: the hash covered it, so it differed at the first draw of each repeat. Copies made: about 2.2 M over the session. `repeat tiles` now reads 4-8 ms per heavy frame.

**FPS.** Compared with run 12: 12.4 against 12.1 at 5000+ draws, 15.7 against 13.7 at 4000-5000, about equal elsewhere. That is within the noise of different routes, as expected without T.

**Next, small.**
- Leave the coherency registers (`0x0A2F` to `0x0A31`) out of both the entry check and the hash.
- Print which field a false match differed in.
- Turn T off after three false matches instead of one, so a level load alone does not end the test.

## `0.0.20-log14`: T with the coherency registers left out, not yet run on hardware

The cache-coherency registers `0x0A29` to `0x0A31` (`COHER_SIZE/BASE/STATUS` for PM4, RT and host, by Xenia's register table) are left out of the tile entry check and of the register hash. A false match now names the field that differed and the level, and the third turns T off instead of the first. Same A/B states as `0.0.19-log13`. Compiled and validated; the draw-identity and PNG tests pass.

Reading it: under T, `stopped by a hash` should now be a small part of the repeats and `draws copied` large; `repeat tiles` and `walk` per frame by state; any `FALSE MATCH` with its field. If differences show up only in vertex constants, indices or vertex ranges during loads, that is guest memory changing between tiles.

## Run 14: `0.0.20-log14`, 2 November 2026

Handheld at stock clocks, 10,290 frames in 403 s, levels `Map\Map`, `Gungan_A/B/C/E`, six full captures for material, no visual fault reported. Archived in `private/runs/run14-log14`.

**T does not pay, and stops here.**

- With the coherency registers out, T copied draws: 8,415 draws were checked in full and 8,412 matched. The hash still stopped 1,182 of 1,296 repeats partway. The entry check's refusals now name real draw state: `RB_ALPHA_REF` (`0x210E`), `SQ_INTERPOLATOR_CNTL` (`0x2182`) and `VGT_EVENT_INITIATOR` (`0x21F9`). These are values left over from the end of the previous tile, which differ at some draws before the IB sets them again. A copy there would be wrong for that draw whenever it reads them, so the hash is right to stop.
- The three false matches were all "block sizes", for the same shaders (VS `2E84CD15F17C7538`, PS `A57505D45FC84D0D`) in `Gungan_B`. That is a fault of the check, not of the copies: `range_count` counts upload runs of pages not yet uploaded this frame, so a full record of a later tile legitimately has fewer. The check should not have compared it. T switched itself off after the third.
- Medians of heavy `[GPUPAGES]` lines by state, about 20 samples each: walk 20.1 ms with T against 19.2 without (MAPXST against MAPXS), record 13.7 against 14.0. No gain, and FPS by state shows none either.
- So in this level the repeated tiles are not where the walk's time goes. Telling which repeats would be safe draw by draw needs to know which registers each draw reads, which is not worth it for what is left.

**H is now the clearest CPU gain.** Same medians: walk 12.4 ms and record 8.4 ms with H (MAPXSH) against 19.2 and 14.0 without, pages hashed 1,080 against 2,550. Sampled misses are 0 again, as in every run since `0.0.16-log10`. The state slices land on different scenes, so the size is an indication. The decision stays the owner's: H trusts a page unchanged for 8 frames, which the source comment rejects as proof, and the sampled misses only bound how often that would be wrong.

**FPS** against run 13 is the same within noise in every band (12.5 against 12.4 at 5000+).

**Next.** Native audio mixers (about 5 % of the turn), then a trial of LTO on the runtime and generated code. Take H to the owner with the measurements from runs 10 to 14.

## What the 360 version targets, and the fourth core

**The 360's frame rate: not settled by the code, and probably 60 FPS.** An earlier version of this section concluded 30 FPS from a source comment, and that was wrong. `sub_8229D8B8` stores one of two constants from the executable image into the frame-length floor (`0x830AA980`), depending on what `sub_824FC358` returns (3 picks the first). The values of those constants were not read here, and the function has no direct caller in the generated code, so whether gameplay runs with a 1/30 floor is unknown. The tester reports that several sources say the 360 version runs at 60 FPS, and the port's main loop already lets the game run at 60 with game time following real time. So 60 FPS is treated as the original target.

Why a game that ran on the previous generation is heavy here: the 360 runs its threads on three 3.2 GHz cores (six hardware threads), and its GPU reads the command buffers itself. Here every guest thread shares one 1.02 GHz core, one at a time, through recompiled code, and the GPU's command processing is software on that same turn: the command thread is about 40 % of it in heavy scenes. The cost is the translation, not the game.

**The fourth core, as bo2-360-nx does it.** From its public source (`sdk/src/ui/switch_core3.cpp`, techniques only):

- It needs a forwarder title whose NPDM allows cores 0-3; a normal homebrew launch gets cores 0-2.
- It wraps `svcSetThreadCoreMask` and `svcSetThreadPriority`, because new threads otherwise take the whole process mask.
- It moves named threads, its mixer and XMA decoder, to core 3, all at the lowest priority. Other threads stay on 0-2.
- A meter moves everything back if those threads use more than 85 % of core 3 for 2 s. Its author reports a Switch Lite hard freeze from system services starved on that core, which is why the meter exists.
- Measured there: +16 % on a Lite, +11.9 % on an OLED.

Here the limit is the guest turn on core 0, which core 3 does not lift by itself. What could move there is the XMA worker, the audio output and the profiler, now on core 2 beside Mesa's worker. That would help the render side and Mesa, not the turn. Installing a forwarder title and risking a freeze are the owner's and the tester's decisions. `0.0.21-log15` only logs the process core mask (`[NX] process core mask`) so that each session records its launch route.

## `0.0.21-log15`: native audio mixer (state N), not yet run on hardware

- `native_mixer.cpp` replaces the gain-ramp mixer `sub_827E51D0` (about 19 % of the audio thread's samples) in state N. It computes as the recompiled code does. That includes the build's fused multiply-add: the guest version's disassembly has four FMLA in its loop, so the native one uses `vfmaq_f32` there and keeps the gain additions separate.
- Every 64th call the guest code runs on the real buffer and the native code on a copy, and the bytes are compared. The first difference prints `[NATIVEMIX] MISMATCH` and leaves the guest code in place for the session. The equality is not tested on a PC.
- A/B: `MAPXS,MAPXSN,MAPXSH,MAPXSHN`, so N, H and both together are compared in one session.
- Reading it: `[NATIVEMIX]` must show no mismatch; the audio thread's share of the heavy turn by state; FPS and audio by band.

## Run 15: `0.0.21-log15`, 2 November 2026

Handheld at stock clocks, 10,020 frames in 389 s, levels `Map\Map`, `Gungan_A/B/C/E`, five full captures for material, no visual fault reported. Process core mask `0x7`: a normal homebrew launch with three cores. Archived in `private/runs/run15-log15`.

**N is exact, and small.** `[NATIVEMIX]`: 2.56 M calls, 40,000 checks, all equal, so the native mixer computes the guest's result bit for bit. Its cost in the heavy band's turn: the native loop 1.11 % of samples against 1.60 % for the guest function, each running about half the session (the guest share also holds the 1-in-64 checks). That is roughly a third cheaper per call, about 0.5 % of the turn: the loop is bound by memory more than by the context traffic it removes. Worth keeping on, but not a large gain, and the second mixer (`sub_827F5990`, similar size) would give about the same.

**FPS.** Against run 14 within noise: 13.3 against 12.5 at 5000+ draws, 15.3 against 14.2 at 4000-5000, the rest equal. By state (10-40 s per cell, different scenes) no ordering between MAPXS, N, H and both is visible beyond noise in the heavy band, while `[GPUPAGES]` keeps showing H's lower walk time.

**Heavy turn now:** main thread 44 %, command thread 41 %, audio 12 %.

## `0.0.22-log16`: H against Q, the fourth core ready, not yet run on hardware

- **H protects itself.** H (per-frame page hash, measured since run 10: walk 19.2 to 12.4 ms, record 14 to 8.4 ms, no sampled miss in any run) now turns itself off for the session at its first sampled miss and prints `[GPUPAGES] H MISS: page ... H off for this session`, as P and N do. Making H the default stays the owner's decision; this makes trying it safe in long sessions.
- **State Q: a guest thread switch every 4th quantum.** Run 15's heavy band handed the guest turn over about 1,600 times a second, each a kernel wake and sleep with no guest code running. Q stretches the quantum fourfold (`lsw_quantum_div_set`, the same path as `LSWTCS_QDIV`). Its risk is audio latency: if the audio thread waits longer for the turn, `[AUDIO]` fill falls. A still gives audio the turn first.
- **A/B:** `MAPXSN,MAPXSHN,MAPXSNQ,MAPXSHNQ`, N on everywhere (exact in run 15). Two factors crossed, so H and Q are each read across two states.
- **Fourth core, inert on a homebrew launch.** `lsw_core3_begin` (platform_switch.cpp) moves the calling host thread to core 3 at the lowest priority when `LSWTCS_CORE3=1` and the process mask includes core 3. The XMA worker, the log writer and the background file thread call it. A meter on core 2 sends them all home if they use more than 85 % of core 3 for 2 s, and prints `[CORE3]`. With mask `0x7` it does nothing. A launch with core 3 needs a forwarder title, which this repository does not build and which the tester or owner would install themselves.
- **Reading it:** `[GPUPAGES]` for an H miss; FPS per band by state; the guest-turn handoff rate and `[AUDIO]` fill under Q; `[NX] process core mask` and `[CORE3]`.

**Guest threads on several cores: not attempted, and why.** All guest threads take one turn on core 0 (the cooperative scheduler). Letting two run at once looks possible in part: guest atomics are real compare-and-swap, and guest critical sections map to host mutexes. But the kernel stubs (events, semaphores, thread state, the GPU ring) assume only one guest thread runs, and the recompiled code relies on the turn for memory ordering the 360 gave by `lwsync`/`sync`. Lifting that is an audit of every kernel stub and of barrier translation, with data races that show up only as rare corruption. It is an architectural decision for the owner. The least risky first step would be one thread that touches little shared state, the audio thread (12 % of the heavy turn), on its own core.

## `0.0.23-log17` (draft): why W stopped the picture, and the fix

**What W does.** The thread that walks the ring (VdSwap, or a guest wait for GPU progress) leaves the guest turn for each large IB and walks it on core 1, so the other guest threads run meanwhile. Guest code inside the walk (the GPU interrupt, wait-register polls) takes the turn back first (`WalkNeedsTurn`). Other threads that need the ring walked wait for the walker (`walk_wait_other`).

**The fault, found by reading the code, not yet confirmed on hardware.** `g_walk_off_sid` was cleared at the end of each IB, and a waiter woken by that went on as soon as it got the turn back, without checking again. It gets the turn when the walker leaves it, which is usually for the next IB. So it found a walk still going on, and:
- a GPU wait (`pm4_walk_now`, behind BlockOnFence, ring-space and phase waits) took its re-entrant path and reported the awaited fence or ring space done while the walker had not read it yet. The game then reuses that memory, which is what a black tile band and a frozen picture would look like;
- a VdSwap that waited started a second walker over the same ring, with the same globals.

Neither happens with the walk on the turn, because then no other guest thread runs during it. The tile worker stuck in `KeWaitForSingleObject` in run 12 fits a lost or corrupted tile list, but that link is not shown.

**The fix.** `g_walk_off_sid` is now set exactly while the walker is off the turn: set before it leaves, cleared after it has the turn back, also around `WalkNeedsTurn`. A waiter checks again every time it holds the turn. Other threads then see what they see with the walk on the turn. `[GPUPAGES]` counts `walk-off waits` and `again`: the second number is how often the old code would have gone on wrongly.

**Not covered.** With the walk on the turn, a wait-register poll inside the walk yields the turn with the walk half done, and a VdSwap then could start a second walker; the fix keeps that as it was. Guest threads now also write memory while it is walked. The 360's GPU had the same contract, but the H page hash (new epoch per IB in W) may see more mid-frame changes.

**What it could give.** In the heavy band the walk and recording are about 20 ms of a 75 ms frame with H. Off the turn, the main thread (44 % of the turn) and audio can run beside them, so the gain is at most the walk's share. The thread that kicked the walk still waits for it. Xenia's model goes further: its command processor is a thread of its own that reads the ring while every guest thread runs. That is the next step if this one holds.

**Session:** `profiles/hardware-walk.env`, `MAPXSN,MAPXSNW,MAPXSHN,MAPXSHNW`, walk on core 1, separate from the main A/B session.

## Run 16: `0.0.22-log16`, 2 November 2026

Handheld at stock clocks, 10,950 frames in 401 s, `Map\Map` and `Gungan_A/B/C/E`, five full captures for material, no visual fault reported. Core mask `0x7`, so `lsw_core3_begin` stayed inert. Archived in `private/runs/run16-log16`.

- **H: no sampled miss again** (`H MISS` never printed). Heavy frames (5000+ draws), median frame time by state: MAPXSHN 70 ms (25 frames only) against MAPXSN 87; MAPXSHNQ 79 against MAPXSNQ 84. H is ahead in both pairs, as in runs 12-15.
- **Q is out.** In heavy windows the guest scheduler switched 1,500-2,200 times a second without Q and 1,700-4,100 with it, so the switches come from guest waits, not from quantum ends. No FPS gain either (above). The code stays, unused.
- **N:** no mismatch.
- **FPS** in the heaviest band 11.7 against 13.3 in run 15, but the scenes were heavier: the render thread's replay took 40-49 ms per heavy frame against 33-37 ms, and walk plus record reached 35 + 20 ms in some windows. The other bands match run 15.
- **Audio** 97-100 % in every band, against 70-96 % in run 15. The cause was not found; no setting that touches audio changed except N everywhere, which is too small to explain it.

## `0.0.23-log17`: W fixed, and logged in detail

- The W fix described above, plus a W-only change: with W on, a walk that finds another one half done (its thread yielded on the turn inside a wait-register poll or the guest interrupt) leaves the ring to that walker instead of becoming a second walker. Without W this is only counted.
- `[WALKOFF]` every 5 s while W walks: IBs off the turn, time off, the walker's wait to get the turn back (average and maximum), guest code run inside the walk, fences reported done during a half walk, second walkers, and the waits from the fix. When no walk has ended for 3 s while the game's write pointer is ahead (the stopped picture of runs 7 and 12), `NO WALK` and a line with the walker's state, the tile backlog and who holds the turn.
- Main session `profiles/hardware-ab.env`: `MAPXSN,MAPXSHN`. W session `profiles/hardware-walk.env`: `MAPXSN,MAPXSNW,MAPXSHN,MAPXSHNW`, walk on core 1.

## Run 17: `0.0.23-log17`, 2 November 2026, two sessions

**Main session** (`private/runs/run17-log17`): 9,840 frames in 371 s, `Map\Map` and `Gungan_A/B/C`, `MAPXSN` against `MAPXSHN`. Heaviest band 12.5 FPS (11.7 in run 16); H again ahead there (14.0 against 11.8 FPS) and no sampled miss. No black rows in the 200 history frames. Audio 70-100 % by band, lower than run 16's; not looked into.

The tester saw small faults around 13:29-13:30. One is in capture 6549 (`Gungan_C`, `MAPXSN`): the studs on top of a brick show through the legs of the characters standing on it. It is the same in the unmerged history frame (6549 `_u`) and the merged one before it, so tile merging is not the cause. A depth or draw-order fault of the stud draws; not investigated yet, and whether earlier runs had it is not known.

**W session** (`private/runs/run17w-log17`): crashed in the hub after 61 s, before any level.
- The fix of log17 was needed: `[WALKOFF]` counted 5 waits in each 5 s that would have gone on wrongly (`again 5`), out of 23-26. Up to 1.2 s of walking per 5 s went off the turn (165-242 IBs).
- Still black tile bands in W: 5 of 40 history frames (the unmerged ones) have 45-91 black rows; capture 2162 shows rows 320-620 black. The draw count per frame jumped by about 320 from frame to frame (1123, 1445, 1767, 2090), roughly one tile's records, so tile records landed in the wrong frame.
- The crash: a corrupted free list in rpmalloc, reached from SPIR-V translation of a new shader (`[SHADERNEW]` just before) inside a walk by the tile worker (`sub_822D2AB0` → `sub_822D2798` → ring-space wait → `pm4_walk_now`). The bad pointer holds ASCII ("xe_u"), part of a SPIR-V name.
- The cause found in the code: during a VdSwap walk, the tile drain yields the turn until the tile backlog reads 0, then walks on. The tile worker pops its queue and then walks the segments it kicked, off the turn with W, so both threads walked the ring and filled the same frame, and both used the shader translator, at once. `walk_wait_other` was never called there. That fits the jumping draw counts, the bands and a heap corrupted from two threads; not proved.
- The `NO WALK` lines at boot were a false alarm (the menu's ring sits at 25/22 before any walk); log18 reports a stopped walk only after a walk has run.

## `0.0.24-log18`: the tile drain waits for the tile worker's walk

- With W on, the drain goes on only when the backlog is 0, no walk is off the turn and none is half done on the turn; it waits for an off-turn walk with `walk_wait_other`. Without W the drain is unchanged and such cases are only counted.
- `[WALKOFF]` adds `tile drain found a walk N (waited M)`.
- Sessions as for log17: main `MAPXSN,MAPXSHN`, W `MAPXSN,MAPXSNW,MAPXSHN,MAPXSHNW`.

## Run 18: `0.0.24-log18`, 2 November 2026, two sessions

**Main session** (`private/runs/run18-log18`), `MAPXSN` against `MAPXSHN`: heaviest band 12.7 FPS; H ahead again in the heavy bands (13.7 against 11.8 FPS at 4000+ draws) with no sampled miss. The studs fault came back at the same brick in `Gungan_C` (capture 6142, `MAPXSN`). The tester also notes dark outlines around that brick that should not be there.

**W session** (`private/runs/run18w-log18`): no black bands this time, and the drain counters show what the log18 fix caught: 22-149 times per 5 s the drain found the tile worker's walk under way and waited. The picture then stopped in the hub at replay 2114 (state `MAPXSNW`) while the game, audio and pause menu went on. Every later `[GSPIKE]` shows walk 0.0 ms: the game no longer submitted work. Both tile worker threads (`sub_822D2AB0`) sat in `KeWaitForSingleObject` for 95 % of their samples, as in run 12.

The cause found in the code: VdSwap set the global `g_interrupt_ctx` before waiting for a walk off the turn. That walk (the tile worker's `pm4_walk_now`) had saved a null context and restored it when it ended, so VdSwap then walked with no context. `pm4_fire_interrupt` returns early without one, so the GPU interrupts in that part of the ring were dropped. The tile list they queue never reached the worker, which waits for its event for ever. Not proved; it fits a stop with no black band.

## `0.0.25-log19`

- **W:** VdSwap waits for an off-turn walk before setting the interrupt context. `[WALKOFF]` counts `GPU interrupts lost` (a dropped interrupt for want of a context, in any session).
- **Polygon offset, state O.** The GL backend never applied the Xenos polygon offset (`PA_SU_SC_MODE_CNTL` bits 11-13, `PA_SU_POLY_OFFSET_*`). It is now recorded per draw, as Xenia picks the face, and applied in state O: the slope scale in 1/16 subpixels, the offset times 2^24 on the float32 depth buffer, as Xenia's host-render-target path. Whether the studs use it is not known; the capture log now prints the mode register and both values (`scm=`, `po=`).
- **`[GLPROBE]`** in full captures: after each draw into a scene-wide colour target, which of 32×18 screen cells changed. That names the draw that put the studs over the characters, and the outline draw.
- Main session `MAPXSHN,MAPXSHNO`; W session `MAPXSHN,MAPXSHNW`.

## Run 19: `0.0.25-log19`, 2 November 2026, two sessions

**W works now.** The W session (`private/runs/run19w-log19`, `MAPXSHN,MAPXSHNW`) played through `Gungan_A` to `E` and back to the hub with no stop and no black band; `[WALKOFF]` counted no lost GPU interrupt. So the faults of runs 7, 12, 17 and 18 were the three races fixed in log17-log19.

**But W gains little, and why.** In the heavy band W gave 9.9 FPS against 8.2 in the same session (p95 128 against 186 ms). The tile worker's share of the guest turn fell from about 36 % to 2 % while its CPU stayed at 26 %: its walk ran off the turn. Yet the guest scheduler sat idle 27-42 % of the time. VdSwap waited for that walk, then waited in its tile drain for the worker to empty its queue, so the main thread waited instead of running the next frame. The 360's VdSwap only queues the swap. Log20 tests that (states Y and D).

**The whole W session rendered slowly** (render thread 88-90 ms per heavy frame in both states, against 35-38 ms in the main session), so its absolute FPS is not comparable with the main session. Replay cost per 1000 draws doubled after the first full capture in run 19's main session (6.8 → 14.0 ms in the same level). Earlier runs show smaller rises after captures, so captures, the new probe or both may leave the renderer slower. Not resolved; log20 turns the probe off unless set, and the main session asks for captures only at the end.

**Studs over the characters (captures 5942, 6148, 6207).** The probe shows the studs and brick come from early draws (#51-#57, program 374, `vs 8B12FE19`, `ps AB23B41D`), opaque with depth test and write on. The characters come later. So the legs fail the depth test where the studs are: the studs' depth is nearer than it should be, or the characters' is farther. Those draws have `po=0/0` and no offset bits in `PA_SU_SC_MODE_CNTL`, so polygon offset (state O) is not the cause, and O is dropped. The stud pixel shader samples a cube map while a 2D texture is bound (`DIM-MISMATCH dim1/3`). That would read black and may be the dark outline the tester saw; it would not move depth. Log20 adds each draw's viewport depth range to the capture log.

**Render thread in the main session's heavy band** (`[GLPROF]`): replay CPU 42-54 ms per frame, of which `plan` 12.5-13.7 ms, `fence` 4-8 ms, `bind` 2-10 ms, against about 40 ms of GPU. The render thread is close to being the limit after the guest turn, and `plan` is the largest single part.

## `0.0.26-log20`

- **Y** (with W): VdSwap does not wait for a walk under way, off the turn or half done on it; it leaves the ring to that walker, which reads the write pointer again before it stops. It also no longer sets and clears the interrupt context over a half walk. **D** (with W): VdSwap's own walk skips the tile drain. D can bring back what the drain was added for (the worker lagging behind recycled command buffers), so it is in the second session only.
- `[WALKOFF]` adds `swaps left to a walk` and `swaps without drain`.
- `[GLPROBE]` only with `LSWTCS_GLPROBE=1`; the capture log adds `z=min..max` per draw.
- Main session `MAPXSHN,MAPXSHNW,MAPXSHNWY` (walk on core 1); second session `MAPXSHNWY,MAPXSHNWYD`.

## Run 20: `0.0.26-log20`, 2 November 2026, two sessions

Both sessions (`private/runs/run20-log20`, `run20w-log20`) played `Gungan_A` to `E` without a stop or a visual fault from W, Y or D. Captures came only near the end, as asked.

**Both sessions were render-limited, in every state.** At 5000+ draws the frame took 94-108 ms. The render thread's replay took 82-96 ms of it, against 35-38 ms in runs 18 and 19 (main sessions), so the states cannot be ranked here: 9.6 (no W), 9.8 (W), 10.4 FPS (W+Y); 10.3 (W+Y) against 10.0 (W+Y+D) in the second session.

What grew is `plan` (tile-merge planning, mostly XXH3 hashing of each tile draw's constants and geometry): 53-58 ms per heavy replay against 13.4-13.7 ms. It grew in the states without W as well. But the bytes are the same (the arena per replay matches run 19), the CPU clock read 1020 MHz throughout, and the render thread's CPU time per frame is unchanged (about 20 ms: 20 % of the core at 10 FPS, against 30 % at 14 FPS). So the render thread got the core for only part of its wall time. Every session that used W at any point shows this (run 19's W session, both of run 20); run 19's main session, without W, does not. The working guess is that something W does leaves a thread competing with the render thread on core 1 (the walk runs there). Log21 records each thread's core mask and priority every 60 s (`[THREADS]`, also in `hwprof.txt`) and the first returns from an off-core move (`[OFFCORE]`).

**Studs:** every draw in the stud capture uses the full depth range (z 0..1), so the fault is in the depth the vertex shader computes, or in the order of the depth writes. Still open.

## `0.0.27-log21`

- `[THREADS]` and `[OFFCORE]` as above. No behaviour change.
- Main session: `MAPXSHN` only, as the baseline without W. Second session: `MAPXSHN,MAPXSHNWY`.

## `0.0.28-log22`: one session in two phases, and a placement guard

The tester asked for one session per run with more in it. Log21 was not run; log22 replaces it.

- **Phases.** `LSWTCS_ABSTATES` applies until the player first comes back to the hub from a level (`[ABPHASE] phase 2`), then `LSWTCS_ABSTATES2`. The first pass through the chapter is `MAPXSHN` alone, a baseline nothing earlier in the session can have touched. The second pass cycles `MAPXSHN`, `MAPXSHNWY` and `MAPXSHNWYK`.
- **K** (with W): the off-turn walk goes to core 2 instead of core 1, which holds the render thread.
- **Placement guard** (`LSWTCS_PLACEGUARD=1`, in the profiler thread): once a second, a guest thread that is not in an off-core move but no longer has the core mask or priority it started with gets them back, and `[PLACEMENT]` prints what it had. If the render slowdown of runs 19 and 20 comes from a thread left on core 1, phase 2 shows the guard firing and no slowdown. If the slowdown stays with no `[PLACEMENT]` line, the cause is elsewhere.
- `[THREADS]` and `[OFFCORE]` from log21; each `[GLPROF]` line now ends with the A/B state.
- `profiles/hardware-walk.env` is no longer staged.

## Run 22: `0.0.28-log22`, 2 November 2026, one session in two phases

Two passes through `Gungan_A` to `E`, phase 2 from the return to the hub at replay 8263, no crash, no capture. Archived in `private/runs/run22-log22`.

**The render slowdown of runs 19 and 20 was not W.** `MAPXSHN` ran the same in both phases (13.1 and 14.0 FPS at 5000+ draws, render 33-38 ms per heavy frame, `plan` 3.0 ms per 1000 draws in phase 1).

**Thread placement, as `[THREADS]` shows it:**
- Every guest thread has ideal core -1, mask 7 (cores 0-2) and priority 0x3B. The render thread is on core 1 at 0x3B too, so a guest thread may run on its core at the same priority.
- `bg_files` (0x3A) and the file loader's off-turn LZ2K decompression (core 1, 0x30) rank above the render thread on its own core.
- The guard fired 30 times. 11 were the file loader (`82508C48`) left on core 1 at 0x30 after its decompression: `[OFFCORE]` shows the saved placement was already core 1/0x30, so an earlier return had not taken. Restoring "ideal core -1" does not always move the thread back.
- The other 19 were the audio guest thread (`827DB318`), which lives on core 2 at 0x2B through the native reverb's moves.

That is the likely mechanism of runs 19-20 (a loader thread stuck above the render thread on core 1). Why it showed only with W there is not known.

**W is out.** Phase 2 at 5000+ draws: `MAPXSHN` 14.0 FPS; `MAPXSHNWY` 8.7 FPS with the render thread at 82 ms (the walk on core 1 beside it); `MAPXSHNWYK` 8.8 FPS with the render at 27 ms, yet slower. Off the turn, on a shared core and at a low priority, the walk takes longer, and the game still waits for it (fences, ring space, the tile drain). The GPU's command processing stays on the guest turn; a real gain would need the guest threads themselves in parallel.

## `0.0.29-log23`

- Off-core returns name the lowest core of the saved mask as the ideal core, instead of -1, and log a failed return.
- A/B states for thread placement, as a ladder over the session: `MAPXSHN`, `R` (render thread 0x2C), `RG` (also guest threads at 0x2E), `RGC` (also guest threads on core 0 only). The placement guard follows the state.

## Run 23: `0.0.29-log23`, 2 November 2026

One pass through `Gungan_A` to `E`, no crash, no capture. Archived in `private/runs/run23-log23`.

- **Faster than run 22 in every band** (10-20 %): 14.0 against 11.5 FPS at 5000+ draws (p95 88 against 129 ms), 29.1 against 26.2 at 800-1300 draws. Today's placement (`MAPXSHN`) got no heavy frames this time, so the comparison is with run 22 over the same level, and partly with the other states.
- **By state at 4000+ draws** (14-15 s each, thin): R 13.6 FPS (p95 101), RG 13.9 (p95 89), RGC 14.9 (p95 78). At 1300-2500 draws: today's 24.6, R 25.7, RG 27.8, RGC 28.4. No state had more long frames than another (0-2 of 500 ms or more).
- **Audio got worse with G and C:** filled 96 % with R alone in the heavy band, 65 % with RG, 60 % with RGC. `[THREADS]` shows G and C moved the XAudio thread (`827DB318`) too: to core 0 at 0x2E, where it otherwise moves between core 2 at 0x2B (the native reverb) and its start.
- The off-core return fix held: no failed return was logged. The placement guard still fired 30 times, for the file loader and the XAudio thread, as in run 22.

## `0.0.30-log24`

- G and C leave the XAudio thread's placement alone.
- States `MAPXSHN`, `MAPXSHNRG`, `MAPXSHNRC`, `MAPXSHNRGC`: G and C crossed, both with R, against today's placement.
- `[PARALLEL]` (added before staging): in frames of 1300+ draws, every 2 ms, whether a guest thread holds the turn, how many others are
  ready and waiting for it, and which. It says whether guest threads on several cores (route 1 in `CPU_ROADMAP.md`) has anything to gain.

## Run 24: `0.0.30-log24`, 2 November 2026

`Gungan_A` to `E`, back to the hub and `A`-`B` again; no crash, no capture. Archived in `private/runs/run24-log24`.

**Guest threads wait for the turn most of the time (`[PARALLEL]`, six 60 s windows, frames of 1300+ draws).**
- A guest thread held the turn in 88-100 % of the 2 ms ticks.
- At least one more thread was ready and waiting in 62-76 % of them, two or more in 41-62 %.
- By thread, the share of ticks spent ready but waiting:

| Thread | Waiting |
| --- | --- |
| Main thread | 43-56 % |
| XAudio | 28-40 % |
| Second tile worker (`822D2AB0`) | 14-18 % |
| `82356D18` and the two `823592C0` | 9-15 % each |
| File threads (`824FD9A8`, `82508C48`) | 0-41 %, during loads |

So a second core has work waiting for it most of the time. "Ready" includes threads that only poll and yield, so this is an upper bound on what could run at once, not a gain.

**FPS:** the best so far: 15.2 FPS at 5000+ draws (run 23: 14.0), 17.0 at 4000-5000 draws (run 23: 15.3).

By state at 4000+ draws:

| State | FPS | Audio filled |
| --- | --- | --- |
| Today's placement | 16.2 | 67 % |
| RC | 14.9 | 69 % |
| RG | 16.6 | 65 % |
| RGC | 15.3 | 40 % |

C costs audio and gives nothing, and is dropped. R and G are within noise of today's placement. R alone was not separated in runs 23-24.

**Audio in the heavy band is worse than before as FPS rises:** 57 % filled overall at 5000+ draws, against 89 % in run 22 at 11.5 FPS. The XAudio thread waits for the turn 28-40 % of the time. When the turn serves more game frames, the audio gets less of it. Route 1 (`CPU_ROADMAP.md`) would remove that conflict: the XAudio thread on its own core.

**Run 25** (same build, profile only): `MAPXSHN`, `MAPXSHNR`, `MAPXSHNRG`, to settle R and G against audio.

## Run 25: `0.0.30-log24` (profile `MAPXSHN,MAPXSHNR,MAPXSHNRG`), 2 November 2026

`Gungan_A` to `E` once; no crash, no capture. Archived in `private/runs/run25-log24`.

| State | 4000+ draws | 2500-4000 | 1300-2500 |
| --- | --- | --- | --- |
| Today's placement | 13.9 FPS, audio 96 % | 20.8 FPS, 84 % | 27.9 FPS, 95 % |
| R | 15.9 (8 s only), 57 % | 23.5, 66 % | 24.2, 96 % |
| RG | 15.1, 80 % | 25.8, 62 % | 29.5, 81 % |

**R and G, runs 23-25 together.** In the heavy bands, R and RG are about 10-15 % faster than today's placement, and the audio is about that much worse. It is one trade-off, not a free gain: the XAudio thread shares the one guest turn, and whatever runs the game faster leaves it less. `[PARALLEL]` matches run 24 (the main thread waits about 50 % of the time).

**Default left as it is.** Audio is one of the owner's first priorities, so trading it for frame rate is the owner's call. The route that removes the trade-off is moving the XAudio thread to a core of its own (route 1 in `CPU_ROADMAP.md`). The profile stays `MAPXSHN,MAPXSHNR,MAPXSHNRG` until then, to gather more of the same.

## `0.0.31-log25`: first guest code off the turn (state U)

- **Where the XAudio thread spends its turn** (run 25, frames of 1300+ draws, `private/session-tools/thread_turn_leaves.py`):

| Function | Share of its turn |
| --- | --- |
| Native mixer (N) | 22 % |
| `sub_827F5990` (second mixer, guest code) | 21 % |
| `sub_827E5358`, the caller of the first mixer | 8 % |

  `sub_827E5358` takes a guest spinlock and calls kernel imports, so it stays on the turn. `sub_827F5990` does arithmetic on its
  argument buffers and the stack and calls only an integer-to-double helper. It has no import, spinlock or atomic.
- **U** (`audio_offturn.cpp`): the XAudio thread leaves the turn for `sub_827F5990`, runs the recompiled function unchanged on core 2
  at 0x2B (as the native reverb does), and takes the turn back. Meanwhile other guest threads run.
  - Its guest function entries skip the scheduler quantum (`g_guest_off_turn`).
  - A caller holding a guest spinlock keeps it on the turn.
  - `[AUDIOOFF]` reports calls, off-turn calls and the cost per call with and without the handoffs. If the handoffs cost more than
    the mixing, U loses.
- The ceiling is small: about 21 % of the XAudio thread's turn, roughly 3-4 % of the heavy-frame turn. What it tests is the
  mechanism (guest code beside the turn holder) and whether it eases the audio.
- States `MAPXSHN`, `MAPXSHNU`, `MAPXSHNRG`, `MAPXSHNRGU`: U crossed with RG, whose gain cost audio in runs 23-25.

## Run 26: `0.0.31-log25`, 2 November 2026

`Gungan_A` to `C` and back to the hub; no crash, no capture. Archived in `private/runs/run26-log25`.

- **U never left the turn.** No profile sample passed through the off-turn path, and `[AUDIOOFF]` printed nothing: it printed every
  20,000th off-turn call. The mixer's caller chain takes the XAudio voice lock (`sub_827F1258`: `KeRaiseIrqlToDpcLevel`,
  `KeAcquireSpinLockAtRaisedIrql`), so the guard kept every call on the turn.
- **That guard was right.** `KeAcquireSpinLockAtRaisedIrql` and `KfAcquireSpinLock` took nothing: they counted a depth and relied on
  the one guest turn for exclusion. A thread holding such a "lock" that left the turn would let another enter the same section.
- FPS and audio by state are within noise of each other, as expected with U inert (at 4000+ draws: today's 15.1 FPS, U 14.9, RG
  14.1, RGU 15.6). U's audio read 99 % filled there, against 56-72 % for the others, over 15 s; with U inert, that is the scene.
- `[PARALLEL]` again: the main thread waits 41-50 % of the time and XAudio 40-44 %.

## `0.0.32-log26`: real guest spinlocks (state L)

- **L:** the spinlock stubs take the guest lock word by compare-and-swap, with a tag for this thread. A waiter spins and hands the
  turn on meanwhile, or only yields the core if it is off the turn. A release clears the word whenever it holds this thread's tag,
  whatever L is at that moment. A lock taken while L was off stays held through the turn, and keeps its holder on the turn.
- **Known risk.** At the moment L switches on, a thread holding a turn-only lock and a thread taking it for real could both be inside.
  The states change every 15 s and the sections are short; for a test only.
- **U** may now leave the turn under a spinlock when every lock this thread holds is real.
- `[AUDIOOFF]` prints every 2,000 calls with the off-turn count, the cost per call, and real locks taken and waited for.
- States `MAPXSHN`, `MAPXSHNL`, `MAPXSHNLU`, `MAPXSHNRGLU`. L alone shows the cost of real locks; LU the first guest code beside
  the turn; RGLU whether that gives back the audio RG cost.

## Run 27: `0.0.32-log26`, 2 November 2026: froze in L

The game froze in `Gungan_B` 0.1 s after state L switched on (replay 4020), after playing without faults until then. The audio
went on. Archived in `private/runs/run27-log26`.

- **The hang:** the guest thread `823592C0` spun for ever in `spin_acquire` (`KeAcquireSpinLockAtRaisedIrql` from `sub_827EEE08` <
  `sub_827D8148`), on a lock word that never cleared. The main thread waited in guest waits, and the tile worker waited for its event.
- **Likely cause, not proved:** the game takes some locks in its own code (`lwarx`/`stwcx`, any value) and releases them through the
  kernel stub. The log26 release only cleared a word holding this thread's tag, so a word set by the game's own code stayed set.
  Without L the words were never read, so this never showed. The window when L switches on is the other suspect.
- **U ran:** 18,963 calls off the turn without a fault. But the mixing took 0.010 ms per call against 0.41 ms with the turn handoffs.
  Per call, U only loses. A whole subtree has to leave the turn at once.
- **Audit of the XAudio code** (`private/session-tools/callee_audit.py`, new):
  - `sub_827F1258` (voice processing, under the voice lock) reaches 31 functions. Its kernel imports are spinlocks, IRQL,
    `KeQueryPerformanceFrequency` and `MmGetPhysicalAddress`. It has indirect calls in 3 functions.
  - The XAudio thread's own loop (`sub_827DB318`) calls `KeWaitForSingleObject`, `KeSetEvent` and `KeReleaseSemaphore`. Most of its
    work is behind indirect calls, which a static audit cannot follow.

## `0.0.33-log27`

- **Release** writes 0 with L on, as the 360's does, whoever set the word.
- **A wait for a spinlock past 200 ms** prints `[SPINLOCK]`: the lock, its word, and whether that word is one of this port's tags
  (and whose) or a raw value the game wrote. It then takes the lock anyway: no exclusion, as before L, but no freeze.
- States `MAPXSHN`, `MAPXSHNL`, `MAPXSHNRG`, `MAPXSHNRGL`; U is out until a subtree can leave the turn at once.

## Run 28: `0.0.33-log27`, 2 November 2026: the known driver crash

The game crashed while entering `Gungan_A` (state `MAPXSHN`, so L and R off at that moment). Archived in `private/runs/run28-log27`.

- **The crash** is open issue 1 in `docs/ISSUES.md`, the native Mesa/Nouveau crash: `nvAddressSpaceUnmap` < `nouveau_bo_del` <
  `__wrap_nouveau_bo_ref` (`bo_check.cpp`) < `pushbuf_flush`, on Mesa's glthread worker drawing (`nvc0_constbufs_validate`), with
  far address 0x58 (a null device). It is the same as exact009.
- It is the first time in runs 10-28. R (render thread 0x2C, the same priority as Mesa's worker) has been cycling since run 23 and
  changes how the render thread and that worker interleave. That is a possible influence, not shown. This run cannot tell.
- **L:** it was on during the menu and the hub (replays 0-807, and 2138-2577 with RG). There was no `[SPINLOCK]` report and no hang,
  so log27's release looks sound there. `Gungan_B`, where run 27 hung, was not reached.
- Next run: the same build and profile again. Per `ISSUES.md`, no guard that hides the invalid device.

## Run 29: `0.0.33-log27`, 2 November 2026

`Gungan_A` to `E` and back to the hub; no crash, no hang. Archived in `private/runs/run29-log27`.

- **L no longer hangs, but it is far slower.**
  - The 200 ms limit fired 20 times, always on one lock, `83212774` (the XAudio voice lock, by the threads involved). The holder was
    one of this port's tags each time: ids 0, 1, 4 and 5 in turn, so neither the release nor a game-written word was the problem.
  - In L, 1300-2500 draws ran at 6.0 FPS with audio 36 % filled, p95 638 ms; today's placement ran at 26.0 FPS.
- **Why, from the reports:** the holder did not run for 200 ms. A waiter that holds the turn passed it round the rotation, and with
  audio first (A) the XAudio thread, when it was the waiter, got the turn straight back. Under the old turn-only locks the holder
  simply kept running. This is priority inversion.

## `0.0.34-log28`

- **A spinlock waiter passes the turn directly to the holder**, whose scheduler id is in the lock word's tag (`sched_yield_to`). It
  falls back to an ordinary yield if the holder cannot run.
- `[AUDIOOFF]` lines now also count "turn passed to the holder" and "taken after 200 ms".
- States `MAPXSHN`, `MAPXSHNL` only, so L gets twice the play time.

## Run 30: `0.0.34-log28`, 2 November 2026

`Gungan_A` to `C` and back to the hub; no crash, no hang. Archived in `private/runs/run30-log28`.

- **Passing the turn to the holder did not help.** The turn was passed 1.57 M times, yet 846 waits still ran into the 200 ms limit,
  all on the XAudio voice lock `83212774`, with holders 0, 1 and 4 in turn. L ran 8.4 FPS at 4000+ draws (p95 810 ms), against 14.5
  for today's placement; 10.2 against 27.1 at 300-1300 draws. Real spinlocks as built are not usable.
- **The tagged holder did not release the lock even with the turn**, so one of these holds (not checked):
  1. Some path releases that lock without the stubs: the game's own code writes the word, or another import clears it. The word
     then keeps a stale tag, and the next acquirer waits 200 ms for a thread that no longer holds it. The frequency fits: 846 of
     335,753 acquisitions.
  2. The holder blocks (a guest wait) or spin-waits while holding the lock, for something the waiter has to do. That worked when the
     locks were turn-only, because the waiter was never stopped.
- **Next evidence** (none built): per lock, record the guest lr of each acquire and release through the stubs, and on a forced take
  print the holder's scheduler state (runnable, blocked in a guest wait) and its last acquire site. A lock released outside the
  stubs shows as an acquire with no release. Searching the recompiled code for stores to `0x83212774` (`lis` 0x8321 + 0x2774) is the
  static route.
- **Profile:** `MAPXSHN`, `MAPXSHNRG` (L, U out). Same build.

Route 1 now depends on making guest spinlocks real without this stall (`CPU_ROADMAP.md`). Route 2 (faster generated code) does not.

## `0.0.35-log29`

**What run 30's stall most likely is (read from the guest code and the run 30 counters; the count was never read at run time):**

- The voice lock `83212774` is a recursive lock handled inline in 76 guest functions (`private/session-tools/voice_lock_sites.py`): +0 word,
  +4 count, +8 owner (r13, one per guest thread here), +12 saved IRQL. A thread that owns it only adds to the count. The stub runs only
  when the count is 0 or another thread owns it. A release by a thread that is not the owner does nothing, and the release stub runs only
  when the count reaches 0.
- Without L, a holder preempted in its section lets a second thread in. The owner is overwritten, the first release is lost, and the
  count stays at 1 or more for good. Harmless to the turn-only stubs. With L, the word then keeps the last tag and each change of owner
  waits 200 ms, and the forced take adds to the count again.
- Fits run 30: `waited` equals `taken after 200 ms` on the `[AUDIOOFF]` lines checked, up to the final 846 and 846, so no wait ended
  normally. The first stall came 0.1 s after L switched on in a heavy scene. Reports alternate owner and waiter. Run 27's hang was on the same lock.
- This corrects run 30's text in one point: only the first 20 of the 846 reports were printed, so "all on `83212774`" holds for those 20.
- Hypothesis 2 of run 30 (a holder waiting for the waiter) is less likely but not excluded.
- Possibly also without L, not measured: with a stale count the acquire stub runs and the release stub does not, so `g_spinlock_depth` only
  grows and those threads yield at every 9th quantum boundary.

**The build:**

- `LSWTCS_SPINREAL=1`: real locks from boot, when the fields are clean. L can then go off once and not back on.
- Winning the word of the voice lock clears its count and owner, so a forced take no longer leaves it broken. An address table of one
  entry, since the game's inline code cannot be hooked.
- `[VOICELOCK]` every 5 s (with `LSWTCS_SCHEDPROF`): word, count, owner, stub acquires and releases, stale counts seen (`dirty`), waits by
  duration, time held, quantum boundaries kept or yielded under a held depth, depth by thread. Other locks appear once they have a wait.
- `[SPINLOCK]` is limited per lock and adds the count, owner, holder state and last acquire and release sites.
- Profile: `LSWTCS_ABSTATES=MAPXSHNL` until the first return to the hub, `LSWTCS_ABSTATES2=MAPXSHN` after it. The same level played in both;
  the second pass has its shaders ready, which favours the state without L.

**What run 31 decides:** forced near 0 with short waits and FPS within about 10 % means route 1 continues with the `sub_827F1258` subtree.
Forced near 0 but much slower: use the hold times to decide whether L is worth it. Forced with a clean count: hypothesis 2, and the holder
state says what to fix; without a simple fix, route 2. A count stuck in phase 2 with depth growing: the no-L effect is real, a separate fix.

## Run 31: `0.0.35-log29`, 2 November 2026: real spinlocks work

`Gungan_A` to `E`, the hub, and the same level again; no crash, no hang, no `[SPINLOCK]`. Archived in `private/runs/run31-log29`.

- **The stale count is confirmed at run time.** Phase 1 (L from boot): 4.67 M stub acquires, as many releases, count 0, `dirty` 0, no forced
  wait, 16 waits, all under 1 ms; the longest hold 11 ms, the lock held 1-9 % of wall time.
- **Phase 2 (L off) reproduced the fault by itself:** it stayed clean until the second `Gungan_B`, about 378 s in, then the release stub never
  ran again (releases frozen at 6,078,238 while acquires went on) and the count climbed to 24. `g_spinlock_depth` grew to thousands (0:4343,
  1:16396, 4:9090, 5:9059), so those threads now skip the quantum at 8 boundaries in 9. Today's default has this defect once the level gets heavy.
- **FPS and audio, L first pass against no L second pass** (the second has its shaders ready):

| Draws | L | No L |
| --- | --- | --- |
| 800-1300 | 29.2 FPS, audio 100 % | 28.9, 99 % |
| 1300-2500 | 24.4, 99 % | 27.1, 95 % |
| 2500-4000 | 21.0, 99 % | 21.9, 79 % |
| 4000-5000 | 15.9, 97 % | 15.5, 85 % |
| 5000+ | 13.5, 93 % | 14.0, 91 % |

  So L costs 0-10 % of FPS at most, in the pass that also compiles shaders, and audio is clearly fuller. The two are not separated cleanly: pass order
  and the stale count both differ. L's pass had 4 frames of 500 ms or more (max 1554 ms) against none; cause not checked, but no lock wait comes near that.
- **Decision:** L is sound, so route 1 continues. Making L the default is the owner's call; it stays in the test profile.

## `0.0.36-log30`

- **State U is rebuilt:** the voice processing `sub_827DDE40` (57 % of the XAudio thread's turn in heavy frames, only that thread on the stacks) leaves the turn as a whole,
  on core 2 at priority 0x2B. Log25's per-call `sub_827F5990` is gone. `sub_827F1258` was the earlier candidate, but it sits under a per-voice dispatch.
- **Off-turn guard:** kernel stubs now take the turn back if reached off it (`offturn_rejoin`), except those marked `PPC_FUNC_PROLOGUE_TURNFREE`
  (spinlocks, IRQL, `KeQueryPerformanceFrequency`, `MmGetPhysicalAddress`). The rest of the call then runs on the turn. This covers indirect calls the audit cannot follow, for
  imports in `kernel_stubs.cpp`; guest code reached that way runs off the turn until it meets a stub.
- `[AUDIOOFF]` every 5 s: calls a second, off the turn, stayed off, returns for a stub, cost per call on and off the turn; the first 40 returns name the stub.
- **Profile:** `MAPXSHNL,MAPXSHNLU` until the first return to the hub, `MAPXSHNLR,MAPXSHNLRU` after it. Each pass compares U within itself.
- **Not known:** calls a second (each costs a handoff, about 0.41 ms in run 27). Lock-free sharing without barriers is unaudited (`sync`/`lwsync` are dropped).

**What run 32 decides:** U at or above the same state without it, audio no worse, no corruption: keep it and move more of the XAudio thread. Many stub returns: the
named stub is the next audit. U slower because of handoffs: find a root called less often. Audio faults or crashes only in U: look for lock-free sharing first.

## `0.0.37-log31`

Same code as log30 for U. The run is redesigned to answer more in one pass of the level (the tester's request).

- **Eight states, every 6 s:** U, R, G and Q on top of `MAPXSHNL`, in a new order each round: G is on when an odd number of U, R, Q are. Each letter is on in 4 states and is compared over
  half of the session; a single letter's effect is clear of the others, but pairs are mixed (R+G with U+Q). No second pass.
- **Run 24 and 25 reread with load matching:** R is +2 to +5 % and G +3 to +5 %, with intervals of about 4-5 %, not the 10-15 % of the raw comparison, which mixed scenes of different load.
- **Q is back** on one reading of run 31: the stale count made threads skip 8 quantum boundaries in 9, which is a longer quantum. That may be part of why the second
  pass had more FPS and less audio. Run 16 dropped Q without L, when that stale state was probably already active in heavy scenes, so that result may not carry over.
- **`[ABSTAT]`** at each state change: running totals of turn time by thread, thread switches, voice lock counts, quantum boundaries and off-turn calls, so they can be charged to a state exactly.
- **`private/session-tools/ab_factorial.py`:** the effect of each letter by draw band and load-matched, with a 90 % interval from resampling visits. An interval across 0 means the run cannot say.
- **Limits:** a crash loses the later states but names the state it happened in. If U corrupts something, the other states' numbers share that session. Compiler changes (route 2) still need two builds.

## Run 32: `0.0.37-log31`, 2 November 2026: per-voice U rejected, R, G and Q show no gain

`Gungan_A` to `E` once, 41 state visits; no crash. Archived in `private/runs/run32-log31`.

| Letter | Load-matched speed at 1300+ draws (90 % interval) | Audio filled, with against without |
| --- | --- | --- |
| U | +1.0 % (-4.1 to +5.7) | 31 % against 91 % |
| R | +0.8 % (-3.5 to +5.6) | 62 % against 61 % |
| G | -3.4 % (-7.7 to +1.0) | 65 % against 58 % |
| Q | -1.1 % (-5.5 to +3.7) | 69 % against 56 % |

The audio numbers of R, G and Q are dominated by which states had U; without U: L 100 %, L+Q+G 90 %, L+R+G 85 %, L+R+Q 94 %.

- **U as built loses:** `sub_827DDE40` is per voice, 1,000-5,000 calls a second, 0.043 ms of work against about 0.5 ms with the handoffs (163,023 of 163,043 calls stayed off the turn; 20 returned, all for `KeSetEvent`). The XAudio thread's turn fell from
  about 3 s to 0.8 s per state, and raw FPS rose at 4000+ draws (+7 % at 4000-5000, +12 % at 5000+, intervals clear of 0, not load-matched), but the audio emptied.
- **R, G, Q:** nothing measurable within about ±4 %. They stay out. Q does not explain run 31's second pass, which leaves the shaders ready as the likely cause, not shown.
- **19 forced waits, U states only:** the XAudio thread off the turn lost every compare-and-swap for 200 ms while the main thread, on the turn, polled in a take-and-release loop (`sub_827D9DF8`; stub acquires 7x higher in those states). The main thread ran 235 ms of a 268 ms frame in the first one. Reading of the counters, not traced.
- **Cosmetic:** held-time totals overflow when a thread off the turn sets `hold_t0` between another's read; fixed in log32.

## `0.0.38-log32`

- **U moves up to `sub_827DA6B8`,** which holds the per-voice loop and is called from `sub_827DAB90`, one audio tick's work. Only the XAudio thread on the stacks; only spinlocks and pure stubs in the static reach. Rate not known until run 33.
- **A waiter off the turn gets way:** a thread on the turn holds back up to 50 us before taking a lock while one waits.
- **Profile:** `MAPXSHNL,MAPXSHNLU,MPXSHNL,MPXSHNLU`, every 6 s, one pass. A off asks whether U makes audio-first unnecessary.

**What run 33 decides:** U with audio at about the level without it and FPS up at 4000+ draws: keep U and look at the next thread. Audio still emptied: read the calls a second, then decide between a higher root and the whole thread off the turn. No gain and no harm: route 1 through XAudio is worth little, go to route 2.

**Route 2 prerequisite found:** `private/session-tools/poll_loops.py`: 686 no-call, no-store backward loops whose address looks fixed, many of them list walks, but some true polls (`sub_822D2798`, `sub_822AF268`, `sub_82264240`). Plain guest memory could read those once and spin for ever, so they need to stay volatile. A build-wide change needs that exception first.

**Route 2 probe, no console (`plain_mem_probe.sh`, `plain_mem_compare.py`):** the hottest generated file, `ppc_recomp.359.cpp`, compiled with plain memory: 90,416 to 90,928 bytes (+0.6 %), memory instructions 8,826 to 9,220 (+4.5 %), and the two hottest functions in it about unchanged (`sub_8231ED30` 0 %, `sub_82320328` +1.5 %). One file, and size is only a proxy for speed, but it shows no sign that the compiler removes much. Plain memory drops down the route 2 list until something measures otherwise.

## `0.0.39-log33`: one draw pass per frame (`LSWTCS_ONETILE`)

**What the game does, from the recompiled code.** Tiling is the engine's own choice, not D3D's. `sub_82316AF8` sums the EDRAM a render target set needs (`sub_822A3CE0`, up to 4 colour targets and depth). Above 2048 tiles it computes a tile height, writes the rects, and stores the count at set +264. Every user of that count seen (`sub_82316530`, `sub_823168D0`, `sub_82316978`, `sub_823170F8`) also has a plain path for a count of 1 or less. The main scene set is 2 colour targets and depth at 1280x704, 2x MSAA, in tiles of 320 rows (`[GLDRAWCAP]`, run 17). That needs more than the 2048. So does one sample: about 2112.

The device's 2x MSAA comes from the video mode (`sub_8248CE98`, present parameters at `0x830CB7E8`). Changing it would not be enough, as above, and it is not needed: this port has no EDRAM limit (`rt_get`: one host target per base, pitch and format, 2048 rows tall).

**The change.** While the engine lays out a set, its own calls to the size helper get a quarter of the real size, so the layout fits and takes the plain path; the bases stay apart. Surface creation still gets the real size. MSAA and resolution do not change. Decided at boot; `LSWTCS_ONETILE=0` in `env.txt` returns to the game's tiles without a new build. `[ONETILE]` prints each layout (tile count should be 0 or 1).

**Not known until the console:**
- whether the picture holds. Nothing has run; passes that assume tile-shaped resolves or EDRAM bases may break. Look at resolves, depth reuse between passes, and anything that samples a target just drawn;
- the gain. Guess from run 31's heaviest frames: walk about 33 ms, `repeat tiles` 9 ms, `plan` 13 ms on the render thread;
- the GPU cost, which may change with fewer merged draws.

**Comparing:** draw counts fall, so load matching by draws does not hold across runs. Compare by level with `phase_compare.py` against run 31's L pass (`Gungan_B` 21.3 FPS, `C` 23.3, `E` 31.7, 5000+ draws 13.5) and run 32's states without U. U and A still compare within the session with `ab_factorial.py`.

**What run 33 decides:** picture right and FPS up: keep it, and propose it to the owner as the default. Picture wrong in specific passes: read those passes and decide per target set. No FPS gain: the time is not where the profile suggests; go on with the walk itself.

## Run 33: `0.0.39-log33`, 2 November 2026: one tile is the largest gain so far

`Gungan_A` to `E` once and the hub; no crash, no `[SPINLOCK]`. Archived in `private/runs/run33-log33`. **The logs cannot show whether the picture is right**, and no capture was taken: that needs the tester's answer.

- **Tiling is gone.** Every `[ONETILE]` line reads tile count 0, draws fell about 3x (p95 5425 to 2004), `plan` fell from 13 ms to under 1 ms, and the command thread `822D2AB0`, 38 % of the turn before, no longer holds the turn at all.
- **Against run 31's L pass** (draws x3 to compare load; all states of run 33, half of them with U):

| Load | Run 31 | Run 33 |
| --- | --- | --- |
| `Gungan_B` | 21.3 FPS, p95 80 ms | 26.5, p95 56 |
| `Gungan_C` | 23.3, p95 67 | 28.4, p95 43 |
| `Gungan_E` | 31.7, p95 47 | 37.0, p95 40 |
| 2500-4000 draws | 21.0 | 29.1 |
| 4000-5000 | 15.9 | 25.3 |
| 5000+ | 13.5, audio 93 % | 20.7, audio 99 % |

  One route, by hand, so routes differ; the size of the gain is well above that.
- **U per tick works here.** At 5000+ equivalent: 17.6-18.8 FPS without U (6-12 s each), 23.2-23.3 with it (7-12 s each), audio 98-100 % in all. Load-matched at 1300+ draws in this session: U +18.5 % (90 %: +4.4 to +29.7, 10 visits only); over all frames -4.4 % (-10.0 to +1.6). So U helps where the turn is the limit and may cost a little elsewhere. 740 calls a second, 0.26 ms of work, 0.53 ms with the handoffs; 32 returns to the turn, all for `KeSetEvent`. No forced wait, so the give-way fix held.
- **A off:** no measurable change (load-matched -1.5 %, -7.9 to +5.0), audio 99-100 % either way.
- **What limits now:** GPU `draws..present` is about 30-36 ms at any load, so about 30 FPS is the ceiling at these clocks, and `[GLGPU]` was sampled too rarely here to say more. Heavy scenes are still turn-limited (turn held 95 %, main thread 86 % of it: 59 % guest, 27 % PM4 and record, 6.5 % XXH3).
- **Picture:** the tester reports it unchanged from earlier runs. So `ONETILE` stays on; the default for the port is the owner's call.

## `0.0.40-log34`

- **State E (new):** a page unchanged for 30 checks is hashed one frame in 4. Page hashes are 7.6 % of the main thread's turn at 1300+ draws, all from `gpu_shm_track`. A page that changes then reaches the GPU up to 3 frames late. `[ABSTAT]` counts `agedskips` and `agedlate`, the changes found on such pages.
- **W and Y are back in the test.** Out since run 22 (8.7 against 14.0 FPS), when the game also waited for the tile drain. With one tile, the walk is on the main thread's turn and about a third of it, so moving it could pay now. Default core 2, beside U.
- **`[GLGPU]`** now samples frames from 1300 draws, since 4000 no longer occurs.
- **Profile:** U, WY, R, G and E on `MAPXSHNL`, 8 states every 6 s, one pass. Each letter is on in 4 states and any two in 2. G is the parity of U, W, R and E is U xor R, so a letter's effect mixes with those pair effects.

**What run 34 decides:** which of the five stay on. `agedlate` high or flicker in E states: E goes. W faster with no fault: move more of the record work off the turn. `[GLGPU]`'s top programs give the first GPU list.

## Run 34: `0.0.40-log34`, 2 November 2026: W rejected again, the GPU is now the wall

`Gungan_A` to `E` once and the hub; no crash, no `[SPINLOCK]`. Archived in `private/runs/run34-log34`, with two captures (replays 6102 and 6481). The tester saw small visual differences, things shining differently, only in motion.

| Letter | Load-matched, all frames (90 %) | At 1300+ draws (90 %, 12 visits) |
| --- | --- | --- |
| U | +5.2 % (-2.2 to +12.8) | +6.7 % (-10.9 to +28.1) |
| WY | -11.5 % (-17.2 to -5.5) | -26.4 % (-34.1 to -15.8) |
| R | -5.9 % (-12.5 to +1.2) | -2.4 % (-19.1 to +16.5) |
| G | +5.1 % (-2.2 to +13.5) | +2.1 % (-15.5 to +23.0) |
| E | +3.6 % (-3.3 to +11.3) | +3.1 % (-14.6 to +25.9) |

Audio 98-100 % in every state. Each letter is mixed with some pair (G with U+W+R, E with U+R).

- **W is out, third time.** Idle guest time rises from about 10 s to 20-27 s per state: the game waits for the walk, on a shared core at a low priority.
- **U stays:** positive in runs 33 and 34, both intervals on the heavy side reaching well above 0 or close, audio fine. No forced wait; 38 returns to the turn, all `KeSetEvent`.
- **R and G stay out:** no measurable gain.
- **The shimmer, most likely E (not shown).** Both captures fell in one state, `MAPXSHNLRGE`, and the tester saw nothing in run 33, which had none of R, G, E. R and G only change priorities. E can hand the GPU a page up to 3 frames late (`agedlate` 81 in 2.4 M skips), and a page that sat still and then changes fits particle and glow data. The stills cannot show it. E is out, for a gain within noise.
- **The GPU now limits even the heaviest frames.** Sampled `[GLGPU]` at 1300+ draws: 44-54 ms (plain draws 18-29, quads 9-12, rects 7-10, resolves 5-6), with timestamps inflating it; frames there take about 46 ms and the render thread waits on fences 13 ms. Further CPU work may show little until that moves.

## Probes without a console

- **View table shortcut and `-fPIE`** (`addr_probe.sh`): `ppc_recomp.359.cpp` loses 3.0 % of its instructions and 4.6 % of memory instructions. 23 % of guest accesses are stack-relative. On 62 % of the turn that is a couple of percent at best, below what a run can measure, and it rests on stacks staying below `0xA0000000`. Not taken.

## Log 35: `0.0.41-log35`, logs for scoping GPU work and a possible Deko backend

Same behaviour as log34 outside sampled frames: profile `MAPXSHNLU`, no change to the picture, clocks or quality. Not yet run on hardware.

- **`[GLPASS]`**, in frames already sampled by `LSWTCS_GLGPUPROF` and in captures: GPU ms, draws, vertices and pixels per render pass, resolves with their cost and destination. First 40 frames of 1000+ draws, 8 lighter ones.
- **`[GLINV]`** (`LSWTCS_GLINV=1`), running totals from every 16th replay, printed every 1600: targets and formats, EDRAM reuse (old contents loaded, returns to a target, one base with two formats, colour and depth at one base), resolves by format and clear, textures by format and source, primitives, state counts, largest frame and memory.
- Not included: a replayable frame dump on Switch. The capture writer is desktop only (`lsw_frame_capture_writer.inc`), so a same-frame Mesa and Deko comparison still needs that port.

## Run 35: `0.0.41-log35`, 2 November 2026: the shimmer is gone, the main pass is not limited by fill

One state (`MAPXSHNLU`), handheld, GPU 307.2 MHz, memory 1331.2 MHz. No record of any earlier run being docked.

**Picture:** the tester reports the shimmer gone, so state E (age-based page skipping) is out for good. One pass, without E; E was not retested.

**FPS by draws**, frames over 400 ms dropped (12, mostly captures):

| Draws | Frames | FPS | Median ms | p95 ms |
| --- | --- | --- | --- | --- |
| 0-499 | 5661 | 44.4 | 21.3 | 34.9 |
| 500-999 | 1802 | 29.1 | 33.1 | 44.4 |
| 1000-1299 | 590 | 26.3 | 30.5 | 72.9 |
| 1300-1666 | 337 | 22.7 | 34.8 | 74.3 |
| 1667+ | 928 | 20.3 | 42.2 | 77.2 |

**GPU in heavy frames** (`[GLPASS]`, 1000+ draws, 21 frames; 4 captures and 3 frames of 90 ms or more left out): 31 passes, 1830 draws, 312 000 vertices, 10.9 Mpx, about 41 ms.

| Target | Draws | Vertices | Draw ms | Mpx | Resolves | Resolve ms |
| --- | --- | --- | --- | --- | --- | --- |
| Main scene, 1280 2x MSAA, two colour targets | 1612 | 200 636 | 22.0 | 1.64 | 0 | 0 |
| 640 wide, depth 1838 | 3 | 7 920 | 3.5 | 0.69 | 14 | 2.0 |
| 1280, depth 1864 | 60 | 12 768 | 4.6 | 1.21 | 2 | 0.3 |
| Main scene, one colour target | 79 | 1 074 | 2.1 | 0.12 | 3 | 1.5 |
| 1280, depth 1838 | 25 | 2 664 | 0.8 | 0.90 | 8 | 1.6 |

- **The main pass is more than half the GPU's time, with the screen covered about once.** 1.64 Mpx is less than one layer of 1280x720 at 2x. One pixel shader, `D669ECCA44168448`, takes 7.5 ms over 1021 draws and about 50 samples each, so fill does not explain that pass.
- Fit over passes: 6.4 us per draw, 36 us per 1000 vertices, 0.58 ms per Mpx, 0.24 ms per pass (shares 29, 33, 17, 20 %). R2 is 0.55: an indication only.
- By draw kind: plain 17.1 ms, quads 9.0 (128 draws), rects 7.0 (13), resolves 6.1 (30), points 0.8, present 1.1.
- 59 % of resolves (4914 of 8375) only clear; their copy is already skipped.

**Inventory for a renderer** (`[GLINV]`, 500 frames, 295 514 draws): 149 programs, 10 geometry shaders, 4 blend and 8 depth states. Two colour targets in 76 % of draws. Stencil in 0.5 %, polygon offset never, 32-bit indices never, 18 % without indices. Up to 9 textures per draw, DXT1, DXT3 and DXT5 mostly. Same EDRAM base at another pitch 9.7 times a frame, old contents loaded 1.6 times, return to a target 5.7 times. Colour and depth never share a base. 171 MB of targets, 182 MB of textures. The memory figure in the log is useless: it reports the whole heap. About 3 % of texture binds are cube units given a 2D fetch or nothing, and get a dummy; no visible effect known.

**Shaders:** all 509 distinct sources (110 vertex, 389 pixel, 10 geometry) compile with deko3d's `uam` once samplers are numbered per stage, 2.65 MB in total. This is offline: it says nothing about speed.

**Decision:** Deko stays undecided. Pixel and vertex work is the same shaders from a related compiler, so only draw and state cost, compression and Zcull could differ, and those are not separated yet. Next is run 36, profile only and docked: the omission split shows what each part costs, and the docked clocks show how much FPS the GPU holds back.

## Run 36: `0.0.41-log35` docked, 2 November 2026: the GPU's limit has a price, and it caps Deko

Same build, one state, one pass. Docked: GPU 768 MHz, memory 1600 MHz, no clock override. Handheld is run 35: 307.2 and 1331.2. Sampled frames ran twice as often here and leave things out, so docked FPS is slightly low.

| Draws | Handheld FPS | Docked FPS | Gain | Handheld p95 ms | Docked p95 ms |
| --- | --- | --- | --- | --- | --- |
| 0-499 | 44.4 | 50.6 | +14 % | 34.9 | 25.2 |
| 500-999 | 29.1 | 42.4 | +46 % | 44.4 | 29.1 |
| 1000-1299 | 26.3 | 36.6 | +39 % | 72.9 | 39.5 |
| 1300-1666 | 22.7 | 30.1 | +33 % | 74.3 | 39.3 |
| 1667+ | 20.3 | 24.9 | +23 % | 77.2 | 44.4 |

- **At the heaviest load the CPU limits again.** Docked, the sampled GPU total is 25.5 ms and the median frame 40.1 ms, with no GPU wait at the swap. The guest main thread is 77 % busy over the heavy band, and 95 % of the turn is that thread: guest code 57 %, PM4, recording and hashing 29 %.
- **This caps a renderer change.** Removing the GPU's limit in handheld could give about these gains, and a 2.5x clock only reaches them. Heaviest scenes: about +23 %.
- The GPU is not 2.5x faster in practice: 41 ms handheld against 25.5 ms docked, about 1.6x, with memory only 1.2x faster and pixel queries off here. Stamped runs differ, so this is approximate.

**What the GPU spends** (docked, 1000+ draws, about 10 frames per variant, different frames each; the 1500+ cut in brackets):

| Left out, cumulative | Total ms | Plain | Rects | Quads | Resolves |
| --- | --- | --- | --- | --- | --- |
| Nothing | 25.5 (25.5) | 11.2 | 3.8 | 4.7 | 3.5 |
| Pixels | 12.9 (17.7) | 3.8 | 0.9 | 1.4 | 2.4 |
| Vertices too | 14.0 (20.7) | 7.7 | 0.7 | 1.4 | 2.6 |
| Constant buffer binds too | 7.3 (8.1) | 1.1 | 0.7 | 1.3 | 2.4 |
| Texture binds too | 6.6 (7.1) | 0.6 | 0.7 | 1.1 | 2.7 |
| State too | 6.9 (6.9) | 0.6 | 0.7 | 1.1 | 2.3 |

- **Pixels and constant buffer binds are the two large parts**, roughly 8-13 and 7-13 ms. Vertices do not separate from noise. Texture binds and state are about 1 ms.
- **Quads without a geometry shader is dropped:** with pixels off quads cost 1.4 of 4.7 ms, so that change could save little.
- **The constant cost is not the old barrier.** `mesa_cbflush_edits.py` already flushes only after a write, and heavy frames make 5 shared memory copies, so few barriers remain. Heavy frames change about 3200 bindings. What remains is the bind or the constant cache reload after it, not separated. If it is the reload, Deko pays it too with bound buffers.

**Decision:** recommend not building Deko now. The owner decides. All shaders compile (run 35), so the option stays open, but the ceiling is about +23 % in the heaviest scenes and needs a GPU saving Deko has not shown. The profile is back to `GLGPUSPLIT=0`, `GLGPUPROF=300`, `GLGPUPIX=1`.

## Log 37: `0.0.42-log37`, three measurements in one pass

Docked again, to compare with run 36. Not yet run on hardware.

- **State B** (`mesa_cbdedupe_edits.py`): `nvc0_set_constant_buffer` returns early when a block's buffer, offset and size are the same. `st_bind_ubos` sets every block of every stage on any change, and Mesa marked each dirty without comparing. `[NVC0CB]` counts blocks set and skipped, barriers and serializes per state. States: `MAPXSHNLU`, `MAPXSHNLUB`.
- **Function order** (`hot_order.ld`, `tools/make_hot_order.py`): 2042 sampled guest functions placed hottest first, 3.7 MB apart at most against 73 MB. Compare the state without B against run 36.
- **`[WALKWHY]`**: per state, walks by the guest wait that caused them (fence, ring space, phase, VdSwap), time, ring dwords, and the time between walks. It decides whether a walker thread can run in parallel.
- `LSWTCS_GLGPUPROF=150`, `LSWTCS_GLGPUPIX=0`, for more GPU samples per state.

**The first log37 build did not start (three tries, same day).** The section ordering file put guest functions ahead of `.crt0`, so `_start` moved from address 0 to `0x3825E4` and the NRO header overwrote guest code at the start of the image. The ordering file now keeps `.crt0` first, and `validate_tcs360_switch.py` fails when `_start` is not at 0. Fixed build: `_start` and `__nx_mod0` at the same addresses as log35. Not yet run on hardware.

## Run 37: `0.0.42-log37` docked, 2 November 2026: B cuts the GPU, the order shows nothing, one walk a frame

Docked, states `MAPXSHNLU` and `MAPXSHNLUB`. The tester reports no visual fault.

- **B works.** Heavy frames (1000+ draws, 29 and 30 samples): GPU 27.4 to 22.8 ms; per 1000 draws -21 % (90 %: -31 to +4); plain draws 12.8 to 9.1 ms, -40 %. Rects, quads and resolves unchanged. About 60 % of block sets are skipped. FPS docked +3.2 % load-matched (90 %: -4 to +11), as expected where the CPU limits.
- **Barriers and serializes are not the cost:** about 3 barriers a replay, no serialize.
- **Function order: no visible effect.** Median frame against run 36 by draws: 19.5 to 16.8 ms under 500 (scenes differ), then 21.9 to 21.4, 26.6 to 26.4, 31.3 to 30.7, 40.1 to 39.9. FPS without B at 1300-2500 is 26.4 against 23.9, with 66.2 ms p95 and half the play time at that load, so the means do not compare. Under 2 % at best. It stays: it costs nothing.
- **The ring has one real walk per frame.** `[WALKWHY]`: one fence wait and one ring wait per frame find work, about 3 ring dwords each, so one indirect buffer holds the frame. Phase waits find nothing, VdSwap nothing. Walk 3-5 ms per frame averaged over a 6 s state; `[GPUPAGES]` 6-15 ms in heavy stretches.

## Log 38: `0.0.43-log38`, the same states handheld

Not yet run on hardware. Handheld, where the GPU limits, so B's gain can show in FPS.

- `[WALKWHY]` adds kicks (`sub_822AE048`, strong definition, only notes the time), time from the first kick to the walk, and whether a fence wait with work asked for a fence already done, the newest, or an older one. If the fence is already done, the walk can wait; if it is the newest, the game needs it there and a walker thread must have finished first.
- `[GLUBO]`: binding changes by uniform block per state, to see which blocks still change between draws.

## Run 38: `0.0.43-log38` handheld, 2 November 2026: B does not repeat, half the GPU's time looks like memory

Handheld, GPU 307.2 MHz, memory 1331.2 MHz, states `MAPXSHNLU` and `MAPXSHNLUB`, about 5700 frames in the level. No fault reported.

- **B is small.** Heavy frames (1000+ draws): GPU 41.7 against 42.0 ms; per 1000 draws -4.4 % (90 %: -10.8 to -1.0), -1.0 % at 1500+ draws (-9.2 to +1.7). FPS load-matched at 1300+ draws +0.3 % (-8.0 to +7.6), all frames +1.7 % (-4.2 to +8.5). Run 37's -21 % docked had an interval that crossed 0 and does not repeat here. B stays as harmless, with no claim of a gain.
- **Most binds are two or three blocks.** `[GLUBO]` over the run: vertex floats about 2.1 M changes, fetch 1.1 M, derived 0.9 M, pixel floats 0.28 M, system 0.19 M, bool 0.04 M.
- **No window before the walk.** Each frame's walk starts almost at once after the kick: 584 walks in a 6 s state took 1.2 ms in total from kick to walk; the run's mean is 0.37 ms, raised by a few stretches. A walker started at the kick has no head start.
- **The fence is not settled.** All 10 040 fence waits with work read as "older than put", none as done already. This classifier is weak: `get` moves only in the stub itself, and put is the next segment's fence. The code's note on `LSWTCS_GETMODE=0` records wrong vfetch strides when the wait returned early, so the wait is at least sometimes real. The walker thread is now unlikely to pay.
- **Against run 35** (handheld): at 1667+ draws 20.3 to 22.5 FPS, median frame 42.2 to 42.4 ms, p95 77 to 66 ms. Medians match; run 35 had captures and pixel queries on, so the mean's rise is not a gain from these builds.

**GPU time splits about evenly between clock and memory (estimate).** From 41 ms at 307 and 1331 MHz, and 25.5 ms at 768 and 1600 MHz, with time = core / clock + memory / memory clock: about 20 ms core and 21 ms memory handheld. It assumes both parts scale cleanly, and the two samples differ in pixel queries.

So less memory traffic is the most promising same-quality GPU target. Colour targets are single-sample, kind `0xfe`, uncompressed (`nvc0_mt_choose_storage_type`). Whether depth really gets compression tags on this path is unverified (`PORT_RESEARCH.md`).

## Log 39: `0.0.44-log39`, a test of GPU compression

Not yet run on hardware. Handheld, one pass. It tests whether less memory traffic lowers the GPU's time, with no change meant to the picture.

**What reading the sources shows:**
- Mesa already requests compressed depth kinds (`nvc0_mt_choose_storage_type`) and allocates them at 4 KB alignment. deko3d rounds compressed images to a big page and asks nvmap for that alignment (`dk_image.cpp`, `dk_memblock.cpp`).
- So depth may be running uncompressed today, with the kind falling back in Horizon. That is an inference: nvservices is closed, and nvmap's result is not visible from here.
- 32-bit colour at one sample gets kind `0xfe`, uncompressed. The `0xdb` line is commented out in Mesa as "makes things blurry". deko3d uses the same kind, `NvKind_C32_2CRA`, for these images.
- No `libdrm_nouveau` change is needed. Its `nvMapCreate` takes the alignment from `nouveau_bo_new`, which this port already wraps.
- Resolves sample targets through draws, so the 2D engine's compression flag does not come into them. Captures may read compressed targets wrongly.

**States:** `MAPXSHNLUB` as today; `+Z` depth on a 128 KB-aligned mapping; `+ZJ` also `0xdb` for 32-bit colour. Each has its own render targets (`g_rt_pool`, about 170 MB each), so a state change can lose their contents for one frame every 6 s. `[BOKIND]` lists the allocations.

`[WALKWHY]` adds the distance from fence to put. Mostly 1 means the game waits for the segment it just kicked, and the walker thread is out.

## Run 39: `0.0.44-log39` handheld, 2 November 2026: compression was never on, so it measured noise

The tester saw nothing wrong in any state.

- **Z and J did not take effect.** `[BOKIND]` printed once, for a 4 MB linear buffer, and for none of the 30 render targets the Z and J states created. `rt_compress_set` set the flag directly whenever the pipeline mode was off, while glthread was on and Mesa's worker did the allocation later. The flag was already back to 0 by then. log40 always queues it through `lsw_glthread_call`, which runs it directly when glthread is off.
- **So the three states were the same, and the run is an A/A test.** At 1300+ draws: `+ZJ` -12.0 % FPS (90 %: -20.6 to -3.4) and `+Z` -8.7 % (-20.8 to +0.1), each from 3-8 visits. GPU per 1000 draws: -4.8 % and +1.3 %, intervals across 0.
- **An interval that excludes 0 is not enough with this few visits.** Scene differences give about ±10 % at heavy load in a 6-minute pass. Take a letter as real only when it repeats, or when the per-1000-draw GPU figure and FPS agree.
- **Fence distance: every fence wait with work was for put - 2** (10 324 waits). log40 also records whether the walk's own packets complete that fence or only the stub's write does.

## Log 40: `0.0.45-log40`, the compression test again

Same states, handheld. `[BOKIND]` should list each new render target of Z and J: kinds `0x51` (depth) and, in J, `0xdb` (colour). Not yet run on hardware.

## Run 40: `0.0.45-log40` handheld, 2 November 2026: compression on, no gain, and colour blurs

`[BOKIND]` lists all 30 render targets of Z and J: depth kind `0x17`, colour `0xdb` in J, `0xfe` in Z, all on 128 KB mappings. About 3500 frames in the level.

- **Colour compression blurs the picture.** The tester saw the resolution drop for a few seconds at a time, and nothing else. Both captures of it fell in `+ZJ`, and they look low-resolution. Mesa's comment on `0xdb` holds here. J is also slower: GPU per 1000 draws +10.2 % (90 %: +1.2 to +25.5).
- **Depth on a big-page mapping changes nothing measurable.** GPU per 1000 draws -3.1 % (-6.9 to +3.8), -0.5 % at 1500+ draws; FPS load-matched +0.7 % at 1300+ (-9.7 to +20.9). Whether the hardware really compresses it is not visible from here.
- **Decision:** compression is closed on this Mesa path. Both states leave the profile. The code stays: with no Z or J, `g_rt_pool` is 0 and nothing changes.
- **Fence waits:** all 7738 with work wait for put - 2, and none of them is completed by the ring's own packets; only the stub's write completes it. So the walk at a fence wait is not what reports the fence.

## Log 41: `0.0.46-log41`, docked, one state

`[WALKWHY]` adds three things. First, whether the awaited fence's segment was already in the ring at the previous walk with work, so that walk has read it. Second, main-thread time from the frame's fence walk to VdSwap. Third, from VdSwap to the next kick. If the segment was always read before, a walker thread can take the walk and VdSwap can queue the frame instead of waiting. Not yet run on hardware.

**log41 grows before it runs (`0.0.47-log41`):** handheld instead of docked, with two additions.
- **Same-frame GPU bench (`LSWTCS_GPUBENCH=20`, `gpu_bench`).** At most every 20 s, a frame of 1300+ draws is drawn nine more times, each GPU-timed:
  - plain;
  - early depth tests flipped;
  - B flipped;
  - resolves skipped;
  - the four cumulative omissions of `GLGPUSPLIT`;
  - plain again, which is shown.

  Run 39 measured about ±10 % between A/B visits to identical states, too much for 5 % effects. On one frame that noise drops out, and the two plain replays give the floor. `private/session-tools/gpu_bench.py` reads it. The game holds for about half a second each time.
- **State F, early depth tests (`program_early`).** The translator emits an alpha test in 176 of 499 programs: exactly two discards and a sample-mask write, and no other discard. Either forces depth testing after shading. For a draw whose alpha test always passes (system flags bits 11-13 = 7) and whose alpha to coverage is off, that code cannot discard. Those draws get a variant with `layout(early_fragment_tests)`, so hidden pixels are not shaded. The variant compiles in the background, and the old program is used until it is ready. `[EARLYZ]` counts eligible and served draws. The main pass covers the screen about once, yet takes over half the GPU's time, and one shader spends 7.5 ms on 1021 draws of about 50 samples each. That fits shading that the depth test later rejects. It is a hypothesis.

## Run 41: `0.0.47-log41` handheld, 3 November 2026: the bench works, F never applied, the walk can overlap

States `MAPXSHNLUB` and `MAPXSHNLUBF`, four benches.

**Same-frame bench.** Each bench redraws one frame several times with one change each. Replays that change nothing (early-z flipped while no draw qualified) match the final plain replay within 0.1-1.2 ms.

The first plain replay of a bench also carried the stamps of the frame just drawn, so plain counts doubled when that frame was profiled. Deltas below are against "plain again". log42 reports the drawn frame first.

| Change | Per bench (ms) | Share |
| --- | --- | --- |
| B off | +1.0, +0.4, +0.4, +0.9 | 1-2 % |
| Resolves off | -5.4, -5.6, -4.9, -4.7 | 12-15 % |
| Pixels off and the omissions after it | Not monotonic | — |

Without pixels the GPU outruns the render thread's submission, so those timestamps follow the CPU's pace. They only show that pixel work is well over half of the 33-55 ms frames.

**State F applied to no draw.** `[EARLYZ]` counted 0 eligible draws in both states, so in every draw of a program with the alpha-test code, either the alpha test was on or alpha to coverage was. F's FPS of +17.5 % at 1300+ draws load-matched (90 %: +5.7 to +31.7) comes from identical states: the second A/A result. log42 records the alpha function and coverage of those draws. Its bench adds "early-z forced", which uses the variant for every such draw: an upper bound on the gain, never shown.

**The walk can overlap the game.**
- In 9558 of 9559 fence waits with work, the awaited fence's segment had already been in the ring at the previous walk with work. The walk at that wait is therefore not needed for that fence.
- From the frame's fence walk to VdSwap the main thread runs 13.6 ms on average, and from VdSwap to the next kick 11.1 ms.
- `[GPUPAGES]` puts the walk at 6-9 ms per frame in heavy stretches, so it fits inside the first window.
- One unknown remains: guest interrupt callbacks fired inside the walk, which run guest code. log42 counts them and their time.

## Run 42: `0.0.48-log42` handheld, 3 November 2026: no interrupts in the walk, copies are the resolves' cost, early-z needs care

One state, five benches. Deltas are against "plain again", in ms per bench.

| Change | Per bench (ms) |
| --- | --- |
| Resolves off | -5.8, -4.5, -5.5, -4.8, -4.8 |
| Resolve copies off (clears kept) | -4.4, -4.3, -4.8, -3.8, -3.7 |
| Early-z forced: the variant for every draw with the alpha-test code (1207-1558 draws) | -6.5, -9.0, +4.8, -5.3, -3.3 |
| B off | +0.3, +0.9, +0.5, +2.2, +0.2 |

- **The copies are most of the resolves' cost:** about 4-4.8 ms of the 5 ms.
- **Forcing early depth tests would save about 3-9 ms (one bench is out of line), but it is not safe.** `[EARLYZ]` shows every draw with that code has a real alpha test: function 4 (greater) 1.93 M draws, 6 (greater or equal) 0.72 M, never 7 (always), never alpha to coverage. With early tests, a pixel the alpha test discards would still write depth.
- **No guest interrupt callback ran from any walk**, so a walk can run on a thread with no guest context.

## Log 43: `0.0.49-log43`, the walk on its own thread

Docked, states `MAPXSHNLUB` and `MAPXSHNLUBI`. Not yet run on hardware.

- **State I.** A walker thread joins the scheduler (`async_walker_body`). At a fence wait whose segment an earlier walk already read, the wait hands the walk to that thread and returns. Any later walk site, and VdSwap, first waits for it (`async_walk_join`), so the guest sees the ring as before at every point that depends on it.
- The thread holds the turn for the ring and leaves it for each IB of 4096 dwords or more, as W does, on core 1 (`LSWTCS_WALKOFF_CORE=1`). A guest interrupt on that thread would be dropped and counted; run 42 had none.
- `[ASYNCWALK]`: hand-offs, the walker's time, and waits for it by site.
- **State F is safe now for draws that test depth but write neither depth nor stencil.** A discard after an early test changes nothing for them. `[EARLYZ]` adds their count by depth control; the bench's "early-z on" measures the gain.

## Run 43: `0.0.49-log43` handheld, 3 November 2026: state I runs, but the phase wait undoes it

Handheld (the tester could not dock). States `MAPXSHNLUB` and `MAPXSHNLUBI`. The whole pass played through, with no interrupt dropped on the walker.

- **The hand-off works.** Per 6 s state in I, 125-350 walks went to the walker thread, and it walked each one. In the heaviest stretch that was 191 walks in 1699 ms, about 8.9 ms each.
- **There was almost no overlap.** Nearly every hand-off was waited for at the next phase wait, for about as long as the walk took: 192 waits, 1685 ms. No wait happened at a fence, ring wait or VdSwap. The phase hook (`lswtcs_ad6a0_phase`) runs at the entry of `sub_822AD6A0`, before the guest's own check, so it waited even when the guest would have returned at once.
- **What the phase wait guards.** `sub_822ADD38` allocates from a circular buffer. The phase wait checks the buffer's lap against the GPU's. When the requested phase is the current one, `(phase - cur) & 3 == 0`, the guest returns: the memory it reuses was released at our last completed walk, not by the walk now running.
- **log44 skips the wait in that case.** It still waits when the lap has changed. `[ASYNCWALK]` counts the skips.
- **FPS:** I +1.3 % at 1300+ draws load-matched (90 %: -9.4 to +13.5). Handheld, the GPU sets the pace, and the walk did not overlap anyway.

**Early depth tests are closed.** Draws with the alpha-test code, by depth control:

| Depth control | Draws |
| --- | --- |
| Test and write (6) | 2.41 M |
| Test without write (2) | 0.13 M |
| No depth (0) | 4.5 k |

The safe rule therefore covers about 5 % of them, and the bench's "early-z on" moved -0.46 ms (median; range -5.3 to +3.6). Forced early tests saved 8.1 ms (median), but they would let alpha-tested pixels write depth.

**Bench:**
- Resolves off: -5.7 ms (12.8 %).
- Copies off: -3.8 ms. One bench was out of line.
- Pixels off: -27.6 ms, so pixel work is about two thirds of the frame.
- The costliest programs now carry their GLSL file in `[GLGPU]`. The second costliest, `CA8722BD02CAB82B`, has 576 lines of straight-line code with six texture reads and no loops. Nothing there is an obvious waste.

## Run 44: `0.0.50-log44` handheld, 3 November 2026: the phase skip works, the wait moves to the second fence

Handheld again. No fault, and no dropped interrupt.

- **Phase waits no longer wait.** 2570 were skipped as not needing the walk, and only 0-7 per 6 s state still waited, for a few ms.
- **The wait moved to the frame's second fence wait.** Hand-offs doubled, to about two a frame, and about one in two waited at the next fence wait for about as long as the walk took: in heavy states, 1774 ms of waiting against 1835 ms of walking.
- **The cause is in log44.** It counted a segment as read once it had been handed over: `g_put_at_walk` was set at the hand-off. So when the game waited for the same fence again right away, the stub waited for the walker.
- **log45 tracks finished walks** (`g_put_done`: the put when the last finished walk started). A fence wait waits for the walker only when its segment is not read yet, and a busy walker takes a new hand-off on its next round.
- **FPS with I:** +9.3 % load-matched over all frames (90 %: +1.1 to +17.9), +14.2 % at 1300+ draws (-1.7 to +29.7). That is with little overlap, handheld, and within the noise run 39 measured. It is suggestive, not shown.

## Run 45: `0.0.51-log45` handheld, 3 November 2026: the walk overlaps, and handheld does not care

Handheld again. No fault, no dropped interrupt, and audio was fine.

- **The overlap works.** Hand-offs per 6 s state were 391-606, and none waited at a fence wait. The only waits left were phase waits, 2-6 per state for 4-26 ms, and 0-5 at VdSwap. In all, 4749 phase waits did not need the walk.
- **The main thread is free for it.** Over the session, the main guest thread held the turn for 45.6 s in I against 59.9 s without it (-24 %). Idle went from 42.3 s to 58.6 s.
- **FPS does not move handheld.** Load-matched, I was -0.5 % over all frames (90 %: -8.2 to +7.4) and -2.1 % at 1300+ draws (-16.5 to +12.8). So run 44's +9 % was noise. Handheld waits on the GPU: the bench's plain heavy frame takes 49 ms (41.8-73.5), the same as the 21 FPS measured.
- **The bench repeats run 42.** Resolve copies cost 5.5 ms, forced early tests 7.4 ms, and B 0.4 ms. With pixels off, the frame takes 33 ms less.
- **Program costs.** From the per-draw stamps of heavy frames, two programs take 38 % of the GPU time: `CA8722BD02CAB82B` (6 textures, 72 draws) and `6F9BED73E5CE1620` (alpha tested, 207 draws). Neither shows waste. They have no dynamic register indexing and no loops, and texture sampling is cheap (+tex off: 0.6 ms).
- **Scale.** At 307 MHz, the Tegra X1 does about 157 GFLOPS against about 240 for the 360's GPU (peak figures, rough). The 360 holds about 30 FPS here. Handheld holds 21 at heavy load, close to that ratio. So shader code has little left to give handheld. Clock is the lever: 460.8 MHz is about 236 GFLOPS. That is the owner's decision.
- **Next.** I needs a docked run of the same build, where the main thread limits. Until then it stays a letter. The next GPU item would be resolves drawn straight into their destination. That saves at most part of the 5.5 ms, and only on passes that meet several conditions.

## Run 46: `0.0.51-log45` docked, 3 November 2026: I helps where the main thread limits

Docked, at GPU 768 MHz and memory 1600 MHz. No fault, no dropped interrupt, and no fence wait for the walker.

- **FPS with I:**
  - At 1300+ draws: 27.0 against 24.4 FPS, +11.0 % (90 %: +0.2 to +24.1).
  - Load-matched: +7.1 % at 1300+ draws (-2.8 to +18.9), and +5.0 % over all frames (-2.0 to +12.9).
  - Audio: 94 % in both states.
  - So the gain is only just outside the noise. The mechanism is direct, though: the walk left the main thread and nothing waits for it.
- **The main thread is still the limit.** It holds about 85 % of the guest turn in both states, and gets more frames done in I.
- **The GPU is not the limit docked.** The bench's plain heavy frame takes 21.3 ms (17.1-27.0), against about 37 ms per frame. Resolve copies are 3.2 ms of that.
- **Waste found on the render thread.** In its heavy samples, `getenv` takes about 7 %: a per-draw check of `LSWTCS_GLTEST` scanned the environment for every draw.
- **The sampling profile cannot tell A/B states apart.** In the main thread's mixed profile, about a quarter is the walk (`gpu_shm_track`'s XXH3, register copies, IB parse), which comes from the state without I.

## Log 46

- **I is part of every state now.** The state without I stays as the control (`MAPXSHNLUBI,MAPXSHNLUB`).
- **The `getenv` check is read once.**
- **Each hwprof sample carries its A/B state.** The state goes in the band's high bits, and `[ABCYCLE]` prints the "profile tag". `tools/hwprof_report.py --state NAME` reports one state. This gives the main thread's own profile with I. That profile picks between the generated-code routes: LTO, the quantum counter, and plain memory.

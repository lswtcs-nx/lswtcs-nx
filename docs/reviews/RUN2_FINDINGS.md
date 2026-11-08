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

## Log 47: why I gave 11 % and not 30 %

log46 was not run; log47 replaces it and keeps its changes.

**The gap.** Without I, the main thread's walks at the heaviest Gungan points were 10-15 ms per frame of about 40 ms (`[WALKWHY]`, run 46). With the walk fully off the main thread, the main thread would take about 21-25 ms per frame, and the GPU (21 ms) or the render thread (19 ms) would set the pace. Instead:
- Frames took 37 ms with I.
- The main thread still held about 85 % of the turn.
- In the sampled profile, walk code (record path, page hashes, IB parse) was 32 % of the main thread's turn over both states, against 36 % in run 42, which never had I.
- The walker's samples are 94 % waiting, although `[ASYNCWALK]` counts 13 ms of walking per frame.

**Three explanations fit:**
1. Part of the walk still runs on the main thread.
2. The main thread's own work slows while the walker streams guest memory.
3. The walker shares core 1 with the render thread.

**What log47 measures:**
- `[THREADCPU]`: every thread's CPU ms per replay, by state.
- `[ASYNCWALK]`: top-level IB time on the main thread, the walker and other threads, and the walker's time off the turn and waiting for it.
- State K: the walker's big IBs on core 2 instead of core 1.
- States `MAPXSHNLUBI,MAPXSHNLUBIK,MAPXSHNLUB`, with hwprof samples tagged by state (log46).

## Run 47: `0.0.53-log47` docked, 3 November 2026: the rest of the walk is at the phase wait

Docked. No fault, and no dropped interrupt.

**Heaviest Gungan frames (140-166 replays per 6 s state), CPU per frame from `[THREADCPU]`:**
- **Main thread:** 40.3 ms without I, and 31.5-33.9 ms with I. Without I, frames take about 42 ms, so the main thread is busy 94 % of the time.
- **Walker:** 3.5-4.4 ms.
- **Other threads:** render 11-12 ms, Mesa's worker 10-11 ms, the XAudio guest thread 12-15 ms.

**Where the walk runs, from IB time:**
- Without I, the main thread spends 14.5 ms per frame in IBs.
- With I, it still spends 8.5-9.3 ms, and the walker 3.6-4.2 ms. In light scenes, everything moves to the walker.
- The main thread's part is the GPU-phase wait. Its walks there take 8.8-9.6 ms per frame, about two per frame, each ending in one ring-level IB. The fence wait had 5 ms, and that is the only part I moves.
- Run 41 measured about 15 ms between a kick and the walk that reads it. So most of this walking could start well before the wait.
- The state-tagged profile (`--state MAPXSHNLUBI`) agrees: XXH3, register copies and IB parsing are still the top items of the main thread in I.

**FPS at 1300+ draws:**

| State | FPS |
| --- | --- |
| Without I | 25.0 |
| I | 27.5 |
| I with K | 25.0 |

K was 12 % slower load-matched (90 %: -18.5 to -5.6), with the walker's IB wall time at 15-31 ms per frame on core 2. K is out.

So run 46's "missing 10 ms" is not contention: the main thread's own work outside the walk is about 23-26 ms per frame in both states.

## Log 48

- **State k (lower case, since every capital letter is taken):** with I, each kick hands the ring to the walker. A phase or fence wait then waits only for what the walker has not yet read, instead of walking all of it on the main thread.
- **Expected result:** heavy docked frames from about 36 ms towards 27-30 ms, where the GPU (21 ms) and the main thread's own work (23-26 ms) meet.
- **States:** `MAPXSHNLUBI,MAPXSHNLUBIk`. `[ASYNCWALK]` reports the kick hand-offs.

## Run 48: `0.0.54-log48` docked, 3 November 2026: k moves the whole walk, and the fence wait waits for all of it

Docked. No fault, and no dropped interrupt.

**Heaviest Gungan frames (143-170 replays per 6 s state):**

| | I | I with k |
| --- | --- | --- |
| Main thread CPU per frame | 33.3-34.2 ms | 21.2-26.5 ms |
| IB work on the main thread | 8.6-9.1 ms | 0 |
| Walker CPU per frame | 3.7-4.4 ms | 11.0-14.3 ms, all off the turn |
| Main thread waiting for the walker | about 0 | 11.5-14.3 ms per frame, nearly all at the fence wait |

- **Why the fence wait waited.** It released only once the walker had nothing left. With a kick after every segment, the walker was always busy with segments newer than the awaited one. `g_put_done` moves only when a walk ends.
- **FPS.** At 1300+ draws, k gave 26.4 against 28.8 FPS. Load-matched it was -7.1 % (90 %: -13.1 to +0.3).
- **The inputs for an overlap are there.** The walker reads a heavy frame in 11-14 ms of CPU, against 21-26 ms of the main thread's own work.

## Log 49

- **A fence wait waits only for its own segment.** Each kick records the guest's segment counter (`dev+10908`) and the ring dwords written so far, and each walk publishes its read position after every ring packet (`g_walk_pos`). A fence's segment counts as read once the reader has passed the earliest kick whose counter is past the fence (`seg_read`). A fence wait then releases mid-walk; `[ASYNCWALK]` counts those releases.
- **Safety rule unchanged.** The game still continues only after the awaited segment has been read.
- **Expected:** in I with k, heavy docked frames of 25-28 ms. By then cores 0 (main), 1 (render and walker) and 2 (XAudio thread and Mesa's worker) are each busy about 25 ms per frame.
- **States:** `MAPXSHNLUBI,MAPXSHNLUBIk`.

## Run 49: `0.0.55-log49` docked, 3 November 2026: the fence wait still waits for the whole walk

Docked. No fault, and no dropped interrupt.

**Heaviest frames:**
- In I with k, the main thread's CPU per frame is 20.9-25.9 ms. The walker's is 12.7-14.5 ms.
- Fence waits for the walker take 10.3-13.7 ms per frame, about 3 per frame. That is the walker's whole CPU time.
- `fence waits released mid-walk` is 0: the reader never passed the awaited segment before it ran out of work.

**FPS:** k gave 23.7 against 26.9 FPS at 1300+ draws, -11.5 % load-matched (90 %: -20.6 to -3.8).

**Reading.** A wait that equals the walker's whole time means the walker and the game never ran side by side. Two causes fit:
1. The walker cannot start until the game blocks.
2. The awaited segment is the one just kicked, so its walk cannot have started earlier.

In I, the heavy segment (about 9 ms) was walked at the GPU-phase wait. That wait is the first after the kick, so cause 2 is the more likely. If so, the walk is on the critical path whichever thread does it. Only a faster walk, or a wait that does not need the walk, would help.

## Log 50

Diagnostics only. `[FENCEWAIT]` by state, for fence waits that wait for the walker:
- the time since the kick that submitted the awaited segment, and since the last kick;
- how many kicks came after it, and how often it was the newest;
- how long the walker's walk had been going;
- the walker's start latency after a hand-off.

States are unchanged.

## Run 50: `0.0.56-log50` docked, 3 November 2026: the game waits for the GPU at every flush

Docked. No fault.

- **`[FENCEWAIT]`, heaviest frames in I with k.** There were 2.7-3.0 waits per frame. Every one was for the newest segment, kicked 0.10-0.13 ms before, with no kick after it. The walker started 0.04-0.05 ms after a hand-off. So the walker is not late: the game waits for what it has just submitted.
- **FPS.** k gave 22.4 against 25.7 FPS at 1300+ draws, -8.9 % load-matched (90 %: -16.4 to -1.0). k is out.
- **Fence values step by 2.** `sub_822AE3F0` writes a fence packet and adds 2 to the counter at `dev+10908`. So "put - 2" is the newest fence: the wait means "everything submitted is read".
- **The waiting caller is `sub_822AEEB0`.** It is the flush, and it waits only when the word at `0x82FE0E00` is set and bit 1 of `dev+10941` is clear. Afterwards it sets bit 1, and the port clears it again at once.
- **The word is set from `VdIsHSIOTrainingSucceeded`.** `sub_822B9240` stores `!VdIsHSIOTrainingSucceeded()` there once at boot. Besides the flush, only that boot code reads it, to print a warning.
- **The port's stub returns 0, so training "failed".** Xenia returns 1. On a console whose CPU-GPU link trained, the flush never waits. Here it waits about 3 times per heavy frame, and each time the whole walk lands on the critical path.

## Log 51

- **State h** clears `0x82FE0E00` while it is on, and puts the 1 back after. The flush then behaves as on a console that trained. The game still has its phase, ring-space and other fence waits, which the runtime serves.
- **The boot stub is unchanged.** Its note says that returning 1 at boot blocked the flush's submission path. That note predates walks at phase, ring and VdSwap waits.
- **States:** `MAPXSHNLUBI,MAPXSHNLUBIh,MAPXSHNLUBIkh`.
- **Risk:** something in this runtime may lean on the flush wait. If so, h shows hangs or corrupt frames. VdSwap still waits for the frame's walk, so the walker trails by at most one frame.

## Run 51: `0.0.57-log51` docked, 3 November 2026: h works, and the render thread becomes the wall

Docked. No fault and no hang in h.

**FPS at 1300+ draws:**

| State | FPS |
| --- | --- |
| I | 28.8 |
| I with h | 25.2 |
| I with k and h | 26.4 |

h was -8.4 % load-matched (90 %: -17.6 to -0.1).

- **I with h.** The flush no longer waits, but the GPU-phase wait now walks everything on the main thread: 14 ms per heavy frame, and 36.6-37.7 ms of main-thread CPU.
- **I with k and h.**
  - The main thread is free: 23.5-25.2 ms of CPU, walks nothing, and waits for the walker only 2.4 ms per frame, mostly at ring-space waits.
  - The turn is idle 17-20 % of the time, and frames still take about 35 ms.
  - The state-tagged profile shows where the main thread blocks: in the present path (`gpu_d3d12_present` under `lswtcs_gil_blocking`), waiting for the render thread to take the frame.
  - The render thread's replay takes 19.6 ms of wall time for about 12 ms of CPU, on core 1. The walker (13 ms per frame) shares that core at a higher priority (0x2D against 0x3B).
- **CPU per heavy frame in I with k and h:** main 24 ms, walker 13, render 12, Mesa's worker 11, the XAudio guest thread 10-12, XMA 3-4. That is about 75 ms over three cores, so about 25 ms per core at perfect balance. The fourth core (owner's decision) would give the most room.

## Log 52

- **State w:** the off-turn walk runs below the render thread's priority (0x3D), so it fills core 1's gaps instead of preempting the render thread.
- **States:** `MAPXSHNLUBI,MAPXSHNLUBIkh,MAPXSHNLUBIkhw`.

## Run 52: `0.0.58-log52` docked, 3 November 2026: w shortens the render replay; the main thread is the wall again

Docked. No fault and no hang. The heavy part was short: 12 visits at 1300+ draws, 52 s.

**FPS at 1300+ draws:**

| State | FPS |
| --- | --- |
| I | 26.0 |
| I with k and h | 29.3 |
| I with k, h and w | 31.9 |

**Load-matched, 1300+ draws:**
- Ikh against I: +13.0 % (90 %: +4.1 to +27.1). In run 51 it was -2.8 % (-7.9 to +2.8). Pooled over both runs: +4.7 % (-1.4 to +12.4).
- Ikhw against I: +11.6 % (-1.5 to +24.6).
- Ikhw against Ikh: -1.2 % (-15.0 to +10.9).

Neither k with h nor w has an interval clear of 0 over both runs. The mechanism is clear:
- **w shortens the render replay:** median 16.3 ms in Ikh against 13.6 ms in Ikhw, for about 11 ms of render CPU in both.
- **CPU per heavy frame in Ikhw:** main 23, walker 15, render 11, Mesa's worker 10, the XAudio guest thread 7, XMA 2. Most of the XAudio thread's work already runs on core 2 (U and the native reverb), so all three cores are about 70-80 % busy.

**Where the main thread's time goes, Ikhw, heavy frames (state-tagged profile):**
- It holds the turn 92 % of the time. Guest code is 83 % of its turn samples and runtime code 4 %.
- About 8 % is the ring-space wait (`sub_822AD778`) and the phase wait sleeping in `async_walk_join`, holding the turn.
- About 4 % is present waiting for the render thread, and 2 % is `walk_wait_other`.
- So it waits for the walker about 3 ms per heavy frame.

**The ring-space wait waits for too much.** The boot code (0x822AF670) points the read-pointer write-back at `[dev+10896]+60`, which is the word `sub_822AD778` polls. So that wait is for the ring's read pointer to leave the window about to be overwritten. The hook instead waited for the walker to finish everything handed to it, and with k that is every kick so far.

**A check on preemption:** comparing wall-time samples with thread CPU suggested the main thread lost about 4 ms per frame to preemption. Render and XMA show the same 20-40 % excess, so this is a bias of the sampling and not evidence.

## Log 53

- **State r:** the ring-space wait ends once the walker's position (published after each ring packet, so after its IB) has left the window. The hook then stores that position as the read pointer, as the CP would. It falls back to the old path if the walker stops first or the ring size does not match. `[ASYNCWALK]` counts the releases and the fallbacks.
- **State f:** the off-turn walk may run on core 1 or core 2 (mask 6, `lsw_offcore_begin_mask`), so the kernel puts it where there is time.
- **States:** `MAPXSHNLUBI,MAPXSHNLUBIkhw,MAPXSHNLUBIkhwr,MAPXSHNLUBIkhwrf`. I stays as the control.

## Run 53: `0.0.59-log53` docked, 3 November 2026: r removes the ring-space wait

Docked. No fault and no hang. 15 visits at 1300+ draws, 78 s.

**FPS at 1300+ draws:**

| State | FPS |
| --- | --- |
| I | 27.4 |
| I with k, h and w | 27.5 |
| I with k, h, w and r | 30.8 |
| I with k, h, w, r and f | 31.2 |

**Load-matched, 1300+ draws:**
- r (with against without): +7.3 % (90 %: -0.5 to +14.6).
- Ikhwr against Ikhw: +6.0 % (-7.3 to +17.2).
- Ikhwrf against Ikhwr: +2.9 % (-8.8 to +20.7).
- Ikhwrf against Ikhw: +9.1 % (+0.5 to +20.4), on 3 visits each.
- Ikhw against I: +0.1 % here; pooled with run 52, +6.0 % (-0.7 to +14.1).

**r works as designed:**
- The ring-space wait fell from 1.5-3.2 ms per heavy frame (Ikhw) to 0.00 in every r visit.
- About 700 releases per visit and no fallback.
- The render replay median was 13.9 ms in Ikhwr and 12.4 ms in Ikhwrf.

**Where the main thread's time goes in Ikhwr, heavy frames:**
- It holds the turn about 90 % of the time, almost all of it running guest code.
- The waits left are:
  - present waiting for the previous frame's replay: about 4 % of its samples;
  - `walk_wait_other` (VdSwap and `pm4_walk_now` waiting for an off-turn walk): about 4 %;
  - the phase wait: about 1 %.
- That is roughly 3 ms per frame of waiting against about 24 ms of CPU.

**Docked now meets the game's own code.** The scheduling work since run 46 (I, k, h, w, r) has taken the main thread's waits from over 10 ms per heavy frame to about 3. Further docked gains need the guest code itself to run faster. Its profile is flat: the top 30 functions are 42 % of the samples, and the D3D layer (0x822B0000) is about 15 %.

**GPU, from the docked bench (8 benches, plain frame median 20.8 ms):**
- resolve copies: -3.2 ms (-15 %);
- pixels: -7.6 ms (-37 %).

So resolve copies are the largest GPU item on our side for handheld. They come from two sources: the main 1280-wide target's resolves (about 2.2 ms docked) and the downsample chain (about 1.2 ms).

## Log 54

- **r joins the base.** States: `MAPXSHNLUBI,MAPXSHNLUBIkhwr,MAPXSHNLUBIkhwrf`; f is still open.
- **`[THREADCPU]` now ends with "guest calls per replay".** These are guest function entries over all threads, so the main thread's CPU per call sizes the call-overhead route (inlining, LTO) before a build is spent on it.

**Added to log 54 before it ran (the tester asked for more in the same session, and wants handheld next):**
- **State G again** (`MAPXSHNLUBIkhwrG`): every guest thread except the XAudio thread runs at 0x2E. In run 34, before I, it was +5.1 % (-2.2 to +13.5). The main thread is the wall again, and the second shader-compile worker shares core 0 with it at 0x3B.
- **The bench splits resolve copies by destination size:** full size (1024 pixels wide or more) against reduced. A cheaper resolve path should go after the larger group first.
- **Handheld ground work, from run 53's docked heavy frames (91 frames, 23.6 ms of GPU each):**
  - Ten programs take about 12 ms.
  - Two of them are single full-screen draws: files `91834C0972EDD8A1` at 1.45 ms and `283E052419BCC461` at 1.29 ms per frame. These are post passes, and in handheld they should cost about 2.5 times as much.
  - Their GLSL is on the SD card (`sdmc:/lswtcs/glprogs2/<file>.glsl3`). The local copy in `private/glprogs2` predates the current translator and does not have them.
  - Reading them comes before any translator change for handheld.

## Run 54: `0.0.60-log54`, docked then handheld, 3 November 2026: the scheduling work holds; handheld needs the early depth test

Two sessions. The tester stayed in the heavy area at the start of `Gungan_B` for 126 s docked and 82 s handheld. No fault or hang. The `glprogs2` folder did not come with the logs.

**Docked, load-matched, 1300+ draws:**
- Ikhwr against I: +21.7 % (90 %: +14.4 to +33.3). Pooled with run 53: +15.5 % (+7.4 to +24.1). The scheduling work since run 46 is now a measured gain.
- f against no f: +1.5 % (-4.7 to +8.8); pooled with run 53, +2.2 % (-4.1 to +10.0).
- G against Ikhwr: +1.7 % (-4.0 to +7.2).
- FPS by state: I 24.5, Ikhwr 29.3, Ikhwrf 30.2, IkhwrG 30.7.

**Guest calls per heavy docked frame:** about 155 k, with 23-27 ms of main-thread CPU, so about 160 ns of guest code per call. The prologue and call overhead is a small part of that. Inlining or LTO could reach perhaps 5-10 % of the main thread, which is not the next step.

**Handheld (GPU 307 MHz), load-matched, 1300+ draws:**
- k, h, w and r together against I: +8.5 % (+0.3 to +17.5).
- G: -6.5 % (-15.6 to +5.4). G is out.
- FPS by state: I 20.0, Ikhwr 21.3, Ikhwrf 22.6, IkhwrG 20.2.

**Handheld GPU bench (7 benches, plain frame median 46.5 ms):**

| Change | Saving |
| --- | --- |
| Early depth test forced | -7.2 ms (-17 %) |
| All resolve copies off | -4.4 ms (-9.4 %) |
| Full-size resolve copies off | -3.1 ms |
| Reduced-size resolve copies off | -1.1 ms |
| Pixels off | -28.6 ms (-62 %) |

Docked, the same forced early test was -4.2 ms of 23.0.

**Why the early test matters so much:** `[EARLYZ]` shows the alpha-test code in nearly every world draw. The alpha function is GREATER (1.89 M draws) or GEQUAL (0.78 M), never ALWAYS, and 2.56 M of those draws test and write depth. A shader that can discard while depth is written gets its depth test after shading on nouveau. So hidden pixels are shaded in full. State F's safe subset finds almost nothing to take.

**The full-size copies cost more than their bytes.** About three a frame, roughly one 1280x704 screen in all, take 3.1 ms handheld, several times the memory traffic they need.

## Log 55 (for handheld)

- **State z:** the bench's forced early depth test, shown. It is not exact: a pixel the alpha test discards still writes depth, so something drawn later behind a cut-out (leaves, grates, a fading object) can disappear. It is a visual test for the tester, and the author decides on any use.
- **`[EARLYZ]`** adds a histogram of the alpha test references (0, up to 0.25, 0.5, 0.75, above). If they are 0, the inexact case is only fully transparent texels, which also points at an exact route.
- **New bench variant:** full-size resolve copies over a quarter of their area. If that still costs most of the 3.1 ms, the cost is the wait around each copy and not the copy.
- **States:** `MAPXSHNLUBIkhwr,MAPXSHNLUBIkhwrz,MAPXSHNLUBIkhwrf,MAPXSHNLUBIkhwrfz`. I is no longer a state. G is out, and f stays in a 2x2 design with z.

## Run 55: `0.0.61-log55` handheld, 3 November 2026: z breaks transparency; resolve copies cost what their pixels cost

Handheld, about 2 minutes in the heavy area. There are two full captures, both in states without z. The tester brought `glprogs2` (now in `private/glprogs2`; the 1 November copy is in `private/glprogs2-2026-11-01`).

**z is out.**
- The tester saw transparency broken in nearly everything: foliage, studs, and more.
- `[EARLYZ]` explains it: of the alpha-tested draws, 3.48 M have an alpha test reference between 0.25 and 0.5, 86 k up to 0.25, and 40 k at 0. The test really discards the half-transparent texels of nearly every material.
- z was +12.6 % load-matched (90 %: -3.8 to +29.2), but the picture rules it out.
- An exact early test would need the hardware's coarse depth culling, which nouveau does not drive, or a depth pre-pass. With shaders this cheap, a pre-pass would cost about what it saves.

**Handheld bench (10 benches, plain frame median 39.2 ms):**

| Change | Saving |
| --- | --- |
| All resolve copies off | -4.2 ms |
| Full-size resolve copies off | -3.3 ms |
| Reduced-size resolve copies off | -1.2 ms |
| Full-size copies at a quarter of their area | -2.4 ms |
| Pixels off | -24.2 ms (-63 %) |

At a quarter of their area, the full-size copies cost 0.9 ms against 3.3 ms. So their cost follows their pixels, not a wait around each copy.

**The captured frame's full-size resolves:**
- After the scene: colour 0, colour 1 and depth, each 1280x704. Both colour resolves clear their source.
- One more colour resolve after a full-screen pass, also clearing.
- The depth of the final 1280x720 pass, not clearing.

A colour resolve that clears its source leaves that source dead. Its destination could take the render target's texture instead of a copy. That needs the sampled texture to be taller than the destination (render targets are one fixed height), so the fetch would need a coordinate scale.

**MSAA is not the cost:** 2x surfaces already render at one sample per pixel (`LSWTCS_MSAA1X`, default on).

**Costliest programs, handheld heavy frames without z (42.6 ms of GPU each):**

| File | GPU per frame | Draws per frame | What it is |
| --- | --- | --- | --- |
| `6F9BED73E5CE1620` | 6.5 ms | 117 | World geometry: one texture, alpha test, two colour outputs |
| `CA8722BD02CAB82B` | 3.2 ms | 35 | — |
| `91834C0972EDD8A1` | 3.0 ms | 1 | Full-screen post pass: 734 lines, 42 branches, a control-flow loop, 4 textures |
| `259828D67C3314E5` | 2.5 ms | 812 | — |
| `283E052419BCC461` | 2.2 ms | 1 | Full-screen post pass |

## Log 56 (handheld)

- **z removed.** States: `MAPXSHNLUBIkhwr,MAPXSHNLUBIkhwrf`.
- **Bench "trivial pixel shaders":** each pixel shader becomes one that writes the same outputs a constant and keeps a discard where there was one. Against the plain frame this gives the shaders' own cost; against "pixels off", the filling. It decides between better translated shaders and less filling.
- **Bench "copies with a clear after off":** colour resolves from and into the origin that clear their source, which are the copies a texture hand-over could remove.
- **Dropped from the bench:** "early-z on", "B off" and the quarter-area copy.

## Run 56: `0.0.62-log56` handheld, 3 November 2026: the time is inside the pixel shaders

Handheld, 109 s in the heavy area. No fault.

**Handheld bench (10 benches, plain frame median 46.5 ms):**

| Change | Saving |
| --- | --- |
| Trivial pixel shaders | -26.3 ms (-58 %) |
| Pixels off | -26.3 ms (-64 %) |
| Copies with a clear after off | -1.9 ms (-4.3 %) |
| All resolve copies off | -4.1 ms |
| Early depth test forced | -9.7 ms |

- **Filling costs next to nothing.** Rasterising, the late depth test and the colour writes are what a trivial shader leaves, and that frame is as fast as one with no pixels at all. The handheld GPU time is the pixel shaders' own work: arithmetic and texture fetches.
- **The texture hand-over would reach about 1.9 ms (4 %).** That is real but secondary.
- **f:** -0.5 % (90 %: -8.6 to +9.4). f is out after four runs without a gain.

**What the world's costliest shader does per pixel** (`6F9BED73E5CE1620`, 6.5 ms per heavy handheld frame):
1. One texture fetch and a few multiplies.
2. Then the translator's epilogue, which is the same in every translated pixel shader: the alpha test, whose function is read from the system constants each pixel; the alpha-to-coverage code; a gamma conversion for each colour output behind a flag; and an exponent bias on each output.

These inputs are fixed for a whole draw. They appear in 218 of the 521 pixel shaders in `private/glprogs2` (alpha test) and in all 521 (the gamma flag).

## Log 57 (handheld)

- **State e:** the epilogue specialised per draw. The alpha test function, alpha to coverage and the two gamma flags become constants in a variant compiled for that combination, so the compiler drops the branches not taken. This is exact: the arithmetic of the branch taken is unchanged, and the all-samples `gl_SampleMask` write goes only when alpha to coverage is off. Until a variant is compiled, the draw uses the shader as before. `[EPILOGUE]` counts the draws that use it.
- **Bench:**
  - "epilogues specialised" (or "not specialised" in e);
  - "textures unbound": every fetch runs but reads no memory, so this is the arithmetic left;
  - "anisotropic filtering off": bench only, because the filter is a quality setting.
  - "early-z forced" and its background variants are gone.
- **States:** `MAPXSHNLUBIkhwr,MAPXSHNLUBIkhwre`.

## Run 57: `0.0.63-log57` handheld, 3 November 2026: specialised epilogues are a gain

Handheld, two passes through the heavy area (85 s and 71 s). No fault. The tester brought `glprogs2` again (730 programs).

**State e:**
- **FPS:** +10.4 % load-matched (90 %: +0.9 to +20.5); 23.6 against 21.1 FPS at 1300+ draws.
- **Bench, same frame:** -6.0 ms (-13.6 %) where e was off, and +3.3 ms when it was turned off in e. Mean GPU per heavy frame was 37.2 ms with e against 42.5 ms without.
- **Use:** after the first minutes nearly every eligible draw used its variant, with `[EPILOGUE]` at 100 % in most visits.

The world shader `6F9BED73E5CE1620` went from 6.4 ms to several variants that are far cheaper together.

**Bench, the shaders' cost split:**

| Change | Saving |
| --- | --- |
| Trivial pixel shaders | -21.7 ms |
| Textures unbound | -5.4 ms |
| Anisotropic filtering off | -0.3 ms |

So about three quarters of the shaders' time is arithmetic and a quarter is texture memory. Anisotropic filtering costs nothing worth discussing.

**Not specialised:** the second costliest full-screen pass (`283E052419BCC461`, 2.3 ms) writes depth, and e excluded depth-writing shaders, a caution it did not need.

## Log 58 (handheld)

- **e joins the base.**
- **State g:** with e, also the shaders that write depth, and each output exponent bias component that is exactly 1 (one multiply per output in every shader).
- **States:** `MAPXSHNLUBIkhwre,MAPXSHNLUBIkhwreg`.
- **Dropped from the bench:** anisotropic filtering, and the split by copy size.

## Run 58: `0.0.64-log58` handheld, 3 November 2026: g is small; hidden pixels are the cost left

Handheld, two passes through the heavy area. No fault.

**State g:**
- **FPS:** +2.0 % (90 %: -5.0 to +8.9); 24.0 against 23.4 FPS at 1300+ draws.
- **Bench:** turning e off cost 6.5 ms in g against 6.1 ms in e, so g adds about 0.4 ms.
- It is exact, so it joins the base.

**Bench (16 benches, plain frame median 38.7 ms):**
- Trivial pixel shaders: -19.6 ms.
- Textures unbound: -5.2 ms.
- Resolve copies off: -4.5 ms.

**The specialised world shader still costs a lot for its size.** With g, the one-texture world variant `C6238EE5535ACF1E` took 2.7 ms over 36 draws per heavy frame. Its few operations only add up to that if each screen pixel is shaded several times. Every world draw has the alpha test (references about 0.5), and nouveau then tests depth after the shader. So every hidden layer is shaded in full, and the forced early test (-20 %) showed the size of that.

## Log 59 (handheld)

- **g joins the base.**
- **State p, a depth pre-pass, exact by construction, for alpha-tested draws whose pixel shader reads two or more textures:**
  - **Pass 1:** colour writes off, with a variant whose colour outputs are zero, so only the alpha test's inputs are computed. It writes depth where the alpha test passes.
  - **Pass 2:** the full shader with the depth test before it, EQUAL with no depth write, and the alpha test folded to always.
  - **Conditions:** only draws that test and write depth with LEQUAL, with no stencil and no alpha to coverage. For those, EQUAL after pass 1 passes exactly where LEQUAL would have.
  - **Why the depths match:** the vertex shaders compute position as `precise`, so both passes place each pixel at the same depth.
- **Counters and bench:** `[PREPASS]` counts eligible and used draws. The bench toggles it ("depth pre-pass on" or "off").
- **One-texture shaders are left out:** their alpha path is nearly the whole shader, so a pre-pass would cost what it saves.
- **States:** `MAPXSHNLUBIkhwreg,MAPXSHNLUBIkhwregp`.

## Run 59: `0.0.65-log59` handheld, 3 November 2026: the depth pre-pass loses

Handheld, two passes through the heavy area (81 s and 50 s). No fault. About 60-70 draws per heavy frame took the pre-pass once their variants were compiled.

**State p:**
- **FPS:** -5.6 % load-matched (90 %: -11.8 to +0.7); 23.3 against 24.8 FPS at 1300+ draws.
- **Bench, same frame:** +1.2 ms with it on and -1.0 ms with it off.
- The second pass over vertices and the alpha path costs more than the hidden pixels it skips in the multi-texture draws.
- **p is out.**

The hidden layers are mostly in the one-texture world draws, whose whole shader is the alpha path, so no pre-pass can help there.

**What is left for the hidden layers:**
- The exact remedy is the GPU's coarse depth culling (ZCULL), which works with shaders that discard.
- This Mesa's nouveau has it switched off (`nvc0_validate_zcull` under `#if 0`, and `ZCULL_REGION` deactivated at screen init). Turning it on would mean programming the ZCULL storage and region, likely with the kernel's ZCULL binding. That is a hardware research project, not a next step.

**Untested and available:** nouveau's NIR shader front end (`NV50_PROG_USE_NIR=1`, enabled for the Switch build by `mesa_nir_edits.py`). It applies to a whole session only, cannot be an A/B state, and its stability on these shaders in Mesa 20.1 is unknown.

## Log 60 (handheld)

- **State d:** with e, each fetch's exponent scale (`lsw_fetch_derived[K].w`) and LOD bias (`.z`) become constants when they are exactly 1 and 0 for the draw. That removes four multiplies and a bias per fetch. It is exact and aimed at the one-texture world variants that are shaded layer upon layer.
- **Bench:** "fetch constants specialised" (or "not specialised" in d) replaces the pre-pass variant.
- **States:** `MAPXSHNLUBIkhwreg,MAPXSHNLUBIkhwregd`.

**Log 60, second session: nouveau's NIR shader front end.** The tester asked for it. `NV50_PROG_USE_NIR=1` alone did nothing:
- devkitPro's switch-mesa drops the NIR path with 26 `#ifndef __SWITCH__` guards across the state tracker (`st_program.c`, `st_context.c`, `st_shader_cache.c`, `st_glsl_to_ir.cpp`) and nvc0 (`nvc0_screen.c`, `nvc0_program.c`, `nvc0_state.c`, `nv50_ir.cpp`). `mesa_nir_edits.py` had freed only the variable in `nouveau_screen.c`.
- `libEGL.a` does not merge Mesa's `libnir`, and the log60 ELF had no NIR code at all.

**What changed for this session:**
- `source/switch/mesa/mesa_nir_full_edits.py` turns each NIR guard into `#if 1`, and leaves the draw module, glthread and SPIR-V guards in place.
- `tools/build_mesa.sh` builds `libnir` and stores it beside `libEGL.a` as a full archive. meson makes a thin one, and its SPIR-V objects are named `.._spirv_*`.
- `CMakeLists.txt` links `libnir.a` in a group with `libEGL.a`.

TGSI stays the default. The same NRO runs both sessions:
- the first with `env.txt`;
- the second with `env-nir.txt`, which is `env.txt` plus `NV50_PROG_USE_NIR=1`.

`private/session-tools/run_pair.py` compares two sessions load-matched. Runs 58 and 59 in the same state differed by +0.1 % (90 %: -5.2 to +5.5), so only a difference above about 5 % will show.

The first NIR boot misses Mesa's disk cache, so the 698 saved programs compile again; with TGSI they take about 95 ms each with the cache.

## Run 60: `0.0.66-log60` handheld, TGSI then NIR, 3 November 2026: NIR is out

Two sessions on the same NRO.

**Session 1 (TGSI, 102 s in the heavy area):**
- d was -3.1 % load-matched (90 %: -10.1 to +4.1), 24.2 against 24.8 FPS.
- In the bench, turning d off cost 1.8 ms (median; range -4.8 to +5.4).
- d stays open.

**Session 2 (`NV50_PROG_USE_NIR=1`, 62 s in the heavy area):**
- **What the tester saw:** small shadow errors and darker lighting.
- **Speed:** load-matched against session 1, -21.2 % in the base state (90 %: -26.2 to -16.3; 19.4 against 24.6 FPS) and -18.4 % in d.
- **Compiling:** NIR compiles cost about 540 ms of worker time per program against about 95 ms with TGSI (with its disk cache). 337 of 862 boot-queued programs were still queued when the session ended. Even so, nearly every heavy-area draw used its specialised variant, so most of the slowdown is not missing programs.
- **The crash on leaving the level:** a compile worker linking a program (`st_link_nir` → `nvc0_program_translate` → `nv50_ir::Function::convertToSSA` → `RenamePass::search` → `LValue::LValue`), a null access at 0x20.
- **NIR is out.** The Mesa edits are reverted in the tree and `mesa_nir_full_edits.py` is no longer applied by `tools/build_mesa.sh`; the script stays with that note. `libnir` is no longer linked, and the NRO is back to 115 MB with no NIR code.

**What makes ZCULL reachable:** libnx has `nvGpuChannelZcullBind`, `nvGpuGetZcullCtxSize` and `nvGpuGetZcullInfo`, and deko3d enables ZCULL by default. See CPU_ROADMAP.md, "Zoom out after run 60".

## Log 61

- **The build is back to TGSI only.** States: `MAPXSHNLUBIkhwreg,MAPXSHNLUBIkhwregd` (d still open).
- **New bench variant, "pixels+verts+UBO+tex+state off":** what is left is the per-draw cost, the resolves and present.
- **The next real step is ZCULL** (roadmap item 1). It needs reading deko3d before a build.

- **Log 61 was not run.** The tester chose ZCULL first, and log62 keeps log61's bench variants.

## Deko3D study, 3 November 2026: no large gain on its own

See CPU_ROADMAP.md, "Deko3D study". In short:
- Any handheld GPU gain stops at the CPU wall, about +20 % in heavy scenes.
- The shaders would be the same.
- The installed libdrm_nouveau already binds a ZCULL context on Mesa's channel.

## Log 62 (handheld): ZCULL in nouveau, state c

**What it does** (`source/switch/mesa/mesa_zcull_edits.py`, register values from deko3d's source):
- Turns on the region and writes the init values once.
- Sets the ZCULL size registers from `nvGpuGetZcullInfo()` whenever a depth target is bound.
- Writes the clear depth hint before depth clears.
- Invalidates the ZCULL data when:
  - the depth target changes;
  - draws with a depth-writing shader have run;
  - a blit, a copy or a clear of another surface wrote depth.
- Without c, nothing changes: the region stays off as at screen init.

**Render targets are cut to fit.** They are 2048 rows tall, so at 1280 wide the region covers the top 1632 rows. The game draws in the top rows only. How the GPU treats depth below the region is unverified.

**States:** `MAPXSHNLUBIkhwreg,MAPXSHNLUBIkhwregc`. d leaves the A/B states and stays in the bench.

**New output:**
- `[ZCULL]` per state: framebuffers set up, invalidations by cause, targets without it, regions cut.
- Bench variant "ZCULL on" or "ZCULL off": the same frame with ZCULL flipped.
- `[ZCULLCHECK]` every 900 replays, in any state: the frame drawn with ZCULL off, on, and off again, compared pixel by pixel against the off-off noise. A flagged frame keeps its three images as `zcheck_<replay>_*.png`.

**Risks:**
- A GPU fault or hang while c is on, if a register is wrong.
- Objects missing or cut where ZCULL culls wrongly: ask the tester to look in state c.
- Little gain if the hardware cannot tighten ZCULL from alpha-tested draws. The world draws are almost all alpha-tested, so this is the main unknown.

**Expected:** some of the hidden-layer shading goes. The forced early test was -17 to -20 % of the GPU frame, and it was wrong. The pixel check and the bench decide.

## Run 62: `0.0.68-log62` handheld, 3 November 2026: ZCULL is exact and saves nothing

Handheld, stock clocks (CPU 1020, GPU 307.2, memory 1331.2 MHz), states `MAPXSHNLUBIkhwreg` and `MAPXSHNLUBIkhwregc`. No crash. The tester saw no difference in the picture.

- **ZCULL works as set up and is exact.** `[ZCULL]` counts about 4000-5000 framebuffers set up per 6 s visit in heavy play. Every region was cut to fit the 2048-row targets. All six `[ZCULLCHECK]` frames (1512-1983 records) matched to the pixel: 0 px against 0 px of noise.
- **It saves nothing.**
  - Bench: "ZCULL on" +0.12 ms (5 benches) and "ZCULL off" -0.32 ms (9 benches), against a noise floor of about ±3 ms.
  - FPS at 1300+ draws: 27.3 with c against 27.0 without, raw. Load-matched, the base was ahead by 6.1 % (90 %: +1.0 to +11.3) over 13 visits each. The bench shows no GPU change, so the GPU and FPS figures disagree, and this is read as visit noise.
- **Why ZCULL cannot help here.** `[EARLYZ]` shows that every draw with the alpha-test code has a real test: function 4 (greater) 4.2 M draws, 6 (greater or equal) 1.4 M, never 7 (always). Depth-writing draws without that code are rare. A draw that may discard cannot teach ZCULL its depth, so ZCULL holds little more than the clear value and has nothing to cull. Whether the hardware culled any tile is not measured: the ZCULL statistics counters were not read.
- **Decision:** c is out. The Mesa edits stay applied; without c they leave the region off as before.
- **d joins the base.** It is exact, and its bench saved 1.3 ms here ("fetch constants specialised", 14 benches) and 1.8 ms in run 60.

**The bench now has log61's state-off variant** (14 benches, plain frame median 37.0 ms):

| Change | ms |
| --- | --- |
| Pixels off | -21.6 |
| Plus vertices off | -24.0 |
| Plus uniform buffers off | -25.2 |
| Plus textures off | -26.4 |
| Plus state off | -26.6 |

- **About 10.4 ms is left with everything off.** By kind: plain draws 5.4 ms for about 1300 (4 µs each); quads 2.75 ms for 213 (13 µs each); rects 1.7 ms for 22; points 0.8 ms for 72; resolves 3.7 ms.
- **The quad, rect and point draws all run through a geometry shader.** Each quad draw costs about three times a plain draw even with pixels, vertices, buffers, textures and state off.
- **Each per-draw stamp is a pipeline report that flushes up to stream output** (nouveau's timestamp query, `0x00005002`, without the flush-disable bit). Part of the 4 µs per draw may be the profiler itself. Unstamped heavy frames (`[GLSPAN]`) take 39-43 ms of GPU time, so the GPU still limits handheld.

## Log 63 (handheld): quads without the geometry shader, state q

**State q** (`quad_expand_frame`, `gl_backend.cpp`): quad lists are drawn as indexed triangles, without the quad geometry shader.
- The shader emits the strip 0,1,3,2. That is triangles 0,1,3 and 3,1,2, each with its last vertex as the provoking vertex, and q issues exactly those.
- Non-indexed draws use a fixed index pattern.
- Draws with indices in the arena get their own raw elements reordered. The vertex shader swaps the elements, so they are copied unchanged.
- Draws with indices in shared memory keep the shader: guest memory at replay time may differ from what the GPU reads.
- The program without the shader builds in the background, and draws use the shader until it is ready.
- **The one known difference:** the shader drops a whole quad when any of its positions is NaN, and plain triangles do not.

**States:** `MAPXSHNLUBIkhwregd,MAPXSHNLUBIkhwregdq` (d in the base).

**New output:**
- `[QUADS]` per state: quad draws, how many were drawn as triangles, and how many kept the shader, with the reason.
- **Bench variants:**
  - "quads as triangles" or "quads through the geometry shader";
  - "plain without per-draw stamps": only the frame's first and last stamps, to price the profiler's own reports.
- **Pixel check:** it is now generic. `LSWTCS_EXACTCHECK=900` with `LSWTCS_EXACTWHAT=q` prints `[EXACTCHECK]`, and a flagged frame keeps `xcheck_q_<replay>_*.png`. The ZCULL variant leaves the bench.

**Expected:** up to the quads' fixed cost, about 2 ms (5 %), if it comes from the geometry shader and not from the stamps. The bench and the stamp-free variant separate the two.

## Run 63: `0.0.69-log63` handheld, 3 November 2026: quads without the geometry shader are exact and a little faster

Handheld, stock clocks, states `MAPXSHNLUBIkhwregd` and `MAPXSHNLUBIkhwregdq`, about 4 minutes in the level. No crash.

- **q is exact.** All three `[EXACTCHECK]` frames (1343-2040 records, 178-230 quad draws as triangles) matched to the pixel: 0 px against 0 px of noise. No quad draw had its indices in shared memory. Only the first frames of a new program kept the geometry shader while it built.
- **q saves about 1 ms of GPU time.**
  - Bench: "quads as triangles" -1.24 ms (3 benches, all below 0: -2.3 to -0.4). "Quads through the geometry shader" +0.44 ms (6 benches, -0.4 to +3.8). The quads' own column moves by -1.7 and +1.1 ms.
  - FPS at 1300+ draws: 26.8 with q against 25.8 without, raw. Load-matched: -2.0 % (90 %: -9.7 to +5.2), 9 visits each, so no FPS conclusion.
- **Decision: q joins the base.** It is exact, and the bench agrees in both directions.
- **d confirmed a third time:** turning it off costs +1.38 ms (+0.4 to +2.5, 9 benches).
- **The per-draw stamps cost little:** the stamp-free frame is 0.6 ms faster (median of 9; -2.2 to +0.2).
- **So the 10 ms left with everything off is mostly not GPU work in normal frames.** With pixels, vertices, buffers, textures and state off, the GPU runs out of work and waits for the render thread and Mesa's worker to submit the frame. That residual measures the submission time. In a normal frame the GPU has about 36 ms of work against a submission time well below that, so the residual hides under the real work. This is an inference: the bench does not time the CPU side of each variant.

**Where handheld stands:**
- **GPU:** about 36 ms in heavy frames (bench plain median 36.2; unstamped `[GLSPAN]` 39-41 ms at the heaviest). The main parts are:
  - pixel work, about 22 ms (world draws about 11, quads about 4, full-screen passes about 5);
  - resolves, about 5 ms;
  - vertices, about 2.5 ms.
- **CPU wall:** docked heavy frames run at 29-30 FPS at the same CPU clock. So GPU savings beyond about 4 ms no longer raise handheld FPS.
- **What is left that is exact:**
  - **The resolve texture hand-over, at most about 2 ms.** It needs a coordinate scale and edge clamping per fetch.
  - **The full-screen passes.** The costliest (`91834C0972EDD8A1`, about 3 ms) is a single loop of the translator's skip-flag control flow with 4 texture fetches. A hand-cleaned version is unmeasured.

## Log 64 (docked, two sessions): guest calls that can inline

**Why:** docked heavy frames are limited by the main guest thread, about 24 ms of CPU, 85 % of it guest code at about 155 k guest calls per frame. Handheld is about 4 ms of GPU from the same wall. No guest call is ever inlined today:
- Every generated function is a weak, noinline alias (`PPC_WEAK_FUNC`) so the runtime can replace it.
- GameCode builds with -fPIC, so GCC treats every global function as replaceable. A test file showed that -fPIC alone blocks inlining even of a strong alias.
- Units were alphabetical batches of 5 files: only 19 % of the hot functions' call sites land in the caller's unit.

**What changes** (CMake option `LSW_GAME_INLINE`, on):
- **Grouping:** `tools/make_game_units.py` writes `game_units.txt`, 176 units of at most 20 files grouped greedily along the hot functions' call sites (`hot_order.ld`). 5195 of 10501 hot call sites now fall in the caller's unit (49 %).
- **Overrides:** the same tool writes `include/lsw_guest_overrides.h`, the 80 guest functions the runtime defines strongly, read from the log63 ELF.
- **Linkage:** each unit includes `include/lsw_guest_linkage.h` after `ppc_recomp_shared.h`. It keeps weak, noinline linkage only for those 80 and makes the rest strong aliases, so the generated output stays as checked. A new override missing from the list fails the link with a multiple definition.
- **Flags:** GameCode adds `-fno-semantic-interposition` and the -O3 automatic inlining limit (`max-inline-insns-auto=30`).
- **First unit:** 5304 call relocations remained of 6989 call sites in its sources.

**Test:**
- One docked session with log63 and one with log64, same state `MAPXSHNLUBIkhwregdq` and the same heavy stretch.
- The bench and the pixel check are off for both, so no hold adds noise.
- Compared with `private/session-tools/run_pair.py`: load-matched, 90 % interval. Runs 58 and 59 in the same state differed by +0.1 % (-5.2 to +5.5).
- `[THREADCPU]` gives the main thread's CPU per replay in each.

**Risk:** inlining keeps C++ semantics. It can expose undefined behaviour in the generated code that separate functions hid, such as strict aliasing (GameCode does not build with -fno-strict-aliasing). The signs would be a crash or a visual difference.

**The build:**
- **Size:** code text fell from 101.6 to 95.4 MB, and the NRO from 115.0 to 108.8 MB. Fewer prologues, spills and GOT loads.
- **Checks:**
  - The validator passed.
  - All 80 overrides still replace the generated body.
  - Every other `sub_X` is a strong alias at its body's address.
  - `_start` is at 0, and the hottest guest function follows `.crt0` (hot order kept).
- **Build time:** a full GameCode build with 2 jobs took about 25 minutes (177 units).

**On the SD:**
- `switch/lswtcs/lswtcs.nro` is log64.
- `switch/lswtcs-log63/lswtcs-log63.nro` is log63, the reference.
- One `env.txt` serves both.

**One session is enough.** Main-thread CPU per guest call is steady across builds and modes. The guest-call counter runs in every prologue, inlined or not, so it counts the guest's work, not the build. Median over heavy `[THREADCPU]` windows (100 k calls or more):

| Run | ns per call (90 %) |
| --- | --- |
| 54, docked | 161 (160 to 164) |
| 54, handheld | 156 (154 to 208) |
| 62 | 158 (156 to 160) |
| 63 | 156 (155 to 157) |

One log64 session compares against these. The log63 NRO is kept aside in case a reference session is wanted.

## Run 64: `0.0.70-log64` handheld, 4 November 2026: inlinable guest calls change nothing

Handheld, stock clocks, one session, state `MAPXSHNLUBIkhwregdq`, 54 visits. The session ran log64: the profiled code range is 75.16 MB, log64's `.text`. The tester saw nothing wrong.

- **CPU per guest call did not fall.** Main-thread CPU per guest call was 161 ns (90 %: 160 to 163) over 20 heavy windows, against 156 in run 63 and 158 in run 62.
  - The spread between sessions is about 3 %, not the 2 ns of one session's interval: run 54 gave 161 docked and 156 handheld with the same build.
  - So the change is nil within that spread, and if anything slightly worse.
- **The main thread's profile is unchanged.** It has the same top functions at the same shares: `sub_8231ED30` 5.4 → 5.2 %, `sub_82320328` 3.6 → 3.8 %, and so on. The top 40 hold 54 and 56 %.
  - The time is spent inside large guest functions (`sub_8231ED30` is 790 generated lines), not in calls. Inlining small callees leaves that time alone, and LTO would hit the same limit.
- **What the hot instructions are**, from per-PC samples of the main thread:
  - **Guest memory accesses.** Each one is a shift, a load from `ppc_view_base` (the 512 MB region table) and the access itself, plus a byte swap. They are volatile, so nothing is cached or merged.
  - **Scalar float compares** (`fcmp`, `fcmpe`) on values converted from single precision.
  - **Writes of guest registers back into `ctx`** after each instruction, scalar and vector.
  - **FPCR writes** where the code switches between scalar float and vector work (flush-to-zero on or off). The samples within two instructions after an `msr fpcr` are 1.6 % of the top 55 functions, which hold 60 % of the samples.
- **Decision:** `LSW_GAME_INLINE` is off by default, and its tooling stays. The SD has the log63 NRO back.

**What is left on the CPU side, sized honestly:**

| Option | Size | Cost and risk |
| --- | --- | --- |
| The compare form of the address fold on this CPU (`LSW_ADDR_FOLD_CMOV`, already in `ppc_context.h`) | Unknown; on PC it was 20 % slower than the table | A flag and a 25-minute rebuild, measured in one session by CPU per guest call |
| Flush-to-zero left fixed instead of switched | At most about 2 % | Changes denormal results of either scalar or vector math: not exact |
| PGO | 0-9 % in the other ports | Instrumented build, a training session on the console, then a rebuild |
| Plain guest memory instead of volatile | bo2 measured +5.8 % with three other changes | A loop polling memory that another thread writes could hang; needs an audit and compiler barriers |

## Log 65: `0.0.71-log65`, any mode: loading with the guest turn free during card reads

The tester asked whether loading can be made faster. Built after run 64; not run yet.

**What run 64 shows:**
- **Load lengths** (frames under 100 draws until 30 frames in a row with 100 or more):
  - hub to Gungan: 6.6 and 8.2 s;
  - Gungan to hub: 21.2 and 21.4 s;
  - boot to the hub: 39.7 s, menus included.
- **The guest turn is the bottleneck while loading.** In the light band of the profile (frames under 300 draws: loads and menus), the guest threads together hold the turn about 100 % of the time.
  - Loader `sub_82506D60` holds it 22 % of the time and is blocked in a system call for 98 % of that (95 % in `fsFileRead`).
  - Loader `sub_82508C48` holds it 21 % of the time, blocked for 54 % of that (37 % `fsFileRead`, 6 % attribute queries, 2 % opens).
  - So for about 30 % of the light band, the turn sits with a thread that waits on the card. No other guest thread runs meanwhile, so reading and processing never overlap. On the 360 they ran on separate hardware threads.
- **Small reads.** The read-ahead cache refills 64 KB at a time, about 26 MB/s.

**What log65 changes** (`kernel_stubs.cpp`), in modes cycled over loads by `LSWTCS_LOADMODES`:
- **`-`:** as before.
- **`o`:** the thread leaves the turn during host file calls (read-ahead refills, opens, attribute queries), so other guest threads run while the card works. It comes back with the urgent rejoin the native reverb uses, so it does not wait out the holder's quantum and a round of the others after each read.
- **`b`:** refills of 512 KB instead of 64 KB.
- **Order:** the profile sets `-,ob,ob,-`.
  - The mode moves on only after a load of 4 MB or more from the card, followed by 10 s without file calls. So one load keeps one mode, and the small reads during play (music) do not move it.
  - Two loads close together (a short stay in the hub) share a mode. The four-entry order keeps a fixed pattern of loads from always meeting the same mode.
- **In every mode,** the refill reads into a per-thread buffer outside the cache lock. So no host lock is held while the turn is away.

**Telemetry:** `[LOADEP]` reports each episode of file calls. An episode ends after 2 s without one. It gives:
- the mode, and its VdSwap numbers (frames.log's serial column);
- the span of file calls;
- the game bytes read;
- card megabytes, refills, their time and throughput;
- opens and attribute queries, with their time;
- the level before and after.

**Measure:** `private/session-tools/load_episodes.py <run dir>` finds the loads in frames.log, as above. It gives each load its episodes and mode (a load with two modes is marked mixed and left out), then the median load length per destination and mode. The test is several round trips between the hub and Gungan, about 8 loads or more.

**Risk:** guest threads now run during file calls, which they never did before.
- A race the turn used to hide, in the game or the runtime, would show as a crash or a hang during a load in mode `ob`.
- Load length also depends on what the tester does (skipping, menus), so only medians over several loads per mode count.

**Also in log65, at the tester's request:**

- **Save dates** (`win32_switch.cpp`).
  - The Load Game screen shows a save's last write time (`GetFileTime`, through `NtQueryInformationFile`). Horizon's save data file system keeps no file times, so it read 1970.
  - Now the first write through a handle into the native save records the console's time (UTC) beside the file in `<name>.lswtime`, and beside each folder up to the root, in the same save transaction.
  - These files are hidden from enumeration, deleted with their file or folder, and read back as the file times. The guest converts them with the console's time zone (`ExGetXConfigSetting`, already in place).
  - A save written before log65 keeps the old date until it is saved again.
- **Face buttons** (`LSWTCS_PADLAYOUT`, profile `auto`).
  - `position`: as before.
  - `label`: Switch A is the 360's A, so A confirms as on the Switch, and the game's button letters match.
  - `auto`: `label` while the frames are light (under 150 records for 30 frames: title, menus, load and save screens), `position` in play. The in-level pause menu draws the level, so it stays by position.
  - The layout changes only while no face button is held. Hold L + R and press Minus for the next layout (`[PADLAYOUT]`).
  - The button icons are textures in the game's data; they are unchanged.
- **The console's name in the text** (`LSWTCS_TEXTPATCH`, on by default).
  - The game's platform text is a UTF-16LE CSV in GAME.DAT, uncompressed: 14 mentions in six languages ("360_GAMELOADING", "360_AUTOSAVE_WARNING"). The executable image has the English lines in ASCII.
  - Read-ahead refills and native LZ2K output that look like text are scanned, and so is the image once after it loads (only the phrases naming the console).
  - Each phrase is replaced by one no longer than it: "Xbox 360 console" by "Nintendo Switch", "Xbox 360" by "Switch", and so on. The rest of the string moves left and is padded at its end, so no other string moves.
  - `[TEXTPATCH]` prints each replacement.
  - Tested on the PC against the local GAME.DAT and image.bin: all 14 and both image lines are replaced, with the refill starting at an even or an odd offset. A phrase that straddles two refills is missed.
- **Studs over the legs** (runs 17-20, still open; the tester still sees it).
  - The stud draws' vertex shader (`8B12FE19F8B3BA1D`) multiplies by c0-c3 after its world rows c6-c8. So c0-c3 is the view-projection, shared by the draws of one camera.
  - Each full capture now gives every draw's `c03=` hash, and prints each distinct block once (`[GLCAPVP]`). Studs drawn with another camera (stale constants) would show there.
  - A toggle combo: hold L + R and click the left stick to step through `LSWTCS_ABTOGGLE` (`[ABTOGGLE]`; frames.log records each state). The toggled state holds until the next press. The profile steps through the base parts that reuse or skip data between frames: P, H and S together, then H alone, P alone, S alone.
  - The fault predates the GPU letters, and run 17 showed tile merging is not the cause.

## Run 65 and log66 (in brief, documented with log67)

**Run 65** (`0.0.71-log65`, handheld):
- Save dates and the "Xbox 360" text replacements were right, as the tester confirmed.
- No load ran in mode `ob`. The mode advanced only after 10 s without file calls, and the music reads every 2-4 s.
- The black brick parts and the studs over the characters come from the same draws (`8B12FE19`/`AB23B41D`), which sample a cube map while the game binds a 2D texture.
- The draws share the camera block c0-c3 with the characters, so it is not a stale camera.

**log66** (`0.0.72-log66`):
- A cube stand-in built from the 2D image (`[CUBE2D]`).
- A per-draw colour and depth probe in full captures (`zprobe_<replay>.bin`, read with `private/session-tools/zprobe.py`).
- The capture log gains the vertex system constants.
- The load mode moves on at each stretch of light frames (`[LOADMODE]`).
- A frozen scene counts as a menu for the pad layout (`[MENUPROBE]`).

## Run 66: `0.0.72-log66` handheld, 4 November 2026: the depth write without a test, and a hang on quitting

One session, three full captures, then a hang and a crash when the tester quit to the main menu.

**The black parts and the studs over the characters have one cause: depth written with the depth test off.**
- The cube stand-in did not remove the black parts.
- The depth probe shows what does. Before the characters, the game draws a box (`CDF1199F`, 12 triangles, alpha blended) with RB_DEPTHCONTROL 0x734: test off, write bit on.
- The GL backend applied that as an always-passing test with depth writes. Xenos, like D3D's ZENABLE and Xenia's normalized depth control, writes no depth with the test off.
- In capture 8197, on Jar Jar's body:
  - the character writes depth 0.99353, in front of the studs (0.99425);
  - the box then rewrites 0.99423, behind him;
  - the studs' dark decal (`85DC6248`, test on, no write) then passes over him.
- In capture 9024, the black band under the grey brick is a cell nothing coloured before the box. The box stamped its depth there, and the rock and waterfall drawn later failed against it.
- The box is in every capture since run 17 (studs over the legs, dark outlines).

**Hang and crash on quitting** (load mode `ob`):
- After the first 512 KB read of the quit load, the game's I/O thread (`sub_824FD9A8`) and loader (`sub_82508C48`) passed the turn back and forth for about 50 s: about 150,000 switches a second, 5 µs each, no file calls. The screen stayed black.
- Then the loader crashed reading guest address 0x02422379 (`sub_824FC308`, called from `sub_8230C8C8`).
- The turn released during file calls (`o`) is the likely cause: the game may close a file while another thread reads it, which the turn used to prevent.

**Loads:**
- With 512 KB refills (`b`), the card read 60-66 MB/s against 23-35 MB/s, and Gungan to hub read 71.5 MB in 1.1 s against 58.5 MB in 1.7 s.
- The file-call span of that load barely moved (4.4 against 4.6 s), so loads are not bound by the card or by the turn during reads.

**Character selection:**
- The hub's character screen has 171 records: 144 in the 720-row HUD viewport and no scene draw.
- It was neither light (under 150) nor a frozen scene, so it stayed by position.

**Boot:**
- About 16 s pass from the first log line to the first frame, with the guest 94-99 % idle.
- Then come about 10 s of logo screens, the menus (the tester's time), and an 11 s load to the hub.
- Before the first frame, the render thread reads the roughly 950 saved programs (48 MB) one file at a time.
- The program binary cache stays off. It is opt-in after 2026-10-08 (garbage on the title, slow builds) and under the "cheaper, not cached" rule.

## Log 67: `0.0.73-log67`

- **Depth writes only with the depth test on** (`apply_state`), as Xenos does. `LSWTCS_ZWRITE_NOTEST=1` restores the old behaviour.
- **A frame with no scene draw counts as a menu** (at least 150 records, under 30 scene draws, for 10 frames), so the character selection is by label (`[MENUPROBE] ... no scene, a menu`).
- **Loads use `b` only** (`LSWTCS_LOADMODES=b`), with `o` off.
- **The saved programs are read by a background thread** at the lowest priority (`LSWTCS_GLPROGBG`, default on). A program the game asks for first is built on request, and the reader then skips its key.
- **`[BOOT]` marks** in milliseconds since `main()`:
  - platform up, image loaded, patches done;
  - the render thread's EGL/Mesa, GL resources, program cache and ready;
  - guest frames 1 and 300.

## Run 67

The tester's verdict:
- The black parts and the studs over the characters are gone.
- The character selection takes A to select.
- Quitting to the menu works: no black screen, no crash, with `o` off.
- The cantina shop still uses the Xbox positions.
- The boot feels as long as before.

**Loads** (`b` only, `load_episodes.py`):
- Hub to Gungan: 11.8 and 5.7 s.
- Gungan to hub: 23.3 s (after the level's end) and 12.4 s (quit).
- The card read 55-69 MB/s.

**Boot:**
- `[BOOT]`:
  - platform and save storage up at 8.4 s;
  - image loaded at 10.1 s;
  - `_xstart` at 12.3 s;
  - render thread ready at 12.6 s;
  - first guest frame at 23.0 s, frame 300 at 29.1 s.
- Reading the saved programs in the background (8.0 s, done at 20.6 s) did not move the first frame.
- The guest waits about 10 s after `_xstart` in every run back to run 7 (269 saved programs, against 941 now). In each run, `[SCHEDPROF]` shows the same pattern:
  - one 5 s window with the main thread at about 5.5 %;
  - one window 98.7 % idle;
  - then play.
- hwprof, first window: the main thread's top stack off the turn is a host `WaitForSingleObject` (1222 samples at 8 ms, about 9.8 s). The guest caller lies beyond the 8 sampled frames.
- Offline: the guest's `WaitForSingleObject` (`sub_8229F3D0`) has 27 callers. Among them is `XGetOverlappedResult` (`sub_8229E890`), which waits with no timeout on the XOVERLAPPED's event. It is a candidate, not confirmed.
- The 8.4 s before the save storage is up may include the user selector (run 66: under 5 s by the clock lines).

**Cantina shop:**
- The shop is a live scene: 196-317 scene draws, all new each replay, with 14-16 HUD draws. No menu rule fired.
- In capture 2768, the text shader draws the "B" and "A" icons before "Exit" and "Select", each as one quad of a 256x128 DXT5 sheet (draws 313 and 311). The depth probe puts draw 313 on the B icon.
- The play captures (6020, 7332 and those of runs 64-66) draw no such quad.

## Log 68: `0.0.74-log68`

- **Button prompts count as a menu:**
  - two or more icon quads in the HUD viewport for 10 replays switch to the label layout;
  - fewer than two for 10 replays switch back;
  - the log shows `[MENUPROBE] ... button prompts, a menu over a live scene`, and the 10 s line counts the icons.
  - Risk: a hint during play that shows two icons switches the layout while it is on screen.
- **`[LONGWAIT]`** logs any guest wait on a host object (`NtWaitForSingleObjectEx`, `NtWaitForMultipleObjectsEx`, `NtSignalAndWaitForSingleObjectEx`) that lasts 1 s or more, with:
  - the wait's length, its timeout and its object;
  - the thread's PCR;
  - the guest LR and the return chain (the saved LR of each frame up the back chain).
- **More `[BOOT]` marks:**
  - logs and settings, monitor and profiler, buffer check;
  - how the user was chosen: preselected, the only user, or the selector (shown, then picked);
  - guest memory mapped and cleared, image read, data and dispatch table, host pointer scan.

## Run 68

The tester: the cantina shop now takes A to select.

**Pad layout:**
- `[MENUPROBE]` switched to "button prompts" once, in the hub (2 icons, 221 scene draws), and back on leaving.
- No switch during the two Gungan visits.

**Boot** (`[BOOT]`, `[LONGWAIT]`):
- The user selector was on screen from 79 ms to 3947 ms: the tester's time.
- Guest memory took 247 ms.
- The image read took 1.40 s (21 MB, about 15 MB/s).
- Data and the dispatch table took 0.29 s.
- The host pointer scan took 1.96 s.
- `_xstart` came at 7.9 s, the render thread was ready at 8.2 s, and the first guest frame came at 18.5 s.
- The main thread waited 10000 ms with a 10000 ms timeout on object F0010010, in `sub_8250D550`, which was called from the host link setup `sub_8250DFF8` (chain up to `_xstart`).

**What the 10 s wait is:**
- The engine init `sub_82508480` takes tag 65: the host file server mode and a host name.
- `sub_8229DDB0` passes the mode from 0x82FCD190 (0 in the image).
- `main.cpp` sets 0x82FCD190 to 1 ("disable logging path"). The flag's other use in `sub_8229DDB0` is the `d:\` base path (`sub_824FDCB0`), not logging.
- With mode 1, `sub_82509598` creates the host link:
  - 16 `_event[%d]` and `fileinfo_event[%d]` events, plus "connect";
  - `sub_8250D550` then waits on "connect" in 10 s tries for a "CONN" reply, parsed by `sub_8250D978`.
- No PC tool answers. Our `NtWaitForSingleObjectEx` returns success at the timeout, so the loop ended after one try, with the link not connected (+1410 = 0).

**The other waits** over 1 s are the job workers idling with no timeout.

**Loads** (`b`):
- Hub to Gungan: 13.7 and 6.0 s.
- Gungan to hub: 13.6 and 12.6 s.

**Frame rate** (handheld):
- Only 12 s in the heavy band, 24.2 FPS.
- Paired against run 67: -5.0 % (90 %: -13.2 to +8.4, 4 visits). Inconclusive: the session was about loads.

## Log 69: `0.0.75-log69`

- **No wait for the host link's reply.** `sub_8250D550` is overridden: it marks the link not connected and returns 1, as the original does after the timeout. The game's state is the same, minus the 10 s. `LSWTCS_HOSTWAIT=1` restores the original. The log shows `[HOSTLINK]`.
- **Host pointer scan:** `getenv("LSWTCS_BROADSCAN_DATA")` is read once, not once per word (about 1 M calls). The same words are zeroed.
- **Image read:** unbuffered 4 MB reads into a heap buffer, then copied into the guest window. The bytes are the same.
- **`[LONGWAIT]`** skips waits with no timeout after 30 s from `main`.
- Expected first guest frame: about 2-3 s plus the user selector's time, against 18.5 s.

## Run 69

The tester: everything worked, and the game opened faster.

**Boot** (`[BOOT]`):
- User selector: 79 to 3740 ms (the tester's time).
- Guest memory: 0.25 s.
- Image read: 0.28 s (was 1.40).
- Data and dispatch table: 0.29 s.
- Host pointer scan: 18 ms (was 1.96 s).
- `_xstart` at 4.6 s; `[HOSTLINK]` at about 4.8 s, with no wait.
- First guest frame at 5.4 s (run 68: 18.5 s). Without the selector, about 1.8 s.

**Errors:** none. The log ends at a `[CLOCK]` line: the game was closed from the system.

**Frame rate** (handheld):
- 1300+ draws: 26.3 FPS over 54 s.
- 300-1300 draws: 40.1 FPS.

**Loads** (`b`):
- Hub to Gungan: 10.1 s.
- Gungan to hub: 22.5 s from the pause menu. The loading screen itself lasted about 11 s; the rest was the pause menu and the exit screens.

**Audio:**
- Empty in play (150+ draws): 3.1 s, against 0.4 s in run 68 and 0.2 s in run 66.
- About 1.9 s of it came during the first arrival at the hub (37-45 s of frame time, 5-49 FPS). The background compile of the 950 saved programs (about 46 s on two workers) was still running, now that the boot is 13 s shorter. The render thread built 23 of the hub's programs on request (`[GLSTALL]` replay 1737, 96 still queued), with the audio held while it waited.
- About 0.8 s came at 60 s: new shaders (`[SHADERNEW]` at swap 2594), compiled on the spot and saved for the next session.

## Log 70: `0.0.76-log70`

- **Program use order:**
  - `glprogs2/order.bin` keeps the keys of the programs the session asked for, in the order of the first request.
  - It is saved after 32, 128, 256, 512 and 1024 programs, as this session's order followed by the earlier order's keys not yet seen.
  - The boot reader queues those programs first, then the rest in directory order.
  - The log shows `[GLPROG] ... N of them first in the last session's order`; `LSWTCS_GLPROGORDER=0` turns it off.
  - The first log70 session writes the file; the effect shows from the second boot on.
- **Local co-op** (same build, at the tester's request; the game takes two players):
  - `LSWTCS_PLAYERS=2` (default) reads the handheld Joy-Cons, controller 1 and controller 2 apart. The players are the connected ones in that order, so a lone controller or the handheld is player 1.
  - `XamInputGetState` and `XamInputGetCapabilities` answer for player 2 while a second controller is connected. Player 2 joins with Start.
  - A single Joy-Con is held sideways (hold type horizontal), with its face buttons by position: the bottom button is the 360's A. SL and SR are the bumpers, Plus or Minus is Start.
  - L + R + Plus (SL + SR + Plus/Minus on a single Joy-Con) opens the system's controller screen, on its own thread, to split Joy-Cons.
  - `LSWTCS_JOYROT=1` turns a sideways Joy-Con's stick by hand, if the system does not.
  - `[PADS]` logs who is who.
  - Untested: the stick direction of a sideways Joy-Con, the controller screen from hbmenu, and the game's split screen in the GL backend.

## Run 70

The tester, for one session (the first boot of log70, so no program order yet: `0 keys`):
- Local co-op works. The `[PADS]` lines show handheld plus Pro Controller, Pro Controller plus two Joy-Cons, and two single Joy-Cons, through the controller screen (rc 0).
- A sideways Joy-Con's stick came out a quarter turned: the system reports it as for an upright Joy-Con. On the right Joy-Con, right moved up and down moved right; on the left one, left moved up and up moved right.
- Single Joy-Cons stayed by position in menus, so confirm was not on the right button.
- The hub's audio is still broken when the hub is reached quickly.

**Audio at the hub:**
- The first arrival came at 40-46 s of frame time, with 18-43 FPS and 0.3-0.6 s of empty audio per second. The render thread built 18 programs on request, with 85 still queued.
- The second arrival (after the compile ended) had 0.15 s in all.
- The game's audio feed (`[AUDIOOFF]`) fell to 478-526 calls a second while the saved list compiled, against 750 before and after. This held even at normal frame rates.
- The cause: compile worker 1 and the list reader ran on core 0, the guest threads' core, at their priority (0x3B). The reader took 15.4 s for 945 files.

**Frame rate:** 1300+ draws, 25.8 FPS over 326 s, much of it with two players.

## Log 71: `0.0.77-log71`

- **Worker 1** (core 0) takes jobs only while the render thread waits for a frame's programs, when the game is held anyway. It waits on its own condition variable. `LSWTCS_GLWORKER1=always` restores the old behaviour.
- **The saved-list reader** runs on core 2 at 0x2D, beside worker 0 and below the audio threads there.
- **The program order is saved every 32 programs, up to 2048.** log70 saved at 32, 128, 256, 512 and 1024, and a session asks for about 300-400 programs.
- **Sideways Joy-Con:**
  - The stick is turned a quarter by default: left Joy-Con (x, y) -> (-y, x), right Joy-Con -> (y, -x). `LSWTCS_JOYROT=0` passes the stick as the system reports it.
  - Its face buttons follow the layout. By label, the right button is the 360's A (confirm), bottom is B, top is X, left is Y.
  - The layout switch also waits for the D-pad to be released.

## Run 71 (docked)

The tester went to the hub twice and started with two detached Joy-Cons, one in each hand.

**Joy-Cons:**
- `[PADS]` at boot: player 1 was the right Joy-Con sideways, player 2 the left one. The system kept the single assignment from run 70's controller screen.
- The game took a tester holding both Joy-Cons for two players.

**Hub audio:**
- The first arrival had 0.35 s of empty audio in all (run 70: about 2.4 s).
- `[AUDIOOFF]` held 750 calls a second, except one window at 572.
- Empty audio in play over the session: 0.98 s (runs 69-70: 3.1 s).
- The render thread built 6 programs on request at the arrival (run 70: 18).
- The order held only 128 keys (log70's milestones). The reader on core 2 took 60 s for 955 files, waiting on worker 0 at the same priority.

**Loading-screen audio:**
- Audio empty per second of light frames, after the first minute:

  | Runs | Refills | Empty audio per second |
  |---|---|---|
  | 63-66 | 64 KB, or mixed | 5-14 ms |
  | 67-69, 71 | 512 KB (`b`) | 47-66 ms |

- Run 70 had 14 ms, over a session with mostly menus.
- In run 71 one 256 KB read took 377 ms (`[GSPIKE]` swap 13059), while the music ran dry for 0.8 s.
- `b` never shortened a load.

**Frame rate** (docked, 768 MHz GPU):
- 1300+ draws: 30.0 FPS over 60 s.
- 300-1300 draws: 52.3 FPS.
- Run 54 docked had 29.3-30.7 FPS in its best states: still the CPU wall.

## Log 72: `0.0.78-log72`

- **Joy-Cons at start:** No1 and No2 go to dual assignment. Two single Joy-Cons (one left, one right) are merged into one controller (`hidMergeSingleJoyAsDualJoy`, `[PADS] ... joined as one controller`). Two players with a Joy-Con each come only from the controller screen, then player 2's Start.
- **`LSWTCS_LOADMODES=-`:** back to 64 KB refills.

## Run 72: `0.0.78-log72` handheld, measurement by place

One session for the author's update: the hub, then `Gungan_A` to `E`, a single player, no captures. FPS counts only frames with 150 or more draws (450 or more for 3-tile builds) and frame times under 1 s. The heavy part is 1300+ draws (3900+ before one tile).

| Place | Run 10 (oldest with level tags) | Run 72 handheld | Run 71 docked |
| --- | --- | --- | --- |
| Hub | 14.7 | 44.2 | 55.0 |
| `Gungan_B` | 14.1 | 29.4 | 36.8 |
| `Gungan_B` heavy part | 9.8 | 27.4 | 30.0 |
| `Gungan_C` | 15.9 | 31.5 | 53.5 (18 s) |
| `Gungan_E` | - | 33.1 | - |

The draw-count bands used for the previous update no longer fit handheld: with the GPU as the limit, frames with fewer draws are not lighter. Run 71's heavy-band 2500-4000 reading of 37 FPS came from 14 s of frames.

## Run 73: `0.0.78-log72` handheld, a level never seen before, crash

One session: the hub, then Episode I `PalaceRescue` (`Rescue_A`, `b`, `c`, `b` again, `e`). The tester felt uneven frame pacing and stutters. The game ended at 592 s.

**Crash:**
- `std::bad_alloc` on the walker thread, then `abort()`.
- Stack: `operator new` < the rehash of `get_shader`'s address memo (`gl_bridge.cc:132`) < `prepare_full` < `pm4_extract_ib` < `async_walker_body`.
- No GL error before it (every `[GLDRAW]` err 0x0), and no log line gives the host heap's use, so the cause is not proven.
- Different from runs 68-72: the workers compiled 1122 programs (950-969 before). Run 70 ran 600 s without a crash.
- The address memo is never trimmed. Its key is the microcode's address, and each entry keeps a copy of the microcode.
- The tester's `glprogs2` after this run: 1288 programs, 56 MB of GLSL, median 45 KB. The next boot compiles all of them.

**FPS by place** (same filter as run 72):

| Place | FPS | p95 frame |
| --- | --- | --- |
| Hub | 38.0 | 40 ms |
| `Rescue_A` | 28.5 | 42 ms |
| `Rescue_b` | 30.0 | 43 ms |
| `Rescue_c` | 26.3 | 50 ms |
| `Rescue_e` | 30.5 | 38 ms |

**Stutters:**
- Frames of 250 ms or more: 6.5 % of play time in `PalaceRescue`, 1.7 % in run 72's `Gungan`.
- Most are first-use program builds: 156 programs built while the picture waited, 174 ms each (runs 66-72: at most 23, 3-21 ms each).
- Four waits were for one program alone, 499-601 ms each.
- Worker time per program grew from run to run: 91 ms (run 66), 136 (run 71), 193 (run 72), 249 (run 73). From log71, worker 0 shares core 2 at 0x2D with the guest audio thread and `xma` (0x2B) and with the log writer and Mesa's thread (0x2C).
- A new program goes to worker 0 at once, so the render thread finds nothing left to build and waits.
- Entering an area also decodes and uploads 100-160 textures in one frame: about 0.3-0.6 s decoding and 0.7-1.1 s uploading.
- Steady frames vary: 28-36 ms for 56 % of frames, 36-45 ms for 28 %. On a 60 Hz display that alternates between 2 and 3 refreshes. This is the frame rate itself, not a pacing fault.

## Log 73: `0.0.79-log73`

- **`[HEAP]` every 150 replays:**
  - The host heap: size, in use, free inside, untouched top.
  - Programs: compiled, known, queued, not compiled for memory, in use.
  - Stages, textures, the texture memo, samplers, FBOs and render-target MB.
  - Guest shaders, translations, and the address memo with its clears.
- **Allocation failure handler:** a failed C++ allocation logs `[HEAP] C++ allocation failed` with the heap's figures, then throws as before.
- **Address memo capped at 8192 entries.** When full it is emptied (the bucket array too). The shaders themselves stay.
- **Saved programs not asked for stop compiling when the heap has less than `LSWTCS_GLKEEPMB` free** (default 384 MB). One `[HEAP]` line says when this starts. Such a program is built if the game asks for it.
- **Worker 0 at 0x2A while a replay waits for its programs** (`LSWTCS_GLWAITBOOST`, 0 = off), back to 0x2D after. The audio is already held during the wait.
- **`[GLWAIT]` for waits of 100 ms or more:** programs waited for, built on the render thread (ms, longest), built by the workers meanwhile, and whether worker 0 was raised.
- **Texture cost:**
  - `[TEXSLOW]` logs each new texture whose decode and upload take 8 ms or more (the first 300): size, layers, mips, DXGI format, KB, decode ms and upload ms.
  - `[TEXCOST]`, with each `[HEAP]` line, gives the new textures since the last report by DXGI format: count, MB, decode ms and upload ms.
- **Profile:** `LSWTCS_GLSTEPS=1`, so each worker build logs its GLSL sizes and time (`[GLSTEP]`).

## Run 74: `0.0.79-log73` (first build, before the texture lines) handheld, crash on the second level in a row

One session: the hub, `PalaceRescue` again (`Rescue_A` to `e`), the hub, then `PodSprint_A`. The tester saw stutters in cutscenes and in play; the game crashed entering `PodSprint`, 418 s in. The SD's `env.txt` was the old one (no `GLSTEPS`), and the NRO was log73's first build, so there are no `[TEXCOST]` or `[TEXSLOW]` lines.

**Crash:**
- A data abort on a compile worker (a `std::thread`), inside Mesa's nouveau compiler (`buildLiveSetsPreSSA` calling `LValue::isUniform`), reading address 0x20.
- That code allocates its bitsets with C `malloc` and does not check for failure, so a failed allocation reads near 0. The C++ allocation handler cannot see C allocations.
- The last `[HEAP]` line, about 5 s before: 2829 MB in use of 3043 MB, 213 MB free (57 inside, 155 at the top). In between, a level load decoded 204 textures and built 11 programs. Likely out of memory; not proven.

**Heap** (`[HEAP]`):
- Replay 150: 1889 MB in use with 44 programs. Most of it is the guest window (595 MB) and the GL copy of guest memory (`[GL] resources: shared memory 512 MB`).
- From there it rose with programs built: replay 4650, 824 programs, 2579 MB; replay 5700, 879 programs, 2676 MB. After textures and render targets, that is about 0.6-1 MB per program.
- 822 compiled stages were alive at the end. They are kept for reuse and never deleted, and Mesa keeps each compiled-but-unlinked shader's whole GLSL IR.
- The 384 MB reserve stopped 244 saved programs from compiling, and the session still ran short.
- The address memo was cleared 24 times. It does grow without bound, so log73's cap was needed.

**Waits for programs** (`[GLWAIT]`):
- With worker 0 raised, single-program waits took 107-229 ms (run 73: 499-601 ms).
- At the end, with the heap nearly full, builds on the render thread took 513-1555 ms for one program.

**Stutters:**
- On a second visit, program builds are small (`program=` 6-120 ms per stall), apart from the end.
- Most of the time now goes to new textures at each area entry, cutscenes included: 77-204 textures per frame, decode 0.27-1.57 s and upload 0.68-1.42 s, about 7 ms per texture.

| Place | FPS | p95 frame |
| --- | --- | --- |
| Hub | 37.0 | 41 ms |
| `Rescue_A` | 29.1 | 42 ms |
| `Rescue_b` | 28.2 | 47 ms |
| `Rescue_c` | 26.0 | 53 ms |
| `Rescue_e` | 29.6 | 43 ms |

## Log 74: `0.0.80-log74`

- **Stage cache bounded** (`LSWTCS_GLSTAGEMAX`, default 96). The least recently used stages are deleted first. A stage between its compile and its program's link is pinned.
- **`[HEAPGL]`** with each `[HEAP]` line:
  - stages cached, evicted and reused;
  - the heap kept per stage compile and per link, as averages of in-use bytes around each step (other threads add noise).
- **Reserve for saved programs not asked for:** 512 MB (`LSWTCS_GLKEEPMB`).
- **The crash handler logs the heap's untouched top** (no malloc lock taken).
- Still in from log73's second build: `[TEXSLOW]`, `[TEXCOST]`, and `LSWTCS_GLSTEPS=1` in the profile.

## Run 75: `0.0.80-log74` handheld, crash entering `PodSprint` again

Same route as run 74: the hub, `PalaceRescue` (`Rescue_A` to `e`), the hub, `PodSprint_A`. The SD had log74 and the new `env.txt` (`GLSTEPS` on). The game aborted entering `PodSprint`, 483 s in: `[HEAP] C++ allocation failed`, 3013 of 3043 MB in use, 25 MB free inside, 3 MB at the top. The GL error 0x505 (out of memory) appeared just before.

**What holds the heap:**
- **Stages are cheap.** `[HEAPGL]`: about 25 KB kept per stage compile. Capping the cache at 96 did not help, so the IR was not the cause.
- **Programs cost about 0.35 MB at link and about 0.5 MB more at their first draw.**
  - Replays 600-4200: 504 programs linked, none drawn, +174 MB.
  - Replays 4200-6000: 267 more drawn, +442 MB, of which 174 MB were textures and render targets.
  - Total about 1 GB for 989 programs.
- **Entering `PodSprint`** (replays 15900-16050):
  - 207 new textures (139 MB) arrived in one frame, and about 750 old ones (about 180 MB) were evicted.
  - Yet the heap grew 193 MB.
  - Deleted textures did not give their memory back.
- **Why:** the process allocator is rpmalloc (`LSW_RPMALLOC`, on). Freed blocks of up to about 4 MB stay in its per-thread and global caches, one list per size class, and never return to the heap.
  - Per thread: up to 400 single 64 KB spans, and up to about 100 blocks in each larger class.
  - The global cache holds 8 times that.
  - Mesa's buffer objects (textures, staging copies) are `memalign` calls that land there.
  - `mallinfo` counts these cached blocks as in use, which also inflates the "kept per link" figure.

**Texture upload cost is per call, not per byte** (`[TEXSLOW]`, upload ms each):

| Texture | Count | Upload |
| --- | --- | --- |
| BC3 64x64, 7 mips, 5 KB | 43 | 19.5 ms |
| BC1 32x32, 6 mips | 5 | 37.5 ms |
| BC1 128x128, 8 mips, 10 KB | 13 | 16.1 ms |
| BC1 1024x1024, 1 mip, 512 KB | 32 | 3.4 ms |
| BC1 2048x2048, 1 mip, 2 MB | 18 | 8.9 ms |

- In Mesa 20.1, glthread runs `glTexSubImage3D` and `glCompressedTexSubImage3D` with `_mesa_glthread_finish_before`. Each call, once per mip, waits until the GL thread has executed everything queued.
- Textures are made inside the draw loop, so each waits for the draws queued before it.
- Decoding (untiling) costs about 5 ms per MB: 49 ms for one 2048x2048 BC1.

**Other figures:**
- Waits for one program built by a worker: 107-229 ms. One wait, after the heap filled: 1076 ms.

| Place | FPS | p95 frame |
| --- | --- | --- |
| Hub | 31.1 | 48 ms |
| `Rescue_A` | 29.5 | 41 ms |
| `Rescue_b` | 26.8 | 47 ms |
| `Rescue_c` | 26.6 | 49 ms |
| `Rescue_e` | 30.1 | 41 ms |
| `PodSprint_A` | 26.0 | 49 ms |

## Log 75: `0.0.81-log75`

- **Big blocks from newlib, small ones from rpmalloc.**
  - Blocks of 64 KB or more (`LSWTCS_BIGALLOC`, in KB; 0 = all rpmalloc) come from newlib's allocator, which merges freed blocks.
  - Each big block has a 32-byte header (raw pointer, size, check word, magic) just before it, so `free`, `realloc` and `usable_size` route it whatever the setting was at allocation. The header is zeroed on free.
  - Small blocks, the reason rpmalloc is used (parallel shader builds), stay in rpmalloc.
  - `[HEAPGL]` adds the live big-block MB and count.
- **New textures before the draws** (`LSWTCS_TEXPREPASS`, default on).
  - Before the draw loop, every texture the frame's draws fetch that is not in the cache is made while the GL thread's queue is nearly empty.
  - A texture whose memory overlaps one of this frame's resolve destinations (8 bytes a pixel, an upper bound), or a kept resolve result, is left to the draw loop. It is never made from guest memory before a resolve's copy lands there.
  - `[TEXCOST]` adds the session's made-before and left-to-the-loop counts.
- **Risk:** the allocator change touches every allocation in the process. A boot hang or crash in free means reverting it with `LSWTCS_BIGALLOC=0`.

## Run 76: `0.0.81-log75` handheld, no crash

One session of about 870 s: the hub, `PalaceRescue` (`Rescue_A` to `e`), the hub, all of `PodSprint_A` (298 s), the hub, `RetakePalace` (`Retake_A`, `Retake_B`), the hub. No crash. The tester still feels stutters and is not sure about the frame rate.

**Heap:**
- Peak 2719 MB in use. All 1255 saved programs compiled (none held back for memory).
- Evicting textures now lowers the heap: 2719 to 2533 MB at replay 25200.
- 324-574 MB free over the session. Big blocks from newlib at the end: 2159 MB in 1283 blocks.

**FPS by place** (same filter as run 72): no change from runs 73-75. Handheld is GPU-bound at the stock clock.

| Place | FPS | p95 frame |
| --- | --- | --- |
| Hub | 41.8 | 37 ms |
| `Rescue_A` | 28.9 | 43 ms |
| `Rescue_b` | 26.5 | 51 ms |
| `Rescue_c` | 25.5 | 53 ms |
| `Rescue_e` | 30.1 | 43 ms |
| `PodSprint_A` | 25.4 | 55 ms |
| `Retake_A` | 27.6 | 41 ms |
| `Retake_B` | 28.2 | 46 ms |

**Textures:**
- The pre-pass made all 3005 new textures before the draws.
- Upload still averaged 8.0 ms per texture (24.1 s in total), and decode 3.0 ms (8.9 s).
- So glthread's finish was not the cost.
- The cost follows the number of mip levels:
  - BC1 512x512, 10 mips: 13.2 ms.
  - BC1 1024x1024, 1 mip: 3.2 ms.
  - BC1 256x256, 9 mips: 16.8 ms.
- `nvc0_miptree_transfer_map` makes a new buffer object for every call, i.e. every mip level. On the Switch that is a memalign, a memset, `nvMapCreate` and `nvAddressSpaceMap`, and then `nvAddressSpaceUnmap`, `nvMapClose` and free after the copy's fence.

**Programs** (`[GLSTEP]`, 1235 worker builds):

| GLSL size | Median build |
| --- | --- |
| Under 30 KB | 95 ms |
| 30-60 KB | 157 ms |
| 60-100 KB | 271 ms |
| Over 100 KB (9 programs) | 1424 ms |

- The long waits in new levels are single programs built on the render thread, 0.6-2.6 s each (`PodSprint` and `RetakePalace` entries).
- They happen once per program: it is saved for later boots.

## Log 76: `0.0.82-log76`

- **Texture staging from Mesa's GART suballocator.**
  - `switch/mesa/mesa_texstage_edits.py` (`NVC0_TEX_STAGE_MM=1`, set by the port; `LSWTCS_GLTEXSTAGE=0` = off): nvc0 texture uploads take their staging range from `screen->mm_GART`, as buffer uploads do. They release it with `nouveau_mm_free_work` on the copies' fence.
  - Requests over 2 MB still get their own buffer object inside `nouveau_mm_allocate`.
  - The edit script is idempotent by its tag and runs last in `tools/build_mesa.sh`.
- **Pre-pass flush:** the texture pre-pass calls `glFlush` every 16 MB of new textures, so fences pass and staging ranges return to the suballocator. It keeps every slab it ever made, so the flush bounds that.

## Log 77: `0.0.83-log77` (A) and `0.0.83-log77nv` (B), guest memory without volatile

Route 2 item 3 (CPU_ROADMAP.md), the technique from the mk9-nx study (masseffect-nx measured +15-20 % fps; GPL-3.0, technique only). Both builds include log76 (texture staging), which has not been tested yet.

- **A** (`build-mesa`, `private/lswtcs-log77a.nro`): the current code. It prints `[NX] guest code: volatile loads and stores (log77 A)`.
- **B** (`build-nv`, NACP version `0.0.83-log77nv`): GameCode from `Convert 360/LSWTCS/output_nv`, made by `switch/tools/make_nonvolatile_output.py`. The generated output itself is unchanged. B differs in:
  - **`ppc_context.h`:** the eight scalar `PPC_LOAD_*` / `PPC_STORE_*` macros use `LSW_GV`, which is empty only under `LSW_GUEST_NONVOLATILE` (CMake option, GameCode target only).
    - The runtime includes the same header and keeps `volatile` for its own loops over guest memory.
    - The four `PPC_MM_STORE_*` (17 MMIO sites) keep `volatile` in both builds.
  - **A compiler barrier (`LSW_GUEST_BARRIER`, an empty asm with a memory clobber) after each of the 70 `sync` / `lwsync` / `eieio` / `isync`.** The generator emits these only as comments, so until now `volatile` alone kept the order of stores around them.
  - **The same barrier after each of the 33,616 labels that a later `goto` in the same function targets:** every loop header, including loops built from switch cases. Without it GCC may keep a value read from guest memory in a register for the whole loop. A loop waiting on another thread, the GPU or the walker would then never end.
  - **Unchanged:** `lwarx` / `stwcx.` (`LSW_GUEST_CAS`, an atomic) and vector loads and stores (never volatile).
  - **Log line:** `[NX] guest code: loads and stores without volatile, barriers at loop headers and fences (log77 B)`.
- **The two build caches differ only in `LSW_GUEST_NONVOLATILE`.**
- **SD:** `switch/lswtcs.nro` = A (227095b3...), `switch/lswtcs-nv.nro` = B (9aaad4f7..., 1 MB smaller NRO). Both validated.
- **Test:** docked, where the guest CPU is the limit. The same route on A then B (hub, then `Gungan_B`'s heavy part), one session each. Compare FPS by place, the heavy part, and CPU per guest call (`private/session-tools/guest_call_cost.py`).
- **Risk:** a hang in a loop whose wait the barrier does not cover. B is then dropped.

## Run 77: `0.0.81-log75` handheld (meant to be log77 B)

One session meant as log77 B in handheld: the hub, `Gungan_A`, `Gungan_B` (91 s of the heavy part), the hub. No crash.

**It was not B.**
- The log has no `[NX] guest code` line, which both log77 builds print.
- The profiler's code range (81,408,000 bytes) matches log75 and log76, not B (80,425,760).
- Render-thread samples only resolve to a coherent texture-upload path with log75's ELF (`nvc0_miptree_transfer_map` > `nouveau_bo_new` > `nvMapCreate`), so it was log75.
- The console's SD likely still had an older NRO.

**As a baseline for normal code in handheld:**
- CPU per guest call 162 ns (90 % 161-164). Runs 72 and 76 had 159 and 157.
- `Gungan_B`: 27.9 FPS, heavy part 26.3 (run 72: 29.4 and 27.4).
- Texture upload 7.4 ms per texture (run 76: 8.0).

## Run 78: `0.0.83-log77nv` (B) handheld, wrong game behaviour

One session on B (the log says `(log77 B)`): the hub, `Gungan_A`, `B`, `C`, `E`, `B` again, the hub. No crash.

**The tester saw:**
- The camera moving up and down by itself in the first part of the level.
- Audio that seemed slightly lower in pitch.

**Figures, against run 77 (log75, same route, handheld):**

| | Run 78 (B) | Run 77 | Run 72 |
| --- | --- | --- | --- |
| CPU per guest call | 158 ns (90 % 152-160, fit 152) | 162 | 159 |
| Guest calls per replay | up to 208 k | up to 169 k | |
| `Gungan_B` FPS | 25.3 | 27.9 | |
| `Gungan_B` heavy part | 23.0 FPS, p95 81 ms | 26.3 FPS, p95 55 ms | |

**Cause (very likely):**
- GameCode is compiled at `-O2` without `-fno-strict-aliasing`. The runtime has it.
- With `volatile`, guest accesses kept their order whatever their types. Without it, GCC may assume that a 32-bit store and a 64-bit or 16-bit load of the same guest address do not alias, and reorder or forward them. The game reads stale values.
- So B's figures do not measure the technique.
- B also ran with that extra (unsafe) freedom and still gained at most about 2-5 % per guest call. The fixed build can only gain less.

## Run 79: `0.0.83-log77` (A) handheld

One session: the hub, `Gungan_A` to `E`, the hub. No crash. Log76's texture staging (included in A) is tested here for the first time.

**Texture uploads:**
- 2.73 ms per new texture, against 7.40 in run 77 and 8.02 in run 76 (both log75). That is 65 % less.
- Render-thread samples no longer show staging buffer objects. The remaining `nouveau_bo_new` / `nvMapCreate` samples are the textures' own storage, one buffer object per texture.
- Small textures still cost the most: BC3 64x64 with 7 mips 9.1 ms; BC3 256x256 with 9 mips 7.7 ms.
- Decode: 2.18 ms per texture. A BC1 1024x1024 takes about 9 ms, on the render thread.

**Area-entry stalls in `Gungan`:**
- Texture time inside `[GLSTALL]` went from 3045 ms over 4 stalls (run 77) to 1600 ms over 6.
- The longest frame in `Gungan_A` went from 1565 to 765 ms.

**Two other stalls at the `Gungan_B` entry** (replays 5341 and 5350):
- They are 535 and 688 ms of `bind` (program, uniform buffers, texture units), not textures or program builds.
- 533 saved programs were still queued then: the reader took 131 s to queue the 1255 files, and the workers were still compiling.
- Mesa's GL thread was idle 77 % of the session, so the stall is not its translating. Cause not found.

**Other figures:**
- FPS: hub 37.3; `Gungan_B` 28.6 (heavy part 25.7); `Gungan_C` 31.3.
- CPU per guest call: 161 ns.
- Heap: 2438 MB in use at the end, 604 MB free.
- Clocks: CPU 1020 MHz, GPU 307.2 MHz, memory 1331 MHz. Docked (run 71): GPU 768, memory 1600.


## Log 78: `0.0.84-log78`, three stutter fixes

All three aim at run 79's stalls at area entries and level starts. Each has its own switch and prints its own figures.

- **Texture decode in parallel** (`gl_backend.cpp`, `LSWTCS_TEXPARALLEL=0` = off).
  - The pre-pass gathers the frame's new textures first, then decodes them on three threads: the render thread and two helpers. One helper is on core 2 at 0x2C. The other is on core 0 at 0x3B, so it runs while the guest is held at an area entry.
  - Below 3 textures it decodes inline. GL calls stay on the render thread, in the old order.
  - `lsw_gpu_decode_texture_into` (`lsw_gpu_texture_layout.inc`) decodes into a buffer the caller owns. `lsw_gpu_decode_texture` uses one buffer for the whole process.
  - `[TEXCOST]` adds the batch count, the summed decode work and its wall time.
- **A pool of freed texture storage** (`bo_check.cpp`, `LSWTCS_BOPOOL=0` = off).
  - Freed block-linear buffer objects of up to 4 MB in video memory are kept, up to 64 MB in all. A later request with the same size, alignment, flags, kind and tile mode gets one back without `nvMapCreate`, `nvAddressSpaceMap` or the memset.
  - These buffers hold textures and render targets.
  - Release waits for the buffer's fence, as `nouveau_bo_del` does.
  - The pool reads libdrm_nouveau 1.0.1's private BO (`reference-source/libdrm-nouveau/private.h`). The first buffer checks the layout. If it does not match, the pool turns itself off and says so in `[BOPOOL]`.
  - It is off while the BO checker is armed.
  - Reused storage is not cleared. GL leaves new storage undefined, and `create_texture` uploads every mip.
  - `[HEAPGL]` adds reused, new, kept and freed buffers, and the MB kept.
- **Saved programs by level** (`gl_backend.cpp`, `LSWTCS_GLPROGLEVEL=0` = off).
  - `glprogs2/levels.bin` keeps, per level name (`lsw_level_name`), the programs drawn there. It is merged over sessions and saved off the render thread.
  - When a level starts, its saved programs not built yet move to the front of the queue, in the level's order. They stay behind the programs the game already asked for.
  - The same happens again when the reader finishes. Later copies of a key are dropped from the queue.
  - `[GLPROG] level ...` prints what moved.
  - The reader runs at 0x2C, above worker 0 (0x2D). In run 79 it shared 0x2D with worker 0 and took 131 s.
- **Test:** handheld, a level played before, then one new to the session. Compare with run 79:
  - the `[GLSTALL]` texture time;
  - the longest frames at area entries;
  - the reader's time (`[GLPROG] ... queued ... in N ms`);
  - the stalls at the `Gungan_B` entry.
- **The first run builds `levels.bin`.** The level ordering only helps from the second session on.
- **Risk:**
  - Wrong pixels from reused storage, such as a render target read before it is drawn. Turn the pool off to check.
  - A hitch on Mesa's GL thread while the reader parses a file at its priority.


## Run 80: `0.0.84-log78` handheld

One session of 333 s: the hub, `Gungan_A`, `Gungan_B`, the hub, `PalaceRescue` (`Rescue_A`, `Rescue_b`), the hub, `PodSprint_A`, the hub. No crash.

**Telling the build apart:** log78 still prints `[NX] guest code: ... (log77 A)`, because that text was not updated. Its own lines are `[GLPROG] programs by level` and `[GLPROG] level ...`.

**Saved-program reader at 0x2C:**
- It queued the 1255 files in 16.7 s. Run 79 took 131 s.
- The cost: for those 17 s the boot's logo frames came 270-550 ms apart (`[GSPIKE]` with `rtwait`, 44 spikes against 15 in run 79). Mesa's GL thread, which presents, also runs at 0x2C on core 2.
- No `[GLWAIT]` of 100 ms or more, as in run 79.
- Run 79's `bind` stalls at the `Gungan_B` entry did not come back. Their cause is still unknown, so this is not credited to log78.

**Parallel decode:**
- 60 batches, 4216 ms of decode work done in 1625 ms.
- Upload per new texture over the session: 2.48 ms (run 79: 2.73).

**Area entries:**

| | Run 80 | Run 79 |
| --- | --- | --- |
| Hub arrival (892 draws, 86 textures) | 759 ms, then 466 ms | 665 ms, then 62 ms |
| its decode / upload | 113 / 459 ms | 278 / 200 ms |
| `Gungan_A` entry | 297 ms (decode 68) | 470 ms (decode 256) |
| hub returns | 295 and 441 ms (149 and 140 textures) | |
| `Rescue_A` | 430 ms (151 textures) | |
| `PodSprint_A` entry | 977 ms (256 textures: decode 520, upload 317), then 328 ms | |

- At the hub arrival, 19 uploads waited 13-41 ms each, about one display refresh. Run 79 had 6 at the same place.
- The next frame waited 420 ms on one 10 KB upload. `PodSprint_A`'s next frame waited 222 ms on one 5 KB upload.
- Cause not proven. The render thread waits on Mesa's GL thread or on the GPU. Log78 made all uploads after all decodes, so none overlapped a decode.

**Texture storage pool:** never used (`[HEAPGL]`: `unused`).
- On the Switch, libdrm reports no VRAM, so Mesa puts textures in GART (`GART | NOSNOOP`).
- The pool only took VRAM buffers.

**Programs by level:**
- Recorded: `Map\Map` held 177 programs at its second visit and 294 at its third.
- This was the first session with the file, so no level had a saved list and nothing moved.

**Heap:**
- 335 MB free at the end, after `PodSprint`. Run 76, on the same levels, peaked at 2719 MB in use.
- 34 saved programs were skipped by the 512 MB reserve.

**FPS** (time in each level, loading included; not comparable with run 79's route):
- hub 42.2 over four visits;
- `Gungan_A` 36.8, `Gungan_B` 32.6 (84 s);
- `Rescue_A` 28.1, `Rescue_b` 44.5;
- `PodSprint_A` 35.4.

## Log 79: `0.0.85-log79`

- **Upload as soon as decoded** (`gl_backend.cpp`, `decode_and_upload`):
  - The render thread uploads texture i once it is decoded. The helpers decode the next ones meanwhile.
  - While texture i is still with a helper, the render thread decodes one that nobody has taken yet.
  - Decoded buffers are freed one by one, so fewer are alive at once.
- **Texture storage pool, any domain** (`bo_check.cpp`):
  - The pool takes block-linear buffers of up to 4 MB, without `NOUVEAU_BO_CONTIG`.
  - It keeps up to 32 MB (was 64), and nothing while the heap has less than 256 MB free.
- **Saved-program reader back at 0x2D.**
  - Worker 0 takes only programs the game asked for until the reader is done.
  - The reader then gets core 2 whenever the SD card answers. Mesa's GL thread (0x2C) stays above it.
- **Test:** handheld, the same route as run 80. This is the first session with `levels.bin`. Read:
  - the logo frames at boot;
  - the reader's time;
  - `[GLPROG] level ...` lines that move programs;
  - `[HEAPGL]` pool counts and `[BOPOOL]`;
  - the hub arrival and `PodSprint_A` entry against run 80.
- **Risk:** wrong pixels from reused storage (`LSWTCS_BOPOOL=0` to check).


## Run 81: `0.0.85-log79` handheld, crash loading a save

The game crashed at the hub arrival after loading a save (replay about 1686, the 892-draw scene with 86 new textures).

**The crash** is open issue 1 in `docs/ISSUES.md`:
- Stack: `nvAddressSpaceUnmap` < `nouveau_bo_del` < `nouveau_bo_ref` < `__wrap_nouveau_bo_ref` < `pushbuf_flush` < `nouveau_pushbuf_kick` < `nvc0_flush` < `fence_sync` < `arena_fence` < `render_thread_main`.
- Fault address 0x58: a buffer object whose device pointer is null.
- This is the arena-fence path of earlier003. Its last occurrence was run 28.

**The texture storage pool:**
- Its last report (replay 1650) had 87 buffers tracked and none kept or reused.
- Keeps start only when textures are evicted; in run 82 that was around replay 9000.
- The crash cannot be pinned on the pool, but its last 36 replays were not reported.
- Log80 prints the pool's first keep and first reuse, so a later crash log shows whether the pool had started.

## Run 82: `0.0.85-log79` handheld

320 s on run 80's route: the hub, `Gungan_A`, `Gungan_B`, the hub, `Rescue_A`, `Rescue_b`, the hub, `PodSprint_A`, the hub. No crash.

**Boot and saved programs:**
- The boot is smooth again: one gap over 250 ms in the logos against 19 in run 80; 28 `[GSPIKE]` against 44.
- The reader at 0x2D queued the 1255 files in 16.3 s.
- `levels.bin` was read: 6 levels, 1075 entries.
- At the hub's start, 218 of its 1169 queued programs moved ahead. Later levels found theirs already built.
- No `[GLWAIT]` of 100 ms or more.

**Textures, against run 80 (same route):**

| | Run 82 (log79) | Run 80 (log78) |
| --- | --- | --- |
| Decode on the render thread | 824 ms for 4268 ms of work | 1625 ms for 4216 |
| Upload per new texture | 2.20 ms | 2.48 |
| Uploads of 12 ms or more | 49 | 30 |
| `Gungan_A` entry | 256 ms | 297 |
| Hub returns | 299 and 396 ms | 295 and 441 |
| `Rescue_A` | 379 ms | 430 |
| `PodSprint_A` entry | 879 ms, then 326 | 977, then 328 |

- These are single events: each is within about ±10 %, except the decode.
- The hub arrival is not comparable. Replay 1416 drew 293 draws with 39 textures, not run 80's scene. It took 587 ms, of which 479 were uploads (23 of 11-41 ms each); the guest also spent 821 ms preparing that frame.
- `PodSprint_A`'s next frame still waited 236 ms on one 5 KB upload.

**The texture storage pool:** on, with the layout as expected.
- 283 storages reused against 1437 new ones; 408 kept and 711 freed because the pool was full.
- It started keeping around replay 9000, after textures were first evicted.

**A 1.9 s freeze in `Gungan_B`** (replay 5537):
- The guest waited 1878 ms for the render thread, whose replay took 14 ms.
- In over-1-s freezes, the render thread was 88 % blocked in `nvIoctl2`, the GPU submit (run 80: 82 %).
- Worker 0 was building a 1136 ms program (68 KB vertex shader) at the time.
- Cause not found.

**Where uploads wait:**
- Render-thread samples inside `create_texture`: 38 % in `_util_queue_fence_wait`, waiting for Mesa's GL thread to run the commands queued before the upload.
- The GL thread's own texture-storage creation (`nvAddressSpaceMap`, `nvMapCreate`, memset) shows in only 21 samples, so it does not explain the waits.
- In Mesa 20.1, `CompressedTexSubImage3D` is always synchronous under glthread, even from a PBO.

**Other figures:**
- Heap: 346 MB free at the lowest (run 80: 335).
- FPS: hub 45.8, `Gungan_A` 26.7, `Gungan_B` 31.1, `Rescue_A` 27.7, `Rescue_b` 34.5, `PodSprint_A` 31.7 (time in each level, loading included).

## Log 80: `0.0.86-log80`, where uploads wait

Diagnostics only. Behaviour is log79's.

- `lsw_glthread_sync_hook` is now always set with glthread on. It counts the render thread's waits for Mesa's GL thread.
  - `LSWTCS_GLSYNCLOG` still adds the per-call `[GLSYNC]` report.
- **`[TEXSLOW]` and `[TEXCOST]`:** the part of each upload spent waiting for the GL thread. The rest is the driver's own work on the render thread.
- **`[GLSTALL]`:** that replay's total wait for the GL thread, and the waits of 5 ms or more by GL call.
- **`[GLSYNCLONG]`:** any single wait of 200 ms or more, with its call and replay (first 100). It covers waits outside the replay, such as `arena_fence`'s `glFenceSync`.
- **`[BOPOOL]`:** the first freed storage kept, and the first reuse.
- The boot line now says `(normal build)` instead of `(log77 A)`.
- **Test:** handheld, the same route. Read:
  - in `[TEXSLOW]`, whether the 13-41 ms uploads are GL-thread waits or driver time;
  - which calls `[GLSTALL]` lists;
  - `[GLSYNCLONG]` against any long freeze.


## Run 83: `0.0.86-log80` handheld

305 s on run 80's route: the hub, `Gungan_A`, `Gungan_B`, the hub, `Rescue_A`, `Rescue_b`, the hub, `PodSprint_A`, the hub. No crash.

**Where uploads wait:**
- New textures' uploads took 3002 ms over 1655 textures, 1.81 ms each (run 82: 2.20).
- 2636 ms of that (88 %) was inside glthread's finish. In Mesa 20.1, finish waits for the GL thread's queued batches, then runs the batch not yet queued on the calling (render) thread.
- In `[GLSTALL]`, few single waits reach 5 ms. Most of the time is many short waits, about 1.5-2 ms per texture.
- Render-thread profile:
  - 356 samples inside `nouveau_bo_new`, which is the texture's storage (`TexStorage3D`) made there by finish;
  - 74 % of those samples are calls to the GPU service: `nvMapCreate` (Create 27.5 %, Alloc 11.5 %, GetId 8.1 %) and `nvAddressSpaceMap` (27 %);
  - about 0.8 ms per new texture.
- The pool kept nothing until replay 8781 (`Rescue_A`), when the texture cache first evicted. Until then every storage was new.
  - At the end: 253 reused, 1546 new, 423 kept, 787 freed.
- Two long single waits:
  - `GenTextures` 311 ms in replay 2958, the frame after the hub arrival;
  - `ClientWaitSync` 269 ms at the `PodSprint_A` entry.

**A 3.6 s freeze at the start of `Gungan_B`** (replay 4142, of which 3604 ms in its resolve stage; the wait for the GL thread was 1 ms):
- In over-1-s freezes, Mesa's GL thread was 88 % in `nvGpuChannelKickoff` (`KickoffPb`, the GPU submit). The render thread waited for room in glthread's queue (`util_queue_add_job`).
- The `[CLOCK]` line next to it read `gpu_load=0.0%`: the GPU was not busy. The submit itself stopped.
- Same pattern as run 82's 1.9 s freeze. Both came before the pool kept anything.

**`PodSprint_A` entry:** 2148 ms.
- 1314 ms of it was a program built on the render thread: one the game had not asked for in any earlier session.
- The rest went to its 259 new textures.

**Other figures:**
- Decode: 4675 ms of work in 921 ms on the render thread.
- Heap: at least 316 MB free.
- FPS (time in each level, loading included):
  - hub 44.5;
  - `Gungan_A` 37.0, `Gungan_B` 31.8;
  - `Rescue_A` 28.3, `Rescue_b` 37.1;
  - `PodSprint_A` 30.6.

## Log 81: `0.0.87-log81`

- **Storage made ahead** (`bo_check.cpp`):
  - Each storage kind (size, alignment, flags, memory kind, tile mode) remembers the most made in one replay, up to 16, over sessions (`bopool.bin`, written at each level change).
  - Light replays are under 12 ms, with no new texture and no program wait. Every second one queues one buffer to make on Mesa's GL thread (`lsw_glthread_call`), smallest kinds first, until the pool holds that many of each.
  - Limits: 48 MB in all (the pool's cap, was 32), and only while the heap has 384 MB free.
  - `LSWTCS_BOPOOLFILL=0` turns it off.
  - Logs: `[BOPOOL] N storage kinds remembered` at boot, `first texture storage made ahead`, and `made ahead` in `[HEAPGL]`.
- **GPU submit times** (`--wrap=nvGpuChannelKickoff`):
  - `[HEAPGL]` adds the submit count, their total ms, how many took over 16 ms, and the longest.
  - A submit of 100 ms or more prints `[GPUSUBMIT]` with the channel's error notification and error info. These tell whether the GPU faulted.
- **Test:** handheld, the same route.
  - The first session learns from its first area entries and fills the pool in later light frames.
  - A second session starts with the file.
  - Read: `made ahead` against `reused` and `new`, upload per texture and its waiting share in `[TEXCOST]`, and `[GPUSUBMIT]` at any freeze.
- **Risk:** up to 48 MB held. Each buffer made ahead costs the GL thread about 1 ms in a light frame.


## Run 84: `0.0.87-log81` handheld

298 s on run 80's route. No crash. The tester heard audio that was choppy and blown out in heavy areas at high volume, and a pop at the start of each opening crawl.

**Storage made ahead:**
- 578 buffers made ahead, 609 reuses against 1204 new storages.
- No gain: upload per new texture 2.23 ms (run 83: 1.81), 77 % of it in glthread's finish.
- Render-thread samples in `nouveau_bo_new` rose from 356 to 461.
- The pool, full of made-ahead buffers (47 MB), freed 1083 evicted buffers it could have kept.
- Area entries are within noise of run 83: hub returns 284 and 362 ms, `Rescue_A` 296 ms, `PodSprint_A` 925 then 485 ms.
- Log82 turns it off by default.

**GPU submits:**
- 35,546 in the session, 11.2 s in all (0.31 ms each). 16 took over 16 ms; the longest took 415 ms. Six of the long ones came during the boot.
- `[GPUSUBMIT]` could not read the channel's error state: the notification call returned 0xEA01 and the info call 0x275C. Whether the GPU faulted is still unknown.
- `PodSprint_A`'s next-frame stall fits a submit: a 410 ms upload with only 47 ms of GL-thread wait, next to a 397 ms kickoff.

**Audio, from the log:**
- **Clipping.**
  - The stereo mix's peak exceeded full scale in 6 of 28 ten-second windows (p90 1.96, max 3.27), in loud moments: the first level entry and `Gungan_B`'s heavy part.
  - `fill_slot` sums the 5.1 channels (FL + 0.71 C + 0.5 LFE + 0.71 BL, up to 2.9x) and clamps at ±1. That is a hard clip.
  - The crawl theme opens on a loud chord, which matches the pop the tester heard.
- **Underruns.**
  - In `Gungan_B`'s heavy part (25 FPS), 20-160 ms of each second was silence: 90-130 times per 10 s the 6 device buffers (32 ms) ran dry.
  - The game's frames came up to 45 ms apart. They come from guest code, which waits on the main thread in heavy frames.
- Only 10 A/V holds, all early in the session, so the holds are not the source of the pops.

## Log 82: `0.0.88-log82`, audio

- **Look-ahead limiter on the stereo mix** (`audio_switch.cpp`, `LSWTCS_AUDIOLIMIT=0` = the old clamp).
  - Each mixed block waits one block (5.33 ms). The block before it is output with its gain ramped to what the waiting block needs (ceiling 0.97).
  - The gain recovers by 3 % a block, about 0.2 s from -10 dB.
  - When the device is about to run dry, or an A/V hold starts, the waiting block fades to silence, and the block after the gap fades in.
  - A PC simulation (a silent start, then a peak-3.0 tone, a 4-block gap, then a quiet tone) gave no clipped samples, a maximum of 0.97, and no gain steps.
  - `[AUDIO]` adds the limiter's lowest gain, the blocks turned down, and the fades at gaps.
- **12 device buffers** (64 ms; was 6, 32 ms). `LSWTCS_AUDIOBUFS` sets 4-16. This adds about 32 ms of audio latency.
- **Storage made ahead off by default** (`LSWTCS_BOPOOLFILL=1` turns it on). The demand is still learned and saved.
- **Test:** handheld, `Gungan_B`'s heavy part and an opening crawl, at high volume. Ask whether the audio still crackles or pops, and whether lip sync in cutscenes feels late. Read `[AUDIO]` limiter lines and `[AUDIOQ]` empties against run 84.


## Run 85 (log82, handheld, 2026-11-06): the title-idle movie freezes on a black screen

**Report:** the tester left the title screen idle. The attract movie (`DEMOINTRO.WMV`, see ISSUES.md #3) started, the screen went black and the game froze. The author says this was a known freeze in his last build. Session: about 200 s, title screen and menu only, no level.

**From the log:**
- The last replay is 2277, about 49 s in. It is the movie's first frame: `[ONETILE]` 1280x720, two new shaders, one 1280x720 and two 640x360 single-channel textures (the decoder's Y/U/V planes). No swap or replay follows.
- `[SCHEDPROF]`: the main thread leaves the rotation. The movie's decoder thread (entry `82826FC8`) and a second movie thread (`82826FF8`) run until about 110 s, while the movie's sound plays (`[AUDIO]` peak 0.34-0.36). Then the sound stops (peak 0) and a third movie thread (`82827028`) holds the turn 96 % of the time: in `sub_82829000` it polls `KeWaitForSingleObject` with a timeout and calls `KeSetEvent` without end.
- `hwprof.txt`: the main thread waits on `KeWaitForSingleObject` (null timeout) in `sub_82828418` < `sub_823BD3E0` < `sub_82318280` for the rest of the session. `[LONGWAIT]` prints only when a wait ends, so it never printed.
- The runtime already names this hang (kernel_stubs.cpp, `LSWTCS_IBINT`): XMedia retires its three video surfaces from a GPU interrupt inside the movie's draw IBs (callback `0x82815910`). If that interrupt is dropped, the queue fills and the main thread waits for ever in `sub_82828418` with the movie workers asleep.
- **Cause in this build:** state I (`MAPXSHNLUBIkhwregdq`, with k) walks the ring on the walker thread, and `pm4_fire_interrupt` drops every interrupt met there (`if (t_async_walker) { ++g_aw_isr_dropped; return; }`), because that thread has no guest context. In-game walks carry no interrupts (run 42; `[ASYNCWALK]` "interrupts dropped 0" in every report of this run), so this never showed in play. With k, every kick of the movie goes to the walker, so each surface-retire interrupt was dropped after the first frame.
- Not proven: `[ASYNCWALK]` prints only on replays, so no line shows the drops during the movie. The author's freeze (no state I in his build) may have another cause; ISSUES.md #3 also lists corrupt movie pixels and a hang at playback end.

## Log 83: `0.0.89-log83`, GPU interrupts from the walker thread

- `pm4_fire_interrupt` on the walker thread (state I) no longer drops the interrupt. It hands it over (`aw_isr_hand`) and waits, up to 100 ms, for a guest thread to run it. The interrupt's record is in guest memory the rest of the ring may overwrite, so the walker does not read on meanwhile. It gives up the turn while it waits.
- A guest thread that holds the turn runs it with its own live context (`aw_isr_service`), as VdSwap's walk did before state I:
  - at the start of `KeWaitForSingleObject` (each 16 ms slice of an infinite wait, where the movie's main thread sits), `KeWaitForMultipleObjects`, `NtWaitForSingleObjectEx` and `KeDelayExecutionThread`;
  - inside the loops that wait for the walker: `async_walk_join` (fence, ring, phase and VdSwap sites), `async_walk_try` and `ring_window_wait`.
- If nobody takes it within 100 ms it is dropped as before and counted.
- `LSWTCS_AWISR=0` restores the old dropping.
- New log lines: `[AWISR] interrupt #N ... served by guest thread` (first 5), and every 5 s, from the XMA thread so it prints without replays: `[AWISR] 5 s: interrupts met by the walker, served, timed out, walker wait avg/max, dropped since boot`.
- Gameplay should not change: no interrupts were met by the walker in play.
- Risk: the interrupt now runs on whichever guest thread waits, for example a movie worker. On the 360 it also preempts any thread. If movie playback now continues, the next known faults are ISSUES.md #3's: corrupt pixels, and the hang at playback end (thread `82827028`'s loop may be that hang, or a consequence of the full queue).
- **Test:** handheld. Leave the title screen idle until the movie plays, and let it run to the end. Then press a button and start a level. Ask whether the movie shows a picture, whether the picture is correct, and whether the game returns to the title or menu at the end.


## Run 86 (log83, handheld, 2026-11-06): the movie plays, as garbage, at 3 frames a second

**Report:** the title-idle movie now went past its first frame, but the picture was "all bugged" and the tester thinks it froze near the end. Capture `shot_2373.png`: the whole screen is green and magenta blocks with some structure. Session: about 111 s, title screen and movie only.

**From the log:**
- **log83 worked as meant.** `[AWISR]`: 27-54 interrupts per 5 s met by the walker, all served by a waiting guest thread (the XAudio thread, the main thread, the NuSound workers), none timed out, none dropped. The walker waited 2-10 ms on average, 41 ms at most.
- **No hard freeze.** Frames kept coming until the session ended (`frames.log` to replay 2461), but at about 3 a second: from 45 s on, 12-18 frames per 5 s, 260-360 ms each. The movie's video thread (`82826FF8`) held the guest turn 70-83 % of the time and ran 256-309 ms of guest code per frame (`[GSPIKE]`). At that rate the one-minute movie would take about ten. The movie's sound played, with gaps (`[AUDIOQ]` up to 997 ms empty per 10 s).
- **Why the picture is wrong:** XenonRecomp has no case for some VMX operations. It writes the assembly comment and no code, so the operation does nothing. The whole generated output has about 8,600 such empty operations in 954 functions: mostly `vslh`, `vsrah`, `vsubshs` and `vspltish`, plus conditional traps, which are harmless when empty.
  - The author's `patch_tcs360_movie_vmx.py` filled the twelve WMV functions that were known.
  - Run 86's profile and the direct-call closure of the three movie thread entries reach 826 functions. 62 of them still have about 1,650 empty operations. They include:
    - `8288B6D0`: `vctuxs`, `vpkswus` and `vpkuhus`, a float to 8-bit conversion, likely the colour output;
    - the `82D9xxxx` and `82DAxxxx` family, likely motion compensation;
    - `8291A558`, with 110;
    - `82866770`: `vpkswss`, the sound's float to 16-bit output.
  - About 100 more functions in the same ranges, mostly in `82DAxxxx`, have the same sizes and gaps. They are likely variants picked through function pointers.
- **Range check:** no game thread outside the movie samples code in `0x8282`-`0x8299` or `0x82D9`-`0x82DF`. This holds in run 84 (gameplay) and in this run's menu part. Game threads only touch system calls in `0x8281xxxx` and `0x829Bxxxx`, which have none of these gaps.

## Log 84: `0.0.90-log84`, empty VMX operations filled in the movie code

- `workspace-tools/fill_tcs360_movie_ops.py` fills every empty operation of 26 kinds in two sets of functions:
  - the movie libraries' ranges, `0x82814000`-`0x829A0000` and `0x82D90000`-`0x82E00000`;
  - two movie functions outside them (`82E043C0`, `82E0B540`).
- The 26 kinds: VMX `vslh vsrh vsrah vrlh vsrab vsubshs vaddshs vsubsbs vaddsbs vsububm vsubuhs vmaxsh vminsh vmaxuh vminuh vavguh vcmpgtsh vcmpequh vcmpgtsw vandc vaddsws vpkswss vpkshss vpkswus vpkuhus vspltish vctuxs`, the record forms of the compares, and `eqv` and `mulhd`.
- Result: 5,153 operations in 164 functions, in 15 generated files. Conditional traps stay empty.
- Each inserted line ends in `// fill`, and `--revert` removes them. After a revert, `tools/generated_output.py check target` passes again; this was checked. While the fill is applied, that check reports those 15 files as changed.
- **Register order.** Vector registers hold their bytes reversed, so element-wise operations work lane by lane. Two-source packs take their sources in the opposite order, as the generator's own `vpkshus` does.
- **Non-volatile registers.** A non-volatile register that only omitted operations used becomes a fresh local, as in the author's `v14` case.
- **Check on the PC:** `tests/fill_ops_check.py` builds the exact emitted lines with clang. It compares them with a reference written in PowerPC element order on 485 inputs (random values, edge values, and floats with NaN, denormals and out-of-range values), with the destination both separate and equal to a source, plus the compares' CR6 bits. Result: 60 cases, all equal.
- **Not done:** speed.
  - The video thread's samples, by guest instruction: about 20 % in VMX arithmetic (`vaddshs`, `vsrah`, `vsubshs`, `vslh`, `vcmpgtsh`), about 13 % in byte loops (`lbz`/`stb` in `828C38C0`/`828C2F60`), and 6 % waiting. No single hot spot.
  - A tenfold gap is not something a fill or a micro-optimisation closes. It needs a native decoder or a much faster recompiled path.
  - The backlog says "no decoder install authorized", so that is a decision for the author and the tester.
- **Test:** handheld. Leave the title screen idle until the movie plays. Check whether the picture is now correct, even though slow, and whether the sound is clean. Then press a button and start a level. The movie will still be slow (about 3 frames a second, unless the garbage itself cost time).


## Run 87 (log84, handheld, 2026-11-06): shapes in the movie, still mostly broken; skipping ends the process

**Report:** "something seems to appear, but not completely". Captures `shot_2374` and `shot_2400` show shapes, including a LEGO figure in grey, over a mostly dark green screen with scattered blocks and horizontal streaks. Skipping the movie still ends the game.

**From the log:**
- The fill changed the picture, from noise everywhere to recognisable shapes. Dark green is what Y = U = V = 0 gives, so much of each displayed frame is zero in all three planes. The history captures (`hist_2374`) show the luma mostly black with decoded pieces.
- **Timing.** On the 360 an interrupt in the command stream fires after the GPU has executed the commands before it, so XMedia frees a video surface only after the draw that sampled it. Here, since log83, the interrupt runs when the walker reads it. The draw reads the surface later: the render thread decodes textures from guest memory at replay (`get_texture` / `texture_prepass`, `phys = g_base + 0x80000000`). In between, the decoder may already be clearing the surface (`dcbzl`) and decoding the next frame into it. This also fits the author's "moving corrupt pixels": without state I, the interrupt runs in VdSwap's walk, also before the replay.
  - Not proven: remaining decode errors could also cause it. All other VMX operations the movie code uses were reviewed against PowerPC semantics with the reversed register layout, and look right: `vperm`, the merges, the unpacks, `vsldoi`, the splats, the unsigned compares, `lvlx`/`lvrx`/`stvlx`/`stvrx`/`stvewx`, and `dcbzl`.
- **The skip.** The log ends mid-line at about 105 s, with no `[CRASH]`, `abort()`, `exit()` or throw line, and no `crash.log`. Each of those handlers prints and drains the log first, so the process was ended from outside them, or the system was. Run 86 ended the same way. The candidates:
  - a system fatal, for example `fatalThrow` in Mesa's EGL Switch driver when `nwindowDequeueBuffer`/`QueueBuffer` fail;
  - a kernel or GPU fault;
  - the tester closing a hung game.
  - It is not yet known which.

## Log 85: `0.0.91-log85`, interrupts wait for the replay; skip diagnostics

- **Deferred interrupts** (kernel_stubs.cpp, `aw_isr_defer`, `aw_defer_serve`; `LSWTCS_AWDEFER=0` restores log83's hand-over).
  - An interrupt the walker meets is queued instead of handed over, except the tiling callback `0x822D2BA0`.
  - Each queued interrupt keeps its own `SCRATCH_REG0-7`, which carry the callback and its argument, and the serial of the frame being recorded.
  - The walker reads on.
  - A guest thread at a wait stub or walker wait runs it once `lsw_replay_textures_read`, called by gl_backend.cpp after each replay's draw loop, reports that frame or a newer one. First it writes the saved registers back into the scratch block, because later commands may have changed them.
  - After 500 ms it runs anyway, and is counted as late.
  - Log lines: `[AWDEFER]` for the first 5 runs, plus a 5-second line: queued, run, late, wait avg/max, pending, frames published, and textures read.
- **Skip diagnostics:**
  - `[PAD]`: the first 60 button presses.
  - `[GTHREAD]`: every guest thread start and exit, up to 300.
  - Both go to the SD card at once (`lsw_log_async_drain`), so the next log shows how far a skip gets before the process ends.
- **Test:** handheld. Let the movie play, and check whether the picture is now whole, even if slow. Then press a button to skip it. If the game ends, note what the screen shows (an error code? a return to the HOME menu?). Bring the newest file in `atmosphere/crash_reports` and `atmosphere/fatal_reports` from the SD card, if there is one.


## Run 88 (log85, handheld, 2026-11-06): black screen again, the movie's sound plays, skipping does nothing

**Report:** the screen stayed black while the movie's music played. The skip button did nothing. There was no crash this time.

**From the log:**
- `[AWDEFER]`: only two interrupts were queued in the whole session.
  - The first ran after 133 ms.
  - The second ran late, after 502 ms: "frames published 2296, textures read to 2295". Frame 2296 was published but never replayed.
  - No third interrupt came. The decoder thread (`82826FF8`) then waited for a surface for the rest of the session (`sub_82827918`, 98 % of its samples), as in run 85. From about 115 s, the third movie thread (`82827028`) held 96 % of the turn.
- **Circular wait.** A frame is published when its swap packet is read: by the walker, or by the next VdSwap's walk. The render thread only replays when the game's next VdSwap posts a frame (`gpu_d3d12_present`). So the movie's main thread waited for a decoded frame, the decoder waited for a free surface, the surface's interrupt waited for the replay, and the replay waited for the next VdSwap. Only the 500 ms cap broke the cycle.
- **Why it stopped for good.** log85 wrote all mirrored scratch registers (0, 1, 4 and 5) back before each queued interrupt. The D3D ISR (`sub_822ADA00`) reads only register 0 (CPUs still to acknowledge), 4 (callback) and 5 (argument). Register 1 is written by the VBlank path (`sub_822B7898`). A 500 ms-old value there is the likely reason nothing moved after the second interrupt. This is inferred from the code, not traced.
- **The skip.** No `[PAD]` line in the whole session. With the main thread stuck waiting for the movie, nothing polls the pad, so a press cannot reach the game. The music played to the end (`[AUDIO]` peak 0.33-0.36, then 0 from about 110 s).

## Log 86: `0.0.92-log86`

- **Only registers 0, 4 and 5** are written back before a queued interrupt runs.
- **Replay on request.** When a frame is published while interrupts are queued, kernel_stubs.cpp calls `lsw_gpu_replay_request` (gl_backend.cpp). It posts one more frame to the render thread without a guest swap, so the render thread replays what was just published and reports its textures as read. The game's next present waits for that replay like any other (`LSWTCS_RTSYNC`).
- `[AWDEFER]` adds a count of replays asked.
- **Test:** the same as log85. Let the movie play, check whether the picture is whole (even if slow), then press a button and say what happens.


## Run 89 (log86, handheld, 2026-11-06): three movie frames, then everything waits

**Report:** the video is still broken, and the skip button did nothing. The session lasted about 70 s.

**From the log:**
- log86's two fixes worked as meant. `[AWDEFER]`: 4 interrupts, all run after their frame's replay (46-119 ms), none late. "frames published 2247, textures read to 2247, replays asked 3".
- Three movie frames were drawn (replays 2246, 2248 and 2250, each with new Y/U/V textures). After that:
  - the main thread waits in `sub_82828418`;
  - the decoder (`82826FF8`) waits in `sub_82827918`;
  - the control thread (`82827028`) waits in `sub_82829000`, all on `KeWaitForSingleObject`;
  - only the demux/sound thread (`82826FC8`) runs.
  - In runs 86-87 (log83/84) the movie kept going, with about two interrupts per frame. Only the timing of the interrupts differs.
- There is still no `[PAD]` line: nothing polls the pad while the main thread waits.
- **D3D callback packets** (`sub_822AE760`), in order:
  1. SCRATCH_REG4/5 get the callback and its argument;
  2. SCRATCH_REG0 gets the CPU mask;
  3. WAIT_REG_MEM waits until that mask is in memory;
  4. INTERRUPT;
  5. WAIT_REG_MEM waits until the ISR has cleared SCRATCH_REG0;
  6. SCRATCH_REG4 gets the `0x0BADF00D` poison.
  - The walker skips WAIT_REG_MEM by default (`LSWTCS_WAITREG`, off since 2026-10-05), so it never waits for the ISR. log86's write-back of registers 0, 4 and 5 is consistent with this.
- **A defect that fits a timing-dependent stall.** `KeWaitForSingleObject` only polls the event's SignalState (in 16 ms slices, `kw_block` checking every 1 ms). So a `KeSetEvent` followed by a `KeResetEvent` before the waiter's next look never releases it, and `KePulseEvent` (set, then clear at once) never releases anyone. The 360 kernel releases the threads waiting at the moment of the set. The movie's eight events per object are waited on, set and reset through wrappers at `82827150`-`828272D8`; four of them have reset wrappers. Not proven to be the cause here.

## Log 87: `0.0.93-log87`, kernel wake-ups; long-wait report

- **Wake-ups** (kernel_stubs.cpp, `EvWake`, `kewake_set`; `LSWTCS_KEWAKE=0` restores the old polling).
  - A `KeWaitForSingleObject` on an event (type 0 or 1) registers as a waiter.
  - A `KeSetEvent` or `KePulseEvent` while waiters exist bumps the event's generation (type 0), or adds one release token (type 1, at most one per waiter).
  - A waiter whose event is clear is released if the generation moved, or if it takes a token. Taking a token also clears the event, so one set never releases two threads.
  - This affects every event wait in the game, not just the movie.
  - `[KEWAKE]` counts, every 5 s, the waits released by a set they did not see. A non-zero count shows that lost wake-ups were happening.
- **`[KWAITLONG]`.** An infinite `KeWaitForSingleObject` past 3 s (then every 30 s; up to 60 lines) prints:
  - the thread and its entry;
  - the object, with its type and state;
  - the caller's lr;
  - waiters, tokens, and the sets seen while waiting.
  - If the movie stops again, this names the event each thread waits on.
- `KeWaitForMultipleObjects` keeps the old polling.
- **Test:** handheld. Let the movie play, say whether the picture is whole and whether it keeps going, then press a button to skip. Then start a level and play a little: the wake-up change also affects gameplay.


## Run 90 (log87, handheld, 2026-11-06): the same stop after three frames; it is a wait, not a lost wake-up

**Report:** nothing changed.

**From the log:**
- `[AWDEFER]` is as in run 89: 4 interrupts, all run after their replay. There is no `[KEWAKE]` line, so no wait was released by a set it had missed. log87's wake-up change did not act.
- `[KWAITLONG]` names the stop. Both events belong to one player object (`84ACF490`), and nothing set either of them while it was waited on:
  - the decoder (`82826FF8`) waits on `84ACF4F4`, a synchronization event, from lr `82827B0C`;
  - the main thread waits on `84ACF514`, a synchronization event, from lr `8282894C`.
- **The player's vtable** (read from the local game image only, nothing shared) starts with the method `8281B590`.
  - The three thread loops: `+140` (`82827630`, demux/sound), `+144` (`82827918`, video decoder), `+148` (`82829000`, control).
  - `+76` is `82828418`, the main thread's play loop.
  - Event methods: wait on events 0-7 at `+164`-`+192`, set events 0-7 at `+196`-`+224`, reset events 4-7 at `+228`-`+240`.
  - The decoder waits on event 1, its permission to decode. The main thread waits on event 3, a decoded frame.
  - The decoder sets event 3 (`82827918` calls `+208`). The main loop sets event 1 at one place, `82828DF0`.
  - So it is a deadlock between them. It shows only since log85 moved the interrupts to after the replay. With log83/84's timing the movie played on. The player seems not to tolerate a surface retired that late; this is inferred, not traced.
- The movie's interrupt callback `82815910` calls method `+92` of its argument (another object), not the player's.

## Log 88: `0.0.94-log88`, texture copies at the interrupt; log83's timing back

- **Texture copies** (kernel_stubs.cpp, `tex_snapshot_at_interrupt`; `LSWTCS_TEXSNAP=0` turns it off).
  - At each GPU interrupt, except the tiling callback, every texture sampled by the frames not yet replayed has its bytes copied. Those frames are the one being recorded and the published one not yet taken.
  - The bytes copied are exactly those the decoder and the cache key read: `lsw_gpu_texture_extent` (new, in `lsw_gpu_texture.cc`) visits every block of the planned layout and adds the key's content probe.
  - The render thread decodes a texture from its copy when the fetch constant matches and the copy covers the frame (`tex_source` in gl_backend.cpp, for `get_texture` and the texture pre-pass). It uses the oldest copy taken at or after that frame.
  - Copies are dropped once their frames are replayed, with at most 96 kept.
  - Logged as `[TEXSNAP]`: the first 5 interrupts, and a 5-second line with bytes copied and textures decoded from a copy.
  - In play the walker meets no interrupts, so this costs nothing there.
- **Interrupts run as in log83 again.** They are handed to a waiting guest thread at once. `LSWTCS_AWDEFER=1` brings back log85/86's queue.
- **`LSWTCS_KEWAKE` is now off by default** (`=1` turns it on). It released nothing in run 90, and it changes every event wait in the game. `[KWAITLONG]` stays.
- **Test:** handheld. Let the movie play: is the picture whole now (still slow)? Then press a button to skip, and then start a level.

## Logs 89 and 90: 2 MB guest pages; a launch switch and an A/B control

Described in CPU_ROADMAP.md, "torchlight-nx study" (log89: guest memory window and backings 2 MB aligned, `LSWTCS_GUEST2MB=0` for the old layout) and "renut-nx, katamari-nx and mkdd-nx study" (log90: L held at start for 4 KB pages; the control state `MAPXSHNLUBIkhwregdq0`). log89 was never run on its own.

## Run 91 (log90, handheld, 2026-11-06): 2 MB pages in place, no measurable change; the control state sizes the noise

One launch: the hub, `Gungan_A`, `Gungan_B`'s heavy part, the hub (211 s). No crash. The movie was not played.

**The L switch could not be used.** The tester launches from sphaira, not from the Home menu, so L cannot be held at start. log91 replaces it with a second NRO (below).

**From the log:**
- The 2 MB layout was in place: the window at `0x772600000`, and the 16 MB, 512 MB and 64 MB ranges with 2 MB-aligned backings.
- Handheld is GPU-bound: about 38 ms of GPU per heavy frame. The heavy part ran at 25.7 FPS (run 79: 25.7).
- **CPU per guest call** (`guest_call_cost.py`): 158 ns (90 %: 157-159), against 155-163 ns in runs 72-84. No gain shows. Either the kernel does not use 2 MB blocks for these mappings, or TLB misses were not a large part of the cost. log91 measures which.
- **The control:** the two A/B states are identical. `MAPXSHNLUBIkhwregdq0` against `MAPXSHNLUBIkhwregdq` came out at -4.3 % (90 %: -12.2 to +4.7) over 8 against 9 visits. So one short session cannot resolve effects under about 10 %, as the memory note on noise says. This is a measured noise floor, not an estimate.
- Free memory at the end: 672 MB.

**Main-thread profile, heavy frames** (`hwprof_report.py`; 33,889 samples, 79 % in generated guest code):
- The top functions are `sub_8231ED30` 5.1 %, `sub_82320328` 3.6 %, `sub_8249DB20` 2.7 %, `sub_822BC218` 2.6 %, `sub_822B5878` 2.5 %, then several at 2.0 %. Several pass katamari-nx's 2 % floor.
- **Their sampled instructions** (scratchpad `hot_insns.py`, a sample usually lands after the slow instruction) show two costs that come from the generator, not from these functions:
  - **Float compares.** The CR compare is written `un = isnan(l) || isnan(r); lt = !un && l < r; ...`, which GCC compiles to `fcmp`, `b.vs`, then a signalling `fcmpe` on the same registers. In `sub_8231ED30` and `sub_8249DB20`, 12-16 % of their samples are on those `fcmpe`.
  - **The region table.** Every guest access is `lsr #29`, a load from `ppc_view_base`, then the access itself: two dependent loads on the A57's single load pipe.
- **Over all generated code** (scratchpad `table_cost.py`): 16.6 % of the main thread's guest-code samples sit on the access right after a table load, and 3.6 % one instruction later. Table loads are 3.9 % of the guest instructions (1,250,471 of 32,188,417). Float compares: 3.0 % of the samples.

## Log 91: `0.0.97-log91`, A and B builds; a boot TLB measurement

Two builds, **one NRO on the SD**. The tester asked for a single test rather than three launches, so the SD's `switch/lswtcs.nro` is B, and A stays as a fallback (`private/lswtcs-log91.elf`). A's only change is exact and is inside B, and the boot measurement below answers the 4 KB question within one launch.

| Build | What differs |
| --- | --- |
| A (`build-mesa`) | 2 MB guest pages, quiet float compares |
| B (`build-fold`, on the SD) | A plus guest addresses without the region table |
| A named `*4k*.nro` | 4 KB layout (kept in the code, not staged) |

- **Quiet float compares (A and B).** `workspace-tools/tune_tcs360_context.py` rewrites `PPCCRRegister::compare(double, double)` in the generated `ppc_context.h` with `__builtin_isunordered`, `isless`, `isgreater` and `==`: one `fcmp` and four `cset`. The four CR bits are the same for every input. `tests/cr_compare_check.py` checks 20,001,600 pairs (special values exhaustively, the rest random) at -O0 and -O2: 0 differences. The host's Invalid flag on quiet NaNs is the only difference, and the guest FPSCR does not read it. It applies to all game code (11,660 compare sites), so it is in both builds. Like the movie-op fill, it must stay applied (`--check`, `--revert`).
- **Fast address fold (B only).** `tune_tcs360_context.py --fold-fast` on a copy of the output (`output_fold`, build folder `build-fold`):
  - `PPC_HOST(x)` = `base + (bit 31 set ? x & 0x9FFFFFFF : x)`, which compiles to `adds`, `and`, `csel` and no table load.
  - Same as the table except in region 0xE0000000, which this also folds onto 0x80000000. That region is mapped only with the heap bypass on (`lsw_guest_layout.h`, "small-alloc pool"). With `LSWTCS_HEAPBYPASS=0` (env.txt), any access there faults today, so no running guest code reaches it.
  - main.cpp refuses to start B with the bypass on (`[INIT] this build (fast address fold) cannot run ...`). B prints `[INIT] guest addresses: fast fold, no region table (log91 B)`.
- **The 4k NRO.** The file name is the launcher's argv[0]. Boot prints `[NX] launched as "...": guest page layout: 4 KB ("4k" in the file name)`.
- **`[TLBBENCH]` at boot** (all three; `LSWTCS_TLBBENCH=0` off; about 1 s). Two 32 MB code-memory aliases of 2 MB-aligned backings, mapped like the guest window: one at a 2 MB-aligned address, one 64 KB off, where block descriptors are impossible. Then a chain of 400,000 dependent loads, each on a random 4 KB page; best of three, alternating. Then the same chain over the guest's 512 MB physical range.
  - A clear gap between the aliases means the kernel maps aligned aliases with 2 MB blocks. No gap means log89's change cannot act.
  - The guest range's figure, read against the two aliases, shows the same for the real window.
- **Test: one launch, docked if possible** (FPS shows CPU changes only there): the hub, `Gungan_B`'s heavy part for 3-4 minutes, back to the hub.
  - **The yardstick is history, not a second launch.** ns per guest call does not depend on the mode, since the CPU clock is the same: run 91 158 (handheld), run 71 162 (docked), runs 72-84 155-163 (cross-session spread about 3 %). Docked FPS: run 71's heavy part 30.0.
  - **Reading:** B at or under about 150 ns is a gain beyond the spread. Between 152 and 163 is no measurable change. Over that, B is worse, and A goes on the SD.
  - The size is not predicted: samples on an instruction show where time waits, not how much an exact replacement saves.

## Run 92 (provisional log91, handheld, 2026-11-06): 2 MB pages are not a lever; B's build fixed before staging

The tester ran the NRO that was on the SD before B was staged: log90's code plus the argv layout switch and `[TLBBENCH]`, built at 15:43, without the quiet compares. Its ELF was not kept: `build-mesa` was rebuilt as A before the run came back. The run is in `private/runs/run92-log91pre`; do not read its `hwprof.txt` with another build's ELF. One launch, the hub, `Gungan_A`, `Gungan_B`'s heavy part, the hub (264 s). No crash.

- **`[TLBBENCH]`:** 2 MB-aligned alias 194.0 ns per dependent load, 64 KB-offset alias 203.0 (+4.6 %), guest 512 MB range 203.8.
  - So at most about 9 ns is saved, on a load that already misses every cache (about 200 ns), and nothing on cache hits. That fits runs 91 and 92 showing no change per guest call.
  - The page walks of the 4 KB alias likely hit the L2 cache, which is why the gap is small. Whether the real window gets blocks stays open: its figure matches the offset alias, but it spans 16 times more memory.
  - **Verdict:** log89's layout stays (about 6 MB, harmless), but it is not worth more work.
- **CPU per guest call:** 158 ns (90 %: 156-159), the same as run 91. Two sessions now give the baseline for B.
- **Control state:** -2.8 % (90 %: -11.0 to +3.8) over 10 against 10 visits; run 91 gave -4.3 %. The in-run A/B noise is about ±7 %.
- **B's build, corrected before staging.** The runtime files include `output/ppc_context.h` by a fixed path, not through `RECOMP_OUTPUT`. In B the game code therefore uses the fast fold while the runtime keeps the table; both give the same address everywhere except the unmapped 0xE0000000. But main.cpp's guard and B's line were not compiled in. CMakeLists.txt now defines `LSW_ADDR_FOLD_FAST=1` for the runtime when `RECOMP_OUTPUT`'s header has the fold. The staged B (`c4dce967...`) prints `[INIT] guest addresses: fast fold, no region table (log91 B)`.
- **Static check of the ELFs** (scratchpad `static_count.py`, words in `.text`):

  | | log90 | A | B |
  | --- | --- | --- | --- |
  | region-table loads (`lsr #29` then `ldr [.., lsl #3]`) | 461,018 | 460,877 | 959 |
  | `fcmpe` | 22,657 | 12,825 | 12,808 |

  The remaining ones are in the runtime and libraries.
- **Next test:** B, one launch, as in "Log 91". Compare against 158 ns.

## Run 93 (log91 B, handheld, 2026-11-06): -3.8 % CPU per guest call; the table and the double compares are gone from the profile

One launch of B (`[INIT] guest addresses: fast fold, no region table (log91 B)`), the same route as runs 91 and 92 (the hub, `Gungan_A`, `Gungan_B`'s heavy part, the hub; 269 s). No crash, nothing reported wrong. In `private/runs/run93-log91b` with its ELF.

- **CPU per guest call: 152 ns (90 %: 152-152)**, against 158 (157-159) and 158 (156-159) in runs 91 and 92, the same route on the same day. That is -3.8 %. It is also under every earlier session (runs 72-92: 155-170), whose spread is about 3 %.
  - Before the run, about 150 was set as a clear gain and 152-163 as no measurable change. So 152 sits at that edge. The read here is a likely gain of about 4 %, not proven by the threshold set in advance.
  - It is the two changes together (fast fold and quiet compares); this run cannot split them.
- **FPS:** handheld is GPU-bound (heavy part 25.7), so FPS cannot show it. At 113-169 k guest calls per replay, 6 ns a call is about 0.7-1.0 ms of main-thread CPU per frame. Docked, CPU-bound at about 33 ms a frame, that would be about +3 % (30.0 to about 31 FPS). This is an estimate, not measured.
- **Control state:** +0.2 % (90 %: -7.1 to +6.7).
- **`[TLBBENCH]`:** aligned alias 196.4, offset 203.7 (+3.7 %), guest range 205.8. The same picture as run 92.
- **Main-thread profile** (heavy frames, 37,936 samples, 79 % in guest code). Still flat: `sub_8231ED30` 4.6 %, `sub_82320328` 3.5 %, `sub_822B5878` 3.2 %.
  - On or right after a float compare: 1.0 % of guest-code samples (3.0 % in run 91).
  - After `msr fpcr` (flush-mode switches): 2.4 %. There are 72,249 such instructions in the game code.
  - Region-table loads: gone.
  - What remains is the access after the new `csel`, stores into `ctx`, and the switches. No single cost is above about 2-3 %.

## Log 92: `0.0.98-log92`, build 1 of 3: -O3, one-TBL vector reversal, GPU 460.8 MHz as A/B state v

Build 1 of the plan in CPU_ROADMAP.md, "rc-guide material study": exact code changes together, plus the handheld GPU clock as an in-run A/B state. The fast fold (log91 B) is the default since run 93.

- **Game code at -O3** (`LSW_GAME_O3`, on; OFF = -O2). sa2-nx compiles its recompiled code at -O3. This is the same code, only a different optimisation level, and `-ffp-contract` and aliasing are unchanged from before.
- **Vector byte reversal** (`tune_tcs360_context.py --vec-rev`, applied to all generated files; `--revert` restores).
  - The generated `simde_mm_shuffle_epi8(x, simde_mm_load_si128((simde__m128i*)VectorMaskL))` reloads the mask from the non-const table at every site (stores may alias it), ANDs it with 0x8F, then runs a `tbl`.
  - `lsw_vrev16(x)` shuffles by the constant {15..0}, the table's first row, so the result is the same 16 bytes. GCC keeps the mask in a register: one `tbl`.
  - This was checked on a loop compiled with devkitA64. The rewrite round-trips byte-identically (apply, revert, diff).
  - 19,043 sites. In the ELF, vector `and`s go from 22,893 (log91 B) to 15,404.
- **State v: the official handheld performance configuration `0x92220008`** (GPU 460.8 MHz, memory 1331.2 MHz; nfsmw-nx, technique only). Homebrew starts at the default configuration (run 91: `0x00020003`, GPU 307.2 MHz).
  - `lsw_gpu_boost_set` (hw_monitor.cpp) sets it for the handheld mode only (`ApmPerformanceMode_Normal`), and puts the original back when the state goes. It logs `[GPUCLK]`.
  - The `[CLOCK]` lines show the GPU clock a few seconds later (the call returns before the clocks change).
  - Docked has its own configuration, so v changes nothing there.
- **env.txt:** `LSWTCS_ABSTATES=MAPXSHNLUBIkhwregdq,MAPXSHNLUBIkhwregdqv` (6 s each, random order). This replaces the control pair: runs 91-93 measured that noise (about ±7 %).
- **Test: handheld,** one launch, the usual route (the hub, `Gungan_B`'s heavy part 3-4 minutes, the hub).
  - **State v against the base:** FPS by place (`pair_states.py`), from the same session.
  - **-O3 and the reversal:** ns per guest call against run 93's 152 ns. Mode does not matter, but read it from the base state's windows, since v changes the GPU wait.
  - Expected: v well above noise in heavy frames if handheld is GPU-bound as measured (about 38 ms of GPU at 307.2 MHz).
- **Built:** 30 minutes (2 jobs), NRO 117.0 MB (B: 115.1). On the SD: `1cee37e1...`; ELF `private/lswtcs-log92.elf`. `guest_call_cost.py` now takes `--state S`.

## Run 94 (log92, handheld, 2026-11-06): 460.8 MHz is +9.7 % in heavy frames; -O3 and the vector reversal show nothing

One launch, the usual route (the hub, `Gungan_A`, `Gungan_B`'s heavy part, the hub; 230 s). No crash. In `private/runs/run94-log92` with its ELF.

- **State v works as built:** 28 `[GPUCLK]` switches, all `rc=0x0`, between `0x92220008` and the original `0x00020003`. `[CLOCK]` reads gpu=460.8 in 23 samples and 307.2 in 23.
- **FPS, v against the base** (same session, A/B every 6 s):
  - load-matched heavy frames (`pair_states.py`): **+9.7 % (90 %: +2.7 to +16.1)**, 9 against 7 visits;
  - 300-1300 draws: 34.7 to **44.2 FPS** (+27 %);
  - 1300-2500 draws: 25.9 to **28.0** (+8 %); mean frame 37 to 32 ms.
- **Why heavy frames gain less:** at 460.8 MHz a heavy frame takes about 32 ms, close to the docked CPU wall (run 71: 30.0 FPS, about 33 ms). So the main thread now limits heavy frames in handheld as well, and CPU gains will show there too.
- **Cost:** the SoC peaked at 44.0 °C (run 93: 42.4) and the battery fell 91 to 89 % in 230 s (run 93: 93 to 93). One short session only.
- **-O3 and the one-TBL reversal:** 151 ns per guest call (90 %: 151-153) in the base state's windows, against run 93's 152 (152-153, same state). No measurable change. Both stay: they are exact, and PGO (build 2) is tuned on top of them.
- **In v's windows: 157 ns per call** (156-158). A faster GPU may slow the CPU's memory accesses (shared LPDDR4), or the windows fell on different scenes (118-167 k calls per replay against 146-173 k). Not resolved.
- **`[TLBBENCH]`:** 191.1 / 198.1 (+3.7 %) / 201.3, as in runs 92-93.

## Log 93: `0.0.99-log93-pgogen`, build 2 of 3: the PGO counting session; 460.8 MHz by default

- **460.8 MHz is the handheld default** (run 94). `hw_monitor.cpp` asks for `0x92220008` once its services are open. 1.5 s later, and at every `[CLOCK]` sample, it checks the real clocks: in handheld at 460.8 MHz the memory must read 1331.2 MHz, or the original configuration goes back for the rest of the launch. This check is renut-nx's, technique only.
  - `LSWTCS_GPUBOOST=0` keeps 307.2 MHz; state v then switches it per A/B state.
  - It logs `[GPUCLK] default: ...` and `[GPUCLK] state v on: ...`.
  - `env.txt` is back to one A/B state.
- **PGO, generate** (`-DLSW_PGO=generate`, build folder `build-pgo`).
  - The game code and the runtime are built with `-fprofile-generate=<private/pgo/profile> -fprofile-update=single -fno-profile-values -fprofile-info-section=gcov_info`.
  - `source/lsw_pgo.cpp` streams every registered object (657) with `__gcov_info_to_gcda` into `sdmc:/lswtcs/pgo/lswtcs_<run id>.stream`. It writes every 3 minutes on a thread at priority 0x3F on core 2, and at exit. Each launch keeps one file, replaced by its latest snapshot.
  - `[PGO]` lines report each snapshot.
  - Method from sa2-nx's and renut-nx's docs; no code taken. GCC's `gcov.h` has no `extern "C"`: wrap the include.
- **Merge on the PC:** `python tools/pgo_merge.py <folder with the .stream files>` runs `gcov-tool merge-stream`, then sets `runs` in object summaries the stream left at 0.
- **Use:** reconfigure the same folder with `-DLSW_PGO=use` and rebuild.
  - It must be the same folder: the `.gcda` names hold the object paths. `-fprofile-prefix-path` would need the folder with backslashes, since GCC compares Windows paths, and that does not survive make.
  - The use flags are `-fprofile-correction -fprofile-partial-training`.
  - Change no game or runtime code between generate and use.
- **Sizes:**

  | | Instrumented | Normal |
  | --- | --- | --- |
  | NRO | 246.6 MB | 117 MB |
  | `.text` | 155.9 MB | 79.4 MB |
  | `.data` + `.bss` | 115.9 MB | 43.3 MB |

  About 160 MB more of the heap goes to the image, and the last run ended with 672 MB free. The build took about 50 minutes (2 jobs, new folder).
- **Test (handheld or docked; FPS does not count):** play the usual route plus anything else normally played, about 10 minutes. Quit through the game's menu, not HOME, so the exit snapshot is written. Bring back `lswtcs/pgo/` with the logs.
- **Not yet verified on the console:** that the snapshot writes. The `[PGO]` lines show it, or the error.

## Run 95: log93 instrumented NRO, 31 minutes in handheld; no counts written

- **No `.stream` reached the card.** The log has `[PGO] instrumented build: 657 objects registered` and no `[PGO] snapshot` line at all, in 31 minutes.
- **Cause:** the snapshot thread asked for priority 0x3F. hbloader's NPDM allows this process 0x1C to 0x3B (the monitor thread uses 0x3A and runs), so the thread was most likely refused, and its result was not logged. Not confirmed on the console; log94 logs it.
- **The exit snapshot never ran either:** the log has no `[NX] runtime main returned` line, so the game was closed from HOME (or ended otherwise), where nothing at exit runs. The game has no option that ends the process, so the periodic snapshots are the only path to the card.
- **460.8 MHz for 31 minutes:** GPU 460.8 and memory 1331.2 MHz in every one of 373 `[CLOCK]` samples; the memory check never reverted. SoC 33.0 to 46.2 °C, PCB up to 44.0 °C; battery 92 to 82 %. Not comparable with earlier runs' FPS: the instrumented build is slower.
- **Capture at replay 61211 (end of a level, during a transition):** stars over most of the screen and the previous scene on the right with a soft edge, drawn as 16 star draws and a full-screen quad that samples the resolved scene. The tester reports a visual defect in that cutscene that this frame may not show in full. One frame is not enough to tell the defect from the game's own wipe; their description is needed. The 40 history frames are identical and the memory dump of the sampled target is not the GL copy the draw uses, so neither helps.

## Log 94: `0.0.99-log94pgo`, the counting NRO again with the snapshot thread fixed

- Priority 0x3B (the lowest allowed), and `[PGO] snapshot thread rc=0x...` is logged.
- First snapshot after 1 minute, then every 2 minutes (was 3). Closing from HOME loses at most the last 2 minutes.
- Only `lsw_pgo.cpp` and the NACP version changed; the game code and the rest of the runtime are the same objects, so the counts fit the `use` build of the same folder.
- **Test:** play as in run 95, at least 10 minutes; closing from HOME is fine. Bring back `lswtcs/pgo/` with the logs. The log should show `rc=0x0` and `[PGO] snapshot 1 (periodic): 657 objects, ...` about a minute in.

## Run 96: log94 instrumented NRO, 8.8 minutes in handheld; counts written

- **The snapshot thread ran:** `rc=0x0`, then 4 snapshots of 657 objects, 24.2-24.4 MB each. The first took 40.6 s (new file); the others took 6-12 s, on core 2 at the lowest priority.
- **Played:** the map, Gungan A, B, C and E (Episode I), and PodSprint A (a vehicle level). Closed from HOME, so the last 1-2 minutes are not in the counts.
- **460.8 MHz again steady:** memory at 1331.2 MHz in all 105 samples; SoC 40.6 to 45.2 °C.
- **Merging, two fixes in `tools/pgo_merge.py`:**
  - **Long paths.** Under `private/pgo/profile`, 9 runtime objects' `.gcda` names were 256 to 264 characters long, and the Windows tools do not write past 259. The compiler would not read them either. The profile folder is now `private/pp`: the tool rewrites each name in a copy of the stream (`private/pp-streams`) before `merge-stream`. The longest name is now 255 characters, and all 657 files are written.
  - **Empty summary.** The stream path writes the object summary as zeros, `sum_max` as well as `runs`. GCC sets its hot threshold from `sum_max`, so with 0 every block would count as hot. Each summary now gets `runs = 1` and `sum_max` = the largest arc count over all objects: 11.3 billion (probably a wait loop), stored in 32 bits as 4294967295.

## Log 95: `0.0.99-log95pgo`, build 3 of 3: optimised with run 96's counts

- **Build:** `build-pgo` reconfigured with `-DLSW_PGO=use`, so the profile folder is `private/pp`. Full rebuild of the game code and runtime; the same sources as log94 apart from the PGO hooks. About 33 minutes with 2 jobs.
- **Warnings:** the use flags are now `-Wmissing-profile -Wno-error=coverage-mismatch` (were silenced). The build printed none, but that proves nothing: GameCode and the runtime are compiled with `-w`, which suppresses every warning.
- **Checked object by object instead:**
  - 358 of the 633 game-code objects have no count above zero: they never ran in run 96.
  - 316 game-code objects have the same `.text` size as in `build-mesa`.
  - Every object that ran changed, so the counts were read wherever there were any.
  - The 57 % that never ran is the case for wider counts.
- **The profile is applied:**
  - `.text` is 109.8 MB, against 103.6 MB in `build-mesa` (not PGO).
  - Game-code objects change size by -2 % to +4 %.
  - `gl_backend.o` has 104 hot or cold sections, against 8 without PGO.
  - Guest functions are not split into hot and cold sections. With `-fprofile-partial-training`, functions that never ran keep the normal optimisation.
- **Test (handheld, the usual route; 460.8 MHz is the default):** nothing to switch. Compare with run 94's 460.8 MHz windows: 157 ns per guest call (`guest_call_cost.py`), and FPS in the heavy and medium scenes (medium 44.2). Gameplay outside the counted levels (the map, Gungan, PodSprint) may gain less, or lose a little.

## Run 97: log95 (PGO) in handheld at 460.8 MHz, 5.7 minutes: main thread about 11 % faster

- **CPU per guest call: 140 ns** (90 %: 139-143; 15 heavy windows). Earlier results:
  - run 94's 460.8 MHz windows: 157 ns;
  - run 94 and run 93 at 307.2 MHz: 151-152 ns;
  - the instrumented build: 227 ns.

  About -11 % at the same clock, and the lowest of any session; the earlier cross-session spread was 3 %.
- **Load-matched frame time, Gungan levels** (`private/session-tools/pair_runs.py`, new). Frames are binned by draw count and compared with run 94's 460.8 MHz state.
  - **Heavy (1300+ draws):** -10.2 % frame time, +11.4 % speed (90 %: -30.7 to -0.6 % frame time; 2787 against 1489 frames). Unmatched means: 30.0 against 28.0 FPS.
  - **Medium (700-1299 draws):** -0.7 % (90 %: -16.5 to +3.4 %; 158 frames on run 94's side). No measurable change. These frames are probably GPU-limited at 460.8 MHz.
  - **The tool's checks:** run 94's v against its base state gives +10.1 % heavy, as reported then (+9.7 %). Run 93 against run 94's base state (same build and clock) gives +0.7 % heavy.
- **Caveat:** one session against another. The heavy gain matches the main-thread CPU gain, as expected now that heavy frames are CPU-limited at 460.8 MHz. Gungan B and C were played here, and only B in run 94. No errors in the log.
- **Kept:** the PGO build is the build to stage from now on. Changed functions lose their counts (shown as coverage-mismatch warnings) until a new counting session, so the counts should be renewed after large changes. The instrumented build of the same code is `-DLSW_PGO=generate` in `build-pgo`.

## Log 96: `0.0.99-log96nv`, guest memory without volatile again, with the run 78 fault fixed

**Why now.** The two hottest game functions (`sub_8231ED30` and `sub_82320328`, 8 % of the main thread in run 97) spend their samples on loops that load, increment and store a guest register kept in `ctx` (r10, at `[ctx, #64]`) on every iteration. GCC's loop store motion gives up on loops that contain `volatile` accesses, and every guest load and store is `volatile`.

**PC test on unit 359** (GCC 16.1, `-O3`, no PGO; instructions and `ctx` loads and stores per function):

| | `sub_82320328` | `sub_8231ED30` |
| --- | --- | --- |
| A: as now (volatile) | 508 / 101 | 1409 / 297 |
| B: log77's non-volatile copy (barrier clobbers all memory) | 514 / 47 | 993 / 89 |
| C: non-volatile, barrier on guest memory only | 513 / 44 | 989 / 75 |

In C the r10 loop keeps the register in a host register (the reload is gone; one store per iteration is left).

**Log96 = C, made from the current output** (`switch/tools/make_nonvolatile_output.py` into `output_nv`, build folder `build-nv`, no PGO):
- **`-fno-strict-aliasing` on GameCode.** Run 78's camera drift and audio pitch were very likely type-based aliasing: without `volatile`, a 32-bit store and a 16- or 64-bit load of the same guest address could pass each other. The runtime has always had the flag.
- **`LSW_GUEST_BARRIER()` is now `asm volatile("" : "+m"(*(uint8_t (*)[4 GB])base))`:** it may read and write all of guest memory, and nothing else.
  - It sits at the 33,616 loop headers and 70 guest fences.
  - Guest values are reloaded after it, as before. Registers kept in `ctx` (restrict, never in guest memory) may stay in host registers.
- **Unchanged:**
  - MMIO stores stay volatile;
  - `lwarx`/`stwcx.` stay atomics;
  - the runtime keeps volatile;
  - quiet compares, fast fold and vector reversal are in, as in log95.
- **Boot line:** `[NX] guest code: loads and stores without volatile, no strict aliasing, guest-memory barriers at loop headers and fences (log96)`.
- **Not PGO.** Changed generated code drops the counts. The comparison is run 94's 460.8 MHz state (the same code with volatile, no PGO): 157 ns per guest call. If log96 holds up, the wide counting session is done on its code.
- **Built:** `.text` 101.6 MB, against 103.6 MB in `build-mesa`. `sub_8231ED30` went from 5,636 to 3,956 bytes; `sub_82320328` from 2,032 to 2,052. NRO `545240f85bbe53a8`, ELF `private/lswtcs-log96nv.elf`.
- **SD:**
  - `switch/lswtcs.nro` = log96;
  - `switch/lswtcs-log95pgo.nro` = run 97's build (`029aa06125b91c0a`), the fallback for normal play.
- **Test (handheld, normal speed, the usual route: the hub, Gungan A and B's heavy part, back to the hub):**
  - Watch for run 78's faults (the camera moving by itself, a lower audio pitch) and anything else unusual. If one shows, close it and use the log95 NRO.
  - From the logs: ns per guest call against 157 (run 94 at 460.8 MHz, the same code with volatile, no PGO), and load-matched FPS against run 94's v state (`pair_runs.py`).

## Run 98: log95 (PGO) again, not log96; the PGO CPU gain repeats, its FPS gain is smaller than run 97 showed

- **Not log96.** The boot line reads `volatile loads and stores (normal build)`, and the profiler's code range (89.4 MB) is log95's. Both NROs showed in sphaira under the same name, `LSWTCS`, so the fallback was launched. It is now `lswtcs-reserva-log95.nro`, titled `LSWTCS reserva log95` (the same ELF repacked).
- **Played:** the hub, Gungan A, B and C; handheld at 460.8 MHz. No errors. SoC at most 40.8 °C.
- **CPU per guest call: 141 ns** (90 %: 140-144), against run 97's 140 with the same build. The PGO CPU gain repeats (run 94 at 460.8 MHz without PGO: 157).
- **FPS between sessions is noisy.** The same build against run 97, load-matched: heavy -6.1 % (90 %: -21 to +2), medium +12.1 %. Against run 94's 460.8 MHz state, heavy +3.1 % (90 %: -4.4 to +12.5), not run 97's +11.4 %.
- **Correction to run 97:** PGO's main-thread gain (-10 % CPU per guest call) is solid. Its heavy-frame FPS gain is somewhere between about +3 and +11 %. One session against another cannot place it closer.
- **Method:** ns per guest call is the measure to trust across sessions (±1 % here). Cross-session FPS needs in-run A/B, or several sessions.

## Run 99: log96 (guest memory without volatile) in handheld at 460.8 MHz: about -11 % CPU per guest call without PGO

- **It was log96:** the boot line reads `... no strict aliasing, guest-memory barriers at loop headers and fences (log96)`.
- **Played:** the hub, Gungan A, B, C and E; 7.3 minutes. No errors in the log. SoC at most 44.4 °C.
- **CPU per guest call: 139 ns** (90 %: 138-141; 15 heavy windows). Earlier results:
  - run 94's 460.8 MHz windows, the same code with volatile and no PGO: 157;
  - PGO (runs 97 and 98): 140-141.

  So log96 gains about as much as PGO did, without it.
- **Load-matched FPS, Gungan:** against run 94's 460.8 MHz state, heavy +6.7 % (90 %: -4.2 to +41) and medium +4.7 %. One session against another; run 98 showed that such comparisons move by ±10 %.
- **Behaviour:** normal, the tester reports: no camera moving by itself and no change in audio pitch (run 78's faults). So `-fno-strict-aliasing` fixed what run 78 showed, and log96 becomes the build to play.
- **Next:** log97, the instrumented NRO of log96's code (`build-nvpgo`, counts to `private/pn`), for the wide counting session. Then log98, log96's code optimised with those counts.

## Log 97: `0.0.99-log97nvg`, the instrumented NRO of log96's code, for the wide counting session

- **Build:** `build-nvpgo`, `-DLSW_PGO=generate`, `RECOMP_OUTPUT` = `output_nv`, counts to `private/pn`.
  - The same options as `build-nv` (PCH on, units of 5); the 633 unity files are identical.
  - The longest `.gcda` name is 257 characters (the folder name is 2 characters longer than `build-pgo`'s).
- **SD:**
  - `switch/lswtcs.nro` = log96 (title `LSWTCS`), the build to play;
  - `switch/lswtcs-contagem-pgo.nro` = log97, under its own title `LSWTCS contagem PGO` (the same ELF repacked with that NACP name; `6e7ca985011aaccc`, ELF `private/lswtcs-log97nvg.elf`).
  - The log95 fallback is gone: log96 behaved normally.
- **Test, when the tester has time (slow, 15-20 minutes, any mode):**
  - several episodes, vehicle levels, bonus levels, the hub, cutscenes, and Gungan too: these counts start from zero;
  - close from HOME;
  - bring back `lswtcs/pgo/` and the logs.
- **Then:** `python tools/pgo_merge.py <streams> --profile-dir private/pn`, `build-nvpgo` reconfigured with `-DLSW_PGO=use` (log98nvp), and a normal measuring session.

## Run 100: log97 (instrumented log96), the counting session for log96's code

- **Played:** the map, PodSprint A and Episode III's Dogfight A; 17 minutes in handheld. Eight snapshots, all 657 objects, 24.2-24.5 MB each, 3.6-19.5 s per write. No errors.
- **Merged:** `tools/pgo_merge.py private/pgo/streams-nv --profile-dir private/pn`. 657 `.gcda` files; the longest name is 257 characters. Largest arc count 29.3 billion, stored as 4294967295.
- **The card still held run 96's stream:** that one is from the volatile code, so it was left out.
- **Coverage against run 96 (Gungan, map, PodSprint).** Function ids are the same in both code versions (809,466), so the sets compare:
  - functions that ran: 8,346 in run 96, 8,250 here, 8,019 in both;
  - the functions only run 96 ran hold 0.53 % of its arc counts.

  So the Gungan route is covered although Gungan was not played. Game-code objects that ran: 276 of 633 here, against 275.
- **Next:** log98 (`0.0.99-log98nvp`): `build-nvpgo` with `-DLSW_PGO=use`, then a measuring session on the usual route.

## Log 98: `0.0.99-log98nvp`, log96's code optimised with run 100's counts

- **Build:** `build-nvpgo` reconfigured with `-DLSW_PGO=use` (`private/pn`); the same sources as log96 and log97.
  - `.text` is 107.9 MB, against 101.6 MB for log96.
  - 317 of the 633 game-code objects changed; the 316 unchanged ones never ran in run 100.
  - `gl_backend.o` has 129 hot or cold sections.
- **SD, one folder per build:** sphaira lists every `.nro` directly in `switch/`. In a subfolder it shows `<folder>/<folder>.nro` alone, or else only the first `.nro`; that hid the counting NRO, and very likely caused run 98.
  - `switch/lswtcs/lswtcs.nro`: log96 (`LSWTCS`), the build to play;
  - `switch/lswtcs-log98/lswtcs-log98.nro`: log98 (`LSWTCS log98 teste`). ELF `private/lswtcs-log98nvp.elf`.
- **Test (handheld, normal speed, the usual route):** compare with:
  - log96's 139 ns per guest call (run 99), which has no PGO;
  - log95's 140-141, which has PGO but uses volatile.

  How much of the two gains adds up is unknown.

## Run 101: log98 (log96's code plus PGO) in handheld: 127 ns per guest call; handheld heavy frames are now GPU-limited

- **It was log98:** the boot line is log96's, but the profiler's code range (0x5387000) matches log98's `.text` (0x53869b0); no `[PGO]` lines.
- **Played:** the hub, Gungan A and B; 3.5 minutes, 14 heavy windows. SoC at most 37.3 °C.
- **CPU per guest call: 127 ns** (90 %: 126-130). The two gains add up:

  | Build | ns per guest call |
  | --- | --- |
  | Run 94 at 460.8 MHz: volatile, no PGO | 157 |
  | log95 (PGO) | 140-141 |
  | log96 (no volatile) | 139 |
  | **log98 (both)** | **127, about -19 %** |

- **Heavy-frame FPS did not move,** load-matched:
  - +2.7 % against run 94's 460.8 MHz state;
  - 0.0 % against run 98;
  - -5 % against run 99.

  The reason is in `[GLSPAN]`: in heavy windows the GPU's idle gap between frames is 0.0 ms in every run (draws about 27-28 ms of a 30-33 ms frame). At 460.8 MHz the handheld GPU is the limit in heavy Gungan frames, so CPU gains no longer show there. They should show docked, where the GPU runs at 768 MHz and the main thread was the limit (run 71). They also cut power: SoC 37.3 °C here against 44.4 in run 99, on a shorter route.
- **Unexplained:** 987 bytes of high-entropy binary data in `stdout.log`, between two ordinary lines (after line 2665). The surrounding lines are intact and swap numbers continue. Earlier logs have none (their one flagged line is an em dash). Either a card write fault or a runtime print of a raw buffer; to watch in the next logs.
- **log98 becomes the build to play:** `switch/lswtcs/lswtcs.nro`, title `LSWTCS`, version `0.0.99-log98nvp` (the same ELF repacked with that title). The test folder is gone.
- **Next:**
  - a docked session on log98, to see the CPU gain in FPS;
  - in handheld, only GPU work raises heavy-frame FPS: resolve copies, pixel cost.

## Run 102: log98 docked: the main thread is 23 % cheaper but no longer the limit; docked FPS barely moves

- **Build and mode:** log98 (code range 0x5387000), docked: GPU 768 MHz, memory 1600 MHz, in all 54 `[CLOCK]` samples.
- **Played:** the hub, Gungan A, B and C; 4.4 minutes.
- **CPU per guest call: 124 ns** (90 %: 122-127), against run 71's 162 (log71, the last docked session).
- **Main thread per heavy frame (`[THREADCPU]` windows of 100k+ calls):** 16-20 ms, against 23-27 ms in run 71. Frame time stayed about 28-35 ms in both.
- **FPS, load-matched against run 71:** heavy +2.2 % (90 %: -4.9 to +6.9), medium +2.3 %. Unmatched averages:

  | | Run 102 | Run 71 |
  | --- | --- | --- |
  | Whole session | 48.9 FPS | 50.5 FPS (different route mix) |
  | Heavy frames | 31.1 FPS | 30.0 FPS |
  | `Gungan_B` | 39.3 FPS | 37.3 FPS |
  | `Gungan_C` | 56.8 FPS | 52.5 FPS |
- **What limits docked heavy frames now.** Not the main thread (16-20 ms of a 28-35 ms frame). Not the GPU either: its draws take about 22 ms, and `[GLSPAN]` shows a 3-6 ms idle gap per frame.
  - **The render path:** the render thread spends 13-16 ms of CPU per heavy frame on the replay, and Mesa's worker 10-12 ms, about as in run 71.
  - **The main thread's frame-boundary waits:** 6.9 % of its heavy-band samples in `VdSwap` waiting for the off-turn walker, and 5.7 % in present waiting for the previous frame.

  So a docked heavy frame is now set by the chain from the game's frame to the GPU (walk, replay, driver), not by any one thread.
- **What this changes:**
  - **deko3d.** CPU cost per draw on the render thread and in the driver is now on the docked critical path. That is the cost deko3d removes (no GL front end, no glthread round trips). The earlier estimate, that docked is limited by the main thread and so a GPU API cannot raise it, no longer holds.
  - **Mesa.** On Mesa, the render thread's replay cost and the frame hand-over (`VdSwap` waiting for the walker) are the levers.
- **The binary block again:** 2,512 bytes of high-entropy data in `stdout.log`, after a `[GLSTEP]` line and right before the same `[OBJACT] ... class 1 obj 967CD550` burst as in run 101. It is seen in both log98 sessions, and in none of runs 97-99. `objlog_name` prints only printable bytes, and the async log ring looks correct, so the source is open.

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

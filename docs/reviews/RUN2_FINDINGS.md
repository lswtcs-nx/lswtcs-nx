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
targets behind `LSWTCS_CAPRES=1` and uses larger stdio buffers. The gain is not yet measured.
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

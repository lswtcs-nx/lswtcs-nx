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

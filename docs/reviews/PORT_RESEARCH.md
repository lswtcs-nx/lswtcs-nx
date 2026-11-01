# Research: GPU cost in other Xbox 360 to Switch ports, 1 November 2026

Follows `TCS360_MESA_REFERENCE_REVIEW.md`, which covers the same family of ports for lifetime,
cache and CPU work. This pass asks one question: **what lowers GPU time, given that run 2 found
pixel work to be about 60 % of heavy frames?** Reading only: nothing here has been tried on TCS,
and other games' numbers do not transfer.

## Candidates, in suggested order

### 1. Official handheld GPU profile: a decision, then a measurement

Mass Effect requests the official handheld profile at start-up: CPU 1020, GPU 460.8, memory 1331.2
MHz (`sdk/src/ui/switch_apm.cpp`; its README calls it the official Nintendo handheld profile).
Switchbrew lists it as configuration `0x92220008`, and `0x92220007` as the same with memory at
1600 MHz. sys-clk's table labels 307 and 384 as official handheld clocks and 460 as the
original handheld maximum. Older Switchbrew revisions were the source for these values, so
check them against current firmware.

TCS never calls `apmSetPerformanceConfiguration`, and its logs do not record the active
configuration, so the clock under which all our numbers were taken is **unknown**. If it is 307.2 MHz,
460.8 MHz would be 50 % more GPU clock for a GPU-limited game with no image change.

This is not an overclock in the sys-clk sense, but it is still a clock change, with battery and heat
costs. The harness says no silent clock changes, and the owner tests at stock handheld to track
performance. So it needs the owner's decision.

Smallest safe step: log the configuration from `apmGetPerformanceConfiguration` (read-only), then, only
if agreed, an off-by-default `env.txt` option for one A/B run.

Do **not** use official CPU boost mode. Mass Effect measured 3.2–4.5 FPS because it throttles the GPU
to 76.8 MHz. TCS does not call `appletSetCpuBoostMode`.

### 2. Colour compression: possibly large, higher risk

`nvc0_mt_choose_storage_type` gives single-sample 32-bit colour kind `0xfe`, uncompressed; the
compressed `0xdb` line is commented out with "This one makes things blurry". `nouveau.c:463` also says
`NvKind_C32_2C` and `C32_2CRA` "need special support that nouveau seems to lack". deko3d maps
compressed images as `NvKind_C32_2CRA` and aligns them to a big page.

Since the port renders 2x MSAA surfaces at one sample, its colour targets get no compression. Whether
depth kinds really gain compression tags here is unverified: `nouveau_bo_new` aligns to 4 KB on
this path, while deko3d uses big pages. Bandwidth gain on TCS is unmeasured.

Our own Deko backend never sets `DkImageFlags_HwCompression`. That is a cheap thing to remember if Deko
is reopened, and worth measuring before judging Deko's GPU time.

### 3. Resolve work

Heavy frames spend about 8.9 ms on resolves (4.3 ms remains with pixels off) and 4.4 ms on texture-source
blits. Dead-copy skipping already exists. NFSMW avoids resolve copies by swapping images when the next
step clears the target and restoring old content when it is needed; USF4 samples depth directly
for about 0.4 ms. Plan item R2 applies: validate the complete image, effects and guest readback.
Note `gl_rt.inc`: `glCopyImageSubData` was measured slower than blits on Nouveau, so do not retry it.

### 4. Per-pixel fetch-constant work

Each fetch re-derives offset, LOD bias and exponent from uniforms with `bitfieldExtract`, although these
are constant per draw. NFSMW reports cleaner translated shaders without a figure. Mass Effect's
texture-sign specialization gave no gain, so expect a modest result. Folding needs variant identity
under R2, and variants cost compile time, as the early-Z hint showed.

### 5. ZCULL: low priority

`nvc0_validate_zcull` is under `#if 0`, and screen init deactivates the region. NFSMW reports about 2 ms from
enabling it on NVK; Mass Effect measured no gain (35.3 vs 34.7 ms, vertex-bound). Undocumented
registers make this a risky driver change for an uncertain return.

## Already covered

- **Vertex outputs pruned to what the pixel shader reads** (Mass Effect, 66 to 36.6 ms): the bridge already
  masks interpolators by what the pixel shader uses.
- **Early depth for draws without alpha coverage** (Mass Effect ships this): done in `0.0.12-log3`. Mass
  Effect rejected it for alpha-tested writers because the image was wrong, and our conditions exclude those.

## bo2-360-nx: the closest relative

[bo2-360-nx](https://github.com/AlvaroEPena/bo2-360-nx) is Black Ops II from the 360 executable, by static recompilation with ReXGlue,
drawing through Vulkan on NVK. GPL-3: read for technique, no code taken. Its numbers come from A/B runs on that game, and do not
transfer. What applies here:

- **Threads are the big difference.** Each of its guest threads is a Horizon thread on several cores, with the 360's atomics behind a
  global lock. TCS runs one guest thread at a time on core 0. In run 7 that was the limit. `LSWTCS_WALKOFF`, the small step in that
  direction, failed on hardware, which shows the guest and the walker share state without protection. Leaving the turn model is a large
  change and needs evidence and the owner's decision.
- **A contended lock can dominate.** Its early profile had game threads about 75 % of samples in `svcArbitrateLock`, because newlib's
  mutex sleeps at the first contention; it spins briefly before sleeping. TCS's main thread holding the turn with little CPU could be
  the same family, or not: `LSWTCS_HWPROF` decides first.
- **LTO and PGO: +9.8 % and +9.0 %** in a CPU-limited game. Mass Effect measured nothing, so "Not to adopt" below was too strong. TCS has
  neither (`-O2`, unity units), and heavy levels are CPU-limited. Worth one experiment, but the build cost with two jobs is unknown.
- **A fourth core** from a forwarder gave +12-16 %, at some risk of instability. Audio and XMA went alone on it. An owner decision.
- **Audio:** XMA frame cache +3.8 %, mixer rewrites +2.7 %. TCS's audio thread uses 40-48 % of a core in heavy scenes.
- **Native rewrites run beside the original first**, compared word for word, and turn themselves off on a difference.
- **Its renderer's gains** (+70-80 % from sampler cache, state fingerprints and copy-engine uploads) are Vulkan and NVK patches. Copy
  engine +9.5 %: `gl_rt.inc` measured `glCopyImageSubData` slower here, so it does not carry over directly.
- **Lower-quality presets and far-object hiding** are its main route to 60 FPS, and any such change here is a product decision.
- **Not adopted there:** thread priorities (-2 to -5.4 %), which fits the FPS cost seen with `LSWTCS_AUDIOPRIO`; internal resolution
  (no gain while the CPU sets the pace).
- **Already matched here:** logs on their own thread, A/B within one run.

## Implications for Deko

USF4's paired comparison at raised clocks shows GPU 9.3 to 8.4 ms but render thread **18.6 to 2.8 ms**:
Deko's clear win is CPU submission. TCS frames are mostly GPU-limited, and merging already cut replay CPU by
about 40 %. This supports the owner's order. It does not settle Deko, because USF4 is a different workload
and frontend, and compression may matter.

## Not to adopt

- Lower internal resolution by default (Mass Effect, 960x544), or half-resolution post-processing,
  which Mass Effect rejected as inexact. Any quality change is a product decision.
- Function ordering as an expected gain. For LTO and PGO, Mass Effect measured none and bo2-360-nx about +9 % each: an experiment, not an expectation.
- Async compile that skips draws: Mass Effect saw 4,515 skipped draws as pop-in.

## Sources

- [Mass Effect optimization paths](https://github.com/nebadasSwifty/masseffect-nx/blob/main/docs/optimization-paths.md)
  and [README](https://github.com/nebadasSwifty/masseffect-nx)
- [NFSMW native renderer](https://github.com/StevensND/nfsmw-nx/blob/main/docs/native-renderer.md)
  and [Mesa notes](https://github.com/StevensND/nfsmw-nx/blob/main/docs/mesa.md)
- [USF4 deko3d renderer](https://github.com/Thorhax/usf4-nx/blob/main/docs/deko3d-renderer-usf4.md)
- [deko3d](https://github.com/devkitPro/deko3d)
- [Switchbrew PPC services revision](https://switchbrew.org/wiki/Special:MobileDiff/10485)
- [sys-clk](https://github.com/retronx-team/sys-clk/)

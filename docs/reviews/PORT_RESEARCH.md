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

## Implications for Deko

USF4's paired comparison at raised clocks shows GPU 9.3 to 8.4 ms but render thread **18.6 to 2.8 ms**:
Deko's clear win is CPU submission. TCS frames are mostly GPU-limited, and merging already cut replay CPU by
about 40 %. This supports the owner's order. It does not settle Deko, because USF4 is a different workload
and frontend, and compression may matter.

## Not to adopt

- Lower internal resolution by default (Mass Effect, 960x544), or half-resolution post-processing,
  which Mass Effect rejected as inexact. Any quality change is a product decision.
- LTO, PGO and function ordering as an expected gain: Mass Effect measured none.
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

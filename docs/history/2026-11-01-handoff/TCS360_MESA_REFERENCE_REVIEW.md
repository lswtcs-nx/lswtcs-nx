# Xbox360/Mesa comparison — updated26 October 2026

Supporting evidence for the canonical Mesa-first harness, not a second work plan.
Six Xbox360 port repositories/paths were reviewed: five newly pinned below and
The Simpsons from the preceding review. Public source and documentation only;
no binaries, game-derived shader packs, drivers or dependencies were installed.
No published performance result was reproduced on this project's console.

## Pinned references and applicability

| Project / reviewed revision | Useful evidence | Important limits |
| --- | --- | --- |
| [NFSMW](https://github.com/StevensND/nfsmw-nx/tree/be2d4c67ab42d6d629746a01dbf03cef824c8d2d) | Console measurement protocol, cached native PM4 renderer, draw/texture/upload CPU work, compiler/cache revisioning | NVK/Vulkan, not our GL/Nouveau. Some selectable changes alter effects/geometry; not approved TCS optimizations. |
| [Mass Effect](https://github.com/nebadasSwifty/masseffect-nx/tree/650e14ef109fb0d39bd68ba8d7af61fe459265b8) | Cold versus warm pipeline work, cache write stalls/memory, register-dirty contracts, wait semantics | Some documented implementations explicitly untested on hardware; preserve those distinctions. Lower-resolution or raised-clock examples are not TCS targets. |
| [Project8](https://github.com/R-drg/project8-sw/tree/3393f2615deb7e8ac11ce51d2cd0e9defded5669) | PM4 native Vulkan implementation, resource/view interpretation, wait/pacing | Documents raised GPU clocks, omitted first-use draws/late textures and incomplete MSAA depth resolves. Its picture/coverage policies cannot be copied as TCS fixes. |
| [The Simpsons](https://github.com/R-drg/the-simpsons-game-nx/tree/3cb2cdf7fa1665a1f2d653aa7502fe4baa03680f) | Full-system costs: streaming schedule, CPU/game work, movie decoding, cached native NVK rendering | Raised-clock results, replacement effect shaders and an alternate decoder are separate product/semantic choices. A README inconsistency limits the stock-clock claim. |
| [Prototype/Prototype2](https://github.com/fungalspores/prototype-switch/tree/292de4b382dbe335011d8b4f56ec4212df5ef74c) | Lock/backpressure trace, cache/streaming and native-save file-size semantics, PGO workflow | FPS uses heavy overclocks and P2 quality reductions. Actual drain fix releases owned guest locks; that is not a generally safe TCS synchronization recipe. |
| [Futari](https://github.com/ChanseyIsTheBest/futari-nx/tree/99f13a612bf18f6d07aeaca250dda3bde04fb568) | Historical1.0.0 NVK path retained alongside later Deko; shared SDK/compiler/runtime lessons | Current1.0.1 uses Renut/Deko. Do not count its current60FPS claim as a Mesa result. Search summaries saying 'Vulkan' can be stale. |

These share NFSMW/Mass Effect/ReXGlue ancestry. This is evidence of reusable
implementation mechanisms, not six independent comparisons proving Mesa faster.
Mega Man64/GLideN64 is an additional GL/Mesa threading/cache reference, explicitly
N64 rather than Xbox360. Prior Dusklight/Renut studies remain preserved evidence.

## Concrete findings incorporated into the harness

**Measure the actual cost.** NFSMW's [measurement guide](https://github.com/StevensND/nfsmw-nx/blob/be2d4c67ab42d6d629746a01dbf03cef824c8d2d/docs/measuring.md)
tracks like-for-like load, no-op controls, counter definitions, observation cost
and frame-time tails. Its [native renderer](https://github.com/StevensND/nfsmw-nx/blob/be2d4c67ab42d6d629746a01dbf03cef824c8d2d/docs/native-renderer.md)
shows PM4 processing can be useful without adopting a Direct3D frontend. TCS
already has native GL rendering and substantial profiling; we should audit it,
not assume it still runs the expensive generic SDK Xenos backend described there.

**Shader cache work can itself hitch.** Mass Effect's [cold-start analysis](https://github.com/nebadasSwifty/masseffect-nx/blob/650e14ef109fb0d39bd68ba8d7af61fe459265b8/docs/cold-start-hitches.md)
separates compilation, warm replay and SD/cache serialization; it documents
large writes blocking game reads and duplicate serialization buffers exhausting
memory. Transfer bounded memory, durable compatible reuse and measured write
cadence. Do not copy Vulkan's cache API or its per-game cold-FPS requirement into
our OpenGL code. A background thread alone does not make its I/O free.

**Dirty constants must preserve complete draw inputs.** Mass Effect's
[batched-constants note](https://github.com/nebadasSwifty/masseffect-nx/blob/650e14ef109fb0d39bd68ba8d7af61fe459265b8/docs/batched-constants.md)
reuses existing buffers, tracks changed vectors across every write path, checks
against a full comparison and falls back on lost synchronization. It explicitly
does not overwrite only a few vectors inside earlier immutable GPU inputs. The
document says the candidate is not hardware-validated and notes an exact-symbol
limitation; its expected0.8ms is not a demonstrated transferable TCS saving.

**Wait optimization requires the owning contract.** Mass Effect's [kernel waits](https://github.com/nebadasSwifty/masseffect-nx/blob/650e14ef109fb0d39bd68ba8d7af61fe459265b8/docs/kernel-waits.md)
examines heap work/notifications and lost-wake tests. Prototype's actual
`prototype/src/proto_deadlock_fix.cpp` illustrates renderer backpressure while
the producer holds required locks and a consumer waiting for itself. TCS must
trace its own first invalid transition. Blindly releasing all locks or yielding
at another game's address could expose partially updated shared state.

**Driver changes have applicability boundaries.** NFSMW's [Mesa note](https://github.com/StevensND/nfsmw-nx/blob/be2d4c67ab42d6d629746a01dbf03cef824c8d2d/docs/mesa.md)
covers NVK/NAK/ZCULL and compiler cache revisioning. Our pinned build is patched
Mesa20.1 GL/Nouveau: audit existing8 edits before any upgrade, and preserve exact
driver/source identity. Installing a new SDK/Rust/compiler/driver is not implied
by reading these sources and requires a prior explicit yes.

**Some shortcuts conflict with our product contract.** Project8's
[renderer notes](https://github.com/R-drg/project8-sw/blob/3393f2615deb7e8ac11ce51d2cd0e9defded5669/docs/native-renderer.md)
explicitly skip unready draws and expose late textures. NFSMW describes deferred
fingerprint checks and game-specific readbacks. Those are not general proofs
that TCS can sample stale content or omit CPU visibility. Preserve full valid
rendering and correct producer/alias lifetime across cache optimizations.

## Local audit and evidence limits

- Rehashed tested004 NRO and matching ELF/map. Later hardware save/user confirmation
  is retained separately from the original static-only package metadata.
- Current `switch/build-repro-make` is Deko-on; a Mesa recipe needs qualification
  without destroying633 GameCode objects/PCH. Old `switch/env_play/env.txt`
  has a misleading movie-skip comment: X:/none disables fallback only, while
  working-directory movies still resolve. Actual SD files/paths need checking.
- Existing GL telemetry already includes program work, first-use draw timing,
  CPU stages, GPU queries and625/384 conversion. glFinish sampling and present
  spans perturb or include waiting; audit/calibrate instead of reapplying a
  foreign timer multiplier or treating waits as useful GPU time.
- Existing source-list/internal disk-cache/binary-cache layers are distinct;
  worker versus render-thread binary reuse is inconsistent. No cache has been
  enabled or claimed repaired by this document.
- No new build, game run, hardware measurement, quality/clock/save change, branch,
  installation or package. Prior tests remain evidence only for their scope.

Broad research is now sufficient to start W0. A focused assistant is useful only
for a named remaining uncertainty with source/evidence and a falsifiable test,
not as another open-ended architecture survey.

## 26 October addition — Bejeweled CPU optimization guide

the supplied [optimising-functions.md](https://github.com/ChanseyIsTheBest/bejeweled3-nx/blob/b4ca88954303da539f2e034fd81e50538fbb8d7e/docs/optimising-functions.md),
pinned at `b4ca88954303da539f2e034fd81e50538fbb8d7e`. Full19426-byte text and
source hash retained under `logs/tcs360-project-harness-20261026/mesa005/references/`.
The text is reference evidence, not installation or rewrite authorization.

Applicable to existing C1: profile the actual Switch workload using exact
binary/symbol identity, identify hot functions and compare equivalent work.
Local PPC context registers, changed-only writeback, inlining small hot callees
and native fast paths can reduce measured overhead when register, memory,
callee and floating-point semantics are preserved. TCS already contains native
CRT/math and recompiler work; audit existing mechanisms first. Differential
checks need controlled side effects for nonleaf functions; do not wave away
mismatches as races without evidence. Code size and copying may erase gains.

Do not transfer another game's offsets, reported gains, blanket volatile/
barrier removal or broad code-generation changes. These are candidates requiring
TCS-specific evidence and semantics, not a renderer choice or proof of60FPS.

005 logs contain stage timing, not PC/function samples. Switch's existing
SuspendThread/GetThreadContext shims return failure, so its Windows-style sampler
cannot be enabled as if functional. B requested SCHEDPROF, but its report only
used dbg_ram with TRACE absent. The existing stdout sink now also receives that
five-second summary; six before/after host combinations and full-source ARM
compile pass. Hardware CPU-hook accuracy and integrated use remain unverified.
This repair will join a substantive candidate rather than create another test.

005 data changes ordering within the harness: B's eight large replay stalls
spend80.8% in texture decode/upload and report no new program builds; existing
warmup accounts for148 settled programs. Investigate R1 preparation/visibility
before speculative guest-function rewrites or binary-cache enablement. Large
completion/presentation waits remain unattributed. No reference project's
performance claim substitutes for an equivalent TCS hardware comparison.

## 26 October addition — optional SaltyNX overlay telemetry

Reviewed the full [console overlay guide](https://github.com/StevensND/nfsmw-nx/blob/be2d4c67ab42d6d629746a01dbf03cef824c8d2d/docs/console-overlays.md)
and [template README/header/implementation](https://github.com/StevensND/nfsmw-nx/tree/be2d4c67ab42d6d629746a01dbf03cef824c8d2d/extras/saltynx-template),
pinned at `be2d4c67ab42d6d629746a01dbf03cef824c8d2d`. Exact four source files and SHA256 manifest are retained in
`logs/tcs360-project-harness-20261026/mesa-overlay-reference/`. These are reference
material, not installation authorization or an instruction to change009.

Relevant as optional visible telemetry: publish FPS/resolution into SaltyNX's
shared page for a compatible overlay. Template requires SaltyNX0.7.0+ and libnx;
its default graphics API is Vulkan3, whereas our Mesa path needs OpenGL2.
Heartbeat writes mapped data after presentation; Update supplies summaries.
This exposes measurements, not a renderer optimization or a corruption repair.
Existing detailed logs remain necessary for cause attribution.

A future bounded adapter should reuse existing timing/presentation boundaries,
count only successful swaps, and cover both direct eglSwapBuffers and GLPIPE's
actual worker swap, not queued jobs or guest frame publications. Swap cadence
is not measured panel scanout or guest simulation speed. Publish meaningful
output/internal dimensions separately rather than assume they are identical.
Validate displayed values against logs and compare equivalent play with telemetry
off/on; TCS overhead and console compatibility remain unmeasured.

Do not import the whole template blindly: it also publishes ReverseNX mode
controls, retries optional ports, scans mappings and may raise session limits
or temporarily release/reinitialize the service manager. No-clock-control code
is present in these two files, but the guide's separate sys-clk integration is
not part of the telemetry candidate. Preserve handheld/stock behavior; no
ReverseNX/sys-clk policy changes. Any adapter needs explicit lifecycle, bounds,
thread ownership and graceful-absence handling; pointer atomics alone do not
serialize all publisher state. Check reuse licensing before copying implementation.

Recorded as deferred measurement work. No game code/build/env/staged artifact
changes or installation.009's first-invalid-input trace remains the primary gate.

## 26 October addition — eight more Xbox port references

All eight READMEs and available release metadata are pinned below. Targeted
engineering-note sections and source bodies were read for the mechanisms named
here; this is not a complete audit or reproduced hardware validation of these
repositories. Exact tree inventories, downloaded text/source hashes and review
scope are retained under `logs/tcs360-project-harness-20261026/mesa-community-references/`.
Historical prompts, CLAUDE.md entries and install recipes were read as reference
material only. No tools/dependencies/binaries/shader packs were installed.

| Project / pin | Relevant evidence and limits for TCS |
| --- | --- |
| [Feeding Frenzy](https://github.com/ChanseyIsTheBest/feedingfrenzy-nx/blob/68df7bfda805696146eb4f62dceec443430c9789/README.md) `68df7bfda805696146eb4f62dceec443430c9789` | Renut/Deko and profile-selected native functions with checked register/store semantics. The documented first/release runs inspected are Ryujinx, so their timings do not establish handheld stock-clock TCS performance. Small shader catalog and different game work. |
| [Feeding Frenzy2](https://github.com/ChanseyIsTheBest/feedingfrenzy2-nx/blob/2dd2edc040be48ecfaf4ddf6e27841842b153076/README.md) `2dd2edc040be48ecfaf4ddf6e27841842b153076` | Same renderer family; controller identities/connection generations and assignment UI are useful W4 co-op references. CPU notes distinguish reduced hot-function samples from unmatched whole-frame FPS. Inspected performance runs are emulator runs. |
| [USF4 v0.8.1](https://github.com/Thorhax/usf4-nx/blob/30ffae024fc30856bebe63a421221e35b5e2d9ea/README.md) `30ffae024fc30856bebe63a421221e35b5e2d9ea` | Deko/Renut guide, paired renderer comparison, native thread sampling, shader job priority and attachment-size correctness. Published60FPS requires raised clocks; the detailed comparison also uses overclocked memory. Frontend/renderer/API change together, so it is not an isolated API-speed test. |
| [Shaolin Monks](https://github.com/Thorhax/ShaolinMonks-NX/blob/e645fc7f6f896a1e0f9cb53d57c016e5b15fc9f0/README.md) `e645fc7f6f896a1e0f9cb53d57c016e5b15fc9f0` | Original Xbox x86 static recompilation, D3D8-to-Vulkan/NVK, rather than360 PPC/Xenos. Actual audio output pulls its mixer from audout completions; this is useful scheduling contrast, not a drop-in replacement for360 callback/credit semantics. |
| [Bejeweled3 v1.0.0](https://github.com/ChanseyIsTheBest/bejeweled3-nx/blob/2ad9f6f496bd1a3b1a188a6409c351ad3befb76c/README.md) `2ad9f6f496bd1a3b1a188a6409c351ad3befb76c` | Renut/Deko, CPU profile-driven optimization and differential checking. Notes show visually simple content can spend substantial CPU on a software rasterizer. A Quest-pointer guard is explicitly a mitigation with the invalid writer unproved; do not copy as a root-cause fix. |
| [Bakugan](https://github.com/Wiredlainn-afk/Bakugan-NX/blob/77ac43bfeb2381b6cf01109cc13992a4e6575076/README.md) `77ac43bfeb2381b6cf01109cc13992a4e6575076` | Earlier NVK CPU/audio/movie source and performance history are useful; README now describes Renut/Deko. Published tree at this pin does not contain the advertised app/src/renut_engine implementation, limiting current-backend audit. History and latest release defaults differ (resolution/DOF): do not blend measurements. |
| [WOTW](https://github.com/JohnJames29/WOTW-NX/blob/8e592ac3cb9c0cf7c2de2b2cf7fd90233c26613b/README.md) `8e592ac3cb9c0cf7c2de2b2cf7fd90233c26613b` | Original Xbox, README describes x86-to-ARM64 JIT. The complete public tree at this pin contains only README.md. No source/test suites are available there to verify optimization claims or extract fixes. |
| [Peggle v1.0.0](https://github.com/ChanseyIsTheBest/peggle1-nx/blob/1365bff8a5eb4e189e453bc298e692d2cd1ecb61/README.md) `1365bff8a5eb4e189e453bc298e692d2cd1ecb61` | Renut/Deko, actual empty-worker-event repair and shared queued snapshots/fenced upload retirement. Lightweight workload; its decision not to use LTO/PGO is not a general compiler recommendation for TCS. |

### Concrete mechanisms mapped into the existing packages

**W1 BO ownership remains first.** Peggle's actual `backend/video.cpp` calls
WaitSlotLocked before DrainSlot; retired views/textures/buffers/sets and upload
storage are reclaimed after the dependent fence. UploadRing::Reset itself does
not wait: callers own that invariant. TCS already has arenas/fences; trace actual
references and first mutation across them rather than add a second retirement
system. None of the inspected current paths provides a demonstrated fix for
our specific libdrm_nouveau null-device crash. Do not infer the invalidator from
a shared symptom or adopt a null guard. Guest-object refcount fast paths in
Bakugan are a different domain from native Nouveau BOs.

**W1 visual inputs / later W3 constant cost.** Peggle's `backend/queued.cpp`
retains full or delta device snapshots and has a full-copy reconstruction check.
Its narrow comparisons depend on the exact game device layout and documented
consumer fields. That supports keeping an independent complete-input reference
when optimizing constants; it does not prove TCS's existing reuse correct.
Copied texture-write hooks in the PopCap family retain other-game names/addresses;
their presence alone does not establish that they execute in a given title.
TCS needs actual write/capture/replay ordering and first bad bytes.

**W1/W3 scheduling and audio.** Peggle's worker source establishes the empty
queue/manual-reset-event invariant: after an empty satisfied wake it clears the
event, rechecks the job count and waits again. Retain the recheck and producer
ordering to avoid lost work; never transplant its return address or insert an
arbitrary sleep in TCS. Bakugan's sound/limiter experiments include audible
regressions. Keep our existing audio thread; trace completion -> credit ready ->
guest admission -> callback -> submit/device empty before priority, buffering or
cadence policy changes. Shaolin's pull-mixer model does not supply our guest
mixer results or license changing game speed.

**C1 / W3 native CPU attribution.** USF4's `sdk/src/ui/switch_perf.cpp` uses owned
thread handles, svcSetThreadActivity, svcGetThreadContext3 and a bounded stack walk.
This is a concrete reference for a native Switch sampler where our Windows
SuspendThread/GetThreadContext stubs cannot work. Sampling pauses threads; the
guide reports roughly a core of overhead at1kHz. Before reuse verify syscall
availability, lifetime-safe handles, resume-on-error and sampling exclusions.
Keep any such attribution run short and separate from timing runs; reuse existing
thread registration/reporting. No sampler was implemented or enabled here.
The existing hot-function differential-check objective remains; no blanket
recompiler regeneration, register-localization switch, LTO/PGO or function rewrite.

**M movie correctness.** Bakugan's WMV3 bridge matches compressed input bytes,
checks/copies plane strides and offers a frame-by-frame shadow comparison.
That is a useful independent decoder oracle/fallback pattern, not evidence our
WMV codec, borders, pointers or callbacks match its offsets. Diagnose TCS's first
wrong boundary; retain natural EOF/input and full movies. New decoder dependencies
would require approval; existing FFmpeg capability must be audited first.

**Do not adopt lossy freshness assumptions.** USF4's linear-texture optimization
explicitly allows unobserved writes to appear up to16frames later. With our
one-frame corruption unexplained, preserve strict visibility. Its attachment fix
also demonstrates emulator-versus-Maxwell behavior differences; emulate success
cannot close our hardware gate. Bejeweled's guarded pointer does not identify its
first invalid write. No quality/clock/movie-skip policies are imported.

### Architecture implication and next gate

Six READMEs describe a Renut/Deko route. These are related ports of a shared
Direct3D-hook renderer, not six independent drop-in GL-to-Deko conversions. Five
public Renut trees have the same upload_ring.cpp Git blob; game hook/state layouts
and their queued.cpp versions differ. Source reuse is real, and a future Deko
proposal should evaluate this mature shared implementation before extending a
bespoke backend. It would still require a TCS hook/device-layout/tiling/resource
census, semantics coverage, adoption cost and reuse rights (the USF4 guide records
permission-specific reNut reuse). This evidence does not establish TCS compatibility
or authorize a frontend replacement. Existing Mesa-first decision stays; Deko
remains inactive. Avoid repeatedly fixing symptoms, but do not replace architecture
solely because another game reports60FPS.

Immediate gate unchanged: establish the first invalid BO ownership/reference/
heap/queue transition, repair that shared contract with focused regression, then
integrated rendering acceptance.009 is not requested again. Later measured combat
profiling now has a specific native sampling reference; W4 co-op has an input
identity reference. No new code/build/staging/config/clock/quality change.

Review checklist4/4 complete: eight reference identities/READMEs; targeted
implementation checks; TCS applicability/limits; current harness/register update.
This is100% of the bounded review only. W0 complete -> W1 correctness active ->
W2 startup -> W3 gameplay -> W4 content/co-op -> W5 acceptance. BO, visual, audio
and full-port repair effort remain unknown; no new hardware acceptance passes.

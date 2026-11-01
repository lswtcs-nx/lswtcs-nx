# TCS360 execution harness — Mesa first

**Revision 2 — 23 October 2026.** the project owner requested a Mesa-or-Deko decision and a
full execution harness. Mesa/OpenGL is now the main implementation path. This
revision supersedes the Deko-first work queue. The filename stays unchanged so
existing AGENTS/handoff links continue to identify one canonical harness.

This file is a complete replacement prepared for the project docs directory.
If project writes remain unavailable, the conversation's Mesa-first direction
still governs this session; do not pretend the old on-disk handoff was updated.
The preserved previous harness/handoff are historical evidence, not another queue.

## 1. Outcome, decision and boundaries

Deliver a reliable full TCS port on the original Switch: ordinary movies and
transitions, correct graphics, responsive controls/audio, existing native per-user
saves and local two-player play. Preserve HD content and the agreed quality.
Clone Wars III follows TCS acceptance and does not expand this project now.

**D1 — improve the working OpenGL/Nouveau Mesa renderer first.** Keep the
recompiled game, PM4/Xenos recorder and existing GL bridge. Mesa-first does not
mean a new NVK/Vulkan renderer, a global toolchain update or installation of a
different graphics stack. The opportunity cost of finishing Deko is not yet
justified by a fair TCS hardware comparison. Choosing Mesa now is a delivery
decision, not proof it is the fastest possible final backend.

**D2 — preserve Deko, remove it from the active queue.** Keep its source, shared
semantic work, AOT shader compiler/cache identities, captures, tested package and
unfinished gates. Do not delete it, bind its memory observer, continue its
production EDRAM migration or request Deko tests as part of Mesa work. Existing
shared fixes may be reused when their Mesa behavior is independently verified.
The historical Deko W1 foundation is incomplete; its effort remains unknown.
Its H1 cold-launch acceptance remains0/3; no playable live Deko release is claimed.

**D3 — fix first invalid producers and shared contracts.** Movie pixels, guest
PCR overwrite and Mesa/nouveau BO deletion have separate evidence chains until
an actual trace connects them. No arbitrary address/frame/scene/shader exceptions,
null guards that conceal corruption, guessed PCR relocation, movie suppression,
missing valid draws/textures or blanket instruction/codegen rewrites.

**D4 — evidence before optimization.** Separate compilation/link, driver first
use, game CPU, PM4 recording, texture/upload work, GPU work and presentation/waits.
Choose the largest demonstrated removable cost. A driver/API change does not
repair unrelated guest-memory or scheduling faults. Correctness repairs need not
show a speedup; performance-only complexity must earn its maintenance cost.

No quality/resolution/effect, game-speed, clock or frame-cap changes are approved
as optimizations. Preserve current settings. The existing60FPS cap is not a claim
of achievable60FPS. Use16.67ms as its budget when analyzing that target; propose
any30FPS cap or other real product tradeoff separately with measurements.

## 2. Verified starting point and immediate hazards

| Item | Evidence / limit |
| --- | --- |
| Tested Mesa004 | the project owner confirmed title/gameplay, native saves and separate users. NRO114763973bytes, SHA256 `8aaaf05a5a7b9dcaa55b38e51f0a5159f4693aa2d611e5364388466e7836a95f`. Hash rechecked23October. Not full-game or performance acceptance. |
| Matching004 symbols | ELF SHA256 `3fde7343e20573ec691c16337c6e8200166a81b0ba3d5f53096cd850391db1aa`; map `4cf9d978d15ee499e2d3272a34e51e572d8541356cb36e7a57c7468ea42c7a58`. Rechecked; exact files under scratch `logs/hardware-004/`. |
| Working checkout | Same `codex/tcs360-build-compat` branch, upstream base92ec175285504455777ccd14d2d0dcf19642694d, substantial local changes. Current source is not the tested004 source merely because both build. |
| Current build tree | `switch/build-repro-make` has `LSW_DEKO_BACKEND=ON`. Preserve its633 GameCode objects/PCH. Audit an existing compatible Mesa recipe/output before changing configuration; never casually reconfigure this tree and force regeneration. |
| Current Mesa baseline/candidate |012 recorded draw-identity correction compiled/337local cases/static/link/staged,4/5 gates, hardware unrun.011 BO negative observation did not capture invalidator. Same app/env; no diagnostic marker. |
| Old play profile | `switch/env_play/env.txt` points GAMEROOT at `X:/none/` with a misleading movie-skip comment. This disables fallback lookup only; working-directory movies still resolve (confirmed in hardware003 logs). Verify actual SD files and resolved paths; this template alone proves neither full content nor movie suppression. Never set AUTOSTART in a normal hardware drop. |
| Current driver | Local build script pins patched Mesa20.1.0-rc3, including8 local edits. Most new comparison ports use much newer Mesa NVK/Vulkan. Their flags/NAK patches are not interchangeable with this GL/Nouveau stack. |
| Historical hitches |003:17 large shader stalls,18279ms wall/17591ms program work in that bounded log; later windows about35ms without new programs. Different runs/clock conditions and capture scope limit extrapolation. This is not a current stock-clock baseline. |
| Mesa caches | Existing Mesa internal disk cache, saved `glprogs2` GLSL list, session stage/program maps, and optional app program binaries are different mechanisms. Binary cache defaults off after a documented old slowdown/corrupt-output experiment. That failure has not been reproduced in this revision. |
| Concrete cache inconsistency | Workers do binary-load/build/store; render-thread help and zero-worker paths call build directly. Fix one preparation operation across consumers if using this cache, with correct GL context/synchronization ownership. Source warmup is not proof of retained native code or first-draw readiness. |
| Existing measurement | `GLPROF`, `GLSTALL`, program counters, sampled `GLGPU` and delayed `GLSPAN` exist. Reuse/audit them before adding telemetry. GPU conversion already uses625/384; do not blindly apply another port's1.627 correction again. Heavy GPU profiling uses glFinish and disturbs its sampled frame. |
| Independent defects | PCR0x88010000 overwritten with allocator domains overlapping; first writer unknown. WMV picture corrupt/hang on ordinary title idle. BO deletion sees null device at unmap; invalidator unknown. Keep each defect open until its own evidence closes it. |

Tested artifacts remain unchanged. Native-save confirmation is later evidence
than the package's original static-only build-info; preserve both provenance
and the owner's subsequent confirmation without rewriting history.

## 3. Work packages W0–W5 and acceptance

These are **revision2 Mesa-first scopes**. Earlier Deko W0/W1 results remain in
the archived revision1 plan and do not count as new Mesa gates. H1 still means
title/menu, and H2 gameplay/saves; neither means full-game completion. Work on one
primary integrated package. Interleave a bounded independent repair only when
it unblocks that package or prevents wasted owner testing.

### W0 — restore a reproducible Mesa baseline and classify cost

Checklist (not an estimate of remaining effort):

1. Pin/preserve the tested004 NRO, ELF/map and saves/account behavior. **Done**:
   hashes above rechecked. Never overwrite the only known-working package.
2. Audit inherited code/config/build differences, driver/cache paths, content
   inventory and available telemetry. **Initial audit done**: concrete hazards
   above;26October owner reports handheld at stock clocks for both005 runs.
3. Establish an exact reproducible Mesa candidate recipe from approved sources
   plus explicitly selected shared fixes. Reuse compatible objects/PCH; dry-run
   the build first. If a separate backend build directory is necessary, document
   why and reuse only ABI/flag-compatible artifacts. No new branch or SD app.
   **Done for005:** isolated tracking-off build, qualified object reuse, component
   dry-runs, strict full build/static checks,4992 input fingerprints and verified
   three-file staging payload. It is not a bit-for-bit004 reconstruction.
4. Obtain one bounded stock-clock full-content baseline session: boot/menu,
   cantina, the same saved EpisodeI segment, transition/save/reload and natural
   title-idle movie. Record cold-launch versus shader-cache-cold versus warm.
   Prefer existing004 diagnostics if verified available; add only missing data.
   **Done26October:** owner006 cantina/EpisodeI Chapter1/repeat/native save/
   reload/clean exit and separate natural idle-movie run received on handheld/
   stock. The movie FAILED; this records baseline coverage, not W1 correctness.
   Complete effective B env received byte-exact. Exact battle/visit timestamps
   remain unknown, so no numeric matched-scene speedup is claimed. Caches retained;
   fresh app and first/repeat visit are not isolated disk-cache-cold conditions.

5. Publish route-local bottleneck ranking and the next experiment's hypothesis,
   expected observable effect and keep/reject rule. Record unmeasured items as
   unknown, especially GPU time when only fence/presentation waits are known.
   **Done on26October:** decode/upload is80.8% of B's thresholded large-replay
   stalls; existing warmup covered B's programs. Steady completion/present wait
   ownership remains unknown. Next R1 experiment is duplicate-preparation audit
   with identical pixels/required uploads and bounded memory as its keep gate.

**Current W0 checklist:5/5 complete,100% of baseline data-collection gates, not
game completion.** Exact build/settings, named stock route, known failed movie
and ranked next action are recorded. Movie/audio/visual repairs remain W1/W3.
**Exit:** exact runnable Mesa artifact/profile, baseline logs with known timer
semantics and one ranked next action. No broad research or rewrite is a gate.

### W1 — stable runtime, movies and GPU lifetimes

Resolve release-blocking failures as owning-system contracts, not separate
per-scene patches. Use the earliest reproducible invalid transition to order work.

- **Guest memory/PCR:** audit allocator reserve/commit/free, static/thread owner
  ranges and aliases; reject conflicting claims before writes. Trace the first
  invalid store if ownership evidence does not identify it. Validate host thread
  -> guest thread -> r13 -> live PCR at lifecycle boundaries. Preserve the port's
  actual PCR/KPRCB contract instead of importing another SDK's offsets.
- **WMV:** align decoder-ready bytes, upload input, texture interpretation and
  displayed pixels for the same frame. Verify planes, pitch/endian/format/bounds,
  ring resets/writeback, callback/event completion and movie end/input recovery.
  Existing327-function/VMX tests are reusable evidence, not proof of valid pixels.
  Separate loose WMV playback from in-engine chapter cutscenes.
- **Mesa BO crash:** identify buffer/reference lifecycle and the first invalid
  release/write using exact004/appropriate candidate symbols. Audit shared GL
  contexts, submissions, retirement and destruction order. Reuse patched Mesa
  source, existing crash evidence and bounded instrumentation. Do not conceal a
  null device or assume PCR corruption caused this native failure.
- **Progress/exit:** validate waits, wake-up and lock order, idle rendering,
  decoder/thread exit and GPU drain/context cleanup. No arbitrary sleeps or
  releasing all guest critical sections based on another port's workaround.

Every repair needs a reproducer or captured invalid transition, an appropriate
focused regression/negative control where feasible, and the actual integrated
route. A non-reproduced failure remains an explicit unresolved risk; several
clean trials alone do not prove its cause fixed.

**Exit:** identified owning causes repaired;3 cold launches,3 natural movie
cycles, repeated affected transitions and at least one30minute mixed play/exit
session complete with correct picture/audio/input, readable existing saves and
no observed recurrence. These are finite planned acceptance samples, not a
guarantee against every rare crash. Record residual risk and coverage honestly.
Use a substantial owner checklist once local evidence warrants it. Do not block
a small independent cache investigation on unrelated all-game completeness.

### W2 / Mesa H1 — shader readiness and smooth ordinary startup

Implement the project owner's already-approved hybrid objective using existing mechanisms:
common boot/menu preparation, variants discovered/prepared during relevant
loading, and persistent compatible reuse where the driver supports it.

1. Measure internal Mesa cache hits/effective reuse, GLSL source recompilation,
   stage sharing, binary format support and first-draw backend work independently.
   Confirm actual paths/flags for our Mesa version; NVK cache advice is not GL
   configuration advice.
2. Consolidate the current worker/render/synchronous preparation contract, job
   claim/publication/failure behavior and statistics as necessary. Preserve valid
   draws and GL context ownership. Reuse current queues/maps rather than adding
   a second scheduler/cache framework. Resolve the prior binary extraction issue
   before enabling that optional path; correct source fallback remains valid.
3. Build route coverage from actual shader modifications and VS/PS/GS pairings,
   not just raw microcode count. Use existing captures/source recordings first.
   Add a narrow loading hook only if measured misses justify it; do not reverse
   engineer the whole asset engine before the first useful improvement.
4. Validate cache identity (source/generator/driver/compiler/config), stale or
   truncated rejection, safe rebuild, persistence across normal exit/restart,
   bounded memory/SD writes and harmless read/write failure. Preserve user caches
   during diagnostics. Keep catalog coverage distinct from compiled-cache state.
5. Add progress UI only after the underlying preparation meaning is proven.
   It describes the known set, not 'all game shaders'. Skipping preparation can
   defer compilation, never skip rendering or silently reduce quality. Any new
   user-visible flow must match the existing game/approved objective.

**Exit:** three current-candidate cold launches reach animated title/menu,
physical Start/options/back, audio and clean exit; a genuinely empty isolated
shader cache and a matching warm repeat both work. Known boot-route programs
are accounted for; warm reuse and remaining first-use work are quantified.
No preparation-induced corruption, unbounded queue/memory, missing valid draw or
new long-frame regression. Compare startup, first-use hitches and gameplay
separately. No 'compile once forever' claim and no promise of zero loading cost.

### W3 / Mesa H2 — measured gameplay optimization and transitions

Use W0 route evidence, refreshed after W1/W2 changes. Select the top cost class:

| Measured owner | First candidates | Evidence required before retaining |
| --- | --- | --- |
| Shader/first-use | Remaining catalog/load readiness, correct existing persistence | Cold/warm counts/times, identical complete output and lower hitch cost. |
| Game CPU | Existing codegen audit; native CRT/math/hot functions, register/call overhead only where sampled hot | Matched Switch stacks/cost per call, memory/register/rounding equivalence and integrated behavior. |
| PM4/GL CPU | Duplicate state/lookup/constant work; dirty ranges; vertex/index upload dedupe and bounded streaming | Actual work counts/bytes, same draw inputs/order, lower relevant CPU time beyond noise. |
| Texture/SD | Repeated decode/upload/allocations, unnecessary cache writes, read reuse | Correct invalidation/aliases, immutable inputs, hit/miss attempts, lower bytes/time and bounded residency. |
| GPU | Redundant full copies/resolves, shader work/bandwidth and proven safe reuse | Semantics/capture comparison, required CPU readers preserved, validated timing, no skipped effect. |
| Waiting/pacing | Queue contention/backpressure, delayed submission, frame completion vs display timing | Wait/wake trace and completion identities, no lost wake or simulation-speed change, p95/p99/latency improvement. |

Check what is already optimized before adding code. Keep the current renderer
structure unless a measured shared bottleneck demands a narrow change. Do not
change texture checking frequency, omit small readbacks, repurpose render targets
or dedupe buffers based only on addresses/frame age. Do not remove volatile,
atomics or guest access barriers without proving all writers and synchronization.

For a CPU hot function, a game address identifies the function; it is not a
license for an address-specific behavior exception. Preserve calls, registers,
memory ordering and PPC floating-point behavior. Differential checks must control
side effects and races; do not double-execute irreversible native calls.

LTO/PGO/function ordering are candidates after hotspots/routes are understood and
reproducible. Audit current compiler options first. A deliberate whole-generated
rebuild is justified only by a measured candidate and preserved rollback—not a
default step. Keep max2 compile/LTO jobs. Train PGO across boot, gameplay, movies,
transitions and co-op; do not claim one scene's training is universal.

**Exit:** physical gameplay through cantina/combat/camera, a representative
chapter, transition/return, save/exit/reload under the same user; repeated route
shows useful improvement against the qualified baseline with unchanged quality,
no correctness/audio/input/latency regression and bounded resources. Publish
route-local p50/p95/p99 and hitch counts, not only mean FPS. H2 is this route,
not all chapters or a finished game.

### Implementation register — concrete Mesa work inside W0–W5

23October owner request: develop the complete Mesa improvement plan and begin
implementation; logging during ordinary play is desired if its cost is modest.
This register expands the work packages above, not a competing plan. It lists
the candidates worth examining, not changes to enable all at once. No speedup
or completion date is promised before the TCS route evidence exists.

**Immediate order:** W1/R1 recorded draw identity.011 BO diagnostic delivered
a negative active interval; no first invalidator, no repeat logger test.012 fixes
reproduced tile-key omissions using existing key/arena/bridge schema (all uploaded
spans/bytes, state/topology and non-tile system constants); unsupported keys never
authorize merge.337 local cases/ARM/static pass, same app/env staged,4/5(80%)
candidate gates; physical visual/cost acceptance remains. One NRO-only5–10minute
Chapter1 route. Full resident geometry, earlier walk-time copying/texture/resolve
inputs remain outside this key; no screenshot/BO/FPS fix claimed. See current
handoff/README. No new framework or quality/clock/speed change.

| ID / package | Implementation and existing reuse | Local and hardware keep/reject gate |
| --- | --- | --- |
| L1 / W0 | Keep current frames.log and buffering. Separate timing rows from optional matrix probing and camera-history copies; retain explicit camera dump/capture requests. First change is in gl_backend.cpp, no new logger/thread/service. | Exact timing columns preserved; quiet mode needs no camera guest-memory reads; camera opt-ins still work. Actual Switch syntax and host behavior checks, then qualified Mesa build and matched on/off cost measurement. No zero-overhead claim. |
| A1 / W1,W3 |007 repairs existing audio output readiness: classify all six device slots after fill, predicate on pending client frames/hold under existing mutex/CV, synchronize hold publication.1152 slot-state and200 real-thread checks reproduce/fix lost submit/resume and later-slot waits. No extra mixer/thread/buffer/latency/clock change. | Integrated/static pass;007 physical route saved/exited normally but owner heard no improvement. Audible-benefit gate failed; producer/service deadline attribution remains. Correct credits/holds/PCM and audible continuity, no latency/control regression. Old idle-wait count included false cases; lower count alone is not an audible gain. |
| S1 / W2 | Audit one load/build/publish contract across compile workers, render-thread help and zero-worker fallback. Current worker path alone attempts optional binaries; duplicated queue entries can outlive a claim. Reuse current queue/maps, establish unique ownership of an in-flight content key and consistent failure/wakeup/accounting. | Exercise duplicate/urgent job claims, failure and zero-worker cases. Validate actual shared GL context rules before changing binary extraction/publication. Every valid draw must remain rendered; no cache enabled merely because it links. |
| S2 / W2 | Existing Mesa internal cache plus glprogs2 catalog provide the starting point. Verify effective disk-cache location/identity, binary formats, source/generator/driver/config keys, bounded record lengths, truncated/stale rejection and durable writes. Resolve prior bad binary-cache result before enabling it. | Genuine isolated-cache versus matching warm route; account separately for source reuse, binary hit, compilation/link and first-draw backend work. Failure rebuilds safely; no corrupt output or growing memory/SD traffic. Preserve production caches. |
| S3 / W2 | Prioritize common boot/menu programs and known required variants during existing loading. Use actual VS/PS/GS combinations and existing saved sources, not a new asset-engine scanner. Keep urgent jobs ahead of unrelated background warmup and respect audio/guest scheduling. | Boot/loading cost, first-use holds and tail stalls improve without hiding work in gameplay. Progress UI comes after its denominator is real; interruption is safe and never suppresses a draw. Do not claim the catalog covers all shaders. |
| P1 / W1,W3 |009 one-run bounded input-reuse diagnostic through existing recorder/frame/untile/capture path. Original rendering unchanged; first unequal pair tied to serial/records/source.4096 slots/32MiB signature work per frame, explicit incomplete and one-run marker. No new observer/cache/thread. | Actual before/after draw/plan, handoff, bounds/marker/OOM and ARM/static qualification pass; physical matched trace pending. Interpret used versus unused bytes, not automatic pixel-fault proof.008 visual gate failed; total repair effort unknown. |
| R1 / W1,W3 |005 decode/upload hitches motivated006 shared sampler-independent image key, reusing current cache/decoder/sampler. Actual-cache CPU fixture15 uploads instead of30, identical pixels, different samplers and unchanged content-check frequency. Integrated/static pass, no console gain yet. Sparse guest-write coverage remains incomplete. | Physical006 picture/sampling/save/exit, bounded memory and useful preparation reduction on matched route. Broader partial writes/cross-frame aliases/CPU readers remain open. No skipped draws, scene exceptions or unchecked fingerprint shortcuts. |
| R2 / W3 | Audit full-screen copies, presentation and resolves when GPU evidence justifies them. Reuse correct attachments/views and safe regions instead of copying unchanged content. Consider equivalent shader generation/constant folding only with variant identity and output evidence. | Validate the complete image, effects, depth/stencil/alpha/MSAA and guest readback. Time useful work separately from present waits. Retain only repeatable benefit; no reduced resolution/effects, omitted valid draws or simplified substitute shaders. |
| C1 / W1,W3 | Sample the actual guest/runtime/decoder hot paths. Check existing native CRT/math/NEON, recompiler call/register overhead, allocator and lock contention. Specialize only proven expensive operations with a complete PPC/register/memory/rounding contract; preserve current audio decoder first. | Differential/negative checks for the affected contract plus real gameplay/movies/audio. Show lower relevant CPU cost beyond noise. No whole-codebase rewrite, guessed game-address condition or global fast-math option. |
| P2 / W1,W3 | Trace scheduler waits, renderer backpressure, queue depth, frame publication and completion. Reuse existing wait helpers, condition variables, fences and queue caps; change the first invalid ownership/wakeup or unnecessary serialization. | No lost wake, self-wait, unsafe lock release, stale frame publication or lifetime regression. Same game speed and bounded input latency; report busy work versus waiting. More queued frames is not automatically better. |
| I1 / W3 | Measure SD reads, shader catalog/cache writes, logging flushes and streaming allocations together. Avoid repeated unchanged writes and large duplicate serialization buffers; schedule or bound background work only where it competes with game reads. Audit existing XMA/WMV I/O and memory pools before creating new ones. | Cold/warm loading and transition tails improve; cache/save failure remains recoverable. No unbounded writer queue, new long flush on exit or unchecked resident growth. Background I/O is still real I/O. |
| C2 / W3,W5 | After CPU evidence and reproducible builds: assess current optimization flags, LTO/function layout and multi-route PGO. Keep compiler/runtime/driver changes separate; no installation follows from a plan entry. | Same behavior across boot, movies, multiple levels, transitions and co-op; measured gain outweighs code/build/memory cost. Preserve previous artifacts and max2 compiler/LTO jobs. Do not rebuild all633 units simply to try generic flags. |
| F1 / W4,W5 | Full product acceptance: movies, rare crashes, correct save dates, contextual controls/glyphs, two-player, suspend/exit and full chapter coverage. Verify performance under streaming/co-op and sustained memory/thermal conditions. | Existing saves/users remain compatible; required coverage and long sessions pass. Report where the unchanged60FPS target is met/missed. Any real quality/clock/cap tradeoff requires a separate owner decision. |

26October C1 reference: Bejeweled's pinned function-optimization guide reinforces
actual-console hotspots, exact ELF/map attribution and changed-register/memory/
rounding validation before local specialization.005 has no function samples.
The existing Windows sampler uses Switch stubs and cannot supply them. Scheduler
reports were lost to TRACE-only output; the stdout sink repair passes before/
after host checks and full-source ARM compilation. It is now integrated in006,
absent from tested005. Validate alongside006 texture behavior, without full TRACE
or a standalone logging run. See the existing reference review for limits.

**Reference lessons and limits:** NFSMW's pinned measurement protocol motivates
L1 and no-op/noise controls; its renderer reinforces P1/R1. Mass Effect's
cold-start/cache-write analysis motivates S2/I1, and its dirty-vector design
motivates P1's verification against complete immutable inputs (that particular
candidate is documented as not console-tested). Simpsons/Prototype reinforce
whole-system CPU/streaming/wait analysis. Project8 and historical Futari provide
resource/compiler examples, not permission to omit unready content or change
clocks. Sources and pins are in TCS360_MESA_REFERENCE_REVIEW.md. These mechanisms
need adaptation to TCS's OpenGL/Nouveau stack; no NVK migration is bundled here.

**Keep rule:** correctness is mandatory; a performance-only change must improve
its owning cost beyond matched-route noise without worse startup/gameplay tails,
input latency, memory or image completeness. When no benefit is shown, remove
the candidate rather than leave a dormant framework. If one metric trades off
against another, record the numbers and ask for a concrete product decision.

### W4 — complete content and product behavior

Maintain one coverage register, one row per meaningful route/feature and evidence:

- All six episodes/chapters and significant bonus/free-play modes; ordinary WMV
  movies and in-engine cutscenes; effects/streaming paths actually encountered.
- Existing save slots readable; new progress correct after restart; native user
  isolation; correct slot dates traced through file/time metadata; safe no-space
  or write-failure behavior. Never delete saves or silently change user/storage.
- Approved context-sensitive controls: A confirms/B returns in menus; B jumps/A
  performs special actions in gameplay. Audit glyphs/tutorials/dialogs and visible
  Xbox wording; avoid a global button swap or internal API-name replacement.
- Local two-player with Pro Controller and paired Joy-Cons first: independent
  movement/input/player identity, join/drop, reconnect, chapter completion and
  shared progress/save reload. Sideways Joy-Cons later. No online expansion.
- Brightness/gamma and stereo routing verified; report docked HDMI5.1 feasibility
  instead of assuming the surround menu works. Virtual surround/new audio modes
  are separate product choices, not invisible optimizations.
- HOME/suspend/resume/controller disconnect, app exit and native user selection.
  HOME launcher/package uses the intended app/save identity; installation or
  packaging behavior needs the project owner's review before touching the console.

**Exit:** the coverage register closes required routes with real evidence and
explicit known limits; two-player progression/saves pass, repeated transitions
and long play do not show continuing resource growth or the known failures.
Unknown coverage stays unknown. Do not infer all chapters from one test scene.

### W5 — sustained performance, release and renderer decision

Measure qualified representative workloads in handheld and docked modes at
recorded official clocks, unchanged visual settings and full content. Preserve
baseline/candidate binaries and logs. Report cold setup, warm play, streaming,
movies, co-op and long-session behavior separately. Report thermals/power state
when available with existing facilities; install no monitoring tool silently.

Final acceptance requires the stable W1/W4 behaviors plus measured, reproducible
performance at the agreed target—not a cross-port FPS comparison. State precisely
where60FPS/16.67ms is met and where it is not. If a target is unmet, report the
remaining cost and seek a concrete product decision; never silently cut quality,
raise clocks or slow the game to obtain the headline number.

**Deko/NVK reopening gate (may be evaluated earlier if W3 proves a hard limit):**
identify a dominant graphics/frontend/driver cost after the easier safe fixes;
bound the benefit and implementation/maintenance cost; compare a thin reuse
candidate using existing captures at equivalent semantics/quality. Preserve
Mesa fallback. A switch needs an explicit decision and integrated acceptance;
one faster frozen frame is insufficient. CPU/runtime corruption is not evidence
for switching APIs. No open-ended dual-renderer implementation in the meantime.

## 4. Test and optimization protocol

### Routes and measurement definitions

| ID | Route | Purpose |
| --- | --- | --- |
| B | Launch -> animated title -> Start/options/back -> clean exit | Startup/readiness, controls/audio and shutdown. |
| G | Same-user saved cantina loop plus the same EpisodeI segment | Representative movement/combat/camera; compare matching intervals. |
| T | Chapter transition/return -> autosave -> exit/reload | Streaming, resource retirement and saves. |
| M | Natural title-idle WMV start/play/end/input recovery, repeated | Decoder/submission/thread lifecycle; full loose movies present. |
| C | Two-player chapter/join/drop/reconnect -> progress reload | Co-op CPU/input/streaming and save correctness. |

A **cold launch** restarts the app. A **shader-cache-cold** run uses isolated empty
application and driver caches while retaining the installed catalog; a **warm**
run retains both and repeats the same route. Neither means deleting user saves
or production caches. Record which layers were isolated and verify the flag/path
actually acted. Do not hide a warm driver cache under a 'cold' label.

Record NRO/ELF/map/source/driver/config/cache identities, asset layout, launch
mode, console/power mode, actual clocks, route/user/starting save identity and
logger settings. Preserve originals; use an isolated save copy for desktop
automation. Do not overwrite the user's progress to manufacture a benchmark.

Reuse existing profilers. First audit attribution, overlap and publication.
Required quantities where available: frame cadence p50/p95/p99/max; frames over
16.67/33.33/50/100/500ms; shader counts/compile/link/first-use/cache hits and writes;
per-thread busy CPU or sampled stacks; draw/state/upload/decode counts/bytes;
queue depth, resident/pending-retirement memory and SD work. Log why a metric is
unavailable rather than inventing it. A CPU fence wait is not GPU execution time.

Do not sum overlapping CPU/thread times as wall time. GPU timestamp spans can
contain present/vsync waits; a full-span timer is not proof of continuous useful
GPU work. Validate units and boundaries against the actual driver and wall clock.
Existing625/384 scaling must not be applied twice. Heavy glFinish/timestamp-all-
draw diagnostics run in a separate labeled window; exclude their perturbed frames
from ordinary performance claims. Keep `LSWTCS_GLGPUSPLIT=0` and other omission/
frozen-state probes off. Do not use visibly incomplete frames as a success test.

### One experiment, one decision

Before editing, write a short entry in the existing feature register:

1. Player-visible problem, owning shared contract and evidence/log identity.
2. Hypothesis, measured cost and why the simpler existing path is insufficient.
3. Smallest change/reuse candidate; correctness invariant; expected observable
   improvement; known limits and rollback.
4. Appropriate focused check, integrated route and keep/reject condition.

Validate locally first for correctness where possible; emulator results do not
establish console timing, native saves or physical controls. One owned measured
process at a time. No Ryujinx work without verified working prerequisites and an
actual question it can answer; no keys/firmware/emulator installations now.

For performance A/B, use a matching route and stable scene/save, compare A/B then
B/A when effects are small, and use a no-op control to estimate noise if needed.
Only use an in-process toggle when switching is safe and changes one behavior;
otherwise use sequential candidates in the same SD app. A paused scene can help
GPU/image analysis but is not the moving-game CPU benchmark. Record rejected and
neutral results; no repeated probe without changed discriminating evidence.

Keep a performance change only if its relevant cost improves beyond measured
variation without material p95/p99/latency/correctness regressions. Remove a
failed speculative path after preserving the result. Neutral changes are kept
only for a demonstrated correctness or simplification benefit. Do not grow a
permanent forest of tuning flags; remove diagnostic-only scaffolding once its
decision is closed. If evidence cannot distinguish the current hypotheses,
redesign the measurement instead of patching another symptom.

### Owner test contract

Request hardware testing only after the candidate is concrete and locally
qualified, or for a specific hardware-only uncertainty. One substantial checklist
per milestone, normally10–20minutes; long-session/full-chapter acceptance is
separate and labeled. Supply exact identity, changed behavior, copy locations,
full-content prerequisite, expected screen/actions, effort, log paths, known
limits, rollback and the decision each step resolves. A failed or truncated log
is not a pass. Never silently repeat a closed diagnostic.

## 5. Research incorporated, not an execution dependency

Read pinned primary sources under scratch
`logs/tcs360-project-harness-20261023/mesa-references/`; detailed findings in the
adjacent `TCS360_MESA_REFERENCE_REVIEW.md`. Five additional repositories plus the previously
reviewed Simpsons port cover multiple Xbox360/Mesa projects. Futari's NVK path
is explicitly historical; its current release uses Deko. Mega Man is a separate
OpenGL/Mesa reference, not another Xbox360 game. Many share renderer ancestry;
they are not independent A/B proof of an API's advantage.

Transfer: honest console profiling, compiler/cache identity and coverage,
bounded persistence, reduced duplicate state/constants/uploads, checked CPU
hot paths and correct wait/lifetime behavior. Do not import their addresses,
timestamp constants, clocks, reduced effects, skipped-draw policies or partial
guest-read assumptions. TCS's proven required semantics remain authoritative.

No broad research assistant is needed to begin W0. A later focused external
review must name one unresolved question (e.g. the exact GL binary/context race
or first allocator conflict), provide pinned source/evidence, propose a falsifier,
and return applicability/cost/limitations. Research text never authorizes installs
or becomes a competing work queue. the project owner may supply such a review if useful.

## 6. Execution, preservation and checkpoint rules

- Owner does not make manual SD NRO/env backups. Agent maintains rollback
  through existing PC source/version history, preserved tested builds and
  recorded configs. Do not add per-test SD backup steps. Preserve saves/logs.
- Same checkout/branch. No reset/clean, unsolicited code regeneration, extra SD
  app variants or ZIPs. Preserve tested004, Deko artifacts and build objects/PCH.
  Max2 compile jobs total, including LTO. Dry-run when configuration changes.
- Active Mesa SD app remains `/switch/lswtcs/lswtcs.nro`; assets `/lswtcs`.
  Preserve `dist/tcs360-0.0.4`; use one future candidate staging root
  `<USER_HOME>/.codex/scratch/switch-ports/dist/tcs360-mesa-test/sdmc` only when
  a qualified candidate exists. This does not create another SD app. Parked Deko
  staging stays `dist/tcs360-deko-test/sdmc`, app `/switch/lswtcs-deko-draw`.
- ANY installation, portable dependency/runtime/driver/tool/plugin included,
  needs the project owner's explicit yes after purpose, size and location. Existing tools
  suffice for planning/baseline inspection. Never change security/permissions
  as a workaround for an unexplained write failure.
- Preserve saves and originals; no content/movie skips or quality/clock/speed
  reductions. Native user/save identity stays compatible. Investigate causes
  across consumers; add code only when reuse/removing an assumption is insufficient.
- Describe previews in text; export opaque RGB PNGs while retaining raw RGBA.
  Stop only owned PID/executable/start-time-verified processes. Keep the blocked
  LLVM18/SIMDe cleanup untouched.
- Update this harness's register and `docs/NEXT_SESSION.md` / session JSON after
  each checkpoint, never add a competing forward plan. Distinguish compiled,
  locally run, emulator-validated, hardware-passed and owner-confirmed claims.
- At every checkpoint report active W0–W5 package, completed checklist/denominator,
  percentage of that named checklist only, completed work, remaining acceptance,
  shared cause and why added code is necessary. Say unknown for remaining effort.
  Never turn test counts into whole-game completion or imminent-readiness claims.

### Current feature/defect register

| ID | State | Next decisive evidence | Gate |
| --- | --- | --- | --- |
| B0 | W0 baseline5/5 complete:006 named gameplay/stock/save/exit and known failed movie recorded; exact builds retained | Baseline collection is not W1 correctness.007 audio candidate staged, physical route open | W0 complete/W1 active |
| A1 |007 audio readiness fix passes1152 queue states/200 real-thread cases and integrated ARM/static checks; same6 buffers/PCM/credit semantics | One8–12minute stock handheld Chapter1 battle audio route; audible benefit and remaining starvation unmeasured | W1/W3 |
| L1 |005 timing-only output works on hardware; host6-case, ARM/integrated checks pass | Camera-on/off cost not isolated by A/B (both camera off);4/5 gates,80% checklist only; no speedup claim | W0 |
| M1 | Guest PCR overwritten; independent allocator claims can overlap | First invalid claim/write and repaired ownership/thread lifecycle | W1 |
| M2 |006 hardware movie: corrupt moving pixels, audio continues, no input recovery/end freeze. CPU decode64–70ms/replay, busy movie worker, residency/eviction growth; zero shader holds. First bad pixel producer/completion still unknown | Same-frame decoder-ready/upload/output boundary; first failed event/worker lifecycle, preserve PCR/BO separation | W1 |
| M3 |009 null BO-device/unmap crash unresolved.011 active bounded observer logs1,728,889 events at last checkpoint, no detected invariant violation; diagnostic delivery passed, cause not captured | Local review of unchecked lifetime/reuse windows and discriminating first-invalid evidence before owning repair; no repeat011 just to seek crash | W1 |
| S1 | B prewarmed148 programs, no queued jobs/recorded shader holds; A has3 holds totalling1320.2ms; optional binary reuse still dormant | Unique preparation ownership and safe persistence, targeted to observed first-use work | W2 |
| R1 |006 shared image/sampler key retained.012 recorded draw-key correction rejects26 reproduced unequal-input merges;300 legitimate plans unchanged, ARM/static staged | Physical atlas-spike/shifted-scene behavior and cost; full resident inputs, walk-time copying/texture/resolve dependencies remain open; no photo attribution | W1/W3 |
| P1 |006 has zero shader holds, persistent combat drops and completion waits; exact battle/profile alignment unknown. CPU report output works | Separate battle CPU/driver/GPU/pacing costs;007 audio repair does not promise higher combat FPS | W3 |
| F1 |006 save/reload/normal exit owner-confirmed,10 commits logged; prior per-user isolation preserved; dates/product coverage incomplete | Existing users/saves, date/time, controls/co-op/full content and sustained acceptance | W4 |
| D0 | Deko preserved, incomplete and inactive | Reopen only for a demonstrated limitation and bounded reuse/adoption case | W5 or explicit earlier decision |

Current work: W1/R1 recorded draw identity,012 candidate4/5(80% named gates),
physical visual/cost acceptance remains. Same NRO root/app/env, no marker; exact
011 retained. BO/visual/photo attribution and whole repair effort unknown.011
diagnostic bring-up4/4 complete separately. Prepared docs current; root sync
unverified. No new cache/thread/snapshot framework or quality/speed change.

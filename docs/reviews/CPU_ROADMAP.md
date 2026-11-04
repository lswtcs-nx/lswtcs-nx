# CPU roadmap after run 25, 2 November 2026

The state of the CPU work for whoever takes it over. It summarizes what runs 10-23 measured, what was tried and dropped, and the
two routes left: guest threads on several cores, and faster generated code. The run-by-run evidence is in `RUN2_FINDINGS.md`, and
the earlier survey of bo2-360-nx techniques is in `BO2_OPTIMIZATION_PATHS.md`. Numbers are from one handheld console at stock
clocks (CPU 1020 MHz, GPU 307 MHz), in Episode I chapter 2 (`Gungan_A` to `E`), the heaviest level tested.

## Where the frame goes (run 23, frames of 5000+ draws)

- **Guest turn:** about 70 ms per frame. Every guest thread runs one at a time under the cooperative scheduler (`kernel_stubs.cpp`,
  the "turn"). Shares of the turn in run 15: main thread 44 %, GPU command thread (`sub_822D2AB0`, the tile worker: PM4 walk and
  draw recording) 41 %, XAudio thread (`827DB318`) 12 %. With H, the walk is about 11-13 ms and recording 7-9 ms per frame.
- **Render thread:** 33-38 ms of CPU per frame (`[GLPROF]`). The largest part is `plan` at about 13 ms: XXH3 hashes of each tile
  draw's constants and geometry, to merge the three predicated tiles (`plan_untile`, `draw_base_key`).
- **GPU:** about 40 ms (`[GLGPU]`).

So the guest turn sets the pace in heavy scenes, and the render thread would be next at about 38 ms.

## FPS by run (frames with dt under 1 s)

| Run | 800-1300 draws | 1300-2500 | 2500-4000 | 5000+ | What changed |
| --- | --- | --- | --- | --- | --- |
| 10 | 17.1 | 18.2 | 16.2 | 9.5 | |
| 12 | 26.8 | 26.2 | 21.6 | 12.1 | XMA kick no longer takes a lock (the largest single gain) |
| 15 | 25.0 | 24.2 | 19.7 | 13.3 | H, native mixer (N) |
| 18 | 25.0 | 25.4 | 16.8 | 12.7 | |
| 22 | 26.2 | 23.5 | 20.5 | 11.5 | |
| 23 | 29.1 | 26.6 | 23.0 | 14.0 | Render 0x2C, guest priority/core states (RGC 14.9 at 5000+) |
| 24 | 28.7 | 26.5 | 22.1 | 15.2 | Same states with XAudio left alone; best heavy run so far, audio 57 % filled |

Scenes differ between runs; compare within a session where possible (`LSWTCS_ABCYCLE`).

## Kept, dropped, open

- **Kept (in the profile):** M tile merging, A audio first, P preparation reuse, X XMA/LZ2K off the turn, S direct stitching,
  H per-frame page hash (no sampled miss in runs 10-23; turns itself off at the first), N native mixer (exact).
- **Dropped:**
  - W (PM4 walk off the turn) now works after three race fixes (log17-log19), but it is slower: 8.7 against 14.0 FPS in run 22.
    The game waits for the walk anyway (fences, ring space, tile drain), and off the turn it runs on a shared core at a low
    priority. Y and D (VdSwap not waiting) did not change that.
  - T (tile copies) gave no walk gain. Q (longer quantum) did not lower the thread switches, which come from guest waits.
  - O (polygon offset) is not used by the faulty stud draws. The polygon offset code stays: it was never applied before log19.
- **Thread placement, runs 23-25:** R and RG are 10-15 % faster in heavy scenes and cost about as much audio (run 25: today's 13.9 FPS / 96 % filled, R 15.9 / 57 %, RG 15.1 / 80 % at 4000+ draws). The default is unchanged; the trade-off is the owner's. Details below.
- **Thread placement (log23-log24):** guest threads start with mask 7 and priority 0x3B, like the render thread. R (render 0x2C),
  G (guest 0x2E) and C (guest on core 0) are under test; G and C exclude the XAudio thread since log24, because in run 23 they
  emptied the audio buffers (60-65 % filled). `LSWTCS_PLACEGUARD=1` repairs and logs a guest thread left off its core; it fired
  for the file loader stuck on core 1 above the render thread, the likely cause of runs 19-20's slow render.
- **Open faults:** studs and dark outlines of one brick drawn through the characters (`Gungan_C`), from program 374's early
  draws (cube sampler bound to a 2D texture, `DIM-MISMATCH`). See `RUN2_FINDINGS.md`, runs 17-20.

## Big picture after run 32: half of the turn is this port's own code

Run 31, frames of 1300+ draws, by the sampled pc while holding the turn (`private/session-tools/turn_big_picture.py`):

| Share of the turn | What |
| --- | --- |
| 41 % | guest code (main thread 76 % guest, XAudio thread 54 %) |
| 34 % | PM4 walk, draw recording, hashes: `pm4_extract_ib` 6.7+, `XXH3` 6.3, `pm4_regs_copy` 6.1, `gpu_record_draw*` 3.6 |
| 8 % | `memmove`, `memset`, `memcmp` |
| 7 % | Horizon syscalls |
| 10 % | other host code, thread and scheduler code, kernel stubs |

The command thread is 38 % of the turn and almost all native. So compiler work on generated code (LTO, PGO, plain memory) reaches 41 % at most: bo2-360-nx's +9.8 % would be about +4 % here, and
`plain_mem_probe.sh` shows little for plain memory to remove. Order to try, by how much each can reach:

1. **One tile at the source (`LSWTCS_ONETILE`, log33, not yet run).** The game draws its world three times per frame for EDRAM. The engine's layout function decides that, and has a plain path, so log33 makes the layout fit without changing MSAA or
   resolution (`RUN2_FINDINGS.md`, "0.0.39-log33"). Heaviest frames: walk about 33 ms, `repeat tiles` 9 ms measured, of a 72 ms frame. A guess of 9-22 ms saved, plus much of `plan` (13 ms on the render thread); no measurement behind it. Run 14's T could not reach this, since each tile's draw state differs.
2. **The walk and recording themselves.** Native, not guest-thread-safety work, and often switchable in A/B. Recording on a second thread, as bo2-360-nx defers it, moves only native work.
3. **Fourth core** (bo2-360-nx: +11.9 to +16 %, audio and XMA alone on core 3). Needs a forwarder installed: owner decision.
4. **Game settings** (bo2-360-nx's main route: half the draws at its very low preset). Product decision of the author, never silent.
5. **XAudio off the turn** (U): about 12 % of the turn at most. Run 33 tests it.
6. **Compiler work:** late, as above. bo2-360-nx measured a full Direct3D-level renderer at about +2 % at best, so replacing PM4 with D3D hooks is not a route on its own.

## Where this stands after run 34 (stock clocks only, the tester's decision)

**CPU.** One tile and U took the big steps. What is left is flat, and heavy frames are now also GPU-limited, so CPU gains show only in part of the level:
- guest code: the top function is 3 % of the turn and the top 20 about 25 %, so native rewrites at bo2-360-nx's 1.4-2x would give perhaps 5-8 % for a lot of work;
- record path and hashes: about 17-27 % of the turn, with no single large item. E, the cheap idea, was rejected;
- LTO, direct calls and PGO: now reach about 60 % of the turn, so perhaps 5-10 % together, with a large build cost. Unmeasured;
- fourth core: frees core 2, which Mesa's worker (40 % busy) shares with XMA and U. Needs a forwarder: owner decision.

**Mesa is not the CPU limit:** at 1300+ draws (run 34) the render thread is 27 % busy and Mesa's worker 40 %. So Deko's clear win elsewhere, CPU submission (USF4: render thread 18.6 to 2.8 ms), would not raise FPS here by itself. What could is GPU time, where USF4 went from 9.3 to 8.4 ms and TCS has no measurement. GPU `draws..present` in run 34: median 26.5 ms, 34.5 at the 75th percentile, 42-45 at the top.

**Deko.** The harness's gate (`TCS360_DEKO_IMPLEMENTATION_PLAN.md`) asks for a dominant graphics cost after the easy fixes, a bounded benefit and cost, and a thin comparison on existing captures at equal semantics; then an explicit decision. The first part is starting to hold. The rest is not done. Next: list GPU cost by pass on Mesa, try the cheap Mesa items (resolves, then colour compression, `PORT_RESEARCH.md`), and take the same-frame comparison to the owner.

## Route 1: guest threads on several cores

The only route that could roughly double heavy scenes; also the riskiest. **Run 24 measured that there is parallelism to gain**
(`[PARALLEL]`, frames of 1300+ draws):
- the turn is held 88-100 % of the time, and at least one more guest thread is ready and waiting in 62-76 % of it;
- the main thread waits 43-56 % of the time, and the XAudio thread 28-40 %.

"Ready" includes threads that only poll, so this bounds the gain rather than measuring it. It also explains why heavy-scene audio
gets worse as FPS rises (57 % filled in run 24): the XAudio thread competes for the same turn. It is the natural first thread to
move.

What has to change before two guest threads may run at once, from reading the code (not exhaustive):

1. **Memory barriers.** The generated code drops `sync`/`lwsync`/`eieio` (comments only; 43 such comments counted). Under the turn
   that is safe, because every handoff goes through a mutex. In parallel, ARM's weak ordering needs `dmb ish` where the PowerPC
   code had them.
2. **Kernel object stubs** (events, semaphores, mutants, critical sections, thread state, `KeWaitFor*`): written for one runner.
   Each needs its own lock, or one kernel lock, held only inside the stub.
3. **GPU path globals:** `g_interrupt_ctx`, `g_pm4_walking`, the ring read pointer, the frame being recorded, the tile drain.
   Runs 17-18 showed how two walkers corrupt them. One walker at a time, under a lock, is the minimum.
4. **Other globals:** the shader-compile `g_compile_jmp`/`g_compile_active` (one jmp_buf for the process), the guest pool
   allocator (has a host mutex), file I/O stubs, the scheduler's own counters and `lswtcs_gil_ctr`.
5. **Atomics** already map to host compare-and-swap (`LSW_GUEST_CAS`), and guest critical sections to host mutexes, which helps.

**Spinlocks were exclusion by the turn alone** until log26: the `KfAcquireSpinLock` / `KeAcquireSpinLockAtRaisedIrql` stubs
only counted a depth. State L (log26) takes the lock word for real; any guest code that leaves the turn while holding a spinlock needs
it. This is item 2 of the list above, done for spinlocks only.

**What runs 26-27 taught about the granularity:** a turn handoff costs about 0.41 ms (two scheduler wake-ups). The second mixer's
call takes 0.010 ms, so moving single calls loses about 40 to 1. Either a whole subtree leaves the turn (the voice processing,
`sub_827F1258`, 31 functions, see `callee_audit.py`), or the XAudio thread runs entirely off the turn and syncs only through real
locks and kernel objects. The second needs every kernel stub it calls made safe beside the turn holder, and its indirect-call
targets found at run time; the static audit cannot follow them. Real spinlocks (L) must work first: run 27 froze in L, and log27 is
the attempt to fix it.

**Run 30 (log28):** real spinlocks still stall on the XAudio voice lock (846 waits past 200 ms, 8.4 FPS), even with the turn passed
to the holder. Either the lock is also released outside the stubs, or a holder waits for something while holding it. Find which
first (`RUN2_FINDINGS.md`, run 30, "Next evidence"); until then L is off and route 1 is blocked at this step.

**log29:** the guest code points to a third cause: the voice lock is recursive, with an inline count and owner, and overlapping sections
leave the count stuck above 0, so the release stub stops running. Not yet confirmed at run time. log29 takes real locks from boot and clears
the count on winning the word; run 31 compares L and no L on the same level (`RUN2_FINDINGS.md`, "0.0.35-log29").

**Run 31 (log29): confirmed, and route 1 is unblocked.** With L from boot: no forced wait, 16 short waits in 4.67 M acquires, 0-10 % of FPS at most and fuller audio. Without L the count
goes stale in heavy scenes, so today's default also skips the quantum most of the time from then on. log30 moves `sub_827DDE40`, 57 % of the XAudio thread's turn, off the turn.

**Started in log25 (state U, `audio_offturn.cpp`):** one pure guest function of the XAudio thread, `sub_827F5990`, runs off the turn
on core 2 while other guest threads hold it. The pattern applies to any guest function that touches only its own buffers: check the
recompiled body for imports, spinlocks, atomics and calls, override the weak symbol, and run it inside `lswtcs_gil_blocking_urgent`
with `g_guest_off_turn` set. Next candidates by the same check: other leaves of the XAudio thread, then larger subtrees once their
locks are understood. The handoff costs two scheduler wake-ups per call, so small calls may lose; `[AUDIOOFF]` measures it.

A staged start: let one thread run beside the turn holder and nothing else. The XAudio thread (12-20 % of the turn) touches
little shared state, and its native reverb already runs off the turn. Every stub it calls has to be audited first. Expect rare
corruption rather than clean crashes when something is missed, so each step needs long sessions and the input checks kept on.

## Route 2: faster generated code

Lower risk, and it speeds up all guest code at once. Registers are already C++ locals (`lswtcs_locals.toml` builds `output`).
Candidates, from `BO2_OPTIMIZATION_PATHS.md` and this review:

1. **LTO on a subset of the generated units** (bo2-360-nx: +9.8 %). Guest calls go through `PPC_WEAK_FUNC` (`weak,noinline`),
   so direct, inlinable calls between guest functions come first, keeping the functions the runtime replaces (natives, hooks).
2. **PGO** trained on the console (bo2-360-nx: +9 %). Needs a training session and a retrain after code changes.
3. **Plain guest memory instead of volatile.** Every guest load and store is `volatile` (`ppc_context.h`), which forbids the
   compiler from keeping values in registers or merging accesses. The risk is spin loops that poll memory with no call in the
   loop; a call ends any hoisting, and the scheduler only switches threads at calls. Audit those loops, then switch.
4. **The quantum counter** (`++lswtcs_gil_ctr` at every guest function entry, `PPC_FUNC_PROLOGUE`) is a global load, add and store
   per call. A per-thread register counter or a check on loop back edges would be cheaper. Small, but on every call.
5. **Native rewrites** of the hottest guest functions, checked against the original on a sample (as N does). The profile names
   them (`tools/hwprof_report.py`, `private/session-tools/heavy_turn_leaves.py`).

Compiler changes cannot switch during play, so they need two builds and the same scene played on each, or a fixed benchmark path,
which does not exist yet. A rough guess of 10-25 % in heavy scenes for 1-3 together has no measurement behind it.

## Route 3: the render thread

Not the limit today, so it pays only after route 1 or 2. `plan` hashes about the same bytes as the walker already hashed (H's
pages); reusing the walker's hashes for unchanged ranges, or comparing tile draws only where the walker saw a change, could cut
most of its 13 ms.

## How the runs are made

- One session per run (the tester's request). Use A/B states (`LSWTCS_ABSTATES`, 15 s each) and, if needed, a second list after
  the first return to the hub (`LSWTCS_ABSTATES2`).
- Analysis: `private/session-tools/run_summary.py <session>` (FPS by band and state), `turn_by_thread.py`, `thread_stacks.py`,
  and the `[GLPROF]`, `[GPUPAGES]`, `[THREADS]`, `[PLACEMENT]`, `[PARALLEL]`, `[WALKOFF]` lines in `stdout.log`.
- Staging: `profiles/hardware-ab.env` to `private/sdcard/lswtcs/env.txt`, the build's NRO to `private/sdcard/switch/lswtcs/`,
  the ELF to `private/lswtcs-logN.elf`, and the logs to `private/runs/runN-logN` after analysis.

## Where to go after run 36

Stock clocks. Frames of 1667+ draws, `Gungan` level.

| | Handheld | Docked |
| --- | --- | --- |
| FPS | 20.3 | 24.9 |
| Median frame | 42 ms | 40 ms |
| GPU, sampled | about 41 ms | 25.5 ms |
| Limit | GPU, with the main thread close behind | main guest thread, about 95 % busy on core 0 |

**There are two walls.** Handheld gains need the GPU to drop first and the CPU soon after. Docked gains need the CPU alone. No single change moves both, so the plan works both sides.

What the main thread does at heavy load, by samples: guest code 56 %, and PM4 walk, recording and hashes on its stacks about 37 % (stacks are scans, so this is approximate). It walks the ring itself: the walk is lazy, run at guest waits (`pm4_walk_now`, VdSwap).

| Order | Route | Reach | Cost and risk | Status |
| --- | --- | --- | --- | --- |
| 1 | **Skip constant buffer binds that change nothing (B).** One changed block made Mesa rebind every block of every stage. | Part of the 7-13 ms the omission split gave docked. Handheld more. | Done. Runtime switch. | Run 37 |
| 2 | **Hot functions together (`hot_order.ld`).** 2042 sampled functions now span 3.7 MB, not 73 MB. Only placement changes. | Unknown, likely a few percent of guest code. | Done. Relink only. | Run 37 |
| 3 | **Ring walker on its own thread.** Up to about 37 % of the main thread. Docked heaviest from 40 ms towards 28 ms. Handheld only as far as the GPU allows. | The largest CPU item left. | High: races, as W showed. Waits for `[WALKWHY]`: if the guest waits right after each kick, little can run in parallel. | Decide after run 37 |
| 4 | **Native record path.** XXH3 about 7 % of the main thread, register copy 5 %, IB parse 5 %. Copy is already NEON. | Maybe 3-6 %. | Medium. Exact behaviour is checkable. | After 3 |
| 5 | **Generated code.** Stack pointer in locals: -2.2 % instructions on one unit (`stack_local.py`). The view table is 6 % of instructions, plus a shift, and has no flat replacement while physical aliases exist. `-O3` is larger; `-fipa-pta` and plain memory change little. | Perhaps 3-5 % with LTO still unmeasured. | Full rebuild for each test. | Late |

**Dropped:**
- W: the main thread is the walker, so going off the turn frees only others. Others hold 5 % of the turn.
- Guest threads on several cores: main is 95 % of the turn, and with none else waiting in 55 % of heavy ticks, there is little to run beside it.
- Quads without a geometry shader, Deko for FPS (`RUN2_FINDINGS.md`, run 36).
- Fourth core: Mesa's worker is not limiting.

**Owner or tester decisions, the largest levers and not to be applied silently:**
- Handheld GPU clock. 307 MHz is the default. Docked's gains show what more clock buys.
- Fewer far draws (level of detail). One shader spends 7.5 ms on 1021 draws of about 50 samples each, and every draw also costs the main thread. Needs finding the engine's setting.
- MSAA or resolution in the main pass.

## Update after run 38

Changes to the table above:
- **B:** harmless and small handheld, not the gain run 37 suggested.
- **Function order:** no visible effect.
- **Walker thread:** down. The frame's walk follows the kick at once, so it can only help if the fence wait can return before the walk. One more log, the distance from fence to put and the caller, settles it.

Next, in order:
1. **Memory traffic on the GPU.** Handheld GPU is about half memory (estimate). Find out what compression this Mesa path really gets, then try colour and depth compression. It may need changes in `libdrm_nouveau`, whose source is not in this tree. If Mesa cannot do it, this is the one concrete reason left to reopen a thin Deko comparison, since Deko exposes `DkImageFlags_HwCompression`.
2. **Resolves:** 6 ms in 30 per frame handheld, with 2.4 ms docked left with pixels off.
3. **Main thread:** hashes and record path, then LTO on hot units. A few percent each, and they show docked first.

## Zoom out after run 40 (3 November 2026)

**What changed since run 34.** One tile and U were structural and gave the large gains. Every item since has been small or nothing: B, function order, compression. The A/A run 39 shows why measuring them failed: one pass carries about ±10 % between A/B visits at heavy load.

**Method from here:**
- GPU changes are measured on one frame drawn again (`LSWTCS_GPUBENCH`).
- CPU changes count only when large (10 % or more), or when they show in direct timings such as `[GPUPAGES]` or `[WALKWHY]`.
- A letter is not kept on one pass's FPS alone.

**Two candidates are structural, and log41 sizes both:**
1. **Early depth tests (GPU, handheld).** A third of the programs shade every pixel before the depth test because of an alpha test that is usually off. If the bench shows several ms, it is the largest same-quality GPU item found.
2. **The walk on its own thread (CPU, docked).** The awaited fence is two segments back, and the ring's packets never complete it. If its segment was always read by an earlier walk, the frame's walk can overlap the game's next frame: about 5-15 ms of the main thread in heavy scenes.

**After those:** whatever the bench shows as the largest remaining GPU part (constant binds or resolves), then the hash and record path.

**Not ours to apply, and the largest levers:** handheld GPU clock, far detail (level of detail), resolution.

## Update after run 45 (3 November 2026)

- **The walk on its own thread (I) now overlaps fully.** No fence wait waits for it. The main thread holds the turn 24 % less. Handheld FPS does not change, because handheld is limited by the GPU. Docked decides whether I becomes the default.
- **Handheld GPU.** Compared with the 360's GPU, the handheld clock gives about two thirds of the shader throughput, and FPS sits near that ratio. The items left on our side are resolve copies (at most 5.5 ms) and nothing else of that size. The handheld GPU clock stays the largest lever, and it is not ours.

## Zoom out after run 46 (3 November 2026)

**Where each mode stands, heavy Gungan frames:**
- **Docked (I on):** 37 ms per frame. The main thread is busy for about 31 ms of it, the GPU takes 21 ms (resolve copies 3.2 ms), and the render thread's replay takes about 19 ms.
- **Handheld:** 49 ms of GPU per frame. Pixel work is about 33 ms of that, and resolve copies 5.5 ms. The CPU does not limit handheld.

**The largest open item is docked.** The walk was 10-15 ms of the main thread per frame without I. Moving it off should have taken frames towards 25-28 ms, where the GPU and render thread meet. Instead they went from 41 to 37 ms. About 10 ms per frame is unaccounted for. log47 measures it: CPU per thread by state, IB time by thread, and the walker's core.

**Next steps, depending on what log47 shows:**
- If walk work remains on the main thread, finishing I is the largest CPU item left (up to about 25 % docked).
- If the main thread slows beside the walker, cut the walker's memory traffic, mainly the page hashes.
- If neither, the main thread's own guest code is the wall. Then the routes are generated-code changes (LTO, the quantum counter, plain memory). Those need two builds and a cross-run comparison.

**Handheld:** the clock is the lever, and it is not ours. On our side the next item is resolves drawn straight into their destination, at most part of 5.5 ms.

## Zoom out after run 60 (3 November 2026)

The measurements below are from heavy frames (1300+ draws, the start of `Gungan_B`) at stock clocks.

| | Docked | Handheld |
| --- | --- | --- |
| FPS, before | 24.4 (run 46, no I) | 20.0 (run 54, I only) |
| FPS, now | 29-30 (run 54, I k h w r) | 24.8 (run 60, plus e g) |
| Limit | Main guest thread on core 0: about 24 ms of CPU and 3 ms of waits | GPU: 36-40 ms per frame |

**Kept, with evidence:**
- **Docked:** I, k, h, w, r. Ikhwr against I is +15.5 % over runs 53-54 (90 %: +7.4 to +24.1).
- **Handheld:** e is +10.4 % (+0.9 to +20.5), and g is exact but small. On handheld, k, h, w and r together are +8.5 % (+0.3 to +17.5).
- **Out:**
  - z broke transparency.
  - p (depth pre-pass): -5.6 %.
  - G, f and K: no gain.
  - NIR: wrong shadows, darker light, -21 %, and a crash in `nv50_ir`.
- **Open:** d. It was -3.1 % in FPS (-10.1 to +4.1), but the bench showed 1.8 ms saved.

**The handheld GPU frame (run 60 bench, plain frame median 36.4 ms):**
- **Pixel shaders' own work, about 17 ms.**
  - About three quarters is arithmetic and a quarter texture memory. Filling (raster, late depth, colour writes) costs next to nothing.
  - Much of it is hidden layers shaded in full. Every world draw has the alpha test (references about 0.5), so nouveau tests depth after the shader. The forced early test was -20 % but wrong.
- **Resolves, about 5 ms.** The copies are about 4 ms, and about 2 ms of those could become texture hand-overs.
- **Vertices:** 2-3 ms.
- **The rest, about 10 ms:** per-draw cost, resolves and present. log61's bench adds a state-off variant to size the per-draw part.

**Docked:** guest code is about 85 % of the main thread. At about 155 k guest calls per frame and 160 ns each, inlining or LTO could reach 5-10 % at most.

### Plan, in order

1. **ZCULL in nouveau, for handheld.** It is exact and the largest item within reach.
   - **Why it fits:** the GPU's coarse depth culling works with shaders that discard.
   - **This Mesa:** it is off (`nvc0_validate_zcull` under `#if 0`, `ZCULL_REGION` deactivated at screen init).
   - **The tools exist:** libnx has `nvGpuChannelZcullBind`, `nvGpuGetZcullCtxSize` and `nvGpuGetZcullInfo`, and deko3d turns ZCULL on by default. deko3d's code is a working reference on this GPU (ZLIB licence: take its technique and register sequence, write our own code).
   - **Steps:**
     - read deko3d's ZCULL setup;
     - the kernel binding is already done: libdrm_nouveau binds a ZCULL context on Mesa's channel (see "Deko3D study" below);
     - program ZCULL storage and region for the depth target in nvc0, invalidating on depth clears and target changes;
     - an A/B state plus a bench variant, with a pixel comparison as in state V.
   - **Risk:** GPU faults while it is misprogrammed. It will take several rounds.
   - **Status:** log62 builds it as state c (`source/switch/mesa/mesa_zcull_edits.py`), with a bench variant and a pixel check (`[ZCULLCHECK]`). **Run 62: exact, no gain.** Every draw that writes depth has a real alpha test, and ZCULL cannot learn depth from draws that may discard. Closed.
   - **Reach:** part of the forced early test's 20 %.
2. **Resolve texture hand-over (about 2 ms, 5 %).** Render targets have one fixed height, so a destination that takes the target's texture needs a per-fetch coordinate scale (through the fetch fold) and exact edges under clamping. Alternatively, render targets could be sized to their resolve.
3. **The full-screen post passes (about 3-5 ms in two or three passes).** Measure a hand-cleaned version of the costliest pass in the bench first, and change the translator's control-flow output only if that pays.
4. **Docked, generated code:** inlining or LTO on the hot units, compared over two sessions with `private/session-tools/run_pair.py`. Runs 58 and 59 in the same state differed by +0.1 % (90 %: -5.2 to +5.5).
5. **Settle d** in a run where nothing else changes.

### The author's decisions (the largest levers, not ours)

- **Handheld render resolution.** Pixels are about 60 % of the handheld GPU frame.
- **Clocks:** handheld GPU 307 MHz against 384 or 460, and docked CPU.
- **The fourth core.** It needs a launch from a title whose NPDM allows core 3, and it would relieve the other threads, not the main one.
- **A Deko3D backend.** That is the author's later priority. It would remove Mesa's CPU cost (render thread and Mesa worker, about 20 ms per docked frame on cores 1-2) and bring ZCULL natively. The study below finds no large gain from it on its own.
- **Making the measured states defaults:**
  - Older letters M A P X S H N L U B.
  - Newer letters I k h w r e g.
  - **h needs the author's review:** it makes the game behave as on a console whose CPU-GPU link trained, as Xenia's stub reports.

## Deko3D study (3 November 2026)

**Question:** would moving the renderer to deko3d bring a large FPS gain at stock clocks?

**Answer: not on its own.** The one large GPU item it brings, ZCULL, can be had on Mesa.

**The ceiling.** The CPU runs at 1020 MHz in both modes. Docked heavy frames (29-30 FPS) sit at the CPU wall, so any GPU-side change in handheld (24.8 FPS) reaches about +20 % at most in heavy scenes. That holds for Deko, ZCULL and resolution alike.

**Docked.** The wall is the main guest thread, about 85 % guest code. Deko would remove Mesa's render thread and worker (about 20 ms per frame on cores 1-2), and neither runs on that thread. At most it trims the main thread's 3 ms of waits: under 10 %, unmeasured.

**Handheld, against the run 60 bench (36.4 ms):**

| Part | ms | With Deko |
| --- | --- | --- |
| Pixel shaders | about 17 | Same. deko3d's compiler (`uam`) is nouveau's codegen, fed the same GLSL. |
| Hidden layers, inside the 17 | unknown share | ZCULL, on by default. Mesa can have it too (below). |
| Resolves | about 5 | Hand-over is possible on either API. |
| Vertices | 2-3 | Same. |
| Per-draw cost and present | about 10 | The only part that could differ; unmeasured. The prototype binds 6 uniform buffers per stage per draw, so it is not cheaper by design. log61's state-off variant sizes this part. |
| Colour compression | memory is about half the GPU time (run 38 estimate) | Possible with deko3d's compression flag. On Mesa it blurred (run 40). Unmeasured. |

**What a migration costs:**
- **The prototype is unfinished.** `deko_backend.cpp` (835 lines, 18 headers) reads the same recorder, but cold launch passed 0 of 3, and EDRAM and resource ownership are not done.
- **No shader compiler on the console.** `uam` is a host program (`tools/bin/uam.exe`), and the prototype stops at any shader it lacks. The game makes variants at run time, and e and g add more: 1017 programs saved now against 509 sources in run 35. Deko needs an offline catalogue that covers every variant, or `uam` built for the console.
- **The GL-side work would be redone and remeasured:** resolves, fetch fold, epilogues, early depth rules and the walker hand-off.
- **It is the author's priority and decision.**

**ZCULL on Mesa is smaller than planned.** From deko3d's source (ZLIB; technique and register values only):
- **The kernel binding is done.** libdrm_nouveau 1.0.1, the installed version, allocates a ZCULL context and binds it to Mesa's channel (`pushbuf.c`, `nvGpuChannelZcullBind`).
- **nouveau already writes the per-shader test mask** (`ZCULL_TEST_MASK`, 0x11 when the shader writes depth). 13 of the 1017 saved programs write `gl_FragDepth`, so most draws could use ZCULL.
- **What is missing is the 3D engine's setup.** deko3d writes it in two places:
  - **At init:** `ZCULL_REGION` 0 (nouveau writes 0x3f, "deactivate"), `0x1968` = 0x11, test mask 0, stencil criteria at `0xdd8`.
  - **When a depth target is bound:**
    - width and height aligned to `nvGpuGetZcullInfo()` (32 pixels);
    - layer size = aligned area / `pixel_squares_by_aliquots`;
    - `0x7e0` = image size << 16 and layer size, `0x15c8` = 2, `ZCULL_WIDTH` = width, height and layers, window offsets 0;
    - invalidate when the depth target's address changes. deko3d uses method `0x4b2` for that; nouveau's header names `ZCULL_INVALIDATE` at `0x1958`. Which one works needs checking.
  - nouveau's `#if 0` `nvc0_validate_zcull` writes the same `0x7e0` and `0x15c8`, but it guesses the sizes and uses the desktop address registers.
- **Open risks:**
  - depth written where ZCULL does not see it (copies, `gl_FragDepth` restores of old EDRAM contents) needs an invalidate;
  - pooled render targets reuse addresses at other sizes;
  - GPU faults while the setup is wrong.

**Recommendation:** no migration now. ZCULL on Mesa stays item 1, now without the kernel part. Reopen Deko if log61 shows a large per-draw part that Mesa cannot shed, or if the author chooses it for other reasons (hitches, memory). Even then, handheld gains stop at the CPU wall.

## Update after run 63 (3 November 2026)

**Kept:** d (fetch constants; three benches, 1.3-1.8 ms) and q (quads without the geometry shader; exact, about 1 ms). **Out:** c (ZCULL). It is exact, but every depth-writing draw has a real alpha test, so ZCULL has nothing to learn from.

**Handheld now:**
- Heavy frames run at about 26-27 FPS, with about 36 ms of GPU work per frame.
- Docked runs 29-30 FPS at the same CPU clock. So about 4 ms of GPU savings brings handheld to the CPU wall, and anything past that needs CPU work.
- The bench's leftover with everything off (about 10 ms) is mostly the GPU waiting for submission, not GPU cost (RUN2_FINDINGS.md, run 63).

**Options left:**

| Option | Size | Notes |
| --- | --- | --- |
| Resolve texture hand-over | at most about 2 ms handheld | Needs a coordinate scale and edge clamping per fetch. Complex. |
| Full-screen passes | the costliest is about 3 ms | Gain unmeasured. |
| CPU wall (docked, and handheld once the GPU drops) | inlining or LTO estimated at 5-10 %; PGO unmeasured | Compared across two sessions with `run_pair.py`. |
| Author's levers | larger | Handheld resolution and GPU clock. |

**log64 (CPU wall, first step):** guest calls made inlinable (`LSW_GAME_INLINE`: units along hot calls, weak linkage only for the 80 runtime overrides, `-fno-semantic-interposition`). Code text 6 % smaller. Measured over two docked sessions (log63 against log64). If it pays, the next steps are larger units or LTO over the hot units, then PGO.

**Run 64:** inlinable guest calls changed nothing (CPU per guest call 161 ns against 156-158; the same profile). The main thread's time is inside large guest functions: memory accesses through the region table, writes back into `ctx`, float compares and FPCR switches. Options left on the CPU side are in RUN2_FINDINGS.md, run 64. None is expected to reach 10 % alone.

**Loading (log65):** the tester turned to load times. During loads, two loader threads hold the guest turn while blocked on the card, about 30 % of the time, so reading and processing never overlap. log65 tries freeing the turn during file calls, plus 512 KB reads, in modes cycled over loads (RUN2_FINDINGS.md, log 65). Run 64's loads: hub to Gungan 7-8 s, Gungan to hub about 21 s.

## mk9-nx study (4 November 2026)

mk9-nx (Thorhax, Mortal Kombat Komplete Edition, ReXGlue recompilation on deko3d, GPL-3.0: techniques only, no code) runs 60 FPS at CPU 1785 MHz. Read: `docs/mk9-notes.md`, `app/perf_overrides.toml`, `app/jobs.toml`. Compared with run 72's profile: the guest main thread holds 88 % of the turn samples; its profile is flat (top function `sub_8231ED30`, a vector matrix routine, 4.7 %).

**Worth trying here:**

| Technique | Their result | Here |
| --- | --- | --- |
| Guest loads and stores without `volatile`, plus a compiler barrier at every loop header (GCC turns polling loops into infinite loops without it) | +15-20 % FPS in masseffect-nx | Route 2 item 3, not done. Fits a flat profile. Do it on a copy of the generated output; A/B pair, docked. |
| Read-only image constants folded into the code; float loads straight into the vector unit | 58.8 -> 59.2 FPS | Small; could ride with the item above. |
| First controller to press Start or A becomes player 1 | fixed a sign-in prompt from a second controller | Fallback for co-op order if log72's Joy-Con merge leaves cases. |
| Switch button icons drawn over the Xbox icon atlas at texture upload (known atlas by hash, re-encode touched blocks) | all prompts right | Cosmetic; the tester's and the author's call. |

**Already here or not useful:** registers as locals (`lswtcs_locals.toml`); `skip_msr`, their largest gain (11 -> 47 FPS), removed a global lock our `mtmsrd` never had (it only stores `ctx.msr`); native sound mixer (our guest audio thread is 2 % of the turn and the reverb is already native); physical allocation scan (ours is a bump allocator); critical section spin (one core under the turn); store cards instead of render-thread hashing (render thread is not the limit, and every guest store pays for them); drawing nothing while a pipeline compiles (they turned it off: coloured triangles; we wait, as they now do); LTO (run 64, no gain).

## Deko3D for memory and stutters (after run 75, 4 November 2026)

Runs 73-75 crashed from host memory, and the remaining stutters are texture uploads. Would deko3d fix them?

| Problem | In the Mesa path | With deko3d |
| --- | --- | --- |
| Heap filling up | Mostly rpmalloc keeping freed buffer memory (log75 sends 64 KB+ blocks to newlib); programs about 0.85 MB each | Programs as DKSH binaries, tens of KB; no Mesa program objects; we free GPU memory ourselves. Likely the 512 MB GL copy of guest memory could become a direct mapping (untested) |
| Upload stutter | glthread syncs on every `TexSubImage` (log75 makes new textures before the draws); untiling at 5 ms/MB on the render thread | Copy straight into GPU memory from any thread, no glthread |
| Shader stutter | Compile time is Mesa's nv50 codegen | `uam` is the same codegen: similar per shader; no link or glthread costs |
| FPS | Handheld GPU bound, docked guest CPU bound | At most about +20 % handheld (Deko3D study above) |

**Cost:** the live renderer is about 6,700 lines of GL path with months of fixes (tiles, resolves, untiling, EDRAM, ZCULL). `deko_backend.cpp` is 835 lines from the 1 November baseline. A switch is a rewrite, and the author's call ("not now").

**Recommendation:** fix the two causes found in the Mesa path first (log75), measure, and bring deko3d to the author only if memory or upload costs remain. The case for it would be memory (programs, the 512 MB copy) and upload cost, not FPS.

## Zoom out after run 79 (4 November 2026)

**Where the port stands (handheld, stock clocks):**
- **Stability:**
  - Runs 76, 77 and 79 had no crash, up to 870 s and three levels.
  - Runs 73-75's out-of-memory crashes came from rpmalloc keeping freed blocks (log75) and no heap figures (log73). 604 MB stays free now.
- **FPS** (same route, `Gungan`):

  | Place | Run 10 | Run 79 handheld | Run 71 docked |
  | --- | --- | --- | --- |
  | Hub | 14.7 | 37-44 | 55 |
  | `Gungan_B` | 14.1 | 28.6 | 36.8 |
  | `Gungan_B` heavy part | 9.8 | 25.7 | 30.0 |

- **Stutters:**
  - New textures cost 2.7 ms each (log76; was 8).
  - A level's first visit still builds its programs on first use: 0.5-2.6 s for the largest. Later visits find them in the saved list.
- **Boot:** first guest frame at 5-10 s. The saved-program list (1255 files) then takes about 2 minutes to queue and longer to compile, in the background.

**What limits each mode:**
- **Handheld is GPU-bound.** The port runs the handheld GPU at 307.2 MHz, the lowest of Nintendo's three official handheld profiles (307.2, 384, 460.8 MHz). Docked runs it at 768.
- **Docked is CPU-bound.** The main guest thread at 1020 MHz, the same clock in both modes.
- **So handheld's ceiling is today's docked FPS:** `Gungan_B` 36.8, heavy part 30.0, hub 55.

**Options, by expected value:**

| Option | Expected | Cost and risk | Whose call |
| --- | --- | --- | --- |
| Handheld GPU profile 384 or 460.8 MHz (official, not overclock) | Up to the docked FPS: `Gungan_B` about +17-28 %, hub more | A setting; battery and heat | Author (and tester: stock clocks) |
| Texture decode off the render thread; pooled storage for small textures | Area-entry and cutscene stalls about halved again | Medium; decode is pure CPU | Ours |
| Saved programs: queue the current level's first, read the list faster | Fewer first-minutes stalls (run 79's 0.5-0.7 s `bind` frames) | Small | Ours |
| CPU wall (docked): native rewrites of the hottest guest functions, PGO | 5-10 % each, unmeasured | Medium-large; no single hot function (top one 4.7 %) | Ours |
| Guest memory without volatile (log77 B) | At most 2-5 % per guest call even with unsafe aliasing; a correct build would need `-fno-strict-aliasing` | Wrong behaviour seen (run 78) | Shelved |
| Deko3D | Memory and upload costs; FPS 0-10 % handheld, capped by docked | Renderer rewrite | Author |
| Guest threads on several cores (Route 1) | Docked CPU wall | Large; earlier attempts stalled on locks | Ours, later |

**Recommendation:**
- Bring the handheld GPU profile to the author first: it is the only lever that moves handheld FPS by more than noise.
- On our side, finish the stutter work (texture decode, small textures, saved-list order). The port is stable now, and stutters are what the tester notices most.

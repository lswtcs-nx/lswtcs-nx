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

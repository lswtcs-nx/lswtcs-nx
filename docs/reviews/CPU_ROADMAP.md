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

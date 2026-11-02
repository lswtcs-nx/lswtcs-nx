# CPU optimization paths from bo2-360-nx, 1 November 2026

Supporting evidence for later work, not a second work plan and not started. Reviewed:
[bo2-360-nx](https://github.com/AlvaroEPena/bo2-360-nx/tree/4bab074a60e8ceb608e669e54961d2d06983fc98), its
[technical notes](https://github.com/AlvaroEPena/bo2-360-nx/blob/4bab074a60e8ceb608e669e54961d2d06983fc98/docs/technical.md)
and a few of its source files. Public source and documentation only; nothing was built or run. No result below was reproduced
on this project's console.

Why it matters: it is Black Ops II from the 360 executable, by static recompilation with ReXGlue, so it has the same kind of
generated code as TCS. Its renderer differs (Vulkan on NVK). `PORT_RESEARCH.md` holds the shorter GPU-side summary.

## Limits

- **License.** bo2-360-nx is GPL-3 and this port is CC BY-NC-SA 4.0. Reuse the techniques and write the code here; do not copy
  its code. Confirm any doubtful case with the owner before it lands.
- **Numbers do not transfer.** Its percentages are frame-rate gains in one game, from its own A/B runs. Mass Effect measured no
  gain from LTO and PGO, so the two ports already disagree.
- **Where it applies.** These paths help where the guest CPU sets the pace, as in run 7's heaviest level: GPU 54 ms and GPU idle
  47 ms of a 99 ms frame. Run 6's lighter levels were GPU-limited and would gain little.
- **Order depends on `LSWTCS_HWPROF`.** Run 8 was the first profile (`RUN2_FINDINGS.md`). No lock dominates in the heavy level, so
  the spin row below has no evidence behind it here. The first gains went to the port's own record path in `0.0.15-log9`. The guest
  mixers `sub_827E51D0` and `sub_827F5990` are named candidates for native rewrites.

## Candidates

"TCS today" was read from this tree. Gains are bo2-360-nx's.

| Technique | Its gain | TCS today | Cost and risk |
| --- | --- | --- | --- |
| Rewrite hot guest functions natively | 1.4-2x per call (decoder, culling, skinning) | Done for the CRT routines, LZ2K and reverb | Low per function; needs the profile to choose them |
| Run a rewrite beside the original and compare, then keep a checked sample | Safety | Not present | Low; supports the row above |
| Brief spin before a contended lock sleeps | About 75 % of samples were in `svcArbitrateLock` in its early profile | Not present | Low; run 8 showed no such waits |
| LTO | +9.8 % | Not used; `GameCode` is `-O2` in unity units | Build memory: 349 MB of generated C++ on the current 15 GB machine, with at most two jobs. May need a subset |
| Direct, inlinable calls between guest functions | Named as the base for LTO | Calls go through `PPC_WEAK_FUNC`, which is `weak,noinline`, so LTO could not inline them | Medium: must keep the functions the runtime hooks |
| PGO trained on the console | +9.0 % | Not used | Medium: needs a training session on hardware, and a retrain after code changes |
| Plain guest memory instead of volatile, with compiler barriers | +5.8 %, together with three other changes | `lsw_guest::load` and `store` are volatile | Medium to high: an error gives subtle faults. Audit against the scheduler's switch points; last in the order |
| Registers as C++ locals | Part of its first pass | Already on (`lswtcs_locals.toml`) | Done |
| Native XMA frame cache and mixer rewrites | +3.8 % and +2.7 % | Reverb only; the XAudio render thread uses 40-48 % of a core in heavy scenes | Medium; check the profile first |
| Logs on their own thread | +1.0 %, worst frame 5x shorter | Done in `0.0.13-log7`, gain unmeasured | Done |
| Fourth core from a forwarder, with audio and XMA alone on it | +12-16 % | Three cores | Its author warns it could in theory make the console unstable. Owner decision |

## Not directly applicable

- **Its renderer gains** (+70-80 % from its first set, copy-engine uploads +9.5 %) rely on Vulkan and NVK patches. TCS uses OpenGL on
  Nouveau, where `gl_rt.inc` measured `glCopyImageSubData` slower. Worth rereading if Deko or Vulkan is reopened.
- **Thread priorities.** It measured -2 to -5.4 % and dropped them, which fits the FPS cost seen with `LSWTCS_AUDIOPRIO` in run 7.
- **Presets and hiding far objects** are its main route to 60 FPS. Any such change here is a product decision, never silent.

## Guest threads on several cores

This is the largest structural difference. Each of its guest threads is a Horizon thread, with the 360's atomics behind a global
lock. TCS runs one guest thread at a time on core 0, under a scheduler built to keep the game stable (`kernel_stubs.cpp`). The
small step in that direction, `LSWTCS_WALKOFF`, went with black tile bands, a 20 s freeze and a picture that stopped updating in run 7.

It may hold the largest gain and is also the largest risk. Keep it out of routine work until the profile shows the turn itself is
the limit and the owner approves. Its stated rule against speculative framework replacement applies (`AGENTS.md`).

## Suggested order

1. Read the first `LSWTCS_HWPROF` results and fix any single dominant wait. Done for run 8: the wait was this port's file polls.
2. Add compare-beside-the-original, then rewrite what the profile names, audio included if it shows.
3. Try LTO on a subset, on a console build compared with the current one in A/B or alternating runs. Add direct calls if it shows
   promise, then PGO.
4. Plain guest memory, after an audit.
5. Take the fourth core and the thread model to the owner, with these numbers.

Effort is unknown. A rough personal guess of 10-25 % for steps 2 to 4 in CPU-limited levels has no measurement behind it.

## How to compare

Follow its rule, which matches this project's: compare only at the same clocks, cores, FPS cap and scene, and a change of 1-2 % needs
whole alternating runs. `LSWTCS_ABCYCLE` covers switches that can change during play. Compiler changes need separate builds, so
the scene has to repeat by hand. A fixed benchmark path would help; none exists here yet.

# BO ownership review: null `device` at final deletion

Asks how the buffer passed to Nouveau's final deletion can have a null `device`, against the 1 November 2026 handoff.
W0–W5 position: W1, native BO fault. This review is **source reading only**: nothing
was compiled, helper-tested, emulator-run or hardware-run. Mesa 20.1 and libdrm's
`bufctx.c` are not in the handoff; they were read later at their recorded pins.
First invalidator: **unknown**. No repair is proposed.

## Main finding

A null `device` does not need a stray write. It is also what a **BO that was already
freed** can look like, and the handoff has not listed that reading. It can be checked
from existing crash logs, without a new hardware run.

1. `device` is the first field of `nouveau_bo`, so it sits at offset 0 of the block
   (`nouveau.h:125`; `nouveau_bo_new` allocates the private struct with `calloc`,
   `nouveau.c:363`).
2. Freeing a block makes rpmalloc store its free-list link at offset 0
   (`rpmalloc.c:2438`, deferred path `:2478`).
3. That link is often null. An active span's list is handed to the heap and cleared
   (`rpmalloc.c:2161-2162`), so the next block freed to it gets a null link. The
   deferred list starts empty in the same way.
4. The rest of the struct survives, including `refcnt` (0), `offset` and `fence`.
5. `pushbuf_kref` raises `refcnt` with no check (`pushbuf.c:133`), taking it 0 → 1.
6. `pushbuf_flush` drops it 1 → 0 (`pushbuf.c:290`, then `nouveau.c:505-506`), so
   `nouveau_bo_del` runs a second time on the same block.
7. `nouveau_bo_fence_wait` does not use the device, so it passes. The first bad use is
   `nvAddressSpaceUnmap(&nvdev->addr_space, bo->offset)` (`nouveau.c:347-348`). That
   matches the exact 009 site and call path.

Limit: the same symptom has other possible causes (ranked below). A free onto a
non-empty list would give a wild non-null `device` rather than null, so two null
crashes (003, 009) are compatible with this reading but do not prove it.

## Call path

| Stage | Location | Reference effect |
| --- | --- | --- |
| Create | `nouveau_bo_new`, `nouveau.c:356` | `refcnt = 1` (`:406`), `device` set (`:407`) |
| External ref/unref | `nouveau_bo_ref`, `nouveau.c:497` | Atomic count; `*pref` read and written non-atomically |
| Queue | `nouveau_bufctx_refn`, not supplied | Stores a BO in a list; unverified whether it takes a reference |
| Acquire for submit | `pushbuf_kref`, `pushbuf.c:94` | Internal `atomic_inc`, once per krec (`:133`) |
| Re-queue | `pushbuf_flush`, `pushbuf.c:298` | Joins `bctx->current` back onto `pending` |
| Re-acquire | `pushbuf_validate`, `pushbuf.c:377-378` | Calls `pushbuf_kref` for every queued BO |
| Submit | `pushbuf_submit`, `pushbuf.c:244-248` | Writes `fence` and `access` into each BO |
| Retire | `pushbuf_flush`, `pushbuf.c:285-290` | Drops each krec reference |
| Delete | `nouveau_bo_del`, `nouveau.c:341` | Fence wait, unmap, then `free` |

Line 298 followed by line 378 matters most. A BO left in `bctx->current` is acquired
again at the next validate, and `nouveau_pushbuf_kick` validates right after flushing
(`pushbuf.c:683-684`). Upstream `bufctx.c` at v1.0.1 confirms that `nouveau_bufctx_refn`
stores the pointer **without taking a reference**. So Mesa must clear that entry before
the BO dies, or the next validate raises the count of freed memory.

Mesa 20.1 at the recorded pin confirms the sharing: every nvc0 context uses the screen's
one pushbuf and client (`nvc0_context.c:381-382`) but has its own bufctx (`:384-389`).
`nvc0_invalidate_resource_storage` runs only for the context that invalidated the
resource. Fence-deferred frees (`nouveau_fence_unref_bo`, `nouveau_buffer.c:82`) run from
`nouveau_fence_update`, on whichever thread updates fences.

This shows the route exists. It does **not** identify a bufctx entry that outlived its BO.

## Threads and locking

- libdrm's pushbuf, krec and client map take no lock in the supplied source.
- `gl_backend.cpp:532` states that the Nouveau contexts share one GPU channel without
  locking, and relies on compile workers never submitting.
- Render thread, glthread worker and two compile workers all hold or use contexts on
  one screen (`gl_backend.cpp:151`, `:504`; `LSWTCS_GLWORKERS` defaults to 2). Sharing
  was enabled by `mesa_egl_share_edits.py`.
- "Workers never submit" is a code comment. I found no enforcement or measurement of
  it, nor of whether workers reach BO or pushbuf code at all.

## What the observer establishes

| Situation | Result |
| --- | --- |
| Wrapped call on a pointer in `Freed`/`Foreign` state | Stops as `Retired`, before the driver call |
| Field or count change seen at a wrapped call | Stops as `Changed`/`Refcount` |
| Free hook with `refs != 0`, or a repeat free | Stops as `EarlyFree`/`DoubleFree` |
| Stale pointer after the address became a new BO | **Missed**: passes as the new generation |
| `atomic_inc` at `pushbuf.c:133` | **Missed** when it happens; only its later effect can be seen |
| Write after the wrapper's sample | **Missed** |
| Which Mesa caller queued a stale BO | **Missed**: a stop at `pushbuf.c:290` names retirement, not the owner |
| Which threads reach BO calls | Only the last 512 events, and only in a stop or crash report |

Assumption to confirm from the 011 map or disassembly: calls from `pushbuf.o` into
`nouveau.o` pass through `--wrap`. Calls inside `nouveau.o` do not.

So 011 means the first-order double delete did not occur in one 6½-minute interval
under changed timing. The original crash was seen once in three Episode I trials, so
this is weak evidence against it. It does not exclude the reuse case.

## Ranked hypotheses

**H1. Stale reference to a freed BO, re-acquired by the pushbuf.**
Falsified if the BO in the 003/009 crash was never freed (see the next section), or if
a quarantine run ends in the same crash with no retired-BO stop.

**H2. Unlocked concurrent use of the shared pushbuf or client map.** A racing flush could
drop one krec entry twice, or lose an update, which frees a BO early and leads into H1.
Falsified for compile workers if a per-thread count shows they never reach BO or
pushbuf calls. That would leave the glthread and render-thread handover to check.

**H3. A live BO's first word is overwritten by another writer.** Examples are an overrun
from the preceding block, or a competing owner. Falsified if the other fields are
intact and the block is shown to be already freed. Supported if `offset` is also wrong.
Any link to the guest PCR overwrite (issue 4) stays unproved.

## Recommended next action

**Read the existing 003 and 009 `crash.log` excerpts.** The handler already writes
`pc`, `lr`, `far` and `x0`–`x28` (`platform_switch.cpp:229-233`). Cost: no build, no
run. Needed from a crash session, with personal paths removed:

| Value | Expected under H1 | Points elsewhere if |
| --- | --- | --- |
| `far`, `x0` | Small value: null plus the `addr_space` offset, or its first used field | Large or non-null value |
| `x1` (if still `bo->offset` at the fault) | Plausible GPU address | Zero or garbage favours overwrite or reuse |
| Registers holding the BO, using exact symbols | Pointer in the BO allocation size class | Not a heap pointer |

This narrows the field but does not identify the owner: a valid `offset` is compatible
with H1 and with an overwrite limited to the first word.

**Only if that stays consistent with H1:** one bounded observer change, behind the
existing one-run marker and off otherwise. Not implemented or tested here.

- **Quarantine:** in the free hook, keep known BO blocks out of `rpfree` in a fixed ring,
  and set `refcnt` to a large negative poison value. Addresses then cannot be reused
  during the run, which closes the reuse gap, and a stale increment cannot reach delete.
- **Owner identity:** add `--wrap` for `nouveau_bufctx_refn` and `nouveau_pushbuf_refn`,
  so a stale BO is caught at insertion with the Mesa return address.
- **Thread evidence:** add a small per-thread counter of BO calls, printed with the
  existing periodic `[BOCHECK]` line. This tests H2.
- **Cost:** 788 BO addresses were used in 011, so a ring of about 4096 pointers is
  enough. No new thread or file I/O. Existing lock shared with the allocator hook.
- **Identity:** generation plus caller offset, from the existing `Record` and `Event`.
- **Stop rule:** first retired-BO stop, one run. Without a stop or a crash, the result
  is "not observed": do not repeat it unchanged.
- **Reject if** it stops a route that otherwise plays, for example through allocator
  exhaustion, because the timing and memory would no longer represent the crash.

The poison hides the crash during that run by design, so a clean run there says nothing
about a fix. It must never ship as a repair, and neither should a null guard.

## Not done

- No local reproducer or negative control.
- Not every Mesa path that clears a bufctx bin has been audited against BO release.
- The 012 regression was not run here. One short 012 hardware session has since run on
  a fresh build with no crash, which says nothing about this intermittent fault.

# Suggested first contribution: BO ownership review

This is a proposal for the owner and Gabriel to agree on, not a message already
sent or authorization to change the active renderer. It is deliberately useful
without a full game build. Driver/concurrency experience is directly relevant.

## Question

How can the BO passed to final Nouveau deletion have a null `device` field, and
what is the smallest **discriminating** way to identify its first invalid
ownership/write transition rather than guard the eventual unmap?

## Read in this order

1. [Current status](CURRENT_STATUS.md) and [issue1](ISSUES.md).
2. `../reference-source/libdrm-nouveau/nouveau.c`, `private.h`, `pushbuf.c` and
   `nouveau.h`: actual public implementation/schema snapshots from the qualified
   v1.0.1 boundary. Provenance/hashes are alongside them.
3. `../source/switch/source/bo_check.cpp` and
   `../source/switch/include/lsw_bo_check.h`.
4. `../source/switch/third_party/rpmalloc/rpmalloc_switch.c` and platform exception
   integration in `platform_switch.cpp`.
5. Relevant submission/arena/shared-context code in `gl_backend.cpp` and the eight
   Mesa edit scripts. Fetching/building the whole driver is not the first step.

## Existing evidence to respect

-009 final release/unmap has a null BO device; exact failure-site localization
  is established, first writer/owner is not.
-011 observation was active through1,728,889 recorded events at its last persisted
  checkpoint, with no recognized violation. These are not1.7million independent
  proofs of correct ownership, and the observer can perturb scheduling.
- The real driver executes outside the metadata mutex. Deletion reads the device
  and unmaps before the allocator free hook. Internal submission acquisitions can
  bypass the external reference wrapper. A reused address alone does not identify
  a stale owner's generation.
- The native BO failure, guest PCR overwrite and movie/visual problems have not
  been linked. Do not combine them into a single theory without a falsifier.

## Deliverable

Return a short report or a small reviewable patch containing:

1. An annotated create/acquire/submit/retire/free call path with source references.
2. Which invariants the existing observer actually establishes and which it misses.
3. At most a few ranked hypotheses, each with a specific observable falsifier.
4. One smallest recommended next action: an owning-contract repair if supported,
   otherwise bounded observation at a newly justified boundary. State expected
   runtime/locking/I/O cost, event identity and stop/reject rule.
5. Any local reproducer and negative control, with honest mocked/GPU limits.

No generic rewrite, new frontend/resource framework, null-pointer suppression,
blanket glthread/worker disable or repeated "try it and see" console run.
Do not install tools or change clocks/quality/saves. Return unknown where the
evidence does not identify the invalidator.

## Alternative if Deko is the preferred collaboration focus

Agree a separate bounded existing-contract review with the owner. Use
[RENDERER_DECISION.md](RENDERER_DECISION.md): one attachment/resolve/fenced-lifetime
question, applicable source comparison and falsifier. Do not silently make Deko
the main implementation route or edit the same files concurrently.

## Another useful independent task

Fresh-generation parity can also be valuable: with your own executable, reproduce
the recorded folded baseline and current patched hashes. The two generator patch
states and their qualification limits are in [BUILDING.md](BUILDING.md). This
would close a real onboarding/reproducibility gap before optimization work.

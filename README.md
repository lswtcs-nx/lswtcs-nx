# TCS360 collaborator handoff

Prepared **1 November 2026** for an initial collaboration. Last documented game
development/test checkpoint: **26 October 2026**. No GitHub account is needed to
review this folder or return a patch. No message, repository or upload was made
on the owner's behalf.

This is a source/review handoff, **not a playable game distribution**. It contains
the current maintained runtime and renderer source, configuration, cumulative
source-edit patches, dependency pins, a runnable isolated regression and project
context. Supply your own matching Xbox360 game when preparing a complete build.
No disc/game files, complete generated GameCode tree, saves, keys, SDK/compiler,
compiled game executable, shader packs or Git account/history are included.

## Read this first

The Discord discussion described continuing Deko. The current execution harness
instead makes **Mesa/OpenGL/Nouveau the primary route**. Deko source is included
as unfinished experimental work. That conversation did not establish a measured
GPU bottleneck or authorize a new frontend/renderer migration.

Mesa has run real gameplay and native saves on the owner's Switch, with poor
combat pacing/audio jitter and intermittent visual corruption still unresolved.
**Mesa012 is compiled and locally qualified; its hardware test is pending.**
Earlier Deko capture/replay successes do not establish live playable Deko.

## A useful first hour

1. Read [current status](docs/CURRENT_STATUS.md), then
   [architecture and code map](docs/ARCHITECTURE.md).
2. Run [the asset-free local regression](docs/VALIDATION.md) with an already
   installed Python and C++20 compiler. It compares actual old/new tile planners.
3. Read [the suggested bounded first task](docs/FIRST_TASK.md). A native
   graphics-buffer ownership review fits a driver-development background and
   does not need a complete game build or another generic renderer rewrite.
4. Use [collaboration instructions](docs/COLLABORATING.md) to return findings or
   changes against this snapshot. Agree on file ownership before concurrent edits.

## Deeper context

- [Building and exact reproduction limits](docs/BUILDING.md)
- [Open defects and evidence](docs/ISSUES.md)
- [Mesa versus Deko: what is known](docs/RENDERER_DECISION.md)
- [Existing execution harness](docs/TCS360_DEKO_IMPLEMENTATION_PLAN.md), revision2;
  its DEKO filename is historical. W0–W5 remains the one project plan.
- [Current checkpoint](docs/NEXT_SESSION.md), [product register](docs/TCS360_CURRENT_BACKLOG.md)
  and [pinned reference review](docs/TCS360_MESA_REFERENCE_REVIEW.md)
- [Input identifiers](provenance/expected-inputs.json),
  [dependency pins](provenance/dependencies.json),
  [license/provenance notes](notices/README.md)

The old owner workspace's top-level documentation had fallen behind the prepared
revision2 harness. This export uses the prepared current harness and a matching
source snapshot; do not resurrect the old Deko-first queue from historical notes.

## What this handoff has and has not validated

The local source build already passed for the owner's Mesa012 candidate.
Dependency source patches and the cumulative generated-source edit patch have
been checked against their local pinned/preserved preimages. The game patch
recreates the current normalized output across **315 changed files** from the
existing **3,166-file folded baseline**, yielding **3,169 files**.

That is **not a fresh generation or clean-room full-build test of this exported
folder**. Generation/compiler drift is an explicit setup gate; use the included
baseline/target hash manifests and stop on a mismatch. The asset-free regression
is the quickest independently usable starting point.

This folder has no owner Git history or account configuration. Known personal
names/home paths have been removed from exported documentation; required upstream
attribution and license notices remain. A hash manifest identifies the supplied
files. The working checkout, build objects, staged game and saves were not changed
to prepare the handoff.

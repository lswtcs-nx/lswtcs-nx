# TCS360 collaborator handoff

## Update, 8 November 2026: performance work since this handoff (read first)

The handoff below is the **1 November** snapshot and still describes the project as it was then. Since then the
collaborator's work (runs 10-102 on a real Switch) changed the runtime, the renderer and the build. Start here:

- **[CPU roadmap](docs/reviews/CPU_ROADMAP.md):** the summary for whoever takes over. Its last section, "State on 8
  November 2026", has the current figures per mode, what limits each mode, the next candidates and the open issues.
- **[Run-by-run findings](docs/reviews/RUN2_FINDINGS.md):** every build and hardware session, newest last, with the
  evidence behind each decision.
- **[Build guide](docs/BUILDING.md):** "The current build (log98)" gives the recipe of the build to play.
- **[Player profile](profiles/player.env):** `env.txt` without diagnostics. `profiles/hardware-ab.env` is the test
  profile used in the sessions.
- **Other reviews** in [docs/reviews](docs/reviews/): `BO2_OPTIMIZATION_PATHS.md`, `BO_OWNERSHIP_REVIEW.md`,
  `PORT_RESEARCH.md`, `FRESH_BUILD_NOTES.md`.

**Measured on the Gungan route (hub, Episode I chapter 2):**

| | Handheld | Docked |
| --- | --- | --- |
| Average FPS, whole session | 41.6 | 48.9 |
| Heavy combat frames | ~29 FPS | ~31 FPS |
| Main-thread CPU per recompiled guest call | 157 ns before, 127 ns now | 162 ns before, 124 ns now |

Other levels were not measured.

**Decisions for the owner.** Each is in the current build and can be undone:
1. **The handheld GPU runs at 460.8 MHz by default.** It is the official configuration `0x92220008`, not an
   overclock; the memory clock is checked and the setting reverts if it moves. This handoff's rule was stock clocks.
   `LSWTCS_GPUBOOST=0` restores 307.2 MHz. In-session A/B: +9.7 % FPS in heavy frames, +27 % in medium ones.
2. **Guest memory without `volatile`**, with `-fno-strict-aliasing` and compiler barriers on guest memory only:
   about -11 % main-thread CPU. It behaved normally in three sessions, all in Episode I.
3. **Profile-guided optimisation:** about -10 % more. The counts come from a session on the instrumented build and
   are not included (game-derived). Rebuild them with one 15-20 minute session, or build without PGO and keep the
   gain of item 2.

**Open:**
- the title-screen movie (issue 3 below);
- a block of binary bytes in `stdout.log`, seen only with the current build;
- a floating-point contraction note (`-ffp-contract`) that needs the owner's review.

Details are in the roadmap's last section.


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

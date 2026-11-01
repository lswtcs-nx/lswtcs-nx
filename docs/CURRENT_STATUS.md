# Current status and acceptance

Snapshot prepared1November2026. The latest documented implementation is Mesa012
from26October; no012 hardware result has been supplied in this conversation.

## Whole-project position

| Package | Meaning | Status |
| --- | --- | --- |
| W0 | Reproducible Mesa baseline and cost classification | Five baseline collection gates complete; this is not full correctness. |
| W1 | Runtime, memory, movies, graphics ownership and rendering correctness | **Active.** Several independent release-blocking faults remain. |
| W2 / H1 | Shader-ready ordinary title/menu startup | Acceptance remains open. H1 means title/menu, not gameplay. |
| W3 / H2 | Measured gameplay, combat, transitions and saves | Route-specific optimization/acceptance remains open. |
| W4 | Broader content, co-op, controls and product integration | Later coverage, not implicitly accepted by one chapter. |
| W5 | Sustained acceptance and adoption decisions | Not reached. |

Whole-project percentage/remaining effort is unknown. The current012 candidate
has **4/5 preparation/acceptance gates**: reproducer/audit, implementation, local
regressions and integrated build passed; affected hardware route/cost remains.
This80% is not a port-completion estimate.

## Version history that affects the next decision

| Version | Change/result |
| --- | --- |
| Mesa004 | Owner reached title/gameplay; native saves and separate users worked. Preserved fallback, not full-game acceptance. |
| 005 | Existing timing/logging streamlined. A/B sessions did not isolate logging overhead quantitatively. |
| 006 | Texture image identity separated from sampler state to reduce duplicate preparation/uploads. Owner reported smoother gameplay/repeat visits. Combat drops, audio jitter and visual errors remained; no controlled numeric speedup. |
| 007 | Audio queue readiness/waiting corrected. Owner reported no noticeable audible/FPS improvement. |
| 008 | Removed an invalid page-age shortcut across explicit vertex/index epochs. Local counterexample fixed; owner's visual glitches/performance did not noticeably improve. |
| 009 | One-run input-reuse diagnostic. Crashed in native Nouveau BO deletion/unmap with a null device field. Exact symbols localized the failure; first invalidator unknown. No mismatched input capture was obtained. |
| 010 | Bounded native BO lifecycle observer. Armed on hardware without a reported crash, but report persistence was incomplete. |
| 011 | Initial report plus periodic coverage persistence. Hardware logged16 running samples and1,728,889 events at last checkpoint, no recognized violation. Diagnostic delivery passed; crash cause did not reproduce. |
| 012 | Existing tile-merger identity strengthened: recorded state/topology, system constants outside permitted tile changes, and all recorded upload span bytes.337 local cases/ARM/static passed. Hardware pending. |

## Hardware context and evidence limits

- Owner tests default to **handheld, stock clocks**. Exact silicon/model is not
  independently established here; do not assume another port's raised-clock FPS.
- The usual route is cantina → EpisodeI Chapter1 → combat/camera/movement → return,
  sometimes revisited. Exact battle timestamps are not consistently annotated.
- Assume no crash when the owner reports none/supplies no crash log. Do not keep
  asking for that confirmation. Unreported clean-exit/reload acceptance is separate.
-011 has10 successful save commits, empty stderr and roughly6½minutes of frame
  records. No final runtime/main return is logged. That is not proof of an exit
  method, leak, fixed crash or fixed visual output.
-012 has not executed the game on this host, an emulator or hardware. Local
  regressions mock environment/MSAA helpers and do not execute GPU shaders.

The compact supplied evidence is in `../evidence/`. Full owner logs, personal
save identity, game captures and matching executable/symbol binaries stay local.
Ask for a targeted additional excerpt when a concrete question requires it.

## Latest candidate identity

- Version0.0.12, OpenGL/Nouveau Mesa.
- NRO SHA256 `b1d58d5d04fa950f65ca0e0a026d922d46749bbd7c18810ff9e2205eb62ecce5`.
- ELF SHA256 `cfff4a1255a9a8daf40851891f3dd1d041d6b506914ad379bd93e7e6ae046c4d`.
-4996 compiler inputs were pinned; only GL backend/bridge/header inputs changed
  from011, plus version metadata. Runtime recorder, frozen draw ABI, BO observer,
  allocator, driver, audio and native saves were unchanged.
- Existing633 Mesa and633 inactive-Deko GameCode objects/PCH and Deko cache were
  preserved during the one-job incremental build.

Hashes identify owner artifacts; those binaries are not contained in this folder.

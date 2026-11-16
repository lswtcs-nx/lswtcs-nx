# Current handoff â€” Mesa first, updated26 October 2026

Read the revision2 canonical harness `docs/TCS360_DEKO_IMPLEMENTATION_PLAN.md`,
`local/session-state.json` and AGENTS. The filename is retained for stable links;
the active renderer is existing OpenGL/Nouveau Mesa. the project owner requested the choice
and a full harness; Mesa was recommended and is the current-session direction.
Deko is preserved and inactive, not deleted or considered completed.

If this handoff is being read in the prepared scratch update rather than the
project directory, check DOCUMENT_UPDATE_STATUS.json: do not claim project
files were replaced. Previous handoff/plan/state were archived intact alongside
this update. They are historical evidence, not next-step instructions.

Current configuration policy: B may remain enabled during development after006.
A is optional for lighter ordinary-play logging. This supersedes older mandatory
restore-A wording below; routine updates should preserve active owner settings.

## Current checkpoint â€”012 recorded draw-identity correction built/staged

011 hardware negative active BO interval does not identify a fix. Reviewed its
known gaps against exact009 failure: wrapper samples precede real ref/delete;
device load/unmap precedes allocator free hook; a later write/race or stale owner
after pointer reuse remains possible. Observation changes scheduling; internal
increments not individually journaled. No extra BO probe or null/worker guard
justified. Keep BO crash separate/open and one-run observer available, marker off.
Review facts/hashes in mesa-draw-identity/bo-observation-review.json.

Selected existing W1/R1 replay-identity violation because actual old/new functions
have a local reproducer: untiling accepted unequal recorded upload bytes/topology
and other state as equivalent, dropping later tile draws. This is independent of
owner photo attribution, which remains unproved. No scene/frame/address exception.

012 strengthens the existing draw_content_key used by plan_untile and repeat
diagnostics: topology/index width, shader/GS objects, fixed state/MRT/blend/stencil/
outputs/bindings, viewport outside Y, non-tile system/clip constants, and every
recorded upload range/address/length/immutable bytes. Old key inspected only first4
range addresses/lengths and omitted these fields/data. Shader bridge's actual
SystemConstants schema owns normalization, excluding only fields patched by
lsw_gpu_tile_state (NDC scale/offset, point radius for points). Unsupported block/
span/index width calculation/descriptor count returns unknown key; zero keys never
authorize a merge or count as equal diagnostic duplicates. Index byte counts use
64bit arithmetic. Original normal replay path remains for rejected groups.

No new cache/logger/thread/snapshot framework, persistent metadata or frozen draw
ABI. Reuse existing key vectors, XXH3 and immutable arena; bridge helper has608byte
temporary stack block. Checking and previously wrongly suppressed draws can add
CPU cost; console effect unknown. No quality, clock, content, audio or game-speed
change. No claim of FPS gain.

337 actual-function local cases pass:26 formerly accepted unequal cases rejected,
one prior inline-index rejection retained,3 allowed tile/system cases preserved,
7 unsupported/bounds cases decline merge,300 legitimate old/new plans identical
(2/3tiles, clear ordering, MSAA, scissor variations and relocation). Actual frame/
draw structs, XXH3 and actual before/after planner/key code; system struct extracted
verbatim with uint32 Endian stub, env/MSAA helpers mocked. No GPU pixel execution
or screenshot root-cause proof. Host synthetic timings are not console performance.

Important boundary: f.ranges are upload DELTAS, not complete resident vertex spans.
This fixes the recorded key, not earlier tile_skip_copy's assumption about actual
current guest inputs. Texture pixel mutation/resolve dependencies and unobserved
writers remain outside the key. Retain those open; do not equate stronger keys
with fully proven tile composition or repaired visual output.

One-job incremental ARM/static build passes; only2 compile units. All633 Mesa/
Deko GameCode objects/PCH and Deko cache preserved. Actual final ARM draw key
calls bridge helper compiled against real schema.4996 compiler inputs rehashed:
only gl_backend.cpp/gl_bridge.cc/lsw_gl_shader.h changed; CMake Mesa version0.0.12
separate, Deko0.0.7 unchanged. Recorder/frozen ABI/BO/driver/allocator/audio/save
sources identical011. Exact011 NRO/ELF/map/NACP retained. Same app/staging root,
B env byte-identical. Only agent PC staging markers removed; no SD file deletion,
new branch/SD variant/ZIP/installation/manual backup.012 game/emulator/hardware
unrun; compiled/helper/static/link/staged claims only.

012 NRO 115026117bytes SHA256 `b1d58d5d04fa950f65ca0e0a026d922d46749bbd7c18810ff9e2205eb62ecce5`;
ELF `cfff4a1255a9a8daf40851891f3dd1d041d6b506914ad379bd93e7e6ae046c4d`;
map `af19d21081414b30256b82d4174b7478558873e9b43afdbae5b77525b9089789`.
Evidence: logs/tcs360-project-harness-20261026/mesa-draw-identity/ has source patch,
reproducer/337case report, build/static/link/input/preservation/candidate/staging.

Next owner gate: copy012 NRO only to /switch/lswtcs/lswtcs.nro, confirm hbmenu
0.0.12, keep env/content/caches/saves. Do not rearm either diagnostic marker.
One usual5â€“10minute handheld/stock EpisodeI Chapter1 movement/camera/combat.
Observe atlas spikes and separate shifted/duplicated lower-scene flashes; report
each independently, plus material slowdown/audio/control change. Save/close as
usual. Collect existing stdout/stderr/frames/crash if present before relaunch.
Question: visible correctness and cost after recorded invalid merges are rejected.
Finite clean run does not establish root cause. No movie repeat or button timing.
Refine cost/remaining inputs from result, never restore proven unsafe merging as
a performance fix.

W0 complete -> W1 correctness ACTIVE (R1 recorded draw identity) -> W2/H1 startup
-> W3/H2 measured gameplay -> W4 content/co-op -> W5 acceptance. This candidate
4/5(80% named checklist: reproducer/audit, implementation, actual-function checks,
integrated ARM/static identity done; physical affected-route acceptance remains).
011 diagnostic bring-up4/4 is closed separately; no logger retest. Whole BO/visual/
movie/audio/game repair effort unknown. Prepared handoff current; root sync
unverified. Hardware gate is not a promise that gameplay/full port is nearly done.


# Open issues and what evidence would close them

These remain independent until an actual trace connects them.

## 1. Native Mesa/Nouveau crash

The exact009 crash is in `nvAddressSpaceUnmap`, reached from final BO deletion/
`nouveau_bo_ref` during `pushbuf_flush` on Mesa's glthread drawing worker. The
device pointer loaded from the BO is null. Earlier003 reached the same invalid
device/unmap boundary via an arena-fence path; a common first invalidator is
unproved. Shader discovery near the crash is correlation, not race proof.

The011 observer has4096 address slots and512 recent events,471288bytes fixed
metadata. Its one-run marker is consumed before graphics initialization. A known
violation writes history and aborts before that recognized bad operation;
unknown/table-capacity states are incomplete coverage. Driver calls run outside
the observer lock.011's negative interval did not capture the original failure.

Known gaps: a write between precheck and real driver access; native deletion's
device load/unmap before the allocator callback; a stale owner after pointer
reuse; internal reference increments; observer timing changing the race.
Do not hide the invalid device with a null guard or claim a clean run repaired it.

Useful supplied material: [BO review](../evidence/bo-observation-review.json),
[011 counters](../evidence/mesa011-summary.json), and public
`../reference-source/libdrm-nouveau/` implementation/header with source provenance.

## 2. Intermittent visual corruption

The owner photographed atlas/glyph-like stretched triangles and separately
reported a shifted/duplicated lower half of the scene. They occur occasionally
for one or two frames and were not simultaneous. Their common cause is unproved.
008 did not visibly fix them.009 did not yield a matched invalid-input capture.

012 rejects reproduced unequal recorded-input merges. The asset-free reproducer
compares actual old/new key/planner functions. It does not execute the GPU, prove
the screenshots' cause, validate earlier walk-time copying or capture every
resident texture/geometry dependency. Physical behavior and cost remain pending.

Next decisive evidence is a corrected owning contract plus affected-route output,
or a matched first-invalid input/composition trace. Avoid texture/shader/address/
scene exceptions and asking the owner to press capture faster than a single frame.

## 3. Title-idle movie corruption and completion hang

The natural title-idle DEMOINTRO.WMV route produced moving corrupt pixels, audio
continued, inputs did not recover, and playback end froze. Chapter cutscenes
reported working are a separate path. Omitted PowerPC CR/VMX decoder operations
were repaired/checked locally; that did not prove the first corrupt pixel source.

Need the same frame's decoder-ready bytes, plane strides/format, upload input and
displayed output, plus ring reset/callback/event completion. Do not skip the movie
or import another game's memory offsets/decoder as a symptom fix.

Run 85 (log82): with A/B state I the movie stopped after its first frame on a
black screen. The ring walker thread dropped XMedia's surface-retire GPU
interrupt (callback 0x82815910). Log83 hands those interrupts to a waiting
guest thread; see RUN2_FINDINGS.md "Run 85".

Run 86 (log83): the movie played on, but as green/magenta blocks and at about
3 frames a second. The decoder still ran through about 1,650 VMX operations
that the generator left empty. Log84 fills them in the movie libraries
(`workspace-tools/fill_tcs360_movie_ops.py`, checked on the PC by
`tests/fill_ops_check.py`); see "Log 84". The speed remains open: it needs a
native decoder or a much faster path.

Run 87 (log84): shapes appeared, but most of each frame was zero (dark green).
Likely cause: the surface-retire interrupt ran when the walker read it, before
the renderer read the surface, so the decoder was already rewriting it. Log85
queues those interrupts until the frame is replayed. Skipping the movie ends the
process with no handler output; log85 logs presses and guest thread exits.

Run 88 (log85): black screen again after two frames. The render thread only
replays on the game's next VdSwap, which waited for the movie, so the queued
interrupt waited 500 ms. Writing back scratch register 1, which belongs to
VBlank, likely stopped the movie for good. No pad press reached the game,
because nothing polls the pad while the main thread waits. Log86 writes back
only registers 0, 4 and 5, and requests a replay when a frame is published
while interrupts wait.

Run 89 (log86): every interrupt now waits for its replay, but the movie
stopped after three frames, with all its threads waiting on events. Separate
defect: `KeWaitForSingleObject` polls, so a set that is cleared before the next
poll (or a pulse) never wakes the waiter. Log87 gives kernel wake-up semantics
(`LSWTCS_KEWAKE=0` off; this affects every event wait) and reports infinite
waits past 3 s (`[KWAITLONG]`).

Run 90 (log87): same stop; no wake-up was missed. The decoder waits on the
player's event 1 and the main thread on its event 3, a deadlock that appeared
only once interrupts were delayed. Log88 runs interrupts at once again and
copies, at each interrupt, the bytes of every texture the unreplayed frames
sample; the renderer decodes from the copy (`LSWTCS_TEXSNAP=0` off).
`LSWTCS_KEWAKE` is now off by default. Untested on hardware.

## 4. Guest PCR overwrite / allocator overlap

Guest PCR memory at0x88010000 was overwritten; allocator ownership domains can
overlap. First invalid claim/write and full lifecycle cause remain open. Trace
the owning ranges and host-thread → guest-thread → r13/PCR contract. A guessed
PCR relocation does not repair competing owners. No link to the native BO crash
or movie pixels is established.

## 5. Combat frame pacing and audio jitter

006 reduced redundant texture preparation and the owner noticed improvement.
Combat still drops severely and affects audible playback.007 did not noticeably
improve that. Warm gameplay often has no shader holds; compilation alone does
not explain the steady cost. Native rendering, shader translation, recompiled
game CPU, recorder work, driver work, GPU work and waits must be measured separately.

Existing GL profiles include CPU-side API/driver waits. A field named `gpuwait`
is a CPU fence/completion wait, not pure GPU execution time. Long display/query
spans are not necessarily useful GPU work. Do not claim60FPS feasibility from
visual simplicity or extrapolate emulator/overclocked results to stock hardware.

Audio needs completion → credit-ready → scheduler admission → callback → submit/
device-empty attribution before priority, buffering or time-stretch changes.

## Product coverage

Native per-user saves work on bounded owner routes. Save dates, controller glyphs/
platform wording, local co-op/reconnect, brightness/audio modes, broader chapter
coverage and launcher packaging remain in the existing product register.
An Android-style single-player arcade-fighting idea in the Discord conversation
is future product discussion, not a current implementation acceptance gate.

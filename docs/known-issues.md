# Known issues

The open issues, for contributors. Players need only the README's short list. Each entry says what is known, where
the evidence is, and what is still open.

## Title-idle movie: its second play may crash

Leaving the title screen idle plays the attract movie (`DEMOINTRO.WMV`). Chapter cutscenes are a separate path and play
correctly.

- **Fixed:**
  - The ring walker dropped the surface-retire interrupt (callback `0x82815910`): black screen. Fixed in log83.
  - About 1,650 VMX operations, and in log127 32 more `eqv`, left empty by XenonRecomp
    (`workspace-tools/fill_tcs360_movie_ops.py`, checked by `tests/fill_ops_check.py`).
  - The corruption (log127): the decoder's transform and motion-compensation fetch read VMX128 registers `v64`, `v65`,
    `v69`, `v72` that nothing in the recompiled game sets. The runtime now gives them their masks
    (`LSWTCS_MOVIEPERM=-1` turns it off). In the emulator the decoded Y, U and V planes match the trailer. Not yet seen on
    the console.
  - Speed (log129): the video now goes through FFmpeg's WMV3 decoder instead of the recompiled one, which ran at about
    a third of real time on the console (run 122: sound and picture drifted apart, and the end looked hung). In the
    emulator the trailer decodes in real time with no errors and the game returns to the title. Not yet seen on the
    console. `LSWTCS_MOVIEFF=0` restores the game's decoder. On the console (run 123): 16 ms a frame, good picture.
  - The black screen at the end (run 123): the scheduler gave the turn to the player's exited demux thread, which a
    resume had put back in the rotation. Fixed in log130 (exited threads stay out; a stall watchdog); confirmed on the
    console in run 124.
  - Tearing (run 123): the player rewrote a texture set the render thread had not yet read. Since log132/log133 the
    movie's fence waits also wait until the renderer is at most one frame behind (`LSWTCS_MOVIEFENCE`,
    `LSWTCS_MOVIEFENCELAG`). On the console (run 126): no tearing, the 61 s trailer in 73.5 s (68 s before).
- **Still open:**
  - The movie's second play (the title left idle again): when the same player plays again it works (log131; on the
    console in run 125). When the game makes a new player, its memory no longer fits (the game keeps its earlier heap
    segments, and here a reservation costs real memory): since log137 the player fails and the screen stays black, the
    game still running; restart the app. Experiments that give it room (log134-136, `LSWTCS_VEXT`, `LSWTCS_VEXTRA`) are
    off by default: they broke on the console (run 127) or froze in the emulator.
  - The render thread spent about 44 ms a frame on the movie's planes on the console (run 122); in the emulator the game
    now shows about 19 of the trailer's 30 frames a second.
  - The skip cannot work while the main thread waits inside the player, because nothing polls the pad.

Evidence: [progress-log.md](progress-log.md), "Run 85" to "Log 88", "log127", "Run 122", "log129", "Run 123", "log130", "log131", "Run 124", "log132", "Run 125", "log133", "Run 126", "log134", "log135", "Run 127", "log136" and "log137", and
[history/2026-11-01-handoff/ISSUES.md](history/2026-11-01-handoff/ISSUES.md), issue 3.

## Heavy scenes on handheld run at about 30 FPS

The GPU limits them on handheld, at Nintendo's 460.8 MHz handheld configuration. Docked, the limit is the chain from
the game's frame to the GPU. See the last section of [reviews/CPU_ROADMAP.md](reviews/CPU_ROADMAP.md).

## First visit to a level stutters briefly

Shader programs are prepared the first time a level needs them. The game keeps a list of them in
`sdmc:/lswtcs/glprogs2/`, so later visits and the next boot prepare them in advance. With four cores, a second
compiler on core 3 prepares the saved list in the background; the queue empties in about two minutes after boot.

Since log109 the translations themselves are saved in `sdmc:/lswtcs/glprogs2/xlate.bin`. Run 117 loaded 175 in
184 ms at boot and the hub's longest frame fell from 976 ms to 391 ms; what remains is the area load (textures and
programs). Since log122 (v0.4.0) the file is tied to the translator's sources, not the build, so only a build that
changes the translator translates again, about 21 ms a program, mostly in SPIRV-Cross and the
GLSL clean-up. Open: one 1.6 s wait in Mesa at the hub's first frame in run 116, seen once.

Since log116 (release v0.3.0), nouveau's own translation of each shader stage is kept in `sdmc:/lswtcs/mesacache/lsw_nvc0.bin`, across
builds of the port. Run 119's second session loaded 1348 of them: programs built in 28 ms of worker time instead of
170-240, the saved list was done 36 s after boot, and frames held for programs fell from 0.9-2.6 s to 0.1 s. The session
that first writes the file pays extra (log117 writes it in blocks to cut that). `LSWTCS_GLXLATEASYNC=1` (off by
default) moves most of the shader translation off the game's thread; not measured on the console yet.

## Short audio dropouts in the heaviest scenes

About 1-2 % of a session, in the heaviest combat. Run 107 measured 2.6 s of silence in 203 s, against 8.7 s in run
106. `[AUDIO]` in the testing profile counts them.

## Crash in the GPU driver: a buffer with no device

A buffer object reaches its final deletion with a null `device`, during a command flush. It has not been seen in the
recent sessions, including a 93-minute one, but its cause is not fixed. A freed buffer would look the same, because
rpmalloc stores its free-list link at offset 0. See [reviews/BO_OWNERSHIP_REVIEW.md](reviews/BO_OWNERSHIP_REVIEW.md).
On 18 November 2026 it happened once in the emulator (Ryubing), in the hub, with log114: render thread,
`arena_fence` -> `nvc0_flush` -> `nouveau_bo_del` -> `nvAddressSpaceUnmap` at address 0. So it can be chased there.

## A block of binary bytes in `stdout.log`

About 2.5 KB of high-entropy data appears in some sessions, before an `[OBJACT]` burst. It does not affect play. The
object-name print writes only printable bytes and the async log ring looks correct, so the source is open. See
[progress-log.md](progress-log.md), log98.

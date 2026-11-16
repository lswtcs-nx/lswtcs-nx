# Known issues

The open issues, for contributors. Players need only the README's short list. Each entry says what is known, where
the evidence is, and what is still open.

## Title-idle movie: corruption and a hang at its end

Leaving the title screen idle plays the attract movie (`DEMOINTRO.WMV`). The picture shows moving corruption, and the
game hangs when the movie ends. Chapter cutscenes are a separate path and play correctly.

- **Fixed so far:**
  - The ring walker dropped the surface-retire interrupt (callback `0x82815910`), which made the screen go black. Fixed
    in log83.
  - About 1,650 VMX operations of the movie decoder were left empty by XenonRecomp. log84 fills them
    (`workspace-tools/fill_tcs360_movie_ops.py`, checked by `tests/fill_ops_check.py`).
- **Still open:**
  - The decoder costs about 280 ms of guest work per frame, so the movie runs at about 3 frames a second.
  - The skip cannot work while the main thread waits inside the player, because nothing polls the pad.
  - Whether the remaining corruption comes from timing or from decoding.
- **Tried and rejected:** deferring the interrupts until the replay (`LSWTCS_AWDEFER=1`, deadlocks after three
  frames) and kernel wake-up semantics (`LSWTCS_KEWAKE=1`, released nothing).
- **Next evidence:** a session on the movie with `LSWTCS_TEXSNAP`, then `[TEXSNAP]` and a capture.

Evidence: [progress-log.md](progress-log.md), "Run 85" to "Log 88", and
[history/2026-11-01-handoff/ISSUES.md](history/2026-11-01-handoff/ISSUES.md), issue 3.

## Heavy scenes on handheld run at about 30 FPS

The GPU limits them on handheld, at Nintendo's 460.8 MHz handheld configuration. Docked, the limit is the chain from
the game's frame to the GPU. See the last section of [reviews/CPU_ROADMAP.md](reviews/CPU_ROADMAP.md).

## First visit to a level stutters briefly

Shader programs are prepared the first time a level needs them. The game keeps a list of them in
`sdmc:/lswtcs/glprogs2/`, so later visits and the next boot prepare them in advance. With four cores, a second
compiler on core 3 prepares the saved list in the background; the queue empties in about two minutes after boot.

## Short audio dropouts in the heaviest scenes

About 1-2 % of a session, in the heaviest combat. Run 107 measured 2.6 s of silence in 203 s, against 8.7 s in run
106. `[AUDIO]` in the testing profile counts them.

## Crash in the GPU driver: a buffer with no device

A buffer object reaches its final deletion with a null `device`, during a command flush. It has not been seen in the
recent sessions, including a 93-minute one, but its cause is not fixed. A freed buffer would look the same, because
rpmalloc stores its free-list link at offset 0. See [reviews/BO_OWNERSHIP_REVIEW.md](reviews/BO_OWNERSHIP_REVIEW.md).

## A block of binary bytes in `stdout.log`

About 2.5 KB of high-entropy data appears in some sessions, before an `[OBJACT]` burst. It does not affect play. The
object-name print writes only printable bytes and the async log ring looks correct, so the source is open. See
[progress-log.md](progress-log.md), log98.

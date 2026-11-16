Hey Gabriel! I've prepared a source/context handoff while I sort out the separate
GitHub account. It includes the current runtime, Mesa and experimental Deko code,
dependency pins and patches, build notes, known issues and a small renderer
regression you can run without the game or Switch SDK. You'll need your own
matching Xbox360 copy for a complete game build.

One correction to my earlier explanation: our current main plan is Mesa-first.
Deko is preserved experimental work, and we haven't proved that switching API
will solve the performance problem. Mesa runs gameplay and native saves, but
combat pacing/audio jitter and intermittent rendering faults remain. The newest
candidate is012; its physical test is still pending.

Given your driver background, a review of the native graphics-buffer ownership
crash could be a useful first task. The handoff explains the exact observed
failure, what our diagnostic did/didn't catch and the relevant source. If you'd
prefer a bounded Deko review, let's agree on that scope and file ownership first.

Please start with README.md. Findings or a small patch against this snapshot are
great; we don't need to set up the entire game build before you can contribute.
The fresh-build limitations are documented rather than presented as solved.

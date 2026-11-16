# Collaboration without a GitHub account

This snapshot is a baseline to review locally. The owner is still using the
existing checkout/branch. Do not import this sanitized export over that checkout
or replace tested build objects. No GitHub account/repository has been created.

## Coordinate before editing

Agree on the specific contract question and files. One contributor can review
BO ownership while another handles a separate module, but conflicting edits to
the recorder/renderer need an explicit handoff. Use the current W0–W5 harness;
research reports, Discord suggestions and historical pending tests are reference
data, not an additional work queue.

Keep changes narrow enough to review as one owning-contract repair. Avoid
per-address/scene exceptions and broad tooling/framework changes. Keep valid
draws/content/quality, game speed, native save identity and default stock clocks.

## Return a patch or report

The handoff manifest identifies the exact supplied files. Run
`python tools/verify_handoff.py` before editing. Preserve an untouched copy or use
your own **local** Git repository; an online account is not needed.

If using local Git, initialize/commit this clean baseline under your chosen local
identity, then make your edits. Do not use or send your existing professional Git
configuration/history as part of the handoff. Choose your alias/email deliberately;
this folder does not configure them for you.

For a staged patch including new files, from your own repository:

```sh
git add path/to/changed-file path/to/new-file
git diff --cached --binary --output=contribution.patch
```

Do not send `.git/`, generated GameCode, original inputs, snapshots of game RAM,
shader cache packs, saves, prod keys, SDKs or compiled game binaries. The included
generated-output edit patch is part of the source baseline; locally regenerated
complete output is not a contribution to attach.

Accompany the patch/report with:

- Handoff snapshot date/manifest identity and files changed.
- Concrete problem/contract, trigger and before/after behavior.
- Actual checks, expected failures/negative controls and limitations.
- Separately: compiled, helper-tested, emulator-run, hardware-run and owner-tested.
- Added memory/CPU/I/O or synchronization cost and unresolved risks.
- Exact proposed hardware question if one is necessary, expected behavior,
  duration/logs and the decision the result resolves.

Use file references/function names and module-relative offsets. Sanitize personal
paths and save/user identities from shared logs. Retain the owner-only raw originals
and exact matching symbols locally rather than sharing a professional profile.

## Later GitHub setup

Once the owner has chosen an account, the source/context can be imported as a
fresh history after a final file review. Preserve upstream attribution and separate
licenses; do not label the entire combined tree as newly authored or assign an
incompatible blanket license. Account creation, repository visibility and publishing
are separate owner actions; none has occurred here.

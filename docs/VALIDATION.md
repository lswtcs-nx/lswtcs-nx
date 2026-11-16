# Local validation

The verification and regression commands below do not install tools, download
dependencies, launch a game/emulator, change SD files or communicate externally.
Historical source/build helpers elsewhere in the snapshot can perform other
actions when explicitly invoked; read their instructions before using them.

## Verify received files

From the handoff root:

```sh
python tools/verify_handoff.py
```

It compares the supplied files to `manifest.json`. A later intentional edit will
change that result; use your contribution diff to describe it. The optional
`--share-scan` also checks the whole tree for unexpected files, binaries/game
payloads, Git metadata and recognizable private-home/credential patterns.

## Run the asset-free tile-identity regression

Use an **already installed** C++20 compiler:

```sh
python tests/run_draw_identity.py --compiler clang++
```

Pass a full compiler path if it is not on PATH; `g++` can be selected if available.
The code uses actual before/after draw-key/planner functions and the current bridge
normalization helper. Small licensed Xenia/xxHash fixtures avoid an SDK/dependency
download. Build/run files are temporary and removed afterward.

Expected result: `passed: true`,28 changed-input cases (26 previously accepted,
two already rejected),4 permitted tile cases,7 unsupported inputs and300 preserved
legitimate plans. The observed host synthetic timing is **not Switch performance**.

The fixture mocks env/MSAA helpers and the uint32 Endian enum around the extracted
SystemConstants declaration. It does not execute shader code, a driver or GPU,
attribute the screenshots or certify full resource equivalence.

## Check the game image built from `default.xex`

Needs your own `default.xex`, the reference `image.bin`, `pristine.bin` and `xex_header.bin`, and a host clang++
(tiny-AES-c comes from the pinned XenonRecomp checkout):

```sh
python tests/xex_image_check.py --xex <default.xex> --data-dir <folder with the reference .bin files>
```

It builds the runtime's `lsw_xex_image.cpp` for the PC and checks that its image gives the guest the same memory at boot
as `image.bin` with `LSWTCS_PRISTINE=1`. Expected: `PASS` and image SHA-256 `eaa95225…`, the value the Switch logs in
`[XEX]` (run 113).

## Patch-chain validation already performed

- The cumulative XenonRecomp and Xenia diffs were applied to exact local pinned
  preimages and compared to current normalized source. SPIRV-Cross has no semantic
  patch at its pin.
- The cumulative generated-output patch recreated315 changed files from the
  preserved folded baseline to current output. Baseline/target manifests contain
  hashes, not the generated game code itself.
- These checks do not establish a new generator invocation or fresh full build.
  The recipient's own baseline check is a mandatory gate for that route.

The baseline's original Mesa012 ARM/static/link checks and source identity are
summarized under `../evidence/`; no matching game executable is bundled.

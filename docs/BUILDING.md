# Build and reproduction guide

## Choose the smallest useful setup

**Review/local regression:** an already installed Python3 and C++20 compiler are
enough. No game, Switch SDK, emulator, keys or firmware are needed. Follow
[VALIDATION.md](VALIDATION.md) first.

**Full game build:** requires your own matching game, locally generated GameCode,
the pinned dependencies and an appropriate toolchain. This export does not install
or fetch them. Any setup on the owner's machine needs their explicit approval
after purpose, size and location; none is requested or performed by this handoff.

The owner's incremental Mesa012 build passed. A fresh generator invocation and
complete build of this exported folder have **not** been run. The patch-chain
qualification below is narrower and explicit.

## Intended layout

```text
handoff/
  source/                     maintained port sources in this folder
    xenia-canary/             obtain separately at the recorded pin
    SPIRV-Cross/              obtain separately at the recorded pin
    simde/simde/              headers from the pinned SIMDe checkout
    Convert 360/LSWTCS/
      default.xex             your input, never shared
      output_locals_raw/      your generated then folded baseline
      output/                 patched generated output used by CMake
  XenonRecomp/                obtain separately; sibling of source/
```

Pins and cumulative patches are in `../provenance/dependencies.json`.
Nested submodule pins/URLs are recorded separately. Only fetch/build what your
selected route needs; a full Xenia emulator build is not required to compile its
source libraries for this runtime.

## Dependency preparation

Use an existing Git installation and your own dependency setup. Example checkouts,
from the handoff root, after you choose to download these source dependencies:

```sh
git clone https://github.com/hedge-dev/XenonRecomp.git XenonRecomp
git -C XenonRecomp checkout ddd128bcca99fe8bfbb99bea583c972351fa6ace
git clone https://github.com/xenia-canary/xenia-canary.git source/xenia-canary
git -C source/xenia-canary checkout 09dbe2cd3605dbbb51a4ab38b5c40df4f7d968a8
git clone https://github.com/KhronosGroup/SPIRV-Cross.git source/SPIRV-Cross
git -C source/SPIRV-Cross checkout f11ba9f0b21ba8fc15153d50a2a1ae31ab1cf8f7
```

Initialize the generator's recorded submodules. For the Mesa libraries, relevant
Xenia submodules include FFmpeg, fmt, glslang, xxhash, utfcpp, cxxopts and
DirectX-Headers; the manifests retain exact gitlinks, not "latest" guesses.
SIMDe is pinned at `a532a12ca9bbdc5e6547eb602e6256b71a5377d4` through the generator.
The Switch include search expects its headers under `source/simde/simde/`.

Apply the current cumulative **Xenia** patch once, at its repository root:

```sh
git -C source/xenia-canary apply --check ../patches/xenia-canary.patch
git -C source/xenia-canary apply ../patches/xenia-canary.patch
```

SPIRV-Cross, nested FFmpeg and SIMDe had no semantic local patch in this snapshot.
Line-ending-only dirty status is not evidence of a required code patch.

### Two generator states — do not mix them

`source/patches/XenonRecomp.patch` records the **current generator source edits**,
including later CR/VMX additions. It is supplied for review/provenance. It was
round-trip checked against the pinned generator source, but fresh output from
that generator state has not been qualified against this game patch.

For the **historical reference-generation route**, use the smaller
`provenance/generator-reference.patch` against the clean pinned generator instead.
That is the patch recorded for the original folded baseline. Do not apply both.
The baseline hash check below decides whether your generation actually matches;
if it does not, stop and report the difference instead of forcing the game patch.

The owner built the reference generator with LLVM20.1.8/clang-cl and VS2022 tools.
MSVC alone and LLVM18 with that installed STL were not suitable. The recorded
CMake settings were Release, clang-cl, C flags `/clang:-msse2`, and C++ flags
`/clang:-mmmx /clang:-msse2`. Build with one job initially, at most two here.
These are observed historical versions, not a recommendation to update globally.

## Own inputs and generated source

1. Check your executable against `../provenance/expected-inputs.json`. It identifies
   Xbox360 USA/Europe Rev1, title4C4107D7, entry0x8229E618. No XEX is supplied.
2. Put your own `default.xex` under `source/Convert 360/LSWTCS/`. Use
   `lswtcs_locals.toml`, not the simpler non-local default config. It enables the
   register-local options that the reference output/patch expects.
3. Create `output_locals_raw`, then run your built XenonRecomp with the absolute
   path to `lswtcs_locals.toml` and to `XenonUtils/ppc_context.h` as its two inputs.
4. Run `python source/switch/tools/fold_recomp_output.py <output_locals_raw>`.
   The historically named "raw" folder is now the **folded baseline**.
5. Check it, then copy it into a new `output/` folder. The existing owner checkout
   is not a place to experiment with full regeneration or overwrite build objects.

```sh
python tools/generated_output.py check baseline "source/Convert 360/LSWTCS/output_locals_raw"
```

The included current game edit patch is **cumulative**, replacing the older
upstream `recomp_output_edits.patch` and subsequent individual generated-output
fixes. Apply it once to the copied folded baseline:

```sh
python tools/generated_output.py apply "source/Convert 360/LSWTCS/output"
python tools/generated_output.py check target "source/Convert 360/LSWTCS/output"
```

The helper verifies all expected baseline files before writing, normalizes source
text to UTF-8/LF, uses isolated Git patch application, and checks every target
file afterward. It refuses a drifted baseline. The patch was verified against
the existing preserved baseline for315 changed files; it is not a fresh-codegen
test. Do not also run the older sentinel/movie generated-source patches: their
current results are already represented by this cumulative delta.

Since log84, also fill the VMX operations the generator left empty in the movie
libraries. After this, `generated_output.py check` reports 15 files as changed;
`--revert` removes the fill.

```sh
python workspace-tools/fill_tcs360_movie_ops.py "source/Convert 360/LSWTCS/output"
```

Since log91, also make the float compares quiet in the generated `ppc_context.h`
(one `fcmp` instead of up to four compares and three branches; the same CR bits,
checked by `tests/cr_compare_check.py --compiler <clang++>`). It changes the
precompiled header, so every game unit recompiles (about 25 minutes with 2 jobs).
`--check` reports the state; `--revert` restores the generator's form.

```sh
python workspace-tools/tune_tcs360_context.py
```

Since run 93, guest addresses are also computed without the region table (152 ns
per guest call against 158). It changes the header the same way, so run it in the
same step:

```sh
python workspace-tools/tune_tcs360_context.py --fold-fast
```

Such a build refuses to start with `LSWTCS_HEAPBYPASS` on: the fold is exact only
while guest region 0xE0000000 is unmapped. The bypass has been off in the
hardware profile since before this change. The log91 B copy (`output_fold`,
`build-fold`) is no longer needed.

Then, from `source/`, run `python tools/gen_native_reverb.py` to generate the
excluded game-derived reverb include from your local patched output. Generated
source, PCH, objects and binaries remain private.

## Game data needed at runtime

For ordinary full-content play the app needs your own `game.DAT`, `game1.DAT`,
`Movies/`, `audio/`, `image.bin`, `pristine.bin` and `xex_header.bin` under
`/lswtcs`. Preserve the same native application/save identity and existing saves.

- `workspace-tools/prepare_tcs360_header.py --xex ... --output ...` extracts the
  private header and refuses overwrite.
- `workspace-tools/xenon-image/` builds an exporter using the pinned XenonUtils
  source. Its output is the clean loader `pristine.bin`.
- **`image.bin` is different:** the runtime expects a runtime-ready reference
  snapshot with import fixups. The existing `capture_tcs360_reference.ps1` reads
  the specifically identified, user-launched Xenia reference process and checks
  its code hash. It does not launch/install Xenia or write into the process.
  Do not rename a clean decode/export to pretend this requirement is met.

This reference capture/import preparation is another recipient setup gate, not
an asset bundled in this handoff. Do not share your outputs or keys back.

## Switch Mesa build

The owner used devkitPro's MSYS2 tools, devkitA64/GCC, libnx and the project's
patched Mesa20.1.0-rc3. At the recorded setup, SDK package versions included
devkitA64r30-1, libnx4.12.0-1, switch-libdrm_nouveau1.0.1-2 and
switch-glad0.1.27-1. The project's custom Mesa exports are required; stock Mesa
alone previously failed the strict link. Do not add dummy exports.

The safe existing recipe is `workspace-tools/build_tcs360_mesa.sh`, with an
explicit port path and **1** job. It obtains the pinned Mesa source/package
recipe and builds into `source/switch/mesa/build/`. This downloads/builds software;
review it and your setup first. It is not executed by the handoff tools.

Mesa source SHA256: `c90b75ea34302ebde9b81b87c5642fa864c40fe9c4ad34ce0793170c1413168d`.
devkitPro recipe pin: `f103fe88e37180ecd0b7a9173b52b7580a54f71a`.
All eight existing Mesa edit scripts are in `source/switch/mesa/`.

From the devkitPro MSYS shell, under `source/switch/`:

```sh
"$DEVKITPRO/portlibs/switch/bin/aarch64-none-elf-cmake" -G "Unix Makefiles" \
  -S . -B build-mesa -DCMAKE_BUILD_TYPE=Release \
  -DLSW_DEKO_BACKEND=OFF -DLSW_GAME_UNITY_BATCH_SIZE=5 \
  -DLSW_GAME_PCH=ON -DLSW_RPMALLOC=ON
cmake --build build-mesa --target lswtcs_nro --parallel 1
```

Preserve completed GameCode objects/PCH; do not clean/reconfigure another backend
tree casually. Original full rebuilds were expensive. The owner's incremental
012 build compiled only two changed units; a new machine must build its own code.

`workspace-tools/validate_tcs360_switch.py --help` describes the static artifact
check. Link/static success is not hardware acceptance. Use
`profiles/hardware-B.env` as the documented reference profile, without emulator
autostart/keys/settings. `GAMEROOT=X:/none/` disables fallback lookup only; movies
in the data working directory still resolve. No movie skip is intended.

## Deko and desktop

The source contains `LSW_DEKO_BACKEND=ON`, the shared model and host shader tools,
but it is an unfinished track. Do not flip the existing Mesa build directory to
Deko or describe a frozen replay as a playable game. Agree on a bounded review
before provisioning a separate full Deko setup. Desktop reference CMake exists,
but its complete fresh setup is not validated by this export either.

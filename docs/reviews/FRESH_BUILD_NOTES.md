# Fresh build notes, 1 November 2026

First build from this exported folder, on a new Windows 11 machine. Results below are
compile, static and hash checks only: the game has **not** been run on this build, in
an emulator or on hardware.

## Results

| Gate | Result |
| --- | --- |
| Input `default.xex` | Size and SHA256 match `expected-inputs.json` |
| Fresh generation, reference generator state | 3,166 of 3,166 baseline hashes match |
| Cumulative game patch | 3,169 of 3,169 target hashes match |
| `xex_header.bin` | Size and SHA256 match |
| `pristine.bin` | Size and SHA256 match |
| Patched Mesa | Built with 2 jobs |
| Mesa012 game target | Built in about 47 minutes with 2 jobs; no compile errors |
| `validate_tcs360_switch.py` | Passed; NACP 0.0.12, strict link true |
| `image.bin` | Captured from Xenia Canary `02d2cb5`; code hash matches; hardware use untested |

The NRO has the owner's exact size, 115,026,117 bytes, but a different hash:

- This NRO SHA256: `79c58c3fe06d2d5e7034bea073f15ab615d0b1f72d95a61855a7b3d05e1412ce`
- This ELF SHA256: `0cd8229009204c51a54607454e1d8b719d04ebd60b56061732a3e79e8fd49a30`

The cause of the hash difference has not been investigated. Different build paths are
one possibility, not a finding.

## Versions used

LLVM 20.1.8, VS 2022 Build Tools 17.14 (MSVC 14.44), devkitA64 r30-1 (GCC 16.1.0),
libnx 4.12.0-1, switch-libdrm_nouveau 1.0.1-2, switch-glad 0.1.27-1, Meson 1.12.1,
MSYS2 Python 3.12.16, CMake 4.4.4 (Windows) and 4.4.5 (MSYS).

## Gaps found in BUILDING.md

- **Xenia submodule:** `third_party/tomlplusplus` is required (`xenia/base/cvar.h`) but
  is not in the listed submodules.
- **Mesa dependencies:** `switch-zlib` and `switch-libexpat` must be installed, or Meson
  stops at the zlib dependency.
- **MSYS packages:** the Mesa recipe needs `python`, `python-mako`, `meson`, `ninja`,
  `flex`, `bison` and `dkp-meson-scripts`, and the game configure needs `cmake`.
- **SIMDe layout:** the pinned `simde-no-tests` repository root must itself become
  `source/simde/simde/`. It has no inner `simde/` folder.
- **Generator configure:** with CMake 4 it needed `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`.
- **`.gitignore`:** `XenonRecomp/` also ignores `notices/XenonRecomp/LICENSE.md`. It
  should be anchored as `/XenonRecomp/`. The license is force-added here.
- **devkitPro setup:** the updater's silent mode exits with 0 and installs nothing. A
  separate MSYS2 with the documented dkp repositories worked.

## Open question for `image.bin`

The capture used the latest official Xenia Canary release, `02d2cb5` (March 2026), which
differs from the source pin. The owner's Xenia build is not recorded. Its image sits at
host address `0x282000000`, not the old fixed offset, so the capture script now searches
for the image and accepts it only if the code hash matches. Import thunks come from this
Xenia build and are not validated by that hash, so they are a suspect if early boot fails.

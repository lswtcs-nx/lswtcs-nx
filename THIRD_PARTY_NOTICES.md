# Third-party notices

The port's own code is under the licence in [`LICENSE`](LICENSE) (see the README, "Credits and licence"). The components
below keep their own licences. Their notices are in [`notices/`](notices/) or next to their files, and the exact
versions are pinned in [`provenance/dependencies.json`](provenance/dependencies.json). The repository has no game data
and no generated game code beyond the context lines of the hand-edit patch in `source/patches/`.

| Component | Used for | Licence | Notice |
| --- | --- | --- | --- |
| [LSWTCS-360-to-Switch-Port](https://github.com/KingDariusStim/LSWTCS-360-to-Switch-Port) (KingDariusStim) | The original port this repository continues | CC BY-NC-SA 4.0 | [`LICENSE`](LICENSE), [`provenance/UPSTREAM_README.md`](provenance/UPSTREAM_README.md) |
| [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) (hedge-dev) | Recompiles the game's PowerPC code to C++; cumulative changes in `source/patches/` | MIT | [`notices/XenonRecomp/`](notices/XenonRecomp/LICENSE.md) |
| [Xenia Canary](https://github.com/xenia-canary/xenia-canary) | Xenos GPU command and shader translation, XMA decoding, kernel pieces | BSD 3-Clause | [`notices/xenia-canary/`](notices/xenia-canary/LICENSE) |
| FFmpeg, the subset Xenia carries | XMA audio decoding | LGPL 2.1 or later (GPL texts included as FFmpeg ships them) | [`notices/FFmpeg/`](notices/FFmpeg/) |
| [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross) | Translated shaders to GLSL | Apache 2.0 | [`notices/SPIRV-Cross/`](notices/SPIRV-Cross/LICENSE) |
| [SIMDe](https://github.com/simd-everywhere/simde) | Xbox 360 vector instructions on ARM64 | MIT | [`notices/SIMDe/`](notices/SIMDe/COPYING) |
| [xxHash](https://github.com/Cyan4973/xxHash) | Hashing | BSD 2-Clause | [`notices/xxHash/`](notices/xxHash/LICENSE) |
| [rpmalloc](https://github.com/mjansson/rpmalloc) | The process's memory allocator | Public domain / MIT | [`source/switch/third_party/rpmalloc/LICENSE`](source/switch/third_party/rpmalloc/LICENSE) |
| [Mesa](https://mesa3d.org/) 20.1 with devkitPro's switch-mesa patch | OpenGL on the Switch (Nouveau). Built from source by `source/switch/tools/build_mesa.sh`, with the port's edits in `source/switch/mesa/*_edits.py` | MIT | in the Mesa source the script downloads |
| [libdrm_nouveau](https://github.com/devkitPro/libdrm_nouveau) (devkitPro) | GPU buffer and command submission; three files kept for reference in `reference-source/` | MIT | the header of each file |
| [libnx](https://github.com/switchbrew/libnx) and the devkitPro toolchain | Switch system interfaces and the compiler | ISC (libnx); the toolchain's own | not included, installed with devkitPro |

**Techniques only.** bo2-360-nx, rr6-nx and mk9-nx are GPL-3.0. Ideas from them (the fourth-core route, PGO,
profiling approaches) were reimplemented here; no code was copied from them.

**Game content.** LEGO Star Wars: The Complete Saga belongs to its rights holders. You need your own copy of the
Xbox 360 game to build and run this port. Files made from it (generated code, data, saved shader lists, PGO counts)
stay on your machine and SD card. The README's logo and the app's icon (`source/switch/icon.jpg`, cut from the
cover by `tools/make_icon.py`) are the game's artwork, shown to identify it.

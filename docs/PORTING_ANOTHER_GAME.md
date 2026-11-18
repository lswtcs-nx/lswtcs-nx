# Porting another Xbox 360 game with this code

What this port's code would give another game, such as another TT Games LEGO title, and what has to be found
again. Written 17 November 2026, from TCS only: nothing here has been tried on another game. Effort for a new game is
unknown until its executable has been analysed.

## What carries over

- **The pipeline:** XenonRecomp (PowerPC to C++), the build (`source/switch/CMakeLists.txt`, PGO, the release tools),
  and the generated-code tools (`workspace-tools/`, `source/switch/tools/make_nonvolatile_output.py`).
- **The image from the disc:** `source/LSWTCSRuntime/source/lsw_xex_image.cpp` builds the guest image from
  `default.xex` (decrypt, decompress, import thunks). `python tests/xex_image_check.py --xex <default.xex> --out <image.bin>`
  makes it on a PC for any game.
  - One TCS-only step: it zeroes the `.XBMOVIE` word at 0x83284B04 when that address is inside the image. Check that
    word in another game.
- **The kernel:** `kernel_stubs.cpp`'s Xbox kernel and XAM exports (threads, events, files, saves, memory, the PM4 GPU
  ring walker and its interrupts) and the 13 kernel variables bound from the XEX header (`lsw_kernel_variables.h`).
- **The GPU path:** the PM4 recorder, Xenia's shader translator, the GL replay on Mesa, and the shader and translation
  caches.
- **The Switch side:** audio and XMA, controllers and co-op, saves, the fourth core, the logs, and the automated runs
  (`docs/AUTOMATED_TESTS.md`).

## What is TCS's own

`python tools/port_inventory.py` lists every guest address the runtime names, by region of TCS's image. On 17
November 2026 there were 349:

| Region | Addresses | How to find it in another game |
| --- | --- | --- |
| D3D runtime (ring buffer, fences, ISR, tiling) | 26 | SDK code: by signature |
| XAudio | 15 | SDK code: by signature |
| XMedia (movies) | 5 | SDK code: by signature |
| Network, voice, XAM wrappers | 22 | SDK code: by signature |
| HLSL compiler (D3DX) | 12 | SDK code, if the game compiles shaders at run time |
| xboxkrnl/Xapi glue | 5 | SDK code: by signature |
| Engine I/O and the C runtime | 40 | engine or CRT code: by signature |
| Engine heap and sync | 5 | engine code: by signature |
| TT engine and game code | 117 | by signature if the engine is close; otherwise by behaviour |
| Other code | 16 | case by case |
| Game data and .bss | 82 | by what reads them |
| Headers and read-only data | 4 | from the XEX |

The region edges come from the code's comments and the runtime's own range tests, so they are approximate.
`--detail '<region>'` lists one region's addresses with the lines that use them.

Most of the game-data patches in `main.cpp` repaired a memory dump taken from Xenia (stale locks, lists, flags). Since
log107 the image comes from `default.xex`, so some of them may no longer be needed even for TCS. That has not been
checked one by one.

## Finding functions by their code

`tools/find_functions.py` takes TCS's image and the recompiler's `ppc_func_mapping.cpp`, makes a masked signature of
each function the runtime names (branch targets and 16-bit immediates hidden, everything else exact), and searches
another image for it. Each function comes out unique, ambiguous or missing.
- **Self-check on TCS:** 212 of the 246 named function starts are found only where they are. 34 are ambiguous, all
  tiny chunks.
- **What to expect on another game:** the same SDK version should match nearly word for word. Another SDK version, or
  engine code the developer changed, may not match, and then it has to be found by behaviour.

```
python tests/xex_image_check.py --xex <other default.xex> --out other_image.bin
python tools/find_functions.py --a private/sdcard/lswtcs/image.bin \
    --mapping "source/Convert 360/LSWTCS/output/ppc_func_mapping.cpp" --b other_image.bin --csv found.csv
```

## Steps for a new game

1. **Image and analysis.** Build the image from its `default.xex`. Run XenonAnalyse for its switch tables, and find its
   save/restore register helpers (`savegprlr_14` and the rest in `lswtcs_locals.toml`). The CRT signatures can find
   these.
2. **Recompile.** A new XenonRecomp config (`Convert 360/LSWTCS/lswtcs_locals.toml` is the model), then the generated-code
   passes:
   - `make_nonvolatile_output.py` and `tune_tcs360_context.py`;
   - a check for VMX operations XenonRecomp leaves empty (`workspace-tools/fill_tcs360_movie_ops.py` finds them as an
     `// op` comment followed directly by another comment or label). In TCS about 8,600 were empty.
3. **Map the runtime's addresses.** `find_functions.py` for the SDK and engine functions. Then go through
   `port_inventory.py --detail` region by region: each override, hook and patch needs the other game's address, or a
   reason it is not needed there.
4. **Boot.** In the emulator first (`docs/AUTOMATED_TESTS.md`): many boots a day, without the console. The milestones
   are the same as here:
   - H0: reaches `_xstart` and its first guest frame;
   - H1: title and menus;
   - H2: bounded gameplay.
5. **Then the console**, for memory, speed and the driver.

## Known risks for a later TT game

- **Memory.** TCS uses about 1.9 GB at its base, of a 4 GB Switch. A game with larger levels and more streaming may not
  fit without work on the GL copy of guest memory, the biggest single item.
- **Speed.** TCS's heavy scenes run near 30 FPS in handheld. A heavier game will run slower, and how much slower is not
  known before it runs.
- **Engine changes.** Where the engine's own code changed, signatures will miss. Each fix this port needed (loaders,
  job system, host link, streaming waits) would have to be found again, by the same kind of log-driven work as runs 1-117.

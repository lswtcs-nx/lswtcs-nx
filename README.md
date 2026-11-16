<p align="center">
  <img src="assets/game-logo.png" width="460" alt="LEGO Star Wars: The Complete Saga">
</p>

<h1 align="center">LEGO Star Wars: The Complete Saga for Nintendo Switch</h1>

An unofficial native port of the Xbox 360 version of *LEGO Star Wars: The Complete Saga* to the Nintendo Switch, as
homebrew. It is not an emulator. The game's PowerPC code is recompiled ahead of time to ARM64 with XenonRecomp, and a
runtime written for the Switch provides the Xbox 360 services:
- memory and threads;
- files and saves;
- input and audio;
- graphics, through OpenGL on Mesa/Nouveau.

## Requirements

- **A Switch that runs homebrew** (custom firmware such as Atmosphère), with
  [sphaira](https://github.com/ITotalJustice/sphaira) to make a HOME-menu forwarder.
- **About 6 GB free on the SD card:** the game's data (about 5.8 GB) plus the app.
- **Your own copy of the Xbox 360 game:** the USA/Europe Rev 1 multilingual release (1.1). Its checksums are in
  [`provenance/expected-inputs.json`](provenance/expected-inputs.json).
- **A PC to build the app** from your copy (see [Building](#building)).

## Building

The build makes Switch code out of your copy of the game. Tested on Windows with devkitPro (devkitA64, libnx), CMake
and Python 3. In outline:

1. **Get the dependencies** at the pinned versions ([`provenance/dependencies.json`](provenance/dependencies.json)):
   XenonRecomp, Xenia Canary, SPIRV-Cross, SIMDe.
2. **Generate the game code** from your `default.xex` with XenonRecomp, and apply the port's patch to the output.
3. **Build Mesa for the Switch** with the port's changes: `source/switch/tools/build_mesa.sh`.
4. **Build the NRO** with CMake from `source/switch`. The current recipe (guest memory without `volatile`, plus an
   optional profile-guided build) is in "The current build" in [docs/BUILDING.md](docs/BUILDING.md).

The game's own files need no preparation: the app reads them from your disc's files on the SD card.

## How to install

There is no ready-made download yet. With `lswtcs.nro` from [Building](#building):

1. **Copy the app** to `sdmc:/switch/lswtcs/lswtcs.nro`.
2. **Copy from your own disc** to `sdmc:/lswtcs/`: `default.xex`, `game.DAT`, `game1.DAT`, `Movies/` and `audio/`.
   The game checks them at start and names any file that is missing or from another version.
3. **Make a forwarder** in sphaira, so the game gets all four CPU cores (from the Homebrew Menu it gets three):
   highlight *LEGO Star Wars: The Complete Saga*, press **X**, choose **Install Forwarder**, set **CPU Cores** to
   **4** and **Address Space** to **39-bit**, then choose **Create Forwarder**.
4. **Start the game** from its new icon on the HOME menu.

The settings for play are built in, so no settings file is needed. Testers add one to get logs
([docs/playing.md](docs/playing.md), "Testing").

The finished SD card:

```text
sdmc:/
  switch/lswtcs/lswtcs.nro
  lswtcs/
    default.xex  game.DAT  game1.DAT  Movies/  audio/
```

**Updating:** replace `sdmc:/switch/lswtcs/lswtcs.nro`. The forwarder starts the new build without being made again,
and the saves are kept.

**Uninstalling:** deleting the forwarder removes only the shortcut. To remove the game, delete
`sdmc:/switch/lswtcs/` and `sdmc:/lswtcs/`. Saves stay in the console's save storage.

## Controls

The game shows its Xbox 360 button icons, but confirm and back follow the Switch: A confirms, B goes back. In play the
face buttons keep the 360 positions, so the bottom button jumps. Plus is Start, Minus is Back, and L/R and ZL/ZR are
the bumpers and triggers.
- **L + R + Minus** switches the face-button layout.
- **Plus** on a second controller joins player 2; **L + R + Plus** opens the controller screen.

[docs/playing.md](docs/playing.md) has every control, the testing profile, forwarders, saves, and how to send logs.

## Known issues

- **The movie that plays when the title screen is left idle** can show corruption and hang at its end. Avoid leaving
  the title screen idle.

The full list, with the technical detail for contributors, is in [docs/known-issues.md](docs/known-issues.md).

## Performance

What the port does to run well on the Switch. Every change was measured on a real Switch; its session and evidence are
in [docs/progress-log.md](docs/progress-log.md).

**Game code (CPU)**
- Guest memory accessed without `volatile`, with `-fno-strict-aliasing` and compiler barriers only at guest sync
  instructions and loop headers.
- Profile-guided optimisation (GCC), with counts from a play session on an instrumented build.
- Guest addresses translated by one bit fold instead of a region-table load per access.
- Float compares without flag side effects, and argument registers kept in locals inside hot loops.
- Together these cut the main thread's CPU per recompiled call from 157 to 127 ns.

**Threads**
- The Xbox 360 GPU command stream (PM4) is walked on its own thread, off the guest's turn. GPU kicks hand the ring to
  it, and ring-space waits end at the walker's published position.
- XMA audio decoding and LZ2K decompression run on worker threads, beside the game, as the 360's hardware decoder does.
- The XAudio mixing work runs off the guest's turn, with a native mixer that matches the game's result bit for bit.
- Guest spinlocks are real compare-and-swap locks, and audio threads come first in the guest scheduler.
- With four cores, core 3 runs work no frame waits for: the log writer, XMA, file reads and a second shader compiler.
  A meter sends them back if core 3 gets busy.

**Rendering (OpenGL on Mesa/Nouveau)**
- The world is drawn once per frame, not once per EDRAM tile. Tile groups are merged, and resolves go straight into
  the stitched target.
- A draw's preparation is reused while no register changed, and the command walk skips guest pages unchanged for 8
  frames. Both keep a sampled check that turns them off at the first difference.
- Shader variants are specialised: alpha test, gamma, output bias, and fetch scale and LOD bias become constants.
  Fetch constants come from one block per draw.
- Mesa: unchanged constant-buffer binds are skipped, quads are drawn as indexed triangles instead of through a
  geometry shader, and 2x MSAA surfaces render at one sample per pixel.
- The handheld GPU runs at 460.8 MHz, Nintendo's official handheld configuration, not an overclock
  (`LSWTCS_GPUBOOST=0` keeps 307.2 MHz).

**Shaders and textures (stutter)**
- Every shader program the game used is saved on the SD card. Later boots prepare the list in the background, the
  current level's programs first.
- Mesa reuses the nvc0 translation of an identical shader stage, which cut the cost per program by about 45 %; a
  sample is re-translated and compared.
- Two shader compilers; with four cores, the second takes only programs nothing is waiting for.
- New textures are made before the draw loop, decoded on three threads and uploaded as each one is ready. Texture
  storage is pooled, and uploads are staged from Mesa's GART suballocator.
- Large allocations come from newlib's allocator and small ones from rpmalloc, so freed large blocks merge.

**Next:** we are studying a move from OpenGL on Mesa to deko3d, the Switch's native graphics library, for a broader
gain.

## How to collaborate

- **Test and bring logs:** play with the testing profile and send the session's folder from `sdmc:/lswtcs/logs/`
  ([docs/playing.md](docs/playing.md), "Testing").
- **Change the code:** read [CONTRIBUTING.md](CONTRIBUTING.md). Then the end of [docs/progress-log.md](docs/progress-log.md)
  has the newest state, and [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) the code map.
- **Coding assistants** follow [AGENTS.md](AGENTS.md).

## Credits

This port is currently developed by Tnes and Gabriel B ([g-brrzzn](https://github.com/g-brrzzn)). The components it
is built on, and their licences, are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

**Disclaimer.** This is a fan project, not affiliated with or endorsed by the LEGO Group, Lucasfilm, TT Games,
Microsoft or Nintendo. *LEGO Star Wars: The Complete Saga*, its logo and its content belong to their owners. The
repository contains none of the game's data. Use your own legally obtained copy. Made by fans, for fans. Thanks for
playing.

<br>

<p align="center"><em>And may the Force be with you.</em></p>

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

## How to install

There is no ready-made download: the app is built from your own copy of the game. Once you have `lswtcs.nro`:

1. **Copy the app** to `sdmc:/switch/lswtcs/lswtcs.nro`.
2. **Copy your game's files** to `sdmc:/lswtcs/`:
   - `game.DAT`, `game1.DAT`, `Movies/` and `audio/` from your copy;
   - `image.bin`, `pristine.bin` and `xex_header.bin`, prepared during the build.
3. **Copy the settings profile** [`profiles/player.env`](profiles/player.env) to `sdmc:/lswtcs/env.txt`.
4. **Make a forwarder** in sphaira, so the game gets all four CPU cores (from the Homebrew Menu it gets three):
   highlight *LEGO Star Wars: The Complete Saga*, press **X**, choose **Install Forwarder**, set **CPU Cores** to
   **4** and **Address Space** to **39-bit**, then choose **Create Forwarder**.
5. **Start the game** from its new icon on the HOME menu.

The finished SD card:

```text
sdmc:/
  switch/lswtcs/lswtcs.nro
  lswtcs/
    env.txt
    game.DAT  game1.DAT  Movies/  audio/
    image.bin  pristine.bin  xex_header.bin
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

Measured on a real Switch, on the Gungan route (hub, Episode I chapter 2):

| | Handheld | Docked |
| --- | --- | --- |
| Average FPS, whole session | about 42 | about 49 |
| Heavy combat frames (1300+ draws) | about 30 FPS | about 31 FPS |
| What limits heavy frames | the GPU | the chain from game frame to GPU |
| Render stalls of 250 ms or more, per 3-minute session | 0-0.65 s (was 2.9 s) | not measured yet |

- **GPU clock.** The handheld GPU runs at 460.8 MHz, Nintendo's own handheld configuration `0x92220008`, not an
  overclock. `LSWTCS_GPUBOOST=0` keeps 307.2 MHz.
- **What made the game code faster:**
  - guest memory without `volatile`;
  - profile-guided optimisation.

  Together they cut the main thread's CPU per recompiled call from 157 to about 127 ns.
- **Shader preparation:**
  - Mesa now reuses the translation of a shader stage it has already compiled. This cut the cost per program by about
    45 %; checked word for word on hardware.
  - With four cores, a second compiler on the fourth core handles the saved list.
- **Next:** we are studying a move from OpenGL on Mesa to deko3d, the Switch's native graphics library, for a broader
  gain.

Every number has its session and evidence in [docs/progress-log.md](docs/progress-log.md). What could come next is in
the last section of [docs/reviews/CPU_ROADMAP.md](docs/reviews/CPU_ROADMAP.md).

## Building

The build makes Switch code out of your copy of the game. Tested on Windows with devkitPro (devkitA64, libnx), CMake
and Python 3. In outline:

1. **Get the dependencies** at the pinned versions ([`provenance/dependencies.json`](provenance/dependencies.json)):
   XenonRecomp, Xenia Canary, SPIRV-Cross, SIMDe.
2. **Generate the game code** from your `default.xex` with XenonRecomp, and apply the port's patch to the output.
3. **Build Mesa for the Switch** with the port's changes: `source/switch/tools/build_mesa.sh`.
4. **Build the NRO** with CMake from `source/switch`. The current recipe (guest memory without `volatile`, plus an
   optional profile-guided build) is in "The current build" in [docs/BUILDING.md](docs/BUILDING.md).
5. **Prepare `image.bin`, `pristine.bin` and `xex_header.bin`** from your copy ([docs/BUILDING.md](docs/BUILDING.md),
   "Game data needed at runtime").

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
repository contains none of the game's data. Use your own legally obtained copy.

<p align="center">
  Made by fans, for fans. Thanks for playing.<br>
  <em>And may the Force be with you.</em>
</p>

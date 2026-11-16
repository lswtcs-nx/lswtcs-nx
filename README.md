# LEGO Star Wars: The Complete Saga for Nintendo Switch

An unofficial native port of the Xbox 360 version of *LEGO Star Wars: The Complete Saga* to the Nintendo Switch, as
homebrew. It is not an emulator. The game's PowerPC code is recompiled ahead of time to ARM64 with XenonRecomp, and a
runtime written for the Switch provides the Xbox 360 services:
- memory and threads;
- files and saves;
- input and audio;
- graphics, through OpenGL on Mesa/Nouveau.

## Requirements

- **A Switch that runs homebrew** (custom firmware such as Atmosphère). [sphaira](https://github.com/ITotalJustice/sphaira)
  is recommended to make a HOME-menu forwarder that gives the game all four CPU cores.
- **About 6 GB free on the SD card:** the game's data (about 5.8 GB) plus the app.
- **Your own copy of the Xbox 360 game:** the USA/Europe Rev 1 multilingual release (1.1). Its checksums are in
  [`provenance/expected-inputs.json`](provenance/expected-inputs.json).
- **A PC to build on.** Tested on Windows with devkitPro (devkitA64, libnx), CMake and Python 3. The full list is in
  [docs/BUILDING.md](docs/BUILDING.md).

## Building and installing

The build makes Switch code out of your copy of the game, so it cannot be shipped ready-made. In outline:

1. **Get the dependencies** at the pinned versions ([`provenance/dependencies.json`](provenance/dependencies.json)):
   XenonRecomp, Xenia Canary, SPIRV-Cross, SIMDe.
2. **Generate the game code** from your `default.xex` with XenonRecomp, and apply the port's patch to the output.
3. **Build Mesa for the Switch** with the port's changes: `source/switch/tools/build_mesa.sh`.
4. **Build the NRO** with CMake from `source/switch`. The current recipe (guest memory without `volatile`, plus an
   optional profile-guided build) is in "The current build" in [docs/BUILDING.md](docs/BUILDING.md).
5. **Prepare the runtime data** from your copy: `game.DAT`, `game1.DAT`, `Movies/`, `audio/`, `image.bin`,
   `pristine.bin` and `xex_header.bin` ([docs/BUILDING.md](docs/BUILDING.md), "Game data needed at runtime").
6. **Copy to the SD card:** the NRO to `sdmc:/switch/lswtcs/lswtcs.nro`, the data and a settings profile to
   `sdmc:/lswtcs/`.

The SD layout is in [docs/playing.md](docs/playing.md).

## Playing

**Settings profiles.** `sdmc:/lswtcs/env.txt` sets how the game runs. Use one of the two profiles:
- [`profiles/player.env`](profiles/player.env) **to play:** tuned settings, diagnostics off.
- [`profiles/hardware-ab.env`](profiles/hardware-ab.env) **to test and bring logs:** the same settings plus the
  diagnostics.

**Controls.** The game shows its Xbox 360 button icons, but confirm and back follow the Switch: A confirms, B goes
back. In play the face buttons keep the 360 positions, so the bottom button jumps. Plus is Start, Minus is Back, and
L/R and ZL/ZR are the bumpers and triggers.
- **L + R + Minus** switches the face-button layout.
- **Plus** on a second controller joins player 2; **L + R + Plus** opens the controller screen.

**Four cores, for the best performance.** Start the game from a sphaira forwarder:
1. In sphaira, highlight *LEGO Star Wars: The Complete Saga*, press **X** and choose **Install Forwarder**.
2. Set **CPU Cores** to **4** and **Address Space** to **39-bit**, then choose **Create Forwarder**.
3. Start the game from its new icon on the HOME menu.

To update, replace `sdmc:/switch/lswtcs/lswtcs.nro`; the forwarder keeps working. Deleting the forwarder removes only
the shortcut, and saves stay in the console's save storage.

[docs/playing.md](docs/playing.md) has the details: every control, the profiles, forwarders, saves, uninstalling, and
how to send logs.

## How to collaborate

- **Test and bring logs:** play with the testing profile and send the session's folder from `sdmc:/lswtcs/logs/`
  ([docs/playing.md](docs/playing.md), "Testing").
- **Change the code:** read [CONTRIBUTING.md](CONTRIBUTING.md). Then the end of [docs/progress-log.md](docs/progress-log.md)
  has the newest state, and [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) the code map.
- **Coding assistants** follow [AGENTS.md](AGENTS.md).

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

## Credits

This port is currently developed by Tnes and Gabriel B ([g-brrzzn](https://github.com/g-brrzzn)). The components it
is built on, and their licences, are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

**Disclaimer.** This is a fan project, not affiliated with or endorsed by the LEGO Group, Lucasfilm, TT Games,
Microsoft or Nintendo. *LEGO Star Wars: The Complete Saga* and its content belong to their owners. The repository
contains none of the game's data. Use your own legally obtained copy.

<p align="center"><em>Made by fans, for fans. Thanks for playing, and may the Force be with you.</em></p>

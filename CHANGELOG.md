# Changelog

What changes for players in each release. The engineering record behind every change is in
[docs/progress-log.md](docs/progress-log.md).

## Unreleased

**Installing gets simpler.**
- The game loads `default.xex` from your disc directly. The files made on a PC before (`image.bin`, `pristine.bin`,
  `xex_header.bin`) and Xenia are no longer needed. Cards that still have them keep working.
- The player settings are built in, so `env.txt` is only for testers.
- A missing file or an unsupported game version shows a message on screen instead of closing the game.

**Since the first public snapshot (1 November 2026).**
- Story levels with native saves, two-player co-op and full audio.
- Four CPU cores from a sphaira forwarder; the fourth runs background work.
- Faster game code (guest memory without `volatile`, profile-guided optimisation) and the handheld GPU at Nintendo's
  460.8 MHz profile.
- Far fewer shader stutters: the shader list is kept on the SD card and prepared in the background, and Mesa reuses
  translations of identical shader stages.
- The app is titled *LEGO Star Wars: The Complete Saga* and uses the game's cover as its icon.

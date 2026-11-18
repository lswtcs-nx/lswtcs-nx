# Changelog

What changes for players in each release. The engineering record behind every change is in
[docs/progress-log.md](docs/progress-log.md).

## v0.3.0 (18 November 2026)

**Performance**
- Far fewer stutters from the second session on. The GPU driver's own compiled shaders are now kept on the SD card too
  (`sdmc:/lswtcs/mesacache/lsw_nvc0.bin`), so each shader program builds in about 28 ms instead of 170-240 ms. In the
  test session the saved shader list was ready 36 s after boot, the waits for shaders at area entries fell from
  0.9-2.6 s in all to 0.1 s, and the longest frame from 1.9 s to 0.6 s.
- The file survives updates of the app (it only depends on the GPU driver), so a new version does not start it over.
  The first session after a clean install builds it, and the first few loaded each session are checked against a
  fresh compile.

## v0.2.0 (16 November 2026)

**Performance**
- Shader translations are saved on the SD card (`sdmc:/lswtcs/glprogs2/xlate.bin`). From the second session, the hub's long first frames (about 0.5-0.9 s) are gone; only areas never visited before still translate.

## v0.1.0 (16 November 2026)

The first release.

**What you can play**
- The story levels, with two-player co-op, native saves and full audio.

**Installing**
- The app reads the game from your own disc's files: `default.xex`, `game.DAT`, `game1.DAT`, `Movies/` and `audio/`.
  No PC tool or emulator is needed to prepare them.
- The game checks those files when it starts, and names any file that is missing or from another version of the game.
- The settings for play are built in; no settings file is needed.
- The app is titled *LEGO Star Wars: The Complete Saga* and uses the game's cover as its icon.

**Performance**
- Four CPU cores from a sphaira forwarder; the fourth runs background work.
- Faster game code (guest memory without `volatile`, profile-guided optimisation) and the handheld GPU at Nintendo's
  460.8 MHz handheld profile.
- Far fewer shader stutters: the shaders the game used are kept on the SD card and prepared in the background, and
  Mesa reuses the translations of identical shader stages.

**Known issue**
- The movie that plays when the title screen is left idle can show corruption and hang at its end. Avoid leaving the
  title screen idle.

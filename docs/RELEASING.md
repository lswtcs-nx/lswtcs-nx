# Making a release

A release is the NRO and nothing from the game: players copy their own disc's files. GitHub Actions cannot build it,
because the game code is generated from `default.xex`, so a maintainer builds it locally and uploads it. Releases are
published only while the repository is public.

## Version

`vMAJOR.MINOR.PATCH`, starting at `v0.1.0`. The NRO's NACP carries the same version (`LSW_RELEASE_VERSION`). Test builds
keep their `0.0.99-logN` names, so a log always shows which build made it.

## Checklist

1. **Changelog.** Move "Unreleased" in [CHANGELOG.md](../CHANGELOG.md) to `## vX.Y.Z (date)`, in players' words.
2. **Build** the play build (guest memory without `volatile`, PGO; "The current build" in [BUILDING.md](BUILDING.md))
   with the release version:
   ```sh
   cmake -S source/switch -B source/switch/build-nvpgo -DLSW_RELEASE_VERSION=X.Y.Z
   cmake --build source/switch/build-nvpgo --target lswtcs_nro --parallel 2
   ```
   Then run `workspace-tools/validate_tcs360_switch.py` on the build folder.
3. **PC checks.**
   - `python tests/xex_image_check.py --xex <your default.xex> --data-dir <folder with the reference .bin files>` passes.
   - The game-free regression in [VALIDATION.md](VALIDATION.md) passes.
4. **On a Switch, from a clean card:** extract the release zip, copy the disc files, no `env.txt`, a new forwarder
   (4 cores, 39-bit). Boot to the title, play one level, save and load, in handheld and docked. The log must show
   `[XEX] image from default.xex` with `matches the PC check`.
5. **Package:** `python tools/make_release.py --version X.Y.Z`. It checks the NRO's version and that no absolute path
   of the build machine is left in it, and writes the zip,
   `SHA256SUMS.txt` and the release notes to `private/releases/vX.Y.Z/`, with the ELF for crash reports.
6. **Tag and draft:** `git tag vX.Y.Z`, push the tag, then the `gh release create ... --draft` command the script prints.
   Read the draft on GitHub before publishing it.
7. **Back to test builds:** `cmake -S source/switch -B source/switch/build-nvpgo -DLSW_RELEASE_VERSION=` clears the
   version, so the next test build carries its `logN` name again.

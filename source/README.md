# Development source snapshot

Start with ../README.md and ../docs/BUILDING.md. This tree contains the maintained
runtime, Mesa and experimental Deko source at the handoff baseline. It is not a
complete fetched dependency checkout or generated game source. Own game inputs,
dependency preparation and documented generation steps are required for a full
build. Nothing in this handoff installs software or fetches dependencies.

source/patches/XenonRecomp.patch and xenia-canary.patch record CURRENT cumulative
dependency source changes against the pins in ../provenance/dependencies.json.
For historical reference generation, BUILDING.md instead specifies the separately
recorded provenance/generator-reference.patch; do not mix the two generator states.
The game output patch is the current cumulative315-file hand-edit delta applied
once to the matching folded baseline. Older generated-output fix patches/scripts
must not be applied again on top of it. Fresh code generation remains unvalidated.

Upstream port provenance: https://github.com/KingDariusStim/LSWTCS-360-to-Switch-Port
base 92ec175285504455777ccd14d2d0dcf19642694d. Its original licence is kept at ../provenance/ORIGINAL_LICENSE-CC-BY-NC-SA-4.0.txt; this repository is GPL-3.0 (../LICENSE).
Git history, account configuration, generated game output and game data are absent.

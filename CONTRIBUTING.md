# Contributing

There are two ways to help: test on a Switch and bring logs, or change the code. Both are welcome.

## Testing

[docs/playing.md](docs/playing.md) covers the testing profile, the mark combo, and what to send. A useful report has:
- the folder from `sdmc:/lswtcs/logs/`;
- handheld or docked;
- forwarder or Homebrew Menu;
- where you played;
- what you noticed.

Every session that leads to a change is written up in [docs/progress-log.md](docs/progress-log.md): what was measured,
what it showed, and the build that followed.

## Changing the code

1. **Start from the current state:**
   - the end of [docs/progress-log.md](docs/progress-log.md);
   - the last section of [docs/reviews/CPU_ROADMAP.md](docs/reviews/CPU_ROADMAP.md) for what limits each mode;
   - [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the code map.

   For anything larger than a fix, open an issue first, so two people do not change the same files.
2. **Build:** [docs/BUILDING.md](docs/BUILDING.md). [docs/VALIDATION.md](docs/VALIDATION.md) has a small regression that
   needs no game and no Switch toolchain.
3. **Keep a change to one thing**, and make it measurable:
   - Put a switch in `env.txt` (`LSWTCS_...`) when the change could be wrong on hardware.
   - Log what the change does with a tag (`[NAME] ...`), so a session shows whether it worked.
   - New Mesa changes go in a `source/switch/mesa/*_edits.py` script listed in `source/switch/tools/build_mesa.sh`, not as edits to a
     downloaded tree.
4. **Claim only what was measured.**
   - FPS between two sessions varies by about 10 %, even on the same route.
   - Compare on the same route and mode, or with the in-session A/B states (`LSWTCS_ABSTATES`).
   - CPU per guest call ([`tools/guest_call_cost.py`](tools/guest_call_cost.py)) varies by about 1 % between sessions.
     [`tools/analyze_run.py`](tools/analyze_run.py) summarises the rest of a session.
   - Do not call a change a gain while its interval still includes zero.
5. **Keep what works.**
   - No per-scene or per-address exceptions.
   - No dropped draws, textures or movies.
   - No silent quality, clock or game-speed changes.
   - Never change the save identity.
6. **Add an entry to [docs/progress-log.md](docs/progress-log.md)** with the evidence, and say what is not verified.

## Never commit

Never commit any of these:
- game files;
- generated game code (`source/Convert 360/LSWTCS/output*`);
- `image.bin` / `pristine.bin` / `xex_header.bin`;
- saved shader lists (`glprogs2/`);
- PGO counts;
- saves;
- keys;
- memory captures;
- build folders;
- `.elf` / `.nro` / `.nacp`;
- personal paths;
- anything under `private/`.

`.gitignore` covers most of them; check `git status` before each commit anyway.

## Code from other projects

Keep the notices of every component ([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)). The repository is GPL-3.0, like
bo2-360-nx, rr6-nx and mk9-nx: code from them can be used, with their copyright notices kept in the file and a line in
THIRD_PARTY_NOTICES.md. Code under a licence that GPL-3.0 cannot take in stays out.

## Rules for coding assistants

[AGENTS.md](AGENTS.md) holds the rules that automated assistants follow in this repository. They apply to people too
where they make sense.

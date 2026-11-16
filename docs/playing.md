# Playing and testing

This page is for people who have built the port (see [BUILDING.md](BUILDING.md)) and want to play it, or to test it
and bring logs back.

## What goes on the SD card

```text
sdmc:/
  switch/lswtcs/lswtcs.nro     the app you build
  lswtcs/
    env.txt                    the settings profile (below)
    default.xex                from your own disc of the game
    game.DAT  game1.DAT        from your own disc of the game
    Movies/   audio/           from your own disc of the game
```

The game checks these files at start. If one is missing, has the wrong size, or `default.xex` is not the USA/Europe
Rev 1 release, a screen names it; press Plus to close. A card set up before log107 with `image.bin`, `pristine.bin`
and `xex_header.bin` instead of `default.xex` still works.

Keep the NRO at `switch/lswtcs/lswtcs.nro`. sphaira shows only `<folder>/<folder>.nro` inside a subfolder, and a
forwarder points at that exact path.

The game adds its own files under `sdmc:/lswtcs/`:
- `glprogs2/`: the shader programs it has seen, so the next boot prepares them early;
- `bopool.bin`: the texture memory it needs per level;
- `logs/`: one folder per session.

These files come from your game. Do not share them, except the logs (see "Testing").

## Two profiles: playing or testing

`env.txt` chooses how the game runs. Copy one of these files to `sdmc:/lswtcs/env.txt`:

| Profile | For | What it does |
| --- | --- | --- |
| [`profiles/player.env`](../profiles/player.env) | Playing | The tuned settings, diagnostics off. The game writes only a short `stdout.log`. |
| [`profiles/hardware-ab.env`](../profiles/hardware-ab.env) | Testing and bringing logs | The same settings plus the diagnostics: frame log, sampling profiler, GPU timings, render reports, capture combos. It runs slightly slower and writes about 120 MB of logs per hour. |

Both use the same fixes, the same shader and texture handling and the fourth core when it is available. Results
from a test session therefore apply to the player profile.

The other files in `profiles/history/` are profiles of earlier experiments. Do not use them for play.

## Controls

The port maps the Switch controller to the Xbox 360 pad.

| Switch | Xbox 360 |
| --- | --- |
| Plus | Start |
| Minus | Back |
| L / R | LB / RB |
| ZL / ZR | LT / RT |
| Sticks, stick clicks, D-pad | the same |
| Face buttons | see below |

- **Face buttons.** By default (`LSWTCS_PADLAYOUT=auto`), menus use the labels (Switch A confirms, as on the Switch).
  In play they go by position, so the bottom button jumps as on the 360. Hold **L + R and press Minus** to cycle
  through position, label and auto.
- **Second player.** In the game, the second controller joins with Plus. **L + R + Plus** opens the Switch
  controller screen to pair or reorder controllers. On a single sideways Joy-Con, SL + SR + Plus or Minus does the
  same.
- **Mark:** hold **ZL + ZR and click the left stick** to mark the moment in the log. Use it when something looks or
  sounds wrong.
- **Capture (testing profile only):** hold **ZL + ZR and click the right stick** to save a full capture of the frame.
  It is written in the background over a few seconds.

## The fourth CPU core and forwarders

From the Homebrew Menu, a homebrew app gets three of the Switch's four CPU cores. A **forwarder** is a small title
on the HOME menu that starts the NRO, and it can ask the system for all four.

- **Recommended:** in [sphaira](https://github.com/ITotalJustice/sphaira), highlight the game, press **X**, choose
  **Install Forwarder**, set **CPU Cores: 4** and **Address Space: 39-bit**, then **Create Forwarder**.
- **One forwarder is enough.** Other forwarder tools also start the game, but without that option it runs on three
  cores.
- The port adapts: on three cores it runs as before, and on four it moves background work to the fourth core.
  - On four cores the shader programs are prepared about twice as fast after boot, and stalls in the first minutes
    are shorter.
  - Frame rate in heavy scenes does not change, because the GPU limits it on handheld.
  - The session log shows which case you got: `[NX] process core mask 0xF` means four cores, `0x7` three.
- **Updating the game:** replace `sdmc:/switch/lswtcs/lswtcs.nro`. The forwarder opens that path, so it starts the
  new build without being made again.
- **Name and icon:** a forwarder copies the NRO's title and icon when you make it. Current builds are titled
  *LEGO Star Wars: The Complete Saga*, with the game's cover as the icon. A forwarder made from an older NRO keeps
  its old name and icon until you make it again.

## Saves, and what uninstalling removes

- **Saves** go to the console's own save storage under a fixed ID of this port. That ID is not the forwarder's and
  not the NRO's file.
- **Deleting the forwarder** from the HOME menu removes only that shortcut. The NRO, the game files in
  `sdmc:/lswtcs/` and the saves stay. By how the system ties save data to a title's ID, the saves should survive.
  This has not been tested, so back them up first with a save manager that lists all save data.
- **To remove the port completely,** delete `sdmc:/switch/lswtcs/` and `sdmc:/lswtcs/`. The saves stay in the
  console's save storage until a save manager removes them.
- **Replacing the NRO** with a newer build keeps the saves.

## Testing: how to bring logs

1. Use the testing profile (`profiles/hardware-ab.env` as `env.txt`).
2. Play normally. Use the mark combo when something goes wrong.
3. Close the game from the HOME menu. The game has no quit option, and closing it this way is safe for the logs.
4. Copy the newest folder from `sdmc:/lswtcs/logs/` and send it, with a few words:
   - handheld or docked;
   - launched from a forwarder or the Homebrew Menu;
   - where you played;
   - what you saw or heard.

`stdout.log` names the console user that loaded the save. Remove that line if you prefer. Never send the game files,
`glprogs2/`, saves or keys.

What happens to the logs, and how a result becomes a change, is in [CONTRIBUTING.md](../CONTRIBUTING.md) and
[progress-log.md](progress-log.md).

# Reload From DD

Your equipment takes its consumables straight from the **Dimensional Depot**:

- **Weapon ammo**: rifle ammo, rebar and nobelisks, when reloading
- **Jetpack fuel** and **chainsaw fuel**, when refuelling
- **Gas mask filters** and **hazmat suit filters**, when a new filter is needed
- **Healing items** (berries, nuts, mushrooms, inhalers) used from your hand

Nothing has to be in your inventory. Counts that the game shows for these items, such as spare ammo and the jetpack's fuel count, include what's in the depot.

## Installation

You need Satisfactory 1.2 (build 517554 or newer) with Satisfactory Mod Loader (SML) 3.12 or newer installed.

The mod isn't on the Satisfactory Mod Repository yet, so Satisfactory Mod Manager can't install it. Install a release from this repository by hand:

1. From the Releases page, download the zip for where it will run:
   - `ReloadFromDD-Windows.zip` for your game
   - `ReloadFromDD-WindowsServer.zip` or `ReloadFromDD-LinuxServer.zip` for a dedicated server
2. Extract it into `FactoryGame/Mods/ReloadFromDD/` inside the game or server folder, so that `ReloadFromDD.uplugin` sits directly in that folder.
3. Start the game or server. The mod appears under **Mods** in the main menu.

In multiplayer, every player and the host or server need the same version.

## Inventory or depot first?

The mod follows the game's own setting, **"Take from inventory before Dimensional Depot"** (the same one that building and crafting use). Each player's own setting applies to them.

- **Inventory first:** your inventory is used first and the depot covers the rest. For healing items, the stack in your hand is used first; when only one is left, the depot pays instead, so the item stays in your hand while the depot has more.
- **Depot first:** the depot is used first and your inventory covers the rest. For healing items, the depot pays while it has stock.

## Settings

Open **Mods → Reload From DD** from the main menu or the pause menu. Each kind can be switched off; all are on by default. Changes apply immediately.

| Setting | What it covers |
|---|---|
| Weapon ammo | Rifle ammo, rebar and nobelisks |
| Jetpack fuel | All jetpack fuels |
| Chainsaw fuel | Solid biofuel for the chainsaw |
| Gas mask and hazmat filters | Gas mask filters and iodine-infused filters |
| Healing items | Berries, nuts, mushrooms and medicinal inhalers |

A switched-off kind behaves exactly like the base game.

## Multiplayer

- **Install the mod on the server or host and on every player's game.** Players without it can't join.
- **The host's switches are used, or the server's on a dedicated server.** Changing Mod Options as a joined player does nothing while you are connected.
- **All players share one depot,** as in the base game.

### Dedicated servers

Dedicated servers have no menu. Use either of these:

- **Edit the config file.** It is `FactoryGame/Configs/ReloadFromDD.cfg` in the server's install folder, and it is created on first start. Set a switch to `false` to turn that kind off, then restart the server:
  ```json
  {
      "Ammo": true,
      "JetpackFuel": true,
      "ChainsawFuel": true,
      "Filters": true,
      "Healing": true,
      "SML_ModVersion_DoNotChange": "0.1.0"
  }
  ```
- **Use the console.** Run these commands from the Server Manager console, which needs admin rights:

  | Command | Effect |
  |---|---|
  | `RFDD.Status` | Shows which kinds are on. |
  | `RFDD.Set Ammo false` | Changes and saves a switch (`Ammo`, `JetpackFuel`, `ChainsawFuel`, `Filters` or `Healing`). It applies within a second. |

## Limits and notes

- **Vanilla Addons:** the Vanilla Addons mod has the same feature. If it is installed, this mod stays inactive (and says so in the log), so items are never counted or taken twice. Use one or the other.
- **Other mods:** other equipment that uses consumables is not covered. Only the base game's equipment listed above is.
- **Saves:** nothing is written into your save. Removing the mod returns everything to the base game's behaviour.
- **No depot built:** like building from the depot in the base game, the mod uses whatever the depot holds.
- **Known issue: HUD counters.** The spare-ammo counter next to your weapon (rifle, rebar gun, nobelisks) and the chainsaw's fuel counter show only what is in your inventory, not the depot. Reloading and refuelling from the depot still work. The jetpack's fuel counter does include the depot.

## Status

Version 0.1.0 has been tested on Windows in single player, as a listen-server host with a joined player, and on Windows and Linux dedicated servers with a joined player, using a late-game save. Tests were driven by console scripts that set exact inventory and depot counts:
- every kind, with the depot only and with inventory plus depot, under both priority settings
- each switch turned off, including a host or server turning one off for a joined player
- a joined player's reload, eating, filters and jetpack: the server takes the items from the depot and the player's game shows matching counts
- a manual playthrough of every item, including the hazmat suit against radiation and the Mod Options menu
- edge cases: a full inventory, the depot emptied during a reload, less than a full magazine available, a jetpack with only a non-preferred fuel, and no fuel at all

It has not been tested in an internet game across several PCs; all multiplayer tests ran on one PC.

## Building from source

1. Set up the [SML starter project](https://docs.ficsit.app/satisfactory-modding/latest/Development/BeginnersGuide/index.html) with the Unreal Engine version it requires.
2. Clone this repository into the starter project as `Mods/ReloadFromDD`.
3. Package it with Alpakit. Building the LinuxServer target also needs Unreal's Linux cross-compile toolchain.

`Tools/create_config_asset.py` creates or updates the Mod Options asset (`Content/ReloadFromDDConfiguration.uasset`) and `Tools/verify_config_asset.py` checks it. Both run headless in the editor, with instructions at the top of each file. `Tools/make_icon.py` redraws the icon and needs Python with Pillow.

For development, set `RELOADFROMDD_DEV_COMMANDS=1` in `ReloadFromDD.Build.cs`. That adds `RFDD.*` test commands and a script runner; the test scripts are in `Tools/tests/`. `Docs/recon.md` lists the game functions the mod hooks.

## License

Copyright (C) 2026 Zambie223

Reload From DD is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version. It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See [LICENSE](LICENSE) for the full text.

## Responsible AI use disclosure

Most of the code, tooling and documentation in this mod were written by an AI coding assistant (Claude, made by Anthropic, used through Claude Code), working under the maintainer's direction. The maintainer set the requirements and design decisions. Correctness was checked with scripted runs against a real Satisfactory game client and dedicated servers (see Status above), but not every line has been reviewed by a person. AI-written code can contain mistakes: review it before relying on it, and back up your saves.

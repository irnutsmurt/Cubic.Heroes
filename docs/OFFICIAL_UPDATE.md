# The official v1.01 update

3D Dot Game Heroes (US, BLUS-30490) had one official update, **version 1.01**. Cubic.Heroes plays **v1.00** fully;
v1.01 is **optional** and is never installed without your choice.

## Official Game Update v1.01

This optional update adds new characters and several gameplay improvements!

What's included:

- **51 additional characters:** 50 original Hall of Heroes creations, plus Sackboy from LittleBigPlanet.
- **Improved New Game+:** Certain swords purchased from King Block now carry over into future playthroughs.
- **Easier Dash Circuit challenges:** More forgiving completion times.
- **Loading Art Gallery improvements:** Previously missing loading-screen artwork can now appear and be collected.

These characters come with the official update. The [Character Store](CHARACTER_STORE.md) is separate: its 352
historical Hall of Heroes characters stay available whether or not you install v1.01.

## For players

- **After v1.00 is installed**, the launcher offers the update once, with the summary above: **Install v1.01** or
  **Not Now**. Not Now is remembered, so you aren't asked again. **Install / Manage > Game Version** installs it
  later, or switches between the two versions.
- **What the launcher needs, and finds by itself:**
  - **The update package.** Sony's official package, downloaded from Sony (186 MB). Or one you already have: its
    official file name beside your disc, in your Downloads folder, or chosen. Or the update an RPCS3 you set up has
    installed. Whatever is used must be exactly the official package or files (sizes and SHA-256); anything else is
    refused.
  - **The keys that open it.** Yours if you have them; otherwise downloaded automatically from the same pinned public
    sources as the game's keys ([INSTALLING.md](INSTALLING.md#your-keys)). An RPCS3 you have is an optional fallback.
- **Before anything changes**, every check is listed in plain words: the game, the update package, opening it, disk
  space, permission and the system. Each one that fails says what to do, with a button: Retry, Download the official
  update, Choose a file, Select keys, Locate RPCS3, or Continue with v1.00. Install stays disabled, saying why, until
  every check passes.
- **Installing:**
  1. your saves are backed up;
  2. the update is downloaded (if needed), unpacked, checked, and its program decrypted and checked;
  3. the game is recompiled for v1.01 and its shaders are prepared.

  Only then is v1.01 made the active version. If anything fails, **v1.00 stays as it was and playable**, and what
  was already checked is kept, so trying again is quicker.
- **Kept as they are:** your saves, your characters (imported, downloaded or made), your settings and your shader
  caches. The v1.00 build is kept too, so switching back is instant.
- **The Character Store** shows the 45 historical characters that come with v1.01 as **Installed (official update)**
  while v1.01 is the active version. They are never downloaded or copied into your library. After switching back to
  v1.00 they are downloadable again, and any you imported or downloaded yourself stay yours.

## How it works

| | |
|---|---|
| The update's files | `<install>/data/official/BLUS30490-01.01/`, as Sony's package lays them out, apart from the saves (`data/gamedata`), so save backups never hold them |
| The update's program | decrypted beside its `EBOOT.BIN` (`USRDIR/EBOOT.elf`); accepted only when it is the pinned program (`config/game_version.json`, `update.decrypted_elf`) |
| The game sees them | at the PS3's own place for an installed update (`/dev_hdd0/game/BLUS30490/`), through the native runtime: the v1.01 build's `build.cfg` names the folder (`DOTHEROES_UPDATE_DIR`) and the program (`DOTHEROES_PROGRAM`); the v1.00 build's has neither |
| The v1.01 build | its own build slot, made from the v1.01 program with its own configuration (`config/programs/BLUS30490-01.01/`): the same native renderer, audio, movies, physics and SPU replacements as v1.00, no emulated path |
| The Character Store | follows `install.json`'s `official_content`, written from the **active** build at every activation (install, update, repair, rollback, switch) |
| Checks | `worker game-update-check` (`tools/update_preflight.py`) |
| Install | `worker game-update-install` (`tools/worker.py` `op_game_update`, `tools/official_update.py`) |
| Switch | `worker game-version --game-version v100\|v101` |
| Keys | `tools/key_sources.py`, `tools/key_download.py`, `config/key_downloads.json` |

Nothing of the update, its keys or its decrypted program is part of this repository or of any package. Everything is
obtained and made on the player's computer from official files.

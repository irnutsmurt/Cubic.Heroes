# M11: release readiness -- plan and checkpoint

M11 follows M9 and M9.2 (closed by the owner on 2026-11-16: docs/M9_LAUNCHER_PLAN.md, docs/M92_SHADER_PLAN.md) and
M10 (docs/M10_PLAN.md; its hands-on checks are the owner's, whenever convenient). Evidence marks as elsewhere: [C]
read in the code or the data, [M] measured, [I] inferred, [H] a hypothesis still to check.

## Checkpoint (kept current; the newest state first)

- **Stage** (2026-11-16): **M11.0, the developer debugging tools, complete** (sections 2-12) and **awaiting the
  owner's review**. Every requirement is met through the game's own logic, behind a developer-mode setting off by
  default, on test saves that never touch the player's; every stage validated offscreen and pushed (7738d7e, d58c85f,
  f045f4a, 4f1fa45, 6282559, 4ad7f3b, 1eff51a and M11.0g's commit).
- **What it does** (section 12 has the table): warp to any of 166 maps and their 395 arrival points; life, magic,
  money, invulnerability, no death; flags, the story in 26 steps, quests, items; bosses fought again the game's way,
  the actors, a map's enemies again, events and scenes by name, the ending; pipeline coverage live and exported,
  frame times, memory, the native systems' reports, hitch marks; test states; a test channel and a regression suite
  for all of it; the in-game menu's Developer pages for all of it.
- **Tests**: tests/devtools_suite.py -- eight scenarios and the install checks -- all pass on the final build; the
  map sweep through `warp` passes but for Field_14's 16 new pipelines (content that differs between runs, kept for the
  coverage work); unit tests (test_dev_saves, test_map_sweep and the existing ones) pass.
- **Known limits** (section 12): no arbitrary enemy or boss outside its arena (no script native; the game's own
  arenas and spawns are used); which sword or shield is equipped is the game's own equipment screen; a flag through
  the game's own save and load, the ending, and a boss fought to its end are not run in a test; the on-screen tag and
  the launcher's switch not seen on screen (offscreen only, by the owner's rule).
- **Outstanding decisions**: none blocking. Proposed ("decided here", can change at review): the developer-mode switch
  on the launcher's Settings, Diagnostics; the 26 story steps' labels; the Developer pages' order.
- **Next** (the owner's order, after the review): comprehensive Windows gameplay validation with these tools, bug
  fixes, release preparation; collecting the pipelines M11's play meets (`coverage export`) into the preparation.

## 1. The owner's request (2026-11-16)

On closing M9.2 and M9:
- **Graphics pipeline libraries stay off by default** until further hardware validation; the capability-gated
  implementation and the standard Vulkan pipeline path (the fallback) are kept.
- **The map sweep becomes part of the permanent regression tests.**
- **During M11, keep collecting pipelines no recording has met** and improving the preparation's coverage --
  specifically bosses, enemies, items, equipment, special effects, events and progression-dependent rendering the
  map sweep did not exercise. Complete pipeline coverage is not claimed until it is demonstrated.

**M11.0 -- developer debugging tools**, before full-game testing. A comprehensive developer debug interface:
- warp to any map, dungeon, floor or valid entry point;
- boss and enemy spawning;
- progression and event-state inspection and manipulation;
- quest and inventory management;
- equipment and item manipulation;
- health, magic and currency controls;
- invulnerability and no-death options;
- replaying or triggering events and cinematics;
- shader and pipeline coverage monitoring;
- performance and native runtime diagnostics;
- automated testing hooks for all of the above.

Reuse the native map change found in M9.2g. The interface matches the existing in-game overlay and is reached
through a developer-mode setting, **disabled by default**. **Normal player saves are protected**: debugging and
progression manipulation use disposable test saves or isolated copies. The design and the implementation stages are
documented before starting; each completed stage is validated and pushed.

**After M11.0**: comprehensive Windows gameplay validation, bug fixes and release preparation. Then Linux and AMD
validation on the owner's Manjaro laptop (Ryzen, Radeon); the Linux launcher distributed as an AppImage that
generates the game's AppImage locally from the player's files. Steam Deck validation needs community volunteers.

Unchanged: M7 unauthorized; no public release; every licensing, privacy, distribution and desktop safeguard binding.

## 2. M11.0: the principles

1. **The game's own logic does the work.** A debug action goes through the function the game itself uses for it --
   its script layer's natives (section 3.4: Event:SetMap, Event:SetEventFlag, Event:SetMoney, Event:AddItem2,
   Event:PlayScene, ...), run on the game's own thread between two frames, as the map sweep runs Event:SetMap -- so
   the game's bookkeeping (the HUD, the save's fields, the triggers that follow a flag) stays consistent. Guest memory
   is written directly only where no game function exists, and each such place is documented with its evidence.
   Reading state (for the menu and the tests) may read memory directly.
2. **Off by default, and invisible when off.** One setting, `DOTHEROES_DEVELOPER_MODE` (default 0). With it off the
   game behaves exactly as now: no Developer entry, no command file read, no hook doing work, no other save folder. A
   test checks that.
3. **The player's saves are never touched.** In developer mode the game runs on a save tree of its own (section 3.2);
   the player's saves are read only to make an isolated copy, on request; nothing is ever written back into them.
4. **The same program the player runs.** The tools are in the shipped build (behind the setting), so M11 tests the
   program a player gets, not a separate debug build.
5. **Nothing game-derived in the repository or the download.** The tools' lists (maps and their entry points, items,
   flags, events, characters) are made on the player's machine from the player's own files when first needed, and
   kept beside the install; the repository has only the code that reads them.
6. **One implementation, two front ends.** Every action is a text command (section 3.3), executed by one interpreter;
   the in-game Developer pages and the automated tests issue the same commands, so what the tests cover is what the
   menu does.
7. **Tests stay off the desktop** (tests/desktop_guard.py), and every run records the developer options it used (its
   run.json), as the existing switches do.

## 3. M11.0: the design

### 3.1 Developer mode
- `DOTHEROES_DEVELOPER_MODE` (bool, default 0) in config/settings_schema.json, category Diagnostics, label "Developer
  mode", help: "Testing tools in the in-game menu. The game uses separate test saves while it is on; your own saves
  are not used or changed." Shown in the launcher's Settings, Diagnostics (beside Log detail) -- decided here: the
  owner asked for a setting; it sits at the end of the page players least use. The worker's command line sets it too.
  It applies at the next start (the save folder is chosen at start).
- The game, started with it on: a small permanent tag in a corner, "DEVELOPER MODE -- TEST SAVES", so a session can
  never be taken for normal play; the in-game menu gains a **Developer** item; the command channel (3.3) listens.
- Environment for tests: the same variable (the settings file's lines are environment defaults), so a test turns it
  on without touching any settings file.

### 3.2 Test saves: the player's saves protected
- In developer mode the game's save roots (PS3_GAMEDATA_ROOT, PS3_HDD0_ROOT, PS3_HDD1_ROOT: port/main.cpp) point at
  `<install>/data/devmode/gamedata` instead of `<install>/data/gamedata`. The game's system save, its saves and its
  trophies there are the test saves'; the player's tree is not opened. (A test run sets those roots itself, to its run
  directory, as now: the environment wins.)
- The test tree starts empty (a new game). **"Copy my saves into the test saves"** (the launcher, while the game is
  closed, and the Developer menu at the title) replaces the test tree with a copy of the player's -- read-only on the
  player's side; the copy is checked (sizes, every file) before the old test tree is removed. There is no operation
  in the other direction.
- **Test states**: named snapshots of the test tree (`<install>/data/devmode/states/<name>/`) -- "Keep this test
  state" and "Restore a test state" in the Developer menu (at the title, or with a restart: the game reads its save
  list from the folder) and the command channel; a state is how a test starts again from a known point. The
  repository's checkpoints for tests (build/checkpoints) stay as they are.
- The launcher's save backups, save browser, Export Logs and uninstall leave `data/devmode` alone, except uninstall,
  which removes it with the game (it holds only test data).

### 3.3 The command channel
- One interpreter, `port/devtools/` (new): commands as text lines (`warp D_1_1_1 -gDebug`, `flag set 123 1`,
  `money 500`, `give <item> [n]`, `life <n>`, `nodeath on`, `spawn <name> [x z]`, `event <name>`, `status`, ...;
  section 3.5 lists them), each answered with one JSON line (`{"id":..,"ok":..,"out":..}`).
- **Where they run**: queued, and executed on the game's own main thread -- the thread that runs every Lua script
  (section 3.4) -- at the start of a frame's flip (the entry of 0x0069DD60, where the map sweep calls Event:SetMap:
  after the frame's update, no script running). Commands that change the game take one per frame; reads take none.
- **While the in-game menu holds the game**, the main thread waits in the runtime's hold (ps3_guest_pause_wait, at
  its flip request). The hold's wait loop gets a hook that runs queued commands there, on that same thread, so the
  Developer pages read and change the game while it is paused [H: that the thread held at the flip is the main
  thread that runs Lua -- checked first in M11.0a; if it is not, the pages show what was read before the hold and
  actions apply on the first frame after it]. Warps and events close the menu themselves (they need frames to run).
- **Producers**: the Developer pages (3.6); the test channel -- in developer mode (a test sets
  DOTHEROES_DEVELOPER_MODE=1 in its environment; its saves are its run directory's), `dev.cmd` in the game's working
  directory (written whole by the client once the game has taken the previous one; read and removed every 50 ms; a
  line `@ID COMMAND` names its answer), answers appended to `dev.out` as JSON lines; and the map sweep (since
  M11.0b a list of `warp` commands: section 7).
- **Guarded**: a command needing the game in play (not at the title, not mid-load, no menu of the game's own open) is
  refused with the reason, not queued forever; every command and its answer goes to the game's log (`[dev]`).

### 3.4 The mechanism: the game's own script layer (static research, 2026-11-16) [C unless marked]
The game's logic is scripted in Lua, and the research for M11.0 (local notes, build/m11/agents) mapped it:
- **Stock Lua 5.1.3 with tolua++ bindings**, one `lua_State` (the pointer at 0x0159C08C, owned by the engine's biLua
  object at 0x0159BE98), used only on the main game thread [I]. The scripts the game runs are **embedded in the
  EBOOT** (a 3.8 MB bundle loaded at boot, 0x0055E51C); the disc's `all.lua.eu` is never opened (68 of its 1,485
  sections differ from the embedded copy).
- **1,052 natives registered** by 16 tolua++ packages (EventClass 263, ObjectClass 321, actor_manager, map_manager,
  item, swordData, boss, ...); each wrapper checks its arguments and calls a C++ implementation. Event:SetMap's wrapper
  calls 0x004D5958, the function the map sweep already calls. Scripts reach the natives as
  `local Event=EventClass.Event; Event:SetEventFlag(5,1)` -- one global object.
- **Running a chunk from the host**, the tools' central primitive: at the flip (or in the hold), with the state idle
  (biLua's call level -1), `luaL_loadbuffer` (0x0081B060) then `lua_pcall` (0x007F3450) through the lift's function
  table, r2 = the Lua module's TOC (0x010764C4), on a guest stack region of the tools' own (the main thread's stack is
  64 KB and the Lua parser recurses); the error message read back (`lua_tolstring`); the stack restored
  (`lua_settop`). Never `lua_call` (an error there reaches Lua's panic and ends the game), never the engine's biLua
  call helpers (a failure there sets a sticky error that silences every later script call).
- **Events** are one coroutine at a time, in the Lua global `Coroutine`, started by name -- StartEvent 0x004D5BC0
  generates `Coroutine=coroutine.create(<name>)` -- and resumed by the per-frame update (0x004DA400). 22 natives (Wait,
  Fade*, Move*, Select, ...) only set the event's wait state; a host chunk never calls them, never assigns
  `Coroutine` and never yields. Replaying an event goes through StartEvent, only while no event runs.
- **The player's own functions** exist in the EBOOT but their bindings were never registered: CPlayerActor setHP,
  setMP, setMaxHP, setMaxMP, getHP, getMP, damageHP (0x0011A94C), setInvincibility (0x00101F78), isNoDamage, isDead.
  The tools call these implementations on the player object the game's actor manager returns (getPlayer), on the
  main thread -- the game's code, not a memory write.
- What has no game function (direct memory, each place documented when found): the exact count of an item and its
  removal (the scripts only add), the ammunition maxima, the sword upgrade state beyond swordData's natives.
- Open points to verify at run time (M11.0a/c): that `EventClass.Event` (0x02AE1568) is the game manager's event
  object (0x02AE8704 + 0x8F0); that SetMap takes a door-style entry (`-g212;-o212`) for the 51 maps without `-gDebug`;
  that a boss can be created outside its arena (its resource set comes from the map's header).

### 3.5 The features, as commands
Each line: the command, what does it in the game, and its test. Names are the game data's own (maps `D_1_1_1`,
`Field_17`; items and flags by number, with the catalog's names).
- **Warp** -- `warp MAP [ENTRY]` (ENTRY defaults to `-gDebug` where the map has it, else its first arrival point):
  Event:SetMap, as the game's scripts and the sweep do. `maps` lists the catalog's 167 loadable maps, their kind
  (field, town, temple floor, boss room, sage room, tower floor, cave, minigame) and every arrival point (501).
  Test: the sweep through `warp` with the arrival check (section 5); a door-style entry on a village interior.
- **Progression and event state** -- `flag get N`, `flag set N V`, `flags [FROM TO]` (Event:GetEventFlag /
  SetEventFlag; one store of 277 integer flags, 0-98 named in the scripts, 99 the game cleared, 100-326 numbered),
  `progress` (the main story's 27 steps reconstructed from the flags, from the hero's dream to the ending: which are
  done), `progress set STEP` (sets every flag up to that step, so a later area plays as it would; the world map's
  progress flags with Event:SetProgressFlag). Test: set, read back, survive a save and a load in the test saves.
- **Quests** -- the side stories are flags 100-326 grouped by the person or event that sets them (the catalog);
  `quests` lists them with their state; set and reset as flags.
- **Inventory, items and equipment** -- `give ITEM [N]` (Event:AddItem2, the game's own item-get path; items 1-159),
  `items` (Event:GetItem for each), `equip sword|shield ID` (Event:EquipSword / ChangeSword, the shield's natives),
  `sword ID STAT LEVEL` (swordData's natives, the Blacksmith's stats), `remove ITEM` / `count ITEM N` (no game
  function: memory, documented when found in M11.0d). Test: give, read back, the HUD and the save show it.
- **Health, magic and currency** -- `life N`, `maxlife N`, `magic N`, `maxmagic N` (CPlayerActor setHP / setMaxHP /
  setMP / setMaxMP), `heal` (Event:HealPlayerHp / HealPlayerMp), `money N` (Event:SetMoney, 0-9999), `blocks N` (the
  Small Blocks, through their item). Test: each value read back by the game's own getters and shown by the HUD.
- **Invulnerability and no death** (both off by default, each logged and recorded in a test's run.json) --
  `invulnerable on|off`: the game's own CPlayerActor::setInvincibility, held while on [H: whether the game clears it
  itself, e.g. after a hit's blink -- then it is set again each frame]; `nodeath on|off`: **only a lethal hit is
  stopped** (the owner's 2026-11-01 requirement: hits, knockback, enemies and the sword beam's full-life rule all
  unchanged) -- an entry hook on the player's damage function (damageHP 0x0011A94C) lowers a lethal hit's damage so
  one unit of life remains [H: that every hit reaches it -- its callers checked in M11.0c]. The existing
  DOTHEROES_DEV_CANT_DIE (which heals below 4 apples) stays for the benchmarks that use it; the new one replaces it
  elsewhere.
- **Spawning bosses and enemies** -- `boss NAME` (the game's way: the boss's arena with its defeat flags cleared, so
  the arena's own entry event starts the fight; the boss rematch flags of the Dark Tower likewise), `spawn boss NAME
  X Z` (Event:CreateSnake/Bee/Golem/Kraken/Dragon/DarkKnight/Priest/Devil, CreateGuard: within an arena whose
  resources are loaded), `spawn npc SCRIPT X Y Z` (Event:CreateNpc), `respawn` (the map entered again: its own enemy
  spawns), `enemies` (the actor manager's actors, with life). An arbitrary enemy in any map has no script native: the
  actor manager's own creation is investigated in M11.0e and offered only if it works through the game's code.
  Test: each boss's fight starts and the boss takes damage; a spawned NPC appears in the actor list.
- **Events and cinematics** -- `events [MAP]` (the catalog's 604 event functions, 146 cinematic), `event NAME`
  (StartEvent, only when no event runs; optionally after `warp` to its map and `progress set` to its point), `scene N`
  (Event:PlayScene, a boss's introduction in its map), the movies (the opening, the ending and its credits) through the
  game's own movie start. Test: an event runs to its end (the event state back to idle) offscreen.
- **Test saves** -- `saves copy-player` (at the title or with the game closed), `state keep NAME`, `state restore
  NAME`, `states` (section 3.2).
- **Shader and pipeline coverage** -- `coverage`: this session's pipelines built in play, prepared and unforeseen (with
  their time), translations compiled in play, from the renderer's own counters; the unforeseen ones are already
  recorded for the next preparation (DOTHEROES_RECIPES); `coverage export FILE` writes them with the map, area and
  event they were met in, so M11's runs feed the manifest (tools/pipeline_manifest.py) -- the owner's request to keep
  collecting bosses, enemies, items, effects and events. A small live counter can stay on screen.
- **Performance and the native runtime** -- `perf`: frame times (mean, p99, the longest, frames over 50 and 100 ms),
  the frame limit and VSync, per-thread CPU, the guest and host memory, the GPU and driver; `runtime`: the native
  systems' reports now rather than at exit (physics tasks, inflation, water, waver, flip waits, the PPFX renderer);
  `mark` (a hitch mark, as F8); `trace N` (the frame trace for N frames).
- **Status and the raw layer (developers only)** -- `status` (map, area, the hero's position, life and magic, money,
  the event state, the frame); `lua CHUNK` (a chunk in the game's state, its result or error returned -- for
  investigation and new tests; the menu does not offer it).

### 3.5a What the tools never do
- Write the player's saves (3.2), touch anything outside the install, or run with developer mode off.
- Call a native that yields or waits from a host chunk, or start an event while one runs.
- Ship game data: the catalog and every research note stay on the player's machine (3.7).

### 3.6 The Developer pages (the in-game menu)
- The overlay's look and behaviour (port/overlay.cpp: the panel, the pixel titles, the gold focus, the prompts; the
  controller, keyboard and mouse), with what the settings pages lack: **scrolling lists** (hundreds of maps or
  flags), grouped (a dungeon's floors together), with a filter by group and, on a keyboard, by typed text.
- **Developer** (on the main page, between Performance and Return to Launcher, only in developer mode) opens:
  Warp; Player (life, magic, money, no death, invulnerable); Items & Equipment; Progression (flags, quests, stage);
  Spawn; Events & Cinematics; Test Saves; Pipeline Coverage; Diagnostics.
- Each value shown is read when the page opens and after each action; actions go through the command channel.

### 3.7 The catalog: made locally
- Made by the game itself, in developer mode, the first time the Developer pages or the test channel's `catalog` need
  it -- so it always matches what the game runs, with nothing but the player's own files and no other program:
  - the maps and their arrival points, from the map files on the player's disc (data/map/pac_data, map_alignment:
    the named arrival groups, as tests/map_sweep.py reads them);
  - from the running game's own Lua tables, through one chunk (3.4): the named flags, the item table, the event
    functions (the script globals the game starts by name), the character list -- the scripts the game runs are the
    bundle embedded in its EBOOT, not the disc's all.lua.eu;
  - Kept at `<install>/data/devmode/catalog.json`, made again when the game's program changes (its hash). The game
    data's own identifiers; never committed, never in the download.
- **In the repository, in our own words**: the main story's steps as flag numbers with our own short labels ("Temple
  1's boss defeated"), each boss's arena and defeat flags, the kinds of map. Facts about the game, as the code's
  addresses are; nothing copied from its text. The research notes behind them (build/m11/agents) stay local.

### 3.8 Automated testing hooks
- `tests/devtools.py`: a client for the channel (send a command, wait for its answer, `status` as a dict) and the
  checks built on it.
- tests/drive.py: `dev <command> [timeout]` (sends, waits, fails on an error answer) and `devstate <cond>[,...]`
  (waits on `status` fields, as `state` does on state.txt).
- Suites (offscreen): the channel and each command against its effect; developer mode off changing nothing; the save
  isolation (the player's tree byte-identical after a developer session); the map sweep through `warp`.

## 4. M11.0: the stages
Each stage: implemented, tested offscreen, recorded in this document's checkpoint, committed and pushed before the
next. The Developer pages grow with the stages (each stage adds its page).

| Stage | Content | Validated by |
|---|---|---|
| **M11.0a** Foundation | `DOTHEROES_DEVELOPER_MODE` (schema, launcher's Diagnostics, worker); the test saves' tree and "copy my saves" (3.2); the developer tag; `port/devtools/` with the command queue at the flip and in the hold, the test channel (dev.cmd/dev.out), the Lua chunk runner (3.4) on its own guest stack; `status`, `lua`; the runtime checks of 3.4's open points; tests/devtools.py, drive.py `dev`/`devstate` | mode off: no Developer item, no channel, the normal save root (a test); mode on: the player's saves byte-identical after a session that saved; `lua` returns values and errors without disturbing the game (an error chunk, then normal play); the hold-thread check |
| **M11.0b** Warp and the catalog | `warp`, `maps`; the catalog (3.7); the Developer pages' scrolling lists in the overlay; the Warp page; the map sweep moved onto `warp` | the full sweep through `warp` passes `check` against the baseline; door-style entries on the village interiors and caves; the page offscreen (its pictures dumped) |
| **M11.0c** The player | `life/maxlife/magic/maxmagic/heal/money/blocks`, `invulnerable`, `nodeath` (the damage hook; a relift if the hook point needs it), the Player page | each value read back by the game; a fight with `nodeath` survives lethal hits with knockback and damage otherwise normal; `invulnerable` takes no damage; both off: a death happens as before |
| **M11.0d** Progression, quests, items, equipment | `flag*`, `progress`, `quests`, `give/items/equip/sword/remove/count`, their pages | set/read back; through a save and a load in the test saves; `progress set` to a late step then a warp plays that area as the story has it |
| **M11.0e** Bosses, enemies, events, cinematics | `boss`, `spawn`, `respawn`, `enemies`, `events`, `event`, `scene`, the movies; their pages | each boss fight started and damaged (with `nodeath`); events run to their end; a cinematic replayed |
| **M11.0f** Coverage and diagnostics | `coverage` (+ export feeding the manifest), `perf`, `runtime`, `mark`, `trace`; their pages; the test-state pages | coverage figures equal the end-of-session report; an export merged by tools/pipeline_manifest.py; the runtime reports on demand |
| **M11.0g** Integration | the suites (3.8) as one regression run; the documents (this plan, HANDOFF's debugging tools, the README's developer note); the owner's review | the whole suite offscreen; a fresh install with developer mode off unchanged |

After M11.0 (the owner's order): comprehensive Windows gameplay validation with these tools, bug fixes, release
preparation; then Linux and AMD on the owner's laptop (the AppImage launcher making the game's AppImage locally);
Steam Deck through volunteers. Each gets its plan here when it starts.

## 5. The map sweep as a regression test (2026-11-16)

The owner made the sweep (M9.2g, docs/M92_SHADER_PLAN.md section 2h) part of the permanent regression tests.
`tests/map_sweep.py run EVIDENCE --shaders <a preparation's shaders>` sweeps (every map, or `--first/--last`), then
`tests/map_sweep.py check EVIDENCE --baseline build/m11/sweep_baseline.tsv` decides:
- **fails** when a chunk's game did not run to its planned end and exit cleanly (a crash, a hang the guard stopped);
  when a map was never changed to, or no arrival was logged for it; when the guest's frame counter hardly advanced
  in a map (fewer than 10 guest frames a second: the game stalled there); when a map arrived in another field area
  than the accepted baseline's; and, with a preparation given, when any shader translation was compiled in play;
- **reports** the pipelines met unprepared (new content legitimately meets new state; M11 collects them).
- The arrival: since M11.0b each map is a `warp` (section 7), answered and logged once the game has arrived there:
  `[warp] t=... arrived MAP area A frames F` (the map's number as the game keeps it and the guest's frame counter,
  the addresses dev_state.cpp reads). M9.2g's own sweep code (DOTHEROES_MAP_SWEEP in ppfx_recorder.cpp, which logged
  `[sweep] ... arrived` before each change) is removed; the check reads both.
- The baseline is written from a reviewed run (`--write-baseline`); it holds map names and area numbers -- read from
  the player's own data, so it is kept locally (build/), not in the repository.
- When to run it: after any change to the renderer, the lift, the runtime's file or memory handling, the map loading
  path, and before every release candidate; it takes about 45 minutes for all 167 maps (6 chunks).

**What the arrival check found at once** [M] (F:\3d dot game heroes\m11\evidence\sweep-full1): the first full sweep
with it, over the shipped preparation, passed every map's call -- but fields 1 to 9 "arrived" in the area of the map
before them (D_8_2's 80; the map numbers otherwise follow the game's map list: Field_10 is 9). The log shows the game
loading D_8_2 again after each of those changes: the sweep named the nine fields by their files (Field_01 ..
Field_09), while the engine, in its links between maps and its map parameters, calls them Field_1 .. Field_9, and the
map change given a name it does not know reloads the current map. M9.2g's sweep therefore never reached those nine
fields (docs/M92_SHADER_PLAN.md section 2h, corrected). tests/map_sweep.py now gives the engine's names; the nine
fields swept again (evidence sweep-fields1): each arrived in its own area (0 to 8, the game's map list), 15 pipelines
first met, **all 15 prepared** by the shipped preparation, no translation in play -- the missed fields left no gap.
Field_99, a map file the game's map list lacks, cannot be reached by the map change at all (it too reloaded the map
before it); the sweep leaves it out. The check also fails, with no baseline, a map that arrives in the area of the
map before it -- that alone flags all ten of the first sweep's misses. Every other map: 157 arrived, each in its own
area but Field_80 (in D_24_1's, 108, consistently -- through the wrong arrival point: corrected in M11.0b, section 7);
the baseline (build/m11/sweep_baseline.tsv) holds the 166.

## 6. M11.0a: the foundation (2026-11-16) [C, M]

**Built**:
- **The setting**: `DOTHEROES_DEVELOPER_MODE` in config/settings_schema.json (Diagnostics, "Developer mode", off;
  applies at the next start); the launcher's Settings show it with the other switches (launcher/main.cpp); the
  worker's `settings --set` checks it (0 or 1).
- **The test saves** (port/main.cpp, installed layout): with it on, the save roots are `data/devmode/gamedata`
  (made empty on a first session); a test's own roots still win. tools/dev_saves.py and the worker's `devsaves`
  (docs/M9_WORKER_PROTOCOL.md): `copy-player` (the player's tree only read; the copy checked file by file before the
  old test tree goes), `keep`/`restore`/`delete` named test states, `states`; all refused while the game runs from
  the install. tools/uninstall.py removes `data/devmode` with the game's data (test data only).
- **port/devtools/devtools.{h,cpp}**: inert unless developer mode is on. The command queue run on the game's main
  thread at the entry of its flip (r1_observe.cpp's observer of 0x0069DD60) and, while the in-game menu holds the
  game, from the runtime's hold (`g_ps3_guest_hold_work`, ps3recomp patch 0164) when the held thread is the flip's;
  the test channel (`dev.cmd` read and removed every 50 ms, `@ID` lines; JSON answers appended to `dev.out`; every
  command and answer in the log as `[dev]`); the tools' guest memory (384 KB from the runtime's user memory: a 256 KB
  stack for their calls, a chunk buffer); lifted calls through the function table with the callee's TOC (any
  trampoline of the interrupted guest code kept aside); the Lua runner (`luaL_loadbuffer`, `lua_pcall`, values back
  by `lua_type`/`lua_tolstring`/`lua_toboolean`, the stack restored with `lua_settop`; refused while biLua's call
  level is not -1). Commands so far: `ping`, `status` (the map's number, the guest's frames, the HUD's life, money
  through `Event:GetMoney()`, the Lua state and call level, the event state, the scripts' Event object and the game
  manager's, held, hold runs), `lua CHUNK`, `check`.
- **The tag**: "DEVELOPER MODE - TEST SAVES" in the window's top-left corner (ps3recomp patch 0164: `g_ps3_osd_tag`,
  drawn by the Vulkan backend with the messages' box routine).
- **Test hooks**: tests/devtools.py (the client; `--wait-ready`); tests/drive.py `dev`, `devfail`, `devstate`,
  `write`; scenarios devtools_basic, devtools_off, devtools_install, devtools_install_play; tests/devtools_suite.py.

**Tested** (offscreen, silent; the development build of the play lift):
- devtools_basic **PASS**: at the title and in play the channel answers; `lua return 1+1, "two", nil, true` gives
  `2 two nil true`; `Event:GetMoney()` 45 on the castle-front save; a runtime error (`error(...)`), a syntax error
  and an unknown command are each refused with the Lua or tool message, and the game goes on (its frames advance);
  **the open points of section 3.4 hold**: the scripts' Event object is the game manager's (0x02B38540 both);
  **commands are answered while the in-game menu holds the game**, on the main thread (hold runs 3, from another
  thread 0), the guest's frame counter standing still across the hold (350 to 350) and running again after it.
- devtools_off **PASS**: developer mode off, a `dev.cmd` left in the game's folder is never taken, no `dev.out`, no
  `[dev]` line.
- The install (a throwaway install with the setting on, linked to the test install's disc data, a copy of its saves;
  the game started as a player starts it): part 1 **PASS** -- the game boots on its own empty test tree (4 files
  written there: its system data), **the player's 12 save files byte for byte the same**; part 2 **PASS** -- the
  worker's `devsaves --action copy-player` (12 files, 636 KB), then CONTINUE loads the copy and plays (the castle's
  area, money 45, the frames running), **the player's saves again byte for byte the same**. (A first try failed on
  the drive's timing: an install's first session, with no shader cache, loads longer, so "Change characters?" came
  after the drive's answer; the scenario now answers until play starts.)
- Unit tests: tests/test_dev_saves.py (5: the player's tree only read, states kept and restored, refusals while the
  game runs and for bad names), tests/test_map_sweep.py (8); test_worker, test_uninstall, test_settings_diagnostics
  and the others unchanged and passing (the project's venv). The launcher builds.

**Not verified here**: the tag on screen (drawn only in a window: an on-screen check is the owner's, or a run they
allow); the launcher's Settings page with the new switch (the same code path as every other on/off setting).

## 7. M11.0b: warp, the catalog, the Developer pages (2026-11-16) [C, M]

**Built**:
- **The catalog, made by the game** (port/devtools/devtools.cpp `catalog`/`maps`): one Lua chunk reads the game's own
  map list (`map_manager.MS_*`: each map's number) and its map parameters (`MapParameter`: each map's place name);
  the map files on the player's disc give the maps that exist and, from each map's alignment file, its **arrival
  records** (u32 type 0x1005, u32 0, the name's and the kind's lengths, the name -- `-gDebug`, `-gfrom_d_1`, a
  door's `-g212;-o212;` -- and its kind: Exit, WarpDoor, PitFall, destructionWall, BlackHole, TombStone, WoodDoor;
  the developers' `-gDebug` first where a map has one). The engine's names for the first nine fields are kept apart
  from their files'; a file the game's map list lacks (Field_99) is left out. 166 maps, every one with an arrival
  point. Nothing of it is written to the repository; the place names are the player's own data, shown at run time.
- **`warp MAP [ENTRY]`**: the game's own `Event:SetMap` in a chunk, then answered when the game has arrived -- the
  map's own number reached and settled, or another number that settled (a map the game shows under another's,
  Field_80), or refused after 90 s; `[warp]` lines on the compile log's clock for tests/map_sweep.py. A map or an
  entry outside the catalog, or with characters other than letters, digits, `_`, `-` and `;`, is refused.
- **The Developer pages** (port/devtools/dev_menu.cpp, port/overlay.cpp): "Developer" on the in-game menu's main
  page in developer mode only; Developer (Warp, Status); Warp: the groups (the fields; each D_<n> with its place
  name; the others), a group's maps with their place names and arrival points, a map's arrival points (the default
  marked); choosing one warps and closes the menu; Status: the map by name and the game's state, read on the paused
  game. The menu now scrolls a page longer than its panel ("N more above/below"; left and right jump a panel's
  worth on a Developer page), sizes its rows to fit, and leaves out the resume hint where it would overlap the
  prompts. With no window (a test offscreen, DOTHEROES_OVERLAY_DUMP set) the menu composes its pictures at 1280x720.
- **The map sweep through `warp`** (tests/map_sweep.py: a generated drive per chunk, `devtry warp MAP ENTRY` -- a map
  that does not arrive is logged and the chunk goes on; the check fails it). tests/drive.py `devtry`.
  tests/boot_smoke.py records a drive on another disk.

**Tested** (offscreen, silent; the development build of the play lift):
- devtools_warp **PASS**: the catalog lists 166 maps; warps to a temple floor (D_1_1_1, its -gDebug), a field by the
  engine's name (Field_1), a village interior and a cave by their door records (D_21_2 `-g212;-o212;`, D_60_1
  `-g291;-o291;`), an arrival point given (D_7_1_1 `-g711;-o711;`), a boss room (D_1_2), a minigame (D_80_1
  `-gtd80_1`) and a field (Field_17) -- **each arrived at its own map number** (37, 0, 92, 115, 63, 38, 149, 16),
  about 2 s after the change; an unknown map and a malformed entry refused.
- devtools_menu **PASS**: through the menu's own test input, Developer, Warp, the D_1 group, D_1_1_1's arrival
  points, the default chosen -- the menu closes and the game arrives at D_1_1_1 (37); then Status read on the
  paused game (the map by name, money, life) and Back out to the game, which runs on. 26 pictures composed and
  reviewed (the main page with Developer, the Developer page, the groups with their place names and "10 more
  below", a group's maps, the arrival points, Status).
- **The full map sweep through `warp`** (evidence sweep-warp1, over the shipped preparation): **all 166 maps arrived**,
  each at its own number; Field_80 now arrives at its own number (168) through its real arrival record (the old
  sweep's string scan picked another point and arrived in D_24_1's area; baseline corrected). 218 pipelines first
  met, 202 prepared; **the one failure: Field_14** (the first map of its chunk) **met 16 pipelines and 12 shader
  translations no preparation had** (185 ms of building). Not the tools: the same first warp in four controlled
  runs -- the old sweep path without developer mode, developer mode with its memory taken at the title, the catalog
  chunk, a Lua warp with the memory taken in play -- met none, while two sweep runs met the same 16 (at Field_14,
  and at Field_13 when it came first). Content that differs between runs [H: a rare random enemy of the fields'
  spawn lists]; the 16 recipes are kept (evidence sweep-warp1/chunk5/recipes.bin) for M11's coverage work, and the
  check keeps failing such a run until the preparation has them.
- The four scenarios again on the final build (the old sweep code removed): all **PASS**.
- tests/test_map_sweep.py (9): the arrival records read from a made-up alignment file, the engine's names.

## 8. M11.0c: the player (2026-11-16) [C, M]

**Built** (port/devtools/devtools.cpp, dev_menu.cpp):
- **The player object**: the game's own actor manager (`actor_manager.gActorManager`, the object its Lua getter
  pushes: the TOC word at 0x01028658 + 0x196C) asked for its player (CActorManager::getPlayer, 0x000A2248), checked by
  its class (vtable 0x00FE3D20). The player's state as the game keeps it [C]: life +0x5B8, magic +0x5BC, their
  maximums +0x5C0 and +0x5C4, invincibility +0x54D.
- **`life`, `magic`, `maxlife`, `maxmagic`, `heal`**: the game's own CPlayerActor::setHP (0x00101928), setMP
  (0x00101940), setMaxHP (0x001020B0), setMaxMP (0x00101FFC) -- unregistered bindings of the EBOOT, their
  implementations called on the player (the game clamps a value to its maximum, and a maximum to its own caps).
  **`money`**: Event:SetMoney (0-9999). **`player`**: all of it read back, and the game's GetMoney.
- **`invulnerable on|off`**: the game's own CPlayerActor::setInvincibility (0x00101F78). **The game clears that flag
  itself** [M: a run took a hit after its blink ended] -- so it is set again on the first frame it is found off, and a
  hit that still reaches the player's damage function does no damage while it is on (the game's knockback and blink
  stay).
- **`nodeath on|off`**: the owner's rule (2026-11-01) -- only a lethal hit is stopped: an entry observer on the
  player's damage function, CPlayerActor::damageHP (0x0011A94C; every one of its ten call sites in the EBOOT goes
  through it), lowers a hit that would end the hero's life so half a heart (one unit) remains; every other hit, the
  knockback, the enemies and the full-life sword beam stay as the game makes them; each one held is logged and
  counted. The observer is added to the lift's entry observers (config/r7_native_hooks.json: one function more; the
  player's build lifts with that file): a lift made again (build/r8/gen19 differs from gen18 in that one function's
  chunk). Developer mode off: the observer returns at once (devtools_entry is reached only while the tools are on).
  The older DOTHEROES_DEV_CANT_DIE (it heals below four apples) stays for the benchmarks that use it.
- **`damage N`** (tests): a hit through the game's own damage function.
- **The Player page**: life, magic (each a short list: full, half, one heart, the lowest; a heart or a point of magic
  more or fewer), money (0 to 9999), invulnerable and no death (on/off in place), heal fully.

**Tested** (offscreen, silent; the development build of the new lift, build/m11/dev):
- devtools_player **PASS**: life 4 read back by the game and on its HUD's own copy; the maximum raised to 12;
  money 1234 read back by the game's GetMoney; heal to the maximum; **a lethal hit (999) through the game's own damage
  function with no death on left half a heart**; then among the river's enemies: **invulnerable on, life stayed 10
  for 25 s** while the game cleared its own flag once (set again: a hit and its blink came and went); **then at the
  same spot, invulnerable off and no death on: life fell from 10 to half a heart and 12 lethal hits were held** --
  their hits land, the hero never died. (A first version with an unprotected control let the river's enemies end the
  hero in 25 s; the control now runs with no death on.)
- devtools_menu **PASS** with the Player page: Developer, Player, Money, 9999 chosen in the menu -- the game's money
  9999; the pictures reviewed (the Player page with its values, the money list).
- devtools_basic, devtools_off, devtools_warp **PASS** on the new lift.

## 9. M11.0d: progression, quests, items (2026-11-16) [C, M]

**Built** (port/devtools/devtools.cpp, dev_menu.cpp):
- **Flags**: `flag get N`, `flag set N V` (Event:GetEventFlag / SetEventFlag), `flags [A B] [all]` (every flag of a
  range that is not 0, or every named one too) -- with the names the game's scripts give flags (their `EventFlag`
  table, read through a chunk at run time).
- **The main story in 26 steps** (`progress`, `progress set K`): our own labels ("Temple 1: its guardian defeated"),
  each with the flags the game's scripts set at that point, the world map's progress flags (Event:SetProgressFlag)
  and the items it gives -- reconstructed from the scripts the game runs (the research notes, build/m11/agents);
  `progress set K` sets every step up to K and gives each step's items through the game's own item-get
  (Event:AddItem2) unless already held. A step is "done" when its flags hold.
- **Quests** (`quests`): the flags of 100 and above grouped by the script function that sets them -- read at run
  time in the scripts the game runs, in its own memory (the bundle in the EBOOT at 0x00A7C908); 187 functions.
- **Items**: `items [all]` (Event:GetItem for 1-159, with the game's own item identifiers: `item.Item.IL_*`), `give
  ID [N]` (Event:AddItem2, N times), `count ID N` and `remove ID` -- the item's own object in the player's data, found
  the way the game's GetItem finds it (0x00985C08 then 0x00985D88), its count set as CItem::setNum sets it (+0x54); an
  item never held is first given once (the game makes its object then).
- **Pages**: Story (the steps, Done marked; choosing one sets the story up to it), Flags (named flags and any set;
  choosing one turns it on or off), Quests (each function with how many of its flags are set; its flags, each turned
  on or off), Items (named or held items with their counts; Give one, Give ten, Set the count to 99, Remove).

**Tested** (offscreen; build/m11/dev):
- devtools_progress **PASS**: the flags with the scripts' names (flag 0 `scene_opening1` on the castle-front save);
  flag 300 set, read back and set back; the Boomerang given (count 1), 30 arrows counted, the Boomerang removed;
  187 quest functions; **temple 1's boss room entered: its event starts (the event state 8, a script waiting);
  after `progress set 9` (the guardian defeated) and the room entered again: no event -- the game's own scripts
  treat it as beaten** (flag 6 `scene_snake` 0, then 1).
- devtools_menu **PASS** with the new pages: Story, Flags and Quests opened, Items' first row (the Boomerang) given
  from the menu -- read back by the game; pictures reviewed.
- The whole suite (basic, off, warp, menu, player, progress) **PASS**.

**Not covered yet**: a set flag through the game's own save and load in the test saves (the flags shown to hold
across map changes, the place the scripts read them); choosing which sword or shield is equipped -- the items are
given through the game's own item-get and equipped in the game's own equipment screen; the sword natives
(ChangeSword, HandOverSwordAbility: the latter waits like an event) are left for later study.

## 10. M11.0e: bosses, enemies, events, cinematics (2026-11-16) [C, M]

**Built** (port/devtools/devtools.cpp, dev_menu.cpp):
- **Bosses, the game's way** (`bosses`, `boss ID`): each boss's arena and the flags its defeat sets (our own table and
  labels: the six temples' guardians, the battle at the castle, the tower's rematches and the last two); `boss`
  clears its flags and warps into its arena -- the arena's own entry event (the map's header) starts the fight as the
  game does. Answered on arrival. Creating a boss outside its arena (the scripts' Create* natives need the arena's
  resources, loaded by its map) is not offered.
- **Enemies**: `enemies` -- the game's actor manager's actors (getActor over its slots), each with its kind named by
  the game's own constants (`actor_manager.AT_*`: PLAYER, ENEMY, NPC, DOOR, ...), position, state, visibility;
  `respawn` -- the map entered again through the game's own map change (its own spawns again), answered once the map
  has reloaded: the guest's frame counter stands still while a map loads [M], so a warp to the map it is already in
  waits for that stall to end. An arbitrary enemy anywhere has no script native and is not offered (design 3.5).
- **Events and cinematics**: `events [FILTER]` -- the event functions, read at run time in the scripts the game runs
  (its own memory): every script function that yields (an event is a coroutine), 600-odd, "cinematic" where it plays
  a scene or moves the camera; `event NAME` -- the game's own StartEvent (0x004D5BC0) on it, refused while an event
  runs (the event state not 0) or for a name not in the list; `scene N` -- a scene of the map, as the game's events
  play one: an event function of the tools' own (`__devtools_scene`: Event:PlayScene, Event:WaitScene, yield) started
  the same way, so its waits run where they belong; `ending` -- the game's own Event:Ending (its ending and the credits
  movie, as M8's credits switch calls it).
- **Pages**: Bosses (Beaten marked; choosing one closes the menu and the fight starts), Enemies ("Enter this map
  again"; the enemies and other characters with their places), Events (the ending and its credits -- chosen twice --
  then the event functions by group, mainevent, mapevent, npcevent, ...; choosing one starts it).
- `status` also gives the game's own map state (Event:GetMapState: it is the map's number -- no loading signal).

**Tested** (offscreen; build/m11/dev):
- devtools_encounter **PASS**: the bosses listed (15); **temple 1's guardian started the game's way** -- its arena
  entered, its event running (a wait, 8), its lines answered with CROSS (state 2 waits for a button) until the event
  ended and the fight was on, **seven enemy actors** in the arena; **scene 1 played** inside the tools' own event
  (state 5, then done); **a sign's dialogue event started by name** (Signboard25) and answered until it ended; the
  map entered again (`respawn`: the frame counter stood still about 2 s, then the answer); an unknown event and boss
  refused. The raw map change followed: the map's number changes as the load begins and the frame counter stops
  until it ends.
- devtools_menu **PASS** with the Bosses page viewed; the whole suite (basic, off, warp, menu, player, progress,
  encounter) **PASS**.

**Not covered yet**: `ending` was not run in a test (it ends the session in the game's credits; M8's credits tests
cover the game's own Ending); a boss fought to its end with the tools (the fight starts and its enemies are there;
winning it is the game's).

## 11. M11.0f: coverage, diagnostics, test states (2026-11-16) [C, M]

**Built** (port/devtools/devtools.cpp, dev_menu.cpp; port/native/render/ppfx_render.cpp):
- **Pipeline coverage** (`coverage`): the renderer's own counters now -- the pipelines built in play that the
  preparation had made and had not, their time, the frames held building and the longest, the pipelines prepared for
  this GPU, the warm-up -- and the runtime's translations (in play, from disk, from memory)
  (ppfx_render_coverage; rsx_hlsl_to_spirv_cache_stats). Every pipeline built in play that the preparation had not
  made is also handed to the tools as it is built (g_ppfx_unforeseen_hook) with the map it was met in.
  **`coverage export [NAME]`**: this session's new pipelines with their maps and times (coverage.json) and the
  recipes the renderer recorded for them (DOTHEROES_RECIPES), into `<install>/data/devmode/exports/NAME` -- what M11's
  play-throughs feed the manifest with (the owner's request: keep collecting bosses, enemies, items, effects, events).
- **Performance** (`perf`): frame times measured at the game's own frame boundary (the flip), the time the in-game
  menu held the game left out -- frames per second, the mean, the 99th percentile and the longest of the last 600,
  the frames over 50 and 100 ms and the longest of the session -- and the process's working set and private memory;
  the frame limit and VSync as set.
- **The native runtime** (`runtime`): the native systems' reports (physics tasks, inflation, water, waver, flip
  waits) written to the game's log now rather than at its exit; `mark`: a hitch mark as F8 makes one (the log, the
  frame trace, the screen).
- **Test states in the game** (`states`, `state keep NAME`, `state restore NAME`, `saves copy-player`): the test saves
  kept or put back as named copies (`<install>/data/devmode/states`), and the player's saves copied into them (an
  installed game only; the player's tree only read) -- only ever on a save root that is developer mode's own or a
  test's run directory; a restored state is loaded from the title (CONTINUE). The worker's `devsaves` (M11.0a) does
  the same with the game closed.
- **Pages**: Coverage (its counters; "Export this session's new pipelines"), Diagnostics (frame times, memory, the
  limit; "Write the native systems' reports to the log", "Mark a hitch"), Test saves (keep a new state, copy my
  saves, restore one).

**Tested** (offscreen; build/m11/dev):
- devtools_diag **PASS**: coverage before and after a warp (a test runs without a preparation: 42, then 54 new
  pipelines, 60 then 70 translations); the export lists all 54 with their maps (those met in D_1_1_1 named); frame
  times (59.5 fps over the last 600 frames), memory; the five native reports written; a mark; a test state kept,
  listed and restored; an unknown state and `saves copy-player` (not an installed game) refused.
- devtools_menu **PASS** with the Coverage and Diagnostics pages viewed (pictures reviewed: 60.5 fps, mean 16.5 ms;
  the menu's own hold no longer counted as a frame -- a first version showed it as a 2 s "frame").
- The whole suite (basic, off, warp, menu, player, progress, encounter, diag) **PASS**.

## 12. M11.0g: integration (2026-11-16) [M]

**The requirements, met** (the owner's list, section 1):

| Requirement | Command(s) | Page | How, in the game's own terms | Tested by |
|---|---|---|---|---|
| Warp to any map, dungeon, floor or entry point | `warp`, `maps` | Warp | Event:SetMap; the maps and their arrival records read in the player's files | devtools_warp, devtools_menu, the map sweep |
| Boss and enemy spawning | `boss`, `respawn`, `enemies` | Bosses, Enemies | the arena's own entry event after its defeat flags are cleared; the map entered again | devtools_encounter |
| Progression and event state | `flag`, `flags`, `progress`, `status` | Story, Flags, Status | Event:Get/SetEventFlag, SetProgressFlag; the event object's state | devtools_progress |
| Quests and inventory | `quests`, `items`, `give`, `count`, `remove` | Quests, Items | flags grouped by the scripts that set them; Event:AddItem2, GetItem, the item's own object | devtools_progress, devtools_menu |
| Equipment and items | `give` (swords 95-118, shields 89-94) | Items | the game's own item-get; equipped in the game's own screen | devtools_progress |
| Health, magic, currency | `life`, `maxlife`, `magic`, `maxmagic`, `heal`, `money` | Player | CPlayerActor setHP/setMP/setMaxHP/setMaxMP; Event:SetMoney | devtools_player |
| Invulnerability and no death | `invulnerable`, `nodeath` | Player | setInvincibility, held; the damage function's entry observer | devtools_player |
| Events and cinematics | `events`, `event`, `scene`, `ending` | Events | StartEvent; a scene inside an event of the tools' own; Event:Ending | devtools_encounter |
| Shader and pipeline coverage | `coverage`, `coverage export` | Coverage | the renderer's own counters and each new pipeline as built | devtools_diag |
| Performance and runtime diagnostics | `perf`, `runtime`, `mark` | Diagnostics | frame times at the flip, memory, the native systems' reports | devtools_diag |
| Automated testing hooks | dev.cmd / dev.out, `lua` | -- | tests/devtools.py, drive.py `dev`/`devfail`/`devtry`/`devstate` | every scenario |
| Developer mode off by default; saves protected | the setting; `devsaves`, `state`, `saves` | Test saves | test saves of their own; the player's only read | devtools_off, the install checks, test_dev_saves |

**Integration tested** (the final build, build/m11/dev; offscreen and silent):
- tests/devtools_suite.py, all eight scenarios **PASS** (basic, off, warp, menu, player, progress, encounter, diag).
- The install checks **PASS** on the final build (a throwaway install, started as a player starts it): the game boots
  on its empty test saves, then plays from a copy of the player's saves; **the player's 12 save files byte for byte
  unchanged** after both.
- **The same install with developer mode off**: the game plays on the player's own saves as before; a command left
  for it is never read (no answer, no `dev.out`, no `[dev]` line); no `data/devmode` folder is made; the player's saves
  unchanged.
- The documents: README (developer mode, under Play), docs/HANDOFF.md (the debugging tools), this plan.

**Not verified here** (offscreen only, by the owner's rule): the "DEVELOPER MODE - TEST SAVES" tag and the menu over
a real frame (the pictures are composed and reviewed offscreen); the launcher's Settings showing the switch (the same
code path as every other switch; the worker's `settings --set` checked).

# M11: release readiness -- plan and checkpoint

M11 follows M9 and M9.2 (closed by the owner on 2026-11-16: docs/M9_LAUNCHER_PLAN.md, docs/M92_SHADER_PLAN.md) and
M10 (docs/M10_PLAN.md; its hands-on checks are the owner's, whenever convenient). Evidence marks as elsewhere: [C]
read in the code or the data, [M] measured, [I] inferred, [H] a hypothesis still to check.

## Checkpoint (kept current; the newest state first)

- **Stage** (2026-11-16): **M11.1a done** -- the owner's items before the broad validation (sections 13-20); M11.1b
  (the matrix's areas, docs/M11_VALIDATION.md) next. M11.0 accepted by the owner on 2026-11-16.
- **Found and fixed**: field 14's pipelines were the fields' rare metal slime (5% a spawn point), which exposed three
  rendering faults -- cube maps never drawn (B-002), texture kinds keyed for units no program reads (B-003), linear mip
  levels and cube faces read from the wrong bytes (B-004, runtime patch 0165); older test switches honoured with
  developer mode off (B-001); a unit test failing since M9.2g (B-005).
- **Validated** (offscreen): flags, story, money and items through the game's own save and load and a new process;
  temple 1's guardian from its introduction to its rewards and the sage's room; the ending from the last arena to the
  credits and the cleared title; every enemy type spawned the game's way; developer mode off meaning off in an
  install; the full map sweep (166/166, nothing unprepared) and the RPCS3 reference scenes (5/5) on the new build.
- **Not done**: the other 14 boss encounters, the enemies' damage, deaths, drops and sounds per type, the minigames,
  the side quests, the editor, death and respawn, audio, frame-rate independence -- the matrix's todo rows.
- **Outstanding decisions**: none blocking.

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
  (Event:AddItem2) unless already held, and the magic containers its event adds (Event:AddMaxMP: the yellow orb's 3,
  mainevent_007_yelloworb -- only when the step's first flag was not yet set; added 2026-11-17, section 30: without it
  a hero set to the tower had 5 of the story's 8). A step is "done" when its flags hold.
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

## 13. M11.1: the owner's request and the plan (2026-11-16)

The owner accepted M11.0 and started **M11.1 -- comprehensive Windows gameplay validation and bug fixes**, preparing
for a Windows beta: correctness and stability before features; the developer tools and automated scenarios used
wherever possible, not only the owner's own play; rendering compared with RPCS3 where a difference is suspected;
graphics pipeline libraries still off by default; every test offscreen; the owner's saves protected; every
licensing and distribution safeguard binding; M7 unauthorized, no public release.

**Before the broad validation** (the owner's list; M11.1a):
1. Field_14's unprepared pipelines: the content responsible, not just the recorded pipelines added; whether similar
   random or conditional content exists elsewhere (section 15).
2. The newly found shaders and pipelines into the preparation.
3. The 167 maps against the 166 reachable (section 16).
4. Progression and event flags changed with the tools survive the game's own save and load.
5. One boss from its introduction through defeat, rewards and progression.
6. The ending and the credits.
7. The developer command interface inert with developer mode off (section 14).
8. Arbitrary enemy spawning, through the game's own actor set-up where possible (every enemy type's rendering,
   physics and audio). Equipment chosen in the game's own screen stays.

**The validation** (M11.1b onward): a structured matrix, docs/M11_VALIDATION.md -- every dungeon, floor, field and
interior; every boss and enemy type; the main story; side quests and optional events; items, equipment, magic and
special abilities; character creation and editing; death, respawn and checkpoints; saving, loading and persistence;
cinematics, the ending and post-game content; audio, music, environmental effects and visual correctness; frame-rate
independence and performance; shader and pipeline coverage. Each row names its method (an automated scenario, a
sweep, the tools, a comparison with RPCS3, or the owner's hands -- the last only where nothing else can see it), its
evidence and its state. Every bug found gets an entry there: its severity (S1 crash, hang, lost progress or save
damage; S2 wrong behaviour or rendering a player meets; S3 minor; S4 cosmetic), how to reproduce it, its root cause,
its fix and the regression test that keeps it fixed.

**Stages**: M11.1a the pre-validation items; M11.1b the matrix, its runner and the first full pass of what the
existing scenarios and sweeps cover; M11.1c onward the matrix's areas in turn (maps and visuals, bosses and enemies,
story and quests, items and abilities, the editor, death and saves, cinematics, audio, performance), bugs fixed as
found, each stage validated and pushed.

## 14. M11.1a: developer mode off means off (2026-11-16) [C]

The developer tools themselves were already inert with developer mode off (M11.0a: nothing set up, dev.cmd never
read, no hook installed, the in-game menu without its Developer item; devtools_off and the install checks). An audit
of every developer switch found older test switches (R1-M8) a player's own build still honoured from the
environment, whatever the setting: **life.trigger and pos.trigger** (with DOTHEROES_DEV_STATE: the hero's life and
position set from a file in the game's folder), **DOTHEROES_DEV_CANT_DIE**, **DOTHEROES_DEV_CREDITS** (the ending on a
key or a timer), **DOTHEROES_DEV_WAVER** and **DOTHEROES_DEV_SOUND** (an effect and sounds started).

**Changed**: in an installed game (started as a player starts it) with developer mode off, those switches are off --
named once in the log (`[dev-switches] developer mode is off: ignored in an installed game: ...`); the read-only
ones a test follows stay (DOTHEROES_DEV_STATE's state and actors, the logs, hitch marks, the harness's stop). Outside
an installed game -- the repository's own test runs -- every switch works as before. And with developer mode on, the
saves' roots are always developer mode's own: a root inherited from the environment, which could be the player's,
is replaced (port/main.cpp; port/devtools/devtools.cpp `devtools_gate`).

**Test**: the install checks gain devtools_install_off -- the same install with its setting at 0, every one of those
switches set and a command, a life and a place left for it: none taken, the switches named as ignored, no
developer-mode folder, the player's saves unchanged. **PASS** (a throwaway install as a player starts it, offscreen):
dev.cmd, life.trigger and pos.trigger left where they were, no dev.out, no `[dev]` line, `[dev-switches] developer mode
is off: ignored in an installed game: DOTHEROES_DEV_CANT_DIE DOTHEROES_DEV_CREDITS DOTHEROES_DEV_WAVER`, the state a
test reads still written, no data/devmode, the player's 12 save files byte for byte unchanged; devtools_install and
devtools_install_play (developer mode on) **PASS** on the same build.

## 15. M11.1a: Field_14's pipelines -- a rare enemy, a renderer gap and a key too wide (2026-11-16) [M, C]

**What met them** (probes over the shipped preparation, the tools' warp, F:\3d dot game heroes\m11\evidence\
probe-f14, probe-f14b, probe-f14c): not the first visit after start-up as such -- in 76 arrivals on Field_14 the
16 (or the 8 of them a run had not met yet) came on 5, at any visit, always about 0.2 s after the arrival, and the
same loading screens and the same nearby enemies came on visits without them. A diagnostic of the renderer's own
(each frame's cube maps, which draws read them) and the frame described as it happened found the content: **a cube
map (32x32, 6 levels, in main memory) on texture unit 3, read by one model and left bound for the ~200 draws after
it**, for as long as the hero stayed on the map. The model: a small voxel creature whose vertex colours are the
script's (0.18, 0.18, 0.34) -- **the "HagureSlime"**, the fields' rare metal slime. Its script
(lua/Object/36HagureSlime) gives each of its spawn points a **5% chance** to show it when the map loads (10% in the
game's harder second mode); otherwise the object stays hidden. Field_14 has 18 such points, Field_13 10.

**Three things it showed**:
1. **A renderer gap (a real bug, S2)**: the native renderer had no source for a cube map, so every draw reading one
   was not drawn (`not rendered: asp: unit 3 (format A5 32x32 mips 6 dim 2 cube 1) has no source`, 691 draws in the
   sweep's visit). **The metal slime was invisible.** Fixed: a cube map is now a cube-compatible image of six layers
   with a cube view, each face's mip chain read from its place in guest memory (the six faces one after another,
   each a whole mip chain aligned to 128 bytes, as the runtime's layout code describes); port/native/render/
   ppfx_render.cpp `guest_texture_live`, `make_image`, `make_view`.
2. **A key too wide**: the pipeline key and the translation took each unit's texture kind (cube, volume) whether or
   not the program reads that unit -- the shadow and unnormalised kinds were already limited to the units read. So
   the cube map left on unit 3 made a second pipeline and a second translation of every later draw that never reads
   unit 3: **14 of the 16** (the ninth post-processing pass among them). Fixed: those kinds limited to the units the
   fragment program samples; the textures bound for a draw were already only those it samples.
3. **The cube map's own bytes read wrongly (a runtime bug)**: the slime's material (package record
   20020414:BF5A102F) binds a texture descriptor the engine keeps at a fixed address -- a 32x32 six-level linear
   ARGB cube whose image is **in the game's executable itself** (the engine's default cube map: six faces, each one
   coloured marker; found byte for byte in EBOOT.elf). Decoded, our faces after the first began inside the one
   before: the runtime's layout code packed a linear image's lower mip levels at their own width, where the RSX keeps
   the pitch register's row pitch on every level (as RPCS3 lays out a texture that is not swizzled) -- 8064 bytes a
   face here, not 5504. Every linear mipmapped texture's lower levels were read from the wrong bytes too. Fixed in
   the runtime's shared layout (both renderers use it): patches/ps3recomp/0165; its unit test
   tests/test_rsx_texture_layout.c.
4. **What is genuinely new**: the slime's own reflective material with the cube map (the model's pass, and an 8-bit
   pass seen once). No program on the disc declares a cube sampler (the inventory: 0), so neither the programs'
   headers nor the pass profiles can predict it: **the two recorded pipelines are added to the manifest**
   (config/pipeline_manifest.txt, 244 lines: a header of numbers and where the two programs are on the player's
   disc, as every line). **Prepared again** with this build (the throwaway install's worker, `prepare --rebuild`):
   191 recorded and 2,265 package pipelines, 3,488 translations, none failed, 5.8 s; **both slime pipelines among
   the prepared keys**. Over that preparation **every enemy type created (devtools_enemies, the slime and the dragon
   shown at once): 51 pipelines built, all 51 prepared, no translation in play**.

**Regression, on this build** (offscreen): **the full map sweep over the new preparation PASS** -- 166 of 166 maps
arrived, **no pipeline met unprepared, no translation compiled in play** (M9.2g's sweep: 202 of 218 prepared,
Field_14's 16 not); nothing drawn left out but the 7 known records of the first frame (evidence sweep-m111).
**The RPCS3 reference scenes: 5 of 5 within the accepted figures** (the first dungeon's lamp-lit door, the entry hall,
the corridor, the king's close-up, the editor's lines). The slime's look itself has no RPCS3 capture: the cube map's
contents are the game's own bytes decoded as every other texture, the faces now each one marker, as the image in the
executable holds them.

**Similar content elsewhere** (every enemy script read): two enemies are hidden at random -- **HagureSlime** (5%;
10% in the second mode) and **BlueDragon** (1%; 20% in the second mode), both on many fields' spawn lists -- and
about 29 others take a **stronger version** at random (their scripts' StrongRate: the same model and materials,
other speeds and colours). Conditional content beside: the spawn sets (A-H) the maps' records name, the second
mode (bosses' life, spawn rates), the tower's rematches (flag 310: bosses' life, damage, attacks). The matrix covers
each (docs/M11_VALIDATION.md).

## 16. M11.1a: 167 map files, 166 reachable (2026-11-16) [C, M]

The disc has **167 map packages** (map/pac_data/*.ps3.pac.gz, each with its alignment file) and the game's map list
(the scripts' MS_ constants, 210 entries with the capture slots and the editor) names **166** of them; the map change
(Event:SetMap) knows a map only by that list's names, and a name it does not know reloads the current map. The one
package outside the list is **Field_99**, and it is not unused: the executable's own table of maps pairs the
**character editor's world** ("Editor", MS_EDITOR) with Field_99's alignment, and every kept run of the editor loads
Field_99's alignment, collision and package (archived editor runs, R3-R8). So all 167 are reachable -- 166 by the map
change (the sweep), Field_99 through the title's character editor (the matrix's editor rows; the M9.2 preparation
recorded the editor). The sweep's list stays at 166 and says why.

## 17. M11.1a: what the tools change survives the game's own save and load (2026-11-16) [M]

**Test** (tests/scenarios/devtools_persist.drive, devtools_persist_load.drive; tests/devtools_suite.py
`devtools_persist`): from castle-front, the tools set event flags of three kinds (a counter to 9, another to 5, a
quest's to 1), the story to step 4 (its flags, the world map's progress, its items), the money to 1234 and the
arrows' count to 23; every flag 0-330, every item held, the money and the story's steps are read; the game's own
SAVE (the system menu); back to the title and CONTINUE -- the game reads its save again -- and everything read
again; then **a new process** continues from the saves that run wrote (kept as the local checkpoint
build/checkpoints/m111-persist) and reads them a third time.

**Result: PASS** -- the three reads identical: the 12 flags that are not 0 with the values set, the 6 items held
(arrows 23), money 1234, the story's steps 1-4 and 7 done (7, the ancient sword, was the save's own). The game's
save carries every change the tools make through its own functions; nothing the tools set lives outside it.

## 19. M11.1a: any enemy, anywhere -- through the game's own object set-up (2026-11-16) [C, M]

**How the game makes its objects** [C]: an enemy is an object running its type's script -- `<Name>Initialize`
sets it up (its type AT_ENEMY, its model's nodes, its life, its colours), `<Name>Update` moves it; the map's
alignment records create them on load. The scripts' own `Event:CreateNpc(dx, dy, dz, "<script>")` makes an object
beside the hero running the named script (the game uses it 114 times for its people), and the developers' own
test object (`Soldier`) made enemies by name with `Object:CreateObject`. Enemy models are not map-bound: every
type's nodes are there on any map.

**Built** (port/devtools/devtools.cpp, dev_menu.cpp):
- `spawnables`: the enemy types, read in the scripts the game runs (as the events are): each `<Name>Initialize`
  that makes an enemy -- 41 field and dungeon types (39 types, the temple-3 guardian's child, the spiked roller's
  dashing version) and 21 minigame versions; `rare` for the two that hide themselves at random, `minigame` for those.
- `spawn NAME [DX DZ]`: `Event:CreateNpc` with the type's script, three cells from the hero by default; a rare type
  is shown at once (the scripts' `math.random` answers 1 until its set-up has run, a few frames, then is given
  back); a minigame's version is refused outside its minigame (it expects its minigame's state, and a script error
  inside the engine's own calls silences every later script call).
- The in-game menu: Enemies, "Create an enemy here", the types.

**Tested** (tests/scenarios/devtools_enemies.drive; offscreen): on field 14 every field and dungeon type created
in turn, eight cells from the hero, two seconds to act, a picture, the map entered again every five types: **all
40 types of the first run created, each one an enemy of the game's own actor list near the hero, and drawn** (the
pictures reviewed: models, colours, effects -- the metal slime and the blue dragon among them, both shown at once);
the slime's script life (88) and the rabbit's (16) as their scripts set them. The run's pipelines exported
(`coverage export enemies`) for the preparation (section 15).

## 18. M11.1a: a boss from its introduction to its rewards and what follows (2026-11-16) [M]

**Test** (tests/scenarios/devtools_boss.drive; tests/devtools_suite.py `devtools_boss`): from the checkpoint at the
first temple's boss door (the hero with the dungeon's items, five hearts), `boss temple1` -- its defeat flags cleared,
its arena entered, the arena's own event introducing it -- its lines answered; the fight, automated: the drive's new
`devfight` walks the hero to the nearest of the game's own ENEMY actors (the tools' `enemies`: the snake's head and
its six segments, which the drive's older actor scan did not see) and strikes, until the game sets the defeat's flag
(6); the drive's new `devcollect` walks over what the game lists as items (the money the boss drops, the life vessel
its script places at grid 13, 10); the north passage, whose door script changes the map to the sage's room; the
sage's event paged until the game sets the orb's flag (7). No death (the tools') keeps the snake's contact damage
from ending the run; the game's own damage, knockback and the boss's attacks are untouched.

**Results** (offscreen; five runs while the scenario was built, the last the whole scenario: **PASS**): the guardian
**defeated by the automated fight every time** -- 45, 28, 54 and 60 swings, 6.5 to 17 minutes; no death absorbed
47-124 hits that would have ended the hero --
the game then set **flags 6 and 8** (the snake's scene and its tomb); its money dropped and the life vessel appeared
where the script puts it; **the vessel taken: maximum life 10 -> 12**; the door to the sage's room open; in the
sage's room the game's event ran, set **flag 7** (orb 1) and gave the first spell (**magic 1 -> 4**). The dropped
money does not stay: collected at once after the defeat (the first run's walk over it gained 110; the passing run's
`devcollect`, 13 -- most coins were out of the hero's reach while the boss's end played, and gone after it).
**Not covered**: the boss's own look and sound against RPCS3 (no capture of this fight exists; the matrix's boss rows);
the other 14 encounters (the matrix, section 2 of docs/M11_VALIDATION.md, with this scenario as their pattern).

## 20. M11.1a: the ending and the credits, the game's own way (2026-11-16) [C, M]

**The chain** [C: the scripts]: the last enemy's defeat runs its after-event (DevilAfterEvent: the music stopped,
mainevent_ending); that plays its scene, sets the ending's flag and moves to the tower's last room (D_7_2_9), whose
event moves to the ending field (Field_90), whose event moves to the castle's epilogue (Field_91, -gtf91): there the
game gives the hero back to the player -- the people gathered to talk -- and the player walks up the stairs and
through the castle's door into the throne room (D_20_9), whose own event ends with Event:Ending: the credits movie,
"Okay to save?", and the title of a cleared game.

**Test** (tests/scenarios/devtools_ending.drive; tests/devtools_suite.py `devtools_ending`): from castle-front, the
story set to the tower's rematches won (step 24), `boss last` (the last arena entered the game's way, its
introduction answered), then the after-event's own call (mainevent_ending, as DevilAfterEvent makes it; the boss's
defeat itself is the matrix's boss row); the chain paged with the drive's new `pressuntil` up to the epilogue; the
player's part walked (to the stairs, up through the door); the throne room's event paged to the credits; the credits
watched to their end; YES; the cleared title. Invulnerable and no death (the tools'), as the last enemy stays alive
in its arena until the ending moves the hero away.

**Result: PASS** (offscreen, 25 minutes): the maps in the game's order -- D_7_2_8, D_7_2_9, Field_90, Field_91,
D_20_9; up to the epilogue 110 s, the throne room to the credits 79 s; **the credits movie played to its last frame**
(7,860 of 7,860, none late, 1.97 ms a frame to decode); "Okay to save?" answered; **the cleared game's title movie**
opened after the save. The pictures reviewed (the throne room's crowd, the credits, the question, the cleared title).
Two of the scenario's first versions stopped where the game waits for the player (the epilogue's walk, which paging
alone never does): the scenario's, not the game's.

## 21. M11.1b: the rest of the matrix -- the owner's requirements (2026-11-17)

The owner accepted M11.1a and asked for the remaining matrix, prioritising gameplay correctness, crashes, softlocks,
rendering and audio defects and performance regressions; no new player-facing features or unneeded architectural
changes; the matrix and the bug list kept current; validated fixes committed and pushed as they come; tests
offscreen; the owner's saves protected; M7 unauthorized, no public release. Specifically:
- the rare metal slime and blue dragon **forced**, not left to chance; the **stronger variants** and the **second
  mode's** content validated;
- the slime against **RPCS3** if possible (an on-screen run: the owner's approval asked, 2026-11-17);
- **mipmapped and cube-map textures** in the regression tests (the fixes of B-002 and B-004 can touch other content);
- **every remaining boss, the final one fought for real**;
- minigames, side quests, equipment, magic, character editing, death, respawn and post-game content;
- **audio and movies** throughout;
- **frame-rate independence** at 30, 60 and higher frame rates in representative scenes;
- new shader and pipeline variants collected and prepared as they are met.

**How** (each through the game's own logic, the tools only to reach a place or a state): the rare enemies and the
stronger versions forced by the scripts' own `math.random` answering 1 for the moment of their set-up (the tools'
`spawn`, or a map's own spawns when it loads); the bosses as devtools_boss, with the swords the hero would have by
then equipped in the game's own equipment screen; the minigames entered by their own entries; every scenario over a
preparation with `coverage export`, its new pipelines into the manifest.

## 22. M11.1b: every boss fought to its defeat (2026-11-17) [M]

**How** (tests/scenarios/devtools_bossfight.drive, tests/devtools_bosses.py; offscreen, over the M11.1a preparation):
one scenario for every boss -- BOSS, STEP and FLAG from the environment: the story set up to the step before it (its
flags, the world map, its items, as the scripts give them); the hero armed as the story arms him by then -- the Hero
Sword (item 97) and the ancient sword's ability handed over with the game's own SetSwordData and HandOverSwordAbility
(the castle guard's event); the equipped sword read from the player object (+0x5D0, CPlayerActor::getSwordType's
field: 1 -> 3); the arena entered the game's way (`boss`); its introduction answered to its end (`devpage`: CROSS
until the game runs no event); the fight (`devfight`) until the game sets the defeat's flag; the drops collected; the
defeat's own event answered; the session's unprepared pipelines exported. No death (the tools'); the game's damage,
knockback and attacks untouched. `devfight` sets aside a target it gets no nearer to in 5 s (across water, in the air)
and holds its place swinging at whatever comes near.

**Results** (offscreen; each fight to the game's own defeat flag):

| Boss | Arena | Result | The fight |
|---|---|---|---|
| Temple 1's guardian (the snake) | D_1_2 | PASS | devtools_boss (section 18): the ancient sword, 28-60 swings |
| Temple 2's guardian (the bee) | D_2_2 | PASS | 29 swings, 44 s |
| Temple 3's guardian (the golem) | D_3_2 | PASS | |
| Temple 4's guardian (the kraken) | D_4_2 | see below | its arms and body across the water: fought from the platform they reach |
| Temple 5's guardian (the dragon) | D_5_2 | PASS | 79 swings, 86 s |
| Temple 6's guardian (the dark knight) | D_6_2 | PASS | 491 swings, 11 min |
| The castle battle | D_8_2 | PASS | scripted: the dark priest beats the hero down (WaitHpOrTime), the sages arrive, the game sets flag 64 and moves to the castle |
| The tower's rematches 1, 2, 3, 5, 6 | D_7_2_1..6 | PASS | stronger (the tower's flag 310): the snake's in 119 s |
| The tower's rematch 4 (the kraken) | D_7_2_4 | see below | |
| **The last enemy** | D_7_2_8 | **PASS** | 62 swings, 73 s; its defeat ran the game's own after-event: the tower's last room, then the ending field (the ending: section 20). Its flag (74) is set at the defeat and set back to 0 by the ending itself |

Two scenario faults were found and fixed on the way (not the game's): a fixed number of presses ran out where the
castle battle first beats the hero down (now `devpage`); the last enemy's flag cleared by the ending (now the fight's
own observation of it). **Pipelines**: the fights met two pipelines the preparation lacked -- one material group drawn
with the stencil test off on an 8-bit target (prepared only with it on), in four arenas and the last enemy's; both
added to the manifest (246 lines).

## 23. M11.1b: frame-rate independence across the game (2026-11-17) [M]

**What was known**: R8 measured the castle front (tests/fps_check.py: the hero's walk, the grass's debris, the static
clocks, the hero's own object) and the river's enemies at 30, 60, about 120 and 144 fps; the first temple's guardian
counted frames (head and segments: gated to 30 Hz steps, DOTHEROES_TICK30, docs/NATIVE_R8.md 9.6). The other bosses
and the other enemy types were never measured.

**How** (offscreen, silent, the dev build): the castle-front drive again at 30, 60, 144 and 240; and a new drive,
tests/scripts/fps_actors.drive, at the same four rates -- every enemy type outside the minigames created in turn on
field 14 (the tools' `spawn`: the game's own Event:CreateNpc; each type a segment of its own, 12 s), then the temples'
guardians and the last enemy, each entered the game's way and left to act for 40 s against a hero who stands at the
door and never swings (invulnerable; `devpage ... ifany` answers only a running event). The characters' positions
every 4 ms (DOTHEROES_DEV_ACTOR_LOG), the segments' times (their marks), the frames (DIAG_FRAMES).
tests/fps_actors.py compares each segment's characters with the 30 fps run: per class, the speed in 0.25 s windows
(distance over the time its steps cover) and the rate at which new characters appear (an enemy's shots); every
enemy is one class (0x00FE4500), so an enemy segment counts only the characters new in it -- the created type and what
it throws. A quantity that follows time keeps its ratio near 1; one that follows frames, the frame rate's ratio.
Its own unit test (tests/test_fps_actors.py) checks both on made-up runs.

**Results** (2026-11-17, the dev build, offscreen and silent; build/m11's fps_check_m111b2.txt, fps_actors_m111b.txt,
fps_bosses_m111b.txt):

- **The castle front** (fps_check at 30, 60 -- run twice, see below -- 144 and 240; each held its rate): the 1 s walk
  down and back up 5.00-5.14 units at every rate (the walk's start and stop, within 3%); the grass's debris stepped 60
  times a second at 60 and above (54.5 at 30) and settled the same; the words that follow frames are the frame
  counters R8 accounted for. The first 60 fps run read as broken -- no walk back up, little movement at the grass --
  but the game was not: mark.trigger was counted twice, 11 ms apart (a removal that failed while another process still
  held the file left it for the next poll), so every later segment of the analysis was shifted by one. Fixed in
  port/native/dev_state.cpp: a trigger file acts once a touch (mark, trace, pause -- a pause toggled twice --, memsnap,
  actors, watch); tests/scenarios/devtools_triggers.drive touches mark.trigger four times, twice held open 0.3 s
  (drive.py's `touchhold`): 59 marks before the fix, 4 after. Two reruns at 60: as the other rates.
- **Every enemy type** (fps_actors, 41 types on field 14, one segment each): no speed follows the frame rate. Each
  type moves at the same few speeds at every rate (the skeleton 1.00 at 30 and at 240, the spider 11.9-12.0, the
  squid 11.9); the tool's 15 flags are which gait or state the enemy chose in that run -- the metal slime fled (16
  u/s) at 60, 144 and 240 alike and walked (3) at 30; the cyclops charged (20) only at 60 -- and the rates of new
  characters (shots) wander up and down, not with the frame rate.
- **The bosses**: the first batch measured none -- the actor log's heap scan stepped 16 bytes and missed objects on
  other alignments (only the kraken's arms were logged). The log now reads the game's own actor list (CActorManager
  at 0x029B71A0: 1024 entries of 8 bytes, the actor at +4; CActorManager::getActor) -- every character, nothing
  else; DOTHEROES_DEV_ACTOR_SCAN=1 keeps the scan. Run again (build/m11's fps_bosses: the six guardians and the last
  enemy, 40 s each against a hero at the door): temple 1's guardian (its segments 4.22, its head 3.00 -- the 30 Hz
  gating of R8), the bee and its stings, the kraken and the dragon the same at every rate within 1-5 %; the golem's
  body chose its faster gait (3.2 for 1.6) at 60 only; the last enemy's pieces' fast moves ~37 % quicker at 60, 144
  and 240 alike than at 30 (no growth with the rate). **The dark knight (temple 6)**: two of his classes' fastest
  moves (14-18 u/s at 30 and 60, 24-30 at 144 and 240) and travel (+45 % at 144 and 240) higher at the higher rates,
  levelling off rather than following the frame rate -- the shape of a per-frame approach to a target. Repeated
  (build/m11 fps_knight: his arena alone, 60 s; 30 and 240 twice, 144, 60): his main part 3.55 and 3.44 units a
  second at 30, 4.60 at 60, 5.75 at 144, 5.87 and 5.79 at 240; his fastest steps 15-17 at 30, 28-29 at 144 and
  240; his two other parts the same way. A defect: **B-012** (docs/M11_VALIDATION.md), his updates to be read.

**B-012, the fix.** His three classes' updates (slot 8 of vtables 0x00FE52C0, 0x00FE5460, 0x00FE5390; the functions
read from the game's own memory through the tools' `peek`: 0x002C8300, 0x00264850, 0x0025EFD8): the two parts place
themselves each update at an offset from the object they follow (+0x6A0), using no time at all; the body's update
(0x002C8300) is the mover -- it scales its velocity by the frame's time, yet something in it (not traced further) still
moves more per frame than per second at higher rates. The remedy R8 proved on the first temple's guardian: the body's
update gated to 30 Hz (an entry hook in config/r8_hooks.json, `dotheroes_tick30_knight_body` in port/game_patches.cpp:
the shared 30 Hz clock, the update run only when a 1/30 s step is due and told that step), the parts left to follow
him every frame. A new lift (build/r8/gen20: gen19 and that hook; one chunk differs). Measured (build/m11
fps_knight, his arena alone, 60 s; DOTHEROES_TICK30_LOG at 240: 150 steps in every 5 s, 29.98 a second): his main
part 3.84 and 3.98 units a second at 30, 3.75 at 60, 3.72 and 3.53 at 240 (before: 3.5 at 30, 5.8 at 240); his
fastest steps 15-16 at every rate (before 28-29 at 240). At 30 fps the gate is off (the original pacing); above it he
moves in 30 Hz steps, as on the console, like the first guardian. tests/scripts/fps_knight.drive and fps_bosses.drive
are the measurement (tests/fps_actors.py compares the runs). On the gated build his fight and his tower rematch
are won as before, and the first guardian's too (tests/devtools_bosses.py temple6, tower6, temple1: PASS).

## 24. M11.1b: a draw's uploads that no longer fit its batch (B-006) (2026-11-17) [M]

**Finding**: the sweep's stricter check (sweep-m111, chunk 5) found one draw left out on arriving at
Field_50 (`asp: the batch is full`); a short run of seven maps did it again on its own. The native renderer records
into batches with a 16 MB upload buffer each (textures, uploaded vertices and indices, constants) and submits one once
a record leaves it past half (DOTHEROES_PPFX_SUBMIT_KB, a test switch, moves that point). A draw's uploads were
therefore guaranteed only the other half; on a map's first frame every texture is new, and one draw's textures and
vertices together did not fit -- it was left out for that frame.

**Fix** (port/native/render/ppfx_render.cpp, draw_record): a draw whose uploads no longer fit is given up before
anything of its own is recorded (the failure points come before its render pass, descriptor set and occlusion
query), the batch is submitted and the draw recorded again in the next one. What its first attempt already recorded
stays in the first batch and runs before the second: the clears before it (not recorded again), the textures and
vertex ranges it uploaded (found again in the caches), its semaphore waits (taken once) and their release (pushed
once). A draw that does not fit an empty batch still fails as before. The report counts them ("batches submitted
early").

**Verified** (build/m11's b006 runs, offscreen): the same seven maps with the usual batches and with batches carried
nearly full (15.8 of 16 MB before a submission): 1 and 9 draws recorded again, none left out, the pictures the same
(Field_50, Field_14, Field_91, D_8_2). The sweep's check no longer lists the reason as known: it fails if it comes
back.

**B-007** (S4, accepted): every case is the arrival at D_8_2, the castle battle's arena (the report's lines now carry their time on the compile log's clock: 0.2 s after the tools' `[warp]` arrival line). The arena's arrival runs an effect that alternates between two 1280x720 surfaces, 0:03190000 and 0:03550000 (not the display buffers, which are at 0x0 and 0x398000), each frame reading the one the frame before drew; the renderer draws them natively only for those ~60 frames. On the effect's first frame the surface it reads has never been drawn -- no native image (no transfer dropped one: the new `[xfer]` log is silent), and the recorder keeps only small textures' bytes -- so that one draw is left out. On the console it would read whatever that memory held. No change: the input is undefined on the console too, and it is one frame; the sweep's check keeps listing it.

## 25. M11.1b: the metal slime against RPCS3 (2026-11-17) [M]

The owner approved on-screen use (2026-11-17). The comparison ran in the scratch copy of RPCS3 (its own saves -- the
castle-front checkpoint, the same bytes -- and its own configuration; nothing of the owner's RPCS3 touched), boot
and grabs through tests/rpcs3_capture.py, the keys posted to its window, the desktop guard's approval for the session
(granted, then revoked).

**Getting a slime on screen in an unmodified game.** RPCS3 runs the game as it is: no tools. The slime's script
shows it at a spawn point 5% of the time, from the scripts' `math.random`; Lua's math.random draws from the C library's
`rand()`, linked into the executable (a lagged table generator: its constants 1664525 and 1013904223). A local RPCS3
game patch (the scratch copy's patches/imported_patch.yml, enabled in its config/patch_config.yml) makes `rand()`
answer 0 -- every `math.random(l, u)` then gives l, so every spawn point shows its slime (and every enemy with a
stronger version takes it), as the tools force it in ours -- and makes the hero's damage function return at once (the
strong enemies killed him in one contact). Where the slimes stand comes from the maps' alignment files: a 128x128
grid whose object records sit in their cells (a record's cell is its offset less the strings before it, over 32
bytes; x = (col + 0.5) * 1.6, z = -(row + 0.5) * 1.6), checked against the three slimes M11.1a's probes logged on
Field_14 and the hero's debug arrival there. The castle front's eastern neighbour, Field_19, has 26 slime points.

**What RPCS3 shows**: the hero walked from the castle front into Field_19 and to its slime points: the metal slime is a
**pale, pink-white dome with yellow glints** (the reflection of the engine's default cube map, whose faces are
coloured markers, over a dark body); its position confirmed by RPCS3's own memory (GDB: the camera target at
(71.6, -135.5) in area 18, Field_19, beside the slime points (74.4, -138.4) and (79.2, -136.8)) and by Field_19's spawn
lists (no other slime there). Ours draws the slime as a **flat dark-navy dome**, its vertex colour alone (M11.1a's
spawn pictures): **the reflection is missing -- B-008 (S2)**, fixed (section 26).

**Also seen there**: the blue dragon (the other rare enemy) shown by the same patch; a death on Field_19 and "Continue?
YES" bringing the hero back at the castle front with his life full -- the reference for our death and respawn check.

## 26. M11.1b: B-008 -- a cube-mapped program's textures unbound (2026-11-17) [M]

**Ruled out, in order** (build/m11's b008 runs, a frame described with DOTHEROES_ASP_DESCRIBE_FRAME and dumped):
- *the cube map's decoding*: a new diagnostic (DOTHEROES_CUBE_DUMP=<dir>: each face and level as uploaded) shows the
  engine's default cube right -- six pastel faces (blue, green, cyan, pink, magenta, yellow), the same at every level;
- *the reflection's strength*: the slime is drawn with the lit model program (its output: the lighting times the base
  texture, plus the cube sample times a constant); that constant, as the game patched it for the draw, is (1, 1, 1, 0);
- *the reflection vector*: a reflection of the view vector about the normal, rotated by three patched rows -- sound;
- *the view*: the cube image is a cube-compatible image of six layers with a cube view.

**The cause** (the Vulkan validation layer, PS3RECOMP_VK_VALIDATION=1): the runtime's translator declares a program's
textures either as one array of sixteen 2D images (`rsx_tex[16]`, binding 16) or, when a unit the program samples is
a cube map (or a volume), each unit on its own at binding 16 + u, so that unit can be a TextureCube. The native
renderer's descriptor set layout had only the array: a per-unit program's units 1-15 matched no binding at all
("descriptor set in layout must match the shader"; at the draw, "viewType must match the Dim operand") and read zero
-- the slime's cube map, and its normal and shadow maps too; only unit 0, the base texture, reached it.

**Fix** (port/native/render/ppfx_render.cpp): the layout holds both forms -- binding 16 the array of sixteen, bindings
17-31 one image each -- and every draw writes both (write_set): the array with a 2D placeholder wherever a unit is a
cube (an array program's units are all 2D; a per-unit program reads only the first element there), each unit at its
own binding. The pipelines' version is raised to 4: pipelines kept from the old layout are not reused (a preparation
makes them again). Not slime-specific: every program that samples a cube map goes through it -- the preparation's
3,488 translations hold two such, both the slime's.

**Verified**: devtools_cubemap (new, tests/devtools_suite.py): the slime created beside the hero and the field's spawn
points forced, under the validation layer: no error; the slime now a pale pink-white dome with yellow glints, as
RPCS3 draws it (build/compare/b008-before-after-rpcs3.png, local).

## 27. M11.1b: B-009 -- the game ending itself after the priest's game over (2026-11-17) [M]

**The owner's statement of the original** (2026-11-17): refusing the priest's offer starts the fight; accepting it
twice is an intentional game over; "Continue" treats it as a standard death -- the hero back outside the boss door or
at the tower's entrance, by checkpoint, his save's progress intact. Here the game ended (exit code 0) after
"Continue?", either answer; and on the tower's seventh floor the system menu's second entry did the same.

**Narrowing it down** (build/m11's b009 and priest runs): no system request to quit was delivered (the runtime's quit
path logs; none); neither of the engine's two flags that end main()'s loop (BiGlobal.isKeepRunning, isWantReboot:
both setters wrapped and withheld from the tools' Lua -- the class's table is in the Lua registry) nor Lua's os.exit
was the way. The runtime's PS3_HLE_ARGS now also walks the guest's stack (patches/ps3recomp/0166): exit(0) was called
from the game's own update (main -> 0x2AA88 -> 0x566600 -> 0x5154A0 -> 0x50B410), the event class's check in
GameClass.cpp at line 1762 -- "NextIndex:%d", then "NextIndex==MAP::MS_NON" and exit. NextIndex is a stored field
(GameClass + 0x4FF8, +0x74), set only by Event:SetDungeonResurrectionPoint, which each dungeon's first floor calls
from its own map event (D_7_1_1's header: mapevent_d07_01, MS_DUNGEON_7_1_1). The tools' warp had put the hero on the
seventh floor and in the priest's room without passing the tower's first floor, so the tower's resurrection point was
never set: the game, finding nowhere to bring him back to, ends itself -- as the original would in that state. A
player cannot reach the tower's rooms without that floor. The standard death checked the same way elsewhere
(field, temple 1, its boss room) brings him back as the console does.

**Regression** (tests/devtools_priest.py; scenario devtools_priest): the tower entered at its first floor (its own map
event), then the priest's room -- accept twice and continue (back at the tower's entrance; flags, items, money as
before the room); accept twice and decline; refuse (the fight begins); bag-door (BAG=door): a sleeping bag tried
outside his door first, then accept and continue.

**Results** (dev build, offscreen): accept-continue PASS -- GAME OVER, "Continue?" YES, the hero back at the tower's
entrance (area 63) with life 12/12, magic 5, money 45, the same 20 items and 73 story flags, the saves untouched, the
game running on; refuse PASS -- the priest and his summons in the room (nine visible enemies), the game running on.
The tower's save on its seventh floor, entered at its first floor (build/m11 tower_save2), saves, goes to the title
and continues: the save's exit was the same cause. Accept and decline: "Continue?" NO, "Save your progress?" NO, the
game's title (the hero no longer in play), the title's movie. The game then ended (its tiles unbound, its SPURS task
sets shut down, sys_process_exit(0)) -- at the tools' own read of the items: the exit's callers (PS3_HLE_ARGS) are
Event:GetItem's implementation (0x4CCE44, under the Lua VM), which with no player data ends the game. The tools'
`items` (and `give`, `count`, `remove`, `progress set`, which go through GetItem and AddItem2) now refuse with no hero
in play. That end found B-010: the process crashed while ending (below). On the new build, accept-decline PASS (the
title, the game running on, the saves untouched); a field death declined the same way: the title, then its demo.

**The checkpoint.** The tower has one resurrection point, its first floor (the scripts' only call for it,
mapevent_d07_01; the other dungeon with several is D_60's: mapevent_d60_02, 14, 15, 22). A sleeping bag ("Restores
LIFE and sets Revival Point") tried outside the priest's door is refused by the game ("It won't work here": bag-door
PASS, the bag kept, the continue at the entrance); the game's own save beside the seventh floor's door resumes at the
tower's entrance (tower_save2). A death in temple 1's boss arena returned to its entrance on RPCS3 as here (section
25's comparison). No script or binding the game has brings the hero back outside the priest's door.

**The save in RPCS3** (the owner's request: a late-game developer save in the scratch RPCS3): the game's own save
beside the seventh floor's door, made here after entering the tower at its first floor (tower_save2's slot), copied
into the scratch install (its own saves kept aside and put back after) and continued there: RPCS3 lists it ("Dark
Tower") and resumes it at the tower's entrance -- the same room, life, magic, money and circle-slot item as here.
The original's continue and ours put the hero at the same checkpoint. (Its decline and the priest's own game over
cannot be compared there: they need the tower's floors played to his room.)

**B-010** (found here; S1): the runtime's sys_process_exit called exit(), whose handlers and C++ destructors ran
while the native renderer (the crash: its thread allocating in guest_texture_live as the heap was torn down), the
movie's decoder and the game's other threads still ran -- an access violation in the heap, or the heap reported
corrupted (0xC0000374, the first tower save's end): 2 of the 10 game-ended exits in M11.1b's runs. A test's stop never
met it: it drains the renderer and ends the process without exit()'s handlers. Now every end the runtime makes goes
through a host hook (patches/ps3recomp/0167: ps3_exit and g_ps3_exit_hook -- sys_process_exit, the host quit's
fallback, the guest's entry returning), and the port's (main.cpp game_end) writes the window's place, the census and
the game log's summary, then runs the test stop's own sequence (dev_state.cpp dotheroes_end: the renderer drained and
stopped, the logs and reports) and ends the process with the game's status.

## 28. M11.1b: the rare and the stronger enemies on their own spawn points (2026-11-17) [M]

**Why**: the tools' `spawn` (section 18) makes any enemy beside the hero, but the rare ones (the metal slime, 5%; the
blue dragon, 1%) and the stronger versions a spawn point may pick appear in play only as a map's objects set
themselves up, by the scripts' own `math.random`. That path -- a spawn point choosing, the chosen enemy placed where
the map puts it -- had not run with a rare outcome.

**How** (tests/scenarios/devtools_forced.drive, devtools_suite.py's check): field 13 entered with the scripts'
`math.random` answering 1 while the map loads and its objects set themselves up (every rare spawn point shows its
enemy, every enemy with a stronger version takes it), the random given back once the map has arrived; then the map
entered again (`respawn`) with it answering its highest value (the ordinary versions; the rare ones hidden). Each
time the game's own actor list (twice), pictures and the pipelines met.

**Result** (devtools_forced PASS, offscreen, over M11.1b's preparation): forced, 9 enemies on the screen's spawn
points -- a blue dragon among them, at the map's own place for it, and the stronger versions of the others -- and
ordinary, 8 (no dragon, the ordinary versions); no pipeline met unprepared (43 built, all prepared), no translation in
play. The check: more enemies forced than ordinary, both lists read twice.

## 29. M11.1b: the krakens by the game's mechanics (2026-11-17) [M]

**The owner's criterion** (2026-11-17): both kraken fights won through normal play -- the walkthrough's upgraded sword
with piercing, or the Wire Rod to the platforms near its head -- checking the head's damage, the arms cut and grown
again, its movement and attacks, its defeat, rewards and what follows; a weakened kraken tests the defeat sequence
only.

**The arena** (temple 4, D_4_2; the tools' actor list and the kraken's script, B04Kraken.lua): the head is one actor
(the boss's first, slot 39), four arms of eleven segments each, four more roots hidden for its later phases; twelve
anchor piles (AT_ANCHOR_PILE: the posts the Wire Rod's anchor catches). Its script: life 660, six arms under 440,
eight under 220; an arm's life 3, grown again 40 s after it is cut; the head surfaces for about 10-12 s and dives
for about 6, between its ink volleys (3 shots) and its arms' swats and sweeps.

**Found, by the game's own mechanics** (the hero invulnerable, no death; the story's sword):
- the head takes the sword's hits: the six-arm phase (67 visible actors: its life under 440) within a minute while it
  was in reach; after each wound it sinks and surfaces elsewhere -- five places seen, (12.8, -20.8), (23.2, -11.2),
  (33.6, -24.0), (9.6, -16.0), (33.6, -8.0); unhurt it stays where it is;
- an arm cut (eleven segments gone) grows back 40 s later, as its script's armRevivalTime says, again and again;
- the Wire Rod (a shop's: given with the tools) fired at an anchor pile in line pulls the hero to it -- 4.8 and 6.4
  units across water, not 11.2;
- two of the head's places are out of reach on foot from where the hero arrives: the walkthrough's Wire Rod is the
  way there.
- What did not work, and why: the tools' first "piercing" sword (setBasePenetrateLevel in the swords' Init, then
  SetSwordData and HandOverSwordAbility) changed nothing -- the swords' Init functions do not run again once the save
  is loaded; a fixed spot and a fixed facing lose the head once it moves; walking one axis at a time does not cross
  the water or pass the posts.

**The fighter** (tests/drive.py): devfight's `aim` follows the boss's body (its first actor) -- within the sword's
reach (`aim@9`) it swings toward it, in its line or sweeping its quarter; `wire` fires the Wire Rod (`select R2
<icon>` puts it in the circle slot, its icon matched) at a pile whose platform is nearer; `map@` plans the fewest
walks and pulls to a cell in reach over the arena's map, which `devexplore` builds by walking every cell and firing
the rod at every pile in line from each. (The kraken's m_info.hp read through its Init object is its starting life,
660 or the rematch's 1320, not the current one: the arms' phases are the life's evidence.) When the boss is gone and
its flag not yet set, the fighter pages the after-event's words. A path goes only through cells from which the map
leads back to where it was begun (an unfinished map has platforms whose ways out were never tried: the first
rematch stranded the hero on one), and a step that fails -- an arm lying across the walkway -- is passed over for 20 s.

**The arena's map** (devexplore, 25 min, the kraken in it): 115 cells, 149 walks, 21 pulls -- the start's platform
and the bottom walkway, the far platforms reached only by the Wire Rod. The tower's rematch arena (D_7_2_4) has its
twelve piles exactly where temple 4's are: the same arena, the same map. The map and the rod's icon (`select`'s
reference) are taken from the game's own picture and layout: kept with the local drives (build/m11), not committed.

**Temple 4: won by the game's mechanics** (build/m11 kraken_map: the story's sword, the Wire Rod, the map): the head
followed across the arena -- 11 steps and 2 pulls to (33.6, -8.0), back on foot to (9.6, -16.0), and so on as it moved
after each wound; six arms at 49 s (life under 440), eight at 75 s (under 220), the head gone at about 90 s (80
swings); its after-event -- the fairy freed -- then flags 33 (scene_kraken) and 37 (scene_krakentomb); the hero's
life 10 -> 12, its heart. (That run's after-event was paged by hand at the pad file: the fighter then waited for the
flag without pressing; it pages now.)

**The tower's rematch: won by the game's mechanics** (build/m11 kraken_map_tower: the same, temple 4's map): its
stronger variant, as its script makes it once the tower's flag 316 is set -- life 1320 (six arms under 880, eight
under 440), faster to surface and to sink (2 s after an attack), contact damage 4, faster arms; the head followed
(17 steps and 2 pulls to (9.6, -16.0) and back), six arms at 34 s, eight at 116 s, defeated at 153 s (180 swings):
flag 317 set, the hero still in the arena (area 73), no heart (none dropped). Its second mode ("From mode": life
x1.5) is the same script's.

Both krakens are therefore winnable by normal play, the walkthrough's Wire Rod and the story's sword: the head's
damage, the arms cut and grown again, its moves and attacks, its defeat and what follows each as the game makes them.

## 30. M11.1b: the priest's second fight by the game's mechanics (2026-11-17) [M]

**The fight as the game makes it.** His script (B07Priest.lua) weights his summons toward blue slimes because the
point light is required; the walkthrough (GameFAQs, EgHeadFool, paraphrased): the Point Light spell lights the torch
in the room's centre and he can be hurt only while it burns; the real priest casts a shadow, his clones none; hit, he
and some clones warp; more clones as he weakens; about three lights for a player.

**What the runs found** (build/m11 priest_probe2-6, priest_light; local drives, offscreen):

1. **The hero's magic.** The tools' `progress set` gave a hero at the tower 5 magic containers; the story gives 8:
   the yellow orb's event adds 3 (Event:AddMaxMP(3), mainevent_007_yelloworb) and each of five spell events one
   (item 55). With 5 the spell (cost 3) was cast once and the room was dark for 30 s only. Fixed in the tools: a
   step's own magic containers (port/devtools, `Step::max_magic`), added only while the step's first flag is unset
   (section 9). devtools_progress checks `progress set 10` twice: +3, then nothing more.
2. **His life.** `m_info.hp` read through his Init object is the life he is given at the start and is never lowered
   (as the kraken's, section 29): the earlier "no wound" judgements came from it. His Init object is actor 29, a
   hidden controller at (0, 0); the priests drawn are actors 25-28, of which 25 (the map's own, group ID 11) is the
   real one: its own life, the u32 at its actor's +0x5BC, went 1200 -> 944 in one light while 26's and 29's stayed
   1200. New: the tools' `peek ADDR [N]` (developer mode; guest words in 0x10000-0x2FFFFFFF) and drive.py's `peekobj`
   (a script object's native address from its userdata's payload, then its words); devtools_basic checks it.
3. **The fight** (priest_light: the tower entered at its first floor, his offer refused; the story's sword, the hero
   invulnerable as for the krakens, magic 8): devfight `aim@9 cast@<the spell's icon>,28 collect farm@3 cycle@120
   life@0x5BC` -- the spell cast every 28 s while the magic holds 3; below it, the light gone, his summons fought and
   their apples picked up; the body's own life read every 5 s (`cycle`: a body without a wound for 120 s passed over
   -- not needed, 25 stayed the real one). **Won in 361 s**, 369 swings: 64 a hit, five lights, his summons' apples
   restoring the magic twice. His defeat ran the game's own after-event: the cutscene, mainevent_darktower3, the move
   to D_7_2_8 (area 77) and the next cinematic.
4. **Not verified:** the real priest's shadow against the original (outside the torch's light the floor is black in
   our frames; an RPCS3 comparison needs the fight reached there). The priests lean as they glide (also unlit: the
   game's motion, not the light). One frame of a probe that paused the enemies (priest_probe4, frozen shot 7 of 8)
   showed the scene entirely black under the light; none of about 60 lit frames of two fights did -- watched.
5. Earlier conclusions corrected: without the light he cannot be hurt (priest_aim's 25 minutes: the design, not a
   defect); the first lit attempts failed for the magic and were judged by his starting life.

B-011: no hang in these runs (the hang was 13 minutes into an unlit fight; the winning fight lasted 6).

## 31. M11.2d: QUIT GAME in the title menu (2026-11-17) [M]

**What the owner asked**: a way to quit from the title menu, in the game's own style, on the normal title and on a
cleared game's title; a Yes/No question defaulting to No, answered with a keyboard, a mouse or a controller; the
game ended in order (B-010); "Return to Launcher" respected when the launcher started the game, a clean exit in direct
play (docs/CLI.md); saves and settings untouched.

**How.** The PS3's INSTALL item (a copy of the disc to the hard disk) was already gone from both menus (M10.4); its
place, the menu's last item, becomes QUIT GAME:

1. **The label** is the game's own menu text, set from the port: the title's Lua tables (`Title.Menu[n].MenuString`
   and the cleared title's `ClearMenu[n]`, full-width text as the other items) get QUIT GAME and its line of help, the
   way the game draws every other item (port/devtools/devtools.cpp `title_tick`, every 30 flips while a title is up;
   it runs in every build, not only in developer mode).
2. **The choice**: an entry hook on the title menu's input (0x005CE018, config/r8_hooks.json; port/game_patches.cpp
   `dotheroes_title_menu_input`) runs the game's own input and, when it chose the last item (action 5 in both menus),
   withdraws the choice -- the action, the title's step and its timer put back, 0 returned: the menu stays, its sound
   played -- and asks the question. Without the step and the timer restored, a cancelled question left the title
   counting toward its attract demo.
3. **The question** is the in-game menu's (port/overlay.cpp, its third kind of question): QUIT GAME?, "Quit the game?"
   (or "Quit and go back to the launcher?" when the launcher started the game), Cancel and Quit Game, **Cancel
   focused**; drawn over the title itself (not over the paused menu's panel, which the window's own exit question
   keeps). The PS3's message dialog is not drawn by this runtime (cellMsgDialog answers by itself), so the in-game
   menu's question -- the same pixel font and colours -- is the one the PC build has. Its input is the menu's: the
   controller (A/B, the stick and the pad), the keyboard (arrows, Enter, Escape) and the mouse.
4. **Its memory** (B-014, found by the release qualification): setting the label is one call into the game's Lua, which
   needs a little guest memory (a stack and the chunk's text). It first used the developer tools' region (384 KB), so
   with developer mode off the tools' memory was taken in every player's game -- the installed developer-mode-off
   check failed on its "[dev]" line. With developer mode off the label now has one 64 KB page of its own, logged under
   "title"; the tools' region is made only with developer mode on (port/devtools/devtools.cpp `ensure_region`).
5. **Quit** ends the game through the in-game menu's own ending (`end_session`: B-010's orderly end, status 0): with
   the launcher it says "launcher" (the launcher comes back, as the menu's Return to Launcher), in direct play
   (`CubicHeroes --play`, or the game started without the launcher) "exit". Nothing is saved or changed: the title has
   no unsaved progress.

**Tests.** tests/title_quit.py (tests/scripts/title_quit.drive; offscreen): on the normal title (a save) and on a
cleared game's title (six items): down to QUIT GAME, chosen -- the question, Cancel (the default) taken -- the title
back, the cursor still on QUIT GAME; chosen again, Quit -- the game's own end, status 0, no crash report: **both
PASS** (gen21). On screen (2026-11-17, the owner away, the guard's approval): the question over both titles as
described, the focus on Cancel, then on Quit Game; the game ended with status 0.

## 32. M11.2a-c, e: the portable installation and the public command line (2026-11-17) [M]

**The owner's M11.2** (the Decomp Launcher's feedback): CubicHeroes.exe the one program a player or another launcher
starts; everything the application manages inside one folder, portable by default; a command line other programs can
drive, documented with exit codes; tested on throwaway installations. The interface is docs/CLI.md; the Linux side
docs/LINUX_PACKAGING.md.

**The root** (launcher/main.cpp `resolve_root`): `--install-dir`, then `CUBIC_INSTALL_DIR` (a relative path in either
from the current folder), then `cubicheroes-root.txt` beside CubicHeroes (a relative path from its folder; written by
`--remember` and the window's "Choose another install folder"), then CubicHeroes's own folder (an AppImage's: a
`Cubic.Heroes` folder beside the `.AppImage`). The launcher's preferences (`config/launcher.json`), the jobs' logs
(`logs/`), the game's crash and hang reports (`logs/crash`), the worker's temporary files (`data/tmp`), the compiler's
caches (`data/cache/zig-*`) and the update's staging (`downloads/`) are all inside it. Outside it only: the
shortcuts and the Steam entry the player asks for, the GPU driver's own caches, and the launcher's removal helper (it
must run from outside the folder it removes). The earlier launcher's `%APPDATA%\CubicHeroes` is only read: its
choices, and an offer to copy its installation in (find-legacy, migrate-root -- the earlier folder left exactly as it
was). A root that can't be used is reported at once (the window; exit code 7), and `--status` of a folder that isn't
there creates nothing.

**The command line** (launcher/cli.h): `--play` (the game directly, the window never opened; CubicHeroes waits and
exits with the game's status; the game is in a job that ends with CubicHeroes), `--install`, `--update`,
`--check-update`, `--prepare-shaders`, `--rebuild-shaders`, `--status`, `--migrate-from`, `--set KEY=VALUE` (through
the settings schema), the session options `--fullscreen/--windowed/--borderless`, `--width/--height`, `--fps`,
`--vsync` (for that session only unless `--persist`), `--json` (one object, or events then a result), `--no-gui`;
exit codes 0-7. Keys are named by path only, never read into a log or an argument.

**The worker's side** (5d9cad9): the launcher's update replaces only the package's items in the folder (never the
install's), `remove-launcher` keeps the installation whose root is its folder (`--everything`, separately confirmed,
removes it), `migrate-root` and `find-legacy`, temporary files and the compiler's caches inside the root; 152 unit
tests pass.

**Tests** (tests/cli_integration.py: copies of a built launcher in work folders whose names hold spaces, an accent and
Japanese; the development installation copied in as the disposable one):

| Case | Result |
|---|---|
| --version, --help | PASS |
| the default root (the launcher's folder) from another working folder | PASS |
| CUBIC_INSTALL_DIR; a folder that isn't there is not created by --status | PASS |
| cubicheroes-root.txt with a relative path | PASS |
| a relative --install-dir (from the working folder) | PASS |
| a root that can't be used: --set exits 7, --status reports it | PASS |
| nothing installed: --play exits 3, --install without --disc 2 | PASS |
| the installation in the launcher's folder (portable) | PASS |
| --set kept in config/dotheroes.cfg; a value the schema refuses changes nothing | PASS |
| the folder moved with its launcher: still installed, its game inside the new place | PASS |
| --migrate-from into a second root: the copy installed, its 16 save files identical, the source unchanged file by file | PASS |
| --migrate-from refused into a root that holds an installation (nothing changed) | PASS |
| two installations side by side: a setting of one leaves the other | PASS |
| --remember keeps a root; remembering the launcher's own folder removes the pointer | PASS |
| --play (offscreen; the exit question answered through the in-game menu's test input): `--fps 120 --width 1024 --height 600 --windowed` in effect (the game log's frame limit), not kept (config/dotheroes.cfg and window.json unchanged); the started and ended events; the game's own end, status 0 | PASS |
| --play passes on the game's exit status (the game ended from outside with 42: CubicHeroes exits 42) | PASS |
| the game ends with CubicHeroes (CubicHeroes ended from outside while the game runs: the game gone at once -- its job) | PASS |

**The earlier installation's offer** (the launcher's window, a folder holding no installation; the packaged launcher
on this PC, whose earlier launcher's preferences name an installation on F:): EARLIER INSTALLATION FOUND, the folder
named, Copy It Here focused, Not Now on B; Not Now kept in `config/launcher.json` and the offer not made again at the
next start (the scripted controller test, `--nav-test` with `CUBIC_NAV_LEGACY_OFFER=1`; its pictures read back by
the launcher). Copy It Here runs the worker's migrate-root, the command `--migrate-from` runs (tested above). Two
small faults found on the way and fixed: `--version` wrote a worker log into the folder (now nothing; the version
case checks it), and the scripted test put its pictures at the drive's root when its script was named by a relative
path (now beside the script).

The first run of the `--play` cases failed: the game ended at once with status 2, "No game data". Not the command
line (it passed the game's status on, as it should), not a player's build: the test installation carried a
developer's build of the game, which -- unlike the players' builds (tools/zig_build.py `windows_resources`) -- had no
manifest, so it opened its files through the ANSI code page, which cannot hold the test folder's Japanese name. The
developers' build now links the same resources when they are there (port/CMakeLists.txt `DOTHEROES_WIN_RES`); the
cases then passed (17 of 17), and the job case now also requires the game to be running when CubicHeroes is ended
(it had passed vacuously). The release qualification's folder holds Japanese too, for the players' build itself.

## 33. M11.1: the window's place after B-010, and the hang watchdog during the exit question (2026-11-17) [M]

The owner's item: the installed game's window place checked again after the shutdown changes (B-010).
tests/window_place_check.py (on screen, the development installation): the game started windowed, its window moved
and resized (Win32), closed as a player closes it, config/window.json read, the game started again.

1. **First run: FAIL** -- the game did not end within 60 s of the window's close. Not a defect: closing the window
   opens the in-game menu's exit question (M10.2), and a second close at the question ends the game; the test closed
   once. The test now closes as a player does (twice).
2. **A defect found by it (B-013)**: while the question was up, the game's hang watchdog wrote a hang report (one in each start)
   ("no guest flip for 10 s"). The question holds the game (its clocks stopped: no flips), which the watchdog took
   for a hang -- with DIAG_HANG_EXIT=1 (the tests) it would have ended the game. The watchdog now counts no time while
   the host holds the game (port/diag/diag.cpp: `ps3_guest_paused`). The window test fails on any crash or hang
   report written during it.
3. **Again: PASS** -- the second close ended the game with status 0; window.json written; the second start's window
   at the same place and size (180, 140, 1100x640); no hang report.

## 34. M11.2f: the rendering resolution (2026-11-18) [M]

The owner's requirement before the beta is finalised: a Rendering Resolution option in the launcher's settings and the
in-game menu -- native (the default), 1280x720, 1920x1080, 2560x1440, 3840x2160 or custom -- independent of the display
mode and the window's size, above or below the display's, scaled to the output with its aspect kept; the display's own
mode never changed; no gameplay timing affected; a CLI `--resolution`; vendor-neutral, Linux- and ray-tracing-ready.

**Design** (port/native/render/ppfx_render.cpp, "M11.2f"; the runtime's backend, patch 0168):

- **What is scaled.** The game draws its frame at its display buffers' size (1280x720). The rendering resolution draws
  it at a scale of that: every local-memory render target is made at the scale (its size in the guest's pixels kept
  beside it), except the post-effects' reduced copies of the frame -- the targets smaller than the frame and of its
  shape (640x360 down to 20x12): their taps sit at the game's texel offsets and their output is blurred, so they stay at
  the game's size (DOTHEROES_RES_SCALE_CHAIN=1 scales them too, for comparison) -- and tiny ones. Main-memory targets
  (the CPU reads them: the luminance probe) never scale. The shadow map and the water's reflection targets do (sharper
  shadows and reflections). The HUD, menus and text are the game's own draws into the frame: they scale with it.
- **The game's numbers stay in its units.** Its viewport transform and texel offsets are untouched; the renderer
  scales what is in pixels: the Vulkan viewport, the scissor and clear rectangles (the guest's rectangle, then the
  target's scale), the depth bias's slope factor (the depth's slope per pixel is 1/scale of the guest's), occlusion
  counts (divided by the scale squared: the game compares them with its own thresholds), and WPOS (the fragment
  translator divides the host position by the scale passed in fp_alpha.w -- only programs that read WPOS change text,
  so every prepared pipeline and shader cache stays valid; none of the 87 fragment programs in the M9.2 recipe set read it).
- **What goes back to the game is at its size.** A transfer's write-back into guest memory and the snapshot encoder's
  read of a display buffer blit the scaled rectangle to a scratch image of the guest's size first.
- **The present.** The presented frame (the backend's frame image) is made at the display buffer's scaled size, so the
  frame is copied once (as before) and scaled to the window by the present's blit with its aspect kept (letterboxed).
  The backend re-makes its frame image and readback buffer under the present's lock once the present in flight has
  completed (rsx_vk_bridge_present_prepare_sized).
- **Changes take effect between two frames** (rescale_check after each present): every scaled target made again, its
  picture carried over (a linear blit), depth targets made again empty (the frame clears them); with native the
  output's size is followed after it has settled for 250 ms (a window being resized). The check costs nothing in a
  frame where nothing changed.
- **Limits and memory.** The scale is kept within the device's maxImageDimension2D, framebuffer and viewport limits
  for the largest scaled target, and the scaled targets within half the device-local heap; a target that cannot be
  allocated at its scale is made at the game's size and the resolution falls back to it (said in the report and log).
  Range 0.25x to 8x.
- **Native** is the output's own size: the display's in fullscreen, the window's drawable in a window (1:1 pixels);
  offscreen (no window) the game's size. The game is per-monitor DPI aware (port/main.cpp; the launcher keeps its own
  way) so that on a display scaled above 100% native is the display's real pixels, not Windows' virtual screen.
- **Settings**: DOTHEROES_RENDER_RES (native, the presets, custom; live) and DOTHEROES_RENDER_RES_CUSTOM (a new
  schema type, `size`: WxH within 320x180..7680x4320); the launcher's Display settings (custom: a list of common sizes),
  the in-game menu's Display & Graphics page, `--resolution` (session only unless `--persist`), and the developer
  tools' `resolution` (the live path, for tests).

**Tested** (offscreen, the development build, build/m11 res_*.drive; the owner's display: 1920x1080 at 100%):

- **Pictures at each preset** (the title, the castle front, the equipment screen, a temple room; SHOT_FULL):
  1280x720, 1920x1080, 2560x1440, 3840x2160 and a custom 960x540 -- the same composition at each: the HUD, menus
  and text in the same places and sharper above 720p; the depth of field (the water and trees behind the hero
  blurred), the hero's shadow, the water's highlights alike at every scale; 960x540 softer, nothing missing.
- **Changed live** (`dev resolution`, the in-game menu's own path): 720p -> 1440p -> 540p -> 2160p -> 720p -> 1440p
  on the castle front and into a temple: each change took effect between two frames (9 targets made again, their
  pictures carried over), the game ran on without a stall or a stop. The in-game menu's row (overlay test input:
  Display & Graphics, Rendering resolution, right twice): native -> 1280x720 -> 1920x1080, the frame followed.
- **Frame pacing** (tests/frame_times.py over a 27 s window, the castle front standing and walking, no frame dumps):
  at the 60 fps limit 1280x720, 1920x1080, 2560x1440 and 3840x2160 all 60.0 fps, p99 18.5-19.4 ms, none over 25 ms;
  unlimited 180.6 fps at 1280x720 and 178.1 fps at 3840x2160 (p99 7.7 / 7.5 ms) -- the GPU's share is small, the game
  is bound by its own work. Video memory for the images: 73.5 MB (720p), 122.5 (1080p), 191.4 (1440p), 371.1 (2160p).
- **Shader cache**: 44 pipelines on the castle front at every resolution (54 with the temple), none made because of a
  resolution; the 7 unsupported draws are the same at every resolution (from before this change).
- **The safe fallback**: with a 600 MB device-local heap pretended (DOTHEROES_RES_TEST_HEAP_MB, tests only)
  7680x4320 asked for live became 3789x2131 ("lowered to fit the video memory"), then 1920x1080 and native (offscreen:
  the game's size) as asked.
- **Settings and command line**: tests/test_settings_diagnostics.py (the presets, a custom size normalised to WxH,
  sizes and words refused); tests/cli_integration.py gains `--resolution` (wrong sizes are exit 2; a session's
  2560x1440 in effect and not kept) -- run with the release qualification.
- Not tested here: a display scaled above 100% (the DPI awareness), exclusive fullscreen at native on a 1440p/4K
  display, and the owner's on-screen look; the launcher's row was compiled, not driven.

## 35. M11.2g: the button prompts throughout the game (2026-11-18) [M]

The owner's requirement (completing M10's prompts): every prompt the game shows in the player's style -- automatic
(the device used last, the default), Xbox, PlayStation, Nintendo Switch, Steam, generic controller, keyboard and
mouse -- with the player's own bindings, Nintendo's labels, hot-plugging, PlayStation keeping the game's own art,
Kenney's prompts (the owner's assets, CC0) for the others, and no modified game texture distributed.

**The audit** (build/m11 prompts, local: the player's files read, nothing kept in the repository). The English build
shows its buttons four ways:

1. **Glyphs of its fonts inside text** -- Font_eng (the menus) and Font_b/Font_c (the messages: FONT A and B), 512x2048
   DXT3, 32x32 cells: Circle, Cross, Triangle, Square, small L1/R1/SELECT/START (cells 248-255) and wide R1, L1,
   SELECT, START, R2, L2 (two cells each, 647-681). 18 strings use them (29 glyphs): the guards' and the maid's
   lessons, the yellow orb, the minigames' tutorials and starts. Full-width X is never the Cross button in English.
2. **Button art in 37 UI textures** (DXT1/3/5; the dungeons' map window the same picture in 7 packages): the HUD's key guide and windows, pause, options, the item, equipment
   and orb menus, the action balloons (CHECK/TALK/OPEN), the title's character choice and difficulty, the character
   editor, name entry, the albums, the shops and the blacksmith, the minigames, the maps, the camera.
3. **Three prompts as words**: the title's "PRESS START BUTTON", "the left stick" and "the directional buttons" in the
   castle guard's lesson.
4. **The attract demo's video** (PlayDemoMovieUS.bik, the title's DEMO PLAY): a recording of play, the HUD's
   buttons in its picture (the key guide in two layouts, the slots' Cross and Circle, the item cycle's L2/R2).

The executable inserts no button text of its own; the shops, editor, title, options and save screens have no glyphs in
text (their prompts are atlas art beside action words).

**Design** (port/prompts.cpp, port/prompts_table.cpp; the renderer's guest_texture_live; runtime patch 0169):

- **The textures, at upload.** port/prompts_table.cpp lists each atlas (its package and name) and each button's cell
  (our annotations of the audit) and the fonts' glyph cells. At start the player's own files are read once (40
  textures) and each one's fingerprint kept (its size and format, its first level's first 16 KB and last 4 KB). The
  renderer asks for a DXT texture of a listed size whether it is one of them (only when its content changed); a match
  is uploaded as RGBA with each cell drawn again: the old art cleared to what surrounds it (the bar's panel, a
  balloon's grey, or transparency), the new glyph fitted to the old art's box (aspect kept, area-averaged). A font's
  glyphs take the size its own Circle has (one cell) and its wide R1 (two characters: the icon in the first one's cell, centred in the room the text keeps, so no join shows; the
  second cell left empty), so text keeps its layout. Mip levels are made from the redrawn first level. No game texture is
  written anywhere: everything happens in memory on the player's machine.
- **The style and the bindings.** The style is the player's choice (LAUNCHER_PROMPTS, shared with the launcher; now
  also "keyboard") or, automatic, the device used last (cellPad's: a keyboard key, or a controller's family by SDL's
  type -- PlayStation, Switch, Xbox, the Steam Controller or Deck by name, else generic -- before any input, the first
  controller connected). Each of the game's buttons is drawn as the physical button bound to it: the pad's binding
  (DOTHEROES_PAD_MAP) or the key (DOTHEROES_KEY_MAP). Changing the style or a binding never changes a binding. The
  style and the bindings make one signature; a texture drawn for another is made again at its next use (no work in
  a frame where nothing changed).
- **Nintendo's layout.** SDL reports a Switch pad's face buttons by label (now stated in cellPad: the hint set
  explicitly): the button labelled A presses Cross. Every prompt names them by label -- Cross A, Circle B, Square X,
  Triangle Y -- including the in-game menu's binding names and the save screen, which had them by position (Cross
  "B": wrong, fixed here).
- **PlayStation** keeps the game's own pictures: with the default bindings no texture is touched; with a button
  remapped, the game's own art of the physical button (taken from the player's files at start) is drawn into the cell.
- **The other styles** draw Kenney's input prompts (the owner's set, CC0; 163 of its 64-pixel pictures embedded,
  port/assets/ui/prompts) **as drawn, on a transparent background** -- the owner's direction after a first version
  filled each icon's shape as a dark button: the icon alone, nothing behind it. Xbox's face buttons in its colours;
  Nintendo's by label; Steam's; generic: the face buttons by their place (a diamond with one lit), neutral L1/R1,
  START and SELECT icons, LS/RS; the sticks as lettered stick tops. Keyboard: the key's cap with its name (Kenney's,
  else drawn with the UI font, its label clear); the direction pad and the left stick as the direction keys; the
  right stick has no key and keeps a stick's picture.
- **The attract demo's video**: where its HUD shows the buttons (our measurement of the recording: 11 places in
  1280x720, build/m11 pr_demo_locate.py), each decoded frame is checked (3 px around each place) against the game's
  own art of that button from the player's files (normalised correlation of the luma: the video's own scale and
  offset), and where it is found the old button's own pixels are filled from what surrounds them and the icon drawn
  over, in the frame's own planes (BT.601) -- nothing where the HUD is not shown (a cut, a fade).
- **The words** are rewritten in the game's own Lua (port/prompts.cpp lua_chunk, run from the flip hook every 30
  flips -- every 5 until installed -- with developer mode off too, as QUIT GAME's label): a hook on the Title table's
  assignment (the title loads its script each time it opens) sets "PRESS START BUTTON" -- PlayStation: as written;
  else "PRESS" and the START glyph, which the font then draws in the style -- and a wrapper on the engine's
  Event:Print (its metatable's __index answers Print with it) changes, for the keyboard, "the left stick" and "the
  directional buttons" to "the arrow keys" (or "the direction keys" when they are rebound).
- **The host screens**: the in-game menu's button names per style (Steam and generic added, Switch by label, a
  "Keyboard and mouse" choice), the save screen's confirm/back pictures for Steam and generic (Kenney's) and Switch
  by label; the launcher's own pixel glyphs already covered every style and switched with the device -- it gains the
  keyboard choice, and a manual choice now holds whatever is used.

**Tested** (offscreen, the development build; tests/scripts/prompts.drive checked by tests/prompts_check.py; build/m11
pr_switch.drive, pr_demo.drive, the self-test):

- **Every style on representative screens** (the title, the HUD's key guide and slots, the equipment screen, the
  guards' and the maid's lessons with button glyphs in text, the second answer's words): PlayStation, Xbox, Nintendo
  Switch, Steam, generic, keyboard -- all pass: the 40 prompt textures recognised from the files, both word hooks in,
  textures redrawn only in the style asked for. PlayStation: no texture touched, and the equipment bar and the HUD's
  key guide pixel for pixel as before this change; "PRESS START BUTTON" and the lessons as written.
- **Every cell without visiting its screen** (DOTHEROES_PROMPT_SELFTEST: the 40 textures redrawn from the files in
  each style, 135 cells reviewed beside the originals): shops, blacksmith, editor, name entry, albums, minigames,
  maps, balloons, camera, the three fonts -- each cell replaced, nothing of the old art left, neighbours untouched
  (Font_c's "?" placeholders for the wide SELECT, START, L2 and R2 now carry the glyphs too).
- **Automatic switching and remapping** (pr_switch.drive: the pad's Cross and Square swapped, Cross on Space; the
  input's device reported as each kind in turn, as a real device's input sets it): keyboard (SPACE for SELECT), Xbox
  (X for SELECT, A for EQUIP), PlayStation (the game's own Cross and Square pictures swapped), Switch, Steam, generic,
  keyboard again -- each change on the open equipment screen at once. With no input yet the prompts follow the
  connected controller (this PC's Xbox 360 controller: Xbox). The style changed never changed a binding.
- **The words**: keyboard -- "use the arrow keys for a Spin Slash"; the title "PRESS" and the START glyph in every
  non-PlayStation style (Menu, +, the Steam start icon, Enter).
- **Text glyphs**: the wide R1 in "Hold R1 to guard" -- first cut where the text advances; the owner saw "RB" read
  as "R3" (the cut at 22 columns, the real advance 20: measured, the game's own R1 39 pixels wide on screen); then a
  hairline at 3840x2160 (the text draws each character's dark outline before it, one character after the other: the
  second half's outline crossed the light icon) -- now the whole icon in the first character's cell, centred in the
  room the text keeps: clean at 1280x720 and 3840x2160.
- **The attract demo**: every HUD button in the sampled frames redrawn (key guide in both layouts, slots, L2/R2), no
  box left around them (the old button's pixels filled from their surroundings; a few pixels of a slot frame's edge
  can spread into a tab's fill).
- **Rendering resolutions**: Xbox at 3840x2160 -- the icons scale with the frame like the game's own art.
- **Cost**: a texture is redrawn once when the game loads it or the style changes -- a 512x2048 font 2.5-3 ms, an
  atlas 0.2-0.6 ms; nothing in a frame otherwise. Frame pacing with the Xbox prompts drawn: 1920x1080 at the 60
  limit 60.0 fps, p99 18.4 ms (one frame of 25.4 ms in 27 s; 23.4 ms at most without prompts in M11.2f's run);
  unlimited 1280x720 245.8 fps, p99 5.6 ms. The demo's check: 11 places x 49 positions x ~60 samples a frame.
- Not tested here (owner): real controllers of each kind plugged and unplugged (the families come from SDL's types;
  this PC has an Xbox 360 controller), a display scaled above 100%, the launcher's own prompts (compiled; its glyphs
  are its own, unchanged but for the keyboard choice).

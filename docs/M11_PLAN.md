# M11: release readiness -- plan and checkpoint

M11 follows M9 and M9.2 (closed by the owner on 2026-11-16: docs/M9_LAUNCHER_PLAN.md, docs/M92_SHADER_PLAN.md) and
M10 (docs/M10_PLAN.md; its hands-on checks are the owner's, whenever convenient). Evidence marks as elsewhere: [C]
read in the code or the data, [M] measured, [I] inferred, [H] a hypothesis still to check.

## Checkpoint (kept current; the newest state first)

- **Stage** (2026-11-16): M11.0 (developer debugging tools) -- the design and its stages written (this document);
  M11.0a next.
- **Done**: the map sweep made a regression test (tests/map_sweep.py `check`; section 5): the full sweep passes, 166
  maps arrived in their own areas, 0 pipelines unprepared, 0 translations in play; the check found that M9.2g's sweep
  never reached fields 1-9 (named by their files, not the engine's names) -- fixed, swept, all their pipelines were
  already prepared; docs/M92_SHADER_PLAN.md corrected.
- **Next**: M11.0a (section 4).
- **Outstanding decisions**: none blocking; the defaults "decided here" in section 3 can be changed at review.

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
  line `@ID COMMAND` names its answer), answers appended to `dev.out` as JSON lines; and the map sweep
  (DOTHEROES_MAP_SWEEP becomes a list of `warp` commands).
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
- The arrival: the build logs, before each change and after the last map, `[sweep] ... arrived N/M MAP area A frames
  F` (port/native/render/ppfx_recorder.cpp map_sweep: the field area's number and the guest's frame counter, the
  addresses dev_state.cpp reads).
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
area but Field_80 (in D_24_1's, 108, consistently); the baseline (build/m11/sweep_baseline.tsv) holds the 166.

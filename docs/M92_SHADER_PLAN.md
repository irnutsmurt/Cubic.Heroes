# M9.2 revisited: comprehensive shader and pipeline preparation

The owner's request (2026-11-16): before M11, eliminate shader-compilation stutter as thoroughly as technically
possible, including during a player's first playthrough. The 98 pipelines the first-session manifest prepares
(docs/M9_LAUNCHER_PLAN.md section 15) come from a handful of recorded routes and are not enough for the finished
release. Stages M9.2a-M9.2f, each validated, committed and pushed; M11 starts once this is done or its remaining
limits are established. Labels as in the other records: [C] read in the code, [M] measured, [H] hypothesis.

## Checkpoint

- **Stage**: **M9.2 done** (2026-11-16); its limits established (section 2g); ready for M11.
- **Done**: the investigation (section 2); the test harness keeps every automated run off the desktop, enforced
  centrally (section 7); **M9.2a**: the inventory in the game (section 2a) -- 636 vertex and 2,481 fragment records
  read, none unparsed; every program of 332 recorded pipelines found; layout and control word derived exactly;
  **M9.2b/c** (section 2b): one pipeline identity with the ignored state normalised (98 recorded -> 76); every
  program on the disc translated before play and pipelines from the package's records by pass profiles;
  **M9.2d** (2d): every pipeline built in play logged with its frame, prepared or not; the warm-up; the start-up
  stall removed; pipeline libraries measured (2f); **M9.2e** (2e): the launcher's one comprehensive run, its
  check, stop and resume for real; **M9.2f** (2f): the held-out comparison, the manifest and profiles learned again
  from every recording, a second visit building nothing -- as it ships, 2,202 pipelines and 2,716 translations in
  8.4 s cold, no translation compiled in play on any measured run, pipeline coverage 98.8-100% on the routes
  measured since, 96-99% measured on areas no recording had reached.
- **Next**: M11 (validation), with M9.2's limits (2g) carried: later areas recorded as the drives reach them;
  AMD and Intel measured.
- **Open questions for the owner**: none (pipeline libraries not adopted, section 2f; the owner may ask for them).

## 1. What decides a pipeline (the renderer, port/native/render/ppfx_render.cpp) [C]

A draw's pipeline is keyed by:
- **its two programs**: the vertex program's bytes; the fragment program's *structure* (its instructions, not
  the inline constants the game patches into it as it draws: those are buffered, `rsx_fp_structural_hash`);
- **the translation's parameters**: the vertex layout (which inputs), the fragment control word (register count,
  H0 output, depth replace, kill, texcoord control), per texture unit the kind (cube, volume, shadow compare,
  unnormalised, gamma, sign expansion), sRGB output, the alpha test and its function;
- **the render state**: colour and depth formats (the pass's targets), the vertex stride and count, topology
  (triangles or lines), blend, depth, stencil, cull, winding, colour mask, depth clamp.

## 2. The investigation (2026-11-16) [M, C]

**The recorded set** (config/pipeline_manifest.txt: 98 pipelines from four routes; build/m92/*.py, local):
- 53 program pairs, 57 distinct translations; the other 41 pipelines differ only in render state;
- **22 of the 98 differ only in state the GPU ignores**: a stencil function with the stencil test off, blend
  factors with blending off, a depth function with no depth test, a cull face with culling off. Normalising that
  state before the key (same pixels, by Vulkan's rules) leaves **76**. Making depth, stencil, cull and winding
  dynamic state (Vulkan 1.3 core) would leave 67; blend, colour mask and depth clamp too (VK_EXT_extended_dynamic_
  state3) 64. The state is not where the count comes from: the translations and targets are;
- of the translation's parameters, only the alpha test (21 of 98) and a shadow-compare sampler on unit 4 (22)
  varied; cube, volume, gamma, sign expansion, unnormalised, sRGB and the clip planes were never used;
- the vertex layout equals the inputs the vertex program's own header declares (96 of 98; the other 2 a subset),
  and the fragment control word is stored in the fragment program's header: both derivable before play.

**The programs on the disc** (the player's own; counted, never copied):
- `data/shaders/shaders.ps3.pkg` ("spkg", version 4): three tables after a 64-byte header -- 567 vertex records,
  567 fragment records with the same (group, id) at the same index, then a sorted 8-byte (group, id) index. Each
  record is the game's own compact container ("CGB": a 32-byte header -- microcode size, profile, the vertex
  program's input mask, the fragment program's control word and register count -- then the microcode, then its
  parameters). 292 distinct vertex and 358 distinct fragment records by bytes;
- **the engine draws a package record's vertex and fragment program together**: every one of the 58 recorded
  package pipelines uses the two programs of one record (counting byte-identical duplicates). The 567 records are
  the game's program pairs -- statically known;
- **the group field names the pass**: scene materials (group 0x2..., 0x3...) drew to the HDR target with depth
  (formats 97/129); groups 0x000x04xx to a 16-bit colour target with stencil (4/129); group 0xf holds the generic
  sprite and interface shaders (several targets and blend modes); [H] to be confirmed on more areas;
- `shaders_0.ps3.pkg`: 2 records (the same layout); `SasShader.ps3.spk` and `SasShader2.ps3.spk`: size-prefixed
  CGB records (31 and 2);
- **the EBOOT**: 35 CGB records and **1,882 standard Cg binary programs** (CgBinaryProgram: profile, sizes, a
  parameter table with names, types and resources, the microcode) packed at unaligned offsets -- 23 vertex and
  1,859 fragment programs (1,738 distinct), nearly all the post-processing library's (PPFX: tone mapping, bloom,
  depth of field, ... in every combination of its options; ~1,667 named variants). The game uses a few dozen;
- every program the recorded sessions handed the renderer is among these (section 15 of the M9 plan: 98 of 98
  pipelines' programs on the disc; here: the 77 EBOOT ones are 55 Cg programs and 22 CGB records).

**What is static and what is not**:
- static: every program (above), the package's pairs, the vertex layout and fragment control word, the sampler
  types (Cg parameters);
- runtime-modified, without consequence: fragment programs' inline constants (patched by the game; buffered by
  the renderer, so one translation serves every constant variant);
- runtime-only: which post-processing programs are paired, and per draw the alpha test, the shadow sampler, the
  texture kinds, the pass's targets and the material's render state. These are predicted (section 3), not read;
- no program was found to be generated at run time.

**The GPU features** (the test PC: an RTX 3070, driver 616.92): VK_EXT_graphics_pipeline_library with fast
linking and independent interpolation, VK_EXT_extended_dynamic_state3 (blend, colour mask, depth clamp). The
other vendors: Intel's Arc Windows driver lists graphics pipeline libraries (2025); AMD's Windows driver lists
extended dynamic state 3 (since 23.3.2); AMD's Windows support of pipeline libraries not established. So these
can only be accelerators over a path that works without them.

## 2a. M9.2a: the inventory, in the game (2026-11-16) [M, C]

`dotheroes --shader-inventory [--disc USRDIR] [--recipes FILE ...]` (port/native/render/shader_discovery.cpp; the
install's own disc copy by default; no device, no window): every container parsed with its header checked, each
program put in the form the renderer is handed (vertex: little-endian words; fragment: as stored) and identified
with the renderer's own functions (`rsx_vp_analyze_inputs`, `rsx_fp_structural_hash`, `rsx_fp_texture_units`,
`rsx_fp_collect_constants`). JSON: counts, distinct programs, constant-only variants; with recipes, every recorded
program looked up. On the test install's disc (build/m92/inventory_check.sh; F:\3d dot game heroes\m92\evidence):

| Source | Vertex records (distinct) | Fragment records (distinct structures) | Unparsed |
|---|---|---|---|
| shaders.ps3.pkg | 567 (292) | 567 (357) | 0 |
| shaders_0.ps3.pkg | 2 (2) | 2 (1) | 0 |
| SasShader.ps3.spk | 30 (30) | 30 (8) | 0 |
| SasShader2.ps3.spk | 1 (1) | 1 (1) | 0 |
| EBOOT, CGB records | 13 (12) | 22 (19) | 0 |
| EBOOT, Cg binary programs | 23 (23) | 1,859 (1,738) | 0 |
| **all** | **636 (356)** | **2,481 (2,118)** | **0** |

- **Duplicates and constant-only variants**: 636 vertex records are 356 programs; 2,481 fragment records are 2,118
  structures, and on the disc no two fragment programs differ in constants alone (2,118 distinct byte strings for
  2,118 structures). Constant-only variants exist only at run time -- the game patches a program's constants as it
  draws -- and the renderer's structural identity already makes one translation serve them all;
- **569 package records are 562 distinct program pairs**;
- **every vertex program's inputs are known exactly** by the renderer's analysis (636 of 636), and agree with what
  its header declares (636 of 636);
- **no cube or volume sampler** is declared by any Cg program's parameters; the package's CGB records carry no
  sampler types (per-draw texture kinds stay predicted).

**Checked against everything recorded** (332 pipelines in five recipes files: the four M9.5 routes, the river
fights, the install tests):
- **both programs of all 332 found** (36 of 36 vertex programs, 49 of 49 fragment structures): 189 from one package
  record, the rest the EBOOT's (102 Cg, 41 CGB, some mixed);
- **the vertex layout derived from the program equals the recorded one in 332 of 332** (both renderer paths take
  it from the program's analysis alone);
- **the fragment control word derived from the header equals the recorded one in 332 of 332**: the package's and
  CGB records' as stored; the Cg programs' from registerCount (4 more when the output is from H0, as the game sets
  it in all 102 recorded), outputFromH0, depthReplace, pixelKill. The translator reads only its H0 and depth bits.

So the programs, their pairing in the package, the vertex layout and the fragment control word are static; what
remains per draw is the pass's targets and render state, the alpha test, a shadow sampler, and which
post-processing programs are paired -- section 3.

## 2b. M9.2b/c: every shader translated, pipelines from structure (2026-11-16) [M, C]

**One identity for a pipeline** (port/native/render/ppfx_render.cpp):
- `canon()`: the render state the GPU ignores zeroed before the key and before `vkCreateGraphicsPipelines` (a
  disabled stencil test's functions and operations, a disabled blend's factors, a disabled depth test's function
  and write, a disabled cull's face, a missing target's state). The recorded 98 pipelines are **76** pipelines;
- `key_ppfx` / `key_asp`: the one key function the draws use and a recipe's key is made by (`recipe_key`), so a
  pipeline prepared from a recipe -- recorded, the manifest's, or made from the disc -- is the one a draw looks up.
  Checked at every new recipe in play: the key made again from what is kept must equal the draw's (five reference
  runs, ~100 new recipes: no difference).

**`dotheroes --prepare --static PROFILES [--disc USRDIR] [--threads N]`** (the install's disc copy by default):
- the recipes (recorded, the manifest's); with the disc and the profiles, a recipe for each package record and
  each profile its group has (else its group's class): its programs from the disc, its vertex layout from the
  program's analysis, its fragment control word from its header, a shadow sampler where the profile has one and
  the program samples that unit, its render state from the profile;
- **every program on the disc translated on its own** (vertex with its layout, fragment with its control word),
  besides each recipe's two -- the SPIR-V the game will look up for each, made now;
- translations and pipelines on all but one of the CPU's threads (the SPIR-V translator's lock narrowed to its
  cache: patches/ps3recomp/0161), the pipeline cache saved as it grows; `prepared-keys.bin` beside it: the keys of
  every pipeline made (what play is measured against);
- what did not translate or make is listed (what, which, why), never skipped silently.

**The pass profiles** (tools/pipeline_profiles.py `learn`; config/pipeline_profiles.txt, 70 lines of numbers):
learned from the manifest's own recordings (the four M9.5 routes and the river fights: 98 pipelines, 58 of one
package record): 37 profiles in 17 groups, 30 in 5 classes. Of the package's 569 records, 60 have their group's
profiles, 402 their class's, 107 neither (classes no recording reached). tests/test_pipeline_profiles.py (4).

**Measured** (build/m92/prep_test.py: the test install, our shader folder and NVIDIA's own cache empty per run; RTX
3070, 16 threads; F:\3d dot game heroes\m92\evidence\prep1):

| Preparation | Pipelines | Translations | One thread | 15 threads | Our cache |
|---|---|---|---|---|---|
| the first-session manifest (as M9.5) | 76 | 88 | 1.3 s | 0.3 s | 2.9 MB |
| comprehensive | 76 + 1,785 from the package | 2,726 (every program on the disc, and the variants) | 25.9 s | 7.3 s | 94 MB of SPIR-V, 19 MB of pipelines |

- nothing failed: 2,726 of 2,726 translated, 1,861 of 1,861 pipelines made;
- on 15 threads the 2,726 SPIR-V files are the same, byte for byte, as on one;
- the reference scenes against RPCS3 (offscreen), 5 of 5 within the accepted figures with the normalised state
  and the new keys.

**Shader coverage of the disc**: every program found (636 vertex records, 2,481 fragment) translated before play
with the parameters its own header gives. A draw whose texture kinds, alpha test or sRGB output differ from those
is a different translation; the variants the profiles name are translated with their pipelines. **Pipeline
coverage** of play is section 2c's measurement.

## 2d. M9.2d: the runtime -- what play builds, logged; the warm-up (2026-11-16) [M, C]

**Every pipeline built in play accounted for** (ppfx_render.cpp `compile_account`, `frame_builds_end`):
- `prepared-keys.bin` (written by the preparation beside its pipeline cache) is read with the cache when the cache
  is valid for this GPU and driver; each pipeline built in play is **prepared** (taken from the prepared cache:
  ~2-3 ms on the test PC) or **not prepared** (compiled now, recorded in the recipes for the next preparation);
- the session log (port/diag/game_log.h): each one not prepared at Info with its time, each prepared one at Debug;
  at each frame's end, a frame that built pipelines with its whole time and the building's (Info when one was not
  prepared or the building took 4 ms or more); at the session's end a summary -- built in play, prepared or not,
  their times, the frames that held building and the longest -- in the log and the renderer's report;
- the compile log (`VK_COMPILE_LOG=1`, the tests') marks each line `prepared` or `unforeseen`.
- Nothing is skipped to avoid a stall: a pipeline not prepared is still compiled when first drawn, as before.

**The warm-up** (`warm_start`, `warm_take`): in the game, when the pipeline cache is valid here, the pipelines the
last preparation made (`prepared-recipes.bin`: the player's own programs, kept in their shader folder) are created
again from that cache on two lowest-priority threads during start-up and the title; a draw whose pipeline the
warm-up has made takes it instead of building it. The same pipelines and keys: nothing is drawn differently, and
one not yet warmed is built as before. `DOTHEROES_WARM=0` turns it off; `DOTHEROES_WARM_THREADS` (2). Since M9.2f
the warm-up also takes the pipelines met in play before that the preparation had not made (section 2f).
- Measured (the river, the comprehensive set, F:\3d dot game heroes\m92\evidence\heldout): 1,850 pipelines made in
  6.4 s, the draws took 51 of them, **one** pipeline built in play (the first, before the warm-up reached it: 2.9
  ms); without the warm-up the same route built 55 (72.5 ms in all, all prepared). The cost: **+177 MB** of the
  process's private memory (1,388 MB against 1,210 MB), the warm-up's two threads busy for those 6 s at start-up.

## 2e. M9.2e: the launcher (2026-11-16) [C]

- **The worker** (tools/worker.py `op_prepare`): with config/pipeline_profiles.txt shipped and the install's disc
  copy present, one comprehensive run (`--prepare --static`) -- the recipes, the disc's programs, the package's
  records -- whose progress is the game's own ("N of M shaders and pipelines", the launcher's bar and time left as
  before); its figures kept beside the cache (`prepare-result.json`). Stopped part-way, it keeps what it made: the
  translations as they are written, the pipeline cache every 10 s; the next run goes quickly over those. Without the
  profiles or the disc copy, the recorded recipes in parts, as before.
- **The check** adds whether the comprehensive preparation can run and has run into this cache, and its figures.
- **The launcher**: shaders are ready when the cache is valid, the first-session list is in it, and the
  comprehensive preparation has run into it; the completion notice says "N shaders and M pipelines ready for this
  GPU and driver"; the Shaders page adds where the pipelines come from (the package's records, recorded in play).
  **Rebuild** deletes the shader folder and makes the whole set again. Play and preparation never overlap (as
  before: Play pauses a preparation, a preparation waits for the game).
- **The renderer's version** (kPipeVersion 2): a pipeline cache made before the normalised state is "made by another
  version of the renderer", so an updated install prepares again (automatically, by the update setting).
- **Diagnostics**: the export's about.json carries the last preparation's figures (no programs, no shader text).
- **Stop and resume, for real** (build/m92/cancel_resume.py: the worker as the launcher drives it, "cancel" on its
  input, the test install, NVIDIA's cache empty first; the comprehensive set as it ships): a Rebuild stopped at 61%
  ("3005 of 4918 shaders and pipelines", 4.4 s, the time left shown as 2.6 s) ends "cancelled" with the lock
  released; preparing again resumed over the 2,716 translations kept (none translated again) and made the 2,202
  pipelines in 6.0 s (the pipeline cache had not reached its first 10 s save); once more, 1.1 s; the check then:
  prepared, the comprehensive preparation done, its figures, 98.7 MB on disk.

## 2f. M9.2f: validation (2026-11-16) [M]

**The held-out comparison** (build/m92/heldout.py, heldout_report.py, unforeseen.py; F:\3d dot game heroes\m92\
evidence\heldout): the same build, the test install as a player starts it, offscreen, no sound device; for each
run our shader folder a copy of the preparation's, NVIDIA's own cache an empty folder, an empty recipes file. Two
preparations, both cold: the first-session set as it shipped (the manifest: 76 pipelines, 0.6 s) with no warm-up,
and the comprehensive set (1,850 pipelines, 7.6 s on 15 threads) with its warm-up. The profiles and the manifest were
learned from four routes only (the river, the fields, the first dungeon, the menus); eight areas were never recorded
before they were measured. Frames are the guest's flip requests over the drive's own route segments (the same
stretch of play in both), the start-up excluded; "build frames" are frames holding 8 ms or more of pipeline
building. Pipeline coverage: the pipelines a run used that the preparation had made, over all it used.

| Route | Pipelines used | Coverage, first-session set | Coverage, comprehensive | Not prepared (ms), first-session / comprehensive | Build frames (worst ms), first-session / comprehensive |
|---|---|---|---|---|---|
| the river (recorded) | 55 | 100% | 100% | 0 / 0 | 0 / 0 |
| the fields (recorded) | 41 | 100% | 100% | 0 / 0 | 0 / 0 |
| the first dungeon (recorded) | 61 | 100% | 100% | 0 / 0 | 1 (41) / 0 |
| the menus (recorded) | 42 | 100% | 100% | 0 / 0 | 1 (80) / 0 |
| **the first boss** | 77 | 75.3% | **98.7%** | 19 (583) / 1 (0.1) | 11 (139) / **0** |
| **the dungeon's east rooms** | 52 | 71.2% | **96.0%** | 15 (372) / 2 (7.6) | 7 (142) / **0** |
| **the dungeon's deeper rooms** | 59 | 64.4% | **98.0%** | 21 (587) / 1 (0.1) | 8 (179) / **0** |
| **the field after the dungeon** | 71 | 85.9% | **98.2%** | 10 (183) / 1 (7.2) | 3 (125) / **0** |
| **field 14's building** | 52 | 80.8% | 82.7% | 10 (102) / 9 (38) | 2 (113) / 1 (51) |
| **the editor** | 52 | 61.5% | 67.3% | 20 (268) / 17 (149) | 5 (109) / 5 (81) |
| **attract mode** | 5 | 100% | 100% | 0 / 0 | 0 / 0 |
| **the credits** | 42 | 100% | 100% | 0 / 0 | 0 / 0 |

- **Frame times**: every route averaged 16.67 ms (60 fps) both ways. On the held-out dungeon and field routes the
  comprehensive set cut p99.9 from 23-44 ms to 21-23 ms and removed every frame that held a pipeline build; the
  worst frames left (37-132 ms) held no building ([H] loading, as in the other set). Field 14 and the editor kept a few
  build frames (their passes, below). With the warm-up, the recorded routes built **one** pipeline in play instead
  of 41-61 (the first, before the warm-up reached it).
- **Shader coverage** (each translation a draw needed, made before play or compiled in play), counted separately:
  the comprehensive set compiled **none** in play on all twelve routes (2-36 translations each, all from the
  prepared ones); the first-session set compiled 4-15 in play on each held-out route.
- **What the profiles missed** (unforeseen.py, each pipeline not prepared against the prepared ones with the same
  programs): of 25 distinct pipelines, 21 had programs of one package record and differed from a prepared one in
  render state the recordings had not shown for that group -- a depth function (LESS against LESS-OR-EQUAL), the
  alpha test on or off, stencil on or off, a cull face, and the editor's own target (8-bit RGBA, lines) for
  materials the fields draw to the HDR target; 2 were records of a group class no recording had reached; 3 had
  programs of no package record (the editor's own programs; two post-processing pipelines in field 14). A wider
  rule (each group given its class's profiles too) would have made only 2 of the 21: the variants are the
  passes', not the classes', so they are learned from recordings, not guessed.
- **The cost**: start-up reached play ~9 s later with the comprehensive set (all 2,726 translations read into
  memory before the first frame); the process's private memory higher by 9-440 MB, mostly 150-260 (the pipelines the
  warm-up made, and the translations held in memory).

**What changed from it**:
- **The manifest and the profiles learned again from every recording** (build/m92/relearn.py): the manifest's own
  and the held-out batch's runs, the same pipeline counted once by its key's canonical fields -- the manifest **131**
  pipelines (was 98 lines, 76 distinct), the profiles 81 in 37 groups and 50 in 6 classes (were 37 in 17 and 30 in
  5). Of the package's 569 records, 125 now have their group's profiles, 382 their class's, 62 neither (were 62, 400
  and 107). The held-out areas are recorded now, so their coverage above stays the measure of a first visit to an
  area no recording reached.
- **Pipelines met in play are made at the next start** (ppfx_render.cpp `warm_start`): the warm-up also takes the
  recipes recorded in play that the preparation had not made (their pipelines in the cache since that session
  ended), so an area's second visit builds none, without a preparation in between.
- **The start-up stall removed** (patches/ps3recomp/0162): the shader folder's translations are checked by their
  headers on a thread of their own and read when first asked for; after the warm-up the ones it read are dropped
  from memory (their files stay).

**The second batch** (build/m92/v2.py; F:\3d dot game heroes\m92\evidence\v2; the build with those three changes,
otherwise as above):
- **The preparation as it ships now**, cold: **2,202 pipelines** (131 recorded + 2,071 from the package's records)
  and **2,716 translations** in **8.4 s** on 15 threads (translations 2.4 s, pipelines 5.6 s), none failed; 105 MB
  on disk (the translations, and 20 MB of pipeline cache).

| Route | Pipelines used | Coverage | Built in play | Build frames | p99 / p99.9 / worst (ms) | Reached play | Private memory |
|---|---|---|---|---|---|---|---|
| the river | 52 | 100% | 0 | 0 | 19.8 / 22.1 / 64.3 | 21.8 s | 1,334 MB |
| the editor | 52 | 100% | 0 | 0 | 17.7 / 22.2 / 236 (*) | -- | 962 MB |
| field 14's building | 49 | 100% | 0 | 0 | 18.4 / 21.7 / 53.2 | 21.8 s | 1,026 MB |
| the first boss | 80 | 98.8% | 1 (0.7 ms) | 0 | 19.6 / 23.3 / 66.2 | 21.7 s | 1,036 MB |

  (*) the editor's drive has no route segments: its frames include the start-up. The first batch's runs reached
  play in 21.6 s with the first-session set and 30.5-31.7 s with the comprehensive one: **the start-up stall is
  gone**. The warm-up made 2,202 pipelines in 10.7-11.2 s on its two low-priority threads (during start-up and the
  title; a draw that comes first builds its own from the cache, as before).
- **A second visit** (the editor, prepared with the profiles as they were before, played twice; the second over the
  first's shader folder, pipeline cache, recipes and NVIDIA cache, no preparation between): the first visit built
  **17** pipelines not prepared (147 ms, 6 frames held, the longest 81 ms); the second built **none** -- its warm-up
  made the 1,850 prepared and the 17 met before in 2.5 s, and the draws took all 52 they used.
- **The rendering unchanged**: the RPCS3 reference scenes (tests/rpcs3_reference.py, offscreen; the river's
  reflection, the entry hall, the corridor, the king's close-up with bloom and the shadow map, the editor's lines)
  5 of 5 within the accepted figures with this build.
- **A fresh install** (tests/player_install_test.py fresh check game prepare --run-game; F:\3d dot game heroes\
  install-test): from the disc image through the worker, the comprehensive preparation as part of it; the game's
  river fight from the install built **no** pipeline in play (2,202 made by the warm-up, 52 taken); the prepare
  stage: 2,202 pipelines (2,071 from the package's 569 records), 2,716 shaders, none failed; the check: prepared,
  comprehensive. The install's lift equals a developer lift made from the current configuration byte for byte
  (build/r8/gen18; the test's earlier reference predated M10.5's title-menu patches). The launcher's Play and
  Shaders pages on it, hidden (build/m92/nav_shots.py): "Shaders are ready for your graphics card"; "Preparation
  complete: 2716 shaders and 2202 pipelines from the game's files are ready", the details line beneath.

**Graphics pipeline libraries** (build/m92/gpl_probe.py; F:\3d dot game heroes\m92\evidence\gpl): the first 300
recipes of the comprehensive preparation, cold (our folder and NVIDIA's cache empty per process), one thread, RTX
3070 (driver 616.92), each mode its own process:

| | mean | median | p95 | max |
|---|---|---|---|---|
| a whole pipeline (Vulkan 1.0 device) | 9.1 ms | 3.9 ms | 32.5 ms | 59.0 ms |
| a whole pipeline (device with the libraries) | 9.0 ms | 3.8 ms | 33.1 ms | 59.0 ms |
| library: vertex input / fragment output | 0.002 ms | 0.001 ms | 0.005 ms | 0.08 ms |
| library: pre-rasterisation (the vertex shader) | 3.5 ms | 0.06 ms | 25.1 ms | 43.0 ms |
| library: fragment shader | 3.0 ms | 0.05 ms | 16.4 ms | 27.3 ms |
| **fast link of four libraries** | **0.003 ms** | 0.002 ms | 0.006 ms | **0.013 ms** |
| optimised link | 3.6 ms | 0.06 ms | 24.9 ms | 42.4 ms |

- Once a pipeline's shader libraries exist, linking it is ~3,000 times cheaper than building it whole; the cost
  moves to the libraries, which depend on the programs (and the part of the state each library holds) rather
  than on the whole state. So libraries made for every prepared program would turn the remaining unforeseen
  pipelines -- programs known, a state not foreseen (21 of the 25 above) -- into a link of a few microseconds,
  with the optimised pipeline made in the background.
- **Not adopted in M9.2** [decision, reasoned]: with the coverage above, no held-out game route has a frame held by
  building (the editor and field 14 had, before they were recorded); the libraries need a Vulkan 1.3 instance and
  VK_EXT_graphics_pipeline_library, which AMD's Windows driver is not established to have, so they can only be an
  accelerator beside the present path; and they change how every pipeline is made (state split across four
  libraries, dynamic state, a second optimised pipeline swapped in) -- a renderer change of its own, to be
  measured on AMD and Intel first. The probe stays (`PS3RECOMP_VK_GPL_PROBE=1`, `DOTHEROES_GPL_PROBE`), off by
  default.

## 2g. What M9.2 leaves (the established limits, 2026-11-16)

**Shader coverage** and **pipeline coverage**, stated separately:
- **Shaders**: every program on the disc is found (636 vertex, 2,481 fragment records; none unparsed) and
  translated before play with the parameters its own header gives, and the variants the profiles name (2,716
  translations, none failed). On all 22 measured runs with the comprehensive set (both batches), **no translation was
  compiled in play**. A draw with texture kinds or an sRGB output no header or profile names would still be
  translated when first drawn; none was seen.
- **Pipelines**: predicted, not proven. On areas no recording had reached, the first profiles prepared **96-99%** of
  the pipelines the dungeon and field routes used (no frame held by building), and **67-83%** where a pass no
  recording had shown was used (the editor's target, field 14's building); those areas are recorded now and
  prepared at 100%. **Not 100% in general**: the game's later dungeons, fields and bosses are beyond the drives'
  saves and were not measured.

**The limits**:
1. **Passes no recording has reached** are not foreseen: 62 of the package's 569 records belong to group classes
   no recording reached and get no pipelines; a material drawn with a state its pass was never recorded with is
   not prepared. Such a pipeline is compiled when first drawn (~4-65 ms on the test PC, logged with its frame),
   recorded, and **made at every later start** (section 2f) -- a first visit's stutter at most, never a second.
   More recordings of later areas (the profiles learned again with build/m92/relearn.py's method) close it further.
2. **Post-processing pairs** (PPFX): all 1,859 of its fragment programs are translated, but which are paired and
   drawn to which target is decided at run time; only the recorded ones (the manifest) are prepared as pipelines.
3. **Measured on one GPU** (RTX 3070, NVIDIA 616.92, Windows). The path is plain Vulkan 1.0 pipelines and a
   VkPipelineCache, vendor-neutral, unchanged on Linux; AMD and Intel (and their driver caches' controls for cold
   tests) are not measured yet.
4. **The cost**: the preparation ~8.4 s on 15 threads (~26 s on one), 105 MB on disk; the warm-up ~11 s of two
   low-priority threads at start-up and ~100-130 MB more memory in play.
5. **Re-preparation**: a driver change invalidates the pipeline cache, and any change to the translator's source
   (the SPIR-V cache's key) the translations; the launcher prepares again (automatically, by its setting).
6. **Pipeline libraries** not adopted (section 2f): measured, kept as a probe, a renderer change of its own.

## 3. The design

1. **Static discovery in the game** (`dotheroes --shader-inventory`, port/native/render/shader_discovery.cpp):
   the containers above parsed from the player's own disc, each program identified as the renderer identifies it
   (its own functions: the vertex program's inputs, the fragment program's structure and units), the translation
   parameters derived from the headers. One implementation, so what is prepared is what the renderer asks for.
2. **Every shader translated** (`--prepare`): each discovered vertex program with its layout, each fragment
   program with its control word and the variants the profiles need, to SPIR-V through the existing cache, in
   parallel; failures counted and listed, never skipped silently.
3. **Pipelines from structure**: the render state normalised (section 2); for each package record the profiles its
   group's pass uses (targets, blend, depth, stencil, cull, alpha test) -- a small table of numbers,
   config/pipeline_profiles.txt, learned from the recorded routes and checked against areas not recorded; the
   recorded recipes and the manifest kept as further coverage; pipelines made in parallel.
4. **At run time**: the prepared pipelines created in memory in the background during start-up and the title, so
   a draw finds them made; anything unforeseen compiled as before (never skipped), logged with its duration and
   its frame's time, and recorded for the next preparation; graphics pipeline libraries measured as a fast link
   for the unforeseen, where the driver has them.
5. **The launcher**: the same offer, bar, stop and resume; the time left from measured rates; Rebuild makes the
   whole set; no play while preparing; coverage under Shaders and in Diagnostics.

## 4. Stages

| Stage | What | Done when |
|---|---|---|
| M9.2a | the inventory and static discovery (section 3.1) | every recorded program found in it; the counts on record |
| M9.2b | every discovered shader translated to SPIR-V (3.2) | translated and failed counts by container; time |
| M9.2c | pipelines from structure (3.3), normalised state, parallel | the prepared set's size and time; its coverage of the recorded set |
| M9.2d | the runtime fallback, warm-up, logging; pipeline libraries measured (3.4) | every runtime compile in the log with its frame; the warm-up's time and memory |
| M9.2e | the launcher (3.5) | progress, time left, stop/resume, rebuild, coverage shown |
| M9.2f | validation: fresh installs, empty caches, the 98-pipeline build against the comprehensive one on the same routes, areas never recorded | average, p95, p99, p99.9, worst frames and compile stalls; shader and pipeline coverage stated separately |

## 5. Validation method

- **Held out**: the manifest's four routes (the river, the fields, the first dungeon, the menus) are what the
  profiles may learn from; the bosses, the editor, other fields, later dungeons and the credits are not recorded
  before they are measured, so their coverage is a first playthrough's.
- **Cold**: our shader folder empty and the driver's own cache pointed at an empty folder for the process
  (NVIDIA: `__GL_SHADER_DISK_CACHE_PATH`; AMD and Intel equivalents documented before those GPUs are claimed).
- **Reported separately**: shader coverage (programs met in play that were translated before play) and pipeline
  coverage (pipelines met in play that were prepared); never 100% without the evidence.

## 6. Constraints

Native Vulkan; nothing of the game's distributed (programs, translations, pipelines are made on the player's
machine from their files; what ships is code and numbers); AMD, NVIDIA and Intel; Linux kept in mind; no vendor
requirement; the rendering unchanged (the reference scenes against RPCS3, the snapshot drives).

## 7. Tests stay off the desktop

tests/desktop_guard.py (the owner, 2026-11-16): a window only while the owner has approved an on-screen run
(`allow MINUTES WHAT`, after their yes); otherwise every request for one is overridden (tests/boot_smoke.py: WINDOW,
VISIBLE, TOPMOST, GRAB_EVERY, a scenario's `#! env` line), a window the game shows anyway ends the run at once
(`Watch`: the process's visible top-level windows, every 0.25 s), the launcher's `--nav-test` is hidden unless
`CUBIC_NAV_HIDDEN=0`, and RPCS3 captures refuse to start. tests/test_desktop_guard.py (6).

# M9.2 revisited: comprehensive shader and pipeline preparation

The owner's request (2026-11-16): before M11, eliminate shader-compilation stutter as thoroughly as technically
possible, including during a player's first playthrough. The 98 pipelines the first-session manifest prepares
(docs/M9_LAUNCHER_PLAN.md section 15) come from a handful of recorded routes and are not enough for the finished
release. Stages M9.2a-M9.2f, each validated, committed and pushed; M11 starts once this is done or its remaining
limits are established. Labels as in the other records: [C] read in the code, [M] measured, [H] hypothesis.

## Checkpoint

- **Stage**: M9.2a (inventory and static discovery) in progress.
- **Done**: the investigation below (section 2); the test harness keeps every automated run off the desktop,
  enforced centrally (section 7).
- **Next**: the inventory in the game binary (`--shader-inventory`), checked against every recorded program.
- **Open questions for the owner**: none yet.

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
   config/pipeline_profiles.json, learned from the recorded routes and checked against areas not recorded; the
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

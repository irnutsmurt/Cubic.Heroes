# First-launch shader and pipeline preparation: requirement and plan

A release-readiness requirement (the owner, 2026-11-10). The persistent caches
help, but a shader or pipeline the game has not used before on this PC is
still compiled when it is first drawn, and that can hitch during play. Before
release: prepare the known shaders and pipelines before gameplay, keep what is
still valid, throw away what is stale, test with caches that are really empty,
and never claim complete coverage from the areas the tests happen to visit.

Status: **steps 0-3 done** (M9.0-M9.2, 2026-11-14: docs/M9_LAUNCHER_PLAN.md sections 9 and 11) -- the cold
and warm costs measured; the native renderer's own pipeline cache (validated, checksummed); the variant record
(recipes: the programs and parameters, kept in the install); `dotheroes --prepare`, run by the pipeline worker after
an install, an update, a driver or GPU change. **Step 5 investigated** (section 11 there: about half the programs
seen can be found in the player's shaders.ps3.pkg; not yet used). Step 4 not started. The inventory below, then the plan. Labels as in
the other records: [C] read in the code, [M] measured, [H] hypothesis.

## 1. What is compiled at run time in the native release today

A draw of the native renderer (port/native/render/ppfx_render.cpp) needs, the
first time its combination is seen:

| Stage | What | Cached between sessions? |
|---|---|---|
| 1. Microcode to text | the game's NV40 vertex and fragment programs decompiled to HLSL text (`programs_text`), with per-draw parameters folded in: texture kinds (cube, volume, shadow, unnormalised), gamma and colour expansion, sRGB output, the alpha test's function, user clip planes, the window-position grid [C] | no (cheap, CPU) [H: not measured] |
| 2. Text to SPIR-V | glslang (`rsx_hlsl_to_spirv`) | **yes**: `PS3RECOMP_SPIRV_CACHE`, one file per shader, checked at start-up against this build, glslang, GPU and driver and preloaded (ps3recomp 0090); the install sets `<install>/data/cache/shaders` (port/main.cpp) [C] |
| 3. Pipeline | `vkCreateGraphicsPipelines`: the two modules plus the render state the record carries (blend, depth, stencil, topology, vertex layout, render pass and formats) under a 64-bit key in `s_pipes` [C] | **no**: both creation sites pass `VK_NULL_HANDLE` as the pipeline cache (ppfx_render.cpp, the PPFX pipelines and asp's). Only the driver's own disk cache, if it has one and it is warm, can help [C]. The `VkPipelineCache` of ps3recomp 0090 is the backend's (rsx_vulkan_backend.c), whose drawing the native release no longer does |

So the record in NATIVE_PORT.md's priority 8 ("a VkPipelineCache kept on
disk") is true of the backend, not of the native renderer that draws the
game since R7. What a cold first launch costs in the native release is **not
measured**: the 2026-11-02 numbers (one 267-278 ms freeze entering the
dungeon cold; warm, 59 pipelines in 3 ms, HANDOFF.md) were the backend's.

What is not known [H]:
- how many shader variants and pipelines the whole game uses: the variants
  multiply the programs by the parameters of stage 1, and the pipelines by
  the render state of stage 3. The test routes see a few hundred
  translations (83 on one 2026-11-03 install relaunch); the game has 567
  precompiled Cg programs plus PPFX's own (FEASIBILITY.md), and which
  parameter combinations each can meet is decided by the game at run time;
- how much of a cold hitch is stage 2 and how much stage 3, and how much
  the driver's own cache hides in our runs (the test PC's NVIDIA cache is
  warm from months of runs).

## 2. Requirements

1. **Prepared before play.** Every known shader variant (stage 2) and
   pipeline (stage 3) is compiled before gameplay starts: at first launch
   (during start-up and the title, on worker threads, with visible progress,
   skippable), and optionally by the setup app at install. Anything not
   prepared is still compiled when first drawn -- never an error -- and is
   counted.
2. **Kept.** The native renderer's pipelines get a `VkPipelineCache` kept on
   disk beside the SPIR-V (`<install>/data/cache/shaders`), saved at exit
   and at a safe point after preparation, as the backend's is.
3. **Invalidated correctly.** A cached item is used only for the same game
   build (translator and renderer versions), glslang version, GPU vendor and
   device, driver version and the driver's pipeline-cache UUID; anything else
   is discarded and rebuilt, never loaded. A truncated or corrupt file is
   detected (size and checksum) and discarded. The cache has a size bound.
   A game update that changes the translator invalidates its translations.
4. **Coverage stated, not assumed.** The list of known variants records
   where each came from (an exhaustive enumeration, a named test route, a
   play session). The report says how many there are and from where. A list
   built from the routes we test covers those routes; it is never described
   as the whole game. Coverage of the whole game needs either an enumeration
   from the game's data or a design that makes the variants few (section 3,
   steps 4-5).
5. **Hitches accounted for.** Every on-demand translation and pipeline
   creation during play is logged with its time and the frame it delayed, and
   counted per route segment (as `[vk-compile]` and tests/batch_report.py do
   for the backend), so a cold run's spikes are attributed.
6. **Tested empty.** Cold tests start with our cache directory empty **and**
   the driver's own shader cache pointed at an empty folder for the process
   (NVIDIA: `__GL_SHADER_DISK_CACHE_PATH`, as in 2026-11-02; the AMD and
   Intel equivalents found and documented before those GPUs are claimed), on
   a fresh install. Then: the same run warm; a run after the recorded driver
   version is changed (the cache must start empty and say so, as the backend's
   cache-tamper test); a run after a translator version change.

## 3. Plan

| Step | What | Done when |
|---|---|---|
| 0 | **Measure the present native release cold and warm**: empty caches as in requirement 6; the river, the fields, the dungeon and the menus; count and time translations (stage 2) and pipeline creations (stage 3) per segment, and the frame-time spikes they cause; then warm | the cost of each stage, cold and warm, on record |
| 1 | **A pipeline cache for the native renderer** (requirement 2), validated as the backend's (requirement 3) | a warm run creates its pipelines from the cache (counted); the driver-version tamper test starts empty |
| 2 | **A variant manifest**: on every translation and pipeline creation, its key (program hashes, the stage-1 parameters, the pipeline state) recorded with its source; merged across test runs; a shipped manifest from all test routes and the owner's sessions, labelled as such; new variants met in play appended locally | the manifest exists with sources; its size and coverage reported |
| 3 | **The preparation pass**: at first launch, compile the manifest's SPIR-V and pipelines in parallel on worker threads before and during the title, with progress; the setup app can run it headless at install | on the routes the manifest covers, a cold install after preparation compiles nothing during play; preparation time measured |
| 4 | **Fewer variants**: where the GPU has them, `VK_EXT_graphics_pipeline_library` and extended dynamic state, so a pipeline depends on its shaders and little state: the shader parts compiled once per variant ahead of time, linked cheaply at draw time | measured against step 3: link times, spikes |
| 5 | **Enumeration from the game's data**: read the program containers on the disc (the 567 Cg programs, PPFX's) and the parameter domains each program can meet, to prepare variants no test has seen | the manifest's share from enumeration, and what is still only observed |
| 6 | **Release acceptance**: requirement 6's tests on a fresh install; the report states coverage by source | the numbers, and an honest coverage statement |

Order: 0 first (nothing should be built before the cold cost is known),
then 1 (small, independent of the rest), then 2-3, then 4-5 by what 0-3
show. Each is its own change, validated and measured separately, as R8's.

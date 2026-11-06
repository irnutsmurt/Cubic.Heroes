# R7: the PS3 graphics path retired from the native release build

R7 follows R6 (docs/NATIVE_R6.md, accepted 2026-11-06 with its open items
carried forward). Its scope, as the owner approved it with seven
clarifications: remove the PS3 graphics command path -- command
generation, ring decoding, the FIFO walker, the stream markers, the bridge
and the old backend's drawing -- from the native release build, proven by
build and link evidence as well as runtime counters; give every mutable
input an explicit ownership and timing rule before the walker's gate goes,
and prove it; replace the remaining state-reset calls semantically and keep
"consumed", "GPU work completed" and "presented" apart; carry the open
correctness gaps explicitly (the moat band, the post-boss warp's copy,
Bink's frames, the vertex arenas' retirement) and compare actual images
throughout; keep the real occlusion behaviour separately identifiable; take
a fresh game-thread profile and cut what it supports; and measure the
release configuration against the preserved R6 and production builds,
genuinely uncapped. As before: measured [M], read from code [C], inferred
[I], a hypothesis [H]; evidence under build/r7/ (local, game-derived).

The old renderer stays: the development builds (tools/r1_dev_lift.sh,
tools/r6_native_lift.sh, tools/r7_native_lift.sh) still contain the walker,
the decoder, the draw engine and the bridge, as the reference the native
renderer is compared against. The native **release** build
(tools/r7_release_build.sh) contains none of them.

## Current status (2026-11-06)

| Item | State |
|---|---|
| The PS3 graphics command path in the native release build | **gone**: no FIFO walker, ring decoding, method decoder, RSX state, register file, draw engine, live draw, backend drawing, stream markers or bridge is compiled into build/r7/rel3 -- 0 of their functions in its PDB, 375 fewer functions than the development build (section 4) |
| Command generation | **gone**: every libgcm call that wrote command words is replaced at its entry by what it means (section 3); the detector found **0** command words in the title's ring in every scenario on the release build (section 4) |
| The walker's synchronisation | the native renderer's: records consumed after the game's flush, labels, the reference, waits, reports and flips written by it at the stage the RSX would -- consumed, GPU work completed, presented (section 1) |
| Ownership of mutable inputs | a rule (section 2), checked: inputs hashed at the flush and at their capture; the one input the game rewrites inside the window (the water's texture) found and taken at the flush with the small textures; **0** inputs used that changed after the flush (rel3) |
| The moat band | its mechanism measured (the water's texture rewritten after the flush) and closed by taking it at the flush (section 2) |
| Pictures | grabs follow R6's and production's; Bink's movie frames as production draws them (median difference 0.00, maximum 1.54 of 255, 181 same frames); the snapshot from the native image (section 5) |
| The post-boss warp | its 2D transfer implemented as a native copy; **not exercised** (no automated route reaches it: the owner's optional session) |
| Vertex-arena retirement | **forced and exercised**: 4 MB arenas, two kept, every image retired every 30 frames -- 16,509 arena retirements in the river's 100 s, the pictures the normal run's; 5,443 under the Khronos synchronisation validation, 0 errors (section 2) |
| Delayed consumption | the capture check with consumption delayed up to 500 us a record (the native thread's work on a frame stretched from about 12 to 32 ms after the flush) and with a 64-record queue (the full-queue path 271,358 times): **0** inputs used that changed after the flush (section 2) |
| Repeated cycles, memory | five title-to-dungeon cycles: every segment, 0 command words; a growth of 100-220 MB a cycle (inherited from R6: the record slots kept large buffers) found by a census and fixed -- the release now flat at 1.1 GB, production 1.6 GB (section 4) |
| Synchronisation validation | the Khronos layer's synchronisation validation over the river fight (dev and release) and the menus: **0** errors; its negative control (the present not waiting for the blit) caught, 11 hazards (section 4) |
| Occlusion | real counts by default; the previous port's results kept apart (DOTHEROES_R7_OCCLUSION=legacy); the RPCS3 comparison still the owner's (section 6) |
| Game thread | the recorder 18.9% of it (R6's 23.5%; 0.46 ms a combat frame against 0.81) -- program identity, the shadow's owner map, inlined switches (section 7) |
| Throughput (uncapped) | combat: R7 152.2-152.8 fps, production 128.4-144.0, R6 145.3-151.2, R7's p99.9 the shortest; exploration: R7 168.5-168.7, production 179.7-181.1 (7% ahead), R6 165.8-168.3; 144 fps sustained not reached (section 8) |
| Capped and gameplay timing | every arm at 60.0, R7's presents the steadiest; R7's gameplay timing at 60 as production's |
| Not done | the warp's run; the RPCS3 comparison of the exposure; the depth clamp the game's reset enables (z min/max control 0x11, modelled neither by the native renderer nor by the old engine: a behaviour change to propose, section 9); the release still carries the development recorder's dormant modes |

## 1. No walker: the native renderer is the title's GPU [M, C]

R6 left the walker almost nothing to do (NATIVE_R6.md section 9): flips,
labels, the wait-flip acquire, the profiling meter's report requests, the
stream markers, and the `walked` gate the native thread consumed behind.
R7 gives each of those to the native side, in the order and at the stage
the RSX itself would have done it.

**When a record may be consumed.** Every record is consumable once the
game's flush that would have submitted its commands has happened:
cellGcmFlush (libgcm 0x006A336C, already hooked) publishes the records made
since the previous flush (the queue's `flushed`), and the native thread
consumes `[0, flushed)` -- no walker, no marker, no `walked`. A full queue
publishes what is recorded, as libgcm's own ring-full callback flushes the
ring, and is counted apart (`forced`).

**The front end (cellGcmSys's native mode, ps3recomp 0116).**
- A flip is requested without a marker (`_cellGcmSetFlipCommand`): status
  WAITING, the request counted, the handler called, as before. It is
  *executed* by the native renderer -- `cellGcm_native_flip_executed`, the
  walker's half of a flip (the displayed buffer, the flip counted as
  reached), called once the present is submitted after the frame's work --
  and *completed* at the next vblank tick (at once in the benchmark mode),
  where the game's flip status becomes DONE.
- `get` answers `put` on a guest read of it: the ring has nothing to
  consume. Every word that nevertheless reaches the ring is found by the
  detector (below), so this answer is checked, not assumed.
- `ref`, labels and reports are written by the native renderer
  (`cellGcm_native_ref`, `cellGcm_label_write_pub`,
  `cellGcm_report_write_pub`).
- The ring-full callback recycles without waiting (never expected; counted).
- main.cpp's walker thread becomes a window pump: the SDL events (the pads'
  keyboard state comes from them) and a blank present until the first flip.

**The three completion stages, kept apart** [C]:

| What | Written when | Stage |
|---|---|---|
| command label (cellGcmSetWriteCommandLabel, NV406E release) | the native thread consumes it | consumed |
| wait for a label (cellGcmSetWaitLabel: the wait-flip acquire) | the native thread consumes it: it waits until the label holds the value, retiring its own batches meanwhile (1 s at most, counted) | consumed |
| back-end and texture-read labels (Bink's fence) | the work recorded before it has completed on the GPU (the open batch submitted at once, or the last batch in flight) | GPU work completed |
| the reference (cellGcmSetReferenceCommand; cellGcmFinish waits on it) | the same | GPU work completed |
| reports (the occlusion counts, the profiling meter's requests) | the same (with the counts its queries took) | GPU work completed |
| a flip | its present submitted after the frame's work (executed), then the vblank tick (completed) | presented |

**The detector.** At every flush the title's ring is checked: any word
between the position seen last and the context's current is decoded by
method and counted, the first few named with their frame. "No command word
is generated" is the claim that count stays 0 -- in the release build over
every scenario run.

## 2. Ownership and timing of mutable inputs [M, C]

The `walked` gate gave R6 its timing: the walker read a record's small
textures as it passed the record's marker, after the game's flush, as the
RSX's front end would. R7 replaces it with a rule, made explicit and then
checked.

**The PS3's contract.** Guest bytes a command reads belong to the CPU until
the flush that makes the command visible (put). From then until the GPU
signals that it has read them -- a label, a report, the reference, a flip's
completion, whatever the game waits on before writing them again -- the RSX
may read them at any moment, so a correct game does not write them. After
that signal they are the CPU's again.

**R7's rule** [C]:
1. A record is consumed only after the flush that would have submitted it
   (section 1).
2. The native thread reads every guest input of a record -- vertices,
   indices, every texture whatever its size or format (DXT included),
   Bink's planes, the water's texture, PPFX's vertex bytes and small
   textures (R6 took these at the call: R7 takes them at consumption too) --
   while it consumes that record: copied into a batch's upload buffer, or
   hashed to find an identical image already held. Nothing reads guest
   memory later; the GPU never reads guest memory.
3. Every signal the game can use to reuse those inputs is written by the
   native thread, in record order, after the records before it were read:
   at consumption for the front end's, at the GPU's completion of that work
   for the back end's, reports and the reference, at the present for a flip
   (section 1). So the game's own synchronisation keeps each input as it
   was at the flush until it has been taken -- a stronger ordering than
   R6's, whose walker wrote labels and flips before the native thread had
   read the large textures and the vertices.
4. The write watch is detection only. It lets an unchanged range skip
   rehashing (its reset precedes the read, so a write during or after the
   read is seen next time), and R7 no longer takes a range from "earlier in
   the frame" on the strength of a sample of eight words: a range rewritten
   within a frame is hashed again.

**The capture check** (DOTHEROES_R7_CAPTURE_CHECK=1, development): each
record's input ranges hashed on the game's thread at the flush, and again by
the native thread as it takes them. A difference is a write inside the
window the rule says the game does not write in -- counted and named, never
silently used.

**What it found, and the rule's one amendment** [M]. On the first release
build (build/r7/rel2, every input taken at consumption):

| Run (rel2) | records checked | input ranges | records with a range written between the flush and the capture |
|---|---|---|---|
| rcc1river, the river fight 100 s | 395,277 | 1,768,140 | 10,204 -- every one the same range: a 128x128 linear A8R8G8B8 texture in local memory (0:0A90AF80, 64 KB) |
| rcc1menus, the title, its Bink movie, the menus (to 138 s: the check's hashing slowed the game past a drive step's 30 s wait) | 646,679 | 2,819,068 | 3,090 -- the same texture |

That texture is the water's (R5 found it: the game rewrites it after
issuing the draw, NATIVE_R5.md section 6) -- and, the check shows, also
after the flush, without waiting for the GPU: the protocol the rule assumes
does not hold for it. Taken at consumption, a water draw could draw the
next frame's water: the moat band's mechanism, measured at last (R6 had it
as a hypothesis). Everything else -- vertices and indices, Bink's planes,
large and DXT textures, every other texture -- was the same at the capture
as at the flush.

So R7's rule takes **small uncompressed textures at the flush**: at most
256 KB, not DXT, not an address a record drew -- the walker's own class,
which R5's dual run found right for the water -- copied into the record on
the game's thread when the game's flush publishes it, the earliest moment
the RSX could have read them (DOTHEROES_R7_TEX_AT_FLUSH=0 restores
consumption, for development). Large and DXT textures, vertices and indices
stay at consumption, where the check found them stable. On the build with
the amendment (build/r7/rel3):

| Run (rel3) | records checked | ranges written between the flush and the capture, used | rewritten after the flush, taken at it (not used) |
|---|---|---|---|
| rcc2river, the river fight 100 s | 376,044 | **0** | 9,089 (the water) |

Every input the renderer drew with was as the game left it at the flush.
The copy costs the game thread 10,919 textures (828 MB) in the river's
100 s and 95,081 (5.8 GB, Bink's colour planes among them) in the menus'
300 s: 8-19 MB a second.

**The check over every scene and under stress** [M] (build/r7/b4, the
development build with the same rule, the capture check on; build/r7
batch 2):

| Run (b4) | records checked | input ranges | ranges written between the flush and the capture, used | rewritten after the flush, taken at it (not used) |
|---|---|---|---|---|
| cc1river, the river fight 100 s | 388,900 | 1,709,090 | **0** | 9,638 (the water) |
| cc1menus, the title, Bink, the menus (to 141 s: the check's hashing again slowed the game past a drive step's 30 s wait) | 647,266 | 2,823,526 | **0** | 3,110 |
| cc1fields, 200 s | 766,666 | 3,033,237 | **0** | 0 |
| cc1dungeon, 200 s | 875,059 | 3,154,232 | **0** | 5,467 |
| cc2river: a 64-record queue, consumption delayed 40 us a record | 485,008 | 2,115,146 | **0** | 0 |
| cc3river: the full queue, consumption delayed 250 us a record | 175,793 | 734,097 | **0** | 3,865 (the water) |
| cc4river: the full queue, consumption delayed 500 us a record | 118,298 | 483,830 | **0** | 2,337 (the water) |

cc2river took the full-queue path 271,358 times (records published before
the game's flush, the game blocked 54.6 s of the 100 s): a small queue keeps
the native thread within 64 records of the game, so the window between
publication and capture is long in time but short in records -- the game
cannot write while it waits. cc3river and cc4river widen the window
instead: at the full queue size the delay stretched the native thread's
work on a frame's records from about 12 ms after the flush (cc1river: the
run less the native thread's waits for records, a frame) to about 25 and 32
ms, while the checked game's frame went from 36 ms to 43 and 47 ms. The
queue never filled (0 full-queue waits): the game did not run further
ahead, because its own wait for the previous flip -- completed at the
present, after the delayed records -- held it. That is the protocol the
rule relies on, now measured under delay: the widest window the game's
synchronisation allows, and still no input it drew with changed after the
flush. (The check hashes every input twice, so the capped river runs at
about 27 fps with it on, 60 without; the windows above are the checked
game's.)

**Early reuse of the native side's own buffers** [M]: the vertex arenas
(the ring the native thread copies vertices and indices into, retired when
the GPU has finished with them) cut to 4 MB, two kept, and every image
retired every 30 frames (DOTHEROES_R7_ARENA_MB=4 DOTHEROES_R6_ARENAS=2
DOTHEROES_PPFX_REALLOC_EVERY=30), so an arena is reused within frames of its
retirement -- the path R6 left untested. lt1river (the river fight 100 s):
**16,509 arena retirements**, the game at about 20 fps under the churn, its
grabs the same pictures as the normal run's (the scene, the water, the
debris, the HUD: build/r7/lt1river/run/grabs against v1river's; the one
dark grab a load's fade at a different second). The same under the Khronos
synchronisation validation (val3river, 90 s): 5,443 retirements, **0**
errors.

**The reverse case: native images and CPU writes.** An address a record
drew into is served from its native image. If the CPU later rewrote that
memory and a draw then sampled it, the image would be older than the guest
bytes. DOTHEROES_R7_TARGET_WATCH=1 watches the guest pages of local-memory
targets and counts such samples [M]: tw1river (the river fight 100 s)
sampled native images 72,774 times, tw1menus (the title, Bink, the menus
300 s) 84,500 times -- **0** of them with their guest memory written by the
CPU since the image was drawn.

## 3. The writers replaced [M, C]

R6 replaced every draw, state and program call at its entry; what still
wrote words in the river fight were 13 libgcm functions (NATIVE_R6.md
section 3). R7 hooks them and everything else found writing (config/
r7_native_hooks.json: 162 entry points, R6's 119 and 43 more), each replaced
by what it means:

| Entry point | Wrote | R7 |
|---|---|---|
| cellGcmFlush (0x006A336C) | the put register (no word) | publishes the records made since the last flush; its body runs |
| performance-marker push and pop (0x0069DF80, 0x0069E0E0) | NV4097 no-operation words carrying a string | nothing (inert) |
| cellGcmSetWaitLabel (0x0069E58C): the wait-flip acquire | NV406E semaphore offset and acquire | a wait record (consumed stage) |
| a report request (0x006A5624, the profiling meter's) | NV4097_GET_REPORT, type 1 | a report record, written at completion where cellGcmSetReportLocation said |
| cellGcmSetReportLocation (0x006A9358) | NV4097_SET_CONTEXT_DMA_REPORT (from libgcm's table) | state: main or local |
| the frame start's reset (func_00029618): blend equation, MRT colour mask, front face, z min/max control, MRT blend, depth bounds, two-sided stencil, point sprites (0x006A5B8C ... 0x006A7484) | 22 words a frame | state, in the record's explicit form (below) |
| cellGcmSetWriteCommandLabel / BackEndLabel / TextureLabel (0x006A577C, 0x006A5830, 0x006A5900) | NV406E release; NV4097 back-end and texture-read release | a label record (consumed; completed) |
| cellGcmSetReferenceCommand (0x0069E474) | NV4097_SET_REFERENCE | a reference record (completed) |
| cellGcmSetJumpCommand (0x0069E504) | a jump | nothing (no ring to steer) |
| cellGcmSetInvalidateTextureCache (0x006A7548): Bink's, each movie frame | NV4097 0x1FD8 | nothing: the native renderer has no texture cache -- a guest texture is checked by its bytes at every use |
| cellGcmSetLineWidth (0x006A6220, its Unsafe form 0x006AA3A0): the editor's grid | NV4097 0x03B8 | state (lines wider than 1 are counted: not modelled) |
| cellGcmSetTransferImage (0x0069FDD0): the post-boss warp's capture | NV3062 and NV3089 setup | a transfer record: a GPU copy between native images and the destination's guest memory written back at completion, or a guest-memory copy (section 5) |
| asp's fragment heap set-up (0x006CADE4's first call) | an inline transfer asp writes itself (24 words) | the allocation run as guest code (func_006CAD4C: no command), the upload replaced as for every later one |
| the flip (cellGcmSetFlip's HLE half, _cellGcmSetFlipCommand) | the runtime's flip marker | no marker; executed and completed natively (section 1) |
| the other 2D transfers, inline images, label helpers, the Unsafe forms, the FIFO reset (0x006A0D60, 0x006A1444, 0x006A17C4, 0x006A1974, 0x006A1B48, 0x006ABE64 ... 0x006AD65C, 0x0069EDB0, 0x0069EEA4, 0x006AA034 ... 0x006AB68C, 0x006A32D8, 0x006AA8A8) | (never seen called) | watched: their bodies run and every caller is named, so a call would show (and its words would be the detector's) |

**The inventory** [M]: the first R7 build (build/r7/b1, batch 1) ran every
scenario without a walker -- the river fight, the menus with the title and
its Bink movie, the fields, the dungeon, the snapshot, the field after the
dungeon, the editor and the exit with the boss beaten -- and the detector
found two writers left: Bink's texture-cache invalidate (0x1FD8, 556-1,171
words a run, two a movie frame) and, in the editor alone, the line width
(0x03B8, 3,708 words). Both are hooked since build b2. The watched entry
points were called 0 times in every run.

## 4. The native release build [M]

tools/r7_release_build.sh builds R7's lift (tools/r7_native_lift.sh: the
production lift with the 162 entry hooks, no store probes) with
DOTHEROES_NATIVE_RELEASE=ON:

- the runtime with PS3RECOMP_NATIVE_GRAPHICS (ps3recomp 0116): rsx_commands.c
  (the method decoder and RSX state), rsx_dispatch.c (the register file),
  rsx_draw_engine.c, rsx_live_draw.c and the null and D3D12 backends are not
  built; cellGcmSys.c is compiled without the walker, the 2D engines, the
  resync, the reference queue, the frame trace, the flip marker, the ring
  limit and the SPU put hook; rsx_vulkan_backend.c without its
  fixed-function and register-file drawing, its pipeline cache and the
  bridge's copies into the engine's stream -- what stays is the device, the
  window, the swapchain and present, the OSD and the frame dump (the native
  present module); rsx_vertex_compact.c without the fetch plan read from the
  register file. The topology expansion the native renderer uses moved to
  rsx_topology.c;
- the port without ppfx_bridge.cpp (R4's bridge: markers, matching,
  handoffs, handbacks, the cross-check), without the native renderer's
  handoffs, handbacks, the bridge's slots and the comparisons with the
  engine's outputs, without main.cpp's walker loop; R7 always on;
- the HLE NID table generated for what this build compiles
  (gen_hle_nids.py --define PS3RECOMP_RSX_COMMAND_PATH=0).

**The link evidence** (tools/r7_symbols.py: the function symbols of the
executable's PDB, through DbgHelp) [M]:

| Function symbols | development build (build/r7/b4) | release build (build/r7/rel3, the build measured) |
|---|---|---|
| FIFO walker (cellGcmSys.c) | 9 | 0 |
| 2D engines | 2 | 0 |
| method decoder and RSX state (rsx_commands.c) | 6 | 0 |
| register file (rsx_dispatch.c) | 18 | 0 |
| draw engine (rsx_draw_engine.c) | 18 | 0 |
| live draw | 27 | 0 |
| Vulkan backend drawing | 13 | 0 |
| R4's bridge | 9 | 0 |
| all function symbols | 38,966 | 38,593 |

These are named samples of each part; the whole difference is 375
functions present in the development build and absent from the release
(build/r7/symbols3_detail.txt lists them). Every rsx_/gcm_/vk_ function left
in the release is shader translation (the vertex and fragment program
decompilers, HLSL to SPIR-V), texture and vertex-format decoding, topology,
the cellGcm system API (flip requests, IO mapping, guest allocation) or the
Vulkan device, window and present. The link itself is evidence: the
release links with those files absent, and its first link failed on
exactly the 13 references that still reached them (cellGcmInit's SPU put
hook, the register-file fetch plan, a development hotkey arming the frame
trace, three walker diagnostics in the loader, and four walker functions
the NID table registered), each then removed.

**The runtime counters** [M]. The release build (rel2) over every scenario,
capped, real audio (build/r7 batch 3):

| Run | command words in the ring | flushes (forced by a full queue) | labels written | references written | flips executed natively | ring recycles |
|---|---|---|---|---|---|---|
| r1river, the river fight 100 s | **0** | 5,887 (0) | 557 of 557 | 1 of 1 | 5,885 | 0 |
| r1menus, the title, Bink, every menu, 300 s | **0** | 11,615 (0) | 1,171 of 1,171 | 4 of 4 | 11,610 | 0 |
| r1fields, 200 s | **0** | 9,510 (0) | 554 of 554 | 1 of 1 | 9,509 | 0 |
| r1dungeon, 200 s | **0** | 8,581 (0) | 1,172 of 1,172 | 9 of 9 | 8,572 | 0 |
| r1snap, the snapshot, 200 s | **0** | 6,782 (0) | 545 of 545 | 1 of 1 | 6,781 | 0 |
| r1after, the field after the dungeon, 250 s | **0** | 14,866 (0) | 557 of 557 | 1 of 1 | 14,865 | 0 |
| r1editor, the editor, 200 s | **0** | 5,520 (0) | 622 of 622 | 1 of 1 | 5,519 | 0 |
| r1exitboss, the exit with the boss beaten, 150 s | **0** | 3,064 (0) | 542 of 542 | 6 of 6 | 3,058 | 0 |

Every run ended as its drive does (done, or its time), and the label waits
-- the wait-flip acquire each frame -- found their label set every time (0
spun, 0 timed out). The labels are Bink's back-end fences, one a movie frame
(the title's movie plays in every run), each beside one of Bink's texture
cache invalidates. One flip a run is not presented, as in R6: the game's
first, before anything is drawn.

The development build with the same front end (b4, DOTHEROES_R7=1, batch 2:
v1river ... v1exitboss, the same eight scenarios) found **0** command words
in every run as well, no flush forced by a full queue, every drive ended
as it does.

**Synchronisation validation** [M] (the Khronos validation layer with
synchronisation validation, errors and warnings logged):

| Run | build | errors | warnings |
|---|---|---|---|
| rval1river, the river fight 90 s | release (rel2) | **0** | 10, all the same kind: a fragment output with no attachment (the depth-only passes' pipelines) |
| val1river, the river fight 90 s | development (b4) | **0** | the same 10 |
| val3river, the river fight 90 s, the arenas' retirement forced (5,443) | development (b4) | **0** | the same 10 |
| val4menus, the title, Bink, the menus 300 s | development (b4) | **0** | the same 10 |
| rval2river / val2river: the negative control, the present not waiting for the blit (DOTHEROES_R6_HAZARD=present) | release / development | **11** each: 10 read-racing-write, 1 write-after-write | |

The control shows the layer sees this renderer's hazards; the runs show it
found none.

**Repeated cycles, and a memory growth found and fixed** [M]. The
dungeon route's drive repeated: the title, the save's load, the castle, the
walk to the first dungeon, its entrance, and back to the title by the menu
(each cycle replays the title's Bink movie). Every run: every segment done,
0 command words; the grabs (every 5 s) as many dark ones as production's
-- loads, fades, the menu: 15-18 of 117-122 in each five-cycle run -- and
the dungeon's entrance the same picture in the fifth cycle as in the first
(rcyc2dungeon's grabs 0125 and 0580).

| Run | build | cycles | segments | flips presented | process memory (working set / private, MB) after cycle 1 ... the end |
|---|---|---|---|---|---|
| cyc1dungeon | development (b4) | 2 | 6 of 6 | -- | 1,382 / 1,875 ... 1,551 / 2,048 |
| rcyc1dungeon | release (rel3, the build measured in section 8) | 5 | 15 of 15 | 36,317 | 1,535 / 1,860 ... **2,183 / 2,512** |
| pcyc1dungeon | production (build/r6/prodF) | 5 | 15 of 15 | -- | 1,558 / 1,789 ... 1,569 / 1,812 |
| rcyc2dungeon | release with the fix (rel5) | 5 | 15 of 15 | 36,462 | 1,120 / 1,442 ... **1,125 / 1,444** |

rel3 grew by 100-220 MB at each cycle's loads and did not level off;
production levelled off after its first cycle; R6's cycle runs
(build/r6/cyc1dungeon, cyc2dungeon) had grown the same way, unremarked. A
census of everything the recorder and the native renderer keep
(DOTHEROES_R7_CENSUS=1: the program shadow, the bound programs, the
blobs, the guest textures, the static ones, the pipelines, the vertex and
range caches, the arenas, the images, every 1,800 frames) found all of it
small and flat after the first cycle -- the growth was not in a cache. It
was in the record queue's slots: 4,096 records, reused in turn, whose
buffers (the vertex bytes, the texture bytes, the constants' delta, the
capture check's hashes) keep their capacity when cleared, so every slot
that ever held a large input -- a Bink frame's planes taken at consumption,
a long vertex stream -- held that memory for good, and each replay of the
title's movie landed in other slots. The fix: a slot being reused lets go
of any buffer past 64 KB (acquire, ppfx_recorder.cpp). With it the release
holds 1.07-1.14 GB across five cycles -- flat, and below production's 1.57
GB. Its cost, measured against rel3 (build/r7 batch 6: uncapped, batch 4's
conditions, two rounds each, the arms alternated): the river fight 150.2
and 153.6 fps against rel3's 153.6 and 151.6, p99.9 11.8-12.0 ms in both;
the fields 170.0 and 171.7 against 170.3 and 170.6, p99.9 10.5-10.8 in both;
the game thread 6.32-6.64 ms a combat frame against 6.34-6.54 -- nothing
beyond the runs' own spread (single frames of 49-117 ms in three of the
four river runs, both builds).

## 5. Pictures [M]

The black-scene defect (NATIVE_R6.md section 3) showed counters do not
prove a picture, so every R7 run grabbed the window every 5 s, and the
grabs were compared (tools/r7_grabs.py: each grab's mean brightness, its
share of pixels above black, the mean absolute difference from another
run's grab at the same second).

- **Against R6.** The first R7 runs (build/r7/b1) against R6's final build
  (build/r6/z6river, z4menus): the river's 19 grabs and the menus' 39 grab
  for grab, the same dark grabs (boot, title, loads) and the same lit ones;
  the menus' static screens 0.0-3.2 apart, one transition's timing 43
  apart. R7's river and R6's at the same moment are the same picture
  (scene, water, depth of field, HUD).
- **Against production** (build/r6/prodF, the same scenarios): the release
  build's menus (rel3, r3menus) 36 of 39 grabs within 6 of production's,
  the other three transition moments; the river and the fields as close as
  two runs of a moving scene are (the enemies' positions differ).
- **Bink's frames, frame by frame.** The attract mode plays the game's demo
  movies (Bink videos the game draws itself). Production and the release
  (rel2) grabbed the window every 0.1 s for 150 s; every grab of R7 was
  matched by content against every grab of production
  (tools/r7_bink_compare.py: a 64x36 thumbnail below the frame-rate strip),
  and the pairs that are one frame shown by both builds compared whole:

  | Pairs (thumbnails within) | pairs | full-image mean difference, of 255: p50 | p90 | p99 | max |
  |---|---|---|---|---|---|
  | 0.5 (the same frame) | 181 | **0.00** | 0.51 | 1.03 | 1.54 |
  | 1.0 | 235 | 0.16 | 2.15 | 2.71 | 3.52 |
  | 1.5 | 294 | 0.33 | 3.06 | 5.07 | 5.16 |

  Looser thresholds admit adjacent movie frames (a character a few pixels
  on) and the title screen's blinking prompt: the larger differences are
  motion, not drawing. R7 draws Bink's frames as production does.
- **The snapshot** (rel2, r1snap): two reads of a display buffer, both
  served from the native image; the two photos saved show the scene (mean
  124.5, 99.3% of pixels above black).
- **The editor** (rel2, r1editor): its 1,854 line-width calls set width 1;
  no line was drawn wider than the native renderer draws it.
- **The development build against the release** (batch 2's v1 runs against
  batch 3's r1 runs, all eight scenarios): the same dark grabs (boot,
  title, loads) and lit ones; the menus 38 of 39 grabs within 6, the editor
  17 of 18, the snapshot 20 of 23; every larger difference looked at was a
  moment where one run was at another point of its drive (one in a load's
  fade, or back at the title, the other still in the dungeon) -- both
  pictures right.
- **Under forced arena retirement** (lt1river) and **the previous port's
  occlusion results** (lg1river): see sections 2 and 6.
- **The post-boss warp: not exercised.** Its copy is implemented
  (cellGcmSetTransferImage as a native copy, section 3; the frame the copy
  leaves is dumped with DOTHEROES_R7_XFER_DUMP) and the scenarios do not
  reach it. The owner's optional session (tests/scenarios/
  owner_warp_r7.drive, OWNER_PLAY.md) is the remaining coverage.

## 6. Occlusion [M, C]

Real occlusion results (R6's queries, NATIVE_R6.md section 6) are the R7
default. Without a walker, the previous port's results -- a fixed count of
0xFFFF at each report, the report copies never made -- would be written by
nobody, so R7 writes them itself when asked: DOTHEROES_R7_OCCLUSION=legacy
keeps that behaviour available and separately identifiable (the report says
which: `legacy occlusion: reports N, copies dropped N`). Matching the
previous port is not evidence of the original behaviour where that port
returned incomplete reports; the comparison of the exposure with RPCS3
remains the owner's item (R6's checklist item 10).

Both, in the river fight (100 s, development build b4) [M]:

| Run | reports written | their counts | report copies |
|---|---|---|---|
| v1river (the default: real) | 44,730 | the draws' samples (72,825 draws counted; the last 222,080) | 202 made |
| lg1river (DOTHEROES_R7_OCCLUSION=legacy) | 44,601 | 0xFFFF each | dropped |

Their pictures: the grabs' mean brightness within 2.1 of each other at
every grab, the lit grabs' differences 1.1-16.4 (the fight's movement; two
runs of the same scene differ as much). In this scene the exposure the
reports feed shows no difference at the grabs' scale -- which says nothing
about which of the two the original's is.

## 7. The game thread [M]

A fresh profile of the release build (build/r7/pr1river: the river fight,
uncapped, DIAG_PROF over 40 s, the game thread's samples) put the recorder
-- everything under the entry hooks -- at **23.5%** of the game thread, the
rest the game's own code. Inside it:

| Inclusive, % of the game thread | rel1 (pr1river) | rel2 (pr2river) |
|---|---|---|
| the hooks' whole path (ppu_entry_observe) | 23.5 | **18.9** |
| asp's calls (asp_record_call_inner) | 11.7 | 9.2 |
| asp's draw replaced at entry (asp_draw_skip) | 8.3 | 7.6 |
| ... its record built (asp_record_begin) | 8.0 | 6.6 |
| program identity: blob_of (the hash and its compare) | 3.2 | 1.1 |
| ... the fragment program's (asp_fp_now) | 2.2 | 1.0 |
| ... the vertex program's (vp_blob_now) | 1.6 | 1.0 |
| the constants' delta (take_vconst) | 1.7 | 1.4 |
| the program shadow's per-word owner (an unordered_map<u32,u32>) | 1.5 | 0.1 |
| the shadow's page lookup (shadow_page) | 1.0 | 0.5 |
| the hooks' own dispatch (ppfx_rec_observe, self) | 2.3 | 2.0 |

What rel2 changed, where the profile pointed:
- **Program identity**: blob_of hashed every program byte at a time (FNV)
  each time a constant patch changed it -- after every one of PPFX's and
  asp's patches. Now a 64-bit hash with xxHash64's lanes (the renderer's
  fast_hash): 3.2% to 1.1%.
- **The program shadow**: each word's owner (the shader object whose upload
  or patch wrote it, R6's fix for resident binds) lived in an unordered_map
  per word, and every word looked its page up in another. Now the owner is
  in the shadow's page beside the word, the last page is kept, and a write
  takes each page once: 2.5% to 0.6%.
- **Switches read on every call** (R6's =all and asp's bypass): cached
  behind always-inlined getters.
- **The caller's attribution** (a walk up the guest stack to tell PPFX's
  calls from the game's) is made only for the calls that use it, after
  asp's methods and R7's entry points have returned.

The recorder's cost fell by a fifth (23.5% to 18.9% of the game thread).
What remains is mostly the record itself: each of asp's ~250 draws a frame
copies the whole explicit state (1.6 KB) and its constants' delta into a
queue slot, and identifies its programs -- the design's price for records
that carry everything they draw with. The rest of the game thread (81%) is
the game's own code.

## 8. Measurement [M]

Batch 4 (build/r7/batch4_run.sh), three arms under equal conditions: the
production build (build/r6/prodF), R6 on its lean native-play lift
(build/r6/nI, R6's switches) and the R7 native release build (build/r7/rel3).
All: real audio, the scenario's own shader cache, can't-die, normal CPU
placement, comparisons off, the physics load counted in every arm
(DOTHEROES_PHYS_STATS=1). Uncapped = the benchmark mode (no game or vblank
ceiling, flips complete when executed, a mailbox present, no per-frame
readback) with the frame trace and GPU timestamps; three rounds of each,
the arms' order rotated. Nothing else ran meanwhile. Frame times are the
guest's flips inside the drive's segments (tools/r7_measure.py); the game
thread's busy time is the guest frame less the main thread's waits, from the
frame trace (`busy`: the same as R6's walker_breakdown.py where both apply
-- 6.01 and 6.41 ms on the production and R6 runs -- and it needs no walker).

The release measured is build/r7/rel3. The release committed adds the
record slots' buffer trim found by the cycle runs after this batch
(section 4) and a development census; its own A/B against rel3 (section 4)
shows the same rates and tails, so the tables stand for it.

**Combat: the river fight, uncapped** (60 s of frame trace in the fight):

| Arm (3 runs) | fps | p50 ms | p90 | p99 | p99.9 | game thread busy ms | walker ms | GPU ms |
|---|---|---|---|---|---|---|---|---|
| production | 128.4-144.0 | 6.68-7.68 | 7.80-8.46 | 10.56-11.40 | 16.83-17.84 | 6.01-6.05 | 5.20-6.16 | 2.35-2.60 (engine) |
| R6 | 145.3-151.2 | 6.36-6.61 | 7.43-7.93 | 10.13-10.40 | 11.99-12.39 | 6.41-6.75 | 0.32-0.33 | 0.73-0.74 (native) |
| **R7 release** | **152.2-152.8** | 6.30-6.39 | 7.21-7.35 | 9.20-10.14 | 11.49-11.90 | 6.35-6.49 | none | 0.44 (native) |

**Heavy debris** (the fight's frames whose interval had debris physics --
any awake body in a collision task; 33-524 such frames a run, 41-64 bodies
at most -- against the rest of the fight):

| Arm | debris frames: rate | p50 ms | p99 | max | other frames: rate | p99 |
|---|---|---|---|---|---|---|
| production | 110-118/s | 7.96-8.46 | 16.27-18.07 | 17.8-19.2 | 129.5-146.7/s | 10.11-10.37 |
| R6 | 105.6-106.9/s | 9.10-9.39 | 10.48-12.39 | 10.5-15.7 | 149.0-154.6/s | 8.70-8.98 |
| **R7 release** | 110.5-117.1/s | 8.32-9.00 | 11.03-12.14 | 11.3-14.8 | 154.2-156.2/s | 8.41-8.72 |

**Exploration: the fields, uncapped:**

| Arm (3 runs) | fps | p50 ms | p90 | p99 | p99.9 | max | game thread busy ms | GPU ms |
|---|---|---|---|---|---|---|---|---|
| production | **179.7-181.1** | 5.40-5.42 | 6.07-6.19 | 7.88-8.14 | 10.79-14.02 | 108-117 | 5.34-5.39 | 1.72-1.74 (engine) |
| R6 | 165.8-168.3 | 5.86-5.96 | 6.46-6.60 | 7.87-8.22 | 10.36-10.62 | 12.4-97.0 | 5.80-5.86 | - |
| **R7 release** | 168.5-168.7 | 5.79-5.86 | 6.42-6.51 | 7.99-9.93 | 10.31-12.59 | 35.6-40.0 | 5.75-5.89 | 0.48-0.49 (native) |

**Transitions** (the river's route: the walk from the castle across the
area boundary, its load included, 23-24 s):

| Arm | rate | p99 ms | p99.9 | max | > 25 ms | > 50 ms |
|---|---|---|---|---|---|---|
| production | 162.6-172.3/s | 12.35-13.59 | 16.64-22.33 | 91.6-93.2 | 3 | 2-3 |
| R6 | 170.8-174.5/s | 8.44-9.06 | 13.49-25.64 | 92.0-93.3 | 3-4 | 1-2 |
| **R7 release** | 215.3-217.2/s | 7.76-8.13 | 13.06-15.12 | 91.6-94.1 | 3 | 1 |

**Capped (60 fps), the river fight, two rounds:** every arm holds 60.0
flips a second; the window's presents, p99 / p99.9 / over 25 ms: production
21.96-21.98 / 24.15-24.65 / 1 and 4; R6 19.64 / 21.33-21.44 / 0 and 1; R7
18.59-18.74 / 20.72-20.95 / 0 and 1 (one present of 29.5 ms in one run).
The GPU's clocks while R7 runs capped: mostly P5 (the memory clock at 1,516
MHz on average); uncapped, P3 and P0 (5,473 MHz) -- R6's finding.

**Gameplay timing** (tests/fps_check.py, the same inputs): at 60 fps R7 and
production walk exactly the same (5.00 down, 5.00 up), physics steps 60 a
second, frames x1.00. Uncapped R7 runs 159.7 fps standing, 159.9 walking,
144.4 in the grass; physics still 60 steps a second; its walk 4.86 (x0.972),
production's uncapped 5.00 in this batch -- in R6's batch both walked 4.86
uncapped: the walk above 60 fps varies run to run with the frame times, a
property of the game's input timing at high rates, not of either renderer
[I]; the frame counters run x2.66 (production x2.79), as the game's own
frame-counted timers do at any rate above 60 (R6's finding).

**What it shows:**
- In combat R7 is the fastest of the three (152 fps against production's
  128-144 and R6's 145-151), and its tail the shortest (p99.9 11.5-11.9 ms
  against production's 16.8-17.8). Production is bound by its walker (5.2-
  6.2 ms a frame decoding the FIFO); R7 has none.
- In exploration production is still faster (180 fps against R7's 168.6,
  7% ahead): there the walker is not production's limit, and the game
  thread is -- R7's is 0.4-0.5 ms a frame busier (5.75-5.89 against 5.34-
  5.39): the recorder (0.28-0.30 ms a frame by its own timing) and the hooks'
  cost around it. R7's worst exploration frame is 36-40 ms; production's
  108-117.
- R7's game thread is its limit everywhere: 6.35-6.49 ms a combat frame,
  against R6's 6.41-6.75 (the recorder now 0.46 ms a frame, R6's 0.81); the
  native renderer's own thread and the GPU have room (1.3 ms and 0.44 ms a
  frame).
- Debris frames run at about 110 a second in every build, R7's p99 there
  11-12 ms against production's 16-18: the physics, not the graphics, sets
  their pace.
- **144 fps sustained: not reached.** R7 averages 152 in combat and 169
  exploring, but one combat frame in ten takes 7.2-7.4 ms (144 needs every
  frame under 6.94), debris frames run at 110 a second, and exploring's p99
  is 8-10 ms. **200+: not reached** outside a transition's light frames.
  Neither target is claimed from the averages.

## 9. What R7 leaves

R7 removes the PS3 graphics command path from the native release build:
what it leaves is the system API the game needs and what the measurements
and the coverage did not reach.

| Left | Why | Next |
|---|---|---|
| **cellGcmSys's system API** (memory and IO mapping, the display buffers, the flip status and handlers, the label and report areas, the control register) and the ring's memory | the game calls them and reads them (system-API compatibility, kept by the plan) | -- |
| **cellGcmFlush's body** | it writes `put` (no command word); the hook publishes the records there | -- |
| **The 2D transfers other than cellGcmSetTransferImage**, the label helpers, the Unsafe forms, the FIFO reset | never called in any scenario (watched: a call would be named, and its words found by the detector) | replace one if it is ever seen |
| **The post-boss warp** | implemented, never run (no route reaches it) | the owner's optional session (OWNER_PLAY.md, tests/scenarios/owner_warp_r7.drive) |
| **The depth clamp** | the game's frame-start reset sets z min/max control to 0x11 (near/far culling and depth clamping on) every frame; neither the native renderer nor the old engine clamps (both clip): a fidelity gap of both, not an R7 regression | a separate behaviour change, switchable, compared with RPCS3 (device feature depthClamp) |
| **Occlusion against RPCS3** | real counts are the default; matching the previous port proves nothing where it returned incomplete reports | the owner's comparison (R6 checklist item 10) |
| **The entry hooks' form** | the release uses the lift's generic observer call (ppu_entry_observe, dispatching by address: 2% of the game thread), not a direct call per entry point | a direct native hook per entry point (the lifter's entry_hooks form, with a return) if the profile keeps pointing at it |
| **The recorder's development modes** | compiled into the release but inert (R4's verify, R5's probes, R6's verify modes, the capture check); not command path | trim them from the release |
| **The game thread** | R7's limit: 6.35-6.49 ms a combat frame, 5.75-5.89 exploring (production 5.34-5.39 exploring); the record (the whole state copied for each of asp's draws) is most of the recorder's cost | records that share unchanged state; the hooks' direct form |
| **144 fps sustained, 200+** | not reached (section 8) | the game thread above; the debris frames' pace is the physics' |
| **The dual run** | R3-R6's per-draw comparison needs the engine to read the commands R7 no longer writes; its stall (NATIVE_R6.md section 7) is not resolved -- R7 compares instead with the capture check, grabs against R6 and production, and Bink's frames matched by content | the development builds keep it for R6's configurations |

## 10. Reproducing

```
tools/r7_native_lift.sh                 (R7's lift: config/r7_native_hooks.json, no store probes; the development build in build/r7/native/bld)
tools/r7_native_lift.sh build           (build only)
tools/r7_release_build.sh               (the native release build from that lift: build/r7/release/bld)
tools/r7_scene.sh <tag> <scenario> <secs> [NAME=value ...]    (a run into build/r7/<tag>; R7_BIN picks the binary)
  the development build without a walker: DOTHEROES_R7=1 (sets R4-R6's switches)
  the release build: nothing (R7 always)
  the capture check:    DOTHEROES_R7_CAPTURE_CHECK=1
  the target watch:     DOTHEROES_R7_TARGET_WATCH=1
  inputs at consumption (no textures at the flush): DOTHEROES_R7_TEX_AT_FLUSH=0
  the previous port's occlusion results: DOTHEROES_R7_OCCLUSION=legacy
  small vertex arenas (their retirement forced): DOTHEROES_R7_ARENA_MB=<n> with DOTHEROES_R6_ARENAS=<n>
    (and DOTHEROES_PPFX_REALLOC_EVERY=<frames> to retire every image)
  consumption delayed: DOTHEROES_PPFX_DELAY_US=<us a record>, a small queue DOTHEROES_R6_QUEUE_CAP=<records>
  the transfers' results dumped: DOTHEROES_R7_XFER_DUMP=<dir>
  what the recorder and the renderer hold, every 1,800 frames, to stderr: DOTHEROES_R7_CENSUS=1
  benchmark: DOTHEROES_BENCH=1 PS3_FTRACE=70:60:ftrace.txt VK_GPU_TIME=1 DOTHEROES_PHYS_STATS=1
tools/r7_symbols.py <exe> [...]         (the link evidence: command-path function symbols, from the PDB)
tools/r7_grabs.py <run> [<reference run>]               (the grabs: brightness, coverage, difference)
tools/r7_bink_compare.py <run A> <run B> [--same 0.5]   (movie frames matched by content, compared)
tools/r7_measure.py <fight|explore|debris|transitions|busy> <run> ...   (frame times by segment; the debris split; transitions; the game thread's busy time)
```

Switches (all development, all off by default unless said): DOTHEROES_R7=1,
DOTHEROES_R7_CAPTURE_CHECK=1, DOTHEROES_R7_TARGET_WATCH=1,
DOTHEROES_R7_TEX_AT_FLUSH=0 (default on), DOTHEROES_R7_OCCLUSION=legacy,
DOTHEROES_R7_ARENA_MB=<n>, DOTHEROES_R7_XFER_DUMP=<dir>,
DOTHEROES_R7_CENSUS=1; the build options
DOTHEROES_NATIVE_RELEASE (port) and PS3RECOMP_NATIVE_GRAPHICS (runtime).
Engine change: patches/ps3recomp/0116.

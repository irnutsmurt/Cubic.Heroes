# The audio milestone: FMOD's SPU workloads, mapped, and the plan to replace them

The owner's request (2026-11-10): once the non-audio SPU work is covered,
map FMOD's remaining SPU workloads and their dependencies, then replace them
incrementally while preserving music, effects, timing and mixing. Switching
audio libraries is not the plan. The reservation checks stay until every
workload that relies on them is replaced and validated.

Labels: [C] read in the code, [L] seen in run logs (all 245 runs under
build/r8 as of 2026-11-10), [I] inferred, [D] from an earlier document.
Addresses are this game's (BLUS-30490).

## 1. What runs today

FMOD Ex 4 for PS3 is statically linked into the game. Its PPU side is lifted
game code. Its SPU side runs as lifted SPU code on host threads through the
runtime's SPU environment: an lv2 SPU thread group of one thread, not SPURS
[L]. That settles FEASIBILITY.md's "raw SPU or SPURS [unknown]". On the
profile, the FMOD thread is 3.3-3.7% of its wall time [D, NATIVE_R8.md
section 1].

### 1.1 The images (runtime ids 6-26; build/gen/spu/spu_0005..spu_0025)

Each is identified by its `.note.spu_name` [C].

| Id | Image | Lifted functions | Imported in runs [L] |
|---|---|---|---|
| 6 | fmodex_spu.elf, the mixer (entry 0xF0, 72,544 bytes) | 922 | once a run |
| 7 | the NeuralTHX mixer | 472 | never |
| 8 | codec_adpcm | 682 | never |
| 9 | codec_raw | 197 | 16 times a run |
| 10-26 | 17 DSP effects: chorus, compressor, delay, distortion, echo, flange, highpass, itecho, lowpass2, lowpass, lowpass_simple, normalize, oscillator, parameq, pitchshift, sfxreverb (25), tremolo | | sfxreverb once a run; the rest never |

There are 17 DSP images, not 16 as FEASIBILITY.md says. Every image is
registered by two FNV-1a fingerprints (build/gen/spu/spu_workloads.c).
Images 8-26 are also registered as relocatable overlays (link address 0x80)
[C]. Overlay loads seen: image 9 only, at local store 0x156F0 [L]. The log
stops at 64 lines a process, and every run reaches that cap, so a
sfxreverb load would not show. **Whether sfxreverb ever executes is not
known.**

### 1.2 How the PPU starts the SPU side [C, L]

- `func_008D0F6C` imports the mixer from 0x00F96A80 with
  `_sys_spu_image_import`.
- `func_008D0EBC` loads the firmware's flashMP3.pic. Its failure is patched
  (config/insn_patches.json), and MP3 is not used.
- `func_008D045C` creates and starts the group:
  - event queues ×2, a port connected locally;
  - the group "FMOD", one thread at priority 16, then thread_initialize;
  - connect_event spup 9; bind_queue spuq 0x63; group_start; set_spu_cfg 0.
  - group_start's arguments: r3 the cellAudio ring 0x58000000, r4/r5
    FMOD's system object, r6 the 0x80-byte parameter block at G+0x2480
    (G = 0x02C9DD80).
- The NeuralTHX set-up (`func_008CFF28`) and the teardown (`func_008CFCB8`,
  `func_008D0234`) never ran in a test.
- Codec and DSP images are imported by `func_008EC4BC` through
  `func_008CFD48` (three call sites).
- Runtime side: the import HLE is runtime/ppu/ppu_sysprx.cpp:695 (it puts
  segment arrays at 0x0D000000 and ignores the caller's r6 buffer). The
  group syscalls are in syscalls/lv2_register.c, and the thread runs in
  spu/spu_lifted_thread.c.

### 1.3 One audio block [C, L, I]

1. cellAudio's host mix thread (libs/audio/cellAudio.c:566) takes one
   256-sample block from the guest ring at 0x58000000. That ring is 8
   channels of big-endian f32 at 48 kHz, 8 blocks.
2. The host thread downmixes the block to stereo with a fixed matrix
   (cellAudio.c:507-512: centre 0.707, LFE and rear 0.5, side 0.3), clips
   it, and queues it to SDL2 (`SDL_QueueAudio`, at most 4 blocks queued).
   It then advances the read index and pushes cellAudio's notify event.
   **That event is the clock of the whole chain.**
3. On the PPU, "FMOD libAudio event receive thread" (`func_008D1634`):
   - waits on the notify queue;
   - reads the ring's read index;
   - takes two locks and atomically increments a counter (lwarx/stwcx);
   - sends an event on the port connected to the queue the SPU bound
     (spuq 0x63);
   - blocks on its reply queue (spup 9).
4. The SPU mixer (main loop `func_0000E39C`):
   - receives the event (stop 0x110 and the mailboxes);
   - does two atomic decrement-if-nonzero operations on 128-byte lines
     (GETLLAR/PUTLLC, `func_0000E6C8`/`E720`, six call sites; most likely
     the counters the PPU increments [I]);
   - GETs its inputs (sizes 0x4000, 0xC00, 0x80, 0x40, 0x20), runs codec
     and DSP plugins through the overlay mechanism, mixes;
   - PUTs the 0x2000-byte block (256 × 8 × f32) into the ring at block
     index × 0x2000;
   - replies with an event (the interrupt mailbox).
5. A second kick path (`func_00884748`, reply on spup 8) has an unknown
   role.

### 1.4 What the SPU side uses of the runtime [C]

| Service | Where | Used by |
|---|---|---|
| SPU context, local store, channels, mailboxes, stop-and-signal | spu_channels.c, spu_context.h | the mixer, the plugins |
| MFC DMA: synchronous copies, PUT with coherency | spu_dma.h:170-805 | the mixer (GET 16, PUT 5, 14 dynamic sites), sfxreverb, delay, pitchshift, chorus, echo |
| Atomics: GETLLAR/PUTLLC, the one lock-line lock shared with the PPU's stores and stwcx | spu_channels.c:603, spu_coherency.c:43, ppu_loader.cpp, ppu_memory.h:147 | the mixer's counters |
| The SPU reservation test on every PPU store | the lift (NATIVE_R8.md section 5.4) | anything that takes reservations: the mixer here |
| Overlays: relocatable images, residency, `SPU_RELOC` dispatch | spu_channels.c:1888-2273, spu_context.h:299-315, 1039 | the codec and DSP plugins |
| lv2 SPU thread group and event syscalls; sys_event ports and queues | lv2_register.c, sys_event.c | the group, the PPU-SPU events |
| Decrementer (host time at 79.8 MHz) | spu_channels.c:1319 | the mixer, six reads |
| cellAudio ring and notify; the SDL2 backend; the host downmix | libs/audio/cellAudio.c | the output |

### 1.5 Unknowns and risks

- Whether sfxreverb executes; whether the 19 never-imported images can be
  reached (NeuralTHX, ADPCM, 16 DSPs): the call sites of `func_008CFD48`
  and `func_008CFF28` decide it, not the absence of a log line.
- What each field of the parameter block (G+0x2480) and the shared lines
  (G+0x2300..0x3300) means; the role of `func_00884748`.
- One mixer branch, when its mode word is 2 or 6, throws an event to port
  11. Port 11 is never connected, and the runtime would wait 200 ms for it
  each time (lv2_register.c, `SPU_PORT_BIND_WAIT_MS`). It is apparently not
  taken with 8 channels [I].
- The host's fixed 7.1-to-stereo downmix is ours, not FMOD's or the
  console's. "The same mixing" at the device is a separate question from
  the SPU replacement and stays a behaviour change of its own.
- The import HLE's segment region at 0x0D000000: whether it is free guest
  memory is not established.
- No per-image counts for FMOD: the SPU census counts SPURS starts only.

## 2. The plan

Each step is its own change, checked and measured before the next, with
the lifted path kept behind a switch until the step is validated.

| Step | What | Check | Retires |
|---|---|---|---|
| A0 | **Evidence first**: per-image counters (imports, overlay loads and dispatches per image, mixer blocks, time per block, the PPU's wait per block), the overlay log made a count; a capture of N blocks: the mixer's inputs (the event, the parameter block and the lines it reads, the sample data it GETs) and its output block, for offline replay; whether sfxreverb runs | counts and captures from the river, the fields, the menus and the title music | nothing |
| A1 | **codec_raw native** (image 9, 197 functions, the plugin in use): the mixer's indirect call into the overlay goes to native code, bit-exact | a check mode running both into scratch: each decoded buffer equal, over the captured blocks and in the game | nothing yet (one plugin) |
| A2 | **sfxreverb native** (image 25), if A0 shows it executes; else documented as imported but not run | as A1 | the overlay mechanism, once no plugin in use is lifted |
| A3 | **The mixer native** (image 6, 922 functions), bit-exact, as a native job driven by the same event: the PPU's port send wakes it, its reply goes through sys_event's host path, its block goes into the ring | each output block equal to the lifted mixer's (check mode), over captures and in the game: music, effects, menus, the title; the block cadence and the PPU's wait per block measured; then the owner's listening check | for FMOD, the SPU thread group, context, channels, mailboxes, MFC and DMA, the overlay registry, the image-import HLE's role |
| A4 | **The counters' atomics**: the native mixer's decrement-if-nonzero must stay atomic against the PPU's lwarx/stwcx on the same lines: either through the runtime's lock-line lock (as an SPU's PUTLLC is now) or a host CAS the PPU's reservation sees | a stress test of both sides on the lines; the game's audio over long sessions | with A3, the last lifted SPU code that takes reservations (after the water, the inflate, the physics, and the waver): **only then** the SPU reservation test on every store can go (NATIVE_R8.md section 9's decision) |
| A5 | The 19 images no run imports: reachability from the PPU's call sites; any reachable one ported as A1 or kept lifted with a loud diagnostic, never removed on "never seen" | the call-site reading; a forced test where reachable | the rest of the SPU environment, once nothing lifted remains |

What stays after all of them: sys_event (the PPU's own ports and queues),
cellAudio's ring and notify, the host backend. The downmix is reviewed
separately: the console's rule for 7.1 to stereo against ours, as a
behaviour change of its own.

Status: section 1 mapped (2026-11-10). A0: the counters done, the captures not yet (below). A1: ported, checked in the game bit-exact with the clock replayed (rel20), **native by default since
rel21** (below); DOTHEROES_NATIVE_FMOD_CODEC=0 keeps the lifted overlay.

### A1, codec_raw (2026-11-10) [C, M]

What it is [C, port/native/fmod_codec_raw.h]: not a decoder. `read` GETs raw bytes from main memory into its
file's 256-byte buffer and copies them on; 16-bit samples are byte-swapped unless a flag says not to; an
optional key decrypts (bit reversal and XOR). `setposition` seeks (bytes, or position x rate / divisor,
rounded down to the block). Reads from 0xC0000000-0xCF8FFFFF are paced: once over 1 ms has built up
between reads, it spins 17,500 decrementer reads. The mixer loads it (link address 0x80, at LS 0x156F0) and
calls `read` (link 0x490) and `setposition` (0x3D0) through its codec object.

- **The port**: a lane-exact transcription (fmod_codec_raw.cpp); offline (tests/native/fmod_codec_raw_check.cpp,
  the lifted overlay run relocated as the game does): 5,169 calls equal at x86-64-v3 and v2, 44,077 more at
  v3 -- every local-store byte, channel operation and kept register, halts at the same PC.
- **In the game**: the runtime calls a native replacement for a relocatable overlay's function when the
  port registers one (ps3recomp 0136, `spu_overlay_set_native`, on the indirect branch into a verified
  resident overlay); port/native/fmod_codec_native.cpp registers codec_raw's three entries.
  DOTHEROES_NATIVE_FMOD_CODEC=1 runs them natively, =check runs the native call on a copy of the local
  store and registers, then the lifted one, and compares.
- **Check** (rel17c, the river fight and the menus' music): **172,958 and 267,484 calls, all equal** apart
  from the read pacing's clock (the milliseconds and the last decrementer it keeps at delta + 0x2F10 and
  0x2F20, from the host's time, read at another moment by each run: compared apart; 56k and 87k calls).
  Before that was known, the check counted those calls as different (rel17). rel17c's menus report also
  counted one call more than it settled (267,484 checked, 267,483 equal, 0 differing): the count was taken
  before the comparison, and a call still running when the report printed was in it.
- **Check with the clock replayed** (rel20, 2026-11-10): the clock is no longer compared apart. The lifted
  run's decrementer reads are logged (ps3recomp spu_channels.c `spu_dec_log_begin`/`_end`), and when the
  two runs differ the native one runs again from the same state with those reads fed to it in order; the
  whole local store and the registers are compared, the clock included, and each call is counted once,
  when it is decided. **The menus: 267,517 calls, 180,923 equal as run, 86,594 equal with the reads
  replayed, 0 differing. The river fight: 102,959 calls, 69,630 and 33,329, 0 differing.** No call read
  the decrementer a different number of times in the two runs. Every mismatch of the earlier checks is
  thereby explained -- the two runs' different moments of reading the time, and nothing else -- and the
  port is bit-exact on every call these runs made, clock included.
- **Native** (rel17): 171,274 and 266,882 calls, none declined, no halt; the mixer's work a block 128.7 us
  in the river against 125.3 us lifted -- no gain to speak of (it copies memory), and none expected: A1 is
  a step to A3 (the mixer), which needs its plugins native.
- **The validation set with it on** (rel20, DOTHEROES_NATIVE_FMOD_CODEC=1, build/r8/cc20on_run.sh): the eight
  scenes, sync validation (0 messages) and the normal-damage run with no crash -- 1,582,504 native calls,
  none declined, no halt. The reference scenes: 4 of 5 within the accepted figures; entry_hall's floor
  reflection was far outside them once (refl mean 2.56 against 0.18 accepted), with the camera where it
  should be. Run again alone, twice with the codec native and twice lifted: all four within the figures
  as in rel16-rel20. A one-off, not the codec's (which touches no picture); its cause is not known.
- **The default** (rel21): native; =0 the lifted overlay (checked: a river run each way, the quit drive).

### A0, the counters (2026-11-10, rel16) [M]

`lv2_spu_thread_report` (syscalls/lv2_register.c: per SPU thread, the events its receive service delivered by
spuq number, the time it waited for them, the time it ran between them) and `spu_overlay_report`
(spu/spu_channels.c: overlay loads by image), printed beside the SPU census at the test stop:

| Run | Mixer events (spuq 0x63) | Rate | Work between events: mean, max | codec_raw loads | sfxreverb loads |
|---|---|---|---|---|---|
| the river fight (v16river) | 14,832 | 187.5 a second | 146.6 us, 2,124 us | 70,604 (4.8 a block) | 0 |
| the fields, the waver started twice (wv16nf) | 25,634 | 187.5 a second | 162.2 us, 2,556 us | 152,972 (6.0 a block) | 0 |

- One event a 256-sample block at 48 kHz (5.33 ms): the mixer is clocked by cellAudio as section 1.3 reads.
- Its work is 2.8-3.0% of a core on average, with spikes of 2-2.6 ms a block (under half a block's time).
- codec_raw is loaded 5-6 times a block; **sfxreverb is imported but never loaded, so never run, in these
  scenes** -- its port waits for evidence that it runs (step A2).
- **sfxreverb does run, in the dungeons** (rel20's validation scenes, 2026-11-10): overlay loads by image
  -- the first dungeon's approach (v20dungeon) image 9: 103,101, **image 25: 7,102**; the exit after its
  boss (v20exitboss) image 9: 21,314, **image 25: 928**. None in the river, the fields, the menus, the
  editor, the field after the dungeon or the snapshot scene. So the dungeons' sound has reverb, and **A2
  is needed**: sfxreverb ported as codec_raw was, before the mixer (A3) can be native.

# Audio: FMOD Ex today, OpenAL Soft next

**The requirement (the owner, 2026-11-11)**: the PC recompilation's game audio runs on **OpenAL Soft**
(dynamically linked, its LGPL notices shipped); FMOD Ex is not executed in the final game, and no
FMOD-derived implementation code is in this project's public source or in anything it distributes. The plan
and its milestones: docs/OPENAL_MIGRATION_PLAN.md. This replaces the 2026-11-10 direction recorded here
before ("replace FMOD's SPU workloads incrementally; switching audio libraries is not the plan"), which is
**withdrawn**. What stays from it: the reservation checks stay until every workload that relies on them is
replaced and validated (docs/NATIVE_R8.md section 9; the plan's M6).

Labels: [C] read in the code, [L] seen in run logs, [M] measured, [I] inferred, [D] from an earlier
document. Addresses are this game's (BLUS-30490).

## 1. What runs today (2026-11-11, after M1)

FMOD Ex 4 for PS3 is statically linked into the game. All of it runs **as lifted code**: its PPU side as the
rest of the game's lifted code, its SPU side (a mixer program and the codec and DSP plugins it loads) as
lifted SPU code on host threads through the runtime's SPU environment -- an lv2 SPU thread group of one
thread, not SPURS [L]. Its output is cellAudio's 8-channel ring, which the runtime's host thread downmixes to
stereo with a fixed matrix of ours (centre 0.707, LFE and rear 0.5, side 0.3; not FMOD's, not the
console's) and plays through SDL2 [C].

- **The clock** [C, L]: cellAudio's host thread takes one 256-sample block (48 kHz, 5.33 ms) from the ring and
  pushes its notify event; one of FMOD's PPU threads turns that into an event for the SPU mixer, which mixes
  the next block into the ring and replies. 187.5 blocks a second; nothing in the game's frame loop waits on
  it.
- **Cost** [M, rel16 counters]: the mixer's work is 2.8-3.0% of a core on average (146-162 us a block lifted,
  spikes of 2-2.6 ms a block), on a thread that waits the rest; FMOD's thread 3.3-3.7% of its wall time
  [D, NATIVE_R8.md section 1].
- **What runs on the SPU side** [L]: the mixer once a run; the raw-PCM codec plugin 5-6 times a block; the
  SFX reverb plugin **only in the dungeons** (the first dungeon's approach: 7,102 loads; the exit after its
  boss: 928; none in the river, the fields, the menus, the editor); the other 19 images (another mixer, an
  ADPCM codec, 16 DSP effects) are registered but never imported in any run.
- **What the SPU side uses of the runtime** [C] (what M6 can retire once OpenAL Soft runs and FMOD does not):

| Service | Where | Used by |
|---|---|---|
| SPU context, local store, channels, mailboxes, stop-and-signal | spu_channels.c, spu_context.h | the mixer, the plugins |
| MFC DMA (synchronous copies, PUT with coherency) | spu_dma.h | the mixer and the reverb |
| Atomics: GETLLAR/PUTLLC and the lock-line lock shared with the PPU's stores and stwcx | spu_channels.c, spu_coherency.c, ppu_loader.cpp, ppu_memory.h | two counters the mixer shares with an FMOD PPU thread |
| The SPU reservation test on every PPU store | the lift (NATIVE_R8.md section 5.4) | anything that takes reservations: the mixer is the last |
| Overlays: relocatable images, residency, `SPU_RELOC` dispatch, the native-overlay hook (ps3recomp 0136) | spu_channels.c, spu_context.h | the codec and DSP plugins |
| Native thread programs (ps3recomp 0142) | spu_lifted_thread.c | none by default since M1 |
| lv2 SPU thread group and event syscalls; sys_event ports and queues | lv2_register.c, sys_event.c | the group, the PPU-SPU events |
| Decrementer (host time at 79.8 MHz) | spu_channels.c | the mixer |
| cellAudio ring and notify; the SDL2 backend; the host downmix | libs/audio/cellAudio.c | the output (only FMOD opens cellAudio) |

## 2. The game's own audio layer, and where OpenAL Soft goes in

Everything the game does with sound goes through one class of its own, its sound manager (26 methods); no
other game code calls FMOD. That class is the boundary: entry hooks on its methods (config/audio_hooks.json,
port/audio_hooks.cpp) lead to an independently written native sound manager on OpenAL Soft, and FMOD is never
initialised. The interface, the behaviours to reproduce, the sound list and files (reconciled: 324 entries,
322 files named, 326 on the disc, 42 with loop points), the reverb's parameters and the validation are in
docs/OPENAL_MIGRATION_PLAN.md.

- **M0 (2026-11-11)**: the hooks trace every call (DOTHEROES_AUDIO_TRACE=<file>: each sound-manager call and,
  for the baseline only, each FMOD API call it makes, with arguments and results; inert unless set) and
  cellAudio writes the final mix (AUDIO_WAV=<file>: f32 stereo at 48 kHz). rel27's baseline is captured scene
  by scene with them (the plan's section 4); the captures are kept locally, outside the repository.
- **M2 (2026-11-11)**: OpenAL Soft 1.25.2 built (Windows, Linux) and shipped beside the game with its licences
  and source, without its HRTF data; the audio layer on it (port/native/audio: the WAV reader with the `smpl`
  loops, the device with its loss and reopen, the music streamer with fades, the reverb, the sound list),
  tested alone (tests/native/openal_check.sh; the plan's section 4.3). The game does not use it yet.

## 3. A record: FMOD's SPU programs as native code (A0-A3, 2026-11-10/11), withdrawn

Before the requirement above, FMOD's three SPU programs in use were translated into native code mechanically
from their lifts, each **bit-exact with the lifted program**, and made the default one at a time:

| Step | What | Checked | Default from |
|---|---|---|---|
| A1 | the raw-PCM codec plugin (197 lifted functions) | offline 5,169 + 44,077 calls equal; in the game 267,517 (the menus) and 102,959 (the river) calls equal, the clock replayed | rel21 |
| A2 | the SFX reverb plugin (158) | offline 5,235 + 54,774 calls equal; in the game 17,302 (the first dungeon) and 1,886 (the exit after the boss) calls equal, every local-store byte, register and PUT | rel23 |
| A3 | the mixer (922) | the game's own channel traces replayed (5,189, 5,375, 24,070 and 24,445 blocks: every record and every block boundary equal) and 77,855 synthesized cases | rel27 |

- **Their cost** [M]: the codec no faster (it copies memory); the mixer, after a liveness pass in the
  generator, 9.6-12% less work a block than the lift in the game (the river 111-115 us against 123-128 us; the
  first dungeon 255 against 290 us).
- **Withdrawn from the public tree on 2026-11-11** (M1): the translated code, its generators, templates and
  checks are FMOD-derived and may not be distributed. The game is back on the lifted FMOD path (as before A1)
  until OpenAL Soft replaces FMOD altogether (the plan's M5); rel27, the last build with them, is kept locally
  as the compatibility reference with its source, its captures and its measurements.
- **What stays in the repository**: the generic pieces written for that work, which hold nothing of FMOD --
  the runtime's native-overlay and native-thread hooks (ps3recomp 0136, 0142), the SPU channel trace (0140,
  0141; tools/spu_trace.py), the decrementer log (0138), the SPU thread group's stop on terminate (0137) and
  the generic SPU-to-native generator (tools/gen_spu_native.py), which now generates the water's and the
  waver's tasks from the player's own lift (docs/SPU_NON_AUDIO.md).
- **The captures (A0)**: channel traces of the mixer thread (SPU_TRACE_IMAGE=<runtime image id>,
  SPU_TRACE_FILE, SPU_TRACE_MB: its registers and local store at its start, then every channel read and
  write, every MFC command with the bytes it moved, every stop), each up to 1,500 MB -- the river fight
  (5,189 blocks) and the first dungeon's approach (5,375); kept locally, not in the repository. A block:
  one event receive and five inbound-mailbox reads, two GETLLAR and one PUTLLC, the reply, about 63 GETs and
  48 PUTs, about 70 decrementer reads.

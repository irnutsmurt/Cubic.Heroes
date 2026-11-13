# Audio: FMOD Ex out, OpenAL Soft in -- audit and migration plan (for review)

Status: **approved by the owner 2026-11-11 with the adjustments below; M0-M3 done (sections 4.1-4.4); M4 accepted by
the owner 2026-11-12 (sections 4.5, 4.6); M5 done (section 9.1); M6 begun (section 10); M7 (history) needs a separate
approval; M8 (Bink: docs/FFMPEG_MIGRATION_PLAN.md) after M6.** Requirement (the owner, 2026-11-11): the PC recompilation's game audio runs on OpenAL Soft; FMOD Ex is not executed and no
FMOD-derived implementation code is in the public source or any distributed artifact. The FMOD work so far
(docs/AUDIO.md A0-A3) stays a local compatibility reference only. Tags: [C] read in the code, [M] measured,
[L] run logs, [I] inferred.

**The owner's adjustments (2026-11-11)**, binding on everything below:
1. rel27 stays locally as the known-good reference, with its audio captures, performance results and the
   behavioural documentation; technical findings are not discarded unnecessarily.
2. Water and waver: their native implementations are **generated locally from the player's own game files**
   (as the lift is), not distributed as translated game code; native compilation stays, not SPU emulation.
3. Audio fidelity is validated explicitly: positional stereo, attenuation, looping, fade timing, reverb, voice
   limits, music transitions. The WAV and sound-list counts are reconciled before implementation (section 1:
   done).
4. OpenAL Soft's licence is LGPL-2.0-or-later (GNU Library GPL version 2 or any later version); the licences of
   the selected build and of everything it bundles are verified.
5. CPU savings are hypotheses until measured against rel27.
6. Cleanup: the FMOD-derived implementation leaves the public tree, and the game-derived water/waver
   transcriptions once local generation works; independently authored compatibility code and factual test
   documentation stay where appropriate.
7. History: the owner favours the bounded rewrite but has **not** authorized any rewrite or force push; the
   exact cleanup plan is prepared for separate approval once the source cleanup is complete.

## 1. The interception boundary: the game's own SoundManager

**Everything the game does with sound goes through one C++ class**, the game's `SoundManager` (one source
file, `soundManager.cpp`; code 0x0046596C-0x00473400) [C]:
- 72 call sites from that class reach 28 FMOD API functions; no other game code calls FMOD (no game TOC
  references an FMOD descriptor) [C].
- All 547 `SoundManager::instance()` sites go straight into a SoundManager method; nothing outside the class
  reads its fields (checked at every site, heuristically) [C].
- FMOD itself: 2,793 lifted PPU functions at 0x0082F650-0x00908584 (+ ~30 at 0xA42E10-0xA43800), its five PPU
  threads, cellAudio (only FMOD opens it), the SPU group, the codec and DSP images [C, L].

**Proposed boundary: a native SoundManager.** Entry hooks (config/r8_hooks.json) on the class's methods send
every call to an independently written native class on OpenAL Soft. `init` never creates an FMOD system, so
none of FMOD runs: no FMOD thread, no cellAudio port, no SPU group. The interface (26 methods) [C]:

| Method (guest) | What the game asks | Sites |
|---|---|---|
| playSE 0x46E0EC | (id, priority, position or none) -> a channel handle | ~420 |
| isPlaying 0x465AC0 | (id) -> bool | 37 |
| playBGM 0x46F4BC / fadeOutBGM 0x46638C / isBGMActive / currentBGMId | music: start (loop or once), fade out, query | 12 / 20 / 6 / 2 |
| playSEFadeIn, fadeOutSE, stopSE, stopChannel, fadeOutLoopSEs, pauseById | effect control | 4 / 2 / 2 / 3 / 6 / 2 |
| duckBGM, unduckBGM, duckSE, unduckSE | menu ducking | 2 / 2 / 1 / 1 |
| applyVolumeOptions, setBGMMaster, setSoundVolume | the options' BGM and SE levels | 10 / 2 / 1 |
| setListener 0x465D9C | the listener's position, every frame | 1 |
| setReverb 0x465D60 | the dungeon reverb on or off (set at map set-up) | 5 |
| init, loadSounds, preload, isLoaded, update(dt), shutdown | lifecycle | 1 each |

Why this and not FMOD's API: it is the game's own vocabulary (ids from its sound list, BGM vs SE, fades,
ducking), it needs no FMOD handles, return codes or callbacks, and nothing of FMOD's design carries over.
The alternative -- an OpenAL shim for the 28 FMOD functions at their call stubs (0x987478-0x987658) -- keeps
the game's lifted class exact but recreates part of FMOD's API (opaque handles, error codes, the asynchronous
open callback, getName), which the owner asked not to do; it stays only a fallback if the class proves
entangled.

**The sound list and the files, reconciled (2026-11-11, tools/sound_inventory.py on the player's files)** [M]:
`sound.lua.eu` has 324 entries {file, volume, 3D, min, max}, ids in list order: **ids 0-41 music** (42 entries;
**id 32 is an empty entry** inside the music section, so 41 files), **ids 42-322 effects** (281 files), **id
323 the empty "no sound"**. 322 distinct files are named, none twice, none missing; the disc has **326** .wav
files, the other four named by no id (038_guard_success, 085_boss_attack14, 086_boss_attack15,
bgm_staff_long). So 326 = 322 listed + 4 unlisted, and 324 = 322 named + 2 empty. All are RIFF PCM 16-bit:
effects 278 mono 44.1 kHz + 3 stereo 44.1 kHz (ids 143, 172, 203, all 2D), 33.7 MB of samples; music 36 stereo
44.1 kHz + 4 stereo 48 kHz (boss01, gameover, cave, editor) + 1 mono (spelunker_theme), 589 MB. 238 named
entries are 3D, 84 2D. **Loop points**: 42 files carry a `smpl` chunk with one forward loop -- 27 music tracks
(six with an intro before the loop start: field07, race, spelunker_field, deepforest, defence, arkanoid;
spelunker_field and cave end their loop before the file ends) and 15 effects (the surround ambiences -- river,
sandstorm, magma, birds, altar --, sliding, sea, water, quake and cheer with a loop start inside the file,
and others). The earlier "no loop markers" was wrong. Whether rel27 honours these loops (the game asks no
loop mode for effects) is established by M0's captures, not assumed; the loader reads them either way.

**What the native SoundManager must reproduce (behaviour, written fresh -- not a translation of the game's
code)** [C, meanings I]: the sound list above, read from the player's own files; one instance per id (playSE stops the same
id first); the fade engine's states (SE, BGM, BGM duck/restore, looping-SE fade-in/duck/pause, fade-out then
stop); the music start sequence (open, ready, play; loop count; crossfade with two streams open); the volume
model (BGM = list volume x BGM master x option level x 0.11 x fade; SE = list volume x option level x 0.11;
linear, clamped 0-1); pause-all from the game's flag; handles that are non-zero, 0 on failure, and safe when
stale (three callers keep a playSE handle to stop it later); `isPlaying` timing (37 game sites poll it; M3's
reading corrected the earlier "FMOD's state advanced at the game's 10 Hz update": a sound's end shows within a
frame of the file's end -- the 10 Hz throttle only paces FMOD's own update) [M, T].

## 2. What the migration eliminates

- **FMOD entirely at run time**: its PPU code (never called), its five threads (BGM status query, libAudio
  event receive, stream, nonblocking, file), cellAudio and the host's 7.1-to-stereo downmix, the SPU group,
  mixer, codec_raw, sfxreverb, the event ports, the shared lock-line counters, the flashMP3 patch.
- **The FMOD-derived code in the repository** (section 5): the native codec, reverb and mixer, their
  generators, templates and checkers, and their glue. They are not needed by the OpenAL path at all.
- **In the player's locally generated build**: today every FMOD routine is lifted and compiled (2,793 PPU
  functions, 21 SPU images). After the switch, the FMOD range is lifted as traps (a call reports and stops:
  proof that nothing reaches it) and the FMOD SPU images are not lifted -- the generated output then holds no
  FMOD routine to execute. (The traps are kept out of the shipped default only once the scenes have shown
  none is reached.)

## 3. What needs an independent implementation (OpenAL Soft, dynamically linked)

| Behaviour | Approach |
|---|---|
| Device, context, failure, shutdown | ALC device open on the default output (stereo, 5.1, 7.1 as the system reports; OpenAL Soft's own mixing and downmix); disconnect detection (ALC_EXT_disconnect) and reopen (ALC_SOFT_reopen_device), retried every second, on the audio thread (the game's thread makes no ALC call: a reopen holds OpenAL Soft's device lock); a source a loss stopped is never taken as ended (a poll after it settles it, M4), the effects restarted where their time is, the music where it stopped, a sound paused through a loss resumed where it was paused; clean release of sources, buffers, effects at shutdown |
| Effects (SE) | the 281 effect files resident as AL buffers (33.7 MB): our own RIFF/PCM loader (all files are 16-bit PCM, mono or stereo, 44.1 or 48 kHz; no other codec), reading the `smpl` loop points (AL_SOFT_loop_points) [M] |
| Music (BGM) | our own streaming thread: buffer queue (e.g. 4 x 250 ms) from the WAV file on disk; the loop as rel27 plays it (the `smpl` loop with its intro, or the whole file: M0 decides), crossfades with two streams; underrun detection and counters |
| 3D | AL_INVERSE_DISTANCE_CLAMPED with reference = the list's min, max = its max, rolloff 1 (FMOD's default inverse rolloff with the game's set3DSettings(1,1,1)); listener from setListener, forward (0,0,1), up (0,1,0), never turning. The game's frame is FMOD's default left-handed one (System::init flags 0: +x is the right), OpenAL's right-handed: x is mirrored (taken as written, the plan would have swapped left and right). A 3D effect played with no position sounds at the listener (measured in rel27's mix: loud and centred), not at the world origin [C, M] |
| Reverb | an EFX EAX reverb in one auxiliary slot, every source sending to it, the slot enabled by setReverb: the game's settings are FMOD's default SFX reverb with Room -100 mB, applied to the whole mix, music included [C] -- in I3DL2 terms gain 0.89, HF gain 1.0, decay 1.0 s, HF ratio 0.5, reflections off (-10000 mB), reflections delay 0.02 s, late gain 1.0, late delay 0.04 s, diffusion 1.0, density 1.0, HF reference 5 kHz, LF reference 250 Hz, dry full. EFX implements I3DL2's model itself; we set the parameters, nothing of FMOD's reverb code |
| Voices | FMOD's default 32 real voices for the effects, given by priority, then audibility (the gain times the distance attenuation from the listener); the rest virtual (their time and handles kept, isPlaying answering), re-ranked at each update (an equal-priority channel takes a real one's voice when 1.6 dB more audible); 100 channels (the game's System::init(100)): at the limit the least important stops if the new one is at least as important, else it is refused. The music has its own streams outside the 32 -- in rel27 it shared FMOD's 32 at the same priority (128) -- so effects never take its voice (M4) |
| Pitch | not used by the game (no frequency or pan calls) [C] |
| Pause/resume, looping | AL source pause/play; the file's `smpl` loop (or the whole buffer) for looping SE; music loop by the stream. The class's own pause (its flag set by the PS3's system overlay, sysutil 0x131) holds every sound and the class's clock; the PC port never sends that event, and the game's START menu does not pause the sound (rel27's traces: the music and ambience go on, only the menu's own sounds play) |
| Timing | a sound's end reported within a frame, as FMOD reported it (M3); the music's open answered asynchronously, as FMOD's callback was; no audio clock drives the game |

Decoding libraries: none needed (PCM only). OpenAL Soft (**LGPL-2.0-or-later**) is fetched from its official
source at a pinned release, its archive verified (SHA-256 recorded), and built by tools/build_deps.py as a DLL
beside the game, with its licence, the licences of the code it bundles (checked in the release's own tree),
the source location and the version recorded (download approved by the owner 2026-11-11).

## 4. Baseline, validation and listening

- **Baseline (rel27, kept locally as the reference)**: the performance figures already recorded (NATIVE_R8,
  AUDIO A3) plus, per scene, (a) a trace of the game's SoundManager calls and of the FMOD calls they make
  (instrumentation hooks only: ids, positions, volumes per frame), and (b) the game's audio output captured as
  a WAV (the cellAudio ring, before the downmix and after it).
- **Scenes**: the title and menus, the overworld, the river fight, the first dungeon (reverb), the boss fight
  and its transitions, save/load and quitting, and a 30-minute session.
- **Automated gates**: the native SoundManager given the same calls produces the same events (play, stop,
  pause) at the same frames and the same per-voice volume curves (within float tolerance); no FMOD routine
  reached (the traps); no underrun, no duplicate start, no leaked source or buffer (counters at exit); output
  compared with the rel27 capture by event onset times, loudness per event (within 1 dB), stereo balance, and
  the reverb's decay in the dungeon.
- **Listening**: the owner's sessions on each scene against rel27 (A/B), the differences noted.
- **Fidelity checks, each explicit** (the owner's list): positional stereo (left/right balance of 3D effects
  against the listener, including the 340 position-less 3D calls), attenuation (gain against distance,
  between min and max and beyond), looping (the `smpl` loops, intros, loop ends inside the file, looping
  ambiences, play-once music), fade timing (BGM fade-out, crossfades, duck/unduck, SE fades: durations and
  curves against rel27's per-frame volumes), reverb (on and off at the right map set-ups, its decay and
  level), voice limits (the cap, which voice yields in crowded moments), music transitions (area changes,
  boss start and end, menus, game over, title).
- **Expected, intended differences** (recorded as such): the output is OpenAL Soft's own panning and mix
  (FMOD mixed 7.1 and the host downmixed it with a fixed matrix -- that matrix goes); resampling 44.1 -> 48 kHz
  by OpenAL Soft's resampler; EFX's reverb is a different implementation of the same I3DL2 parameters, so its
  tail will not be sample-identical; voice stealing order may differ in crowded moments.

### 4.1 M0, the rel27 baseline (2026-11-11) [M]

**How**: m0t = rel27's source with the boundary's hooks (config/audio_hooks.json, port/audio_hooks.cpp: the 26
sound-manager methods, the open callback and, for the baseline only, the 28 FMOD API functions they call;
inert unless DOTHEROES_AUDIO_TRACE is set). Each traced call is the lifted function itself, run from its hook,
its arguments and results recorded (tools/audio_trace.py reads them). The mix: cellAudio's AUDIO_WAV (the final
stereo, f32, 48 kHz; tools/audio_mix.py). The hooks change nothing audible: rel27 itself and m0t on the river
give the same levels and balance, 10 s by 10 s, within the runs' own variation. The captures (about 1.3 GB)
are kept locally with rel27, outside the repository.

| Scene (drive) | Length | Sound-manager / FMOD calls | Effects played (ids) | Most channels at once | Music | Reverb |
|---|---|---|---|---|---|---|
| title and menus (menus_r3) | 197 s | 32,912 / 67,156 | 151 (9) | 6 | title, menu, field01, back to the title | off |
| overworld (fields_beyond) | 161 s | 46,646 / 79,069 | 413 (9) | 6 | title, menu, field01 | off |
| river fight (river_fight) | 152 s | 41,399 / 61,329 | 180 (18) | 6 | title, menu, field01 | off |
| first dungeon, approach (dungeon1_approach) | 147 s | 27,355 / 46,456 | 159 (20) | 6 | field08, field01, dungeon01 | on in the dungeon |
| first dungeon, deeper (dungeon1_deeper) | 202 s | 31,203 / 39,531 | 190 (23) | 6 | dungeon01 | on |
| out of the dungeon after the boss (dungeon1_exit_after_boss) | 54 s | 5,681 / 9,829 | 19 (7) | 4 | dungeon01, field01 | on, then off |
| save inside the dungeon, continue (dungeon1_save_continue) | 175 s | 30,904 / 51,629 | 212 (20) | 6 | ... dungeon01, title, menu, dungeon01 | on/off with the areas |
| the save-data list: load, save, load (save_picker) | 106 s | traced | -- | 5 | title, menu, field01 | off |
| a pause, a reload, the game's own quit (river_water) | 600 s | traced | -- | -- | -- | -- |
| a long session at the river (long_session) | 1,804 s | 1,406,137 calls | 124 channels | 5 | field01, one stream looped 1,766 s | off |

Every run: no crash, cellAudio 187.4-187.6 blocks a second, **0 dropped frames, 0 underruns** (the long session
included). Not captured: **the boss fight itself** -- no automated route reaches it (the boss drive stops at the
special key's chest); it needs the owner's session (M4), as does a crowded scene for the voice cap.

What the baseline shows (the behaviour the native sound manager has to match):
- **The volume model** holds as read: the title's music 0.132 = 0.4 (its list volume) x 3 (the music level)
  x 0.11; the menu's 0.297 = 0.9 x 3 x 0.11; field01 0.132. The game sets each channel's volume every frame
  (applyVolumeOptions and setSoundVolume run every frame in the field).
- **Music transitions**: playBGM opens the new stream at once and fades the old one out over the fadeOutBGM
  time (1 s title to menu, 2 s menu to field and between areas), the old channel stopped when its fade ends;
  the stream's open callback arrives within a millisecond.
- **Reverb**: on (FMOD's bypass off) exactly when the dungeon's music starts at the map's set-up, off at every
  area change outside; set before the new music.
- **Looping**: the river's and the birds' ambiences (098_surround_river 4.34 s, 102_surround_bird 8.86 s, both
  with loop points over the whole file) are started once and **play continuously -- 1,749 s in the long
  session** -- though the game asks no loop mode for effects: rel27 loops the files that carry loop points.
  Field01 (loop points over the whole file) looped on one stream for 29 minutes. What rel27 does with an intro
  before the loop, a loop ending before the file, or a play-once of a looped file: the sound lab (below).
- **Voices**: at most 4-6 channels at once in every scene: the 32-voice cap was never reached.
- **Position**: 3D effects with a position are a minority in menus (52 of 151) and the majority in the fields
  (379 of 413); the listener is set every frame from the hero, forward (0,0,1), up (0,1,0).
- **217_enemy_appear** (resolved in M3): the channel that answered "playing" for 8.55 s was the third of three
  plays of the id in one frame (the class stops the id's earlier channels first); it was never paused, looped
  (no loop points, no loop mode) or played again, FMOD's update ran throughout, and the class's code holds
  nothing that could keep it alive: FMOD's own end reporting missed it. The game polled no other answer of that
  id meanwhile (no effect on play). The native manager ends a channel at its file's end and does not reproduce
  it; whether it was audible that long is not established (the dungeon's music masks it in the mix).

**The sound lab** (tests/scenarios/sound_lab.drive, DOTHEROES_DEV_SOUND in port/audio_hooks.cpp): the hero
standing still while the game's own sound manager is asked for the sounds the scenes never play -- effects
whose loops start inside the file, a looped music track with an intro, one whose loop ends before the file, a
play-once -- traced and captured as the scenes are, on the lifted FMOD path (bit-exact with rel27's). Run
with M1's build (lab29, 461 s; tools/audio_mix.py loop: the mix after the first pass against the file at its
loop start and at its start, 10 ms envelopes):
- **Effects loop from their loop start**, however the game asks (it asks no loop mode for effects): 262_quake
  (2.36 s, loop 0.12-2.36 s) and 269_cheer (6.35 s, loop 0.48-6.35 s) carry on from the loop start, not the
  file's start (within 0.05 s), until the game's stopSE (12 s and 17 s); 061_sliding (loop over the whole file)
  the same (27 s). 001_kettei (no loop points) plays once and ends.
- **Music: intro, then the loop**: bgm_field07 (120 s, loop 8.01-120.00 s) goes back to 8.01 s after its first
  pass (146 s played); bgm_cave (61.94 s, loop ending at 59.99 s) jumps back at the loop end, not the file's
  end (81 s played).
- **A play-once request still loops a looped file**: playBGM(26, once) opens the stream without the loop flag
  (createStream mode 0x10000, against 0x10002 when looped), and bgm_title still plays its loop again from the
  start (59 s played, 21.33 s long). The game's own scenes never ask for music once; the native sound manager
  keeps rel27's behaviour (the file's loop points decide).
- The level and balance (-28 to -35 dBFS, balance within 1.2 dB) and the stops and fades as asked; no
  dropped frame, no underrun.

### 4.2 M1, the public tree (2026-11-11) [M]

**Done**: FMOD's codec, reverb and mixer ports, their glue, generators, templates, configuration and checkers
removed (19 files); the game runs FMOD's lifted SPU programs again until M5. The water's and the waver's hand
transcriptions replaced by code generated on the player's machine from the player's lift
(tools/gen_spu_native.py, tools/spu_native_task.cpp.in, tools/spu_native/; docs/SPU_NON_AUDIO.md). Notices,
README, the architecture requirements (section 8) and the licensing audit (docs/LICENSE_AUDIT.md,
tools/license_audit.py, which fails if a removed path is tracked again) updated.

**The gate (rel29)**: no FMOD-derived object in the build; memtest 695 of 695. Offline, at x86-64-v3 and v2,
the generated tasks equal the lifted ones (the water's 300-step evolution; the waver's 600 frames and 1,049
runs). The validation scenes, sync validation (0 messages), the damage run, the quit drive and the save-data
screen's four flows with no crash; the reference scenes 5 of 5; in the game's check modes the water 2,475 of
2,475 steps equal and the waver 1,085 of 1,092 frames (the others the game's own races at a ripple's start and
end, as with the hand port); the sound lab as section 4.1; the pipeline's unit tests; the player's install test
(a fresh setup generates both tasks; update, relift, settings, a river fight from the install).

**Its cost against rel27** (uncapped, real audio, three rounds rotated per arm; r27fl = rel27 with FMOD's
mixer, codec and reverb lifted, as rel29 runs them):

| Scene | rel27 | r27fl | rel29 |
|---|---|---|---|
| river fight: fps | 204.6-210.0 | 207.6-214.2 | 197.6-207.2 |
| river fight: p99 / frames over 6.94 ms | 6.29-7.51 ms / 0.3-1.8% | 6.18-6.70 ms / 0.2-0.6% | 6.43-7.86 ms / 0.4-2.4% |
| fields, 4 ripples forced: fps | 246.4-247.9 | 245.6-248.8 | 238.1-239.9 |
| fields: p99 / frames over 6.94 ms | 5.32-5.53 ms / 0.2% | 5.38-5.46 ms / 0.1-0.2% | 7.42-7.43 ms / 2.7-2.8% |

- **FMOD's native ports cost nothing measurable when gone**: r27fl is within rel27's range in both scenes (the
  ports saved about 10 microseconds a mixer block, on FMOD's own threads).
- **The cost is the generated tasks**: in the fields (no water there) rel29 loses 3% and its p99 rises by
  1.9 ms -- the frames while a ripple runs, where the game's frame waits for the waver's task (4.58 ms against
  the hand port's 2.63; 0.90 ms of waiting a frame). At the river (no ripple) rel29's mean is 2% under
  rel27's and 4% under r27fl's, the game thread's busy time 4.77-5.00 ms against 4.71-4.83, within the
  scene's spread: the water's step (2.85 ms against 0.78) runs on its worker thread, which the game does not
  wait for, but takes a share of a core.
- Every run of every arm far above 60 fps (the lowest p99.9 11.2 ms). What is lost is headroom above 144 fps
  while a ripple runs. Recovering it is the generic generator's work (a follow-up, not M2-M4's): the
  waver's time is mostly the SPU code's own form (the profile in docs/SPU_NON_AUDIO.md, "Generated on the
  player's machine"), 13% the float helpers' exact slow path.

### 4.3 M2, OpenAL Soft and the audio layer alone (2026-11-11) [M]

**The library**: OpenAL Soft 1.25.2, the release archive from openal-soft.org (sha256 1dbaac44...1089, pinned
in config/deps.sha256; the same tree as the project's tag 1.25.2 on GitHub, checked file by file), built by
tools/build_deps.py with the bundled Zig as a shared library for both platforms -- OpenAL32.dll (Windows: WASAPI,
DirectSound, WinMM; imports only system DLLs) and libopenal.so.1 (Linux: the SDL2 backend, through the SDL2 the
game ships, plus PipeWire, PulseAudio and ALSA when their headers are on the build host -- this Windows host has
none, so the Linux build here has SDL2 and the wave writer). No utilities, examples, configuration, presets,
legacy EAX API or HRTF data. The game loads it at run time from its own folder (dynamic linking, LGPL); the
prebuilt folder, the player's install and the setup's download carry it with its notice
(OpenAL-Soft-NOTICE.txt), its licences (LGPL-2.0, BSD-3-Clause, PFFFT, {fmt}, GSL) and, in the download, its
source (licenses/source/openal-soft-1.25.2.tar.xz).

**The HRTF data set** (the question left open in M2's draft): OpenAL Soft's default HRTF is derived from MIT's
KEMAR measurements, whose terms (MIT Media Lab, 1994) allow use with a citation and say nothing about
redistribution. Rather than ship it on an unclear basis, the build neither embeds nor installs it
(ALSOFT_EMBED_HRTF_DATA=OFF; checked: none of its bytes are in the DLL) and the shipped source leaves the file
out (the release archive less hrtf/Default HRTF.mhr, which the build never reads). Nothing is lost: the game's
sound never had HRTF (FMOD mixed for speakers), and the device is opened with HRTF off.

**The layer** (port/native/audio, in the native library; independent of the game):
- wav_pcm: our RIFF/WAVE reader -- PCM (and extensible PCM) 8/16-bit, mono/stereo, any rate; the `smpl` loop
  (intro, loop end before the file's end); a loop outside the data dropped, not fatal.
- al_api: the library loaded by full path beside the game (no other OpenAL on the system is picked up); its
  absence reported, the game going on without sound.
- al_device: the device (default output, HRTF off, one auxiliary send); its health from the audio thread --
  ALC_CONNECTED, and the default output changing (ALC_SOFT_system_events: WASAPI reports it but does not move
  the output itself) -- reopened in place (ALC_SOFT_reopen_device: context, sources, buffers survive), retried
  every second; every source, buffer, effect and slot counted, a leak reported at shutdown; the sound files as
  buffers with their loop points (AL_SOFT_loop_points); one EFX reverb (EAX reverb when present) in one slot.
- al_stream: music on one audio thread -- 4 x 250 ms of buffers ahead read from the file, the loop region (the
  intro once, then the loop; or the whole file; or once), linear fades and crossfades, underruns (restarted and
  counted) and near-underruns, the device's recovery (streams restarted); the game's thread only queues
  commands in a bounded queue (1,024: a full queue refuses and counts, never blocks) and reads statuses;
  handles go stale when a stream ends; shutdown releases everything and ends the thread. Stereo music plays
  its channels straight to the output (AL_SOFT_direct_channels), as FMOD's 2D music did.
- sound_list: the player's sound.lua.eu read independently (324 entries, ids 32 and 323 empty, 238 3D,
  BGM_FadeTime 2.0: as tools/sound_inventory.py).

**The gate** (tests/native/openal_check.sh; tests/native/openal_check.cpp, no game running):
- every sound file: 326 of 326 load (282 mono, 44 stereo, 669 MB of samples), the 42 loops all set as loop
  points, no object left;
- every looped file played looped and compared **sample by sample** with the file at its own rate (nothing
  resampled), over the intro and two passes of the loop: 42 of 42 exact (0 frames off) -- the 15 effects
  resident, the 27 music tracks streamed, among them the intros (field07 8.01 s, arkanoid 15.69 s, deepforest,
  defence, race, spelunker_field) and the loops ending before the file's end (cave at 59.99 of 61.94 s,
  spelunker_field at 77.28 of 80.98 s);
- made-up files: the loop forms resident and streamed (intro and loop, loop end before the file's end, mono
  effect loop, whole file, once, once of a looped file) exact; a 2 s crossfade's two gain curves within 0.5%
  of full scale of linear ramps, the outgoing stream released at its end; positional stereo (ahead balanced,
  left and right) and attenuation (inverse distance clamped, reference 2, maximum 30) within 0.001 dB of
  2/(2+(d-2)) from 1 to 100; the EFX reverb: no tail when off, a tail decaying at 63 dB/s (decay time 0.95 s
  for 1.0 s set) when on; the command queue's bound (41 commands into 16 places: 25 refused, counted, none
  blocking; stale handles ignored); 100 streamer lifetimes with their threads, 300 samples, a missing file, a
  ninth stream refused, 20 device opens: no object left, nothing hung;
- real time: an hour of music (bgm_field07, its intro and 32 passes of its loop) streamed in real time through the audio thread on OpenAL Soft's wave writer (paced as a device, silent), alongside game runs and builds, with a disconnect simulated at 30 minutes: 0 underruns, 0 near-underruns, the stream restarted after the reopen, the audio thread's worst period 16.5 ms of 231,551 (the open and the reopen; 4 ms otherwise) against 1 s of audio queued, nothing left at shutdown; a minute with a device loss half way (the sources stopped as a disconnect stops
  them, the device reopened, the stream restarted: no underrun); on a real output device (WASAPI)
  20 s with a loss and reopen: no underrun, the audio thread's worst period 49 ms (the reopen), against 1 s of
  audio queued;
- both platforms compile it (Windows run; Linux built, not run: no Linux machine here).

**Found, recorded as intended differences**: OpenAL Soft holds a finished play-once source's last frame to the
end of its mixing update (up to 784 frames, 18 ms, at 44.1 kHz) before silence; of the game's 284 files without
a loop, one effect ends above -40 dBFS (085_boss_attack14, -36.7 dBFS, named by no id), so the step is
inaudible. Not tested here: a physical unplug (the loss is simulated; the owner's sessions, M4).

### 4.4 M3, the native sound manager (2026-11-11) [M]

**Done**: the game's sound-manager class answered natively on OpenAL Soft (port/native/audio/sound_manager.cpp,
interface native_sound.h; the glue in port/audio_hooks.cpp), behind **DOTHEROES_NATIVE_AUDIO=1** -- the default is
still the lifted FMOD path (rel29's), kept for comparisons until M5. With the switch FMOD is never initialised: no
FMOD thread, SPU mixer or cellAudio port. Written from the class's behaviour as M0 traced it and as its methods
read (a behavioural description, kept locally; the facts that matter are below), not translated from it:
- **Records and states**: every sound the game started, until it ends or is stopped -- an effect, an effect fading
  in, the music -- each in one of the class's states (effect; music; music ducking/unducking; fade-in armed for one
  update, fading in, steady; effect ducking out, ducked silent but playing; music and effect fade-outs armed for
  one update, then fading); fades linear in the game's own dt; a music fade-out ends in stop and unload.
- **One instance per id**; the volume model as M0 found it (list volume x level x 0.11, music also x master; the
  game's option integers read at each call, clamped 0-1); isPlaying true until a sound's end, within a frame;
  isBGMActive / currentBGMId from the music's records (323: none).
- **Music**: playBGM opens the stream (asynchronously, as FMOD's callback was); it starts at full volume on the
  second update after the open; the same track asked again restarts it; a track already open is left as it is; a
  "once" track that ends is unloaded; a file with loop points loops whatever is asked (section 4.1).
- **Pause**: the system menu's flag pauses every sound and stops the manager's clock until it clears (which also
  ends any pauseById); pauseById pauses an id's sounds while their fades go on.
- **3D**: corrected in section 3 (the game's frame is left-handed: x mirrored; a 3D effect without a position at
  the listener); positions given at the start only, the listener every frame, never turning.
- **Centred mono**: a mono sound not placed in 3D (2D effects, 3D effects without a position, the one mono music
  track) plays as a stereo copy straight to the outputs at 0.707 a side, as rel27's mix had it for a 2D effect
  (0.712 measured in the sound lab's capture; OpenAL's own centre on stereo speakers is 1.5 dB lower: 0.603
  measured in the game before this was done; 0.715 after). A 3D effect without a position was louder in rel27's
  mix, about 1.0 a side (measured in M4, section 4.6).
- **Not done**: the class's preload (33.7 MB of effect files read into the game heap at boot, freed again by
  loadSounds, read by nothing else): the native loadSounds reads the player's files itself.

**Gates**:
- **Replay** (tests/native/sound_replay.sh; needs the developer's local M0 captures): rel27's own traced calls
  given to the native manager in order, on a loopback device stepped by the game's dt -- all 14 traces (every M0
  scene, the 30-minute session, the sound lab): **about 360,000 answers the game read, 0 different**; about 400,000
  channel volumes rel27 gave FMOD, all within 0.002; every music start on rel27's update except three in the sound
  lab (its playBGM calls are made from inside the update hook: the open completes within that update natively,
  after it in rel27 -- a call site the game's own scenes never use); no underrun; nothing left at shutdown.
- **Behaviour** (tests/native/sound_manager_check.cpp, made-up sounds, no game data): 32 cases (records, fades,
  ducks, music start and unload, the restart of a playing track, pause, stale handles).
- **In the game** (rel30, then rel31 with the centred mono; both the gen13 lift): memtest 695/695; M0's scenes with
  the switch, traced and captured (OpenAL's wave writer in place of the speakers): no crash, every drive done; the
  same music in the same order and the same reverb switches as rel27 in every scene; **0 FMOD API calls**; the
  quit's shutdown left 0 sources, buffers, effects and slots; the 30-minute session's memory flat (working set
  904 MB from 150 s on), 47-48 threads against rel27's 50-51 (FMOD's gone, OpenAL's added).

| Scene | effect plays rel27 / native | mix level native - rel27 (mean, worst 10 s) | balance (mean, worst) |
|---|---|---|---|
| title and menus | 151 / 151 | 0.00, 0.12 dB | 0.00, 0.09 dB |
| overworld (rel31) | 413 / 427 | -0.46, 1.29 dB | +0.37, 1.35 dB |
| river fight (rel31) | 180 / 168 | -0.67, 1.76 dB | +0.05, 1.26 dB |
| dungeon approach | 159 / 172 | +0.08, 2.07 dB | -0.10, 0.64 dB |
| dungeon deeper | 190 / 168 | -0.24, 7.61 dB | -0.23, 1.78 dB |
| exit after the boss | 19 / 21 | -0.12, 0.32 dB | -0.04, 0.09 dB |
| save inside the dungeon, continue | 212 / 183 | +0.09, 1.34 dB | +0.01, 0.43 dB |
| save-data list | 33 / 33 | -0.01, 0.19 dB | +0.04, 0.17 dB |
| pause, reload, quit | 110 / 89 | -0.09, 0.60 dB | -0.07, 0.50 dB |
| 30 minutes at the river (rel31) | 121 / 49 | +0.02, 0.55 dB | -0.02, 0.51 dB |

- **Effect plays differ by themselves** between runs of a fight (the scenes' runs differ frame to frame): two FMOD
  runs, rel27 and rel29 (traced for this), differ as much as rel27 and the native manager -- the river 180 / 169
  (native 168), the overworld 413 / 487 (427), the dungeon's approach 159 / 205 (172); one hit sound 21 / 82 times.
  The music, the reverb and the scripted scenes (the menus, the save-data list: 151 / 151, 33 / 33) agree exactly;
  and given the same calls, the native manager answers exactly as rel27 (the replay).
- **Left and right**: positioned effects 30-150 degrees off centre, the side the mix put each on against the side
  it should be on: at the river rel27 40 of 44, native 34 of 38 (rel31: 32 of 34) on the expected side (a wrong
  mirror would put most on the other); in the fields, crowded, both about 55%.
- **The player's install** (the setup's build of the game, the manager in the prebuilt library, OpenAL Soft beside
  the game): a river fight with the switch: the device opened, 281 effects loaded, no crash.

**Its cost against rel27 and rel29** (uncapped, real audio on the default output, three rounds rotated per arm;
rel31 with the switch; the frame trace's window, and the process's CPU from 60 s to the end):

| | rel27 | rel29 | rel31, native audio |
|---|---|---|---|
| river fight: fps | 198.7-208.6 | 206.1-211.0 | 209.5-211.0 |
| river: mean / p95 / p99 frame time | 4.79-5.03 / 5.32-5.87 / 6.43-7.10 ms | 4.74-4.85 / 5.36-5.98 / 6.53-7.16 ms | 4.74-4.77 / 5.28-5.66 / 6.34-6.41 ms |
| river: frames over 6.94 ms | 0.4-1.4% | 0.4-1.6% | 0.4% (each round) |
| river: CPU (cores) | 1.87-1.91 | 1.92-1.97 | 1.92-1.94 |
| fields, 4 ripples forced: fps | 245.7-248.1 | 239.6-242.1 | 237.5-245.4 |
| fields: mean / p95 / p99 | 4.03-4.07 / 4.63-4.70 / 5.51-5.57 ms | 4.13-4.17 / 5.51-5.73 / 7.44-7.46 ms | 4.08-4.21 / 5.58-5.79 / 7.34-7.62 ms |
| fields: CPU (cores) | 1.68-1.75 | 1.67-1.72 | 1.67-1.69 |
| river at the game's 60 fps cap (2 rounds): CPU (cores) | 1.51-1.55 | 1.57-1.59 | 1.54-1.55 |
| river at 60 fps: p95 / p99 | 16.96-16.98 / 17.12-17.16 ms | 17.01-17.07 / 17.22-17.24 ms | 16.98-17.03 / 17.17-17.31 ms |
| threads (river, 60 fps) | 55 | 55 | 51 |

- **No measurable change in frame time from the audio**: the native arm is within rel29's range in the fields and
  at the top of the range at the river (its p99 6.34-6.41 ms, every round's frames over 6.94 ms 0.4%, against
  rel29's 6.53-7.16 ms) -- a hint, not established at three rounds. The fields' p99 against rel27 is M1's
  generated water and waver (section 4.2), the same with either audio path.
- **CPU**: uncapped, each build renders as fast as it can, so its CPU follows its frame rate (all three alike). At the
  game's 60 fps cap the arms do the same work: the native audio used 1.54-1.55 cores against rel29's 1.57-1.59
  (about 0.03 of a core, the size of FMOD's SPU mixer's work in docs/AUDIO.md), the same as rel27 (whose FMOD
  mixer, codec and reverb were native ports), with 4 threads fewer. Small and measured in two rounds: section 6's
  hypothesis holds in direction and size, not as a gain that matters.

**Differences, recorded** (for the owner's listening, M4):
- **Positioned 3D effects** keep OpenAL Soft's own spatialization -- **an intentional difference, approved by the
  owner on 2026-11-12**: on stereo speakers a sound straight ahead is 1.5 dB lower than in rel27, one at the sides as
  loud as straight ahead -- rel27's host downmix put the sides 10 dB and the rear 6 dB lower (the matrix that goes,
  section 4). **Corrected in section 4.6**: those figures are the matrix's coefficients; measured end to end, the
  native manager's 3D effects are 2.9-6.1 dB under rel27's in the stereo mix (ahead the most), rel27's sides as loud
  as ahead. The river's -0.67 dB is mostly this (two rel27 runs of it differ by +0.08 dB). The owner's decision:
  no compensating gain on positioned effects, and the old downmix's directional attenuation is not recreated (it
  was an artefact of FMOD's 7.1 output folded to stereo, not the game's intent). Centred 2D mono keeps rel27's
  level (0.707 per side, above): that is the game's own non-positioned mix, not a pan law.
- OpenAL Soft holds a finished one-shot's last frame up to 18 ms (section 4.3); EFX's reverb is its own
  implementation of the same I3DL2 parameters (its tail is not sample-identical); positions are applied every frame
  (FMOD applied them at its ~9 Hz update); voices beyond 32 go virtual by priority, then quietness (never reached:
  6 at most in every scene); the sound lab's in-update music starts one update earlier.
- **Not tested**: the boss fight and a crowded scene (no drive reaches them: the owner's sessions, M4); the system
  menu's pause (no drive opens it); a physical unplug of the output device (simulated in M2).

### 4.5 M4, the automated part (2026-11-12) [M]

The owner's listening sessions and acceptance close M4; this is everything that needed no one at the screen.
**The owner's decision on positioned effects** (2026-11-12): OpenAL Soft's own spatialization stays, no compensating
gain (section 4.4, "Differences").

**What changed in the audio layer** (each found by these tests):
- **Device recovery without a race.** A disconnect stops OpenAL Soft's sources before the audio thread's next poll
  sees it (up to 10 ms): an update in between found a looping ambience's source stopped with the device still
  "connected" and freed it -- the loop lost for good after the reopen, its record gone with it. Now a source found
  stopped early is taken as ended only once a poll begun after that still finds the device connected and not
  reopened (al_device's poll count, voice_pool's stop state); a one-shot at its length ends at once, as before. No
  ALC call on the game's thread: OpenAL Soft's ALC queries take the device's lock, which a reopen holds for as long
  as the output takes to open (alc.cpp's alcReopenDeviceSOFT, read in the shipped source).
- **Where sounds resume after a loss.** The effects from where their time is (a one-shot whose time ran out during
  the loss is not restarted); the music from where it stopped -- its queue is no longer emptied and refilled while
  the device is out, and its source restarted at the offset last heard (it replayed up to a 250 ms buffer, or
  skipped the queued second); a sound paused through a loss from where it was paused (it restarted from the file's
  start).
- **Voices as FMOD's virtual voices rank them**: by priority, then audibility (the gain times the distance
  attenuation from the listener, which the pool now knows) -- a new sound quieter than every real one starts
  virtual instead of taking the quietest's voice; at each update a virtual channel that has become more important
  (a fade-in rising, the listener walking up to it) takes a real one's voice, with a 1.6 dB margin between equal
  priorities; the game's 100 channels (System::init(100)) as the channel limit (the least important stopped for a
  new one at least as important, else refused), where 512 records were allowed. The music keeps its own streams
  outside the 32 voices (rel27: shared, same priority): effects never take its voice.
- Diagnostics for tests (native_sound.h): a channel's state (real, paused, offset, gain, audibility, priority), the
  loopback's output, a device loss with failing or slow reopens; the stats count promotions, swaps, channel steals,
  refusals, restarts and failed reopens. DOTHEROES_AUDIO_TEST_LOSS=<s>[:<failed>[:<ms>]],... loses the output in a
  game run; the sound lab's `reverb` step.

**tests/native/sound_stress_check** (no game; in tests/native/openal_check.sh), all passed:

| Mode | What | Result |
|---|---|---|
| voices | 40 effects on 32 voices; priorities 64/128/200; a volume change, a fade-in and the listener's movement re-ranking; 12 stopped; 3 s effects ending; a reused slot; 100 channels and a 101st | the 32 most audible real, the rest virtual and answering isPlaying; the output: the 32 at their levels (within 0.3 dB), the 8 virtual silent; a swap or promotion resumes at the channel's time (0.000 s off); ends within a frame of 3 s whether real, stolen or promoted; stale handles answer 2 and stop nothing; the 101st refused or stopping the least important |
| voices: soak | 5 minutes, random plays (fade-ins, 3D, priorities), stops, stale stops, pauseById, fades, ducks, listener moves, the game's pause every 20 s; checked after every update | 4,548 plays, 835 stolen voices, 765 promotions, 60 swaps; never over 32 voices or 100 channels; never a virtual channel more important than a real one; AL objects constant; **the music untouched**: active, its gain unchanged, every one-second window within 0.07 dB, no underrun |
| pause | the class's pause with music, a loop, a one-shot, a fade-in, a fade-out and a duck under way; 200 pause cycles of random lengths; a crossfade paused; a virtual channel paused; quitting while paused | 5 s paused: everything answers as playing, nothing moves (offsets within 0.0000 s, fades held), silence; on resume the fades go on from where they were (within 1e-4); through 200 cycles a loop within 0.0000 s and the music within 0.01 s of their unpaused time, a 3 s one-shot ends after 3 s unpaused, a 1 s fade-out after 1 s; the crossfade completes; the shutdown while paused: 4.7 ms, nothing left. The music's pause reaches its audio thread at that thread's next step (10 ms) |
| device | a loss and reopen; an outage of two failing reopens (2 s) with the volume changed during it; a loss while paused; 30 losses in a row (loopback, output measured) | the effects resume where their time is, the music where it stopped (within 0.02 s), each at its level (0.00 dB); during the outage the loops and the music answer as playing, a one-shot ending in it answers by its time and is not restarted, silence; after it the levels set during it (-3.52 dB, as asked); 30 of 30 recovered; no object more, no underrun |
| realtime | 20 losses on the real-time path (OpenAL Soft's mixer thread through the wave writer, the music's audio thread), every third with two failing reopens, every fifth with a 300 ms reopen | 20 losses, 20 reopens, 12 failed reopens retried; the loops and the music answered as playing at every update; the game's thread never waited (update and isPlaying p50 0.013 ms, p99 0.106 ms, worst 0.190 ms); threads 7 before and after; AL objects as before; no underrun; a second manager's life in the process leaves no thread more |

The M2 and M3 suites again with these changes: openal_check (every file, every loop exact, a minute streamed in
real time with a loss), sound_manager_check (32 cases): passed. **rel27's 14 traces replayed** (tests/native/
sound_replay.sh): about 360,000 answers, 0 different; about 400,000 volumes, 0 off -- as in M3 (the sound lab's
three in-update music starts one update earlier, as recorded there).

**In the game** (rel32: rel31 with the work above; rel34: with the movies' sound; rel35: with the reverb calibrated --
the owner's play build):
- **M3's scene set again** on rel32, traced and its mix captured: the menus, the fields, the river, the dungeon's
  approach and its deeper rooms, the exit after the boss, save and continue, the save list, quitting and the
  30-minute session. No crash; against rel27's M0 traces the same music in the same order, the same reverb
  switches, every play with a handle, 0 FMOD API calls; the quit's shutdown left nothing; the 30 minutes kept
  their memory flat (957 MB at 2-5 minutes, 958-959 MB at 25-30) with no underrun.
- **The game's pause** (tests/scenarios/m4_pause.drive: the system menu opened ten times at different lengths
  over the field's music and ambience, TITLE and CONTINUE, then the exit asked with the menu open) on rel32 and on
  rel34, against the same drive on M0's traced rel27 build: the same music and reverb switches and **the same
  isPlaying answers** (4,380 times not playing and 4,372 playing in each), effect counts as two runs differ; the
  exit with the menu open went through the game's own exit path, the shutdown leaving nothing. A correction to
  M3's record: the class's own pause is the PS3 system overlay's (sysutil 0x131), which the port never sends; the
  game's START menu pauses the game, not its sound (rel27's traces: the music and ambience go on).
- **The movies' sound** (found by the M5 inventory, section 9; fixed in rel34): Bink's player plays a movie's
  sound through a cellAudio port of its own, on the cellAudio that FMOD's start-up initialised; with the native
  manager nothing initialised it. On rel32 the attract mode's demo movie played twice without sound (no Bink audio
  thread, no cellAudio); M3's scenes had played no movie with sound. Now the native start-up initialises cellAudio
  for the movies (port/audio_hooks.cpp); on rel34 the movie opens its port and starts its audio thread as on rel27,
  and its sound in cellAudio's capture matches rel27's: -0.02 dB over both demo plays (correlation 0.99, 0.92).
- **Losses of the real output** (rel34; the river fight on the default output, WASAPI;
  DOTHEROES_AUDIO_TEST_LOSS: five losses -- one with two failing reopens retried a second apart, one with a 400 ms
  reopen, one failing once): each lost and reopened through OpenAL Soft's own WASAPI reopen, no crash; the frames:
  the same long frames as the drive's runs without losses (its area loads, at the same moments: 233 ms against 179
  and 223 ms at 20 s, 278 ms against 269 and 265 ms at 35 s), none at the losses. (rel32's run of it tested
  nothing: the hook fired before the class's init -- its clock, fixed.)
- **The dungeon reverb, measured and calibrated** (tests/scenarios/reverb_lab.drive: a dry 0.5 s effect eight
  times with the reverb on; tools/reverb_tail.py): on the FMOD path (the lifted SFX reverb, as rel27's) the tail
  starts 30 dB under the sound and falls about 68 dB a second (0.87 s to -60 dB), -42.6 dB under it over 1.2 s; EFX
  with the game's parameters as I3DL2 states them, -37.3 dB and slower (0.97 s): 5 dB more reverb, which is what
  made the dungeons' mix 0.4-0.85 dB louder for the same calls (below). The late reverb's level, decay and HF
  ratio searched (300 sets, the FMOD run's own calls replayed natively, then the dungeons' windows): late -1.5 dB,
  decay 0.95 s, HF ratio 0.3 -- the dungeons' reverb windows -0.19, +0.27 and -0.01 dB against rel27, the dry
  effect's tail within 2.6 dB RMS of FMOD's step by step (its energy 1.7 dB less). Calibrated by measurement of
  the game's output, nothing of FMOD's code. On rel35 in the game the effect's tail is -43.7 dB under it
  (FMOD's -42.6; -37.3 before), each 100 ms of it within 3 dB of FMOD's, and the dungeon's approach plays the same
  music and reverb switches as rel27 with 0 FMOD calls.
- **The same calls on both engines** (the fidelity review): rel27's own traces replayed natively with the mix
  captured (tests/native/sound_replay --wav) against rel27's capture of the same run, aligned (correlation 0.99),
  10 s windows: with no positioned effect and no reverb, within 0.05 dB (the fields, the river, the menus, the save
  list, the exit after the boss); with positioned effects, the approved spatialization (the fields -0.61, the river
  -0.77 dB on average); with the reverb, after the calibration, within 0.3 dB. So M3's game-run differences (the
  river -0.67, the fields -0.46 dB) are the approved difference, not the runs' chance, and none is left unexplained.

**Remaining differences, recorded**: positioned effects (approved); the reverb's tail not identical (calibrated:
within 2.6 dB of FMOD's on the measured effect, within 0.3 dB on the dungeons' mix); the music outside the 32
voices (rel27: shared); the sound lab's in-update music starts one update earlier; OpenAL Soft's last-frame hold
(up to 18 ms); the music's pause reaching its audio thread within 10 ms. 217_enemy_appear (FMOD's own end
reporting, not reproduced): nothing in the game depends on it -- the game never asks isPlaying of it while the
two engines' answers would differ (the replays: every answer the game read, 0 different).
**Not tested, the owner's sessions**: the boss fight and a crowded scene in the game (the 32-voice limit is
tested synthetically above: no scene reaches more than 6 voices), a physical unplug or a change of the default
output, the listening comparison itself. Not run: the Linux build (it builds; no Linux host here).

### 4.6 M4, the owner's listening (2026-11-12) [M]

The owner's session (rel35, the native manager): the audio good overall; one observation -- sword impacts on the
left of the screen seemed slightly louder than the same impacts on the right (with FMOD not known).

**The sounds** (M0's and M3's traces, every play of them): the impact is effect 202 (221_knock, mono, 0.38 s, 3D,
min 8.8, max 160), placed by the game on the hero's facing axis at the contact point -- (+-d, 0.3, 0) or
(0, 0.3, +-d) from the listener, d 1.5 to 10 -- so left and right impacts are mirror images in the game's own
coordinates; a blow landing (302, 375_hit_01) is placed at the enemy; the swing (148, 156_Anchor_throw) is played
without a position. Every effect file is mono: no channel of a file is louder than the other.
**The listener** is the hero's position, forward +z, up +y. **The screen's directions**, from the owner's session's
own input (record.log: the stick held one way, the hero's position after): right is +x, up is +z -- the frame the
sound manager assumes (section 3), so screen-left is the listener's left. **The outputs the owner had**: OpenAL Soft
opens the G432 headset as 7.1 (its Windows format, Logitech's virtual surround) and the Realtek output as stereo;
the session moved to the Realtek at the demo movie and stayed there, so the gameplay was heard in stereo.

**Measured** (tests/scenarios/sound_pan.drive: the sound lab's `sepos`, an effect at the listener plus an offset,
then the same offset mirrored, in ABBA order; the same calls on both paths of rel38 = rel37 with the lab step;
tools/pan_check.py: each play's level per channel, the noise before it subtracted, over the effect's length). FMOD
is the lifted path, bit-exact with rel27's mix (section 4.2), captured at cellAudio (AUDIO_WAV: the stereo the
device gets); OpenAL Soft through its wave writer in stereo (the Realtek's format) and in 7.1 (the G432's):

| | FMOD (rel27's mix) | OpenAL Soft, stereo | OpenAL Soft, 7.1 |
|---|---|---|---|
| knock at x -1.5 / +1.5: total, left minus right | +0.01 dB | +0.03 dB | +0.02 dB |
| the same, the near channel / the far one, against the mirror | +0.00 / +0.04 dB | +0.02 dB / (silent) | each pair within 0.16 dB |
| knock at x -4 / +4 | -0.01 dB | +0.01 dB | -0.01 dB |
| knock at x -12 / +12 (beyond the min distance) | -0.01 dB | -0.05 dB | -0.05 dB |
| knock at 45 degrees ahead, left / right | +0.02 dB | +0.01 dB | +0.04 dB |
| a blow at x -1.29 / +1.29 (302) | -0.01 dB | -0.02 dB | -0.02 dB |

- **No left/right asymmetry on either path**: every mirrored pair, total and channel by channel, within 0.05 dB
  (0.16 dB for a 7.1 speaker pair), repeats within 0.2 dB. The coordinate conversion, the listener's orientation
  and position, and the attenuation are as section 3 states: beyond the min distance the inverse-distance law
  gives 20 log10(8.8/12) = -2.69 dB from 4 to 12 units; FMOD -2.69, OpenAL Soft -2.64 dB. Nothing in the native
  path to correct; what the owner heard is not in either implementation's output (the listening setup, or the
  game's own impacts -- their contact points vary from 1.5 to 10 units, and other sounds overlap them -- are
  left; neither is measurable from here).
- **How the paths spread an effect at the side**: FMOD's mix keeps the far channel 9.9 dB under the near one at
  90 degrees (9.5 dB at 45); OpenAL Soft's stereo pans hard -- nothing in the far channel at 90 degrees, 20 dB
  under at 45. In 7.1 OpenAL Soft puts a 90-degree effect in the side speakers, the fronts and rears 10 dB under.

**The levels: a correction to section 4.4.** The same plays' totals (all channels, dBFS):

| play | FMOD (rel27's mix) | OpenAL Soft, stereo | difference |
|---|---|---|---|
| knock at the side (1.5 or 4) | -27.3 | -31.5 | -4.2 dB |
| knock at the side, 12 units | -30.0 | -34.1 | -4.1 dB |
| knock ahead (z +1.5) | -26.9 | -33.0 | -6.1 dB |
| knock behind (z -1.5) | -31.6 | -36.3 | -4.8 dB |
| knock at 45 degrees ahead | -27.6 | -31.5 | -3.9 dB |
| blow at the side (302) | -23.8 | -27.8 | -4.0 dB |
| swing, no position (148) | -23.4 | -26.5 | -3.1 dB |
| knock, no position | -28.5 | -31.4 | -2.9 dB |

Section 4.4 described rel27's directions from the host downmix's matrix (the side channels x0.3, the rear x0.5:
"the sides 10 dB and the rear 6 dB lower"); measured end to end, rel27's impact at the side is as loud as ahead
(0.3 dB under) and 4.6 dB under behind it -- FMOD spreads an effect this close over several of its 7.1 channels
and the fold to stereo adds them up. So the native manager's 3D effects are 2.9 to 6.1 dB under rel27's in the
stereo mix (straight ahead the most), not 1.5 dB under ahead and louder at the sides; and a 3D effect played
without a position was about 1.0 a side in rel27's mix, not 0.707 -- the 0.707 holds for the 2D effects (section
3's "centred mono" was measured on one). The music and the 2D effects are unchanged (section 4.5: the mixes with
no positioned effect within 0.05 dB). This is the owner's approved spatialization (OpenAL Soft's own, no gain
added, the old fold not recreated) with its size corrected; nothing in the audio was changed for it.

**The owner's decision on the corrected levels (2026-11-12):** kept as they are. The owner heard them in play and
found them good; OpenAL Soft's own spatialization and levels stay, with no compensating gain and no recreation of
FMOD's downmix. M4 is closed.

## 5. Repository licensing audit and cleanup

File by file: docs/LICENSE_AUDIT.md (every tracked file classified). Findings before M1 (453 files) and what
M1 did with them (section 4.2):

| Class | Files | Origin | Action |
|---|---|---|---|
| FMOD-derived implementation | port/native/fmod_codec_raw.{h,cpp}, fmod_sfxreverb.{h,cpp}, fmod_mixer.{h,cpp} | transcribed or generated from FMOD's SPU programs in the player's executable | removed (M1) |
| FMOD tooling | tools/fmod_sfxreverb.cpp.in, gen_sfxreverb.py, fmod_mixer.cpp.in, fmod_mixer_config.py, fmod_mixer_{check,batch}.sh, fmod_mixer_{coverage,profile}.py; tests/native/fmod_{codec_raw,sfxreverb,mixer}_check.cpp | generators, templates and checkers that reproduce or exercise that code | removed (M1) |
| Generic generator | tools/gen_spu_native.py | ours: turns any lifted SPU image into native C++ (nothing of any image in it; the image-specific facts live in a per-image configuration) | keep: it generates the water and waver on the player's machine |
| FMOD glue | port/native/fmod_codec_native.cpp, fmod_mixer_native.cpp | ours, but only to run the above | removed (M1) |
| Game-derived implementation | port/native/water_task.{h,cpp}, waver_task.{h,cpp}; tests/native/water_task_check.cpp, waver_task_check.cpp | transcribed from the game's own SPU programs | **the owner's decision: generated on the player's machine from the player's files** (gen_spu_native.py over the local lift, like the lift itself): done (M1); the files are now the host around the generated code, the checks build against it |
| Documentation of internals | docs/AUDIO.md, docs/SPU_NON_AUDIO.md | describe FMOD's and the game's SPU code in detail | AUDIO.md rewritten (M1); SPU_NON_AUDIO.md kept as the game's behaviour and the results (facts, no code) |
| Bullet-derived | port/native/bullet_{cd,cd_task,solver,solver_task}.{h,cpp} | Bullet 2.74, zlib licence | kept; Bullet's notice added, listed in the README (M1) |
| Third party, licences present | DejaVu Sans, Kenney's prompts, ps3recomp (MIT) | | kept; listed in the README (M1) |
| Independent | the other 409 (runtime patches, tools, tests, drives, configuration, docs, the renderer) | ours | keep |

- **Not found**: keys (decryption takes the player's keys file), firmware, game data or assets, shader
  binaries, embedded binaries (no long byte arrays in any tracked file), extracted files, build outputs. The
  project icon is original art. No file was ever committed and later deleted. No release or installer package
  has been published, so no artifact carried FMOD-derived code; the installer's prebuilt libraries are built
  locally (build/lib, not tracked) and today contain the FMOD-derived objects -- they are rebuilt without them.
- **The README was wrong** until M1: it said none of the game's code was in the repository while the FMOD and
  water/waver transcriptions were; corrected with their removal.
- **History**: the FMOD-derived code entered the public history in four commits (542794b, c2526e8, b5f5a14,
  e6f2912, 2026-11-10/11), the internals' documentation with them, and the game-derived tasks in three
  (9a71d05, 38a0c9b, a719cef). Removing files from the tree leaves them in those commits. All seven are after
  the three tags, so a cleanup would rewrite only the 28 commits from 9a71d05 (2026-11-10) to today's main
  and **move no tag** (all three tags are ancestors of 9a71d05, checked).
  Options for the owner (nothing is done without explicit approval): (a) remove from the tree only and leave
  the history; (b) a bounded rewrite removing those paths from those 28 commits, then a GitHub Support
  request for the old SHAs, as on 2026-11-10. The owner favours (b) but has not authorized it: the exact
  plan (paths, commits, verification, backups) is prepared once the source cleanup is complete and waits for
  a separate approval.

## 6. Performance and synchronization

- **Hypotheses until measured against rel27** (the owner's adjustment 5): gone from the CPU: FMOD's five PPU threads, its 10 Hz update, the SPU mixer thread (about 3% of a core at
  187 blocks a second), the codec and reverb, cellAudio's host thread and downmix. Added: OpenAL Soft's mixer
  thread (stereo at 48 kHz, tens of voices: expected well under 1%) and one streaming thread.
- The game's frame loop never waited on audio (FMOD mixed on its own threads), so the frame-time effect of the
  switch itself is expected to be small; the larger effect is step 7's retirements (the reservation test on
  every PPU store). Measured, not assumed: CPU per thread, mean and p99 frame times and spikes against rel27.
- Sync: no audio clock drives the game; music and effects start when the game asks, as before. Bink's movie
  audio does not go through FMOD, but it does go through cellAudio (corrected in M4, section 4.5: the movie player
  opens a 2-channel port of its own on the cellAudio FMOD's start-up initialised) [L]; M8 replaces it.

## 7. Milestones and gates

| # | Milestone | Gate |
|---|---|---|
| M0 | Baseline from rel27: the SoundManager/FMOD call traces and output captures per scene (instrumentation only) | captures complete for every scene in section 4 |
| M1 | Public tree: the FMOD-derived ports, tools and glue removed, the game back on the lifted FMOD path by default (the old path stays until M4; rel27 kept locally as the reference); the water and waver generated on the player's machine (gen_spu_native.py over the player's lift, in the setup pipeline), then their transcriptions removed; README, notices and the architecture documents corrected (section 8) | the validation scenes on the lifted FMOD path and the generated water/waver (bit-exact with the lift; their cost against the hand ports and rel27); the install test (the generation on a fresh install); no FMOD- or game-derived implementation file tracked; the guard scan |
| M2 | OpenAL Soft built and loaded dynamically (its DLL, licence, source reference); device lifecycle and the PCM loader and streamer, tested alone | a test program: every sound file loads, music streams an hour with no underrun, device unplug and reopen |
| M3 | The native SoundManager behind a switch (default still the old path) | call-trace parity with rel27; no FMOD routine reached; no underrun or leak; the scenes pass |
| M4 | Validation against rel27 and the owner's listening sessions | all scenes, the 30-minute session; the differences recorded; the owner's acceptance |
| M5 | OpenAL the default; FMOD lifted as traps only, its SPU images not lifted; the old path removed; the installer and setup rebuilt | the validation set, the install test, no trap reached |
| M6 | Retirements: a census of SPU workloads over the whole game (every drive, long sessions, the owner's sessions); then the FMOD SPU machinery, the overlay dispatch, cellAudio if unused, and the per-store SPU reservation test only if no workload takes reservations | the census; the validation set; CPU and frame-time A/B against rel27 |
| M7 | History, if the owner approves a cleanup | the bounded rewrite verified as on 2026-11-10; the Support request |
| M8 | Bink 1 through FFmpeg (LGPL build, dynamic): a separate plan | -- |
| M9 | Native PC experience: display settings, VSync, custom frame caps, audio settings, controller remapping, button prompts and the other agreed PC improvements (docs/MILESTONES.md, "Roadmap: M9-M11") | documented only (2026-11-12); after the core architecture work |
| M10 | Launcher and update management (docs/MILESTONES.md, "Roadmap: M9-M11") | documented only; after M9 |
| M11 | Character library and community sharing (docs/MILESTONES.md, "Roadmap: M9-M11") | documented only; after M10 |

## 8. Architecture requirements (to be written into docs/NATIVE_PORT.md and CLAUDE.md at M1)

Native Vulkan graphics; OpenAL Soft audio; FFmpeg-based video decoding; no permanent proprietary middleware
implementation in public source; game files and keys supplied by the player; no unnecessary PS3 execution
machinery in the final architecture; stable 60 fps with minimal frame-time spikes; correct behaviour across
gameplay, menus, saving, loading and quitting. A decision that conflicts with these is flagged to the owner,
not made silently. docs/AUDIO.md's "switching audio libraries is not the plan" is withdrawn.

## 9. M5 prepared: what goes with FMOD (2026-11-12; an inventory, nothing removed)

Read in the lift (build/r8/gen13: names, addresses and counts only), the runtime, the port and the run logs [C: code;
S: a static scan of the lift; L: logs; I: inferred]. M5 waits for the owner's acceptance of M4.

**FMOD's PPU code** [S]: 2,819 lifted functions -- [0x82F650, 0x908584) (2,793, in five of the generated files) and
[0xA42E10, 0xA43800) (26) -- about 8.5% of the lifted PPU source. Nothing outside reaches it directly except 28 call
stubs, each called only from the sound manager's class (59 calling functions; the plan's 72 call sites); indirect
calls cannot be followed statically, so the traps are the proof. FMOD alone imports cellMic, sys_net and
cellSysutilAvconfExt's extensions, and issues the SPU thread-group, mailbox and event-port syscalls (170-193).
- **Traps need a lifter option** [C]: ps3recomp's lifter has no trap or stub list (its entry hooks run a host
  function in place but still compile the lifted body; the function list does not remove code). M5 adds one: each
  function in FMOD's ranges lifted as a trap -- a call reports the address and ends the game -- so the executable
  holds no FMOD implementation, not even unused, and any path still reaching it shows itself in validation. The 28
  FMOD API hooks (config/audio_hooks.json) and their trace lines go (the hook table links against them; the lift
  refuses an entry hook at no lifted function), and with them `g_fmod_reached` (today it counts and lets the lifted
  body run: a count, not a trap). The flashMP3 patch (config/insn_patches.json, game_patches.cpp) lies in FMOD's
  range and goes.
- **FMOD's SPU programs**: the setup extracts and lifts all 26 SPU images (setup_pipeline.py's SPU step:
  extract_spu_images.py, build_spu_workloads.py, ids by position); 21 are FMOD's -- image 6 (the mixer), 7 (its
  NeuralTHX variant) and 8-26 (its 19 relocatable plugins: config/spu_relocatable.json). M5 stops lifting and
  compiling them (an exclusion in the SPU step; the game's 1-5 keep their ids). The runtime's overlays,
  relocatable images and SPU thread groups lose their only user: retired in M6 after the census.
- **FMOD's start-up and what it creates** (rel29's log) [L]: cellAudio's AudioOutConfigure, Init (its output and
  mixing thread), port 0 (8 channels) and its notify queue; the mixer's image; five PPU threads (the stream, the
  file, the event receive, FMOD_NONBLOCKING, the music status query); event queues and ports; the "FMOD" SPU thread
  group. With the native manager none of it exists (M3), except cellAudio, below.

**cellAudio and SDL2 stay until M8** -- found here, and a fix in M4: Bink's movie player opens a 2-channel cellAudio
port of its own for each movie with sound (the attract mode's demo: 6 ports, 6 "Bink Audio Thread"s in a run)
[L], on the cellAudio that only FMOD's start-up initialised. With the native manager nothing initialised it: the
movie's port would not open (NOT_INIT) [C] -- untested before M4 (no native run had played a movie with sound). The
native start-up now initialises cellAudio for the movies (port/audio_hooks.cpp; tested in section 4.5). So M5 keeps
cellAudio (its SDL2 output carries the movies' sound) and SDL2's audio; M8 (FFmpeg) moves the movies' sound to
OpenAL Soft and retires both. SDL2 stays in any case: the window and Vulkan surface (SDL video), the game
controllers and the keyboard (cellPad), timers; on Linux OpenAL Soft's own output goes through SDL2.

**The switch, the old path and the build**:
- DOTHEROES_NATIVE_AUDIO (read only in port/audio_hooks.cpp; the setup never sets it, so a player's install runs
  FMOD today) goes: the native manager the only path, its hooks always on. Drives that set it keep working.
- The lift configuration changes (the traps, the hooks, the SPU exclusion): a player's update relifts (the
  configuration is in the tools' fingerprint), as M1's generation did.
- native_sound_absent.cpp (a build without OpenAL Soft's headers) would then be a silent game: the build requires
  OpenAL Soft's headers instead (they ship in the prebuilt folder).
- Kept for the native path: DOTHEROES_AUDIO_TRACE (the sound manager's calls), the sound lab, the test losses,
  tools/audio_mix.py and audio_trace_compare.py (its "0 FMOD calls" check becomes M5's gate with the traps),
  tests/native (sound_replay reads rel27's traces: the developer's local captures). FMOD-only: the FMOD API trace
  lines, tools/audio_trace.py's FMOD analysis. cellAudio's AUDIO_WAV stays while the movies use cellAudio.
- at3_standalone (the runtime's ATRAC decoder for cellAtrac) is not FMOD's and unused by this game: left to M6's
  review of the runtime.
- Docs that still describe FMOD as the running path: AUDIO.md's title and section 1, NATIVE_PORT.md, PC_EXPERIENCE.md,
  this plan's status; updated in M5.

**M5's gates** (section 7): the validation set and the install test on the build with the traps; no trap reached in
any run (every drive, the long session, a movie with sound, the owner's session); the executable's FMOD ranges
holding only traps (the boundary check of a built game can count them).

### 9.1 M5 done (2026-11-12): FMOD excluded from the lift

M4 was accepted on 2026-11-12 (section 4.6), and M5 went ahead as prepared above, with these differences found on
the way [C: code; S: the lift; L: run logs; M: measured].

**The lifter's exclusion** (ps3recomp patches 0146-0148; generic, not FMOD's): `ppu_lifter.py --traps <json>` lifts
every function starting in an excluded range as a call of `ppu_trap(address, what, ctx)` -- it reports the address,
the host call chain and the guest stack, then ends the game ("excluded-code") -- and registers the ranges with the
runtime, so `ppu_lookup` answers any other address inside them (an indirect call, a pointer, a jump into a
function's middle) with the same trap. A range that cuts a function is refused, as is a hook, a patch, an observer or
a promoted call inside one. Tested on a synthetic program (tools/test_ppu_traps.py: direct calls, CTR calls, a
function pointer, a tail call, a jump into an excluded function's middle, falling through into one, an ordinary
function beside them, an inert one: 19 checks). build_spu_workloads.py `--exclude` leaves an SPU image out of the
lift.

**The inventory, corrected** [S]: FMOD's code is [0x0082F650, 0x00908584) -- 2,792 functions -- and
[0x00A42AA0, 0x00A440E0), 60 functions: the out-of-line methods of FMOD's classes, each held only by FMOD's own
vtables (read from the vtables and the function descriptors). Section 9's second range, [0xA42E10, 0xA43800), cut a
function in two (0xA43784 runs to 0xA43860) and missed 34 of those methods. The 28 call stubs at
0x00987478-0x00987658 stay lifted (TOC-switching glue): a call through one lands in a trap. 2,853 functions
excluded in all.

**What the traps found at once** [L]: the first M5 build stopped at its start, in FMOD's code at 0x008F476C, called
from 0x0069C264 -- the C runtime's start-up walking its table of static constructors (0x00FE0000-0x00FE0718, 453
entries, 39 of them FMOD's; the destructors' table after it, 273 entries, 2 FMOD's). No inventory of calls had
listed them: they are reached only through that table. The 41 are lifted as a plain return (`inert` in
config/excluded_code.json): they set up and tear down only FMOD's own globals. The lift reports "2,853 traps (41 of
them inert)".

**What went with FMOD**: its 21 SPU programs (the mixer, its NeuralTHX variant, the 19 plugins; build/gen/spu holds
the game's five); the 28 FMOD API hooks (config/audio_hooks.json: the sound manager's 28 stay); the flashMP3 patch
(config/insn_patches.json, its guard in game_patches.cpp); the switch DOTHEROES_NATIVE_AUDIO (=0 is now ignored,
with a line saying so: the native sound manager on OpenAL Soft is the only path); native_sound_absent.cpp (a build
without OpenAL Soft's headers now stops with an error instead of making a silent game). The setup's lift
(tools/setup_pipeline.py, tools/pipeline.py, tools/r8_lift.sh) passes `--traps config/excluded_code.json` and leaves
the excluded SPU images out; the configuration is in the tools' fingerprint, so a player's update relifts. The
executable: 131.7 MB to 120.4 MB.

**Kept until M8**: cellAudio and SDL2's audio output, which carry the movies' sound (Bink's own cellAudio ports).

**M5's gate** (rel41; build/r8/m5_run.sh, local) [M]:
Every run ended without a crash and without reaching a trap.
- The memory test: 695 of 695.
- The validation drives: the river fight, the menus, the fields, a dungeon, the snapshot scene, the after-boss
  scene, the editor, the boss exit, the deeper dungeon, continue from a save, the sound lab.
- Vulkan validation on the river fight: 0 messages. The damage run.
- Quitting through the game's own menu: exit 0 (FMOD's two static destructors inert).
- The water check: 2,483 of 2,483 steps equal. The waver check: 1,170 of 1,177 frames equal (4 with the field
  and 7 with the texture differing, the known mid-ripple class: rel37's 1,162 of 1,169).
- The attract movie with its sound: Bink's 2 audio threads, its 2 cellAudio ports; the level within 0.1 dB of
  M4's capture in every 10 s window.
- A 30-minute session (1,804 s): the working set 960 MB from the fifth minute to the end.
- The save flows (seven drives): each wrote the folders expected and no others.
- The four reference scenes against RPCS3: 5 of 5 within.
- The A/B against rel40 (uncapped, two rounds each, rotated) found no regression:

  | Scene | rel40 fps | rel41 fps | rel40 p99 (ms) | rel41 p99 (ms) |
  |---|---|---|---|---|
  | River fight | 208.3-208.9 | 209.3 | 6.59-6.75 | 6.62-6.72 |
  | Fields | 239.3-243.3 | 237.1-247.1 | 5.69-5.72 | 5.80-6.21 |

- The player side: tests/test_player_pipeline.py's 11 tests; a fresh installation from the disc image (its lift
  equal to the developers', 0 files differ), an update, a relift, the settings, and a river fight from the
  installation (native physics). The installation's build took the port's sources as they stood then, with M6's
  first native changes already in them (not yet committed; section 10): its river fight was also their first run.
- The lift's FMOD ranges hold 2,853 functions: 2,812 traps and the 41 inert ones; no function outside them calls the
  trap.

The installation test's leaf-promotion check expected over 8,000 (R8's lift). M5's lift promotes 7,428: R8's 8,385
less FMOD's 957, the same set outside the excluded ranges. The check now expects over 7,000, and the test compares
with M5's lift (build/r8/gen14) by default.

**Not yet**: the owner's session on a build with the traps. The next build (M6's first, rel42) carries M5 unchanged
and serves for it.

## 10. M6 prepared: the SPU machinery left after M5 (2026-11-12; an inventory, nothing changed)

Read in the runtime (build/ref/ps3recomp), the port and the M3 runs [R: read in code or a log; D: the docs; I:
inferred]. **What runs today with the native audio** (M3's A/B, the river fight, rel30 with the switch) [R]: no SPU
program at all -- `[spu-census] no SPU program started`, no overlay loaded, the physics' 51,231 tasks issued natively
(0 lifted through SPURS), the maps' two packs inflated in the calling thread. **What the game still creates**: the
physics manager's SPURS instances (two Initialize, 3 SPUs each: per instance a kernel thread waking every 1 ms and 3
policy-module threads), their tasksets, 6 queues bound to lv2 event queues and barriers -- with no task ever run in
them [R]. One scene only: M6's first gate is the census over the whole game (every drive, the long sessions, the
owner's sessions), as section 7 says.

| Machinery | Where | Users after M5 | Removable when | Proof |
|---|---|---|---|---|
| The per-store SPU reservation test | ppu_lifter.py (the store emission; `vm_mm_st_ok`), ppu_loader.cpp `VM_WRITE_COH` | none seen in the native river run | nothing can take an SPU reservation or a line watch: the lifted fallbacks gone or fatal (below), the solver's lock and the SPURS host's line writes moved off the lock-line lock | a counter of reservations and watches over the whole-game census at 0; then GETLLAR made fatal (as the absent SPU interpreter is); then the A/B |
| The lock-line lock, coherence bitmaps (2 x 4 MiB), holder list, watches | spu_coherency.c; the SPU branch of ppu_stwcx32 | the native solver's libsync ticket lock (bullet_solver_task.cpp), inflate_direct's per-line notify, the SPURS host kernel's workload lines | as above; the solver's lock a host atomic if the PPU never touches it (not established) | memtest without its watch groups |
| PPU lwarx/stwcx reservations (1,024 sharded locks) | ppu_loader.cpp | the game's own PPU threads | **kept**: only their SPU branch goes | -- |
| MFC/DMA and the SPU channels | spu_dma.h, spu_channels.c | the lifted fallbacks of images 1-5 only (the native water, waver, physics and inflate bypass it) | the fallbacks gone | the census: 0 lifted runs |
| Mailboxes, signal notification, SPU event ports | lv2_register.c, spu_channels.c | FMOD's mixer (each block); none after M5 | M5 | no SPU thread started |
| Overlays and the SPU function dispatcher | spu_channels.c (registry, overlays, `spu_indirect_branch`), spu_dispatch_mt.c | overlays: FMOD's image 6 alone (its plugins); the dispatcher: the lifted fallbacks | overlays at M5; the dispatcher with the fallbacks | `[spu-ovl]` none |
| lv2 SPU thread groups | lv2_register.c | FMOD only | M5 | the thread count |
| SPURS (instances, kernel, policy modules, tasksets, queues, event flags, the job pool) | cellSpurs.c, spurs_pm.c, spurs_policy.c, spu_workload.c | the physics manager's instances, created but given no task | their set-up hooked as the water's and the waver's were (marker values in place of SPURS objects) | threads and CPU at the 60 cap |

**The lifted SPU images** (26: build/gen/spu, runtime ids 1-26) [R]: 1 the water and 2 the waver -- generated natively
(the lifted code a fallback, DOTHEROES_NATIVE_WATER=0; the generator reads their lifts, so they stay lifted as its
input); 3 Edge zlib -- inflate_direct, the lifted shell when it declines; 4 collision and 5 the solver -- native when
the settings ask (the player's dotheroes.cfg does: DOTHEROES_NATIVE_CD=1, DOTHEROES_NATIVE_SOLVER=1,2,4,5,6), lifted
otherwise and for the cases they decline (none seen); 6-26 FMOD's -- not lifted from M5. What blocks retiring the
machinery is these fallbacks and their declines: M6 makes native the default in code, covers or makes fatal every
decline, and keeps the lifted images only as the generator's input.

**The per-store test** (the owner's question). Every lifted scalar store -- about 780,000 write sites in the lift, and
737 vector stores -- tests its 128-byte line's bit in the coherence bitmap before writing (DOTHEROES_PPU_STORES=mov,
the release's and the setup's default on Windows x86-64): a load, a shift, a test and a branch, though the bitmap's
bits are set only by an SPU GETLLAR or a SPURS task's signal watch and never cleared [R]. Its measured price is 0.4 ms
of the game thread's 4.2 ms exploring and of its 5 ms in a fight -- about a tenth -- with the range test and the
diagnostics' span 0.2 ms more (docs/NATIVE_R8.md section 5.4: fields 234-235 fps with the test, 259-260 without it,
measurement-only builds) [D]; measured with FMOD running, whose mixer took two GETLLAR and a PUTLLC per block -- the
main live user, gone at M5. It is a price, not a candidate, while anything can reserve a line the PPU writes; after M5
and the fallbacks, nothing in the native configuration does (to be established by the census). The vector stores'
test goes with it (PPU_RAW_ST_NO_COH).

**M6's benchmark** (the owner's list), with what exists [R]: tools/r8_bench.py (fps; mean, p50/p90/p99/p99.9, max;
the share of frames over 6.94 ms and 5 ms; the game thread's busy time; heavy and light frames); the builds per store
mode (tools/r8_release_build.sh -DDOTHEROES_PPU_STORES=...; the measurement modes need DOTHEROES_MEASURE_ONLY=1); the
A/B scripts of R8 and M3 (uncapped with DOTHEROES_BENCH=1; the 60 fps cap; three rounds rotated, real audio); 120 and
144 fps targets with DOTHEROES_FPS=60 and DIAG_VBLANK_MS=8.333 or 6.944; spikes from tests/slow_frames.py and
frame_times.py. **Missing**: per-thread CPU on Windows -- the process's CPU (memory.csv) exists, per thread only
sampled profiles in development builds; M6 adds a per-thread CPU-time reading (GetThreadTimes) to the runs. The arms:
rel27, rel31 (M3) and the M6 build, on the river fight and the fields, each target three rounds.

### 10.1 M6 under way (2026-11-12): the static census, the counters, the tools

**Where the game's own code can start SPU work** (tools/spu_census_static.py over the M5 lift, gen14: every call
site of the cellSpurs, cellSync and cellAudio imports and of the SPU syscalls, each caller marked excluded, hooked
or lifted) [R]. A scene that starts no SPU program shows what that scene does; this shows what the whole executable
can do:
- **cellAudio**: the port calls (open, start, stop, close, the port's configuration) are all in one cluster of the
  executable, Bink's sound output (M8 retires it); the notification-queue calls FMOD made have no caller left.
- **SPURS, by owner**: the water's set-up and signal (hooked; three helpers behind the set-up still lifted, reached
  only through it); the inflate's SPURS shell and its lock-free queue (hooked; lifted only as the direct path's
  fallback); **the physics manager's set-up** -- its SPURS instance, tasksets, queues and barriers -- **still lifted
  and run** (its task creation and queue pushes hooked: no task ever runs), which is why the game still creates two
  SPURS instances. Retiring SPURS means hooking that set-up as the water's was (marker values in place of the SPURS
  objects), after reading in the lift what else reads them.
- **SPU syscalls in the game's code**: the SPU-printf server's local-store reads (it serves SPU programs' printf),
  one sys_spu_initialize in the game's start-up, one image open; the thread groups themselves are created only by
  the runtime's SPURS.

**The counters** (ps3recomp include/ps3emu/census.h, runtime/platform/ps3_census.c): each run reports at its exit
how often it used each piece of the machinery -- SPU atomics, line watches, PPU stores reaching a line an SPU holds
(the per-store test's slow path), SPU DMA, mailboxes and signals, overlays, lv2 SPU thread groups, SPURS instances
and tasks -- and says the first use of each with what it was. PS3_CENSUS_FATAL=<kinds> makes a kind's first use
fatal: the diagnostic assertion to turn on for each kind the census shows unused, so a part of the game no run has
reached cannot use it unnoticed.

**The tools**: tools/thread_cpu.py (each thread's CPU time from the system's own accounting, named as the runtime
names them; the port's water, waver and audio-stream threads now named too); tools/r8_bench.py with the mean, p95,
the worst frame and the frames over twice the median; build/r8/m6bench_run.sh (local): 60, 120 and 144 fps caps and
uncapped, the river and the fields, two rounds rotated, each thread's CPU beside each run.

### 10.2 The fallbacks, case by case (2026-11-12): which the game can reach

The owner's rule for M6 (2026-11-12): every fallback case the game can legitimately reach gets a proper native
implementation before the machinery behind it is removed; a fatal diagnostic detects unexpected execution and is
no substitute for supporting a real gameplay path. So each case where a native path hands work back to the lifted
SPU code or to SPURS is classed by what the whole executable and the player's data can do, not by what the test
scenes did:
- **reachable**: the game's code or data can produce it (or nothing rules it out) -- it gets a native implementation;
- **unreachable, shown statically**: no code in the executable can construct or issue it, read in the executable and
  the lift (below) -- a fatal diagnostic, with the evidence recorded beside it;
- **corrupt input**: only a damaged installation produces it -- handled natively as the game's own code handles it.

**The evidence** [R]: tools/class_census_static.py reads the executable's RTTI (130 classes: Bullet's, the SPU
support's, a few others; the game's own classes have none), finds each class's vtable, every instruction that puts
its address in a register (lis/addi pairs, and TOC loads with each function's own TOC -- the executable has six,
one per group of object files, read from the function descriptors), the function each is in and that function's
callers in the lift. A class whose vtable is materialised only in code nothing calls cannot be constructed. A
second check compares every vtable in the data with each concrete Bullet shape's: no other vtable resembles one
(the game defines no shape subclass of its own).

| Path | Case | Class | Evidence | M6 |
|---|---|---|---|---|
| collision (bullet_cd) | box, cylinder (any axis) or triangle mesh pairs | native today | -- | -- |
| | another shape type: sphere, capsule, cone, convex hull, compound, plane | unreachable | the executable links only box, cylinder (X, Y, Z), sphere, BVH triangle mesh and triangle shapes; no capsule, cone, hull, compound or plane class exists. The sphere: the game's own sphere maker (0x000DC2E8, beside its box, cylinder and mesh makers) has no caller and no pointer to it anywhere; Bullet's other spheres are stack temporaries of the PPU's CCD and sweep tests, never in the pair cache | fatal diagnostic |
| | mesh against mesh | unreachable | one mesh maker, static objects only; Bullet's broadphase filter keeps static pairs apart | fatal diagnostic |
| | a BVH layout or index type it does not read | unreachable | the game's only mesh maker (0x000DC8EC) has Bullet build the BVH at load (quantized, built: r5 = r6 = 1 at 0x000DCDA4), with 16-bit indices and 12-byte vertices | fatal diagnostic |
| | more than one work unit a task | unreachable | Bullet 2.74's dispatcher issues one unit a task (one page of one unit) | fatal diagnostic |
| solver | commands 1, 2, 4, 5, 6 | native today | -- | -- |
| | command 3 (typed-constraint set-up), cells with typed constraints | unreachable | the only typed-constraint class is btRaycastVehicle (no hinge, point-to-point, cone-twist, 6-DOF or slider class exists), and its constructors have no caller | fatal diagnostic |
| physics issue (phys_direct) | 64 jobs out at once (the pool) | reachable in principle | the game's protocol stays far below, but nothing in the native layer guarantees it | native: the job runs on the issuing thread |
| | 8 managers with work out, an address reused while its work is out | reachable in principle | the game makes two managers | native: the table grows; a reused address keeps its own entry |
| | a manager running another task image | unreachable | the issue and wait functions (func_00955CF0, func_00955BD4) are called only from two virtual methods each, slots 2 and 3 of BulletCollisionSpursSupport's and BulletConstraintSolverSpursSupport's vtables: the collision and solver images | fatal diagnostic |
| inflate (inflate_direct) | gzip header with only FNAME, whole packs | native today | all 167 map packs on the disc (the only input: the map-pack loader is the inflate's only caller) | -- |
| | FEXTRA, FCOMMENT, the reserved bits 0x40 and 0x80 | reachable (the game accepts them) | none on the disc, but the game's own header walk (func_0052EE34) takes them -- FEXTRA by adding XLEN without the length field's two bytes, which is the game's to keep | native: the game's walk, exactly |
| | compressed data bigger than its output | reachable in principle (none on the disc) | Edge's push (func_0069C60C) only prints a warning ("Pointless?") and pushes the request as any other | native: inflated, the warning printed |
| | FHCRC, bit 0x20, another magic or method | corrupt input | the game's walk refuses them by returning a null data pointer, which its caller (func_005CF228) passes on unchecked: on a PS3 the SPU task would read from address 0 and fault | native: nothing written, the counter's error bit set, a line naming the pack (the fault is not reproduced; the game goes on with what the output holds) |
| | an inflate that fails or a CRC that differs | corrupt input | the SPU task ORs 0x80000000 into the counter and nothing reads it: the game goes on with the output as written | native: the same (the output as inflated, the counter left at 0x80000000) |
| | buffers on a stack, memory not mapped | unreachable | the loader's buffers are heap allocations; Edge's own PPU side refuses a stack buffer | fatal diagnostic |
| water, waver | the quit, an unsupported step | native today | with the set-up hooked (the player's settings) every signal is handled natively, the quit included; an unsupported step is one the SPU task would halt on | -- |
| generated code | absent (a build without the player's generated water and waver) | build configuration | the player's setup always generates it | the release build requires it (no lifted fallback linked) |

**Done (rel42, validated 2026-11-12; build/r8/m6a_run.sh):**
- *The physics issue* (port/native/phys_direct.cpp): the job pool grows in chunks of 64 when every job is in use,
  and the manager table grows and keeps one entry per (address, image), so an address reused while its old work is
  out gets an entry beside the old one; neither case goes to SPURS any more.
- *The inflate* (port/native/inflate_direct.cpp): the header is walked as the game's walk walks it (FEXTRA with the
  game's arithmetic, FNAME, FCOMMENT, FTEXT and bits 0x40/0x80 ignored; port/native/gz_walk.h, every branch checked
  by tests/native/gz_walk_check.cpp: 42 checks pass); compressed data bigger than its output is inflated, as Edge
  only warns of it; a damaged pack (a header the game refuses, a name or comment running into the trailer, a
  stream that does not fill the output) is
  finished natively as the SPU task finishes a failed inflate -- the output as inflated, the error bit in the
  counter, which nothing reads -- with a line naming the pack. Declined still: another static block, a buffer on a
  stack or outside guest memory, a failed host allocation (none of them the game's code can produce).
- *The defaults*: native collision and the five native solver commands are the code's defaults (port/main.cpp,
  after dotheroes.cfg is read, so a player's own value still wins); water and waver were already.
- *The census*: a tenth kind, native_decline -- a native path handing work back to the lifted code (a = 1 the
  physics issue for an unknown manager, 2 a collision task, 3 a solver task, 4 the inflate, 5 the water's signal
  for a block not its own). Every run reports it; PS3_CENSUS_FATAL=native_decline is the fatal diagnostic the table
  names for the unreachable cases, turned on for M6's validation and made unconditional when the SPU machinery
  behind the declines is removed.

**rel42's validation** [M] (rel41's lift, gen14, with the above; PS3_CENSUS_FATAL=native_decline and the inflate's
CRC check in every run): no crash, no trap, no fatal diagnostic, native_decline 0 in every census.
- The memory test (695 of 695); the 11 validation drives; Vulkan validation (0 messages); the damage run; the menu's
  quit (exit 0); the movie with sound; a 30-minute session (the working set 969 MB from the fifth minute, flat;
  rel41's 960 MB); the seven save flows (each wrote the folders expected); the reference scenes (5 of 5 within).
- The water check: 2,498 of 2,498 steps equal. The waver check: 1,093 of 1,100 (the known mid-ripple class).
- Every pack the runs loaded: 40, inflated natively, every CRC-32 the trailer's. No physics work went to SPURS; the
  job pool never grew and no address was reused with work out (the growth paths exist for what the scenes do not
  reach).
- The A/B against rel41 (uncapped, two rounds each, rotated):

  | Scene | rel41 fps | rel42 fps | rel41 p99 (ms) | rel42 p99 (ms) |
  |---|---|---|---|---|
  | River fight | 211.9 | 210.9-211.2 | 6.48-6.50 | 6.48-6.68 |
  | Fields | 243.7-246.4 | 242.2-244.5 | 5.59-5.85 | 5.58-5.69 |

  The river 0.3% slower in both rounds (the game thread's busy time 4.68 against 4.66 ms a frame; the census's
  counters the likely cost); the fields within each other's range.

### 10.3 M6's baseline (rel42, 2026-11-12): frame times and each thread's CPU

build/r8/m6bench_run.sh: the river fight and the fields (the waver started four times), the game capped at 60, 120
and 144 fps (a frame a vblank, the vblank clock at 16.67, 8.33 and 6.94 ms, mailbox present) and uncapped, two
rounds rotated, real audio; tools/r8_bench.py over the frame trace's 60 s window; tools/thread_cpu.py over 50 s of
it [M]. One run (the river at 120, round 1) was overlapped by a file search of the developer's and was run again.

| Scene | Cap | fps | p50 (ms) | p99 (ms) | p99.9 (ms) | Worst (ms) | Spikes | Process (cores) | Main thread |
|---|---|---|---|---|---|---|---|---|---|
| River fight | 60 | 60.0 | 16.68-16.69 | 17.32-17.45 | 17.53-17.59 | 17.8 | 0 | 1.54-1.56 | 99% |
| River fight | 120 | 120.0 | 8.35-8.36 | 9.22-9.37 | 9.57-10.53 | 12.6 | 0 | 1.69-1.74 | 99% |
| River fight | 144 | 140.8-143.3 | 6.94-6.95 | 9.11-11.32 | 10.94-13.86 | 86.4 | 0-8 | 1.84 | 99% |
| River fight | none | 203.5-204.2 | 4.75-4.77 | 7.12-7.25 | 8.77-9.32 | 12.4 | 4-6 | 1.91-1.94 | 99% |
| Fields | 60 | 60.0 | 16.68 | 17.21-17.50 | 17.60-17.63 | 18.3 | 0 | 1.47-1.49 | 99% |
| Fields | 120 | 120.0 | 8.36-8.37 | 9.30-9.35 | 9.58-10.17 | 11.3 | 0 | 1.52-1.54 | 99% |
| Fields | 144 | 144.0 | 6.94 | 7.40-7.46 | 9.01-9.27 | 13.3 | 0 | 1.59-1.61 | 99-100% |
| Fields | none | 235.9-240.3 | 4.13-4.17 | 5.79-6.13 | 7.50-8.35 | 10.1 | 12-15 | 1.68-1.73 | 98-99% |

(Spikes: frames over twice the median. Other threads: the native graphics 31-69% of a core, rising with the frame
rate; 22 threads the runtime does not name -- the GPU driver's, OpenAL Soft's, the system's pool -- 7-10% together;
water or waver 2-6%; the audio stream 3.5%; the SPURS kernel 0.4-0.8%.)

What it shows:
- **60 and 120 hold** in both scenes: no frame over 18.3 ms at 60, none over 12.6 ms at 120. **144 holds in the
  fields** (p99 7.4 ms). **The river fight does not hold 144**: its heavy-debris frames reach 10 ms (p99 9.1 ms in
  the round without a stall), the game thread's own work in them more than a 6.94 ms frame.
- **The 86.4 ms frame** at 144 (round 1) was the title-screen freeze's cause in a fight: the main thread 75.4 ms in
  cellPadGetInfo2 while SDL re-enumerated after a device change elsewhere on the system ("[cellPad] SDL controller
  update took 78 ms"), then a run of shorter ones. The pad thread (docs/PC_EXPERIENCE.md section 7b) removes it;
  validated apart from these measurements.
- **The main thread uses a whole core at every cap.** Its own work is 4-5.6 ms a frame; the rest is the game's
  frame wait at 0x00029CC0, which calls sys_timer_usleep for about 200 us some 3,300 times a second, and the
  runtime's usleep spins below 1 ms (sys_timer.c: a QPC deadline, SwitchToThread in the loop) -- 40 s of the 60 s
  window at 60 fps. The loop [S] (func_00029BD8) is the flip wait of Sony's samples: cellGcmGetFlipStatus
  (NID 0x72A577CE) until it returns 0, sys_timer_usleep(200) between the calls, then cellGcmResetFlipStatus
  (0xB2E761D4) -- the main thread waiting for its previous frame's flip. The next M6 candidate: that wait made a
  sleep on the flip's completion (the runtime's flip status and the event that clears it; a hook at the loop or the
  status call), measured by these same runs. Not a correctness issue; a core's worth of power and
  heat, and a core taken from the other threads on a machine with few.
- **SPURS still runs a kernel thread** (0.4-0.8% of a core) for the physics manager's two instances, which the
  game's own set-up creates and no task uses; its three worker threads stay idle. Their retirement (section 10.2)
  removes it.

### 10.4 The main thread's waits, and SPURS's threads (2026-11-13; rel44-rel46)

**The short sleeps** (ps3recomp patches 0152 and 0154; rel44, then rel46). The main thread paces itself with two
loops in func_00029BD8 [S]: it waits for a flag its vblank handler sets (0x00029C20, sys_timer_usleep(300) between
the looks), then for the previous frame's flip (0x00029CC0, the samples' cellGcmGetFlipStatus loop,
sys_timer_usleep(200)). The runtime spun every sleep under a millisecond to its deadline, so the main thread took a
whole core at every frame cap. Such a sleep now waits on the GCM delivery event (set when a vblank or a flip's
completion is pending) and a high-resolution timer: a delivery runs the pump at once -- the handler run, the flip
marked done, as at a syscall's entry -- and the rest of the requested time is still spun, so a sleep never ends
early and the game sees its handler's change no later than before; a thread in the flip loop keeps waiting through
a vblank that did not complete its flip. A sleep no delivery interrupts ends with the timer, about 0.3-0.4 ms past
its deadline (the timer's granularity here is 0.5 ms). Across every drive, the save flows, the quit and the movie,
the only sleeps under a millisecond are the frame pacing's three call sites (0x00029BF4, 0x00029C20, 0x00029CC0) and
one during the quit. PS3_FLIP_WAIT=spin restores the spin; =flip the flip loop alone (rel44's form). Reported at
exit: how the sleeps ended, by call site, the missed wake-ups, and the time from a flip's completion to the game's
next frame (its next cellGcmResetFlipStatus).

The same binary against itself (rel46; PS3_FLIP_WAIT=spin the other arm; uncapped and capped at 30, 60, 120 and 144
fps; two rounds rotated; build/r8/m6ss_run.sh) [M]:

| Cap | Main thread, slept / spun | Process (cores), river | River p99 (ms) | Fields p99 (ms) |
|---|---|---|---|---|
| 30 | 16-20% / 25-26% | 0.64-0.71 / 0.70-0.76 | 33.51-33.53 / 34.01-34.14 | 33.52-33.73 / 33.96-34.25 |
| 60 | 28-38% / 99% | 0.86-0.91 / 1.50-1.53 | 16.85-16.86 / 17.22-17.25 | 16.88-17.02 / 17.22-17.26 |
| 120 | 54-65% / 99% | 1.31-1.33 / 1.68-1.73 | 8.69-8.70 / 9.32-9.35 | 8.69 / 9.24-9.37 |
| 144 | 64-76% / 99% | 1.59-1.61 / 1.83-1.85 | 7.12-7.18 / 7.46-7.47 | 7.04 / 7.40-7.43 |
| none | 99% / 99% | 1.88-1.93 / 1.92 | 6.42-6.54 / 6.49-6.69 | 5.69 / 5.60-5.61 |

- Every cap holds, with better pacing: p99, p99.9 and the worst frame lower at 30-144 (the river at 120: the worst
  9.9-10.3 ms against 10.7-16.3).
- The game's own work a frame is 0.2-0.3 ms longer at 120 and 144 after a sleep (a core's caches and clock, cold
  after sleeping) -- inside every frame budget.
- Uncapped the main thread is busy all the time and the frame rates overlap (river 209.8-214.4 against
  212.2-212.6 fps, fields 247.0-248.0 against 246.7-249.3); the fields' p99.9 0.4 ms higher (7.78-8.16 against
  7.32-7.41 ms), from the waking.
- The flip's completion to the next frame (input delay at the frame's start): at 60, 105-125 us on average against
  154-169 us spun (rel44's runs); at 120 and 144 unchanged, 90-110 us. Missed wake-ups (a completion while another
  waiter took the delivery event, caught by the timer within half a millisecond): 1-21 a 150 s run.
- The validation set on rel46 (the memory test, 11 drives, the menu's quit, the movie, seven save flows): no crash,
  trap or fatal diagnostic; quitting while the main thread sleeps in the flip loop exits 0.

**SPURS's threads** (patch 0153; rel45). The physics set-up's two instances made a kernel thread (a pass every
millisecond, 0.4-0.8% of a core) and three policy-module threads that never ran. An instance's threads now start
with its first workload, task or job chain. In every native run none started (39 threads where there were 43); the
water and waver check modes, which create SPURS tasks, start them on demand (the water check 2,462 of 2,462 steps
equal, the waver 1,151 of 1,158 frames, the known class). PS3_SPURS_EAGER=1 starts them at initialisation.

# Audio: FMOD Ex out, OpenAL Soft in -- audit and migration plan (for review)

Status: **approved by the owner 2026-11-11 with the adjustments below; M0-M3 done (sections 4.1-4.4), M4
(the owner's listening) next; M5 and M6 only once their validation requirements are met; M7 (history) needs a separate
approval; M8 (Bink) afterwards.** Requirement (the owner, 2026-11-11): the PC recompilation's game audio runs on OpenAL Soft; FMOD Ex is not executed and no
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
| Device, context, failure, shutdown | ALC device open on the default output (stereo, 5.1, 7.1 as the system reports; OpenAL Soft's own mixing and downmix); disconnect detection (ALC_EXT_disconnect) and reopen (ALC_SOFT_reopen_device); clean release of sources, buffers, effects at shutdown |
| Effects (SE) | the 281 effect files resident as AL buffers (33.7 MB): our own RIFF/PCM loader (all files are 16-bit PCM, mono or stereo, 44.1 or 48 kHz; no other codec), reading the `smpl` loop points (AL_SOFT_loop_points) [M] |
| Music (BGM) | our own streaming thread: buffer queue (e.g. 4 x 250 ms) from the WAV file on disk; the loop as rel27 plays it (the `smpl` loop with its intro, or the whole file: M0 decides), crossfades with two streams; underrun detection and counters |
| 3D | AL_INVERSE_DISTANCE_CLAMPED with reference = the list's min, max = its max, rolloff 1 (FMOD's default inverse rolloff with the game's set3DSettings(1,1,1)); listener from setListener, forward (0,0,1), up (0,1,0), never turning. The game's frame is FMOD's default left-handed one (System::init flags 0: +x is the right), OpenAL's right-handed: x is mirrored (taken as written, the plan would have swapped left and right). A 3D effect played with no position sounds at the listener (measured in rel27's mix: loud and centred), not at the world origin [C, M] |
| Reverb | an EFX EAX reverb in one auxiliary slot, every source sending to it, the slot enabled by setReverb: the game's settings are FMOD's default SFX reverb with Room -100 mB, applied to the whole mix, music included [C] -- in I3DL2 terms gain 0.89, HF gain 1.0, decay 1.0 s, HF ratio 0.5, reflections off (-10000 mB), reflections delay 0.02 s, late gain 1.0, late delay 0.04 s, diffusion 1.0, density 1.0, HF reference 5 kHz, LF reference 250 Hz, dry full. EFX implements I3DL2's model itself; we set the parameters, nothing of FMOD's reverb code |
| Voices | a voice cap (FMOD's default 32 real voices) chosen by priority then audibility; 100 virtual |
| Pitch | not used by the game (no frequency or pan calls) [C] |
| Pause/resume, looping | AL source pause/play; whole-buffer loop for looping SE; music loop by the stream |
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
  track) plays as a stereo copy straight to the outputs at 0.707 a side, as rel27's mix had it (0.712 measured in
  the sound lab's capture; OpenAL's own centre on stereo speakers is 1.5 dB lower: 0.603 measured in the game
  before this was done; 0.715 after).
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
  section 4). The river's -0.67 dB is mostly this (two rel27 runs of it differ by +0.08 dB). The owner's decision:
  no compensating gain on positioned effects, and the old downmix's directional attenuation is not recreated (it
  was an artefact of FMOD's 7.1 output folded to stereo, not the game's intent). Centred 2D mono keeps rel27's
  level (0.707 per side, above): that is the game's own non-positioned mix, not a pan law.
- OpenAL Soft holds a finished one-shot's last frame up to 18 ms (section 4.3); EFX's reverb is its own
  implementation of the same I3DL2 parameters (its tail is not sample-identical); positions are applied every frame
  (FMOD applied them at its ~9 Hz update); voices beyond 32 go virtual by priority, then quietness (never reached:
  6 at most in every scene); the sound lab's in-update music starts one update earlier.
- **Not tested**: the boss fight and a crowded scene (no drive reaches them: the owner's sessions, M4); the system
  menu's pause (no drive opens it); a physical unplug of the output device (simulated in M2).

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
  audio does not go through FMOD or cellAudio [C]; how it is played is established in the Bink milestone.

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

## 8. Architecture requirements (to be written into docs/NATIVE_PORT.md and CLAUDE.md at M1)

Native Vulkan graphics; OpenAL Soft audio; FFmpeg-based video decoding; no permanent proprietary middleware
implementation in public source; game files and keys supplied by the player; no unnecessary PS3 execution
machinery in the final architecture; stable 60 fps with minimal frame-time spikes; correct behaviour across
gameplay, menus, saving, loading and quitting. A decision that conflicts with these is flagged to the owner,
not made silently. docs/AUDIO.md's "switching audio libraries is not the plan" is withdrawn.

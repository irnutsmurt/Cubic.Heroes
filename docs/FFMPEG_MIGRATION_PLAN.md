# M8: the movies through FFmpeg -- a plan for review

**Status: approved by the owner 2026-11-12 (the architecture); M8.0 begun 2026-11-13 (section 2.1); M8.1 done
2026-11-13 on Windows (section 6.1; the Linux libraries are built, their run-time validation is outstanding with
the rest of Linux's).** The owner's requirement (CLAUDE.md, "Architecture
requirements"; docs/NATIVE_PORT.md section 1): the game's video is decoded by FFmpeg, dynamically linked under the
LGPL; Bink's implementation is not executed and not in anything this project distributes; the movies are the
player's own files, read as they are (no conversion, no converted copies, nothing of them distributed); the
movies' sound goes through OpenAL Soft, which retires cellAudio and SDL2's audio output. This plan follows the
audio migration's shape (docs/OPENAL_MIGRATION_PLAN.md): an inventory first, then the native part behind a
switch, validation against the lifted path, the owner's viewing, then the exclusion.

Labels: [M] measured, [C] read in the code, [L] seen in run logs, [I] inferred, [H] a hypothesis to test.
Addresses and file names are this game's (BLUS-30490); facts only, no code of the game or of Bink.

## 1. What is there now

**The movies** [M: their Bink headers, read by a script; FFmpeg's LGPL decoders decoded all five end to end with
no error in the feasibility study, docs/FEASIBILITY.md section 4]:

| File (data/movie/) | Size | Frames | Length | Sound | When the game plays it |
|---|---|---|---|---|---|
| TitleMovie_Omote_eng.bik | 160 MB | 2,160 | 72.0 s | none | the title screen's background, looped [L] |
| TitleMovie_Ura_eng.bik | 160 MB | 2,160 | 72.0 s | none | the same after the game is cleared [I: the name, "ura" = the reverse side] |
| OpeningMovie_eng.bik | 157 MB | 4,620 | 154.0 s | none | the opening (a new game) [I] |
| PlayDemoMovieUS.bik | 277 MB | 3,465 | 115.5 s | 48 kHz stereo, Bink audio (DCT) | the attract mode's demo [L] |
| CreditMovie_eng.bik | 518 MB | 7,860 | 262.0 s | 44.1 kHz stereo, Bink audio (DCT) | the ending's credits [I] |

All are Bink 1, version i (BIKi), 1280x720, **10000000/333333 frames a second** (30.00003: the header's rate, read
byte by byte and by FFmpeg's demuxer in M8.1; FEASIBILITY.md's 29.97 and this plan's earlier "30/1" are corrected
here -- the lengths above are rounded), and **each has one keyframe, its first frame**: a movie can be played from its start
and stopped, not entered in the middle -- skipping is closing. The title movie and the opening are silent: their
music is the game's own (the sound manager, OpenAL Soft since M4). The game's movie list (Movie.lua.eu) names the
files.

**How the game plays them** [C, L: docs/FEASIBILITY.md section 5, the M4 record]: Bink 1's library is statically
linked into the game and runs as lifted PPU code (no Bink SPU program is embedded); it creates a "Bink IO Thread"
(the file read ahead) and, for a movie with sound, a "Bink Audio Thread" that plays the sound through a 2-channel
cellAudio port of its own -- on the cellAudio that the native start-up initialises for it since M4 (its output:
the runtime's mix thread, SDL2's audio). The decoded frames become GCM textures ("Create_Bink_textures in
gcmtextures", the game's own code), which the game's renderer draws as one quad (the attract demo's frames: one
to 19 draws, the video's quad, no post-effects; docs/NATIVE_R2.md); the title menu is drawn over the title movie.

**Settled since** (2026-11-12, docs/PC_EXPERIENCE.md section 7b): the freezes were SDL re-enumerating input devices
after a device change, not the movie; fixed by ps3recomp patch 0151. The original note follows.
**Linked to an open question** [H]: the title-screen freezes of the owner's first M4 session (0.4-1.5 s about
every 2 s, at 12.6-21 s and 40.5-51 s, flips and presents both stalled; unexplained, tracked separately) happened
while the lifted Bink decoder played the title movie -- 160 MB streamed at about 2.2 MB a second by its IO
thread. Whether the movie is involved is to be established by evidence (the early-start trace, a controlled
reproduction) before M8 relies on it either way; the evidence is kept.

## 2. The boundary (M8.0: an inventory, nothing changed)

As M0 found the sound manager for FMOD, M8.0 finds the game's movie player: the game's own code that calls Bink's
API -- open a file, decode the next frame, copy it into the textures, ask whether the next frame is due, choose and
set the sound track and its volume, pause, close. Anchors in the executable [M]: Bink's strings ("Not a Bink
file.", "Error reading Bink header.", the two thread names), the thread creations that pass those names, the
movie's cellAudio port opening, the texture names in gcmtextures. A string scan alone is not enough (it matches
unrelated constants in large functions): the calls are traced instead.

- **Trace** (as M0's DOTHEROES_AUDIO_TRACE): entry hooks on the movie player's methods record each call, its
  arguments and results, the game frame and the time, on rel41 for every movie: the title loop for 5 minutes, the
  attract cycle (the demo twice), a new game's opening, and the credits (they need a save near the end: the
  owner's, or a session to reach it -- to be decided with the owner). Recorded also: which textures each frame
  writes (address, format, size), the frame pacing the game asks for, the sound track's volume.
- **Decide the boundary**: the game's movie class (preferred: the game's own calls, few, as the sound manager's
  were) or Bink's API functions (more of them, more of Bink's semantics to match). The trace decides.
- **Bink's extent in the executable**: as FMOD's was found (docs/OPENAL_MIGRATION_PLAN.md section 9: the library's
  range, then the out-of-line methods its tables hold), so that M8.4 can exclude it exactly.

### 2.1 M8.0 under way (2026-11-13): the anchors [S]

From the executable's strings and the lift's direct calls (gen15):
- **Bink's library**: func_00A5B438 holds "Not a Bink file." and "Error reading Bink header." (Bink's open); it has
  no direct caller (reached through a function pointer). The thread names "Bink IO Thread" and "Bink Audio Thread"
  sit beside those strings (0x00F55161, 0x00F5528F). Bink's code lies near FMOD's out-of-line methods
  ([0x00A42AA0, 0x00A440E0)); its exact extent is still to be drawn as FMOD's was.
- **The engine's movie manager**: aspMovieScheduleManager.cpp (asp, the game's engine), functions
  0x006E5C50-0x006E68C8; they call a further layer at 0x00A25EC0-0x00A26020 (a movie player between the manager and
  Bink, to be confirmed) -- the likely boundary, as the sound manager was FMOD's.
- **The game's movie opens**: "%s/movie/%s" built in four functions (0x0048C424, 0x004A9814, 0x00585BF0, 0x005CD374):
  four places that start a movie (the title, the attract mode, the opening, the credits: to be confirmed by the
  trace).
- **The movie's textures**: gcmtextures.cpp, func_0045AE14 and func_0045B55C (the game's own texture creation).
Next: the chain from the manager to Bink's API, Bink's extent, then the run-time trace (section 2).

## 3. The native movie player

- **Reading** [design]: FFmpeg's Bink demuxer (libavformat) reads the player's .bik through a custom I/O context
  on the runtime's file access (the disc's folder, as the game's own file opens map it) -- the file as it is, read
  ahead on the decoder's thread. Nothing converted, nothing cached to disk.
- **Decoding**: libavcodec's Bink video decoder (configure's name `bink`, the codec binkvideo; planar YUV 4:2:0,
  with the alpha plane where a file has one -- these five have none [M: the video flags; M8.1: every frame
  yuv420p]) and binkaudio_dct (binkaudio_rdft for completeness), on a decoder thread of the
  movie's own; three or four decoded frames ahead, so the game's thread never waits for a decode except when the
  frame due is not ready (then the previous frame stays).
- **Video on Vulkan**: the Y, U and V planes uploaded as textures and converted to RGB in a shader at the point
  the game's quad samples the movie. Two ways, to be chosen by measurement in M8.2: (a) the converted frame
  written into the game's own movie textures (the game's renderer draws it as now, the title menu over it -- the
  smallest change), or (b) the native renderer binds the movie's planes in place of the game's texture for that
  quad (no CPU conversion; 1280x720 at 30 a second is 1.4 MB of planes a frame against 3.7 MB of RGBA). The colour
  conversion (the matrix and range Bink's own copy-out uses) is taken from the lifted decoder's output, measured,
  not assumed.
- **Sound**: Bink audio decoded to float PCM and played through OpenAL Soft as a stream of its own (the music's
  streamer: port/native/audio/al_stream.cpp), at the file's rate (44.1 or 48 kHz; OpenAL Soft resamples), with
  the game's volume for it (from the trace: the sound track's volume, the options' levels).
- **The clock and A/V sync**: for a movie with sound, the sound's own position is the clock (OpenAL Soft's
  played-sample position with its latency, AL_SOFT_source_latency); the frame due is position x 10000000/333333
  (the header's rate, not 30). For a silent movie, a steady clock from its start. [M8.1, M] The sound was made
  to the picture's length: the declared samples last 115.49990 s against the demo's 3,465 frames' 115.49988 s,
  261.99975 s against the credits' 261.99974 s. Every sound packet decodes to exactly the samples it declares
  but the last, whose final transform block FFmpeg decodes whole (965 and 371 samples more, 20 and 8 ms): the
  player plays the declared samples, so the sound ends with the picture. The game's "is the next frame due?" calls answer from that clock; a
  pause (the game's, or the system's) holds the clock and the sound together.
- **Timing at every frame-rate cap**: the movie advances by its clock, not by the game's calls: at 60, 120, 144
  and uncapped the same frame is shown for 2, 4, 4.8 and more game frames; at 30 (the game's own cap) one each. A
  game frame slower than a movie frame (a hitch) shows the frame due then -- skipping the ones between, as a
  player does -- and the sound goes on unbroken. Tested at each cap (section 5).
- **Skipping, transitions, shutdown**: a close from the game (a press that skips, the title loop's end and
  restart, the attract mode's cycle back to the title, quitting) stops the decoder thread, drains and releases
  the sound's source and buffers, and frees the frames, at once; a new movie opens cleanly after it. Quitting
  during a movie leaves no thread, AL object or file handle.
- **Device recovery**: the movie's stream is one of the audio layer's streams: a lost output (unplugged, the
  default changed) reopens as the music does since M4, the sound resuming where it was; the picture follows the
  sound's clock, so it stays in step through the loss.

## 4. Licensing and the build

- **FFmpeg**, a pinned release, fetched from its official source with the archive's SHA-256 recorded (the owner
  approves the download first, as for OpenAL Soft), built by tools/build_deps.py (tools/build_ffmpeg.py) as
  shared libraries. As built (M8.1, section 6.1): `--disable-everything --disable-autodetect --disable-asm
  --disable-programs --disable-doc --disable-network --disable-avdevice --disable-avfilter --disable-swscale
  --disable-swresample --enable-shared --disable-static --enable-demuxer=bink
  --enable-decoder=bink,binkaudio_dct,binkaudio_rdft` (GPL and non-free parts are off by default; the video
  decoder's configure name is `bink`) -- avutil, avcodec, avformat: 2.0 MB on Windows, 1.4 MB on Linux
  [M]. The libraries go beside the game and
  are loaded at run time by full path (as OpenAL Soft is: port/native/audio/al_api.cpp), so a player can replace
  them; the licence (LGPL-2.1-or-later for this configuration), the source location, the version and the
  configure line ship with them, and docs/LICENSE_AUDIT.md and the README list them.
- **Bink**: RAD's SDK is not used or shipped; FFmpeg's Bink decoders are its own LGPL implementation. After M8's
  validation the game's Bink library is excluded from the player's lift as FMOD's was (config/excluded_code.json:
  traps, ps3recomp ppu_lifter.py --traps), its threads never created.
- **cellAudio and SDL2's audio**: with the movies' sound on OpenAL Soft nothing opens a cellAudio port. The native
  start-up's cellAudio initialisation (port/audio_hooks.cpp) goes, then the runtime's cellAudio output path and
  SDL2's audio subsystem (SDL2 stays for the window, the controllers and the keyboard); the game's cellAudio
  imports answer as an uninitialised cellAudio does, and an M6-style census shows no call in any run.

## 5. Validation

Against rel41 (the lifted Bink, its sound through cellAudio), the developer's local captures only:
- **Pictures**: every frame of each movie decoded off-line by both (the lifted decoder's textures captured from
  the game, FFmpeg's from the file): frame-by-frame PSNR and the largest difference, the first frame compared
  exactly; the colour conversion against the game's own output.
- **Sound**: the demo's and the credits' sound in the mix against rel41's cellAudio capture (level and
  correlation, 10 s windows, as M4's comparisons); no underrun.
- **A/V sync**: the offset between a sound and its picture (the demo's own events), against the lifted path's,
  at 30, 60, 120, 144 and uncapped; held through a hitch made on purpose and through a device loss
  (DOTHEROES_AUDIO_TEST_LOSS during the demo).
- **Behaviour**: 100 skips at random moments of each movie; the attract cycle for 45 minutes (memory flat, no
  leak of threads, AL objects or handles); the title loop for an hour; a quit during each movie; the title
  menu over the title movie; the credits on a cleared save.
- **Cost**: the decoder thread's CPU against the lifted Bink's threads, and the frame times while a movie plays
  (p95/p99/p99.9) at each cap.
- **The title-screen freezes**: whether they occur with the native player (and, before that, whether the lifted
  decoder causes them).
- **The owner's viewing** of every movie, on rel27 and on the native build.

## 6. Milestones

| # | What | Gate |
|---|---|---|
| M8.0 | The inventory: the movie player's calls traced for every movie, the textures, the pacing, Bink's extent | the trace of each movie; the boundary chosen |
| M8.1 | FFmpeg built (LGPL, shared, pinned) and loaded; a standalone test decoding the five movies from the player's files | every frame and every sample decoded; the licence files shipped |
| M8.2 | The native movie player behind a switch (the lifted Bink the default) | the validation of section 5 except the owner's viewing |
| M8.3 | The owner's viewing; the native player the default | the owner's acceptance |
| M8.4 | Bink excluded from the lift (traps), cellAudio's initialisation and SDL2's audio retired | the validation set, the install test, no trap reached, no cellAudio call |

### 6.1 M8.1: FFmpeg built, loaded and decoding (2026-11-13) [M]

- **The source**: FFmpeg 9.0.2 from the project's official GitHub mirror (ffmpeg.org refused the build host's
  connections that day), the tag n9.0.2 = commit 946fcce07b6dcd0331c8cc609192aeff5e1924f8, the archive's
  sha256 6e374ed6...27e9 pinned in config/deps.sha256. Used as it is.
- **The build** (tools/build_ffmpeg.py, called by tools/build_deps.py): FFmpeg's own configure (section 4's line)
  under Git Bash on Windows, cross-compiling with the bundled Zig for x86_64-windows-gnu and
  x86_64-linux-gnu.2.31; without make, the driver compiles the objects configure selects (186: avutil 93,
  avcodec 57, avformat 36) with configure's flags and links each library with FFmpeg's version script (Windows:
  the .def its makedef would derive; Linux: soname, symbol versions LIBAVUTIL_61 ..., runpath $ORIGIN,
  stripped as `make install` does). One correction to configure's output, Linux only, listed in the shipped
  notice: config.h's HAVE_SYSCTL set to 0 (configure's link-only probe finds glibc's compatibility symbol;
  the header is gone since glibc 2.32; the path is never taken on Linux). No build path is in the libraries:
  the tools are named, not given by path, in the configure line avutil embeds, and `__FILE__` is mapped to
  FFmpeg-n9.0.2/ (checked: no hit for the folder, a home or user path in any of the six libraries).
- **The libraries**: avutil-61.dll 0.93 MB, avcodec-63.dll 0.59 MB, avformat-63.dll 0.49 MB (624/153/153
  exports, the same on both platforms); libavutil.so.61 0.69 MB, libavcodec.so.63 0.42 MB, libavformat.so.63 0.25 MB (needing libc, libm,
  libpthread; glibc 2.29 at most). av_version_info "n9.0.2", avcodec_license "LGPL version 2.1 or later".
- **Loaded the game's way**: port/native/movie/ff_api.cpp loads the three by full path from the game's folder
  (Windows) or its lib/ (Linux), in dependency order, every function the player needs looked up, and refuses a
  library of another major version (the ABI its headers declare). M8.2's player uses it.
- **What ships** (tools/zig_build.py's prebuilt folder, tools/setup_pipeline.py's install, tools/make_bundle.py's
  download): the libraries beside the game with FFmpeg-NOTICE.txt (the source, its sha256 and commit, the
  configure line, the correction, how to rebuild) and FFmpeg-LGPL-2.1.txt; in the setup's licenses/ the notice,
  the licence and licenses/source/: the release archive itself (FFmpeg-n9.0.2.tar.gz, the pinned sha256) and
  FFmpeg-build/ (the driver, config.h and config_components.h as built). tools/boundary_check.py requires all
  of them in a bundle that ships the libraries (tests/test_boundary_check.py), and the install test checks them
  beside the game.
- **Open, for the owner**: the boundary check refuses two files inside FFmpeg's archive,
  tests/ref/lavf/peak.wav and peak_only.wav -- FFmpeg's FATE references (three text lines each: an md5, a size,
  a CRC), refused by the .wav extension alone. Nothing else in FFmpeg's tree is refused. Either the check takes
  an exception pinned to those two files' exact bytes inside the pinned archive, or the shipped source leaves
  them out (as OpenAL Soft's HRTF file is, the notice saying so). CLAUDE.md forbids weakening a rule to clear a
  finding, so neither is done until the owner decides; until then make_bundle.py refuses to archive a bundle.
- **The standalone test** (tests/native/ffmpeg_decode_check.sh, on the player's own data/movie): each file read
  through an I/O context of the test's own (as the player will read it), every packet demuxed, every frame and
  sound sample decoded, the decoders flushed. **All five passed**: every frame decoded (2,160, 2,160, 4,620,
  3,465, 7,860, as their headers say), one keyframe each, 1280x720 yuv420p, the header's 10000000/333333; every
  sound packet decoded to its declared samples but the last (section 3, the clock); no warning from FFmpeg;
  each movie read once, front to back, without a seek. A frame's hash (FNV-1a over its planes) is kept per frame
  (build/m8/*.frames.txt) for M8.2's comparisons; two runs gave the same hashes for every frame and the same
  sound. Distinct frames: the opening holds 1,617 different pictures in 4,620 frames, the credits 6,301 in
  7,860 (still pictures held).
- **The cost** (one thread, C only, no assembly; the owner's PC): 1.91-3.40 ms a frame on average, p99 5.4-7.6
  ms, the worst 11.3 ms, against the 33.3 ms a frame lasts; the sound 124 ms for the demo's 115.5 s and 291 ms
  for the credits' 262 s.

## 7. Risks and open questions

- FFmpeg's Bink decoder is an independent implementation: its pictures may differ from RAD's by rounding (to be
  measured, M8.2); a visible difference goes to the owner.
- The game's movie code may count frames or read Bink's internal state in ways the trace must show before the
  boundary is fixed (M8.0).
- The credits need a save near the game's end for testing.
- The colour conversion and range Bink's copy-out uses (taken from measurement).
- The title-screen freezes' cause (section 1): if it is the lifted decoder, M8 removes it; if not, it stays its
  own investigation.

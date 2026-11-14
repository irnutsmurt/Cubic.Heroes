cmake_minimum_required(VERSION 3.20)
# Derived from ps3recomp templates/project (MIT, see LICENSE.ps3recomp).
project(dotheroes
    VERSION 0.1.0
    LANGUAGES C CXX
    DESCRIPTION "3D Dot Game Heroes (BLUS-30490) native recompilation"
)

# ---------------------------------------------------------------------------
# Standards
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Prefer Ninja if available
if(NOT CMAKE_GENERATOR MATCHES "Ninja" AND NOT DEFINED ENV{CMAKE_GENERATOR})
    message(STATUS "Tip: use -G Ninja for faster builds")
endif()

# ---------------------------------------------------------------------------
# The two directories this project is built from
# ---------------------------------------------------------------------------
# PS3RECOMP_DIR is the toolkit checkout: include/, runtime/, libs/, tools/.
# RECOMP_DIR is where the lifter's output for THIS game lives. They are
# separate on purpose -- a port keeps its generated code in its own tree and
# points at whatever toolkit checkout it is building against:
#
#   cmake -B build -G Ninja \
#         -DPS3RECOMP_DIR=/path/to/ps3recomp \
#         -DRECOMP_DIR=/path/to/my_game/recompiled
#
if(NOT DEFINED PS3RECOMP_DIR)
    # Default: the pinned checkout tools/pipeline.py uses
    get_filename_component(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../build/ref/ps3recomp" ABSOLUTE)
endif()
get_filename_component(PS3RECOMP_DIR "${PS3RECOMP_DIR}" ABSOLUTE)

set(RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../build/gen/recompiled" CACHE PATH
    "Directory holding the lifter's output: ppu_recomp.h and the lifted sources")
get_filename_component(RECOMP_DIR "${RECOMP_DIR}" ABSOLUTE)

message(STATUS "ps3recomp SDK:   ${PS3RECOMP_DIR}")
message(STATUS "Lifted output:   ${RECOMP_DIR}")

if(NOT EXISTS "${RECOMP_DIR}/ppu_recomp.h")
    message(FATAL_ERROR
        "No ppu_recomp.h in ${RECOMP_DIR}.\n"
        "That header and the lifted sources beside it are what "
        "tools/ppu_lifter.py writes. Run the lifter, drop its output there, "
        "or point -DRECOMP_DIR= at wherever it already is.")
endif()

# The lifted translation units. A game's lift is hundreds of generated files,
# so they are globbed rather than listed; re-run CMake after a fresh lift.
file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS
    "${RECOMP_DIR}/*.c"
    "${RECOMP_DIR}/*.cpp"
)
if(NOT RECOMP_SOURCES)
    message(FATAL_ERROR
        "${RECOMP_DIR} has a ppu_recomp.h but no .c or .cpp beside it. The "
        "lifted functions and the function table live in those; without them "
        "there is nothing to run.")
endif()
list(LENGTH RECOMP_SOURCES RECOMP_SOURCE_COUNT)
message(STATUS "Lifted sources:  ${RECOMP_SOURCE_COUNT} file(s)")

# Windows development builds (clang/Zig): CodeView line tables and a PDB next
# to the executable, so a debugger, Nsight and tests/prof_report.py (through
# DbgHelp) can name every host frame, the runtime's and the lifted functions'.
# Line tables only: names and lines at a fraction of the cost of full -g. Set
# before the runtime subdirectory so it gets them too; release packages
# (-DDOTHEROES_DEV_DIAGNOSTICS=OFF) stay without.
if(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND
   (NOT DEFINED DOTHEROES_DEV_DIAGNOSTICS OR DOTHEROES_DEV_DIAGNOSTICS))
    add_compile_options(-gcodeview -gline-tables-only)
    add_link_options(-g)
endif()

# ---------------------------------------------------------------------------
# ps3recomp runtime library
# ---------------------------------------------------------------------------
# Built from source as a subdirectory rather than imported as a prebuilt
# archive. A static library carries none of its own link requirements, so an
# imported one arrives without SDL2, pthreads, the Metal frameworks or the
# Winsock/XInput imports -- the link then fails on every platform for a
# different reason. add_subdirectory hands all of that over with the target.
# The native release build (docs/NATIVE_R7.md): the title's graphics are the
# native renderer's alone, from its libgcm calls at their entry (R7: no FIFO
# walker, no command words), so the runtime is built without the RSX command
# path (PS3RECOMP_NATIVE_GRAPHICS: no walker, method decoder, draw engine or
# backend drawing) and the port without R4's bridge to it. The development
# configuration (OFF) keeps all of them: the existing renderer stays the
# reference the native one is compared against.
option(DOTHEROES_NATIVE_RELEASE "Native graphics only: no RSX command path, no bridge" OFF)
set(HLE_NID_DEFINES "")
if(DOTHEROES_NATIVE_RELEASE)
    set(PS3RECOMP_NATIVE_GRAPHICS ON CACHE BOOL "" FORCE)
    # the NID table registers what this build compiles (no walker functions)
    set(HLE_NID_DEFINES --define PS3RECOMP_RSX_COMMAND_PATH=0)
endif()
# R8 (docs/NATIVE_R8.md): no SPU interpreter and no microstepper. Every SPU
# image this game runs is lifted or replaced natively, so SPU code with no lift
# stops the program with its image and address, whether strict mode is on or
# off -- nothing is interpreted at run time. DOTHEROES_SPU_INTERPRETER=ON
# brings both back for bring-up in a development build; a player package
# (DOTHEROES_DEV_DIAGNOSTICS=OFF) and a native release refuse it.
option(DOTHEROES_SPU_INTERPRETER "Development only: build the SPU interpreter and the microstepper" OFF)
if(DOTHEROES_SPU_INTERPRETER AND
   (DOTHEROES_NATIVE_RELEASE OR (DEFINED DOTHEROES_DEV_DIAGNOSTICS AND NOT DOTHEROES_DEV_DIAGNOSTICS)))
    message(FATAL_ERROR "DOTHEROES_SPU_INTERPRETER is for development builds only")
endif()
set(PS3RECOMP_SPU_INTERPRETER ${DOTHEROES_SPU_INTERPRETER} CACHE BOOL "" FORCE)
# M8.4 (docs/FFMPEG_MIGRATION_PLAN.md): the game's sound is OpenAL Soft's (the native sound manager, the movies'
# streams); cellAudio's host output (SDL2's audio, WASAPI) and its mixing thread are retired in the native release.
# SDL2 stays for the window and the controllers (and, on Linux, OpenAL Soft's own SDL2 backend).
# DOTHEROES_CELL_AUDIO_OUTPUT=ON brings them back in a development build (the comparison against the lifted path).
option(DOTHEROES_CELL_AUDIO_OUTPUT "Development only: cellAudio's host output and mixing thread" OFF)
if(DOTHEROES_CELL_AUDIO_OUTPUT AND DOTHEROES_NATIVE_RELEASE)
    message(FATAL_ERROR "DOTHEROES_CELL_AUDIO_OUTPUT is for development builds only (M8.4 retired cellAudio)")
endif()
set(PS3RECOMP_CELL_AUDIO_OUTPUT ${DOTHEROES_CELL_AUDIO_OUTPUT} CACHE BOOL "" FORCE)
add_subdirectory("${PS3RECOMP_DIR}" "${CMAKE_BINARY_DIR}/ps3recomp_sdk"
                 EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# The HLE NID table
# ---------------------------------------------------------------------------
# Maps each firmware import NID onto the handler in the runtime that answers
# it. A port may keep a hand-curated one beside its lift; otherwise generate
# the canonical every-module table, which is what the toolkit's own boot
# harness uses.
if(EXISTS "${RECOMP_DIR}/ppu_hle_nids.cpp")
    set(HLE_NID_TABLE "${RECOMP_DIR}/ppu_hle_nids.cpp")
    message(STATUS "HLE NID table:   ${HLE_NID_TABLE}")
else()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(HLE_NID_TABLE "${CMAKE_CURRENT_BINARY_DIR}/ppu_hle_nids.cpp")
    # The generator discovers handlers from library definitions. Rebuild when
    # an existing module changes, and reconfigure when a new module is added.
    file(GLOB_RECURSE HLE_NID_SOURCES CONFIGURE_DEPENDS
        "${PS3RECOMP_DIR}/libs/*.c")
    # A removed input cannot make the output older than a remaining dependency.
    # Track the list itself so deletions also remove stale handler references.
    set(HLE_NID_SOURCE_LIST "${CMAKE_CURRENT_BINARY_DIR}/hle_nid_sources.txt")
    file(GENERATE OUTPUT "${HLE_NID_SOURCE_LIST}" CONTENT "${HLE_NID_SOURCES}\n")
    add_custom_command(
        OUTPUT "${HLE_NID_TABLE}"
        COMMAND "${Python3_EXECUTABLE}"
                "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                --out "${HLE_NID_TABLE}" --all ${HLE_NID_DEFINES}
        DEPENDS "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                "${PS3RECOMP_DIR}/tools/nid_database.py"
                ${HLE_NID_SOURCES} "${HLE_NID_SOURCE_LIST}"
        COMMENT "Generating the HLE NID -> handler table")
    message(STATUS "HLE NID table:   generated (every module)")
endif()

# ---------------------------------------------------------------------------
# The game executable
# ---------------------------------------------------------------------------
# The PPU boot scaffold is compiled here rather than taken from the runtime
# library: every one of these files #includes the lifter's per-game
# ppu_recomp.h, so they can only be built against a particular lift.
set(PS3RECOMP_SCAFFOLD
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp"
)

# ---------------------------------------------------------------------------
# Lifted SPU images (optional)
# ---------------------------------------------------------------------------
# A title that runs SPU work gets its images lifted into <SPU_DIR>/<image>/ by
# tools/spu_lifter.py, with a registration TU generated by
# tools/build_spu_workloads.py. cellSpurs dispatches them by content
# fingerprint, so the generated table is keyed to the images THIS build ships:
# a table generated from another release registers signatures that can never
# match, which is worse than shipping none. Both the table and the job-module
# registry self-register from constructors and are therefore optional -- a tree
# that has not lifted its SPU images yet configures and links without them.
set(SPU_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../build/gen/spu" CACHE PATH
    "Directory holding the lifted SPU images (<SPU_DIR>/<image>/spu_recomp.c)")
option(PORT_ENABLE_SPU "Compile and register the title's lifted SPU images" ON)
# M6 (docs/OPENAL_MIGRATION_PLAN.md section 10.6): the native release links no lifted SPU program. Every SPU workload
# the game reaches is native, and with the stores' reservation test gone (mov-nospu) the lifted fallbacks that take
# reservations could not run anyway. The lift of the SPU images is still made and read -- the water's and the waver's
# native code is generated from it (below) -- but not compiled into the game. The comparison build
# (-DDOTHEROES_PPU_STORES=mov) links them, for the check modes and the switches that run the lifted tasks.
if(DEFINED DOTHEROES_PPU_STORES)
    set(_stores_hint "${DOTHEROES_PPU_STORES}")
elseif(DOTHEROES_NATIVE_RELEASE AND WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64|X86_64|amd64)$")
    set(_stores_hint mov-nospu)
else()
    set(_stores_hint mov)
endif()
if(_stores_hint STREQUAL "mov-nospu")
    set(_lifted_spu_default OFF)
else()
    set(_lifted_spu_default ON)
endif()
option(DOTHEROES_LIFTED_SPU "Link the lifted SPU programs (the comparison build; the release links none)"
       ${_lifted_spu_default})

set(PORT_SPU_INPUTS "" CACHE STRING
    "Input files to scan for all embedded SPU ELF programs (semicolon-separated decrypted ELF/PRX/data paths)")
set(SPU_SOURCES "")
if(PORT_ENABLE_SPU AND PORT_SPU_INPUTS)
    include("${PS3RECOMP_DIR}/cmake/PS3RecompSPU.cmake")
    ps3recomp_generate_spu("${CMAKE_CURRENT_BINARY_DIR}/generated_spu" ${PORT_SPU_INPUTS})
    list(APPEND SPU_SOURCES ${SPU_GENERATED_SOURCES}
         "${PS3RECOMP_DIR}/runtime/spu/spu_dispatch_mt.c")
elseif(PORT_ENABLE_SPU AND NOT DOTHEROES_LIFTED_SPU)
    message(STATUS "Lifted SPU:      not linked (DOTHEROES_LIFTED_SPU=OFF: the release; the native tasks are generated "
                   "from ${SPU_DIR})")
elseif(PORT_ENABLE_SPU)
    file(GLOB SPU_LIFTED CONFIGURE_DEPENDS "${SPU_DIR}/*/spu_recomp.c")
    if(SPU_LIFTED)
        list(LENGTH SPU_LIFTED SPU_IMAGE_COUNT)
        message(STATUS "Lifted SPU:      ${SPU_IMAGE_COUNT} image(s)")
        # The musttail indirect-branch fast path. Excluded from the runtime
        # library on purpose (see the SDK root CMakeLists): it must be built
        # here, by clang, or the tail call degrades to a plain call and a guest
        # loop that branches indirectly walks the thread stack off its guard
        # page a few thousand iterations in.
        list(APPEND SPU_SOURCES ${SPU_LIFTED}
             "${PS3RECOMP_DIR}/runtime/spu/spu_dispatch_mt.c")
        # Generated beside the lift; each is present only once its generator
        # has been run, so each is included only if it is actually here.
        foreach(gen IN ITEMS spu_workloads.c jobmods_register.c taskset_policy_bytes.c)
            if(EXISTS "${SPU_DIR}/${gen}")
                list(APPEND SPU_SOURCES "${SPU_DIR}/${gen}")
            else()
                message(STATUS "  (no ${gen} -- not registered)")
            endif()
        endforeach()
    else()
        message(STATUS "Lifted SPU:      none found in ${SPU_DIR}")
    endif()
endif()

# The water's and the waver's SPU tasks as native code, generated at build time from this tree's lift of the
# player's SPU images (tools/gen_spu_native.py with tools/spu_native_task.cpp.in and tools/spu_native/<task>.py;
# port/native/spu_native.h). Never committed, never in the prebuilt library: they are made from the player's
# own files, like the lift. A tree without those images builds the stub, and the lifted tasks run.
set(SPU_NATIVE_TASKS water:spu_0000_at_00F47380 waver:spu_0001_at_00F48F80)
set(SPU_NATIVE_SOURCES "")
set(_spu_native_tools "${CMAKE_CURRENT_SOURCE_DIR}/../tools")
set(_spu_native_all ON)
foreach(t IN LISTS SPU_NATIVE_TASKS)
    string(REPLACE ":" ";" _p "${t}")
    list(GET _p 1 _img)
    if(NOT PORT_ENABLE_SPU OR NOT EXISTS "${SPU_DIR}/${_img}/spu_recomp.c" OR NOT EXISTS "${SPU_DIR}/images/${_img}.elf")
        set(_spu_native_all OFF)
    endif()
endforeach()
if(_spu_native_all)
    if(NOT Python3_EXECUTABLE)
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
    endif()
    foreach(t IN LISTS SPU_NATIVE_TASKS)
        string(REPLACE ":" ";" _p "${t}")
        list(GET _p 0 _name)
        list(GET _p 1 _img)
        set(_out "${CMAKE_CURRENT_BINARY_DIR}/spu_native/${_name}_gen.cpp")
        add_custom_command(
            OUTPUT "${_out}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/spu_native"
            COMMAND "${Python3_EXECUTABLE}" "${_spu_native_tools}/gen_spu_native.py"
                    "${_spu_native_tools}/spu_native/${_name}.py" "${SPU_DIR}/${_img}/spu_recomp.c"
                    "${SPU_DIR}/images/${_img}.elf" "${_spu_native_tools}/spu_native_task.cpp.in" "${_out}"
            DEPENDS "${_spu_native_tools}/gen_spu_native.py" "${_spu_native_tools}/spu_native/${_name}.py"
                    "${_spu_native_tools}/spu_native_task.cpp.in" "${SPU_DIR}/${_img}/spu_recomp.c"
                    "${SPU_DIR}/images/${_img}.elf"
            COMMENT "Generating the ${_name} task's native code from the lift (${_img})")
        list(APPEND SPU_NATIVE_SOURCES "${_out}")
    endforeach()
    message(STATUS "Native SPU tasks: generated from ${SPU_DIR} (water, waver)")
else()
    set(SPU_NATIVE_SOURCES native/spu_native_absent.cpp)
    message(STATUS "Native SPU tasks: none (no lifted water/waver images in ${SPU_DIR}): the lifted tasks run")
endif()
set_source_files_properties(${SPU_NATIVE_SOURCES} PROPERTIES
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR};${CMAKE_CURRENT_SOURCE_DIR}/native;${PS3RECOMP_DIR}/include;${PS3RECOMP_DIR}/runtime/spu"
    COMPILE_OPTIONS "-O2")

add_executable(${PROJECT_NAME}
    main.cpp
    stubs.cpp
    diag/diag.cpp
    game_patches.cpp
    cpu_place.cpp
    ${PS3RECOMP_SCAFFOLD}
    ${HLE_NID_TABLE}
    ${RECOMP_SOURCES}
    ${SPU_SOURCES}
    ${SPU_NATIVE_SOURCES}
)
# `dotheroes --memtest` (docs/NATIVE_R8.md sections 5.3, 5.4.1): the lifted loads'
# checked and bare forms, compiled from this lift's ppu_recomp.h, tested
# against the guest space.
target_sources(${PROJECT_NAME} PRIVATE native/memtest.cpp native/memtest_checked.cpp native/memtest_bare.cpp
    native/memtest_stores.cpp)
# R8's physics hooks (config/r8_hooks.json): the guest registers in and out of
# native/phys_direct.cpp
target_sources(${PROJECT_NAME} PRIVATE phys_hooks.cpp)
# M8's credits on demand (config/m8_dev_credits.json, DOTHEROES_DEV_CREDITS): defines its hook and guard; a lift
# without the hook references neither
target_sources(${PROJECT_NAME} PRIVATE movie_hooks.cpp)
# The game's audio boundary (config/audio_hooks.json, docs/OPENAL_MIGRATION_PLAN.md): its sound manager's
# methods, traced with DOTHEROES_AUDIO_TRACE (M0)
target_sources(${PROJECT_NAME} PRIVATE audio_hooks.cpp)
# The lifter's --promote-calls check (docs/NATIVE_R8.md section 9.0): its report,
# and with DOTHEROES_PPU_PROMO_CHECK=ON the test after every promoted call
target_sources(${PROJECT_NAME} PRIVATE promo_check.cpp)
# The save-data screen's font and prompt images (assets/ui, docs/SAVE_UI.md), embedded
# in the executable so every copy of it (a release folder, an install) has them.
set(UI_ASSET_FILES DejaVuSans.ttf
    prompt_xbox_confirm.png prompt_xbox_back.png prompt_switch_confirm.png prompt_switch_back.png)
set(_ui_assets_c "${CMAKE_CURRENT_BINARY_DIR}/ui_assets_data.c")
set(_ui_asset_paths "")
foreach(_f IN LISTS UI_ASSET_FILES)
    list(APPEND _ui_asset_paths "${CMAKE_CURRENT_SOURCE_DIR}/assets/ui/${_f}")
endforeach()
string(REPLACE ";" "|" _ui_asset_arg "${UI_ASSET_FILES}")   # a list through -D: '|' between the names
add_custom_command(OUTPUT "${_ui_assets_c}"
    COMMAND ${CMAKE_COMMAND} "-DOUT=${_ui_assets_c}" "-DDIR=${CMAKE_CURRENT_SOURCE_DIR}/assets/ui"
            "-DFILES=${_ui_asset_arg}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_files.cmake"
    DEPENDS ${_ui_asset_paths} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_files.cmake"
    COMMENT "Embedding the UI's font and prompt images" VERBATIM)
# (in dotheroes_native, below: the player's install links that library prebuilt)

# The lifted SPU sources use bare #include "spu_context.h". Scope that include
# directory (and their flags) to those TUs alone: adding it to the whole target
# changes the compile command of every lifted PPU TU, and those are large enough
# that the needless rebuild is measured in tens of minutes.
#   SPU_USE_DISPATCH_MT -- route SPU_IB_DISPATCH through the musttail path.
#   -O2                 -- the target-wide -O1 exists for the huge PPU TUs; the
#                          SPU sources are small and decode-hot, so they get
#                          full optimization. The last -O on the line wins.
if(SPU_SOURCES)
    set_source_files_properties(${SPU_SOURCES} PROPERTIES
        INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/spu"
        COMPILE_DEFINITIONS SPU_USE_DISPATCH_MT)
    if(NOT MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set_source_files_properties(${SPU_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2")
    endif()
endif()

# app.rc carries the icon, the version block and the manifest, and needs a
# resource compiler. Listing it as a source on a host that has none is not a
# link error, it is a "no rule to make" for a file nothing can consume.
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
    target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    "${RECOMP_DIR}"                            # the generated ppu_recomp.h
    "${PS3RECOMP_DIR}/include"
    "${PS3RECOMP_DIR}/runtime/ppu"
    "${PS3RECOMP_DIR}/runtime/spu"
    "${PS3RECOMP_DIR}/runtime/memory"
    "${PS3RECOMP_DIR}/runtime/syscalls"
    "${PS3RECOMP_DIR}/runtime/platform"        # win32_compat.h and friends
    "${PS3RECOMP_DIR}/libs/video"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

# ---------------------------------------------------------------------------
# Native replacements for the game's SPU tasks (port/native/)
# ---------------------------------------------------------------------------
# The Bullet collision task as host code, experimental: the lifted task stays
# the default, DOTHEROES_NATIVE_CD=1 selects this at run time. Its narrowphase
# is Bullet 2.74's own source (zlib licence), fetched and checked by
# tools/build_deps.py; game-independent, so a library of its own with its own
# flags (the executable's would rebuild every lifted file). A tree without
# Bullet builds an empty init and the lifted task runs as before.
set(BULLET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../build/deps/src/bullet-2.74" CACHE PATH
    "Bullet 2.74 source tree (tools/build_deps.py fetches it)")
add_library(dotheroes_native STATIC native/bullet_cd_task.cpp native/bullet_solver_task.cpp native/bullet_solver.cpp
    native/phys_direct.cpp
    native/dev_state.cpp native/inflate.c native/edge_inflate.c native/inflate_direct.cpp native/water_task.cpp native/waver_task.cpp native/waver_game.cpp
    native/spu_native.cpp
    # R1's boundary capture and R3's recorder and target watch: linked only
    # when a development lift's entry observers reference them
    # (tools/r1_dev_lift.sh)
    native/render/r1_observe.cpp native/render/ppfx_recorder.cpp native/render/ppfx_watch.cpp
    native/render/ppfx_render.cpp
    # M8.0's movie trace (DOTHEROES_MOVIE_TRACE; a lift with config/m8_movie_trace.json's observers)
    native/movie/movie_trace.cpp)
# the save-data screen's font and prompt images (above), registered by main.cpp
target_sources(dotheroes_native PRIVATE ui_assets.cpp "${_ui_assets_c}")
if(NOT DOTHEROES_NATIVE_RELEASE)
    # R4's bridge to the FIFO walker and the existing renderer (development)
    target_sources(dotheroes_native PRIVATE native/render/ppfx_bridge.cpp)
endif()
target_include_directories(dotheroes_native PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/native"
    "${PS3RECOMP_DIR}/include"
    "${PS3RECOMP_DIR}/libs/video"
    "${PS3RECOMP_DIR}/runtime/spu"
    "${PS3RECOMP_DIR}/runtime/ppu")
if(PS3RECOMP_VULKAN_INCLUDE_DIR)
    target_include_directories(dotheroes_native PRIVATE "${PS3RECOMP_VULKAN_INCLUDE_DIR}")
endif()
if(EXISTS "${BULLET_DIR}/src/BulletMultiThreaded/SpuNarrowPhaseCollisionTask/SpuGatheringCollisionTask.h")
    message(STATUS "Native Bullet:   ${BULLET_DIR}")
    set(BT "${BULLET_DIR}/src")
    target_sources(dotheroes_native PRIVATE
        native/bullet_cd.cpp
        native/bt_host_alloc.cpp
        "${BT}/BulletMultiThreaded/SpuNarrowPhaseCollisionTask/SpuGjkPairDetector.cpp"
        "${BT}/BulletMultiThreaded/SpuNarrowPhaseCollisionTask/SpuVoronoiSimplexSolver.cpp"
        "${BT}/BulletMultiThreaded/SpuNarrowPhaseCollisionTask/SpuMinkowskiPenetrationDepthSolver.cpp"
        "${BT}/BulletCollision/CollisionDispatch/btBoxBoxDetector.cpp"
        "${BT}/BulletCollision/CollisionShapes/btBoxShape.cpp"
        "${BT}/BulletCollision/CollisionShapes/btPolyhedralConvexShape.cpp"
        "${BT}/BulletCollision/CollisionShapes/btConvexInternalShape.cpp"
        "${BT}/BulletCollision/CollisionShapes/btConvexShape.cpp"
        "${BT}/BulletCollision/CollisionShapes/btCollisionShape.cpp"
        "${BT}/BulletCollision/NarrowPhaseCollision/btPersistentManifold.cpp")
    target_include_directories(dotheroes_native PRIVATE "${BT}" "${BT}/BulletMultiThreaded")
    # The SPU task's build: 64-bit host addresses for its effective-address
    # type, no profiler, and PlatformDefinitions.h's own integer typedefs
    # replaced by <stdint.h>'s.
    target_compile_definitions(dotheroes_native PRIVATE
        DOTHEROES_BULLET_CD USE_ADDR64 BT_NO_PROFILE __PHYSICS_COMMON_H__)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(dotheroes_native PRIVATE -O2 -w -include stdint.h)
    endif()
else()
    message(STATUS "Native Bullet:   none (no ${BULLET_DIR}; tools/build_deps.py fetches it)")
endif()

# The game's audio on OpenAL Soft (docs/OPENAL_MIGRATION_PLAN.md, M2): the device, the sound files, the music
# streamer and the reverb. Independent of the game; only OpenAL Soft's headers are needed here -- the game loads
# the library at run time from its own folder (native/audio/al_api.cpp), nothing links against it.
find_path(OPENAL_INCLUDE_DIR AL/alext.h HINTS "${PS3RECOMP_VULKAN_INCLUDE_DIR}"
          DOC "OpenAL Soft's headers (tools/build_deps.py installs them)")
if(OPENAL_INCLUDE_DIR)
    message(STATUS "OpenAL Soft:     headers in ${OPENAL_INCLUDE_DIR}")
    target_sources(dotheroes_native PRIVATE native/audio/wav_pcm.cpp native/audio/al_api.cpp
        native/audio/al_device.cpp native/audio/al_stream.cpp native/audio/voice_pool.cpp native/audio/sound_list.cpp
        native/audio/sound_manager.cpp)
    target_include_directories(dotheroes_native PRIVATE "${OPENAL_INCLUDE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/native/audio")
else()
    # the game's only audio since M5 (FMOD is not in the lift): a build without it would be a silent game
    message(FATAL_ERROR "OpenAL Soft's headers not found: run tools/build_deps.py (it builds OpenAL Soft and "
                        "installs its headers into build/deps/<platform>/include)")
endif()
# The movies on FFmpeg (docs/FFMPEG_MIGRATION_PLAN.md, M8.2): the native answers to the movie class's calls into
# Bink (config/m8_native_movie.json), the decoder and FFmpeg's loader. Only FFmpeg's headers are needed here -- the
# game loads the libraries at run time from its own folder (native/movie/ff_api.cpp), nothing links against them.
find_path(FFMPEG_INCLUDE_DIR libavformat/avformat.h HINTS "${PS3RECOMP_VULKAN_INCLUDE_DIR}"
          DOC "FFmpeg's headers (tools/build_deps.py installs them)")
if(FFMPEG_INCLUDE_DIR)
    message(STATUS "FFmpeg:          headers in ${FFMPEG_INCLUDE_DIR}")
    target_sources(dotheroes_native PRIVATE native/movie/ff_api.cpp native/movie/movie_player.cpp
        native/movie/bink_native.cpp)
    target_include_directories(dotheroes_native PRIVATE "${FFMPEG_INCLUDE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/native/movie")
else()
    message(FATAL_ERROR "FFmpeg's headers not found: run tools/build_deps.py (it builds FFmpeg and installs its "
                        "headers into build/deps/<platform>/include)")
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE
    dotheroes_native
    ps3recomp_runtime
)
if(DOTHEROES_NATIVE_RELEASE)
    target_compile_definitions(dotheroes_native PRIVATE DOTHEROES_NATIVE_RELEASE=1)
    target_compile_definitions(${PROJECT_NAME} PRIVATE DOTHEROES_NATIVE_RELEASE=1)
    # R8 (docs/NATIVE_R8.md section 5): the lifted code's guest loads without the
    # poll-word, range and alignment tests (the generated ppu_recomp.h's
    # PPU_BARE_LOADS; a lift from before R8 ignores it). DOTHEROES_PPU_CHECKED_LOADS=ON
    # keeps the tests in a release build.
    option(DOTHEROES_PPU_CHECKED_LOADS "Keep the lifted loads' inline tests in a native release build" OFF)
    # A lift with --promote-calls (section 9.0): DOTHEROES_PPU_PROMO_CHECK=ON tests,
    # after every call in a promoted function, that the callee kept r14-r31 and
    # f14-f31 (port/promo_check.cpp). A test build, not a player's.
    option(DOTHEROES_PPU_PROMO_CHECK "Test the promoted functions' callees keep r14-r31/f14-f31" OFF)
    if(DOTHEROES_PPU_PROMO_CHECK)
        target_compile_definitions(${PROJECT_NAME} PRIVATE PPU_PROMO_CHECK=1)
        message(STATUS "Promoted calls:  checked (DOTHEROES_PPU_PROMO_CHECK)")
    endif()
    # Bare only where their assumptions are established (section 5.3): the
    # whole guest space and a guard reserved, pages committed zero-filled on
    # first touch (port/main.cpp's commit handler), x86-64's scalar loads.
    # Elsewhere -- Linux's mapping is another mechanism, not yet tested --
    # the loads stay checked.
    if(DOTHEROES_PPU_CHECKED_LOADS)
        message(STATUS "Lifted loads:    checked (DOTHEROES_PPU_CHECKED_LOADS)")
    elseif(WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64|X86_64|amd64)$")
        target_compile_definitions(${PROJECT_NAME} PRIVATE PPU_BARE_LOADS=1)
        message(STATUS "Lifted loads:    bare (PPU_BARE_LOADS; dotheroes --memtest tests what they rely on)")
    else()
        message(STATUS "Lifted loads:    checked (the bare loads are established on Windows x86-64 only)")
    endif()
    # The guest stores (docs/NATIVE_R8.md section 5.4). checked: as R7 (the
    # range and SPU reservation tests, then an aligned atomic store or a
    # memcpy). mov: the same tests, then one store -- measured, the default on
    # Windows x86-64. nocoh-measure and bare-measure leave out the reservation
    # test, or every test: builds for measurement only, which refuse to start
    # without DOTHEROES_MEASURE_ONLY=1. mov-nospu (M6, the native release's
    # default): mov without the SPU reservation test -- no lifted SPU code takes
    # a reservation in the game (docs/OPENAL_MIGRATION_PLAN.md section 10), and
    # the runtime makes one fatal (spu_coh_forbid at start). The check modes
    # that run lifted SPU code beside the native (DOTHEROES_NATIVE_WATER=check
    # ...) need a comparison build: -DDOTHEROES_PPU_STORES=mov.
    if(WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64|X86_64|amd64)$")
        if(DOTHEROES_NATIVE_RELEASE)
            set(_stores_default mov-nospu)
        else()
            set(_stores_default mov)
        endif()
    else()
        set(_stores_default checked)
    endif()
    set(DOTHEROES_PPU_STORES "${_stores_default}" CACHE STRING
        "The lifted code's guest stores: checked, mov, mov-nospu, nocoh-measure, bare-measure")
    set_property(CACHE DOTHEROES_PPU_STORES PROPERTY STRINGS checked mov mov-nospu nocoh-measure bare-measure)
    if(NOT DOTHEROES_PPU_STORES STREQUAL "checked" AND
       NOT (WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64|X86_64|amd64)$"))
        message(FATAL_ERROR "DOTHEROES_PPU_STORES=${DOTHEROES_PPU_STORES} is established on Windows x86-64 only")
    endif()
    if(DOTHEROES_PPU_STORES STREQUAL "mov")
        target_compile_definitions(${PROJECT_NAME} PRIVATE PPU_STORE_MOV=1)
    elseif(DOTHEROES_PPU_STORES STREQUAL "mov-nospu")
        target_compile_definitions(${PROJECT_NAME} PRIVATE PPU_STORE_MOV=1 PPU_STORES_NO_SPU_COH=1
                                   DOTHEROES_NO_SPU_COH=1)
        target_compile_definitions(dotheroes_native PRIVATE DOTHEROES_NO_SPU_COH=1)
    elseif(DOTHEROES_PPU_STORES STREQUAL "nocoh-measure")
        target_compile_definitions(${PROJECT_NAME} PRIVATE PPU_STORES_NO_COH_MEASURE=1)
    elseif(DOTHEROES_PPU_STORES STREQUAL "bare-measure")
        target_compile_definitions(${PROJECT_NAME} PRIVATE PPU_STORES_BARE_MEASURE=1)
    elseif(NOT DOTHEROES_PPU_STORES STREQUAL "checked")
        message(FATAL_ERROR "DOTHEROES_PPU_STORES: checked, mov, mov-nospu, nocoh-measure or bare-measure")
    endif()
    message(STATUS "Lifted stores:   ${DOTHEROES_PPU_STORES}")
endif()
if(PORT_ENABLE_SPU AND NOT DOTHEROES_LIFTED_SPU)
    target_compile_definitions(${PROJECT_NAME} PRIVATE DOTHEROES_NO_LIFTED_SPU=1)
    target_compile_definitions(dotheroes_native PRIVATE DOTHEROES_NO_LIFTED_SPU=1)
endif()

# The runtime is C++, and on Apple also Objective-C. Letting CMake infer the
# link language from the generated C would leave the C++ standard library out.
set_target_properties(${PROJECT_NAME} PROPERTIES LINKER_LANGUAGE CXX)

# Windows system import libraries. dbghelp symbolizes crash-handler addresses
# against the PDB, user32 and gdi32 are what the backend's window and its GDI
# present are made of, and winmm is timeBeginPeriod. None of the four names
# anything a POSIX linker can find; the D3D12 backend's own imports come in
# through the #pragma comment(lib) in libs/video/rsx_d3d12_backend.c.
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        dbghelp
        user32
        gdi32
        winmm
    )
endif()

# ---------------------------------------------------------------------------
# Compiler-specific flags
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -fno-strict-aliasing
        # The lift spells PPC arithmetic as signed C arithmetic, which must wrap;
        # without this, clang exploits overflow UB (e.g. folds "x*x < 0" to false).
        -fwrapv
        # The target-wide -O1 the SPU note above relies on, for a build whose
        # configuration sets no optimisation level (no CMAKE_BUILD_TYPE, or one
        # CMake has no defaults for). Without it every lifted PPU TU compiled at
        # -O0: each static inline helper (ppc_rlw64, vmx_w, vm_read32_inl) is a
        # real call that spills to the stack, and those calls were most of the
        # PPU thread's profile. -O1 keeps the huge TUs' build time sane. Debug
        # and the optimised configurations keep their own levels.
        $<$<NOT:$<CONFIG:Debug,Release,RelWithDebInfo,MinSizeRel>>:-O1>
    )
elseif(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W3 /wd4100 /wd4244 /wd4267
        /Zc:__cplusplus         # report correct __cplusplus value
        # ppu_recomp.h defines PPU_FENCE, the lowering of the guest's
        # sync/lwsync/eieio/isync, and in C that is <stdatomic.h>. The lifter
        # writes C++ by default, but --single-file writes ppu_recomp.c, and a
        # port that took it would not compile without this. The runtime's own
        # build already carries the same flag for the same reason.
        /experimental:c11atomics
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
endif()

# The lifted translation units are machine-generated and can run to hundreds of
# thousands of lines each, so they get their own flags:
#
#   - warnings off. They are about generated code and nobody will act on them,
#     and at this volume they bury the diagnostics that matter.
#   - no floating-point contraction. arm64 fuses a multiply and an add by
#     default; a PPC fmadd is spelt as one where the lifter means one.
#   - /bigobj on MSVC. A single lifted file routinely passes the 65,536-section
#     limit of the default object format.
if(MSVC)
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES
        COMPILE_OPTIONS "/w;/bigobj")
else()
    # DOTHEROES_PPU_OPT (R8, docs/NATIVE_R8.md): the optimisation level of the
    # lifted PPU sources and the PPU boot scaffold (ppu_loader.cpp, with
    # ps3_indirect_call) -- empty keeps the target-wide -O1 above, e.g. -O2.
    # The last -O on the line wins. The floating-point options are unchanged:
    # no contraction, no fast-math at any level.
    set(DOTHEROES_PPU_OPT "" CACHE STRING "Optimisation level for the lifted PPU code and its scaffold (empty: -O1)")
    set(_ppu_opt "-w;-ffp-contract=off")
    if(DOTHEROES_PPU_OPT)
        list(APPEND _ppu_opt ${DOTHEROES_PPU_OPT})
        set_source_files_properties(${PS3RECOMP_SCAFFOLD} PROPERTIES COMPILE_OPTIONS "${DOTHEROES_PPU_OPT}")
        message(STATUS "Lifted PPU code: ${DOTHEROES_PPU_OPT}")
    endif()
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES
        COMPILE_OPTIONS "${_ppu_opt}")
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# Development diagnostics and strict mode
# ---------------------------------------------------------------------------
# DEV builds carry the crash handler, hang watchdog and strict mode (every
# silent runtime fallback is fatal; PS3_STRICT=0 overrides at run time). A
# release package is configured with -DDOTHEROES_DEV_DIAGNOSTICS=OFF.
option(DOTHEROES_DEV_DIAGNOSTICS "Crash reports, hang watchdog, strict mode by default" ON)
if(DOTHEROES_DEV_DIAGNOSTICS)
    # Scoped to the files that read it: a target-wide definition would change
    # the flags of every lifted chunk and force a full ~35 minute rebuild.
    set_source_files_properties(main.cpp diag/diag.cpp PROPERTIES
        COMPILE_DEFINITIONS DOTHEROES_DEV_DIAGNOSTICS=1)
    target_compile_definitions(ps3recomp_runtime PRIVATE PS3RECOMP_STRICT_DEFAULT=1)
    if(NOT WIN32)
        # dladdr() names host frames in crash reports only for exported symbols.
        target_link_options(${PROJECT_NAME} PRIVATE -rdynamic)
    endif()
endif()

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)

# ---------------------------------------------------------------------------
# The window launcher (macOS)
# ---------------------------------------------------------------------------
# A title that replaces its executable (sys_game_process_exitspawn, done by a
# port as an exec) loses its window with the old process image. The launcher
# holds the window for the whole session instead: it runs the port with
# PS3RECOMP_HOST_SURFACES set, the Metal backend renders into IOSurfaces it
# shares (libs/video/rsx_host_surface.h), and the image an exec brings up
# attaches to the same window again. One Objective-C file and no runtime
# library, so it builds in seconds:
#
#   ./MyGameRecomp_window [--title T] [--size WxH] -- ./MyGameRecomp <args>
if(APPLE)
    enable_language(OBJC)
    add_executable(${PROJECT_NAME}_window "${PS3RECOMP_DIR}/tools/host_window/host_window.m")
    target_include_directories(${PROJECT_NAME}_window PRIVATE "${PS3RECOMP_DIR}/libs/video")
    target_compile_definitions(${PROJECT_NAME}_window PRIVATE HOST_WINDOW_TITLE="${PROJECT_NAME}")
    target_compile_options(${PROJECT_NAME}_window PRIVATE -fobjc-arc -Wall -Wextra -Wno-unused-parameter)
    target_link_libraries(${PROJECT_NAME}_window PRIVATE
        "-framework AppKit" "-framework Metal" "-framework QuartzCore" "-framework IOSurface")
    install(TARGETS ${PROJECT_NAME}_window RUNTIME DESTINATION bin)
endif()

# ---------------------------------------------------------------------------
# R1's executor (docs/NATIVE_PORT.md section 5)
# ---------------------------------------------------------------------------
# Renders a captured pass description with its own Vulkan device
# (native/render/r1_replay.c). It links the program translators -- the
# decompilers and the HLSL -> SPIR-V step, asset conversion -- and glslang,
# and nothing of the runtime: no FIFO walker, RSX state or GCM HLE
# (tools/r1_check_deps.py checks the executable). Development only; not
# installed.
find_package(glslang CONFIG QUIET)
if(glslang_FOUND AND TARGET glslang::glslang AND TARGET glslang::SPIRV AND PS3RECOMP_VULKAN_INCLUDE_DIR)
    add_executable(r1_replay native/render/r1_replay.c native/render/r1_json.c
        "${PS3RECOMP_DIR}/libs/video/rsx_fp_decompiler.c"
        "${PS3RECOMP_DIR}/libs/video/rsx_vp_decompiler.c"
        "${PS3RECOMP_DIR}/libs/video/rsx_shader_spirv.cpp")
    target_include_directories(r1_replay PRIVATE "${PS3RECOMP_DIR}/include" "${PS3RECOMP_DIR}/libs/video"
        "${PS3RECOMP_VULKAN_INCLUDE_DIR}")
    target_compile_definitions(r1_replay PRIVATE PS3RECOMP_HAVE_SPIRV_TRANSLATION=1)
    target_link_libraries(r1_replay PRIVATE glslang::glslang glslang::SPIRV)
    if(TARGET glslang::glslang-default-resource-limits)
        target_link_libraries(r1_replay PRIVATE glslang::glslang-default-resource-limits)
    endif()
endif()

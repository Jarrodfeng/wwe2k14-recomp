# wwe2k14 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(wwe2k14 LANGUAGES CXX)
if(WIN32)
    enable_language(C)  # lane 283: libopus (third_party/audio/opus) is C
    include(third_party/audio/opus/opus_sources.cmake)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Session 25: one version string (VERSION, e.g. v0.1.0-playtest) for the game and the launcher; it is in every game
# log, perf report and problem report. Builds made by tools/release/make_release.ps1 pass -DWWE2K14_RELEASE=ON; every
# other build is marked "+dev".
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" WWE2K14_VERSION LIMIT_COUNT 1)
option(WWE2K14_RELEASE "A release build (tools/release/make_release.ps1)" OFF)
# Session 39: make_release.ps1 -LocalTest NAME (a local test build, never published) appends "-NAME-playtest".
set(WWE2K14_VERSION_SUFFIX "" CACHE STRING "Appended to the version (make_release.ps1 -LocalTest)")
string(APPEND WWE2K14_VERSION "${WWE2K14_VERSION_SUFFIX}")
if(NOT WWE2K14_RELEASE)
    string(APPEND WWE2K14_VERSION "+dev")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
message(STATUS "wwe2k14 version ${WWE2K14_VERSION}")

# Sources
set(WWE2K14_SOURCES
    src/main.cpp
    src/build_stamp.cpp  # S101 lane 19 review: the only file that includes the generated stamp header (a new commit recompiles just this one)
    src/dev_build.cpp  # S103 L33F: IsDevBuild() / VersionString() for every TU (it gets the WWE2K14_VERSION define below; no other TU does)
    src/start_settings.cpp  # S103 SPB: ApplyStartSettings, the settings both start paths apply (KEPT with and without the stream; tools/scene/start_parity_lint.py)
    # S101 lane 292 (the music picker's VIEW, docs/systems/music.md): the ImGui dialog, the owner-mode texture uploader and the Android touch hook; the cover + menu-font decoders (own STATIC stb_image like crowd_body.cpp). Same list for the phone.
    src/music_picker_ui.cpp
    src/music_picker_assets.cpp
    src/renderer_adapter/capture.cpp
    # Session 100: the new renderer and the live adapter's sink (docs/renderer/LIVE_ADAPTER.md); off unless --wwe2k14_rdr_live.
    src/renderer_adapter/live.cpp
    # S101 lane28: the arena as a live scene contributor (docs/systems/arena_look.md "What is live"). Needs lane 26's live writer (live_scene.cpp) at link.
    src/scene_live/arena_contrib.cpp
    # Session 101 (lane 5/11): the bug button's case zip (docs/systems/case_zip.md). No case_selftest.cpp: that is its own exe (tools/case/build.ps1).
    src/case/zip_writer.cpp
    src/case/case_ring.cpp
    src/case/case_writer.cpp
    # Session 101 (lane 29): the wrestlers as a live scene contributor (docs/systems/wrestler_pose.md). Needs record_io.cpp (lane 26's writer adds it).
    src/scene_live/wrestler_contrib.cpp
    # Session 101 (lane 26): the live scene writer (docs/renderer/SCENE_RECORD.md "The live writer"), its capture-folder sink, record files, and the case ring's scene part.
    src/case/case_scene.cpp
    src/renderer_scene/live_scene.cpp
    src/renderer_scene/live_sinks.cpp
    src/renderer_scene/record_io.cpp
    # S103 lane S (docs/renderer/FRAME_CONTRACT.md): the scene front-end, the record -> frame path, its classes and the live drawer (mirror / ours,
    # --wwe2k14_rdr_scene). live_assets.cpp holds its own STATIC stb_image like crowd_body.cpp. Same list for the phone.
    src/renderer_scene/front_end.cpp
    src/renderer_scene/char_effects.cpp
    src/renderer_scene/shader_reload.cpp
    src/renderer_scene/live_assets.cpp
    src/renderer_scene/live_scene_draw.cpp
    src/renderer_scene/post_from_record.cpp   # S103 P: our post from the record's post row (LiveSceneDraw::BuildPost)
    src/renderer_scene/quads2d_draw.cpp       # S103 L2a: the 2D rows drawn over our frame on an OURS frame (live.cpp, --wwe2k14_rdr_2d_ours; dev only)
    src/renderer_scene/quads2d_register.cpp   # S103 L2a: its programs (registered only with the switch on)
    src/renderer_scene/record_frame.cpp
    src/renderer_scene/record_frame_classes.cpp
    # S103 (integration): the class appliers (R: arena / crowd / models; B: wrestlers; C: stage), lane G's kept draw lists, lane E's maps stage
    src/renderer_scene/rf_arena.cpp
    src/renderer_scene/rf_crowd.cpp
    src/renderer_scene/live_crowd.cpp   # S103 D2
    src/renderer_scene/rf_models.cpp
    src/renderer_scene/rf_wrestlers.cpp
    src/renderer_scene/rf_c_core.cpp
    src/renderer_scene/rf_arena_live.cpp
    src/renderer_scene/rf_ring.cpp
    src/renderer_scene/rf_props.cpp
    src/renderer_scene/rf_screens.cpp
    src/renderer_scene/live_flashes.cpp
    src/renderer_scene/draw_lists.cpp
    src/renderer_scene/live_maps.cpp
    src/renderer_scene/live_map_shadow.cpp
    src/renderer_scene/live_map_circles.cpp
    src/renderer_scene/live_map_props.cpp
    src/renderer_scene/live_map_e2_input.cpp
    src/renderer_scene/live_map_rim.cpp
    src/renderer_scene/live_map_chain.cpp   # S103 L7
    src/renderer_scene/live_map_plane.cpp
    src/renderer_scene/live_map_rope.cpp
    src/renderer_scene/live_map_rope_input.cpp
    src/renderer_scene/live_map_prelight.cpp
    src/renderer_scene/live_map_sweat.cpp
    # S103 R2: the stage hooks + see order of a frame, attached the same way by the live drawer and the rig
    src/renderer_scene/frame_hooks.cpp
    # S101 lane 27 (class C2): the crowd contributor to the scene record (docs/systems/crowd.md). Registered by RegisterCrowd() (crowd_hooks.h).
    src/scene_live/crowd_contrib.cpp
    # S103 F: the particles from the scene record (docs/renderer/SEE_THROUGH_ORDER.md): the record writer (not registered until a game-side seam publishes snapshots) and the StageHook that draws them.
    src/scene_live/particle_contrib.cpp
    src/scene_live/particle_reader.cpp
    src/renderer_scene/live_particles.cpp
    # S101 lane 35 (the crowd's first promotion, docs/renderer/CROWD_PROMOTION.md): our crowd drawn inside the game's frame from the live scene record, on --wwe2k14_rdr_crowd_ours.
    # crowd_body.cpp holds its own STATIC stb_image (no stbi_* symbol is exported: the SDK's copies must not be rebound on Android). Same list for the phone (shared library).
    src/renderer_scene/crowd_ours.cpp
    src/renderer_scene/crowd_cards_core.cpp
    src/renderer_scene/crowd_cards.cpp
    src/assets/crowd_body.cpp
    # S101 lane 45 (the wrestlers drawn in place, docs/renderer/WRESTLER_PROMOTION_MAP.md section 11): the DrawReplaceHook and the roster of the player's character models
    # (wrestler_roster.cpp holds its own STATIC stb_image like crowd_body.cpp). Registered by InstallReplaceFactories() (replace_factories.h). Same list for the phone.
    src/renderer_scene/wrestler_ours.cpp
    src/renderer_scene/caw_ours.cpp
    src/assets/wrestler_roster.cpp
    # S101 lane 40 (the arena's first promotion, docs/renderer/ARENA_OURS.md): our arena drawn in place, on --wwe2k14_rdr_arena_ours. arena_game_space.cpp holds its own STATIC stb_image like crowd_body.cpp. Same list for the phone.
    src/assets/arena_game_space.cpp
    src/renderer_scene/arena_ours_core.cpp
    src/renderer_scene/arena_ours.cpp
    # S101 lane 66 (our UI textures, docs/renderer/UI_OURS.md): the 2D draws' slot 0 from the player's menu pacs, on --wwe2k14_rdr_ui_ours. ui_file_index.cpp (KEPT in the OFF build, S103 PN1) holds its own STATIC stb_image like crowd_body.cpp; ui_ours_core.cpp is the stream-only half. Same list for the phone.
    src/renderer_scene/ui_file_index.cpp
    src/renderer_scene/ui_ours_core.cpp
    src/renderer_scene/ui_ours.cpp
    # S101 lane 148 (the UI's sprite / rectangle quads drawn with our own quad and program, docs/renderer/UI_QUADS_OURS.md): reads no game file. Same list for the phone.
    src/renderer_scene/ui_quads.cpp
    # S101 lane 89 (UT, the game's text laid out by us, docs/renderer/UI_TEXT_OURS.md): on --wwe2k14_rdr_ui_text_ours. ui_text_fonts.cpp holds its own STATIC stb_image like ui_file_index.cpp. Same list for the phone.
    src/renderer_scene/ui_text_fonts.cpp
    src/renderer_scene/ui_text_ours.cpp
    # S101 lane 68 (the crowd flashes drawn in place, docs/renderer/PARTICLES_PROMOTION_MAP.md lane F): flash_game_space.cpp holds its own STATIC stb_image like arena_game_space.cpp. Same list for the phone.
    src/assets/flash_game_space.cpp
    src/renderer_scene/flash_ours.cpp
    # S101 lane 70 (our ring drawn in place, docs/renderer/RING_OURS.md): ring_game_space.cpp holds its own STATIC stb_image like arena_game_space.cpp. Same list for the phone.
    src/assets/ring_game_space.cpp
    src/renderer_scene/ring_ours_core.cpp
    src/renderer_scene/ring_ours.cpp
    # S101 lane 84 (props of other pacs, docs/renderer/PROPS_OURS.md): prop_game_space.cpp holds its own STATIC stb_image like ring_game_space.cpp.
    src/assets/prop_game_space.cpp
    src/renderer_scene/props_ours_core.cpp
    src/renderer_scene/props_ours.cpp
    # S101 lane PO (the post chain's promotion, docs/renderer/POST_OURS.md): our own pass (post_pass.cpp) recorded from the stream's chain (post_plan.cpp, post_ours_core.cpp, post_ours.cpp), on --wwe2k14_rdr_post_ours. Same list for the phone.
    src/renderer_scene/post_pass.cpp
    src/renderer_scene/post_plan.cpp
    src/renderer_scene/post_ours_core.cpp
    src/renderer_scene/post_ours.cpp
    # S101 lane 115 (the stage screens, docs/renderer/SCREENS_OURS.md): screen_game_space.cpp holds its own STATIC stb_image like ring_game_space.cpp.
    src/assets/screen_game_space.cpp
    src/renderer_scene/screens_ours_core.cpp
    src/renderer_scene/screens_ours.cpp
    # S101 lane 131 (the movie planes, docs/renderer/MOVIE_OURS.md): reads no game file.
    src/renderer_scene/movie_ours.cpp
    # S101 lane 99 (the shadow passes' casters, docs/renderer/SHADOW_CASTERS_OURS.md): chcommon_proxy.cpp holds its own STATIC stb_image like prop_game_space.cpp.
    src/assets/chcommon_proxy.cpp
    src/renderer_scene/shadow_casters_core.cpp
    src/renderer_scene/shadow_casters_ours.cpp
    # S101 lane 133 (the crowd's sign / foam-hand draws, docs/renderer/CROWD_PROMOTION.md N4): crowd_prop_game_space.cpp holds its own STATIC stb_image like flash_game_space.cpp. Same list for the phone.
    src/assets/crowd_prop_game_space.cpp
    src/renderer_scene/crowd_props_ours.cpp
    src/renderer_scene/rim_map_ours.cpp  # lane 124
    src/renderer_scene/circle_shadows_ours.cpp  # lane 136
    src/renderer_scene/plane_shadow_ours.cpp  # lane 139
    src/renderer_scene/small_passes_ours.cpp  # lane 135
    src/renderer_scene/rim_chain_ours.cpp  # lane 151
    src/renderer/device.cpp
    src/renderer/frame_pump.cpp
    src/renderer/memory.cpp
    src/renderer/problem.cpp
    src/renderer/renderer.cpp
    src/renderer/swapchain.cpp
    src/renderer/vk_api.cpp
    src/renderer/streamline.cpp
)

# S103 P9 (docs/renderer/LANDING2_PLAN.md section 5, docs/renderer/COMPILE_OUT_LIST.md): WWE2K14_KEEP_STREAM keeps the command stream's reader / binder / replayer in the exe.
# 2026-11-18 (Jarrod): the PLAYER build is stream-free, set by the release scripts (tools/release/release_configure_args.ps1 passes OFF + RUN explicitly on every configure; tools/android/build_all.ps1 -Release passes -StreamFree), NOT by this file: the CMake default stays ON so a dev build / the tools that need the stream keep working, and the scripts are the one place that makes a player build. The line below stays a comment on purpose.
# EARLIER (S103 P9): ON in every configuration. The plan's eventual rule is the commented line below (ON unless WWE2K14_RELEASE); it is NOT
# active: a release that cannot draw a frame without the stream must not be built until the landing-2 sweep says B = 0 (docs/renderer/LANDING2_PLAN.md section 3).
#   if(WWE2K14_RELEASE) set(WWE2K14_KEEP_STREAM OFF CACHE BOOL "" FORCE) endif()
# OFF is built and linked offline (tools/scene/keep_stream_off_link.py) and is never run: src/keep_stream.h is what every `#if WWE2K14_KEEP_STREAM` includes, the OFF source
# list is tools/scene/keep_stream_sources.txt (the same file the lint and the off-link script read).
option(WWE2K14_KEEP_STREAM "Keep the command stream (reader / binder / replayer / in-place replacers) in the game exe" ON)
option(WWE2K14_KEEP_STREAM_LINK_TEST "Allow WWE2K14_KEEP_STREAM=OFF for an offline link test (tools/scene/keep_stream_off_link.py)" OFF)
option(WWE2K14_KEEP_STREAM_RUN "Allow WWE2K14_KEEP_STREAM=OFF for a runnable stream-free exe (landing 2 P7 SceneHost present host landed; preset win-amd64-release-off)" OFF)
if(NOT WWE2K14_KEEP_STREAM AND NOT WWE2K14_KEEP_STREAM_LINK_TEST AND NOT WWE2K14_KEEP_STREAM_RUN)
    message(FATAL_ERROR "WWE2K14_KEEP_STREAM=OFF needs -DWWE2K14_KEEP_STREAM_RUN=ON (a runnable stream-free exe, SceneHost presents; preset win-amd64-release-off) or -DWWE2K14_KEEP_STREAM_LINK_TEST=ON (offline link test, never run).")
endif()
if(NOT WWE2K14_KEEP_STREAM)
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/tools/scene/keep_stream_sources.txt" WWE2K14_STREAM_ONLY_SOURCES REGEX "^src/")
    list(REMOVE_ITEM WWE2K14_SOURCES ${WWE2K14_STREAM_ONLY_SOURCES})
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/tools/scene/keep_stream_off_sources.txt" WWE2K14_OFF_ONLY_SOURCES REGEX "^src/")
    list(APPEND WWE2K14_SOURCES ${WWE2K14_OFF_ONLY_SOURCES})
    message(STATUS "wwe2k14: WWE2K14_KEEP_STREAM=OFF (the stream's reader / binder / replayer are left out; SceneHost presents)")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/scene/keep_stream_sources.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/scene/keep_stream_off_sources.txt")

if(ANDROID)
    # Session 63 (Android port, ANDROID_PLAN.md 3.1): a shared library; SDL's Java activity loads libmain.so and calls
    # SDL_main (the SDK's windowed_app_main_sdl.cpp). Built by tools/android/build_game_android.ps1.
    add_library(wwe2k14 SHARED ${WWE2K14_SOURCES})
    set_target_properties(wwe2k14 PROPERTIES OUTPUT_NAME main)
    # Lane 297 / 283: the game on the phone only looks songs up in the cache the PHONE LAUNCHER wrote (lookup.cpp: stat + small files); it never converts.
    target_sources(wwe2k14 PRIVATE src/audio_import/lookup.cpp)
    # Session 95: the portable native functions (src/nf/all.h), for TRUE NATIVE's wwe2k14_nf_lean / wwe2k14_nf_reg_save on phones.
    target_compile_definitions(wwe2k14 PRIVATE WWE2K14_NF_ANDROID)
    # Codegen stays on the PC: the PC SDK's rexglue.exe (the script passes it), never the Android build of the tool.
    if(TARGET rex::rexglue AND WWE2K14_PC_REXGLUE)
        set_target_properties(rex::rexglue PROPERTIES IMPORTED_LOCATION "${WWE2K14_PC_REXGLUE}"
                                                      IMPORTED_LOCATION_RELEASE "${WWE2K14_PC_REXGLUE}")
    endif()
elseif(WIN32)
    add_executable(wwe2k14 WIN32 ${WWE2K14_SOURCES})
    # Lane 283: the song importer (decoders, loudness normaliser, cover art; the player only plays its cache). Windows has the player.
    target_sources(wwe2k14 PRIVATE src/audio_import/lookup.cpp)  # lane 283: cache lookups ONLY (stat + small files); the converter is in the launcher
else()
    add_executable(wwe2k14 ${WWE2K14_SOURCES})
endif()

rexglue_setup_target(wwe2k14 GPU_PLUGINS xenos)
# S103 P9: an explicit 0 / 1 for every unit of the exe (never undefined); the generated guest code (wwe2k14_recomp) does not see it.
target_compile_definitions(wwe2k14 PRIVATE WWE2K14_KEEP_STREAM=$<BOOL:${WWE2K14_KEEP_STREAM}>)
if(WWE2K14_KEEP_STREAM_LINK_TEST)
    target_compile_definitions(wwe2k14 PRIVATE WWE2K14_KEEP_STREAM_LINK_TEST=1)
endif()

# Session 95 (guest CPU power, out/s95/gcpu): compile switches for the recompiled guest code only (wwe2k14_recomp), all OFF by default.
#   -DWWE2K14_GUEST_ISA=x86-64-v3   AVX2 + BMI/BMI2 + FMA + MOVBE + LZCNT + POPCNT (Haswell/Skylake and newer; NOTHING checks the CPU
#                                   at start: on a CPU without AVX2 the exe dies on its first VEX instruction with no message. Before shipping,
#                                   ship it beside the SSE4.1 exe and let the launcher pick by CPUID incl. AVX2+BMI2 and OS AVX support (XGETBV);
#                                   some Pentium/Celeron parts of those generations lack AVX2).
#                                   -ffp-contract=off keeps a*b+c unfused (only the guest's explicit std::fma fuses, same result
#                                   as the ucrt fma it replaces), so the game's results should not change. Unconfirmed exception: when two
#                                   different NaNs meet, VEX operand order may change which payload survives. Replay hash check still open).
#   -DWWE2K14_GUEST_PGO_GEN=ON      clang front-end instrumentation of the guest code (training build; needs the profile runtime at link).
#   -DWWE2K14_GUEST_PGO_USE=<file>  profile-guided build of the guest code from a merged .profdata (tools/perf/guest_variant_exe.py measured it).
# Android builds already use -march=armv8.2-a (tools/android/build_game_android.ps1) and have no physical-address-offset (REX_PHYS_HOST_OFFSET) check; nothing to add there.
set(WWE2K14_GUEST_ISA "" CACHE STRING "Instruction set of the recompiled guest code ('' = SSE4.1 baseline, 'x86-64-v3')")
# Session 102 W1 ("widened"): the same level for OUR sources linked into the exe (src/*, the renderer, the scene code), not only the generated guest
# code. Same -ffp-contract=off reason: our float code was never fused on the PC (no FMA); fusing would change bits. The SDK DLLs stay SSE4.1 (pinned;
# see docs/research/part2/W1_W2_build.md).
set(WWE2K14_OURS_ISA "" CACHE STRING "Instruction set of our own sources in the exe ('' = SSE4.1 baseline, 'x86-64-v3')")
option(WWE2K14_GUEST_PGO_GEN "Instrument the recompiled guest code for PGO (training build)" OFF)
# Session 102 W2: ON = the same PGO switch (gen / use) also applies to OUR sources in the exe (src/*, the renderer and scene code), not only to
# the generated guest code. One merged profdata serves both (a profile is keyed by function name + structure hash; unmatched functions are ignored).
option(WWE2K14_PGO_OURS "PGO gen/use also for our own sources in the exe" OFF)
set(WWE2K14_GUEST_PGO_USE "" CACHE FILEPATH "Merged .profdata for a profile-guided build of the recompiled guest code")
if(TARGET wwe2k14_recomp AND NOT ANDROID)
    if(WWE2K14_GUEST_ISA STREQUAL "x86-64-v3")
        target_compile_options(wwe2k14_recomp PRIVATE -mavx2 -mfma -mbmi -mbmi2 -mmovbe -mlzcnt -mpopcnt -ffp-contract=off)
    elseif(NOT WWE2K14_GUEST_ISA STREQUAL "")
        message(FATAL_ERROR "WWE2K14_GUEST_ISA must be empty or x86-64-v3")
    endif()
    if(WWE2K14_OURS_ISA STREQUAL "x86-64-v3")
        target_compile_options(wwe2k14 PRIVATE -mavx2 -mfma -mbmi -mbmi2 -mmovbe -mlzcnt -mpopcnt -ffp-contract=off)
    elseif(NOT WWE2K14_OURS_ISA STREQUAL "")
        message(FATAL_ERROR "WWE2K14_OURS_ISA must be empty or x86-64-v3")
    endif()
    if(WWE2K14_GUEST_PGO_GEN)
        target_compile_options(wwe2k14_recomp PRIVATE -fprofile-instr-generate)
        if(WWE2K14_PGO_OURS)
            target_compile_options(wwe2k14 PRIVATE -fprofile-instr-generate)
        endif()
        target_link_options(wwe2k14 PRIVATE -fprofile-instr-generate)
        # the game ends with std::_Exit, so the profile runtime's atexit writer never runs: this hook writes the profile every 10 s
        target_sources(wwe2k14 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tools/perf/pgo_exit_hook.cpp)
    elseif(WWE2K14_GUEST_PGO_USE)
        target_compile_options(wwe2k14_recomp PRIVATE -fprofile-instr-use=${WWE2K14_GUEST_PGO_USE}
                               -Wno-profile-instr-unprofiled -Wno-profile-instr-missing)
        if(WWE2K14_PGO_OURS)
            target_compile_options(wwe2k14 PRIVATE -fprofile-instr-use=${WWE2K14_GUEST_PGO_USE}
                                   -Wno-profile-instr-unprofiled -Wno-profile-instr-missing)
        endif()
    endif()
endif()
if(ANDROID)
    # Session 63: the NDK's libc++ does #include <version>, which the case-insensitive Windows disk resolves to our
    # VERSION file while the repo root is an -I folder. Search the root for "quoted" includes only (nothing includes
    # from it with <...>); the PC build (MSVC STL) never includes <version> and is unchanged.
    foreach(_wwe2k14_t wwe2k14 wwe2k14_recomp)
        if(TARGET ${_wwe2k14_t})
            get_target_property(_wwe2k14_dirs ${_wwe2k14_t} INCLUDE_DIRECTORIES)
            list(REMOVE_ITEM _wwe2k14_dirs "${CMAKE_CURRENT_SOURCE_DIR}")
            set_property(TARGET ${_wwe2k14_t} PROPERTY INCLUDE_DIRECTORIES "${_wwe2k14_dirs}")
            target_compile_options(${_wwe2k14_t} PRIVATE "-iquote${CMAKE_CURRENT_SOURCE_DIR}")
        endif()
    endforeach()
    # main.cpp holds all our headers in one file: report every error of a pass, not the first 20.
    target_compile_options(wwe2k14 PRIVATE -ferror-limit=0)
    # Session 77 (XDET_PLAN.md gap 17): no floating-point contraction. clang's default (-ffp-contract=on) fuses a*b+c
    # into one FMA on ARM64 but cannot on the PC (built without FMA), so our hooks (motion_fix, cinecam, dlaa: 138 float
    # fused ops in APK x77b, tools/xdet/fused_scan.py) computed different bits per device. Explicit std::fma (the
    # guest's fmadd: all of the generated code's fused ops) is unchanged; the PC build is not affected.
    foreach(_wwe2k14_t wwe2k14 wwe2k14_recomp)
        if(TARGET ${_wwe2k14_t})
            target_compile_options(${_wwe2k14_t} PRIVATE -ffp-contract=off)
        endif()
    endforeach()
    target_link_libraries(wwe2k14 PRIVATE android log)
    # Session 98 (phone tron video, src/tron_export.h): the tron export's MP4s through the NDK's encoder + muxer (system libmediandk.so).
    target_link_libraries(wwe2k14 PRIVATE mediandk)
    # Session 72 (Android drivers, src/android_driver.h): libadrenotools (BSD-2-Clause, thirdparty/libadrenotools) loads a
    # bundled custom Vulkan driver; its hook libraries (libhook_impl.so, libmain_hook.so) go into the APK beside libmain.so.
    set(WWE2K14_ADRENOTOOLS "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/libadrenotools")
    if(NOT EXISTS "${WWE2K14_ADRENOTOOLS}/CMakeLists.txt")
        message(FATAL_ERROR "thirdparty/libadrenotools is missing (git clone --recursive https://github.com/bylaws/libadrenotools)")
    endif()
    if(NOT CMAKE_ANDROID_ARCH_ABI)
        set(CMAKE_ANDROID_ARCH_ABI ${ANDROID_ABI})
    endif()
    add_subdirectory("${WWE2K14_ADRENOTOOLS}" adrenotools EXCLUDE_FROM_ALL)
    target_link_libraries(wwe2k14 PRIVATE adrenotools)
    add_dependencies(wwe2k14 hook_impl main_hook)
endif()
# Only main.cpp (our code): a target-wide define would rebuild all the generated code on every version change.
set_property(SOURCE src/main.cpp APPEND PROPERTY COMPILE_DEFINITIONS "WWE2K14_VERSION=\"${WWE2K14_VERSION}\"")
# S103 L33F (review F1): and src/dev_build.cpp, the one place every other TU asks (src/dev_build.h; tools/gate/version_define_lint.py). Never test the macro in another .cpp: it is undefined there.
set_property(SOURCE src/dev_build.cpp APPEND PROPERTY COMPILE_DEFINITIONS "WWE2K14_VERSION=\"${WWE2K14_VERSION}\"")
# S101 lane 19: the build stamp (git hash, dirty flag, SDK pin, APK number) for the case zip's manifest. tools/build_stamp.cmake runs on every
# build and rewrites <build>/stamp/wwe2k14_build_stamp.h only when its text changed; only src/build_stamp.cpp sees that folder (src/build_stamp.h declares Get(); it reads it
# with __has_include, so a build without it says "unknown" and the manifest gives the reason).
set(WWE2K14_STAMP_HEADER "${CMAKE_BINARY_DIR}/stamp/wwe2k14_build_stamp.h")
set(WWE2K14_STAMP_REX_BIN "${rexglue_DIR}/../../../bin|$ENV{REXSDK}/bin")  # '|' not ';': a list would split into separate arguments
add_custom_target(wwe2k14_stamp ALL
    COMMAND "${CMAKE_COMMAND}" "-DSRC=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUT=${WWE2K14_STAMP_HEADER}" "-DREX_BIN=${WWE2K14_STAMP_REX_BIN}"
            "-DSDK_SRC=${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/rexglue-sdk-src" "-DIS_ANDROID=$<BOOL:${ANDROID}>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_stamp.cmake"
    BYPRODUCTS "${WWE2K14_STAMP_HEADER}"
    COMMENT "build stamp"
    VERBATIM)
add_dependencies(wwe2k14 wwe2k14_stamp)
set_property(SOURCE src/build_stamp.cpp APPEND PROPERTY INCLUDE_DIRECTORIES "${CMAKE_BINARY_DIR}/stamp")
# Session 30 (#9): precompiled header for main.cpp: the generated declarations, the SDK, the C++ library, Windows
# (src/pch_main.h). With CMake's default PCH_INSTANTIATE_TEMPLATES (clang: -fpch-instantiate-templates) main.cpp
# compiles in ~24 s instead of ~37 s; the PCH is rebuilt only when codegen or the SDK headers change.
target_precompile_headers(wwe2k14 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/pch_main.h")
# Only for main.cpp: the SDK's own sources in this target (rex_app.cpp, windowed_app_main_sdl.cpp) compile exactly as
# before (windows.h macros in a forced header could rename their identifiers).
get_target_property(WWE2K14_TARGET_SOURCES wwe2k14 SOURCES)
foreach(src IN LISTS WWE2K14_TARGET_SOURCES)
    if(src MATCHES "\\.(cpp|cc|cxx)$" AND NOT src MATCHES "(^|/)src/main\\.cpp$")
        set_source_files_properties("${src}" TARGET_DIRECTORY wwe2k14 PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    endif()
endforeach()

# Session 17: the exe icon (game and launcher): game_logo.ico in the repo root (made from game_logo.png, a fan-made
# SteamGridDB icon, with tools/icon/make_ico.py; both committed: Jarrod, CLAUDE.md rule 1). Icon resource id 1: the
# launcher's window class and the game's window (ApplyWindowIcon) load it. No icon file -> the default icons.
set(WWE2K14_ICON_RC "")
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/game_logo.ico")
    enable_language(RC)
    set(WWE2K14_ICON_RC "${CMAKE_CURRENT_BINARY_DIR}/app_icon.rc")
    file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/game_logo.ico" WWE2K14_ICO)
    # Session 102 W1: a VERSIONINFO (ProductVersion = version + commit): the launcher starts wwe2k14_v3.exe only when it carries the SAME string as wwe2k14.exe
    # (src/launcher/cpu_features.h ExeVersionsMatch). Both release builds come from one commit and one VERSION file.
    execute_process(COMMAND git -C "${CMAKE_CURRENT_SOURCE_DIR}" rev-parse --short HEAD OUTPUT_VARIABLE WWE2K14_COMMIT
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT WWE2K14_COMMIT)
        set(WWE2K14_COMMIT "nogit")
    endif()
    set(WWE2K14_VER_NUMS "0,0,0,0")
    if(WWE2K14_VERSION MATCHES "^v?([0-9]+)\\.([0-9]+)\\.([0-9]+)")
        set(WWE2K14_VER_NUMS "${CMAKE_MATCH_1},${CMAKE_MATCH_2},${CMAKE_MATCH_3},0")
    endif()
    file(WRITE "${WWE2K14_ICON_RC}" "1 ICON \"${WWE2K14_ICO}\"\n"
"1 VERSIONINFO\nFILEVERSION ${WWE2K14_VER_NUMS}\nPRODUCTVERSION ${WWE2K14_VER_NUMS}\nBEGIN\n BLOCK \"StringFileInfo\"\n BEGIN\n  BLOCK \"040904B0\"\n  BEGIN\n"
"   VALUE \"ProductVersion\", \"${WWE2K14_VERSION} ${WWE2K14_COMMIT}\"\n   VALUE \"FileDescription\", \"WWE 2K14 native PC recompilation\"\n  END\n END\n"
" BLOCK \"VarFileInfo\"\n BEGIN\n  VALUE \"Translation\", 0x409, 1200\n END\nEND\n")
    set_source_files_properties("${WWE2K14_ICON_RC}" PROPERTIES OBJECT_DEPENDS "${WWE2K14_ICO}")
    target_sources(wwe2k14 PRIVATE "${WWE2K14_ICON_RC}")
    target_compile_definitions(wwe2k14 PRIVATE WWE2K14_HAS_ICON=1)
endif()

# Session 33 (FSR 3): AMD's FidelityFX runtime (MIT; built from thirdparty/FidelityFX-SDK, ffx-api, by
# tools/fsr/build_ffx.ps1) next to the exe; the SDK loads it at run time when FSR is picked. Missing = no FSR.
set(WWE2K14_FFX_DLL "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/FidelityFX-SDK/ffx-api/bin/amd_fidelityfx_vk.dll")
if(WIN32 AND EXISTS "${WWE2K14_FFX_DLL}")
    add_custom_command(TARGET wwe2k14 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${WWE2K14_FFX_DLL}" "$<TARGET_FILE_DIR:wwe2k14>"
        VERBATIM)
endif()

# Session 100 settings parity S10b: DLSS in the new renderer through NVIDIA Streamline (headers only; the runtime is the player's folder).
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/streamline/sdk/include/sl.h")
    target_include_directories(wwe2k14 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/streamline/sdk/include")
    target_compile_definitions(wwe2k14 PRIVATE RDR_HAS_STREAMLINE=1)
endif()

# Linker map (symbol -> address) next to the exe, so native crash offsets from the
# Windows event log (e.g. "Fault offset: 0x...") can be traced to a recompiled sub_XXXXXXXX.
if(WIN32)
    target_link_options(wwe2k14 PRIVATE "LINKER:/MAP:$<TARGET_FILE_DIR:wwe2k14>/wwe2k14.map")
endif()

# S102 S0-6: wwe2k14.pdb for profiling (Surface stacks profile: tools/surface/push_kit.ps1 -Symbols). OFF by default; the pdb is NEVER shipped
# (make_release.ps1 refuses .pdb; tools/release/check_zip_symbols.py). Debug info does not change code generation; the one thing /DEBUG would
# change is lld-link's default /OPT:REF /OPT:ICF (it turns them off), so they are passed explicitly. Cheapest tier by default:
#   our sources (src/*)  -gline-tables-only -gcodeview   (function names + lines)
#   recompiled guest code  nothing at compile time: the pdb still gets its function names from the linker's public symbols (S_PUB32)
#   -DWWE2K14_EXE_SYMBOLS_GUEST_LINES=ON  also line tables for the guest code (much bigger pdb, slower link)
option(WWE2K14_EXE_SYMBOLS "Emit wwe2k14.pdb (symbols for profiling; never shipped)" OFF)
option(WWE2K14_EXE_SYMBOLS_GUEST_LINES "With WWE2K14_EXE_SYMBOLS: line tables for the recompiled guest code too" OFF)
if(WIN32 AND WWE2K14_EXE_SYMBOLS)
    target_compile_options(wwe2k14 PRIVATE -gline-tables-only -gcodeview)
    if(WWE2K14_EXE_SYMBOLS_GUEST_LINES AND TARGET wwe2k14_recomp)
        target_compile_options(wwe2k14_recomp PRIVATE -gline-tables-only -gcodeview)
    endif()
    target_link_options(wwe2k14 PRIVATE "LINKER:/DEBUG" "LINKER:/OPT:REF" "LINKER:/OPT:ICF"
                        "LINKER:/PDB:$<TARGET_FILE_DIR:wwe2k14>/wwe2k14.pdb")
endif()

# Session 17 item 1C: the launcher, a separate Windows program next to wwe2k14.exe (src/launcher/). Dear ImGui on
# Win32 + Direct3D 11 and the static C runtime, so users need nothing extra installed. It does not link the game or
# the SDK; it only uses the SDK source tree's vendored Dear ImGui and nlohmann/json.
if(WIN32)
    set(WWE2K14_SDK_THIRDPARTY "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/rexglue-sdk-src/thirdparty")
    set(WWE2K14_IMGUI "${WWE2K14_SDK_THIRDPARTY}/imgui")
    if(EXISTS "${WWE2K14_IMGUI}/imgui.cpp")
        add_executable(wwe2k14_launcher WIN32
            src/launcher/launcher_main.cpp
            src/importer/importer_core.cpp  # Session 98: the Superstar Importer core (the MODS > SUPERSTARS list)
            src/audio_import/audio_import.cpp src/audio_import/lookup.cpp  # lane 283: the song importer (MUSIC tab: decode, normalise, cover art)
            ${WWE2K14_IMGUI}/imgui.cpp ${WWE2K14_IMGUI}/imgui_draw.cpp ${WWE2K14_IMGUI}/imgui_tables.cpp
            ${WWE2K14_IMGUI}/imgui_widgets.cpp ${WWE2K14_IMGUI}/misc/cpp/imgui_stdlib.cpp
            ${WWE2K14_IMGUI}/backends/imgui_impl_win32.cpp ${WWE2K14_IMGUI}/backends/imgui_impl_dx11.cpp)
        target_include_directories(wwe2k14_launcher PRIVATE src/launcher ${WWE2K14_IMGUI} ${WWE2K14_IMGUI}/backends
            "${WWE2K14_SDK_THIRDPARTY}/inja/third_party/include" "${WWE2K14_SDK_THIRDPARTY}/stb"
            "${WWE2K14_SDK_THIRDPARTY}/Vulkan-Headers/include")  # Session 37: src/vk_devices.h (headers only)
        target_compile_definitions(wwe2k14_launcher PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING
            WWE2K14_VERSION="${WWE2K14_VERSION}")
        set_property(TARGET wwe2k14_launcher PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
        wwe2k14_add_opus(wwe2k14_launcher)  # lane 283: Ogg Opus songs
        if(WWE2K14_ICON_RC)
            target_sources(wwe2k14_launcher PRIVATE "${WWE2K14_ICON_RC}")
        endif()
        # Session 31: the logo (assets/launcher/wwe2k14_logo.png, from Jarrod's own trace wwe2k14_logo.svg; committed,
        # Jarrod, CLAUDE.md rule 1) as RCDATA resource 101. No file -> the launcher draws the name in text.
        # Session 67: + the KEYBOARD CONTROLS pad pictures as 102 / 103 (assets/launcher/pad_360*.png, made by
        # tools/launcher/make_pad_png.py from Alphathon's "360 controller.svg", Wikimedia Commons, CC BY-SA 3.0, which
        # Jarrod added). A missing file is left out (the launcher says the picture is missing).
        set(WWE2K14_LAUNCHER_PNGS "101=wwe2k14_logo.png" "102=pad_360.png" "103=pad_360_parts.png")
        set(WWE2K14_LOGO_RC_TEXT "")
        set(WWE2K14_LOGO_RC_DEPENDS "")
        foreach(entry ${WWE2K14_LAUNCHER_PNGS})
            string(REPLACE "=" ";" parts "${entry}")
            list(GET parts 0 res_id)
            list(GET parts 1 res_file)
            set(png "${CMAKE_CURRENT_SOURCE_DIR}/assets/launcher/${res_file}")
            if(EXISTS "${png}")
                file(TO_CMAKE_PATH "${png}" png_path)
                string(APPEND WWE2K14_LOGO_RC_TEXT "${res_id} RCDATA \"${png_path}\"\n")
                list(APPEND WWE2K14_LOGO_RC_DEPENDS "${png}")
            endif()
        endforeach()
        if(WWE2K14_LOGO_RC_TEXT)
            enable_language(RC)
            set(WWE2K14_LOGO_RC "${CMAKE_CURRENT_BINARY_DIR}/launcher_logo.rc")
            file(WRITE "${WWE2K14_LOGO_RC}" "${WWE2K14_LOGO_RC_TEXT}")
            set_source_files_properties("${WWE2K14_LOGO_RC}" PROPERTIES OBJECT_DEPENDS "${WWE2K14_LOGO_RC_DEPENDS}")
            target_sources(wwe2k14_launcher PRIVATE "${WWE2K14_LOGO_RC}")
        endif()
    else()
        message(WARNING "wwe2k14: Dear ImGui not found at ${WWE2K14_IMGUI}: the launcher is not built")
    endif()
endif()

# Session 30 (#4): the probe plugin (probes/, our code): a small DLL the coverage build loads and reloads at run time
# (src/probe_plugin.h). Built next to the game in every preset (it only has an effect in the coverage build); rebuild
# it alone while a game runs: pwsh -File tools\probes\build_probes.ps1.
if(WIN32)
    add_library(wwe2k14_probes SHARED probes/probes.cpp)
    target_include_directories(wwe2k14_probes PRIVATE probes)
    # /EHa: a fault (access violation) inside the plugin unwinds its C++ objects too, so a lock_guard it held is
    # released before the game's fault guard catches it (review, Session 30).
    target_compile_options(wwe2k14_probes PRIVATE $<$<CXX_COMPILER_FRONTEND_VARIANT:MSVC>:/EHa>
                                                  $<$<CXX_COMPILER_FRONTEND_VARIANT:GNU>:-fasync-exceptions>)
    set_target_properties(wwe2k14_probes PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:wwe2k14>"
                                                   MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

# Session 8 task 6: scenario coverage build (preset win-amd64-coverage only; OFF everywhere else, where nothing
# below applies). Adds src/coverage_prologue.h to the generated code's precompiled header after wwe2k14_pch.h, so
# every guest function's REX_FUNC_PROLOGUE() records its first call per input-script window (src/coverage.h).
option(WWE2K14_COVERAGE "Instrumented coverage build (see src/coverage.h)" OFF)
if(WWE2K14_COVERAGE AND TARGET wwe2k14_recomp)
    target_precompile_headers(wwe2k14_recomp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/coverage_prologue.h")
    target_compile_definitions(wwe2k14 PRIVATE WWE2K14_COVERAGE=1)
    message(STATUS "wwe2k14: COVERAGE build (guest function prologues instrumented)")
elseif(WWE2K14_COVERAGE)
    message(WARNING "wwe2k14: WWE2K14_COVERAGE=ON but there is no generated code yet (run codegen): not instrumented")
endif()

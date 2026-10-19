# wwe2k14 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(wwe2k14 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Session 25: one version string (VERSION, e.g. v0.1.0-playtest) for the game and the launcher; it is in every game
# log, perf report and problem report. Builds made by tools/release/make_release.ps1 pass -DWWE2K14_RELEASE=ON; every
# other build is marked "+dev".
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" WWE2K14_VERSION LIMIT_COUNT 1)
option(WWE2K14_RELEASE "A release build (tools/release/make_release.ps1)" OFF)
if(NOT WWE2K14_RELEASE)
    string(APPEND WWE2K14_VERSION "+dev")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
message(STATUS "wwe2k14 version ${WWE2K14_VERSION}")

# Sources
set(WWE2K14_SOURCES
    src/main.cpp
)

if(WIN32)
    add_executable(wwe2k14 WIN32 ${WWE2K14_SOURCES})
else()
    add_executable(wwe2k14 ${WWE2K14_SOURCES})
endif()

rexglue_setup_target(wwe2k14 GPU_PLUGINS xenos)
# Only main.cpp (our code): a target-wide define would rebuild all the generated code on every version change.
set_property(SOURCE src/main.cpp APPEND PROPERTY COMPILE_DEFINITIONS "WWE2K14_VERSION=\"${WWE2K14_VERSION}\"")
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
    file(WRITE "${WWE2K14_ICON_RC}" "1 ICON \"${WWE2K14_ICO}\"\n")
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

# Linker map (symbol -> address) next to the exe, so native crash offsets from the
# Windows event log (e.g. "Fault offset: 0x...") can be traced to a recompiled sub_XXXXXXXX.
if(WIN32)
    target_link_options(wwe2k14 PRIVATE "LINKER:/MAP:$<TARGET_FILE_DIR:wwe2k14>/wwe2k14.map")
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
            ${WWE2K14_IMGUI}/imgui.cpp ${WWE2K14_IMGUI}/imgui_draw.cpp ${WWE2K14_IMGUI}/imgui_tables.cpp
            ${WWE2K14_IMGUI}/imgui_widgets.cpp ${WWE2K14_IMGUI}/misc/cpp/imgui_stdlib.cpp
            ${WWE2K14_IMGUI}/backends/imgui_impl_win32.cpp ${WWE2K14_IMGUI}/backends/imgui_impl_dx11.cpp)
        target_include_directories(wwe2k14_launcher PRIVATE src/launcher ${WWE2K14_IMGUI} ${WWE2K14_IMGUI}/backends
            "${WWE2K14_SDK_THIRDPARTY}/inja/third_party/include" "${WWE2K14_SDK_THIRDPARTY}/stb")
        target_compile_definitions(wwe2k14_launcher PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS
            WWE2K14_VERSION="${WWE2K14_VERSION}")
        set_property(TARGET wwe2k14_launcher PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
        if(WWE2K14_ICON_RC)
            target_sources(wwe2k14_launcher PRIVATE "${WWE2K14_ICON_RC}")
        endif()
        # Session 31: the logo (assets/launcher/wwe2k14_logo.png, from Jarrod's own trace wwe2k14_logo.svg; committed,
        # Jarrod, CLAUDE.md rule 1) as RCDATA resource 101. No file -> the launcher draws the name in text.
        set(WWE2K14_LOGO_PNG "${CMAKE_CURRENT_SOURCE_DIR}/assets/launcher/wwe2k14_logo.png")
        if(EXISTS "${WWE2K14_LOGO_PNG}")
            enable_language(RC)
            set(WWE2K14_LOGO_RC "${CMAKE_CURRENT_BINARY_DIR}/launcher_logo.rc")
            file(TO_CMAKE_PATH "${WWE2K14_LOGO_PNG}" WWE2K14_LOGO_PNG_PATH)
            file(WRITE "${WWE2K14_LOGO_RC}" "101 RCDATA \"${WWE2K14_LOGO_PNG_PATH}\"\n")
            set_source_files_properties("${WWE2K14_LOGO_RC}" PROPERTIES OBJECT_DEPENDS "${WWE2K14_LOGO_PNG}")
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

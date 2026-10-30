# wwe2k14 phone launcher (Session 71): its own small CMake project, built by tools\android\build_launcher.ps1.
#   Android: liblauncher.so (SDL_main for LauncherActivity), arm64-v8a, SDL3 linked in statically.
#   Windows: wwe2k14_launcher_mobile.exe, the same launcher in a phone-shaped window (the test build).
# SDL3, Dear ImGui, stb and nlohmann/json come from the SDK source tree's vendored copies (the same SDL the game's
# librexruntime.so carries, so the Java side in the APK matches both).
cmake_minimum_required(VERSION 3.25)
project(wwe2k14_launcher_mobile LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(TP "${REPO}/thirdparty/rexglue-sdk-src/thirdparty")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")  # SDL too: one static C runtime

# SDL3, static, nothing else from it.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
add_subdirectory("${TP}/sdl3" sdl3 EXCLUDE_FROM_ALL)

set(IMGUI "${TP}/imgui")
set(SOURCES
    mobile_main.cpp platform_common.cpp
    ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
    ${IMGUI}/backends/imgui_impl_sdl3.cpp ${IMGUI}/backends/imgui_impl_sdlrenderer3.cpp)

# The Android port's own version (android\VERSION, e.g. android-v0.1.0; Jarrod 2026-10-28: "call it android 0.1.0"),
# separate from the PC's VERSION; its releases are tagged with it.
file(STRINGS "${REPO}/android/VERSION" WWE2K14_VERSION LIMIT_COUNT 1)
# Session 72: re-run CMake when it changes (without this an incremental build kept the old version: 0.1.3 showed 0.1.2).
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${REPO}/android/VERSION")

if(ANDROID)
    add_library(launcher SHARED ${SOURCES} platform_android.cpp)
    # SDL's JNI entry points (JNI_OnLoad, nativeSetupJNI, ...) are referenced by nothing native: keep them all.
    target_link_libraries(launcher PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,SDL3::SDL3-static>" android log)
    target_link_options(launcher PRIVATE "-Wl,-z,max-page-size=16384")
    set(TARGET launcher)
else()
    add_executable(wwe2k14_launcher_mobile ${SOURCES} platform_desktop.cpp)
    target_link_libraries(wwe2k14_launcher_mobile PRIVATE SDL3::SDL3-static)
    target_compile_definitions(wwe2k14_launcher_mobile PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    # The logo next to the exe (the APK carries it in its assets).
    add_custom_command(TARGET wwe2k14_launcher_mobile POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${REPO}/assets/launcher/wwe2k14_logo.png"
                "$<TARGET_FILE_DIR:wwe2k14_launcher_mobile>/wwe2k14_logo.png")
    set(TARGET wwe2k14_launcher_mobile)
endif()
target_include_directories(${TARGET} PRIVATE ${IMGUI} ${IMGUI}/backends "${TP}/stb" "${TP}/inja/third_party/include"
                           "${TP}/vulkan-headers/include")  # Session 73: phone_check.h
target_compile_definitions(${TARGET} PRIVATE "WWE2K14_VERSION=\"${WWE2K14_VERSION}\"")

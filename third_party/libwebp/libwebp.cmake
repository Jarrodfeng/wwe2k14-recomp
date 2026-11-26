# libwebp 1.6.0 (see PIN.txt): the lossless WebP screenshot writer (src/webp_shot.h) and its decoder for the unit. Own static library of C sources,
# for the same reason as opus (third_party/audio/opus/opus_sources.cmake): the game target has a C++ precompiled header. Warnings off (vendored).
# include() this file, then wwe2k14_add_webp(<target>).
function(wwe2k14_add_webp target)
    set(_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    set(_lib ${target}_webp)
    file(GLOB _src "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/dec/*.c" "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/dsp/*.c" "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/enc/*.c"
         "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/utils/*.c" "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/sharpyuv/*.c")
    add_library(${_lib} STATIC ${_src})
    set_target_properties(${_lib} PROPERTIES LINKER_LANGUAGE C C_STANDARD 11 DISABLE_PRECOMPILE_HEADERS ON POSITION_INDEPENDENT_CODE ON)
    target_compile_options(${_lib} PRIVATE -w)
    # clang on Windows defines _MSC_VER, so libwebp's cpu.h turns SSE4.1 / AVX2 on for the whole library without the flags clang needs for the intrinsics: those two file families get them
    # (what libwebp's own CMake does). The code runs only where VP8GetCPUInfo says the CPU has them.
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64|x64|X86|i686")
        file(GLOB _sse41 "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/dsp/*_sse41.c")
        file(GLOB _avx2 "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/dsp/*_avx2.c")
        set_source_files_properties(${_sse41} PROPERTIES COMPILE_OPTIONS "-msse4.1")
        set_source_files_properties(${_avx2} PROPERTIES COMPILE_OPTIONS "-mavx2;-msse4.1")
    endif()
    target_include_directories(${_lib} PUBLIC "${_dir}")
    get_target_property(_rt ${target} MSVC_RUNTIME_LIBRARY)
    if(_rt)
        set_property(TARGET ${_lib} PROPERTY MSVC_RUNTIME_LIBRARY "${_rt}")
    endif()
    target_link_libraries(${target} PRIVATE ${_lib})
endfunction()

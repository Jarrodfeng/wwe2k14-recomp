# libopus 1.5.2 (decoder use only; BSD-3-Clause, see COPYING), built as its OWN static library of C sources. Lane 283: Ogg Opus songs.
# include() this file, then wwe2k14_add_opus(<target>): creates <target>_opus and links it into <target>.
# A separate library (not C sources inside the game target): the game target has a precompiled C++ header (pch_main.h) that CMake would
# otherwise also build for the C files, and a C compile of <utility> fails. Warnings are off for this vendored code.
function(wwe2k14_add_opus target)
    set(_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    set(_lib ${target}_opus)
    file(STRINGS "${_dir}/sources.txt" _rel)
    set(_src "")
    foreach(r ${_rel})
        list(APPEND _src "${_dir}/${r}")
    endforeach()
    add_library(${_lib} STATIC ${_src})
    set_target_properties(${_lib} PROPERTIES LINKER_LANGUAGE C C_STANDARD 11 DISABLE_PRECOMPILE_HEADERS ON)
    target_compile_options(${_lib} PRIVATE -w)
    target_include_directories(${_lib} PRIVATE "${_dir}/include" "${_dir}/celt" "${_dir}/silk" "${_dir}/silk/float" "${_dir}")
    target_compile_definitions(${_lib} PRIVATE OPUS_BUILD USE_ALLOCA PACKAGE_VERSION="1.5.2")
    get_target_property(_rt ${target} MSVC_RUNTIME_LIBRARY)  # the same C runtime as the target it joins (the launcher is /MT)
    if(_rt)
        set_property(TARGET ${_lib} PROPERTY MSVC_RUNTIME_LIBRARY "${_rt}")
    endif()
    target_link_libraries(${target} PRIVATE ${_lib})
endfunction()

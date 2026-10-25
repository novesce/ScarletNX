# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# a64dec (tools/a64dec/a64dec.c) generates the A64 decoder from QEMU's a64.decode and ScarletNX's profile of it
# (src/common/cpu/aarch64/decode/). It runs during the build, so a cross build also builds it for the machine doing
# the build, with that machine's C compiler (SNX_HOST_CC), as shaderdec.cmake does for shaderdec
include_guard(GLOBAL)
if(CMAKE_CROSSCOMPILING)
    include("${CMAKE_CURRENT_LIST_DIR}/host_cc.cmake")
    include(ExternalProject)
    set(snx_a64dec_dir "${PROJECT_BINARY_DIR}/a64dec-host")
    set(snx_a64dec_exe "${snx_a64dec_dir}/bin/a64dec")
    if(CMAKE_HOST_WIN32)
        string(APPEND snx_a64dec_exe ".exe")
    endif()
    ExternalProject_Add(snx_a64dec_host
        SOURCE_DIR "${PROJECT_SOURCE_DIR}/tools/a64dec"
        BINARY_DIR "${snx_a64dec_dir}"
        # no -Werror: the host compiler may not be clang (SNX_HOST_CC)
        CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_COMPILER=${SNX_HOST_CC}"
                   "-DCMAKE_C_FLAGS=-pedantic-errors -Wall -Wextra"
                   "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=${snx_a64dec_dir}/bin"
                   "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE=${snx_a64dec_dir}/bin"
        BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config Release
        BUILD_ALWAYS 1
        BUILD_BYPRODUCTS "${snx_a64dec_exe}"
        INSTALL_COMMAND "")
    add_executable(snx-a64dec IMPORTED GLOBAL)
    set_target_properties(snx-a64dec PROPERTIES IMPORTED_LOCATION "${snx_a64dec_exe}")
    add_dependencies(snx-a64dec snx_a64dec_host)
else()
    add_executable(snx-a64dec "${PROJECT_SOURCE_DIR}/tools/a64dec/a64dec.c")
    set_target_properties(snx-a64dec PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON
                                                C_EXTENSIONS OFF RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
    target_compile_options(snx-a64dec PRIVATE -pedantic-errors -Wall -Wextra -Werror)
endif()

# snx_a64dec(<target>): generates a64_decode.h (the types) and a64_decode.inc (the decoder) into the build folder and
# gives <target> their include root. sve.decode and sme.decode join a64.decode in the list when they are vendored
function(snx_a64dec target)
    set(dir "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/decode")
    set(profile "${dir}/a64.profile")
    set(decode "${dir}/a64.decode")
    set(out "${CMAKE_CURRENT_BINARY_DIR}/a64dec")
    add_custom_command(OUTPUT "${out}/a64_decode.h" "${out}/a64_decode.inc"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${out}"
        COMMAND snx-a64dec --header "${out}/a64_decode.h" --decoder "${out}/a64_decode.inc" "${profile}" ${decode}
        DEPENDS snx-a64dec "${profile}" ${decode}
        COMMENT "Generating the A64 instruction decoder" VERBATIM)
    target_sources(${target} PRIVATE "${out}/a64_decode.h" "${out}/a64_decode.inc")
    target_include_directories(${target} PUBLIC "${out}")
endfunction()

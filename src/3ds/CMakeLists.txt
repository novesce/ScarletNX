# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The board, loader and memory libraries need neither the NX runtime nor an Azahar build tree. The driver links LLVM
# for its compiler, and its presenter and audio output use SDL3 and the Vulkan headers.
add_library(snx_ctr_board STATIC board/board.c)
set_target_properties(snx_ctr_board PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(snx_ctr_board PRIVATE -Wall -Wextra -Werror=missing-prototypes)
add_library(snx_ctr_memory STATIC memory/memory.c memory/host.c)
target_link_libraries(snx_ctr_memory PUBLIC snx_ctr_board scarletnx-common)
add_library(snx_ctr_cpu STATIC cpu/cpu.c)
target_link_libraries(snx_ctr_cpu PUBLIC snx_ctr_memory)
add_library(snx_ctr_loader STATIC loader/loader.c loader/cia.c loader/ncsd.c loader/ncch.c loader/romfs.c loader/blz.c
            loader/tar.c loader/z3ds.c loader/image.c loader/mapped.c loader/decrypt.c)
# a game read where it lies is mapped as the indexed filesystem maps the files it shares (plat_file_share)
target_link_libraries(snx_ctr_loader PUBLIC scarletnx-common scarletnx-filesystem OpenSSL::Crypto snx_zstd)
target_compile_definitions(snx_ctr_loader PRIVATE _FILE_OFFSET_BITS=64 _CRT_SECURE_NO_WARNINGS)
add_library(snx_ctr_process STATIC runtime/process.c kernel/kernel.c kernel/thread.c aot/dispatch.c ipc/ipc.c services/srv.c services/fs.c services/apt.c services/ptm.c services/hid.c services/romfs.c services/file.c services/gsp.c services/dsp.c services/cfg.c services/system.c services/am.c services/y2r.c)
# the system archives' stand-ins (cmake/ctr_system_archives.cmake), which services/fs.c serves through the NCCH archive
target_sources(snx_ctr_process PRIVATE "${PROJECT_BINARY_DIR}/generated/ctr_system_archives.c")
target_sources(snx_ctr_process PRIVATE services/uds.c)
target_link_libraries(snx_ctr_process PUBLIC scarletnx-ldn)
# the player's input lives beside the presenter, whose window's events it reads
add_library(snx_ctr_gpu STATIC gpu/present.c input/input.c gpu/transfer.c gpu/y2r.c gpu/vk/y2r.c gpu/vk/device.c gpu/vk/transfer.c
            gpu/vk/vsgen.c gpu/vk/fsgen.c gpu/vk/render.c gpu/pica.c "${volk_SOURCE_DIR}/volk.c")
target_include_directories(snx_ctr_gpu PUBLIC "${volk_SOURCE_DIR}")
target_link_libraries(snx_ctr_gpu PUBLIC snx_ctr_memory scarletnx-common Vulkan::Headers SDL3::SDL3 snx_settings snx_build_info
                                         snx_window_icon ${CMAKE_DL_LIBS})
set_source_files_properties("${volk_SOURCE_DIR}/volk.c" PROPERTIES COMPILE_OPTIONS "${snx_third_party_flags}")
# the GPU's compute shaders, compiled at build time (cmake/glsl.cmake)
include("${PROJECT_SOURCE_DIR}/cmake/glsl.cmake")
foreach(shader fill display_transfer texture_copy present texture color depth y2r)
    snx_glsl_spirv(snx_ctr_gpu SOURCE gpu/vk/shaders/${shader}.comp NAME snx_ctr_${shader}_spv
                   HEADER 3ds/gpu/vk/shaders/${shader}_spv.h)
endforeach()
# the PICA's software renderer, ported from Azahar's: the CPU reference the GPU's draws are tested against, which the
# runtime does not draw with
add_library(snx_ctr_gpu_reference STATIC gpu/shader.c gpu/draw.c gpu/raster.c gpu/lighting.c gpu/texture.c)
target_link_libraries(snx_ctr_gpu_reference PUBLIC snx_ctr_gpu m)
# the HLE DSP and its SDL3 output
add_library(snx_ctr_audio STATIC audio/dsp.c audio/source.c audio/mixers.c audio/output.c)
target_link_libraries(snx_ctr_audio PUBLIC snx_ctr_memory SDL3::SDL3)
# snx_settings_runtime: g_settings and the 3DS debug table (g_ctr_debug_ini, src/nx/runtime/settings.txt)
target_link_libraries(snx_ctr_process PUBLIC snx_ctr_cpu snx_ctr_loader scarletnx-filesystem snx_ctr_gpu snx_ctr_audio
                      snx_settings_runtime)
include("${PROJECT_SOURCE_DIR}/cmake/swipc.cmake")
snx_swipc(snx_ctr_process CTR ROOT "${CMAKE_CURRENT_SOURCE_DIR}" FILES services/srv.swipc services/fs.swipc services/apt.swipc services/ptm.swipc services/hid.swipc services/file.swipc)
foreach(target snx_ctr_memory snx_ctr_loader snx_ctr_cpu snx_ctr_process snx_ctr_gpu snx_ctr_gpu_reference snx_ctr_audio)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror=missing-prototypes)
endforeach()
# The driver: the offline compiler and the runner, and its command line (runtime/main.c's snx_ctr_main), which is
# scarletnx-3ds's own program, or in a build where scarletnx is also this driver (SNX_MULTICALL, src/multicall) part
# of this library, the name scarletnx-3ds a link to scarletnx
add_library(snx_ctr_program STATIC)
target_link_libraries(snx_ctr_program PUBLIC snx_ctr_board snx_ctr_loader)
set_target_properties(snx_ctr_program PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(snx_ctr_program PRIVATE -Wall -Wextra -Werror=missing-prototypes)
# its checkout, where an older version kept work/ (snx_checkout)
set(ctr_main_definitions SNX_SOURCE_DIR="${PROJECT_SOURCE_DIR}" SNX_BUILD_DIR="${PROJECT_BINARY_DIR}")
if(WIN32)
    target_compile_definitions(snx_ctr_program PRIVATE _CRT_SECURE_NO_WARNINGS)
    list(APPEND ctr_main_definitions _CRT_SECURE_NO_WARNINGS)
endif()
if(SNX_MULTICALL)
    target_sources(snx_ctr_program PRIVATE runtime/main.c)
    target_compile_definitions(snx_ctr_program PRIVATE ${ctr_main_definitions} SNX_MULTICALL)
    set(SNX_CTR_PROGRAM "${PROJECT_BINARY_DIR}/scarletnx-3ds" PARENT_SCOPE) # what the tests run
else()
    add_executable(scarletnx-3ds runtime/main.c)
    target_link_libraries(scarletnx-3ds PRIVATE snx_ctr_program)
    target_compile_definitions(scarletnx-3ds PRIVATE ${ctr_main_definitions})
    set_target_properties(scarletnx-3ds PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON
        RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
    target_compile_options(scarletnx-3ds PRIVATE -Wall -Wextra -Werror=missing-prototypes)
    set(SNX_CTR_PROGRAM "$<TARGET_FILE:scarletnx-3ds>" PARENT_SCOPE)
    # after each build the program is copied beside every 3DS game (recompiled/<title ID>/<title ID>), as scarletnx
    # is beside every Switch game (cmake/deploy.cmake), so a runtime fix reaches them without recompiling. A sanitized
    # test build's stays here
    if(SNX_DEPLOY AND NOT (SNX_TEST_SANITIZERS AND BUILD_TESTING))
        add_custom_command(TARGET scarletnx-3ds POST_BUILD
            COMMAND "${CMAKE_COMMAND}" "-DCTR_RUNTIME=$<TARGET_FILE:scarletnx-3ds>" "-DGAMES=${SNX_RECOMPILED_DIR}"
                    "-DWINDOWS=${WIN32}" "-DMACOS=${APPLE}" -P "${PROJECT_SOURCE_DIR}/cmake/deploy.cmake"
            VERBATIM)
    endif()
    # Windows: the runtime's application manifest, as scarletnx has it (src/nx/runtime/driver/CMakeLists.txt). Its
    # longPathAware lets the native linker start in a staging folder past MAX_PATH (aot/compiler.c), its UTF-8 code
    # page lets a CIA's path hold any character
    if(WIN32)
        if(CMAKE_C_SIMULATE_ID STREQUAL "MSVC")
            target_link_options(scarletnx-3ds PRIVATE
                                "LINKER:/MANIFESTINPUT:${PROJECT_SOURCE_DIR}/src/common/platform/runtime.manifest")
        else()
            enable_language(RC)
            target_sources(scarletnx-3ds PRIVATE "${PROJECT_SOURCE_DIR}/src/common/platform/runtime.rc")
            target_include_directories(scarletnx-3ds PRIVATE
                                       "$<$<COMPILE_LANGUAGE:RC>:${PROJECT_SOURCE_DIR}/src/common/platform>")
        endif()
    endif()
endif()
include(GNUInstallDirs)
include("${PROJECT_SOURCE_DIR}/cmake/llvm.cmake")
if(NOT TARGET snx_arm_llvm)
    add_subdirectory("${PROJECT_SOURCE_DIR}/src/common/cpu/arm" ctr-arm-backend)
endif()
set(ctr_runtime_source "${CMAKE_CURRENT_BINARY_DIR}/ctr-runtime-$<CONFIG>.c")
add_custom_command(OUTPUT "${ctr_runtime_source}"
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=$<TARGET_FILE:snx_arm_rt>" "-DOUTPUT=${ctr_runtime_source}"
        -P "${PROJECT_SOURCE_DIR}/cmake/ctr-runtime.cmake"
    DEPENDS snx_arm_rt "${PROJECT_SOURCE_DIR}/cmake/ctr-runtime.cmake" VERBATIM)
# The offline compiler is part of the driver, as the Switch driver's recompiler is part of scarletnx
target_sources(snx_ctr_program PRIVATE aot/compiler.c aot/discover.c "${ctr_runtime_source}")
target_link_libraries(snx_ctr_program PUBLIC snx_arm_llvm)
# The compiler loads the LLVM toolchain's own libraries (including its z3)
# at startup: resolve them without a caller-provided environment. A static
# LLVM (SNX_LLVM_SHARED off, cmake/llvm.cmake) has none to load.
if(SNX_LLVM_SHARED)
    set(ctr_llvm_rpath "${SNX_LLVM_LIBRARY_DIR}")
    find_library(SNX_Z3_LIBRARY NAMES z3 libz3.so.4
        HINTS "${SNX_LLVM_LIBRARY_DIR}" "${SNX_LLVM_LIBRARY_DIR}/../.."
        PATH_SUFFIXES lib x86_64-linux-gnu)
    if(SNX_Z3_LIBRARY)
        get_filename_component(ctr_z3_dir "${SNX_Z3_LIBRARY}" DIRECTORY)
        string(APPEND ctr_llvm_rpath ":${ctr_z3_dir}")
        # Forced into the dependency table (--as-needed would drop unreferenced
        # libraries): libLLVM's own z3/xml2 dependencies then resolve through
        # the binary's search path instead of the caller's environment.
        find_library(SNX_XML2_LIBRARY NAMES xml2 libxml2.so.2 HINTS "${ctr_z3_dir}" "${SNX_LLVM_LIBRARY_DIR}"
                        PATH_SUFFIXES lib x86_64-linux-gnu)
        if(APPLE) # ld64 keeps every library named, and has no --as-needed
            target_link_libraries(snx_ctr_program INTERFACE "${SNX_Z3_LIBRARY}" ${SNX_XML2_LIBRARY})
        else()
            target_link_libraries(snx_ctr_program INTERFACE "-Wl,--no-as-needed" "${SNX_Z3_LIBRARY}"
                                  ${SNX_XML2_LIBRARY} "-Wl,--as-needed")
        endif()
    endif()
    if(APPLE) # ld64 takes one folder per -rpath
        string(REPLACE ":" ";" ctr_llvm_rpaths "${ctr_llvm_rpath}")
        foreach(path IN LISTS ctr_llvm_rpaths)
            target_link_options(snx_ctr_program INTERFACE "-Wl,-rpath,${path}")
        endforeach()
    else()
        target_link_options(snx_ctr_program INTERFACE "-Wl,-rpath,${ctr_llvm_rpath}")
    endif()
endif()
target_compile_definitions(snx_ctr_program PRIVATE "SNX_CTR_HOST_CC=\"${CMAKE_C_COMPILER}\"")
add_library(snx_ctr_native STATIC aot/load.c)
target_link_libraries(snx_ctr_native PUBLIC snx_ctr_process ${CMAKE_DL_LIBS})
set_target_properties(snx_ctr_native PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(snx_ctr_native PRIVATE -Wall -Wextra -Werror=missing-prototypes)
target_sources(snx_ctr_program PRIVATE aot/run.c aot/jit.c)
# the run-time compiler for the blocks the static module lacks: the LLVM backend's executor (exec.c, in snx_llvm, which
# the Switch driver's build defines)
if(TARGET snx_llvm)
    target_link_libraries(snx_ctr_program PUBLIC snx_llvm)
    target_compile_definitions(snx_ctr_program PRIVATE SNX_CTR_JIT)
endif()
target_link_libraries(snx_ctr_program PUBLIC snx_ctr_native)
if(NOT SNX_MULTICALL)
    install(TARGETS scarletnx-3ds RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

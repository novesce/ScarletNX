# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The PS Vita driver: the VPK's extraction and the eboot's loader, the offline compiler (the A57-profile AArch32 block
# compiler, src/common/cpu/arm, whose ARMv8 AArch32 holds the Vita's ARMv7-A user mode), and the runtime: the kernel and
# system libraries answered by NID, sceGxm on the host's Vulkan device
add_library(snx_vita_loader STATIC loader/inflate.c loader/vpk.c loader/image.c)
target_link_libraries(snx_vita_loader PUBLIC scarletnx-common OpenSSL::Crypto)
target_compile_definitions(snx_vita_loader PRIVATE _FILE_OFFSET_BITS=64 _CRT_SECURE_NO_WARNINGS)

# the USSE translator, which the runtime and snx-vita-usse (the shader corpus' tool) share
add_library(snx_vita_usse STATIC gpu/usse.c)
target_link_libraries(snx_vita_usse PUBLIC scarletnx-common)
add_executable(snx-vita-usse "${PROJECT_SOURCE_DIR}/tools/snx_vita_usse.c")
target_link_libraries(snx-vita-usse PRIVATE snx_vita_usse)
set_target_properties(snx-vita-usse PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")

add_library(snx_vita_program STATIC
    aot/compiler.c aot/load.c
    runtime/process.c runtime/run.c runtime/jit.c runtime/lockstep.c
    hle/hle.c hle/kernel.c hle/io.c hle/ctrl.c hle/display.c hle/audio.c hle/misc.c hle/gxm.c hle/heap.c hle/libc.c hle/fiber.c hle/ngs.c
    hle/mp4.c hle/videodec.c hle/audiodec.c hle/dialog.c hle/net.c hle/trophy.c
    gpu/present.c gpu/render.c gpu/vk.c "${volk_SOURCE_DIR}/volk.c"
    platform/input.c)
# FFmpeg: the movies' demuxer and decoders (hle/mp4.c, videodec.c, audiodec.c); scarletnx-filesystem: the game
# module's path as Windows takes it (snx_host_wpath, aot/load.c)
target_link_libraries(snx_vita_program PUBLIC snx_vita_loader snx_vita_usse scarletnx-filesystem FFmpeg::avformat FFmpeg::avcodec
                      snx_settings_runtime snx_build_info snx_window_icon SDL3::SDL3
                      Vulkan::Headers ${CMAKE_DL_LIBS})
target_include_directories(snx_vita_program PUBLIC "${volk_SOURCE_DIR}")
# the compute shaders (the texture deswizzle, the decoded pictures' layout, YUV textures, 24-bit color surfaces),
# compiled at build time
# (cmake/glsl.cmake)
include("${PROJECT_SOURCE_DIR}/cmake/glsl.cmake")
foreach(shader swizzle picture yuv rgb24)
    snx_glsl_spirv(snx_vita_program SOURCE gpu/${shader}.comp NAME vita_${shader}_spv HEADER vita/gpu/${shader}_spv.h)
endforeach()
set_source_files_properties("${volk_SOURCE_DIR}/volk.c" PROPERTIES COMPILE_OPTIONS "${snx_third_party_flags}")
include("${PROJECT_SOURCE_DIR}/cmake/llvm.cmake")
if(NOT TARGET snx_arm_llvm)
    add_subdirectory("${PROJECT_SOURCE_DIR}/src/common/cpu/arm" vita-arm-backend)
endif()
target_link_libraries(snx_vita_program PUBLIC snx_arm_llvm)
# the helpers compiled blocks call, which the offline compiler links each game module with
set(vita_runtime_source "${CMAKE_CURRENT_BINARY_DIR}/vita-runtime-$<CONFIG>.c")
add_custom_command(OUTPUT "${vita_runtime_source}"
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=$<TARGET_FILE:snx_arm_rt>" "-DOUTPUT=${vita_runtime_source}"
        -P "${PROJECT_SOURCE_DIR}/cmake/vita-runtime.cmake"
    DEPENDS snx_arm_rt "${PROJECT_SOURCE_DIR}/cmake/vita-runtime.cmake" VERBATIM)
target_sources(snx_vita_program PRIVATE "${vita_runtime_source}")
target_compile_definitions(snx_vita_program PRIVATE "SNX_VITA_HOST_CC=\"${CMAKE_C_COMPILER}\"" _CRT_SECURE_NO_WARNINGS)
# the run-time compiler for the blocks the module lacks: the LLVM backend's executor (exec.c, in snx_llvm)
if(TARGET snx_llvm)
    target_link_libraries(snx_vita_program PUBLIC snx_llvm)
    target_compile_definitions(snx_vita_program PRIVATE SNX_VITA_JIT)
endif()
# The backend checked against Unicorn as the game runs (runtime/lockstep.h), for debugging the AArch32 backend: Unicorn's
# headers and library (its Python package's libunicorn.so.2 will do) where the build is told
option(SNX_VITA_LOCKSTEP "Check the Vita's AArch32 code against Unicorn while it runs (VITA_LOCKSTEP=import call)" OFF)
if(SNX_VITA_LOCKSTEP)
    find_path(SNX_UNICORN_INCLUDE unicorn/unicorn.h REQUIRED)
    find_library(SNX_UNICORN_LIBRARY NAMES unicorn libunicorn.so.2 REQUIRED)
    target_include_directories(snx_vita_program PRIVATE "${SNX_UNICORN_INCLUDE}")
    target_link_libraries(snx_vita_program PUBLIC "${SNX_UNICORN_LIBRARY}")
    target_compile_definitions(snx_vita_program PRIVATE SNX_VITA_LOCKSTEP)
endif()
foreach(target snx_vita_loader snx_vita_usse snx-vita-usse snx_vita_program)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror=missing-prototypes)
endforeach()

# the driver: scarletnx-vita's own program, or where scarletnx is also this driver (SNX_MULTICALL, src/multicall) its
# main in this library, the name scarletnx-vita a link to scarletnx
if(SNX_MULTICALL)
    target_sources(snx_vita_program PRIVATE runtime/main.c)
    target_compile_definitions(snx_vita_program PRIVATE SNX_MULTICALL)
else()
    add_executable(scarletnx-vita runtime/main.c)
    target_link_libraries(scarletnx-vita PRIVATE snx_vita_program)
    set_target_properties(scarletnx-vita PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON
        RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
    target_compile_options(scarletnx-vita PRIVATE -Wall -Wextra -Werror=missing-prototypes)
    if(SNX_DEPLOY AND NOT (SNX_TEST_SANITIZERS AND BUILD_TESTING))
        add_custom_command(TARGET scarletnx-vita POST_BUILD
            COMMAND "${CMAKE_COMMAND}" "-DVITA_RUNTIME=$<TARGET_FILE:scarletnx-vita>" "-DGAMES=${SNX_RECOMPILED_DIR}"
                    "-DWINDOWS=${WIN32}" "-DMACOS=${APPLE}" -P "${PROJECT_SOURCE_DIR}/cmake/deploy.cmake"
            VERBATIM)
    endif()
    include(GNUInstallDirs)
    install(TARGETS scarletnx-vita RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    if(SNX_LLVM_SHARED)
        target_link_options(scarletnx-vita PRIVATE "-Wl,-rpath,${SNX_LLVM_LIBRARY_DIR}")
    endif()
endif()

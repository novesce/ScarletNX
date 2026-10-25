# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The version of the walk over a game's files for the shader programs it ships (src/nx/gpu/shipped_pass.c): its
# sources' hash. The renderer keeps the files the walk found nothing in under it across builds (SHIP_NONE, its
# .vkscan file), so a change to what the walk finds must change it. A change to any of them configures again, and
# snx_ship_walk(source) hands the hash to that source, in the calling folder, as SNX_SHIP_WALK
include_guard(GLOBAL)
set(snx_ship_walk_sources src/nx/gpu/shipped_pass.c src/nx/gpu/shipped_pass.h src/nx/gpu/shader/shipped.c
                          src/nx/gpu/shader/shipped.h)
set(snx_ship_walk "")
foreach(source IN LISTS snx_ship_walk_sources)
    file(SHA256 "${PROJECT_SOURCE_DIR}/${source}" hash)
    string(APPEND snx_ship_walk "${hash}")
    set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${PROJECT_SOURCE_DIR}/${source}")
endforeach()
string(SHA256 snx_ship_walk "${snx_ship_walk}")
string(SUBSTRING "${snx_ship_walk}" 0 16 snx_ship_walk)
function(snx_ship_walk source)
    set_property(SOURCE "${source}" APPEND PROPERTY COMPILE_DEFINITIONS "SNX_SHIP_WALK=\"${snx_ship_walk}\"")
endfunction()

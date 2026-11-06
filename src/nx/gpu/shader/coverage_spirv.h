/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_COVERAGE_SPIRV_H
#define SNX_COVERAGE_SPIRV_H
#include "common/utilities/spirv_emit.h"

/* Undithered alpha-to-coverage: floor(clamp(alpha,0,1)*samples) low sample bits, with NaN mapped to zero.
   samples is 1,2,4,8 or16. guest_mask is an optional u32 value ID (0 means no guest mask). Returns a u32 value ID,
   or0 for an unsupported count without emitting instructions. The caller combines this with raster coverage via
   SampleMask output, disables native alpha-to-coverage and applies alpha-to-one after reading alpha here.
   The API specifies spatially constant masks when dithering is off, but does not prescribe their thresholds. */
uint32_t shader_alpha_coverage_spirv(SpvEmitter* e, uint32_t f32, uint32_t u32_type, uint32_t boolean,
                                    uint32_t alpha, uint32_t samples, uint32_t guest_mask);
/* Resolve one primitive after native tests: set0 binding1 is a float4 validity storage image (red0 or1), binding2
   is the integer coverage destination. SH_BLEND_ARRAY/MS and N<<8 select the common image shape. Dispatch
   ceil(width/8),ceil(height/8),layers. OR all valid sample-index bits, then write that union to each valid sample.
   Clear validity before each primitive, capture its original shader once, and resolve before the next primitive. */
uint32_t* shader_coverage_resolve_spirv(uint32_t flags, int signed_target, size_t* words);
/* Fullscreen triangle, no vertex resources; draw three vertices starting at zero. */
uint32_t* shader_coverage_vertex_spirv(size_t* words);
/* Portable one-color-sample reduction. Set0 bindings0/1 are float4 multisample 2D source/validity storage images.
   The original shader must produce the same source for every surviving sample of a pixel/primitive. Count every
   valid sample, select one surviving source, discard zero coverage, and output once at location0. components is
   an RGBA bit mask; factor16 is an optional samples-entry table of exact coefficients0..16, otherwise use count/N.
   Native final color state performs blending, logic operations, channel masking and attachment conversion. */
uint32_t* shader_coverage_fragment_spirv(uint32_t samples, uint32_t components, const uint8_t* factor16,
                                       size_t* words);
#endif

/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The layout of a GXP program (SceGxmProgram) and its parameters as Vita3K's gxm/include/gxm/types.h describes them
   (GPL-2.0-or-later, Copyright (C) 2026 Vita3K team) at Vita3K/Vita3K f68b6ee: offsets into the program's bytes, which
   are the guest's, read little-endian. An offset field counts from where it is */
#ifndef SNX_VITA_GXP_H
#define SNX_VITA_GXP_H
#include "common/utilities/endian.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum {
    GXP_MAGIC = 0x00505847u, /* "GXP\0" */
    GXP_FLAGS = 20,
    GXP_BUFFER_FLAGS = 24,
    GXP_TEXUNIT_FLAGS = 28,
    GXP_PARAMETER_COUNT = 36,
    GXP_PARAMETERS = 40,
    GXP_VARYINGS = 44,
    GXP_PRIMARY_REGS = 48,   /* u16 */
    GXP_SECONDARY_REGS = 50, /* u16 */
    GXP_TEMP_REGS = 52,
    GXP_PHASE_COUNT = 58, /* u16 */
    GXP_PRIMARY_COUNT = 60,
    GXP_PRIMARY = 64,
    GXP_SECONDARY_COUNT = 68,
    GXP_SECONDARY = 72,
    GXP_SECONDARY_END = 76,
    GXP_DEFAULT_UNIFORM_COUNT = 100,
    GXP_LITERAL_DATA = 104,
    GXP_LITERALS_COUNT = 112,
    GXP_LITERALS = 116,
    GXP_UNIFORM_BUFFER_COUNT = 120,
    GXP_UNIFORM_BUFFERS = 124,
    GXP_DEPENDENT_SAMPLER_COUNT = 128,
    GXP_DEPENDENT_SAMPLERS = 132,
    GXP_CONTAINER_COUNT = 144,
    GXP_CONTAINERS = 148,
};
enum {
    GXP_FLAG_FRAGMENT = 1u << 0,
    GXP_FLAG_DISCARD_USED = 1u << 3,
    GXP_FLAG_DEPTH_USED = 1u << 4,
    GXP_FLAG_SPRITECOORD_USED = 1u << 5,
    GXP_FLAG_NATIVECOLOR_USED = 1u << 6,
    GXP_FLAG_FRAGCOLOR_USED = 1u << 7,
    GXP_FLAG_OUTPUT_UNDEFINED = 1u << 16,
};
/* a parameter's category and type (SceGxmParameterCategory, SceGxmParameterType) */
enum { GXP_CATEGORY_ATTRIBUTE, GXP_CATEGORY_UNIFORM, GXP_CATEGORY_SAMPLER, GXP_CATEGORY_AUXILIARY_SURFACE,
       GXP_CATEGORY_UNIFORM_BUFFER };
enum { GXP_TYPE_F32, GXP_TYPE_F16, GXP_TYPE_C10, GXP_TYPE_U32, GXP_TYPE_S32, GXP_TYPE_U16, GXP_TYPE_S16, GXP_TYPE_U8,
       GXP_TYPE_S8, GXP_TYPE_AGGREGATE };
/* the default uniform buffer's container index, the textures' (SCE_GXM_*_UNIFORM_BUFFER_CONTAINER_INDEX) */
#define GXP_DEFAULT_CONTAINER 14u
#define GXP_TEXTURE_CONTAINER 15u

static inline float gxp_f32(const unsigned char* p) {
    uint32_t bits = snx_ld_le32(p);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}
/* the bytes an offset field at field points to */
static inline const unsigned char* gxp_at(const unsigned char* program, unsigned field) {
    return program + field + snx_ld_le32(program + field);
}
static inline bool gxp_fragment(const unsigned char* program) { return snx_ld_le32(program + GXP_FLAGS) & GXP_FLAG_FRAGMENT; }
/* parameter i (16 bytes): its name, category, type, component count, container, semantic, its index, array size and
   resource index */
static inline const unsigned char* gxp_parameter(const unsigned char* program, uint32_t i) {
    return gxp_at(program, GXP_PARAMETERS) + i * 16;
}
static inline const char* gxp_parameter_name(const unsigned char* parameter) {
    return (const char*)parameter + (int32_t)snx_ld_le32(parameter);
}
static inline unsigned gxp_parameter_category(const unsigned char* q) { return q[4] & 15; }
static inline unsigned gxp_parameter_type(const unsigned char* q) { return q[4] >> 4; }
static inline unsigned gxp_parameter_components(const unsigned char* q) { return q[5] & 15; }
static inline unsigned gxp_parameter_container(const unsigned char* q) { return q[5] >> 4; }
static inline unsigned gxp_parameter_semantic(const unsigned char* q) { return q[6]; }
static inline unsigned gxp_parameter_semantic_index(const unsigned char* q) { return q[7]; }
static inline uint32_t gxp_parameter_array_size(const unsigned char* q) { return snx_ld_le32(q + 8); }
static inline uint32_t gxp_parameter_resource(const unsigned char* q) { return snx_ld_le32(q + 12); }
/* bytes of one component of a parameter type */
static inline unsigned gxp_type_size(unsigned type) {
    switch (type) {
        case GXP_TYPE_F16:
        case GXP_TYPE_U16:
        case GXP_TYPE_S16:
        case GXP_TYPE_C10: return 2;
        case GXP_TYPE_U8:
        case GXP_TYPE_S8: return 1;
        default: return 4;
    }
}
#endif

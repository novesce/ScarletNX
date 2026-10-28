/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_PICA_INTERNAL_H
#define SNX_CTR_PICA_INTERNAL_H
/* helpers the PICA's shader interpreter, rasterizer and SPIR-V generators share */
#include "common/utilities/bits.h"
#include <stdint.h>

/* a float converted to s32 as x86 does it: out-of-range and NaN become INT32_MIN */
static inline int32_t to_s32(float f) {
    if (!(f >= -2147483648.0f && f < 2147483648.0f)) return INT32_MIN;
    return (int32_t)f;
}
#endif

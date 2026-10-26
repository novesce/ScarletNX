/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Half floats (IEEE 754 binary16) to floats and back, bit by bit. The runtime converts on the GPU (texconv.glsl's
   TC_BLIT_FLOAT), so this file isn't built into it */
#include "half.h"
#include <string.h>

float half_to_float(u16 h) {
    u32 sign = (u32)(h & 0x8000) << 16, e = (h >> 10) & 0x1F, m = h & 0x3FF, x;
    float f;
    if (e == 31)
        x = sign | 0x7F800000 | m << 13;
    else if (e)
        x = sign | (e + 112) << 23 | m << 13;
    else if (!m)
        x = sign;
    else { /* a subnormal, normalized */
        for (e = 113; !(m & 0x400); e--) m <<= 1;
        x = sign | e << 23 | (m & 0x3FF) << 13;
    }
    memcpy(&f, &x, 4);
    return f;
}
u16 float_to_half(float f) {
    u32 x, sign, e, m, h, rem, half;
    i32 he;
    memcpy(&x, &f, 4);
    sign = (x >> 16) & 0x8000;
    e = (x >> 23) & 0xFF;
    m = x & 0x7FFFFF;
    if (e == 0xFF) return (u16)(sign | 0x7C00 | (m ? 0x200 | m >> 13 : 0));
    he = (i32)e - 112;
    if (he >= 31) return (u16)(sign | 0x7C00);
    if (he <= 0) { /* a subnormal half, or zero */
        if (he < -10) return (u16)sign;
        m |= 0x800000;
        h = m >> (14 - he);
        rem = m & ((1u << (14 - he)) - 1);
        half = 1u << (13 - he);
    } else {
        h = (u32)he << 10 | m >> 13;
        rem = m & 0x1FFF;
        half = 0x1000;
    }
    if (rem > half || (rem == half && (h & 1))) h++; /* a carry into the exponent rounds up to it, or to infinity */
    return (u16)(sign | h);
}

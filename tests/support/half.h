/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Half floats to floats and back on the CPU (half.c), for the tests only: the reference the 2D blit's conversion
   between float formats (texconv.glsl's TC_BLIT_FLOAT) is held to, through fermi2d_ref.c and on its own */
#ifndef SNX_HALF_H
#define SNX_HALF_H
#include "common/cpu/aarch64/cpu.h"

/* the float a half is, exactly */
float half_to_float(u16 h);
/* the half nearest a float, ties to even: too large for one an infinity, NaNs kept */
u16 float_to_half(float f);

#endif

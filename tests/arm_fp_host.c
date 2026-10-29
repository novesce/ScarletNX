/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* snx_arm_fp's host fast path (fp_helpers.c) against its exact software path: each operation once with FPSCR's
   inexact flag already set, which lets the host compute it, and once without, which never does. The results must be
   the same bits and FPSCR the same but for the flag set at the start, for every mode the fast path may run in (flush
   to zero, default NaN, Advanced SIMD's standard FPSCR) and operands from random bits, near values (cancellation,
   ties) and the special ones (zeros, denormals, the extremes, infinities, NaNs) */
#include "common/cpu/arm/arm.h"
#include "common/cpu/arm/simd.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define IXC 0x10u

static uint64_t g_state = 0x9e3779b97f4a7c15ull;
static uint64_t next(void) { /* splitmix64 */
    uint64_t z = (g_state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
static uint64_t operand(uint32_t width, uint64_t near) {
    static const uint64_t specials32[] = {0, 0x80000000u, 1, 0x807fffffu, 0x00800000u, 0x7f7fffffu, 0xff7fffffu,
                                          0x7f800000u, 0xff800000u, 0x7fc00000u, 0x7f800001u, 0x3f800000u,
                                          0xbf800000u, 0x4f000000u, 0xcf000000u, 0x4f800000u};
    static const uint64_t specials64[] = {0, 0x8000000000000000ull, 1, 0x0010000000000000ull, 0x7fefffffffffffffull,
                                          0x7ff0000000000000ull, 0xfff0000000000000ull, 0x7ff8000000000000ull,
                                          0x7ff0000000000001ull, 0x3ff0000000000000ull, 0x41e0000000000000ull,
                                          0xc1e0000000000000ull, 0x41f0000000000000ull, 0x36a0000000000000ull};
    uint64_t r = next(), mask = width == 32 ? 0xffffffffull : ~0ull;
    switch (r % 8) {
        case 0:
            return width == 32 ? specials32[(r >> 8) % (sizeof specials32 / sizeof *specials32)]
                               : specials64[(r >> 8) % (sizeof specials64 / sizeof *specials64)];
        case 1:
        case 2: return (near ^ (next() & 0xff)) & mask; /* a near value: cancellation, ties */
        case 3: { /* a value of moderate exponent, a sign either way */
            uint64_t m = next();
            return width == 32 ? (m & 0x807fffffu) | (uint64_t)(0x60 + (m >> 40) % 0x40) << 23
                               : (m & 0x800fffffffffffffull) | (uint64_t)(0x3c0 + (m >> 52) % 0x80) << 52;
        }
        case 4: return (next() % 200000) - 100000; /* small integers' bits (denormals) or as integers */
        default: return next() & mask;
    }
}
int main(void) {
    static const uint32_t ops[] = {AV_FADD, AV_FSUB, AV_FMUL, AV_FDIV, AV_FMLA, AV_FMLS, AV_FNMLA, AV_FNMLS,
                                   AV_FNMUL, AV_FCMP, AV_FCMPE, AV_ITOF, AV_FTOI, AV_FCVT};
    static const uint32_t modes[] = {0, 1u << 24, 1u << 25, 3u << 24}; /* FZ and DN off and on, RMode nearest */
    unsigned long checked = 0, failed = 0;
    for (unsigned o = 0; o < sizeof ops / sizeof *ops; ++o)
        for (uint32_t width = 32; width <= 64; width += 32)
            for (unsigned m = 0; m < 4; ++m)
                for (unsigned neon = 0; neon < 2; ++neon)
                    for (unsigned n = 0; n < 40000; ++n) {
                        uint32_t op = ops[o], param = neon ? 1u << 30 : 0;
                        if (op == AV_ITOF || op == AV_FTOI) param |= 32u << 8 | (n & 1 ? 1u << 28 : 0);
                        if (op == AV_FTOI) param |= 3u << 24; /* toward zero */
                        if (op == AV_FCVT) param |= width == 32 ? 64 : 32;
                        uint64_t x = operand(width, 0), y = operand(width, x), add = operand(width, y);
                        SnxArmCPU soft, fast;
                        memset(&soft, 0, sizeof soft);
                        soft.fpscr = modes[m];
                        fast = soft;
                        fast.fpscr |= IXC;
                        uint64_t want = snx_arm_fp(&soft, x, y, add, op, width, param);
                        uint64_t got = snx_arm_fp(&fast, x, y, add, op, width, param);
                        ++checked;
                        if (got != want || fast.fpscr != (soft.fpscr | IXC)) {
                            if (++failed <= 20)
                                printf("op %u width %u mode 0x%08x param 0x%08x: %016" PRIx64 " %016" PRIx64
                                       " %016" PRIx64 " gives %016" PRIx64 " fpscr %08x, want %016" PRIx64
                                       " fpscr %08x\n",
                                       op, width, modes[m], param, x, y, add, got, fast.fpscr, want,
                                       soft.fpscr | IXC);
                        }
                    }
    printf("%lu checked, %lu differ\n", checked, failed);
    return failed != 0;
}

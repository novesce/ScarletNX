/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/cpu/recompiler/variants.h"

int snx_variant_hash(SnxVariantWordFn word, void* user, const SnxVariantRun* runs, size_t count, SnxU64* h1,
                     SnxU64* h2) {
    SnxU64 a = SNX_VARIANT_H1_BASIS, b = SNX_VARIANT_H2_BASIS;
    if (!h1 || !h2) return -1;
    *h1 = 0;
    *h2 = 0;
    if (!word || (!runs && count)) return -1;
    for (size_t i = 0; i < count; ++i) {
        SnxU64 address = runs[i].address;
        SnxU32 n = runs[i].words;
        for (SnxU32 k = 0; k < n; ++k, address += 4) {
            SnxU32 w;
            if (word(user, address, &w)) return -1;
            snx_variant_mix(&a, &b, address, w);
        }
    }
    *h1 = a;
    *h2 = b;
    return 0;
}

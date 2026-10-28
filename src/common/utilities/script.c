/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/script.h"
#include <stdio.h>
#include <stdlib.h>

size_t snx_script_parse(const char* program, const char* text, uint32_t buttons, SnxPress** presses) {
    size_t most = 1, count = 0;
    const char* c = text;
    *presses = NULL;
    if (!text || !*text) return 0;
    for (const char* p = text; *p; ++p) most += *p == ',';
    if (!(*presses = (SnxPress*)calloc(most, sizeof **presses))) {
        fprintf(stderr, "%s: out of memory for the script's %zu presses\n", program, most);
        return 0;
    }
    while (*c && count < most) {
        const char* press = c;
        char* end;
        unsigned long at = strtoul(c, &end, 10);
        if (end == c || *end != ':') {
            c = press;
            break;
        }
        const char* hex = end + 1;
        unsigned long mask = strtoul(hex, &end, 16);
        if (end == hex || (*end && *end != ',') || at > UINT32_MAX || (mask & ~(unsigned long)buttons)) {
            c = press;
            break;
        }
        (*presses)[count++] = (SnxPress){(uint32_t)at, (uint32_t)mask};
        c = *end ? end + 1 : end;
    }
    if (*c)
        fprintf(stderr, "%s: the script stops after %zu presses, at \"%s\": not ms:hex with buttons in 0x%X\n", program,
                count, c, (unsigned)buttons);
    return count;
}

uint32_t snx_script_held(const SnxPress* presses, size_t count, double ms, uint32_t length) {
    uint32_t held = 0;
    for (size_t i = 0; i < count; ++i)
        if (ms >= presses[i].at && ms - presses[i].at < length) held |= presses[i].mask;
    return held;
}

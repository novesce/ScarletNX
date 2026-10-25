/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the game module's backends share: the dispatch entries, unit labels and progress lines */
#include "nx/loader/module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int entry_order(const void* a, const void* b) {
    const SnxModuleEntry *x = (const SnxModuleEntry*)a, *y = (const SnxModuleEntry*)b;
    if (x->address != y->address) return x->address < y->address ? -1 : 1;
    return x->function < y->function ? -1 : x->function > y->function;
}

SnxModuleEntry* snx_module_entries(const SnxCodegen* codegen, size_t* count) {
    SnxModuleEntry* entries;
    size_t total = 0, n = 0, kept, i, j;
    for (i = 0; i < codegen->function_count; ++i) total += codegen->functions[i].leader_count;
    *count = 0;
    if (!total) return NULL;
    if (total > SIZE_MAX / sizeof(*entries)) return NULL;
    entries = (SnxModuleEntry*)snx_alloc(total * sizeof(*entries));
    for (i = 0; i < codegen->function_count; ++i) {
        const SnxFunction* f = &codegen->functions[i];
        for (j = 0; j < f->count && n < total; ++j)
            if (f->leaders[j]) {
                entries[n].address = snx_function_address(f, j);
                entries[n].function = i;
                ++n;
            }
    }
    /* by address, then by function, so the first of a run of equal addresses is the lowest function */
    qsort(entries, n, sizeof(*entries), entry_order);
    for (i = 0, kept = 0; i < n; ++i)
        if (!kept || entries[kept - 1].address != entries[i].address) entries[kept++] = entries[i];
    *count = kept;
    return entries;
}


char* snx_chunk_label(const SnxCodegen* codegen, size_t first, size_t last) {
    SnxU64 lo = ~(SnxU64)0, hi = 0;
    size_t instructions = 0, functions = 0, i;
    for (i = first; i < last && i < codegen->function_count; ++i) {
        const SnxFunction* f = &codegen->functions[i];
        SnxU64 end;
        ++functions;
        instructions += f->count;
        if (!f->count) continue;
        end = snx_function_address(f, f->count - 1) + 4;
        if (f->start < lo) lo = f->start;
        if (end > hi) hi = end;
    }
    if (!functions) return snx_strdup("no functions");
    if (lo > hi) lo = hi = 0;
    return snx_format("0x%llX-0x%llX, %zu function%s, %zu instructions", (unsigned long long)lo,
                      (unsigned long long)hi, functions, functions == 1 ? "" : "s", instructions);
}

void snx_unit_line(char* out, size_t size, size_t done, size_t count, const char* name, const char* label, int cached,
                   double seconds, SnxU64 used) {
    if (cached)
        snprintf(out, size, "  compiled block %zu/%zu: %s, %s (cached)", done, count, name, label);
    else if (used)
        snprintf(out, size, "  compiled block %zu/%zu: %s, %s (%.1f s, %llu MiB in use)", done, count, name, label,
                 seconds, (unsigned long long)(used >> 20));
    else
        snprintf(out, size, "  compiled block %zu/%zu: %s, %s (%.1f s)", done, count, name, label, seconds);
}
void snx_report_unit(size_t done, size_t count, const char* name, const char* label, int cached, double seconds) {
    char line[512];
    snx_unit_line(line, sizeof line, done, count, name, label, cached, seconds, cached ? 0 : snx_footprint());
    puts(line);
    fflush(stdout);
}

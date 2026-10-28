/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The part of the PE/COFF format that link_coff.c (the DLL linker) and code_loader_coff.c (the JIT loader's COFF
   objects) read x86-64 objects with, read byte by byte so both build anywhere */
#ifndef SNX_RECOMPILER_LLVM_COFF_DEFS_H
#define SNX_RECOMPILER_LLVM_COFF_DEFS_H
#include "common/utilities/endian.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SCN_CODE          0x00000020u
#define SCN_INITIALIZED   0x00000040u
#define SCN_UNINITIALIZED 0x00000080u
#define SCN_LNK_INFO      0x00000200u
#define SCN_LNK_REMOVE    0x00000800u
#define SCN_LNK_COMDAT    0x00001000u
#define SCN_ALIGN_MASK    0x00F00000u
#define SCN_NRELOC_OVFL   0x01000000u
#define SCN_DISCARDABLE   0x02000000u
#define SCN_EXECUTE       0x20000000u
#define SCN_READ          0x40000000u
#define SCN_WRITE         0x80000000u
#define SYM_EXTERNAL      2
#define SYM_STATIC        3
#define SYM_WEAK_EXTERNAL 105
#define SEL_ASSOCIATIVE   5
#define REL_ABSOLUTE      0
#define REL_ADDR64        1
#define REL_ADDR32        2
#define REL_ADDR32NB      3
#define REL_REL32         4
#define REL_REL32_5       9
#define REL_SECTION       10
#define REL_SECREL        11

/* an 18-byte symbol record's name into out: its 8 bytes inline, or (first word 0) the string table's at the offset in
   the second word, empty when it is outside a table of strings_size bytes */
static inline void snx_coff_symbol_name(const char* strings, uint32_t strings_size, const unsigned char* s, char* out,
                                        size_t size) {
    size_t n = 0;
    if (!snx_ld_le32(s)) {
        uint32_t offset = snx_ld_le32(s + 4);
        snprintf(out, size, "%s", offset < strings_size ? strings + offset : "");
        return;
    }
    while (n < 8 && s[n]) ++n;
    if (n >= size) n = size - 1;
    memcpy(out, s, n);
    out[n] = 0;
}

/* qsort's order of .pdata entries (RUNTIME_FUNCTION: begin, end, unwind data) by their begin address, the bytes read
   as the little-endian word they are: RtlLookupFunctionEntry binary-searches a table in that order */
static inline int snx_coff_compare_function(const void* a, const void* b) {
    uint32_t x = snx_ld_le32(a), y = snx_ld_le32(b);
    return x < y ? -1 : x > y;
}

#endif

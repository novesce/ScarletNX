/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Layout reference: Azahar src/core/hle/romfs.cpp, Copyright 2017 Citra
   Emulator Project, GPL-2.0-or-later (used under GPLv3). New bounded extractor. */
#include "3ds/loader/internal.h"
#include <stdlib.h>
#include <string.h>
#define NONE UINT32_MAX

typedef struct {
    CtrReader* r;
    CtrSpan data;
    bool write; /* the files are copied out, not only checked */
    unsigned char *dirs, *files, *dir_marks, *file_marks;
    size_t dir_size, file_size, dir_count, file_count, visited_dirs, visited_files;
} RomFs;

static bool utf8_name(const unsigned char* p, size_t bytes, char name[1024]) {
    if (!bytes || bytes > 510 || (bytes & 1)) return false;
    size_t used = 0;
    for (size_t i = 0; i < bytes; i += 2) {
        uint32_t cp = snx_ld_le16(p + i);
        if (cp >= 0xd800 && cp <= 0xdbff) {
            if (i + 2 >= bytes) return false;
            uint32_t lo = snx_ld_le16(p + i + 2);
            if (lo < 0xdc00 || lo > 0xdfff) return false;
            cp = 0x10000 + ((cp - 0xd800) << 10) + lo - 0xdc00;
            i += 2;
        } else if (cp >= 0xdc00 && cp <= 0xdfff)
            return false;
        if (!cp) return false;
        if (cp < 0x80)
            name[used++] = (char)cp;
        else if (cp < 0x800) {
            name[used++] = (char)(0xc0 | (cp >> 6));
            name[used++] = (char)(0x80 | (cp & 63));
        } else if (cp < 0x10000) {
            name[used++] = (char)(0xe0 | (cp >> 12));
            name[used++] = (char)(0x80 | ((cp >> 6) & 63));
            name[used++] = (char)(0x80 | (cp & 63));
        } else {
            name[used++] = (char)(0xf0 | (cp >> 18));
            name[used++] = (char)(0x80 | ((cp >> 12) & 63));
            name[used++] = (char)(0x80 | ((cp >> 6) & 63));
            name[used++] = (char)(0x80 | (cp & 63));
        }
    }
    name[used] = 0;
    return ctr_name(name);
}
static bool table_scan(RomFs* fs, bool directory) {
    unsigned char* bytes = directory ? fs->dirs : fs->files;
    unsigned char* marks = directory ? fs->dir_marks : fs->file_marks;
    size_t size = directory ? fs->dir_size : fs->file_size, base = directory ? 24 : 32, count = 0;
    for (size_t at = 0; at < size;) {
        if (!ctr_fits(size, at, base))
            return ctr_fail(fs->r, SNX_CTR_EXTRACT_FORMAT, "truncated RomFS metadata record");
        size_t length = snx_ld_le32(bytes + at + base - 4);
        if ((length & 1) || length > 510 || !ctr_fits(size, at + base, (length + 3) & ~(size_t)3))
            return ctr_fail(fs->r, SNX_CTR_EXTRACT_FORMAT, "invalid RomFS metadata name length");
        marks[at / 4] = 1;
        at += base + ((length + 3) & ~(size_t)3);
        count++;
    }
    if (directory)
        fs->dir_count = count;
    else
        fs->file_count = count;
    return true;
}
static unsigned char* entry(RomFs* fs, uint32_t offset, bool directory) {
    size_t size = directory ? fs->dir_size : fs->file_size;
    unsigned char* marks = directory ? fs->dir_marks : fs->file_marks;
    if (offset == NONE || (offset & 3) || offset >= size || marks[offset / 4] != 1) {
        ctr_fail(fs->r, SNX_CTR_EXTRACT_FORMAT, "invalid, repeated or cyclic RomFS metadata link");
        return NULL;
    }
    marks[offset / 4] = 2;
    if (directory)
        fs->visited_dirs++;
    else
        fs->visited_files++;
    return (directory ? fs->dirs : fs->files) + offset;
}
static bool unique(RomFs* fs, char*** names, size_t* count, const char* name) {
    for (size_t i = 0; i < *count; ++i)
        if (!strcmp((*names)[i], name))
            return ctr_fail(fs->r, SNX_CTR_EXTRACT_FORMAT, "duplicate RomFS name: %s", name);
    char** grown = realloc(*names, (*count + 1) * sizeof **names);
    if (!grown) return ctr_fail(fs->r, SNX_CTR_EXTRACT_LIMIT, "out of memory");
    *names = grown;
    grown[*count] = snx_strdup(name);
    ++*count;
    return true;
}
static bool walk(RomFs* fs, uint32_t offset, uint32_t parent, const char* path, unsigned depth, uint32_t* next) {
    if (depth > 64 || strlen(path) > 3500)
        return ctr_fail(fs->r, SNX_CTR_EXTRACT_LIMIT, "RomFS exceeds depth/path limit");
    unsigned char* dir = entry(fs, offset, true);
    if (!dir) return false;
    uint32_t parent_offset = snx_ld_le32(dir), child = snx_ld_le32(dir + 8), file = snx_ld_le32(dir + 12);
    if ((offset && parent_offset != parent) || (!offset && parent_offset != 0 && parent_offset != NONE))
        return ctr_fail(fs->r, SNX_CTR_EXTRACT_FORMAT, "RomFS parent link mismatch");
    *next = snx_ld_le32(dir + 4);
    if (!offset && (snx_ld_le32(dir + 20) || *next != NONE))
        return ctr_fail(fs->r, SNX_CTR_EXTRACT_FORMAT, "invalid RomFS root");
    /* a check that writes nothing refuses two entries of a folder by one name, as creating the second would */
    char** names = NULL;
    size_t count = 0;
    bool ok = true;
    while (ok && file != NONE) {
        unsigned char* f = entry(fs, file, false);
        char name[1024];
        if (!f) {
            ok = false;
            break;
        }
        if (snx_ld_le32(f) != offset || !utf8_name(f + 32, snx_ld_le32(f + 28), name)) {
            ok = ctr_fail(fs->r, SNX_CTR_EXTRACT_FORMAT, "invalid RomFS file parent/name");
            break;
        }
        uint64_t at = snx_ld_le64(f + 8), size = snx_ld_le64(f + 16);
        if (!ctr_fits(fs->data.size, at, size)) {
            ok = ctr_fail(fs->r, SNX_CTR_EXTRACT_FORMAT, "RomFS file data exceeds section");
            break;
        }
        char* destination = snx_path_join(path, name);
        ok = fs->write ? ctr_copy(fs->r, (CtrSpan){fs->data.offset + at, size}, destination, NULL)
                       : unique(fs, &names, &count, name);
        free(destination);
        file = snx_ld_le32(f + 4);
    }
    while (ok && child != NONE) {
        if ((child & 3) || child >= fs->dir_size || fs->dir_marks[child / 4] != 1) {
            ok = ctr_fail(fs->r, SNX_CTR_EXTRACT_FORMAT, "invalid or cyclic RomFS directory link");
            break;
        }
        unsigned char* c = fs->dirs + child;
        char name[1024];
        if (!utf8_name(c + 24, snx_ld_le32(c + 20), name)) {
            ok = ctr_fail(fs->r, SNX_CTR_EXTRACT_FORMAT, "unsafe RomFS directory name");
            break;
        }
        char* destination = snx_path_join(path, name);
        ok = (fs->write ? ctr_directory(fs->r, destination) : unique(fs, &names, &count, name)) &&
             walk(fs, child, offset, destination, depth + 1, &child);
        free(destination);
    }
    for (size_t i = 0; i < count; ++i) free(names[i]);
    free(names);
    return ok;
}
static bool extract(RomFs* fs, CtrSpan rom, const char* stage, CtrSpan* found) {
    CtrReader* r = fs->r;
    unsigned char ivfc[0x60], header[0x28];
    if (!ctr_read(r, rom, 0, ivfc, sizeof ivfc)) return false;
    if (memcmp(ivfc, "IVFC", 4) || snx_ld_le32(ivfc + 4) != 0x10000)
        return ctr_fail(r, SNX_CTR_EXTRACT_UNSUPPORTED, "unsupported RomFS IVFC format");
    uint32_t master = snx_ld_le32(ivfc + 8), shift = snx_ld_le32(ivfc + 0x4c);
    if (!master || (master & 31) || master > CTR_METADATA_LIMIT || shift < 9 || shift > 20)
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "invalid RomFS IVFC geometry");
    uint64_t block = 1ull << shift;
    uint64_t start = (0x60 + (uint64_t)master + block - 1) & ~(block - 1);
    uint64_t size = snx_ld_le64(ivfc + 0x44);
    if (!size || !ctr_fits(rom.size, start, size))
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "RomFS level 3 exceeds section");
    CtrSpan level3 = {rom.offset + start, size};
    if (!ctr_read(r, level3, 0, header, sizeof header)) return false;
    if (snx_ld_le32(header) != sizeof header)
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "invalid RomFS level 3 header");
    uint32_t data = snx_ld_le32(header + 0x24);
    if (data < sizeof header || data > level3.size)
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "invalid RomFS data offset");
    CtrSpan tables[4];
    for (size_t i = 0; i < 4; ++i) {
        tables[i] = (CtrSpan){snx_ld_le32(header + 4 + i * 8), snx_ld_le32(header + 8 + i * 8)};
        if ((tables[i].offset & 3) || (tables[i].size & 3) || (tables[i].size && tables[i].offset < sizeof header) ||
            !ctr_fits(data, tables[i].offset, tables[i].size))
            return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "invalid RomFS metadata table range");
        for (size_t j = 0; tables[i].size && j < i; ++j)
            if (tables[j].size && tables[i].offset < tables[j].offset + tables[j].size &&
                tables[j].offset < tables[i].offset + tables[i].size)
                return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "overlapping RomFS metadata tables");
    }
    fs->dir_size = (size_t)tables[1].size;
    fs->file_size = (size_t)tables[3].size;
    if (fs->dir_size < 24 || fs->dir_size > CTR_METADATA_LIMIT || fs->file_size > CTR_METADATA_LIMIT)
        return ctr_fail(r, SNX_CTR_EXTRACT_LIMIT, "RomFS metadata exceeds limit or has no root");
    fs->dirs = malloc(fs->dir_size);
    fs->files = malloc(fs->file_size ? fs->file_size : 1);
    fs->dir_marks = calloc(fs->dir_size / 4, 1);
    fs->file_marks = calloc(fs->file_size / 4 + 1, 1);
    if (!fs->dirs || !fs->files || !fs->dir_marks || !fs->file_marks)
        return ctr_fail(r, SNX_CTR_EXTRACT_LIMIT, "out of memory");
    if (!ctr_read(r, level3, tables[1].offset, fs->dirs, fs->dir_size) ||
        !ctr_read(r, level3, tables[3].offset, fs->files, fs->file_size) || !table_scan(fs, true) ||
        !table_scan(fs, false))
        return false;
    fs->data = (CtrSpan){level3.offset + data, level3.size - data};
    char* path = stage ? snx_path_join(stage, "romfs") : snx_strdup("romfs");
    uint32_t next;
    bool ok = (!stage || ctr_directory(r, path)) && walk(fs, 0, 0, path, 0, &next);
    free(path);
    if (ok && (fs->visited_dirs != fs->dir_count || fs->visited_files != fs->file_count))
        ok = ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "unreachable RomFS metadata entries");
    *found = level3;
    return ok;
}
bool ctr_romfs_check(CtrReader* r, CtrSpan rom, const char* stage, CtrSpan* level3) {
    RomFs fs = {.r = r, .write = stage != NULL};
    bool ok = extract(&fs, rom, stage, level3);
    free(fs.dirs);
    free(fs.files);
    free(fs.dir_marks);
    free(fs.file_marks);
    return ok;
}

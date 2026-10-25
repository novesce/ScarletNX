/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A game read where it lies: the file itself mapped read-only and shared for as long as it is open, checked once as the
   extraction checks it, its ExeFS files and RomFS level 3 handed out as views into the mapping. Nothing is written */
#include "3ds/loader/internal.h"
#include "common/filesystem/host.h"
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <sys/stat.h>
#endif

struct SnxCtrSource {
    int fd;
    const unsigned char* map;
    uint64_t size, mtime;
    uint64_t base; /* where the game lies in the mapped file: a bundle's entry (ctr_tar_game), else 0, and the
                      spans the parsed source holds are relative to it */
    CtrSource source;
};
/* the host's modification time of path, UTC Unix milliseconds, as fs_tree_mtime reads it; 0 when it can't say */
static uint64_t modified(const char* path) {
#if defined(_WIN32)
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &attributes)) return 0;
    ULARGE_INTEGER time = {.LowPart = attributes.ftLastWriteTime.dwLowDateTime,
                           .HighPart = attributes.ftLastWriteTime.dwHighDateTime};
    return time.QuadPart < 116444736000000000ull ? 0 : (time.QuadPart - 116444736000000000ull) / 10000;
#else
    struct stat st;
    if (stat(path, &st) || st.st_mtime < 0) return 0;
#if defined(__APPLE__)
    return (uint64_t)st.st_mtime * 1000 + (uint64_t)st.st_mtimespec.tv_nsec / 1000000;
#else
    return (uint64_t)st.st_mtime * 1000 + (uint64_t)st.st_mtim.tv_nsec / 1000000;
#endif
#endif
}
void snx_ctr_source_close(SnxCtrSource* c) {
    if (!c) return;
    ctr_source_free(&c->source);
    plat_file_unshare(c->fd, c->map, c->size);
    free(c);
}
SnxCtrExtractResult snx_ctr_source_open(const char* input, SnxCtrSource** out, char* error, size_t error_size) {
    /* read where it lies every launch: no whole-data hashes (CtrReader.quick) */
    CtrReader r = {.error = error, .error_size = error_size, .quick = true};
    SnxCtrSource* c = calloc(1, sizeof *c);
    *out = NULL;
    if (error_size) error[0] = 0;
    if (!c) {
        ctr_fail(&r, SNX_CTR_EXTRACT_LIMIT, "out of memory");
        return r.result;
    }
    c->fd = -1;
    if (!input || !snx_is_file(input)) {
        ctr_fail(&r, SNX_CTR_EXTRACT_IO, "cannot open the game");
        goto done;
    }
    c->size = snx_file_size(input);
    /* an empty file holds no game: the host cannot map one, and the extraction refuses it as malformed data, not
       as a host failure, so a read where it lies says the same */
    if (!c->size) {
        ctr_fail(&r, SNX_CTR_EXTRACT_FORMAT, "the game is empty");
        goto done;
    }
    /* the largest game the loader reads: a CIA keeps the extraction's input limit, and a cartridge image is only
       mapped and read, so it may be as large as a card is */
    if (c->size > CTR_CARD_LIMIT) {
        ctr_fail(&r, SNX_CTR_EXTRACT_LIMIT, "the game exceeds the 8 GiB input limit");
        goto done;
    }
    const uint8_t* view = NULL;
    if (plat_file_share(input, c->size, &c->fd, &view) || !view) {
        ctr_fail(&r, SNX_CTR_EXTRACT_IO, "cannot map the game");
        goto done;
    }
    c->map = view;
    c->mtime = modified(input);
    r.map = c->map;
    r.size = c->size;
    /* every check as the extraction makes it, over the mapping: the NCCHs' headers and superblocks' own hashes, and
       a CIA's each content's; nothing of the whole data is hashed, as a launch costs no seconds on it */
    if (!ctr_source_load(&r, &c->source)) goto done;
    /* a CIA's contents' hashes, which the extraction checks as it copies them; a cartridge image and a bare NCCH
       have no content table, and their own headers' and superblocks' hashes are already checked */
    if (c->source.kind == CTR_KIND_CIA && !ctr_cia_contents(&r, &c->source, NULL)) goto done;
    c->base = r.base; /* the container may be a bundle's entry: the views below read from it */
done:
    if (r.result == SNX_CTR_EXTRACT_OK)
        *out = c;
    else
        snx_ctr_source_close(c);
    return r.result;
}
uint64_t snx_ctr_source_title(const SnxCtrSource* c) { return c->source.title_id; }
uint64_t snx_ctr_source_mtime(const SnxCtrSource* c) { return c->mtime; }
/* the game's NCCH: a cartridge's update when it carries one, as the console runs that one's ExHeader and code */
static const CtrNcch* game_ncch(const SnxCtrSource* c) {
    return c->source.overlay ? &c->source.over : &c->source.base;
}
const unsigned char* snx_ctr_source_exheader(const SnxCtrSource* c) { return game_ncch(c)->exheader; }
const unsigned char* snx_ctr_source_code(const SnxCtrSource* c, size_t* size) {
    const CtrNcch* n = game_ncch(c);
    *size = n->code_size;
    return n->code;
}
const unsigned char* snx_ctr_source_exefs(const SnxCtrSource* c, const char* name, uint64_t* size) {
    const CtrNcch* n = game_ncch(c);
    /* an ExeFS file's bytes are in the mapping, at its absolute offset in the file */
    for (size_t i = 0; i < 10; ++i)
        if (*n->names[i] && !strcmp(n->names[i], name)) {
            *size = n->files[i].size;
            return c->map + c->base + n->files[i].offset;
        }
    *size = 0;
    return NULL;
}
const unsigned char* snx_ctr_source_romfs(const SnxCtrSource* c, uint64_t* size) {
    const CtrNcch* n = game_ncch(c);
    *size = n->level3.size;
    return *size ? c->map + c->base + n->level3.offset : NULL;
}

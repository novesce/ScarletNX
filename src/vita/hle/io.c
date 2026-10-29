/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SceIofilemgr and SceLibKernel's sceIo*: the game's files. app0: is the application's folder as the VPK holds it (the
   game folder's app/, with the update's files in patch/ over it), read-only on the console and here; ux0: the memory card, the game folder's ux0/, which the game
   writes its data to (ux0:data/...); savedata0: the game's save data, mounted for it as the console mounts it
   (VitaProcess.savedata, in ux0/). A path leaving its device ("..") is refused */
#if !defined(_WIN32) && !defined(_FILE_OFFSET_BITS)
#define _FILE_OFFSET_BITS 64
#endif
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE /* fseeko */
#endif
#include "vita/hle/hle.h"
#include "vita/runtime/watch.h"
#include "common/utilities/endian.h"
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define SCE_O_RDONLY 0x0001u
#define SCE_O_WRONLY 0x0002u
#define SCE_O_APPEND 0x0100u
#define SCE_O_CREAT  0x0200u
#define SCE_O_TRUNC  0x0400u
#define SCE_O_EXCL   0x0800u
#define ENOENT 2u
#define EBADF  9u
#define EACCES 13u
#define EEXIST 17u
#define ENOTDIR 20u
#define EISDIR 21u
#define EINVAL 22u
#define EMFILE 24u

typedef struct {
    char** names;
    size_t count, next;
    char* path; /* the folder's host path, for each entry's stat */
} Directory;

/* guest path to host path, the caller's to free; NULL for a device or path the runtime doesn't give, *writable for ux0: */
static char* host_path(VitaProcess* p, uint32_t address, bool* writable) {
    char path[1024];
    if (!vita_string(p, address, path, sizeof path)) return NULL;
    char* colon = strchr(path, ':');
    if (!colon) return NULL;
    *colon = 0;
    const char* root;
    bool app = false;
    *writable = false;
    if (!strcmp(path, "app0"))
        root = p->app, app = true;
    else if (!strcmp(path, "ux0") || !strcmp(path, "uma0")) {
        root = p->ux0;
        *writable = true;
    } else if (!strcmp(path, "savedata0")) {
        root = p->savedata;
        *writable = true;
    } else
        return NULL;
    const char* rest = colon + 1;
    while (*rest == '/') ++rest;
    /* each part relative and not ".." */
    for (const char* q = rest; *q;) {
        const char* slash = strchr(q, '/');
        size_t n = slash ? (size_t)(slash - q) : strlen(q);
        if (n == 2 && q[0] == '.' && q[1] == '.') return NULL;
        if (!slash) break;
        q = slash + 1;
    }
    if (!*rest) return snx_strdup(root);
    /* the update's file or folder, when it has the path */
    char* patched = app ? snx_path_join(p->patch, rest) : NULL;
    if (patched && snx_path_exists(patched)) return patched;
    free(patched);
    return snx_path_join(root, rest);
}
char* vita_io_host_path(VitaProcess* p, uint32_t address) {
    bool writable;
    return host_path(p, address, &writable);
}
static int file_index(VitaProcess* p, uint32_t fd, bool directory) {
    if (fd < 1 || fd > VITA_FILES || !p->files[fd - 1].host || p->files[fd - 1].directory != directory) return -1;
    return (int)fd - 1;
}
static uint32_t new_file(VitaProcess* p, void* host, int directory) {
    for (unsigned i = 0; i < VITA_FILES; ++i)
        if (!p->files[i].host) {
            p->files[i] = (VitaFile){host, directory};
            return i + 1;
        }
    return 0;
}
static int io_seek(FILE* f, int64_t offset, int whence) {
#ifdef _WIN32
    return _fseeki64(f, offset, whence);
#else
    return fseeko(f, (off_t)offset, whence);
#endif
}
static int64_t io_tell(FILE* f) {
#ifdef _WIN32
    return _ftelli64(f);
#else
    return (int64_t)ftello(f);
#endif
}
static void close_directory(Directory* d) {
    snx_free_strings(d->names, d->count);
    free(d->path);
    free(d);
}

uint32_t vita_io_open(VitaProcess* p, uint32_t address, uint32_t flags) {
    bool writable;
    char* path = host_path(p, address, &writable);
    if (!path) return SCE_ERRNO(ENOENT);
    bool write = (flags & SCE_O_WRONLY) || (flags & (SCE_O_CREAT | SCE_O_TRUNC | SCE_O_APPEND));
    uint32_t result;
    if (snx_is_dir(path))
        result = SCE_ERRNO(EISDIR);
    else if (write && !writable)
        result = SCE_ERRNO(EACCES);
    else if ((flags & SCE_O_EXCL) && (flags & SCE_O_CREAT) && snx_path_exists(path))
        result = SCE_ERRNO(EEXIST);
    else if (!write && !snx_is_file(path))
        result = SCE_ERRNO(ENOENT);
    else {
        bool exists = snx_is_file(path);
        if (write && !exists && !(flags & SCE_O_CREAT)) {
            free(path);
            return SCE_ERRNO(ENOENT);
        }
        const char* mode = !write                  ? "rb"
                           : (flags & SCE_O_TRUNC) || !exists ? ((flags & SCE_O_RDONLY) ? "w+b" : "wb")
                           : (flags & SCE_O_APPEND) ? "ab"
                                                    : "r+b";
        FILE* f = snx_fopen(path, mode);
        result = !f ? SCE_ERRNO(ENOENT) : new_file(p, f, 0);
        if (f && !result) {
            fclose(f);
            result = SCE_ERRNO(EMFILE);
        }
    }
    free(path);
    return result;
}
static uint32_t io_open(VitaProcess* p, VitaThread* t) { return vita_io_open(p, vita_arg(p, t, 0), vita_arg(p, t, 1)); }
FILE* vita_io_file(VitaProcess* p, uint32_t fd) {
    int i = file_index(p, fd, false);
    return i < 0 ? NULL : p->files[i].host;
}
uint32_t vita_io_close(VitaProcess* p, uint32_t fd) {
    int i = file_index(p, fd, false);
    if (i < 0) return SCE_ERRNO(EBADF);
    fclose(p->files[i].host);
    p->files[i] = (VitaFile){0};
    return 0;
}
static uint32_t io_close(VitaProcess* p, VitaThread* t) { return vita_io_close(p, vita_arg(p, t, 0)); }
static uint32_t io_read(VitaProcess* p, VitaThread* t) {
    int i = file_index(p, vita_arg(p, t, 0), false);
    uint32_t buffer = vita_arg(p, t, 1), size = vita_arg(p, t, 2);
    if (i < 0) return SCE_ERRNO(EBADF);
    if (!size) return 0;
    if (!vita_mapped(p, buffer, size)) return SCE_ERRNO(EINVAL);
    vita_watch_release(buffer, size); /* the system writes it: a watched page would refuse it (EFAULT) */
    return (uint32_t)fread(vita_ptr(p, buffer), 1, size, p->files[i].host);
}
static uint32_t io_write(VitaProcess* p, VitaThread* t) {
    uint32_t fd = vita_arg(p, t, 0), buffer = vita_arg(p, t, 1), size = vita_arg(p, t, 2);
    if (!size) return 0;
    if (!vita_mapped(p, buffer, size)) return SCE_ERRNO(EINVAL);
    /* the newlib's stdout and stderr (descriptors the runtime never opens): the game's log */
    int i = file_index(p, fd, false);
    if (i < 0) {
        if (fd > VITA_FILES) {
            fwrite(vita_ptr(p, buffer), 1, size, stdout);
            fflush(stdout);
            return size;
        }
        return SCE_ERRNO(EBADF);
    }
    return (uint32_t)fwrite(vita_ptr(p, buffer), 1, size, p->files[i].host);
}
/* sceIoPread(fd, buffer, size, SceOff offset): the offset in the stack's first two words; the file's position kept */
static uint32_t io_pread(VitaProcess* p, VitaThread* t) {
    int i = file_index(p, vita_arg(p, t, 0), false);
    uint32_t buffer = vita_arg(p, t, 1), size = vita_arg(p, t, 2);
    int64_t offset = (int64_t)((uint64_t)vita_arg(p, t, 4) | (uint64_t)vita_arg(p, t, 5) << 32);
    if (i < 0) return SCE_ERRNO(EBADF);
    if (!size) return 0;
    if (!vita_mapped(p, buffer, size)) return SCE_ERRNO(EINVAL);
    FILE* f = p->files[i].host;
    int64_t was = io_tell(f);
    if (io_seek(f, offset, SEEK_SET)) return SCE_ERRNO(EINVAL);
    vita_watch_release(buffer, size);
    uint32_t got = (uint32_t)fread(vita_ptr(p, buffer), 1, size, f);
    (void)io_seek(f, was, SEEK_SET);
    return got;
}
static uint32_t io_sync_by_fd(VitaProcess* p, VitaThread* t) {
    int i = file_index(p, vita_arg(p, t, 0), false);
    if (i < 0) return SCE_ERRNO(EBADF);
    fflush(p->files[i].host);
    return 0;
}
/* sceIoLseek(fd, SceOff offset, whence): the offset in r2 and r3, whence on the stack */
static uint32_t io_lseek(VitaProcess* p, VitaThread* t) {
    int i = file_index(p, vita_arg(p, t, 0), false);
    int64_t offset = (int64_t)((uint64_t)vita_arg(p, t, 2) | (uint64_t)vita_arg(p, t, 3) << 32);
    int whence = (int)vita_arg(p, t, 4);
    if (i < 0) return vita_return64(t, (uint64_t)(int64_t)(int32_t)SCE_ERRNO(EBADF));
    if (whence < 0 || whence > 2 || io_seek(p->files[i].host, offset, whence))
        return vita_return64(t, (uint64_t)(int64_t)(int32_t)SCE_ERRNO(EINVAL));
    return vita_return64(t, (uint64_t)io_tell(p->files[i].host));
}
static uint32_t io_lseek32(VitaProcess* p, VitaThread* t) {
    int i = file_index(p, vita_arg(p, t, 0), false);
    int whence = (int)vita_arg(p, t, 2);
    if (i < 0) return SCE_ERRNO(EBADF);
    if (whence < 0 || whence > 2 || io_seek(p->files[i].host, (int32_t)vita_arg(p, t, 1), whence))
        return SCE_ERRNO(EINVAL);
    return (uint32_t)io_tell(p->files[i].host);
}
static uint32_t io_remove(VitaProcess* p, VitaThread* t) {
    bool writable;
    char* path = host_path(p, vita_arg(p, t, 0), &writable);
    uint32_t result = !path ? SCE_ERRNO(ENOENT) : !writable ? SCE_ERRNO(EACCES) : remove(path) ? SCE_ERRNO(ENOENT) : 0;
    free(path);
    return result;
}
static uint32_t io_mkdir(VitaProcess* p, VitaThread* t) {
    bool writable;
    char* path = host_path(p, vita_arg(p, t, 0), &writable);
    uint32_t result = !path       ? SCE_ERRNO(ENOENT)
                      : !writable ? SCE_ERRNO(EACCES)
                      : snx_path_exists(path) ? SCE_ERRNO(EEXIST)
                      : snx_mkdirs(path)      ? SCE_ERRNO(ENOENT)
                                              : 0;
    free(path);
    return result;
}
static uint32_t io_rmdir(VitaProcess* p, VitaThread* t) {
    bool writable;
    char* path = host_path(p, vita_arg(p, t, 0), &writable);
    uint32_t result = !path ? SCE_ERRNO(ENOENT)
                      : !writable ? SCE_ERRNO(EACCES)
                      : !snx_is_dir(path) ? SCE_ERRNO(ENOTDIR)
                      : snx_remove_empty_dir(path) ? SCE_ERRNO(EEXIST)
                                                   : 0;
    free(path);
    return result;
}
static uint32_t io_rename(VitaProcess* p, VitaThread* t) {
    bool from_writable, to_writable;
    char *from = host_path(p, vita_arg(p, t, 0), &from_writable), *to = host_path(p, vita_arg(p, t, 1), &to_writable);
    uint32_t result = !from || !to ? SCE_ERRNO(ENOENT)
                      : !from_writable || !to_writable ? SCE_ERRNO(EACCES)
                      : snx_replace(from, to) ? SCE_ERRNO(ENOENT)
                                              : 0;
    free(from);
    free(to);
    return result;
}
/* SceDateTime: year, month, day, hour, minute, second (16 bits each), microseconds */
static void date_time(VitaProcess* p, uint32_t at, time_t when) {
    struct tm tm;
#ifdef _WIN32
    gmtime_s(&tm, &when);
#else
    gmtime_r(&when, &tm);
#endif
    uint16_t fields[6] = {(uint16_t)(tm.tm_year + 1900), (uint16_t)(tm.tm_mon + 1), (uint16_t)tm.tm_mday,
                          (uint16_t)tm.tm_hour,          (uint16_t)tm.tm_min,       (uint16_t)tm.tm_sec};
    unsigned char* b = vita_ptr(p, at);
    for (unsigned i = 0; i < 6; ++i) snx_st_le16(b + i * 2, fields[i]);
    vita_write32(p, at + 12, 0);
}
/* SceIoStat (88 bytes): st_mode, st_attr, st_size (64-bit, at 8), the creation, access and modification times */
static bool stat_into(VitaProcess* p, const char* path, uint32_t out) {
    struct stat s;
    if (stat(path, &s)) return false;
    bool directory = S_ISDIR(s.st_mode);
    memset(vita_ptr(p, out), 0, 88);
    vita_write32(p, out, directory ? 0x1000u | 0x1ffu : 0x2000u | 0x1b6u); /* SCE_S_IFDIR or SCE_S_IFREG, and rw */
    vita_write32(p, out + 4, directory ? 0x10u | 7u : 0x20u | 6u);          /* SCE_SO_IFDIR or SCE_SO_IFREG */
    vita_write64(p, out + 8, directory ? 0 : (uint64_t)s.st_size);
    date_time(p, out + 16, s.st_ctime);
    date_time(p, out + 32, s.st_atime);
    date_time(p, out + 48, s.st_mtime);
    return true;
}
static uint32_t io_getstat(VitaProcess* p, VitaThread* t) {
    bool writable;
    char* path = host_path(p, vita_arg(p, t, 0), &writable);
    uint32_t result = path && stat_into(p, path, vita_arg(p, t, 1)) ? 0 : SCE_ERRNO(ENOENT);
    free(path);
    return result;
}
static uint32_t io_getstat_by_fd(VitaProcess* p, VitaThread* t) {
    int i = file_index(p, vita_arg(p, t, 0), false);
    if (i < 0) return SCE_ERRNO(EBADF);
    FILE* f = p->files[i].host;
    int64_t at = io_tell(f), size = io_seek(f, 0, SEEK_END) ? 0 : io_tell(f);
    io_seek(f, at, SEEK_SET);
    uint32_t out = vita_arg(p, t, 1);
    memset(vita_ptr(p, out), 0, 88);
    vita_write32(p, out, 0x2000u | 0x1b6u);
    vita_write32(p, out + 4, 0x20u | 6u);
    vita_write64(p, out + 8, (uint64_t)size);
    time_t now = time(NULL);
    date_time(p, out + 16, now);
    date_time(p, out + 32, now);
    date_time(p, out + 48, now);
    return 0;
}
static uint32_t io_dopen(VitaProcess* p, VitaThread* t) {
    bool writable;
    char* path = host_path(p, vita_arg(p, t, 0), &writable);
    if (!path || !snx_is_dir(path)) {
        free(path);
        return SCE_ERRNO(ENOENT);
    }
    Directory* d = calloc(1, sizeof *d);
    if (!d || snx_list_dir(path, &d->names, &d->count)) {
        free(d);
        free(path);
        return SCE_ERRNO(ENOENT);
    }
    d->path = path;
    uint32_t fd = new_file(p, d, 1);
    if (!fd) {
        close_directory(d);
        return SCE_ERRNO(EMFILE);
    }
    return fd;
}
/* SceIoDirent (352 bytes): its SceIoStat, then d_name[256]. 1 for an entry, 0 past the last */
static uint32_t io_dread(VitaProcess* p, VitaThread* t) {
    int i = file_index(p, vita_arg(p, t, 0), true);
    uint32_t out = vita_arg(p, t, 1);
    if (i < 0) return SCE_ERRNO(EBADF);
    Directory* d = p->files[i].host;
    while (d->next < d->count) {
        const char* name = d->names[d->next++];
        char* path = snx_path_join(d->path, name);
        bool ok = path && stat_into(p, path, out);
        free(path);
        if (!ok) continue;
        memset(vita_ptr(p, out + 88), 0, 264);
        snprintf((char*)vita_ptr(p, out + 88), 256, "%s", name);
        return 1;
    }
    return 0;
}
static uint32_t io_dclose(VitaProcess* p, VitaThread* t) {
    int i = file_index(p, vita_arg(p, t, 0), true);
    if (i < 0) return SCE_ERRNO(EBADF);
    close_directory(p->files[i].host);
    p->files[i] = (VitaFile){0};
    return 0;
}
void vita_io_close_all(VitaProcess* p) {
    for (unsigned i = 0; i < VITA_FILES; ++i) {
        if (!p->files[i].host) continue;
        if (p->files[i].directory)
            close_directory(p->files[i].host);
        else
            fclose(p->files[i].host);
        p->files[i] = (VitaFile){0};
    }
}

const VitaHle vita_hle_io[] = {
    {0x6c60ac61u, "sceIoOpen", io_open},
    {0xc70b8886u, "sceIoClose", io_close},
    {0xfdb32293u, "sceIoRead", io_read},
    {0x52315ad7u, "sceIoPread", io_pread},
    {0x16512f59u, "sceIoSyncByFd", io_sync_by_fd},
    {0x34efd876u, "sceIoWrite", io_write},
    {0x99ba173eu, "sceIoLseek", io_lseek},
    {0x49252b9bu, "sceIoLseek32", io_lseek32},
    {0xe20ed0f3u, "sceIoRemove", io_remove},
    {0x9670d39fu, "sceIoMkdir", io_mkdir},
    {0xe9f91ec8u, "sceIoRmdir", io_rmdir},
    {0xf737e369u, "sceIoRename", io_rename},
    {0xbca5b623u, "sceIoGetstat", io_getstat},
    {0x57f8cd25u, "sceIoGetstatByFd", io_getstat_by_fd},
    {0xa9283dd0u, "sceIoDopen", io_dopen},
    {0x9c8b6624u, "sceIoDread", io_dread},
    {0x422a221au, "sceIoDclose", io_dclose},
    {0, NULL, NULL},
};

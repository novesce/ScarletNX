/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/cheat_list.h"
#include "common/utilities/ascii.h"
#include "common/utilities/bits.h"
#include <stdlib.h>
#include <string.h>

char* snx_cheats_folder(const char* data) { return snx_path_join(data, "cheats"); }
int snx_cheat_name(const char* line, char* name, size_t size) {
    while (snx_is_space(*line)) ++line;
    if (*line != '[' && *line != '{') return 0;
    const char* close = strchr(line + 1, *line == '[' ? ']' : '}');
    size_t n = close ? (size_t)(close - line - 1) : strlen(line + 1);
    while (n && (line[n] == '\r' || line[n] == '\n')) --n; /* an unclosed name to its line's end */
    if (n >= size) n = size - 1;
    memcpy(name, line + 1, n);
    name[n] = 0;
    return 1;
}
static int listed(char** names, size_t n, const char* name) {
    for (size_t i = 0; i < n; ++i)
        if (!strcmp(names[i], name)) return 1;
    return 0;
}
size_t snx_cheats_list(const char* folder, char*** names) {
    char** files = NULL;
    size_t count = 0, n = 0, room = 0;
    *names = NULL;
    if (snx_list_dir(folder, &files, &count)) return 0;
    qsort(files, count, sizeof *files, snx_compare_strings);
    for (size_t i = 0; i < count; ++i) {
        size_t len = strlen(files[i]), size;
        char *path = snx_path_join(folder, files[i]), *text = NULL, *next;
        if (len > 4 && !strcmp(files[i] + len - 4, ".txt") && files[i][0] != '.')
            text = (char*)snx_read_file(path, &size);
        for (char* line = text; line && *line; line = next) {
            char name[64];
            next = strchr(line, '\n');
            if (next) *next++ = 0;
            if (!snx_cheat_name(line, name, sizeof name) || listed(*names, n, name)) continue;
            if (n == room) {
                room = room ? 2 * room : 8;
                *names = (char**)snx_realloc(*names, room * sizeof **names);
            }
            (*names)[n++] = snx_strdup(name);
        }
        free(text);
        free(path);
    }
    snx_free_strings(files, count);
    return n;
}
int snx_cheat_off(const char* off, const char* name) {
    size_t n = strlen(name);
    const char* at = off;
    while (at && *at) {
        const char* end = strchr(at, SNX_CHEATS_SEPARATOR);
        size_t length = end ? (size_t)(end - at) : strlen(at);
        if (length == n && !strncmp(at, name, n)) return 1;
        at = end ? end + 1 : NULL;
    }
    return 0;
}

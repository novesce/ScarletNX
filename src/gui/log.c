/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gui/log.h"
#include "common/utilities/common.h"
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the folder, which the menu's thread sets and the dumps' icons' thread reads: under a spin lock, a copy at a time */
static struct {
    atomic_flag lock;
    char folder[2048];
} g_gui_log = {ATOMIC_FLAG_INIT, ""};

static void lock(void) {
    while (atomic_flag_test_and_set_explicit(&g_gui_log.lock, memory_order_acquire)) {}
}
static void unlock(void) { atomic_flag_clear_explicit(&g_gui_log.lock, memory_order_release); }
void gui_log_folder(const char* folder) {
    lock();
    snprintf(g_gui_log.folder, sizeof g_gui_log.folder, "%s", folder ? folder : "");
    unlock();
}
void gui_log_path(const char* file, char* out, size_t size) {
    lock();
    if (g_gui_log.folder[0]) {
        char* path = snx_path_join(g_gui_log.folder, file);
        snprintf(out, size, "%s", path);
        free(path);
    } else if (size)
        out[0] = 0;
    unlock();
}
/* text in the log named file of the folder, when there is one */
static void log_to(const char* file, const char* text) {
    char path[sizeof g_gui_log.folder + 64];
    gui_log_path(file, path, sizeof path);
    if (path[0] && snx_log_lines(path, text)) {
        char why[256];
        fprintf(stderr, "scarletnx: %s could not be written (%s): %s\n", path, snx_os_error(why, sizeof why), text);
    }
}
void gui_log(const char* format, ...) {
    va_list args, again;
    char* text;
    int n;
    va_start(args, format);
    va_copy(again, args);
    n = vsnprintf(NULL, 0, format, args);
    text = (char*)snx_alloc(n > 0 ? (size_t)n + 1 : 1);
    vsnprintf(text, n > 0 ? (size_t)n + 1 : 1, format, again);
    va_end(again);
    va_end(args);
#ifndef SNX_IOS /* there stderr is the log's file */
    fprintf(stderr, "scarletnx: %s\n", text);
#endif
    log_to(SNX_CONSOLE_LOG, text);
    free(text);
}
void gui_log_fatal(const char* text) {
    gui_log("%s", text);
    log_to(SNX_ERROR_LOG, text);
}

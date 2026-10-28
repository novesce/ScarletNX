/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Vulkan driver an Apple app carries (MoltenVK): found beside a program or in its bundle, and put beside the runtime
   copy a game runs, which is outside the bundle where SDL finds neither the app's Frameworks nor a system loader
   (docs/APPLE.md) */
#define _DARWIN_C_SOURCE /* chflags and UF_IMMUTABLE, which hide behind it in a strict C99 compile */
#include "common/utilities/common.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void put(const char* path, const char* text) { assert(!snx_write_file(path, text, strlen(text))); }
static void holds(const char* path, const char* text) {
    size_t size = 0;
    unsigned char* data = snx_read_file(path, &size);
    assert(data && size == strlen(text) && !memcmp(data, text, size));
    free(data);
}
/* the file a search found, held against the one expected: the same file, however the path names it */
static void found(char* path, const char* expected) {
    char* a = path ? snx_absolute(path) : NULL;
    char* b = a ? snx_absolute(expected) : NULL;
    assert(a && b && !strcmp(a, b));
    free(a);
    free(b);
    free(path);
}
int main(void) {
    char* dir = snx_temp_dir(NULL);
    char *app, *app_macos, *app_frameworks, *app_program, *app_driver;
    char *phone, *phone_frameworks, *phone_program, *phone_driver;
    char *game_dir, *game, *game_driver, *plain_dir, *plain;
    struct stat app_st, game_st;
    assert(dir);
    app = snx_path_join(dir, "ScarletNX.app");
    app_macos = snx_path_join(app, "Contents/MacOS");
    app_frameworks = snx_path_join(app, "Contents/Frameworks");
    app_program = snx_path_join(app_macos, "scarletnx");
    app_driver = snx_path_join(app_frameworks, "libMoltenVK.dylib");
    phone = snx_path_join(dir, "scarletnx.app");
    phone_frameworks = snx_path_join(phone, "Frameworks/MoltenVK.framework");
    phone_program = snx_path_join(phone, "scarletnx");
    phone_driver = snx_path_join(phone_frameworks, "MoltenVK");
    game_dir = snx_path_join(dir, "recompiled/0100000000000000");
    game = snx_path_join(game_dir, "0100000000000000");
    game_driver = snx_path_join(game_dir, "libMoltenVK.dylib");
    plain_dir = snx_path_join(dir, "build");
    plain = snx_path_join(plain_dir, "scarletnx");
    assert(!snx_mkdirs(app_macos) && !snx_mkdirs(app_frameworks) && !snx_mkdirs(phone_frameworks));
    assert(!snx_mkdirs(game_dir) && !snx_mkdirs(plain_dir));
    put(app_program, "the app's runtime");
    put(app_driver, "MoltenVK, the app's");
    put(phone_program, "the phone app's runtime");
    put(phone_driver, "MoltenVK, the phone app's");
    put(plain, "a checkout's runtime");
    /* the app's bundle, and the framework an iOS app holds in the bundle itself; a checkout's build has neither */
    found(snx_vulkan_driver(app_program), app_driver);
    found(snx_vulkan_driver(phone_program), phone_driver);
    assert(!snx_vulkan_driver(plain));
    /* the game's folder gets the app's, and its own copy of the runtime finds it first */
    assert(!snx_deploy_vulkan_driver(app_program, game_dir));
    holds(game_driver, "MoltenVK, the app's");
    found(snx_vulkan_driver(game), game_driver);
    /* a newer app's replaces the game's, whichever name the older one left it under */
    put(app_driver, "MoltenVK, the app's, newer");
    assert(!snx_deploy_vulkan_driver(app_program, game_dir));
    holds(game_driver, "MoltenVK, the app's, newer");
    /* a program the disk takes no second name from (an app on a read-only image, or one macOS translocated) leaves the
       copy: the deploy fails the run without it */
    assert(!snx_remove_tree(game_driver));
    assert(!chflags(app_driver, UF_IMMUTABLE));
    assert(snx_link_file(app_driver, game_driver)); /* the disk refuses this name */
    assert(!snx_deploy_vulkan_driver(app_program, game_dir));
    holds(game_driver, "MoltenVK, the app's, newer");
    assert(!stat(app_driver, &app_st) && !stat(game_driver, &game_st) && app_st.st_ino != game_st.st_ino);
    assert(!chflags(app_driver, 0));
    /* nothing to put beside a game from a program that carries none, and the game's stays */
    assert(!snx_deploy_vulkan_driver(plain, game_dir));
    holds(game_driver, "MoltenVK, the app's, newer");
    /* a game's folder that lost the library gets it back, as the refresh of a runtime deploys it */
    assert(!snx_remove_tree(game_driver));
    assert(!snx_vulkan_driver(game));
    assert(!snx_deploy_vulkan_driver(app_program, game_dir));
    holds(game_driver, "MoltenVK, the app's, newer");
    assert(!snx_remove_tree(dir));
    free(dir);
    free(app);
    free(app_macos);
    free(app_frameworks);
    free(app_program);
    free(app_driver);
    free(phone);
    free(phone_frameworks);
    free(phone_program);
    free(phone_driver);
    free(game_dir);
    free(game);
    free(game_driver);
    free(plain_dir);
    free(plain);
    puts("vulkan_driver: the app's MoltenVK, found in its bundle and put beside a game's runtime");
    return 0;
}

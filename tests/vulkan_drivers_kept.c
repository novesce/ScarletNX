/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Preloaded into the sanitized 3DS tests that present through Vulkan (tests/CMakeLists.txt): the Vulkan loader unloads
   the driver, which still holds an allocation in its own static data, and LeakSanitizer then reports it from a library
   no longer mapped, where no suppression can name it. Here a Vulkan driver (libvulkan_*.so) stays loaded, so the
   allocation stays reachable; every other library unloads as before, and every leak of ours still shows */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <string.h>

int dlclose(void* handle) {
    static int (*next)(void*);
    struct link_map* map;
    if (!dlinfo(handle, RTLD_DI_LINKMAP, &map) && map->l_name) {
        const char* name = strrchr(map->l_name, '/');
        if (!strncmp(name ? name + 1 : map->l_name, "libvulkan_", 10)) return 0;
    }
    if (!next) {
        void* sym = dlsym(RTLD_NEXT, "dlclose");
        memcpy(&next, &sym, sizeof next); /* a function pointer isn't a void*'s object to store through */
    }
    return next(handle);
}

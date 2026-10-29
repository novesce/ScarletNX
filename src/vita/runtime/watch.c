/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The pages' states and generations, one each a host page of the guest's space. The fault handler runs on any thread
   (the game's, the Vulkan driver's when it is the CPU), so a page's state changes and its protection under one spin
   lock, which the handler can take: the main thread holds it only around mprotect calls */
#include "vita/runtime/watch.h"
#include "vita/runtime/process.h"
#include <stdlib.h>
#ifndef _WIN32
#include <sys/mman.h>
#include <unistd.h>
#endif

enum { UNWATCHED, PROTECTED, WRITTEN };

typedef struct {
    unsigned char* base;
    uint64_t page, pages;
    unsigned char* state;
    uint64_t* generation;
    uint64_t now;
    int lock;
} Watch;
static Watch g_watch;

static void lock(void) {
    while (__atomic_exchange_n(&g_watch.lock, 1, __ATOMIC_ACQUIRE)) continue;
}
static void unlock(void) { __atomic_store_n(&g_watch.lock, 0, __ATOMIC_RELEASE); }
/* the pages [first, last] of a guest range */
static bool span(uint32_t address, uint32_t size, uint64_t* first, uint64_t* last) {
    if (!g_watch.state || !size) return false;
    *first = address / g_watch.page, *last = ((uint64_t)address + size - 1) / g_watch.page;
    return *last < g_watch.pages;
}
static uint64_t next_generation(void) { return __atomic_add_fetch(&g_watch.now, 1, __ATOMIC_ACQ_REL); }

bool vita_watch_init(VitaProcess* p) {
#ifdef _WIN32
    (void)p;
    return false;
#else
    vita_watch_free();
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) return false;
    g_watch.page = (uint64_t)page, g_watch.pages = VITA_SPACE / (uint64_t)page;
    g_watch.state = calloc(g_watch.pages, 1);
    g_watch.generation = calloc(g_watch.pages, sizeof *g_watch.generation);
    if (!g_watch.state || !g_watch.generation) {
        vita_watch_free();
        return false;
    }
    g_watch.base = p->mem;
    return true;
#endif
}
void vita_watch_free(void) {
    free(g_watch.state);
    free(g_watch.generation);
    g_watch = (Watch){0};
}
uint64_t vita_watch_protect(uint32_t address, uint32_t size) {
    uint64_t first, last;
    if (!span(address, size, &first, &last)) return 0;
#ifndef _WIN32
    lock();
    /* runs of pages not protected yet, each one call */
    for (uint64_t i = first; i <= last;) {
        if (g_watch.state[i] == PROTECTED) {
            ++i;
            continue;
        }
        uint64_t end = i;
        while (end <= last && g_watch.state[end] != PROTECTED) g_watch.state[end++] = PROTECTED;
        if (mprotect(g_watch.base + i * g_watch.page, (size_t)((end - i) * g_watch.page), PROT_READ))
            for (uint64_t k = i; k < end; ++k) g_watch.state[k] = UNWATCHED; /* not memory that can be protected */
        i = end;
    }
    unlock();
#endif
    return __atomic_load_n(&g_watch.now, __ATOMIC_ACQUIRE);
}
bool vita_watch_changed(uint32_t address, uint32_t size, uint64_t since) {
    uint64_t first, last;
    if (!span(address, size, &first, &last)) return true;
    for (uint64_t i = first; i <= last; ++i)
        if (__atomic_load_n(&g_watch.state[i], __ATOMIC_ACQUIRE) != PROTECTED ||
            __atomic_load_n(&g_watch.generation[i], __ATOMIC_ACQUIRE) > since)
            return true;
    return false;
}
void vita_watch_written(uint32_t address, uint32_t size) {
    uint64_t first, last;
    if (!span(address, size, &first, &last)) return;
    uint64_t now = next_generation();
    for (uint64_t i = first; i <= last; ++i) __atomic_store_n(&g_watch.generation[i], now, __ATOMIC_RELEASE);
}
void vita_watch_release(uint32_t address, uint32_t size) {
    uint64_t first, last;
    if (!span(address, size, &first, &last)) return;
#ifndef _WIN32
    lock();
    uint64_t now = next_generation();
    for (uint64_t i = first; i <= last;) {
        g_watch.generation[i] = now;
        if (g_watch.state[i] != PROTECTED) {
            g_watch.state[i++] = UNWATCHED;
            continue;
        }
        uint64_t end = i;
        while (end <= last && g_watch.state[end] == PROTECTED)
            g_watch.generation[end] = now, g_watch.state[end++] = UNWATCHED;
        mprotect(g_watch.base + i * g_watch.page, (size_t)((end - i) * g_watch.page), PROT_READ | PROT_WRITE);
        i = end;
    }
    unlock();
#endif
}
bool vita_watch_fault(uintptr_t host) {
#ifdef _WIN32
    (void)host;
    return false;
#else
    if (!g_watch.state || host < (uintptr_t)g_watch.base || host >= (uintptr_t)g_watch.base + VITA_SPACE) return false;
    uint64_t i = (host - (uintptr_t)g_watch.base) / g_watch.page;
    lock();
    /* a page another thread's fault opened meanwhile (both wrote it at once) is the same write: it goes on too */
    bool watched = g_watch.state[i] == PROTECTED || g_watch.state[i] == WRITTEN;
    if (g_watch.state[i] == PROTECTED) {
        __atomic_store_n(&g_watch.generation[i], next_generation(), __ATOMIC_RELEASE);
        g_watch.state[i] = WRITTEN;
        mprotect(g_watch.base + i * g_watch.page, (size_t)g_watch.page, PROT_READ | PROT_WRITE);
    }
    unlock();
    return watched;
#endif
}

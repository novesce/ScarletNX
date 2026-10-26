/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game's threads on one host thread, as the 3DS driver runs its (3ds/aot/dispatch.c): the scheduler gives the core
   to a ready thread, which runs block after block until it waits or its slice ends. The Vita gives a game three cores,
   so a thread spinning on another is common: each slice goes to the ready thread that has waited longest, whatever
   the priorities, which keeps every thread running as three cores would rather than starving the lower ones */
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE /* sigaltstack */
#endif
#include "vita/runtime/process.h"
#include "vita/runtime/jit.h"
#include "vita/runtime/lockstep.h"
#include "vita/hle/hle.h"
#include "common/utilities/common.h"
#include "snx_settings.h"
#include <inttypes.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <synchapi.h>
#else
#include <signal.h>
#include <time.h>
#endif

/* blocks between the scheduler's looks: a few milliseconds of a game's code */
#define SLICE_BLOCKS 20000u
#define VBLANK_NS    16683333u /* 59.94 Hz */
#define CACHE_SLOTS  65536u

typedef struct {
    uint32_t key;
    SnxArmBlock block;
} Cached;
static Cached cache[CACHE_SLOTS];

/* the block at pc: the module's, else one compiled now */
static SnxArmBlock block_at(VitaProcess* p, uint32_t pc, bool thumb) {
    uint32_t key = pc | thumb;
    Cached* c = &cache[(key >> 1) & (CACHE_SLOTS - 1)];
    if (c->key == key && c->block) return c->block;
    SnxArmBlock block = vita_module_block(p->module, key);
    if (!block && p->jit) block = vita_jit_block(p->jit, pc, thumb);
    if (block) *c = (Cached){key, block};
    return block;
}

/* ---- host faults in the guest's memory: the guest's data abort, which ends the run ---- */
#ifndef _WIN32
static sigjmp_buf fault_jump;
static volatile sig_atomic_t fault_armed;
static unsigned char* fault_base;
static volatile uintptr_t fault_address;
static void on_fault(int signal, siginfo_t* info, void* context) {
    (void)context;
    uintptr_t at = (uintptr_t)info->si_addr;
    if (fault_armed && fault_base && at >= (uintptr_t)fault_base && at < (uintptr_t)fault_base + VITA_SPACE + 0x10000) {
        fault_address = at - (uintptr_t)fault_base;
        fault_armed = 0;
        siglongjmp(fault_jump, 1);
    }
    /* the runtime's own: the default action */
    struct sigaction def;
    memset(&def, 0, sizeof def);
    def.sa_handler = SIG_DFL;
    sigaction(signal, &def, NULL);
}
static void faults_install(unsigned char* base) {
    static unsigned char alternate[1 << 16];
    stack_t ss = {.ss_sp = alternate, .ss_size = sizeof alternate};
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    fault_base = base;
}
#endif

static const char* wait_name(uint32_t kind) {
    static const char* const names[] = {"a delay", "a semaphore", "a mutex", "a thread's end", "an event flag",
                                        "a vblank", "audio output", "the display queue", "the GPU", "a condition",
                                        "a reader-writer lock"};
    return kind < sizeof names / sizeof *names ? names[kind] : "?";
}
static void report_threads(VitaProcess* p) {
    for (unsigned i = 0; i < VITA_THREADS; ++i) {
        const VitaThread* t = &p->threads[i];
        if (t->state == VITA_THREAD_FREE || t->state == VITA_THREAD_DEAD) continue;
        printf("  thread 0x%08" PRIx32 " \"%s\": pc 0x%08" PRIx32 "%s lr 0x%08" PRIx32 " sp 0x%08" PRIx32, t->uid,
               t->name, t->cpu.r[15], t->cpu.cpsr & SNX_ARM_T ? " (Thumb)" : "", t->cpu.r[14], t->cpu.r[13]);
        if (t->state == VITA_THREAD_WAITING)
            printf(", waits on %s 0x%08" PRIx32, wait_name(t->wait.kind), t->wait.object);
        else if (t->state == VITA_THREAD_DORMANT)
            printf(", not started");
        putchar('\n');
    }
}
/* the blocks entered last, oldest first from g_history_at: where a bad branch came from */
#define HISTORY 160u
static uint32_t g_history[HISTORY];
static unsigned g_history_at;
/* The words on t's stack that are return addresses into the image's code (just past a BL or BLX), nearest first */
static void report_stack(VitaProcess* p, const VitaThread* t) {
    unsigned shown = 0;
    printf("  stack:");
    for (uint32_t at = t->cpu.r[13] & ~3u; shown < 12 && at - (t->cpu.r[13] & ~3u) < 0x1000; at += 4) {
        if (!vita_mapped(p, at, 4)) break;
        uint32_t w = vita_read32(p, at), ret = w & ~1u;
        const VitaSegment* s = vita_image_segment(&p->image, ret - 4, 4);
        if (!s || !(s->flags & 1)) continue;
        bool call;
        if (w & 1) { /* Thumb: BL or BLX's second halfword, or BLX register */
            uint32_t h = vita_read16(p, ret - 2);
            call = (h & 0xd000) == 0xd000 || (h & 0xd000) == 0xc000 || (h & 0xff87) == 0x4780;
        } else {
            uint32_t i = vita_read32(p, ret - 4);
            call = (i & 0x0f000000) == 0x0b000000 || (i & 0xfe000000) == 0xfa000000 || (i & 0x0ffffff0) == 0x012fff30;
        }
        if (!call) continue;
        printf(" 0x%08" PRIx32, w);
        ++shown;
    }
    putchar('\n');
}
static void report_fault(VitaProcess* p, VitaThread* t, const char* what, uint32_t detail) {
    if (!t) {
        printf("*** Crash: %s 0x%08" PRIx32 " outside the game's threads\n", what, detail);
        return;
    }
    const SnxArmCPU* c = &t->cpu;
    printf("*** Crash: %s 0x%08" PRIx32 " in thread \"%s\" at pc 0x%08" PRIx32 "%s\n", what, detail, t->name, c->r[15],
           c->cpsr & SNX_ARM_T ? " (Thumb)" : "");
    printf("  blocks before:");
    for (unsigned i = 0; i < HISTORY; ++i)
        if (g_history[(g_history_at + i) % HISTORY]) printf(" %08" PRIx32, g_history[(g_history_at + i) % HISTORY]);
    putchar('\n');
    for (unsigned i = 0; i < 16; i += 4)
        printf("  r%-2u 0x%08" PRIx32 "  r%-2u 0x%08" PRIx32 "  r%-2u 0x%08" PRIx32 "  r%-2u 0x%08" PRIx32 "\n", i,
               c->r[i], i + 1, c->r[i + 1], i + 2, c->r[i + 2], i + 3, c->r[i + 3]);
    printf("  cpsr 0x%08" PRIx32 " fpscr 0x%08" PRIx32 "\n", c->cpsr, c->fpscr);
    report_stack(p, t);
}

/* whether t's wait is met (its result set) or timed out, now */
static bool wait_try(VitaProcess* p, VitaThread* t, uint64_t now) {
    bool met;
    switch (t->wait.kind) {
        case VITA_WAIT_DELAY:
        case VITA_WAIT_AUDIO: /* a set: the result is the one the call made before waiting */
            met = now >= t->wait.deadline;
            if (met && !t->wait.a) t->cpu.r[0] = 0;
            return met;
        case VITA_WAIT_DISPLAY:
        case VITA_WAIT_GXM: met = vita_gxm_wait_try(p, t, now); break;
        default: met = vita_kernel_wait_try(p, t, now); break;
    }
    if (met) return true;
    if (t->wait.deadline && now >= t->wait.deadline) {
        t->cpu.r[0] = SCE_KERNEL_ERROR_WAIT_TIMEOUT;
        if (t->wait.timeout) vita_write32(p, t->wait.timeout, 0);
        return true;
    }
    return false;
}
/* the thread to run next, or NULL when none can now: *wake is then when one may (UINT64_MAX: never by time) */
static VitaThread* schedule(VitaProcess* p, uint64_t now, uint64_t* wake) {
    VitaThread* best = NULL;
    *wake = UINT64_MAX;
    for (unsigned i = 0; i < VITA_THREADS; ++i) {
        VitaThread* t = &p->threads[i];
        if (t->state == VITA_THREAD_WAITING && wait_try(p, t, now)) {
            if (t->wait.timeout && t->cpu.r[0] != SCE_KERNEL_ERROR_WAIT_TIMEOUT) {
                uint64_t left = t->wait.deadline > now ? (t->wait.deadline - now) / 1000u : 0;
                vita_write32(p, t->wait.timeout, (uint32_t)left);
            }
            t->state = VITA_THREAD_READY;
        }
        if (t->state == VITA_THREAD_WAITING && t->wait.deadline && t->wait.deadline < *wake) *wake = t->wait.deadline;
        if (t->state == VITA_THREAD_READY && (!best || t->seq < best->seq)) best = t;
    }
    return best;
}
static void sleep_until(uint64_t when) {
    uint64_t now = vita_now_ns();
    if (when <= now) return;
#ifdef _WIN32
    Sleep((DWORD)((when - now) / 1000000u));
#else
    struct timespec ts = {(time_t)((when - now) / 1000000000u), (long)((when - now) % 1000000000u)};
    nanosleep(&ts, NULL);
#endif
}
static bool live(const VitaProcess* p) {
    for (unsigned i = 0; i < VITA_THREADS; ++i)
        if (p->threads[i].state == VITA_THREAD_READY || p->threads[i].state == VITA_THREAD_WAITING) return true;
    return false;
}
/* an SVC of the current thread: an import, or a trampoline's. False when the run cannot go on */
static bool svc(VitaProcess* p, VitaThread* t, uint32_t imm) {
    if (imm < p->image.import_count) {
        vita_hle_call(p, t, imm);
        return true;
    }
    switch (imm) {
        case VITA_SVC_THREAD_RETURN: vita_thread_end(p, t, (int32_t)t->cpu.r[0]); return true;
        case VITA_SVC_DISPLAY_NEXT: vita_gxm_display_next(p, t); return true;
        case VITA_SVC_DISPLAY_DONE: vita_gxm_display_done(p, t); return true;
        case VITA_SVC_FIBER_RETURN: vita_fiber_ended(p, t); return true;
        case VITA_SVC_CALL_RETURN: vita_hle_call_returned(p, t); return true;
        default: report_fault(p, t, "unknown SVC", imm); return false;
    }
}
int vita_run(VitaProcess* p, double seconds) {
    char why[256];
    if (!(p->jit = vita_jit_new(p, why, sizeof why))) printf("No run-time compiler (%s): only the module's blocks run\n", why);
    uint64_t end = seconds > 0 ? vita_now_ns() + (uint64_t)(seconds * 1e9) : 0;
    int result = 0;
    bool closed = false;
    p->display.next_ns = vita_now_ns() + VBLANK_NS;
    VitaThread* volatile running = NULL;
    VitaThread* last = NULL; /* the thread that ran the last slice */
#ifndef _WIN32
    faults_install(p->mem);
    if (sigsetjmp(fault_jump, 1)) {
        report_fault(p, running, "data abort at", (uint32_t)fault_address);
        result = 1;
        goto done;
    }
#endif
    while (!p->exited) {
        uint64_t now = vita_now_ns(), wake;
        if (now >= p->display.next_ns) {
            ++p->display.vblanks;
            p->display.next_ns += VBLANK_NS;
            if (p->display.next_ns < now) p->display.next_ns = now + VBLANK_NS;
            if (!vita_display_vblank(p)) {
                closed = true;
                break;
            }
            if (end && now >= end) break;
        }
        VitaThread* t = schedule(p, now, &wake);
        if (!t) {
            if (!live(p)) break; /* every thread ended */
            sleep_until(wake < p->display.next_ns ? wake : p->display.next_ns);
            continue;
        }
        /* a thread resumed after another ran: its exclusive monitor cleared, as the kernel's context switch clears it
           (CLREX). Its STREX then fails and its loop reads again, where comparing the value LDREX read would let the
           store pass over the other threads' writes in between (a lock-free list's head popped and pushed back) */
        if (t != last) t->cpu.exclusive_valid = 0;
        p->current = running = last = t;
        t->seq = ++p->seq;
#ifndef _WIN32
        fault_armed = 1;
#endif
        for (unsigned n = 0; n < SLICE_BLOCKS && p->current == t; ++n) {
            uint32_t pc = t->cpu.r[15];
            bool thumb = !!(t->cpu.cpsr & SNX_ARM_T);
            VitaLockstep checked = vita_lockstep_step(p, t);
            if (checked == VITA_LOCKSTEP_MISMATCH) {
                result = 1;
                goto done;
            }
            if (checked == VITA_LOCKSTEP_RAN) continue;
            SnxArmBlock block = block_at(p, pc, thumb);
            if (!block) {
                report_fault(p, t, "no code at", pc);
                result = 1;
                goto done;
            }
            block(&t->cpu);
            g_history[g_history_at++ % HISTORY] = pc | thumb;
            if (t->cpu.exit == SNX_ARM_OK) continue;
            if (t->cpu.exit == SNX_ARM_SVC) {
                t->cpu.exit = SNX_ARM_OK;
                if (!svc(p, t, t->cpu.syndrome)) {
                    result = 1;
                    goto done;
                }
                if (p->exited) break;
                continue;
            }
            report_fault(p, t,
                         t->cpu.exit == SNX_ARM_UNDEFINED    ? "undefined instruction"
                         : t->cpu.exit == SNX_ARM_BREAKPOINT ? "breakpoint"
                         : t->cpu.exit == SNX_ARM_IT_ENTRY   ? "Thumb block entered inside an IT block, ITSTATE"
                                                             : "unsupported instruction (exit reason in cpsr) word",
                         t->cpu.syndrome);
            result = 1;
            goto done;
        }
#ifndef _WIN32
        fault_armed = 0;
#endif
        p->current = NULL;
    }
done:
#ifndef _WIN32
    fault_armed = 0;
#endif
    if (p->exited)
        printf("The game exited (%" PRId32 ")\n", p->exit_code);
    else if (closed)
        printf("The window was closed\n");
    else if (end && vita_now_ns() >= end)
        printf("Stopped after %.0f seconds\n", seconds);
    else if (!result)
        printf("Every thread ended\n");
    if (result || (!p->exited && !closed)) report_threads(p);
    printf("Imports called: %" PRIu64 "\n", p->hle_calls);
    size_t hints = vita_jit_save_hints(p->jit, p->folder);
    if (hints)
        printf("%zu blocks compiled at run time were added to the hints, which a recompile (-r) starts from\n",
               hints);
    vita_jit_free(p->jit);
    p->jit = NULL;
    vita_lockstep_free();
    return result;
}

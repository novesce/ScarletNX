/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A test that includes a runtime source file which emits zones sees SNX_TRACY (snx_nx_flags) and links
   Tracy::TracyClient, but not the runtime's tracy.c: the gate is gated out here. */
#include "nx/runtime/platform/tracy.h"
#if defined(SNX_TEST_VMA_STUBS)
#include "nx/gpu/vk/vk.h"
#endif

#if defined(SNX_TRACY)
int snx_tracy_enter(void) { return 0; }
void snx_tracy_leave(void) {}
TracyCZoneCtx snx_tracy_zone_begin(const struct ___tracy_source_location_data* srcloc) {
    TracyCZoneCtx ctx = {0};
    (void)srcloc;
    return ctx;
}
void snx_tracy_zone_end(TracyCZoneCtx ctx) { (void)ctx; }
#if defined(SNX_TEST_VMA_STUBS)
/* texture.c creates allocations without memory.c, which has the real ones. */
void vk_mem_track(VmaAllocation a) { (void)a; }
void vk_mem_untrack(VmaAllocation a) { (void)a; }
#endif
#endif

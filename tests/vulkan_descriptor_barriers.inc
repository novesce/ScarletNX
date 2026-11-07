/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A requested descriptor mode does not establish the enabled device features. Only the barrier call sees the forced
   flags, so a device without descriptor heaps can exercise the feature policy without recording heap commands. */
static struct {
    PFN_vkCmdPipelineBarrier2 barrier;
    u64 dependencies;
} g_descriptor_barrier_watch;
static VKAPI_ATTR void VKAPI_CALL descriptor_barrier_watch(VkCommandBuffer cb, const VkDependencyInfo* info) {
    for (u32 i = 0; i < info->memoryBarrierCount; i++) {
        const VkMemoryBarrier2* memory = &info->pMemoryBarriers[i];
        if (memory->srcStageMask == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT &&
            memory->srcAccessMask == VK_ACCESS_2_MEMORY_WRITE_BIT &&
            memory->dstStageMask == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT &&
            memory->dstAccessMask == (VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT))
            g_descriptor_barrier_watch.dependencies++;
    }
    g_descriptor_barrier_watch.barrier(cb, info);
}
static int check_descriptor_barrier_fallback(void) {
    vk_pipeline_quiesce(); /* No worker can read capability flags during the forced fallback cases. */
    const u64 gpu = 0xb10000000ull, cpu = HEAP_BASE + 0x1e000000;
    const int buffers[] = {0, 0, 0, 1}, heaps[] = {0, 1, 0, 0}, support[] = {0, 1, 1, 0};
    CHECK(!mem_map(cpu, 0x10000, MS_NORMAL, PERM_RW));
    gmmu_map(gpu, cpu, 0x10000);
    DsState* state = calloc(1, sizeof *state);
    CHECK(state);
    state->owner = 0x8000009du;
    state->tgt.count = 1;
    state->tgt.color[0] = (DsSurf){.gva = gpu, .width = 4, .height = 4, .format = 0xd5, .layers = 1};
    state->gen[DS_TARGETS] = ds_generation_next();
    DrawOwner owner = {0};
    DrawClear red = {.color_mask = 15, .color = {0x3f800000u, 0, 0, 0x3f800000u}};
    DrawClear blue = {.color_mask = 15, .color = {0, 0, 0x3f800000u, 0x3f800000u}};
    u32 validation = g_vk_validation_messages;
    for (u32 test = 0; test < sizeof buffers / sizeof *buffers; test++) {
        vk_lock();
        vk_finish();
        /* A whole clear leaves its render pass open with a pending clear load operation. */
        vk_render_clear(state, &red, &owner);
        u64 passes = g_vk_passes;
        int saved_mode = g_settings.descriptor_buffers;
        int saved_buffers = g_vk.has_descriptor_buffers, saved_heaps = g_vk.has_descriptor_heaps;
        int saved_support = g_vk.has_descriptor_heap_support;
        /* Observe the real Vulkan call in either recording mode. Drain all prior calls before replacing its entry. */
        vk_rec_sync();
        g_descriptor_barrier_watch.barrier = vkCmdPipelineBarrier2;
        g_descriptor_barrier_watch.dependencies = 0;
        vkCmdPipelineBarrier2 = descriptor_barrier_watch;
        g_settings.descriptor_buffers = 3;
        g_vk.has_descriptor_buffers = buffers[test];
        g_vk.has_descriptor_heaps = heaps[test];
        g_vk.has_descriptor_heap_support = support[test];
        vk_unlock();
        g_renderer->method_barrier(0);
        vk_lock();
        vk_rec_sync();
        vkCmdPipelineBarrier2 = g_descriptor_barrier_watch.barrier;
        g_settings.descriptor_buffers = saved_mode;
        g_vk.has_descriptor_buffers = saved_buffers;
        g_vk.has_descriptor_heaps = saved_heaps;
        g_vk.has_descriptor_heap_support = saved_support;
        u32 dependency = !(buffers[test] || support[test]);
        CHECK(g_descriptor_barrier_watch.dependencies == dependency);
        CHECK(g_vk_passes == passes + dependency);
        /* A skipped end lets this clear replace the pending red clear. A real end creates a second pass. */
        vk_render_clear(state, &blue, &owner);
        vk_render_end();
        CHECK(g_vk_passes == passes + dependency + 1);
        Surf* surface = vk_rt_find(gpu);
        VkBuffer readback;
        u8* result = vk_readback_buffer(64, &readback);
        CHECK(surface && result);
        vk_surf_use(vk_cmd(), surface, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .imageExtent = {4, 4, 1}};
        rec_CmdCopyImageToBuffer(vk_cmd(), surface->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);
        vk_finish();
        vk_readback_ready();
        for (u32 i = 0; i < 16; i++) {
            const u8 expected[4] = {0, 0, 255, 255};
            CHECK(!memcmp(result + i * 4, expected, sizeof expected));
        }
        vk_unlock();
    }
    free(state);
    CHECK(g_vk_validation_messages == validation);
    puts("PASS descriptor barrier fallback: requested3, sets, heaps, heap draw fallback and buffers checked=256 exact=1");
    return 0;
}

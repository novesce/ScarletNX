/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* One fragment writes one storage texel per draw. Repeated draws retain the image on the GPU and preserve every
   other byte. Count native dependencies in a separate sample, so the timed loop has no driver-call wrapper. */
static struct {
    PFN_vkCmdPipelineBarrier2 barrier;
    VkImage image;
    u64 dependencies;
} g_storage_barrier_watch;
static VKAPI_ATTR void VKAPI_CALL storage_barrier_watch(VkCommandBuffer cb, const VkDependencyInfo* info) {
    for (u32 i = 0; i < info->imageMemoryBarrierCount; i++) {
        const VkImageMemoryBarrier2* image = &info->pImageMemoryBarriers[i];
        const VkAccessFlags2 writes = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
        if (image->image == g_storage_barrier_watch.image && image->oldLayout == VK_IMAGE_LAYOUT_GENERAL &&
            image->newLayout == VK_IMAGE_LAYOUT_GENERAL && (image->srcAccessMask & writes) &&
            (image->dstAccessMask & writes))
            g_storage_barrier_watch.dependencies++;
    }
    g_storage_barrier_watch.barrier(cb, info);
}
static int check_storage_image_barriers(u32 draws) {
    if (!g_vk.feat.fragmentStoresAndAtomics || !g_vk.feat.shaderStorageImageWriteWithoutFormat) {
        puts("SKIP repeated storage-image dependencies: unformatted fragment writes are unavailable");
        return 0;
    }
    const u64 gpu = 0xb30000000ull, cpu = HEAP_BASE + 0x1c400000;
    const u64 cbuf = gpu + 0x3000, pool = gpu + 0x6000, image = gpu + 0x10000;
    const u32 observed = 16, x = 7, y = 9, value = 0x5eed1234;
    CHECK(draws && mem_map(cpu, 0x20000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(gpu, cpu, 0x20000);
    GpuChannel* channel = imgkeep_channel(gpu, pool);
    CHECK(channel);
    const float position[4] = {0.25f, 0.25f, 0, 1};
    CHECK(!gmem_write(gpu + 0x2000, position, sizeof position));
    CHECK(!imgkeep_image(pool, image, 64, 64));
    CHECK(!storage_write_words(cbuf + 0x124, (const u32[3]){x, y, value}, 3));
    vk_lock();
    mx_draw_state_sync(channel);
    DrawCall call = {.topology = 0, .count = 1, .instances = 1};
    u64 validation = g_vk_validation_messages;
    vk_render_draw(&channel->ds, &call, &channel->owner);
    vk_finish();
    /* Resolve the image before installing the watch. This setup can upload or acquire guest memory. */
    u32 tic[8];
    vk_gmem_words(pool, tic, 8);
    Surf* surface = vk_tex_from_tic_storage(tic);
    CHECK(surface);
    vk_finish();
    vk_rec_sync();
    g_storage_barrier_watch.barrier = vkCmdPipelineBarrier2;
    g_storage_barrier_watch.image = surface->img;
    g_storage_barrier_watch.dependencies = 0;
    vkCmdPipelineBarrier2 = storage_barrier_watch;
    u64 drawn = g_vk_drawn, skipped = g_vk_skipped;
    for (u32 i = 0; i < observed; i++) vk_render_draw(&channel->ds, &call, &channel->owner);
    vk_finish();
    vk_rec_sync();
    vkCmdPipelineBarrier2 = g_storage_barrier_watch.barrier;
    CHECK(g_vk_drawn == drawn + observed && g_vk_skipped == skipped);
    u64 dependencies = g_storage_barrier_watch.dependencies;
    CHECK(dependencies >= observed - 1); /* Each later draw must wait for the previous image write. */
    drawn = g_vk_drawn;
    u64 passes = g_vk_passes, start = plat_ns();
    for (u32 i = 0; i < draws; i++) vk_render_draw(&channel->ds, &call, &channel->owner);
    vk_finish();
    u64 elapsed = plat_ns() - start;
    CHECK(g_vk_drawn == drawn + draws && g_vk_skipped == skipped);
    passes = g_vk_passes - passes;
    vk_unlock();
    imgkeep_release();
    CHECK(!imgkeep_expect("after repeated draws", image, (const u32[][3]){{x, y, value}}, 1));
    CHECK(g_vk_validation_messages == validation);
    free(channel);
    printf("GPU_HOTPATH_BENCH workload=storage_image_dependencies operations=%u ns=%llu exact=1 bytes=%u "
           "observed_draws=%u dependencies=%llu passes=%llu\n", draws, (unsigned long long)elapsed,
           IMGKEEP_SIZE, observed, (unsigned long long)dependencies, (unsigned long long)passes);
    puts("PASS repeated storage-image dependencies: ordered draws and all untouched bytes exact=1");
    return 0;
}

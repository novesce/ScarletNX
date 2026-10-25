/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A completed storage result shares a page with an owned range. Acquiring the
   owner must not re-enter frame writeback while that owner is marked landing. */
static int check_owned_readback(void) {
    const u64 cpu = HEAP_BASE + 0x700000, gva = 0xa000000;
    if (!own_traps()) return 0;
    gmmu_map(gva, cpu, 0x1000);
    vk_lock();
    vk_finish();
    for (u32 fresh = 0; fresh < 2; fresh++) {
        VkBuffer output;
        u64 offset;
        VkOwnRes* owner = vk_own_res_new(256);
        VkSemaphore gate;
        VkSemaphoreTypeCreateInfo type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                          .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
        VkSemaphoreCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type};
        CHECK(owner && VK_OK(vkCreateSemaphore(g_vk.dev, &ci, NULL, &gate)));
        memset(GP(cpu), 0x11, 0x1000);
        CHECK(vk_buf_copy_writable(gva, 256, &output, &offset));
        VkBuffer upload;
        u64 at;
        u8* data = vk_stage_upload(512, 16, &upload, &at);
        CHECK(data);
        memset(data, 0x22, 256);
        memset(data + 256, 0x33, 256);
        VkBufferCopy copy = {at, offset, 256};
        rec_CmdCopyBuffer(vk_cmd(), upload, output, 1, &copy);
        vk_buf_writable_commit();
        copy = (VkBufferCopy){at + 256, 0, 256};
        rec_CmdCopyBuffer(vk_cmd(), upload, owner->buf, 1, &copy);
        /* Hold the submission until submit's automatic completed-frame scan
           has returned. The storage result is then complete but still pending. */
        vk_submit_after(gate, 1, 2);
        vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
        VkSemaphoreSignalInfo signal = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
                                        .semaphore = gate, .value = 1};
        CHECK(VK_OK(vkSignalSemaphore(g_vk.dev, &signal)));
        vk_wait_submit(vk_last_submit());
        vk_own_publish(cpu + 256, 256, owner, 0);
        CHECK(own_lookup(cpu + 256, 256, NULL, 0) == 1);
        if (fresh) vk_frame_next();
        own_acquire(cpu + 256, 256);
        CHECK(own_lookup(cpu + 256, 256, NULL, 0) == 0);
        for (u32 i = 256; i < 512; i++) CHECK(GP(cpu)[i] == 0x33);
        vk_finish();
        for (u32 i = 0; i < 256; i++) CHECK(GP(cpu)[i] == 0x22);
        for (u32 i = 512; i < 4096; i++) CHECK(GP(cpu)[i] == 0x11);
        vk_own_res_unref(owner);
        vkDestroySemaphore(g_vk.dev, gate, NULL);
    }
    vk_unlock();
    puts("Owned readback preserves adjacent pending results in active and fresh frames");
    return 0;
}

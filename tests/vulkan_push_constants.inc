/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Repeated and changed constants must produce the same pixels. A masked clear uses graphics constants of its own,
   and a completed command buffer must not supply constants to its successor. Time includes retirement of the draws.
   The shader and its eight pixel-center positions are synthetic. */
static int check_push_constants(u32 draws) {
    const u64 gpu = 0xaf1000000ull, cpu = HEAP_BASE + 0x1c100000;
    const u64 position = gpu + 0x2000, target = gpu + 0x8000;
    const u64 vertex[] = {0xefd9ff800807ff00ull, 0xeff1ff800707ff00ull, 0xe30000000007000full};
    const u64 fragment[] = {storage_mov(0, 0x3f800000), storage_mov(1, 0), storage_mov(2, 0),
                            storage_mov(3, 0x3f000000), 0xe30000000007000full};
    const f32 transform[6] = {4, 4, 0.5f, 4, 4, 0.5f};
    f32 positions[8][4];
    for (u32 i = 0; i < 8; i++) {
        positions[i][0] = ((f32)i + 0.5f) * 0.25f - 1;
        positions[i][1] = 0.125f;
        positions[i][2] = 0;
        positions[i][3] = 1;
    }
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel && draws >= 32 && !(draws & 7));
    channel->id = 155;
    CHECK(mem_map(cpu, 0x10000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(gpu, cpu, 0x10000);
    CHECK(!gmem_write(position, positions, sizeof positions));
    CHECK(mesh_program(gpu, vertex, 3, 0));
    CHECK(mesh_program(gpu + 0x1000, fragment, 5, 1));
    g_renderer->method_barrier(5);
    u32* r = channel->r3d;
    r[0x582] = (u32)(gpu >> 32); r[0x583] = (u32)gpu;
    r[0x850] = 1; r[0x851] = 0x1000; r[0x64b] = 1;
    r[0x286] = 0x6420; r[0x3fd] = r[0x3fe] = 8u << 16;
    r[0x680] = 0x1111;
    r[MAXWELL3D_RASTERIZE_ENABLE] = 1;
    memcpy(r + 0x280, transform, sizeof transform);
    r[0x458] = 1u << 21 | 7u << 27;
    r[0x700] = 0x1000 | 16;
    r[0x701] = (u32)(position >> 32); r[0x702] = (u32)position;
    r[0x7c0] = (u32)((position + sizeof positions - 1) >> 32);
    r[0x7c1] = (u32)(position + sizeof positions - 1);
    vk_lock();
    int depth;
    rt_clear_format(channel, target, 8, 8, 0, 1, nvn_rt_format(NVN_FORMAT_RGBA32F, &depth));
    channel->r3d_dirty = ~0u;
    mx_draw_state_sync(channel);
    DsState* state = &channel->ds;
    state->dyn.alpha_test = 1;
    state->dyn.alpha_func = DS_CMP_GREATER;
    state->dyn.alpha_ref = 0.25f;
    state->gen[DS_DYNAMIC] = ds_generation_next();
    const char* names[] = {"repeated", "changed", "masked_clear", "command_buffer"};
    u32 validation = g_vk_validation_messages;
    for (u32 phase = 0; phase < 4; phase++) {
        DrawClear clear = {.color_mask = 15, .color = {0, 0, 0x3f800000, 0x3f800000}};
        vk_render_clear(state, &clear, &channel->owner);
        DrawCall call = {.topology = 0, .count = 1, .instances = 1};
        state->dyn.alpha_ref = 0.25f;
        state->gen[DS_DYNAMIC] = ds_generation_next();
        /* Exercise the same state changes before timing, not just the first pipeline compile. */
        u32 warmup = draws > 1024 ? 1024 : 32;
        for (u32 i = 0; i < warmup; i++) {
            if (phase == 1) {
                state->dyn.alpha_ref = i & 2 ? 0.75f : 0.25f;
                state->gen[DS_DYNAMIC] = ds_generation_next();
            }
            if (i == warmup / 2 && phase == 2) {
                clear.color_mask = 7;
                vk_render_clear(state, &clear, &channel->owner);
            }
            if (i == warmup / 2 && phase == 3) vk_finish();
            call.first = i & 7;
            vk_render_draw(state, &call, &channel->owner);
        }
        vk_finish();
        clear.color_mask = 15;
        vk_render_clear(state, &clear, &channel->owner);
        u64 pushed = g_vk_draw_pushes, drawn = g_vk_drawn, skipped = g_vk_skipped;
        u64 start = plat_ns();
        for (u32 i = 0; i < draws; i++) {
            if (phase == 1) {
                state->dyn.alpha_ref = i & 2 ? 0.75f : 0.25f;
                state->gen[DS_DYNAMIC] = ds_generation_next();
            }
            if (i == draws / 2 && phase == 2) {
                clear.color_mask = 7; /* A shader clear changes pc[0..7], then every final pixel is drawn again. */
                vk_render_clear(state, &clear, &channel->owner);
            }
            if (i == draws / 2 && phase == 3) vk_finish();
            call.first = i & 7;
            vk_render_draw(state, &call, &channel->owner);
        }
        vk_finish();
        u64 elapsed = plat_ns() - start, commands = g_vk_draw_pushes - pushed;
        CHECK(g_vk_drawn == drawn + draws && g_vk_skipped == skipped);
        Surf* surface = vk_rt_find(target);
        VkBuffer buffer;
        u8* pixels = vk_readback_buffer(8 * 8 * 16, &buffer);
        CHECK(surface && pixels);
        VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {8, 8, 1}};
        vk_surf_use(vk_cmd(), surface, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT);
        rec_CmdCopyImageToBuffer(vk_cmd(), surface->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
        vk_finish();
        vk_readback_ready();
        for (u32 y = 0; y < 8; y++)
            for (u32 x = 0; x < 8; x++) {
                int red = y == 4 && (phase != 1 || !(x & 2));
                const u32 expected[4] = {red ? 0x3f800000 : 0, 0, red ? 0 : 0x3f800000,
                                         red ? 0x3f000000 : 0x3f800000};
                for (u32 c = 0; c < 4; c++) {
                    u32 got = snx_ld_le32(pixels + ((y * 8 + x) * 4 + c) * 4);
                    if (got != expected[c])
                        fprintf(stderr, "push constants %s pixel(%u,%u) component%u: %08x expected %08x\n",
                                names[phase], x, y, c, got, expected[c]);
                    CHECK(got == expected[c]);
                }
            }
        CHECK(g_vk_validation_messages == validation);
        printf("GPU_HOTPATH_BENCH workload=push_%s operations=%u ns=%llu exact=1 pushes=%llu bytes=80\n",
               names[phase], draws, (unsigned long long)elapsed, (unsigned long long)commands);
    }
    vk_unlock();
    free(channel);
    puts("PASS graphics push constants: repeated, changed, masked clear and new command buffer exact=1");
    return 0;
}

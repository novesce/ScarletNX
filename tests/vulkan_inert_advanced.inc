/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* An advanced equation has no color effect when all components are masked, or raster discard is active. Preserve
   whole-draw depth/stencil, shader atomics and available transform feedback. Every triangle overlaps all16 pixels.
   The ordinary draw supplies an independent raw-output oracle. Benchmark mode also accepts the older capture path.
   Include after the late-depth fixtures for their native color/depth/stencil readback helper. */
static int check_inert_advanced(u32 primitives, int require_native) {
    vk_pipeline_quiesce();
    if (!g_vk.feat.fragmentStoresAndAtomics || !g_vk.feat.vertexPipelineStoresAndAtomics ||
        !g_vk.feat.shaderStorageImageReadWithoutFormat || !g_vk.feat.shaderStorageImageWriteWithoutFormat ||
        !g_vk.image_slots[ST_VS] || !g_vk.image_slots[ST_FS]) {
        puts("SKIP inert advanced blend: vertex and fragment image atomics are unavailable");
        return 0;
    }
    CHECK(primitives && primitives <= 1024);
    const u64 gpu = 0xaf3000000ull, cpu = HEAP_BASE + 0x1c300000;
    const u64 pool = gpu + 0x2000, vs_cb = gpu + 0x3000, fs_cb = gpu + 0x3400;
    const u64 vs_counter = gpu + 0x4000, fs_counter = gpu + 0x5000, target = gpu + 0x8000;
    const u64 depth = gpu + 0xa000, control = gpu + 0xc000, feedback = gpu + 0x10000, stream = gpu + 0x20000;
    const u32 vertices = 3 * primitives, bytes = vertices * 16;
    float* positions = malloc(bytes);
    u8* feedback_bytes = malloc((size_t)bytes + 16);
    u8* feedback_reference = malloc((size_t)bytes + 16);
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(positions && feedback_bytes && feedback_reference && channel);
    channel->id = 157;
    CHECK(!mem_map(cpu, 0x40000, MS_NORMAL, PERM_RW));
    gmmu_map(gpu, cpu, 0x40000);
    const float triangle[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    for (u32 i = 0; i < vertices; i++) memcpy(positions + i * 4, triangle + (i % 3) * 4, 16);
    CHECK(!gmem_write(stream, positions, bytes));
    const u64 vertex[] = {storage_mov(0, 0), storage_mov(1, 0), 0x4c98078804a70004ull,
        storage_mov(8, 1), (0xeb50020a00070008ull & ~(7ull << 33)) | 3ull << 33,
        0xefd9ff800807ff00ull, 0xeff1ff800707ff00ull, 0xe30000000007000full};
    CHECK(mesh_program(gpu, vertex, sizeof vertex / sizeof *vertex, 0));
    CHECK(blend_capture_program(gpu + 0x800, 0, 0, 0));
    const u32 format = 0x0f | 4u << 7 | 4u << 10 | 4u << 13 | 4u << 16 |
                       2u << 19 | 3u << 22 | 4u << 25 | 5u << 28;
    for (u32 stage = 0; stage < 2; stage++) {
        u64 address = stage ? fs_counter : vs_counter;
        const u32 header[8] = {format, (u32)address, (u32)(address >> 32) | 3u << 21,
                               0, 3u | 1u << 23, 3u | 1u << 31, 0, 0};
        CHECK(!storage_write_words(pool + 32 * stage, header, 8));
    }
    CHECK(!storage_write_words(vs_cb + 0x128, (const u32[1]){0}, 1));
    CHECK(!storage_write_words(fs_cb + 0x128, (const u32[1]){1}, 1));
    u32* r = channel->r3d;
    r[0x582] = (u32)(gpu >> 32); r[0x583] = (u32)gpu;
    r[0x55d] = (u32)(pool >> 32); r[0x55e] = (u32)pool; r[0x55f] = 1;
    r[0x850] = 1; r[0x851] = 0x800; r[0x64b] = 1;
    r[0x286] = 0x6420; r[0x3fd] = r[0x3fe] = 4u << 16;
    r[0x680] = 0x1111;
    r[MAXWELL3D_RASTERIZE_ENABLE] = 1;
    const float transform[6] = {2, 2, 0.5f, 2, 2, 0.5f};
    memcpy(r + 0x280, transform, sizeof transform);
    r[0x458] = 1u << 21 | 7u << 27;
    r[0x700] = 0x1000 | 16;
    r[0x701] = (u32)(stream >> 32); r[0x702] = (u32)stream;
    r[0x7c0] = (u32)((stream + bytes - 1) >> 32); r[0x7c1] = (u32)(stream + bytes - 1);
    vk_lock();
    vk_finish();
    u32 validation = g_vk_validation_messages, native = g_vk.advanced_blend_caps.available;
    int interlock = g_vk.has_fragment_interlock;
    g_vk.advanced_blend_caps.available = 0;
    g_vk.has_fragment_interlock = 0;
    rt_clear_format(channel, target, 4, 4, 0, 1, 0xc0);
    channel->r3d_dirty = ~0u;
    mx_draw_state_sync(channel);
    DsState* state = &channel->ds;
    state->bind.tex_pool = pool;
    state->bind.tex_max = 1;
    state->bind.cb_addr[ST_VS][2] = vs_cb;
    state->bind.cb_size[ST_VS][2] = 0x200;
    state->bind.cb_addr[ST_FS][2] = fs_cb;
    state->bind.cb_size[ST_FS][2] = 0x200;
    state->tgt.depth = (DsSurf){.gva = depth, .width = 4, .height = 4, .format = 0x19,
                                .layers = 1, .layer_stride = 512};
    state->prog.early_z = 0;
    state->dyn.depth_test = state->dyn.depth_write = state->dyn.stencil_test = 1;
    state->dyn.depth_cmp = DS_CMP_LESS;
    state->dyn.alpha_test = 0;
    state->rkey.sample_mask = ~0u;
    state->rkey.alpha_to_coverage = state->rkey.alpha_to_one = 0;
    state->blend.en[0] = 1;
    for (u32 face = 0; face < 2; face++) {
        state->dyn.face[face].cmp = DS_CMP_ALWAYS;
        state->dyn.face[face].fail = DS_STENCIL_ZERO;
        state->dyn.face[face].zfail = DS_STENCIL_INCREMENT_AND_CLAMP;
        state->dyn.face[face].pass = DS_STENCIL_REPLACE;
        state->dyn.face[face].ref = 9;
        state->dyn.face[face].cmask = state->dyn.face[face].wmask = 255;
    }
    const u32 destination[4] = {0x3f100000, 0x3e400000, 0x3ec00000, 0x3f400000};
    u64 packed = nvn_set_field(nvn_set_field(nvn_set_field(0, NVN_BLEND_ADV, NVN_BLEND_ADVANCED_MODE_OVERLAY),
                                             NVN_BLEND_ADV_PMS, 1), NVN_BLEND_ADV_NPM, 1);
    for (u32 discard = 0; discard < 2; discard++) {
        u8 reference[336], image_reference[2][512];
        int xfb = discard && g_vk.has_xfb;
        state->dyn.discard = (u8)discard;
        state->blend.mask[0] = discard ? 15 : 0;
        state->prog.xfb_on = (u8)xfb;
        state->xfb = (ShXfb){0};
        state->xfb.count[0] = 4; state->xfb.stride[0] = 16;
        for (u32 i = 0; i < 4; i++) state->xfb.layout[0][i] = (u8)(28 + i);
        state->bind.xfb[0].addr = feedback;
        state->bind.xfb[0].size = bytes;
        state->bind.xfb[0].on = (u32)xfb;
        /* The first identical job compiles and executes this path before its reported measurement. */
        for (u32 advanced = 0; advanced < 2; advanced++)
        for (u32 measured = 0; measured < 2; measured++) {
            u8 initial[512], counters[2][512];
            memset(initial, 0xcd, sizeof initial);
            for (u32 y = 0; y < 4; y++)
                for (u32 x = 0; x < 4; x++) snx_st_le32(initial + bl_offset(4 * x, y, 0, 16, 4, 0, 0), 0);
            CHECK(!gmem_write(vs_counter, initial, sizeof initial));
            CHECK(!gmem_write(fs_counter, initial, sizeof initial));
            memset(feedback_bytes, 0xcd, (size_t)bytes + 16);
            CHECK(!gmem_write(feedback, feedback_bytes, bytes + 16));
            CHECK(!gmem_write(control, initial, 32));
            for (u32 i = 0; i < DS_GENS; i++) state->gen[i] = ds_generation_next();
            DrawOwner owner = {0};
            if (xfb) CHECK(vk_render_xfb_control(state->owner, control, 0, 16, &owner));
            DrawClear clear = {.color_mask = 15, .aspects = 3, .depth = 1, .stencil = 4};
            memcpy(clear.color, destination, sizeof destination);
            vk_render_clear(state, &clear, &owner);
            vk_finish();
            DrawCall call = {.topology = 4, .count = vertices, .instances = 1};
            call.advanced_blend[0] = advanced ? packed : 0;
            u64 drawn = g_vk_drawn, skipped = g_vk_skipped, passes = g_vk_passes;
            u64 start = plat_ns();
            vk_render_draw(state, &call, &owner);
            vk_finish();
            u64 elapsed = plat_ns() - start, made_passes = g_vk_passes - passes;
            CHECK(g_vk_drawn == drawn + 1 && g_vk_skipped == skipped);
            if (advanced && require_native) CHECK(made_passes == 1);
            if (xfb) CHECK(vk_render_xfb_control(state->owner, control, 1, 16, &owner));
            u8 output[336];
            CHECK(!late_shadow_read(target, depth, output));
            CHECK(!gmem_read(vs_counter, counters[0], sizeof counters[0]));
            CHECK(!gmem_read(fs_counter, counters[1], sizeof counters[1]));
            for (u32 pixel = 0; pixel < 16; pixel++) {
                for (u32 c = 0; c < 4; c++) CHECK(snx_ld_le32(output + pixel * 16 + c * 4) == destination[c]);
                CHECK(snx_ld_le32(output + 256 + pixel * 4) == (discard ? 0x3f800000u : 0x3f000000u));
                CHECK(output[320 + pixel] == (discard ? 4 : primitives + 8 > 255 ? 255 : primitives + 8));
            }
            u8 expected[2][512];
            memcpy(expected[0], initial, sizeof initial);
            memcpy(expected[1], initial, sizeof initial);
            snx_st_le32(expected[0], vertices);
            for (u32 y = 0; y < 4; y++)
                for (u32 x = 0; x < 4; x++)
                    snx_st_le32(expected[1] + bl_offset(4 * x, y, 0, 16, 4, 0, 0), discard ? 0 : primitives);
            CHECK(!memcmp(counters, expected, sizeof expected));
            if (xfb) {
                u32 captured[8];
                CHECK(!gmem_read(control, captured, sizeof captured));
                CHECK(captured[0] == 16 && captured[1] == bytes);
                CHECK(!gmem_read(feedback, feedback_bytes, bytes + 16));
                for (u32 i = 0; i < vertices; i++) CHECK(snx_ld_le32(feedback_bytes + 16 * i + 12) == 0x3f800000u);
                for (u32 i = bytes; i < bytes + 16; i++) CHECK(feedback_bytes[i] == 0xcd);
            }
            if (!advanced) {
                memcpy(reference, output, sizeof reference);
                memcpy(image_reference, counters, sizeof image_reference);
                if (xfb) memcpy(feedback_reference, feedback_bytes, bytes + 16);
            } else {
                CHECK(!memcmp(reference, output, sizeof reference));
                CHECK(!memcmp(image_reference, counters, sizeof image_reference));
                if (xfb) CHECK(!memcmp(feedback_reference, feedback_bytes, bytes + 16));
                if (measured)
                    printf("GPU_HOTPATH_BENCH workload=inert_advanced_%s operations=1 ns=%llu exact=1 primitives=%u "
                           "vertices=%u passes=%llu xfb=%d\n", discard ? "discard" : "mask0",
                           (unsigned long long)elapsed, primitives, vertices, (unsigned long long)made_passes, xfb);
            }
            CHECK(g_vk_validation_messages == validation);
        }
    }
    g_vk.advanced_blend_caps.available = native;
    g_vk.has_fragment_interlock = interlock;
    vk_unlock();
    free(positions); free(feedback_bytes); free(feedback_reference); free(channel);
    puts("PASS inert advanced blend: preserved color, depth/stencil, VS/FS atomics and available XFB exact=1");
    return 0;
}

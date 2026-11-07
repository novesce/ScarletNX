/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A point at each pixel reads a color from an instanced stream. GPU-owned inputs keep their native handles and
   offsets while their guest bounds and strides change. Exact output checks each transition without a host wait.
   This watch runs only for this fixture, outside all timed workloads. */
static struct {
    PFN_vkCmdBindVertexBuffers2 bind;
    PFN_vkCmdBindPipeline pipeline;
    PFN_vkCmdDraw draw;
    u32 calls, pipeline_calls, capture, draws, invalid;
    VkPipeline current_pipeline;
    struct {
        VkCommandBuffer cb;
        VkPipeline pipeline;
        u32 calls, pipeline_calls, count, strides;
        VkBuffer buffers[2];
        VkDeviceSize offsets[2], sizes[2], stride[2];
    } current, seen[16];
} g_vertex_bind_watch;
static VKAPI_ATTR void VKAPI_CALL vertex_bind_watch(VkCommandBuffer cb, u32 first, u32 count,
        const VkBuffer* buffers, const VkDeviceSize* offsets, const VkDeviceSize* sizes, const VkDeviceSize* strides) {
    g_vertex_bind_watch.calls++;
    if (g_vertex_bind_watch.capture) {
        g_vertex_bind_watch.current.cb = cb;
        g_vertex_bind_watch.current.count = count;
        g_vertex_bind_watch.current.strides = !!strides;
        if (first || count != 2 || !sizes)
            g_vertex_bind_watch.invalid = 1;
        else
            for (u32 i = 0; i < count; i++) {
                g_vertex_bind_watch.current.buffers[i] = buffers[i];
                g_vertex_bind_watch.current.offsets[i] = offsets[i];
                g_vertex_bind_watch.current.sizes[i] = sizes[i];
                g_vertex_bind_watch.current.stride[i] = strides ? strides[i] : 0;
            }
    }
    g_vertex_bind_watch.bind(cb, first, count, buffers, offsets, sizes, strides);
}
static VKAPI_ATTR void VKAPI_CALL vertex_pipeline_watch(VkCommandBuffer cb, VkPipelineBindPoint point,
                                                         VkPipeline pipeline) {
    if (point == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        g_vertex_bind_watch.pipeline_calls++;
        g_vertex_bind_watch.current_pipeline = pipeline;
    }
    g_vertex_bind_watch.pipeline(cb, point, pipeline);
}
static VKAPI_ATTR void VKAPI_CALL vertex_draw_watch(VkCommandBuffer cb, u32 vertices, u32 instances, u32 first,
                                                     u32 instance) {
    if (g_vertex_bind_watch.capture) {
        if (g_vertex_bind_watch.draws == 16 || cb != g_vertex_bind_watch.current.cb)
            g_vertex_bind_watch.invalid = 1;
        else {
            g_vertex_bind_watch.current.pipeline = g_vertex_bind_watch.current_pipeline;
            g_vertex_bind_watch.current.calls = g_vertex_bind_watch.calls;
            g_vertex_bind_watch.current.pipeline_calls = g_vertex_bind_watch.pipeline_calls;
            g_vertex_bind_watch.seen[g_vertex_bind_watch.draws++] = g_vertex_bind_watch.current;
        }
    }
    g_vertex_bind_watch.draw(cb, vertices, instances, first, instance);
}
static void vertex_binding_capture_begin(void) {
    vk_rec_sync();
    g_vertex_bind_watch.calls = g_vertex_bind_watch.pipeline_calls = g_vertex_bind_watch.draws = 0;
    g_vertex_bind_watch.invalid = 0;
    g_vertex_bind_watch.current_pipeline = VK_NULL_HANDLE;
    memset(&g_vertex_bind_watch.current, 0, sizeof g_vertex_bind_watch.current);
    g_vertex_bind_watch.capture = 1;
}

static void vertex_binding_state(DsState* state, u64 gpu, u64 color, u64 alternate, u32 test, DrawCall* call) {
    *call = (DrawCall){.topology = 0, .first = test, .count = 1, .instances = 1};
    state->prog.code[1] = gpu + (test == 9 ? 0x800 : 0);
    state->vbuf.addr[1] = test >= 7 ? alternate : test >= 2 ? color + 16 : color;
    state->vbuf.size[1] = test >= 7 ? 16 : test == 3 ? 16 : 64;
    state->vfmt.stride[1] = test == 5 ? 32 : 16;
    if (test >= 3 && test <= 6 && (test != 3 || g_vk.has_vertex_robustness2)) call->base_instance = 1;
    state->vfmt.constant = test == 12 ? 2 : 0;
    state->gen[DS_VFORMAT] = state->gen[DS_VBUF] = state->gen[DS_PROGRAMS] = ds_generation_next();
}

static int vertex_binding_pending(u64 position, u64 color, u64 alternate) {
    VkBuffer pending, color_pending;
    u64 pending_offset, color_pending_offset;
    CHECK(vk_buf_copy_writable(position, 256, &pending, &pending_offset, 1));
    vk_buf_writable_commit();
    CHECK(vk_buf_copy_writable(color, 128, &color_pending, &color_pending_offset, 1));
    vk_buf_writable_commit();
    CHECK(vk_buf_copy_writable(alternate, 16, &pending, &pending_offset, 1));
    vk_buf_writable_commit();
    VkBuffer first_window, shifted_window;
    u64 first_offset, shifted_offset;
    CHECK(vk_buf_get_input(color, 64, 4, &first_window, &first_offset));
    CHECK(vk_buf_get_input(color + 16, 64, 4, &shifted_window, &shifted_offset));
    CHECK(first_window == color_pending && first_offset == color_pending_offset);
    CHECK(shifted_window == first_window && shifted_offset == first_offset + 16);
    return 0;
}

static int vertex_binding_pixels(u64 target, const u32* expected, u32 count) {
    vk_render_end();
    Surf* surface = vk_rt_find(target);
    VkBuffer buffer;
    u8* pixels = vk_readback_buffer(16 * 8 * 16, &buffer);
    CHECK(surface && pixels);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {16, 8, 1}};
    vk_surf_use(vk_cmd(), surface, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    rec_CmdCopyImageToBuffer(vk_cmd(), surface->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    vk_finish();
    vk_readback_ready();
    for (u32 y = 0; y < 8; y++)
        for (u32 x = 0; x < 16; x++)
            for (u32 c = 0; c < 4; c++) {
                u32 want = y == 4 && x < count ? expected[4 * x + c] ? 0x3f800000 : 0 : c >= 2 ? 0x3f800000 : 0;
                u32 got = snx_ld_le32(pixels + ((y * 16 + x) * 4 + c) * 4);
                if (got != want)
                    fprintf(stderr, "vertex binding pixel(%u,%u) component%u: %08x expected %08x\n", x, y, c, got, want);
                CHECK(got == want);
            }
    return 0;
}

/* Change only one native color-window member. All reads remain in bounds, including on devices without exact
   robustness2 results. The native watch checks the size itself rather than relying on an out-of-bounds pixel. */
static int vertex_binding_key_phase(GpuChannel* channel, u64 gpu, const u64 address[4],
                                     const DrawClear* clear, int stride) {
    DsState* state = &channel->ds;
    DrawCall call;
    vertex_binding_state(state, gpu, address[1], address[2], 0, &call);
    state->vbuf.addr[1] = address[1] + 16;
    state->vbuf.size[1] = 64;
    call.base_instance = stride ? 1 : 0;
    vk_render_clear(state, clear, &channel->owner);
    vk_finish();
    CHECK(!vertex_binding_pending(address[0], address[1], address[2]));
    vertex_binding_capture_begin();
    u32 expected[16 * 4];
    for (u32 i = 0; i < 16; i++) {
        int changed = !!(i & 2);
        call.first = i;
        if (stride) {
            state->vfmt.stride[1] = changed ? 32 : 16;
            state->gen[DS_VFORMAT] = ds_generation_next();
        } else {
            state->vbuf.size[1] = changed ? 16 : 64;
            state->gen[DS_VBUF] = ds_generation_next();
        }
        expected[4 * i] = stride && changed;
        expected[4 * i + 1] = !stride || changed;
        expected[4 * i + 2] = stride && !changed;
        expected[4 * i + 3] = 1;
        vk_render_draw(state, &call, &channel->owner);
    }
    vk_finish();
    vk_rec_sync();
    g_vertex_bind_watch.capture = 0;
    CHECK(!vertex_binding_pixels(address[3], expected, 16));
    vk_rec_sync();
    CHECK(!g_vertex_bind_watch.invalid && g_vertex_bind_watch.draws == 16);
    u32 isolated = 0, repeated_hits = 0;
    for (u32 i = 0; i < 16; i++) {
        u32 wanted = stride ? (i & 2 ? 32 : 16) : (i & 2 ? 16 : 64);
        CHECK(g_vertex_bind_watch.seen[i].count == 2);
        if (stride)
            CHECK(g_vertex_bind_watch.seen[i].strides && g_vertex_bind_watch.seen[i].stride[1] == wanted);
        else
            CHECK(g_vertex_bind_watch.seen[i].sizes[1] == wanted);
        if (!i || g_vertex_bind_watch.seen[i].cb != g_vertex_bind_watch.seen[i - 1].cb ||
            g_vertex_bind_watch.seen[i].pipeline != g_vertex_bind_watch.seen[i - 1].pipeline ||
            g_vertex_bind_watch.seen[i].pipeline_calls != g_vertex_bind_watch.seen[i - 1].pipeline_calls ||
            g_vertex_bind_watch.seen[i].strides != g_vertex_bind_watch.seen[i - 1].strides ||
            memcmp(g_vertex_bind_watch.seen[i].buffers, g_vertex_bind_watch.seen[i - 1].buffers,
                   sizeof g_vertex_bind_watch.seen[i].buffers) ||
            memcmp(g_vertex_bind_watch.seen[i].offsets, g_vertex_bind_watch.seen[i - 1].offsets,
                   sizeof g_vertex_bind_watch.seen[i].offsets) ||
            g_vertex_bind_watch.seen[i].sizes[0] != g_vertex_bind_watch.seen[i - 1].sizes[0] ||
            g_vertex_bind_watch.seen[i].stride[0] != g_vertex_bind_watch.seen[i - 1].stride[0] ||
            (stride ? g_vertex_bind_watch.seen[i].sizes[1] != g_vertex_bind_watch.seen[i - 1].sizes[1]
                    : g_vertex_bind_watch.seen[i].stride[1] != g_vertex_bind_watch.seen[i - 1].stride[1]))
            continue;
        u64 a = stride ? g_vertex_bind_watch.seen[i].stride[1] : g_vertex_bind_watch.seen[i].sizes[1];
        u64 b = stride ? g_vertex_bind_watch.seen[i - 1].stride[1] : g_vertex_bind_watch.seen[i - 1].sizes[1];
        if (a != b) isolated++;
        if (a == b && g_vertex_bind_watch.seen[i].calls == g_vertex_bind_watch.seen[i - 1].calls) repeated_hits++;
    }
    if (!isolated || !repeated_hits)
        fprintf(stderr, "vertex binding watch: %u %s-only pairs, %u exact cache hits\n",
                isolated, stride ? "stride" : "size", repeated_hits);
    CHECK(isolated && repeated_hits);
    printf("vertex binding watch: %u %s-only pairs, %u exact cache hits in one command buffer\n",
           isolated, stride ? "stride" : "size", repeated_hits);
    return 0;
}

static int check_vertex_binding_changes(void) {
    const u64 gpu = 0xaf2000000ull, cpu = HEAP_BASE + 0x1c200000;
    const u64 position = gpu + 0x2000, color = gpu + 0x3000, alternate = gpu + 0x4000, target = gpu + 0x8000;
    const u64 vertex[] = {0xefd9ff800807ff00ull, 0xeff1ff800707ff00ull, 0xefd9ff800907ff00ull,
                          0xeff1ff800807ff00ull, 0xe30000000007000full};
    const u64 fixed_vertex[] = {storage_mov(0, 0x3e400000), storage_mov(1, 0x3e000000), storage_mov(2, 0),
        storage_mov(3, 0x3f800000), 0xeff1ff800707ff00ull, storage_mov(0, 0x3f800000),
        storage_mov(1, 0x3f800000), storage_mov(2, 0x3f800000), storage_mov(3, 0x3f800000),
        0xeff1ff800807ff00ull, 0xe30000000007000full};
    u64 fragment[5];
    for (u32 i = 0; i < 4; i++) fragment[i] = 0xe000000000070000ull | (0x20ull + i) << 30 | i;
    fragment[4] = 0xe30000000007000full;
    const f32 transform[6] = {8, 4, 0.5f, 8, 4, 0.5f};
    const f32 colors[8][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 0, 1},
                             {1, 0, 1, 1}, {0, 1, 1, 1}, {1, 1, 1, 1}, {0, 0, 0, 1}};
    const f32 cyan[4] = {0, 1, 1, 1};
    f32 positions[16][4];
    for (u32 i = 0; i < 16; i++) {
        positions[i][0] = ((f32)i + 0.5f) * 0.125f - 1;
        positions[i][1] = 0.125f;
        positions[i][2] = 0;
        positions[i][3] = 1;
    }
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 156;
    CHECK(mem_map(cpu, 0x10000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(gpu, cpu, 0x10000);
    CHECK(!gmem_write(position, positions, sizeof positions));
    CHECK(!gmem_write(color, colors, sizeof colors));
    CHECK(!gmem_write(alternate, cyan, sizeof cyan));
    CHECK(mesh_program(gpu, vertex, 5, 0));
    CHECK(mesh_program(gpu + 0x800, fixed_vertex, 11, 0));
    CHECK(mesh_program(gpu + 0x1000, fragment, 5, 1));
    g_renderer->method_barrier(5);
    u32* r = channel->r3d;
    r[0x582] = (u32)(gpu >> 32); r[0x583] = (u32)gpu;
    r[0x850] = 1; r[0x851] = 0x1000; r[0x64b] = 1;
    r[0x286] = 0x6420; r[0x3fd] = 16u << 16; r[0x3fe] = 8u << 16;
    r[0x680] = 0x1111;
    r[MAXWELL3D_RASTERIZE_ENABLE] = 1;
    memcpy(r + 0x280, transform, sizeof transform);
    r[0x458] = 1u << 21 | 7u << 27;
    r[0x459] = 1 | 1u << 21 | 7u << 27;
    for (u32 stream = 0; stream < 2; stream++) {
        u64 source = stream ? color : position;
        u32 size = stream ? 64 : sizeof positions;
        r[0x700 + stream * 4] = 0x1000 | 16;
        r[0x701 + stream * 4] = (u32)(source >> 32); r[0x702 + stream * 4] = (u32)source;
        r[0x7c0 + stream * 2] = (u32)((source + size - 1) >> 32);
        r[0x7c1 + stream * 2] = (u32)(source + size - 1);
    }
    r[0x621] = r[0x707] = 1;
    vk_lock();
    vk_finish();
    u32 validation = g_vk_validation_messages;
    int depth;
    rt_clear_format(channel, target, 16, 8, 0, 1, nvn_rt_format(NVN_FORMAT_RGBA32F, &depth));
    channel->r3d_dirty = ~0u;
    mx_draw_state_sync(channel);
    DsState* state = &channel->ds;
    state->dyn.alpha_test = state->dyn.depth_test = state->dyn.depth_write = state->dyn.stencil_test = 0;
    state->gen[DS_DYNAMIC] = ds_generation_next();
    DrawClear clear = {.color_mask = 15, .color = {0, 0, 0x3f800000, 0x3f800000}};
    vk_render_clear(state, &clear, &channel->owner);
    /* Cold translation can span the periodic submission limit. Compile every state variant before the checked
       transitions, then recreate the pending inputs after the warm-up's results finish. */
    for (u32 test = 0; test < 14; test++) {
        DrawCall call;
        vertex_binding_state(state, gpu, color, alternate, test, &call);
        vk_render_draw(state, &call, &channel->owner);
    }
    vk_finish();
    DrawCall prepared;
    vertex_binding_state(state, gpu, color, alternate, 0, &prepared);
    vk_render_clear(state, &clear, &channel->owner);
    vk_finish();
    CHECK(!vertex_binding_pending(position, color, alternate));
    vk_rec_sync();
    g_vertex_bind_watch.bind = vkCmdBindVertexBuffers2;
    g_vertex_bind_watch.pipeline = vkCmdBindPipeline;
    g_vertex_bind_watch.draw = vkCmdDraw;
    vkCmdBindVertexBuffers2 = vertex_bind_watch;
    vkCmdBindPipeline = vertex_pipeline_watch;
    vkCmdDraw = vertex_draw_watch;
    vertex_binding_capture_begin();
    u32 alternating[16 * 4];
    for (u32 i = 0; i < 16; i++) {
        int shifted = !!(i & 2);
        alternating[4 * i] = !shifted;
        alternating[4 * i + 1] = shifted;
        alternating[4 * i + 2] = 0;
        alternating[4 * i + 3] = 1;
        state->vbuf.addr[1] = color + (shifted ? 16 : 0);
        state->gen[DS_VBUF] = ds_generation_next();
        prepared.first = i;
        vk_render_draw(state, &prepared, &channel->owner);
    }
    vk_finish();
    vk_rec_sync();
    g_vertex_bind_watch.capture = 0;
    CHECK(!vertex_binding_pixels(target, alternating, 16));
    vk_rec_sync();
    CHECK(!g_vertex_bind_watch.invalid && g_vertex_bind_watch.draws == 16);
    u32 offset_pairs = 0, repeated_hits = 0;
    for (u32 i = 1; i < 16; i++) {
        /* Fresh state always rebinds the pipeline. Same command buffer and pipeline-bind count therefore exclude
           both `fresh` and `pipe_changed` as reasons to bind these vertex windows. */
        if (g_vertex_bind_watch.seen[i].cb != g_vertex_bind_watch.seen[i - 1].cb ||
            g_vertex_bind_watch.seen[i].pipeline != g_vertex_bind_watch.seen[i - 1].pipeline ||
            g_vertex_bind_watch.seen[i].pipeline_calls != g_vertex_bind_watch.seen[i - 1].pipeline_calls ||
            g_vertex_bind_watch.seen[i].strides != g_vertex_bind_watch.seen[i - 1].strides ||
            memcmp(g_vertex_bind_watch.seen[i].buffers, g_vertex_bind_watch.seen[i - 1].buffers,
                   sizeof g_vertex_bind_watch.seen[i].buffers) ||
            memcmp(g_vertex_bind_watch.seen[i].sizes, g_vertex_bind_watch.seen[i - 1].sizes,
                   sizeof g_vertex_bind_watch.seen[i].sizes) ||
            memcmp(g_vertex_bind_watch.seen[i].stride, g_vertex_bind_watch.seen[i - 1].stride,
                   sizeof g_vertex_bind_watch.seen[i].stride) ||
            g_vertex_bind_watch.seen[i].offsets[0] != g_vertex_bind_watch.seen[i - 1].offsets[0])
            continue;
        u64 a = g_vertex_bind_watch.seen[i].offsets[1], b = g_vertex_bind_watch.seen[i - 1].offsets[1];
        if (a != b && (a > b ? a - b : b - a) == 16) offset_pairs++;
        if (a == b && g_vertex_bind_watch.seen[i].calls == g_vertex_bind_watch.seen[i - 1].calls) repeated_hits++;
    }
    if (!offset_pairs || !repeated_hits)
        fprintf(stderr, "vertex binding watch: %u offset-only pairs, %u exact cache hits\n", offset_pairs, repeated_hits);
    CHECK(offset_pairs && repeated_hits);
    printf("vertex binding watch: %u offset-only pairs, %u exact cache hits within command buffers\n",
           offset_pairs, repeated_hits);
    const u64 phase_address[4] = {position, color, alternate, target};
    CHECK(!vertex_binding_key_phase(channel, gpu, phase_address, &clear, 0));
    if (!g_vk.vertex_input_dynamic && g_vk.has_dynamic_stride)
        CHECK(!vertex_binding_key_phase(channel, gpu, phase_address, &clear, 1));
    else
        puts("SKIP vertex stride API pairs: the binding command does not supply strides; pixel controls still run");
    vertex_binding_state(state, gpu, color, alternate, 0, &prepared);
    vk_render_clear(state, &clear, &channel->owner);
    vk_finish();
    CHECK(!vertex_binding_pending(position, color, alternate));
    vk_rec_sync();
    g_vertex_bind_watch.calls = 0;
    u32 expected[14 * 4] = {
        1, 0, 0, 1,  1, 0, 0, 1,  0, 1, 0, 1,  0, 0, 0, 0,  0, 0, 1, 1,  1, 1, 0, 1,
        0, 0, 1, 1,  0, 1, 1, 1,  0, 1, 1, 1,  1, 1, 1, 1,  0, 1, 1, 1,  0, 1, 1, 1,
        0, 0, 0, 1,  0, 1, 1, 1};
    u64 drawn = g_vk_drawn, skipped = g_vk_skipped;
    for (u32 test = 0; test < 14; test++) {
        DrawCall call;
        vertex_binding_state(state, gpu, color, alternate, test, &call);
        if (test == 3 && !g_vk.has_vertex_robustness2) {
            /* Core robustness permits less precise out-of-bounds values. Do not invent an exact oracle for them. */
            expected[4 * test + 1] = expected[4 * test + 3] = 1;
        }
        if (test == 8) vk_render_forget_state();
        if (test == 11) vk_finish();
        vk_render_draw(state, &call, &channel->owner);
    }
    vk_finish();
    vk_rec_sync();
    vkCmdBindVertexBuffers2 = g_vertex_bind_watch.bind;
    vkCmdBindPipeline = g_vertex_bind_watch.pipeline;
    vkCmdDraw = g_vertex_bind_watch.draw;
    CHECK(g_vk_drawn == drawn + 14 && g_vk_skipped == skipped);
    CHECK(!vertex_binding_pixels(target, expected, 14));
    CHECK(g_vk_validation_messages == validation);
    vk_unlock();
    free(channel);
    printf("PASS vertex binding cache: 14 draws, %u binds, offsets, sizes, strides, empty inputs and resets exact=1\n",
           g_vertex_bind_watch.calls);
    return 0;
}

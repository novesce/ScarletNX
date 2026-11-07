/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Observe the create info at the native requirements query: VMA keeps its own vkCreateImage entry point. sRGB
   targets and crops retain sRGB backing. Sampled linear views expose exact UNORM bytes to the readback shader. */
#include "common/utilities/spirv_emit.h"
#include <math.h>

typedef struct {
    u32 queries, wrong_formats, wrong_lists;
    u64 copies;
    VkFormat first_format;
} SrgbCropTrace;
static struct {
    PFN_vkGetDeviceImageMemoryRequirements requirements;
    PFN_vkCmdCopyImage copy;
    VkFormat expected;
    VkImage source;
    int twins;
    SrgbCropTrace trace;
} g_srgb_crop_watch;
static VKAPI_ATTR void VKAPI_CALL srgb_crop_requirements_watch(
    VkDevice device, const VkDeviceImageMemoryRequirements* query, VkMemoryRequirements2* requirements) {
    const VkImageCreateInfo* image = query->pCreateInfo;
    if (!g_srgb_crop_watch.trace.queries++) g_srgb_crop_watch.trace.first_format = image->format;
    g_srgb_crop_watch.trace.wrong_formats += image->format != g_srgb_crop_watch.expected;
    if (g_srgb_crop_watch.twins) {
        u32 formats = 0;
        for (const VkBaseInStructure* node = image->pNext; node; node = node->pNext)
            if (node->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) {
                const VkImageFormatListCreateInfo* list = (const void*)node;
                for (u32 i = 0; i < list->viewFormatCount; i++) {
                    if (list->pViewFormats[i] == VK_FORMAT_R8G8B8A8_UNORM) formats |= 1;
                    if (list->pViewFormats[i] == VK_FORMAT_R8G8B8A8_SRGB) formats |= 2;
                }
            }
        g_srgb_crop_watch.trace.wrong_lists += formats != 3 || !(image->flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT);
        if (image->format == VK_FORMAT_R8G8B8A8_SRGB && (image->usage & VK_IMAGE_USAGE_STORAGE_BIT))
            g_srgb_crop_watch.trace.wrong_lists += !(image->flags & VK_IMAGE_CREATE_EXTENDED_USAGE_BIT);
    }
    g_srgb_crop_watch.requirements(device, query, requirements);
}
static VKAPI_ATTR void VKAPI_CALL srgb_crop_copy_watch(VkCommandBuffer cb, VkImage source,
    VkImageLayout source_layout, VkImage destination, VkImageLayout destination_layout,
    u32 count, const VkImageCopy* regions) {
    if (source == g_srgb_crop_watch.source) g_srgb_crop_watch.trace.copies++;
    g_srgb_crop_watch.copy(cb, source, source_layout, destination, destination_layout, count, regions);
}
static void srgb_crop_watch_begin(VkImage source, VkFormat expected, int twins) {
    vk_rec_sync();
    g_srgb_crop_watch.requirements = vkGetDeviceImageMemoryRequirements;
    g_srgb_crop_watch.copy = vkCmdCopyImage;
    g_srgb_crop_watch.source = source;
    g_srgb_crop_watch.expected = expected;
    g_srgb_crop_watch.twins = twins;
    g_srgb_crop_watch.trace = (SrgbCropTrace){0};
    vkGetDeviceImageMemoryRequirements = srgb_crop_requirements_watch;
    vkCmdCopyImage = srgb_crop_copy_watch;
}
static SrgbCropTrace srgb_crop_watch_end(void) {
    vk_rec_sync();
    vkGetDeviceImageMemoryRequirements = g_srgb_crop_watch.requirements;
    vkCmdCopyImage = g_srgb_crop_watch.copy;
    return g_srgb_crop_watch.trace;
}
static int srgb_crop_trace(SrgbCropTrace trace, int created, u32 copies) {
    if (trace.wrong_formats)
        fprintf(stderr, "sRGB crop: native create-info format %d, expected %d (%u wrong queries)\n",
                (int)trace.first_format, (int)g_srgb_crop_watch.expected, trace.wrong_formats);
    CHECK(!trace.wrong_formats && !trace.wrong_lists);
    CHECK((trace.queries != 0) == created && trace.copies == copies);
    return 0;
}
/* One invocation samples every pixel at its center. Integer views return one packed word, linear views four
   float components. No production texture decoder or renderer conversion supplies the comparison values. */
static u32* srgb_crop_sample_program(u32 width, u32 height, int integer, size_t* words) {
    SpvEmitter e;
    spv_emit_init(&e, 512);
    spv_emit_shader_capability(&e);
    u32 function = spv_emit_id(&e), label = spv_emit_id(&e);
    u32 void_type = spv_emit_type_void(&e), u32_type = spv_emit_type_u32(&e), f32 = spv_emit_type_f32(&e);
    u32 vec2 = spv_emit_type_vec2(&e, f32), component = integer ? u32_type : f32;
    u32 vec4 = spv_emit_type_vec4(&e, component), function_type = spv_emit_type_function_void(&e, void_type);
    u32 array = spv_emit_type_runtime_array(&e, u32_type, 4), block = spv_emit_type_struct_one(&e, array);
    u32 sampled_type, interfaces[2];
    interfaces[0] = spv_emit_storage_buffer(&e, block, 0, 0);
    interfaces[1] = spv_emit_sampler2d(&e, component, 0, 1, 0, &sampled_type);
    u32 pointer = spv_emit_type_storage_pointer(&e, u32_type), zero = spv_emit_constant_u32(&e, u32_type, 0);
    u32 lod = spv_emit_constant_f32(&e, f32, 0);
    spv_emit_compute_entry_str(&e, function, "main", interfaces, 2);
    spv_emit_local_size(&e, function, 1, 1, 1);
    spv_emit_function_void(&e, void_type, function, function_type);
    spv_emit_label(&e, label);
    for (u32 y = 0; y < height; y++)
        for (u32 x = 0; x < width; x++) {
            u32 u = spv_emit_constant_f32(&e, f32, ((float)x + .5f) / (float)width);
            u32 v = spv_emit_constant_f32(&e, f32, ((float)y + .5f) / (float)height);
            u32 uv = spv_emit_constant_vec2(&e, vec2, u, v);
            u32 texel = spv_emit_sample2d_lod(&e, vec4, sampled_type, interfaces[1], uv, 0, lod);
            for (u32 c = 0; c < (integer ? 1u : 4u); c++) {
                u32 value = spv_emit_vec4_extract(&e, component, texel, c);
                if (!integer) value = spv_emit_bitcast(&e, u32_type, value);
                u32 at = spv_emit_constant_u32(&e, u32_type, (y * width + x) * (integer ? 1 : 4) + c);
                spv_emit_store_u32(&e, spv_emit_storage_access_u32(&e, pointer, interfaces[0], zero, at), value);
            }
        }
    spv_emit_return(&e);
    spv_emit_function_end(&e);
    u32* code = spv_emit_finish(&e, words);
    spv_emit_destroy(&e);
    return code;
}
static int srgb_crop_read(Surf* surface, VkImageView view, VkSampler sampler, int integer, u32* result) {
    vk_surf_use(vk_cmd(), surface, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    vk_finish();
    size_t words;
    u32* code = srgb_crop_sample_program(surface->w, surface->h, integer, &words);
    CHECK(code);
    int ran = run_compute(code, words, result, surface->w * surface->h * (integer ? 1 : 4), view, sampler);
    free(code);
    CHECK(!ran);
    return 0;
}
static int srgb_crop_sample(Surf* surface, const u8* expected, const u32* decoded, u32 stride, int srgb,
                            VkSampler sampler) {
    const VkComponentMapping identity = {0};
    int integer = surface->f.vk == VK_FORMAT_R32_UINT;
    VkImageView view = vk_tex_view(surface, VK_IMAGE_VIEW_TYPE_2D, srgb, &identity, 0);
    CHECK(view && surface->w <= 8 && surface->h <= 6);
    CHECK(vk_tex_view_format(surface, srgb) == (integer ? VK_FORMAT_R32_UINT
          : srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM));
    u32 result[8 * 6 * 4] = {0};
    CHECK(!srgb_crop_read(surface, view, sampler, integer, result));
    for (u32 y = 0; y < surface->h; y++)
        for (u32 x = 0; x < surface->w; x++) {
            const u8* pixel = expected + (y * stride + x) * 4;
            if (integer) {
                CHECK(result[y * surface->w + x] == snx_ld_le32(pixel));
                continue;
            }
            for (u32 c = 0; c < 4; c++) {
                u32 bits = result[(y * surface->w + x) * 4 + c];
                if (srgb) {
                    CHECK(decoded);
                    u32 wanted = decoded[(y * stride + x) * 4 + c];
                    if (bits != wanted)
                        fprintf(stderr, "sRGB crop sample %ux%u pixel(%u,%u) component=%u byte=%u "
                                "actual=%08x native source=%08x\n", surface->w, surface->h, x, y, c,
                                pixel[c], bits, wanted);
                    CHECK(bits == wanted);
                    continue;
                }
                float actual, wanted = (float)pixel[c] / 255.f;
                memcpy(&actual, &bits, sizeof actual);
                CHECK(fabsf(actual - wanted) <= .000002f);
                CHECK((u32)lroundf(actual * 255.f) == pixel[c]);
            }
        }
    return 0;
}
static int srgb_crop_upload(Surf* surface, const u8 data[8 * 6 * 4]) {
    VkBuffer buffer;
    u64 offset;
    u8* upload = vk_stage(8 * 6 * 4, vk_stage_align(), &buffer, &offset);
    CHECK(upload);
    memcpy(upload, data, 8 * 6 * 4);
    vk_render_end();
    vk_surf_use(vk_cmd(), surface, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy copy = {.bufferOffset = offset, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageExtent = {8, 6, 1}};
    rec_CmdCopyBufferToImage(vk_cmd(), buffer, surface->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    vk_surf_wrote(surface);
    vk_rt_drawn_full(surface);
    return 0;
}
static int check_srgb_crop(void) {
    const u64 gpu = 0xb50000000ull, cpu = HEAP_BASE + 0x1d000000;
    CHECK(!mem_map(cpu, 0x10000, MS_NORMAL, PERM_RW));
    gmmu_map(gpu, cpu, 0x10000);
    vk_lock();
    vk_finish();
    u32 validation = g_vk_validation_messages;
    srgb_crop_watch_begin(VK_NULL_HANDLE, VK_FORMAT_R8G8B8A8_SRGB, 1);
    Surf* source = vk_rt(gpu, 8, 6, 0xd6, 0x1000, 0, 32);
    SrgbCropTrace trace = srgb_crop_watch_end();
    CHECK(source && source->f.vk == VK_FORMAT_R8G8B8A8_SRGB);
    CHECK(!srgb_crop_trace(trace, 1, 0));
    if (source->storage_capable) CHECK(vk_tex_storage_view(source, VK_IMAGE_VIEW_TYPE_2D));
    VkSamplerCreateInfo sampler_info = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VkSampler sampler;
    CHECK(vk_sampler_create(g_vk.dev, &sampler_info, NULL, &sampler) == VK_SUCCESS);
    /* Use a direct native sRGB view as the decoded reference, not an assumed hardware EOTF precision. The crop
       and runtime views must return exactly these bits. Independent linear views must still return every byte. */
    VkImageViewUsageCreateInfo sample_usage = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
                                               .usage = VK_IMAGE_USAGE_SAMPLED_BIT};
    VkImageViewCreateInfo native_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .pNext = &sample_usage,
        .image = source->img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_SRGB,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView native_view;
    CHECK(vkCreateImageView(g_vk.dev, &native_info, NULL, &native_view) == VK_SUCCESS);
    NvnTexture header = {.gpu = gpu};
    header.d.target = NVN_TEXTURE_TARGET_2D;
    header.d.depth = header.d.levels = 1;
    Surf* kept[3] = {0};
    u32 copies = 0, byte_checks = 0;
    for (u32 phase = 0; phase < 2; phase++) {
        u8 expected[8 * 6 * 4];
        for (u32 y = 0; y < 6; y++)
            for (u32 x = 0; x < 8; x++) {
                u8* pixel = expected + (y * 8 + x) * 4;
                pixel[0] = (u8)((16 + 11 * x + 17 * y) ^ (phase ? 0x55 : 0));
                pixel[1] = (u8)((201 - 7 * x - 13 * y) ^ (phase ? 0x33 : 0));
                pixel[2] = (u8)((9 + 9 * x + 3 * y) ^ (phase ? 0x66 : 0));
                pixel[3] = (u8)((64 + 17 * x + 3 * y) ^ (phase ? 0x77 : 0));
            }
        u32 generation = source->wgen;
        CHECK(!srgb_crop_upload(source, expected) && source->wgen != generation);
        VkBuffer readback;
        u8* raw = vk_readback_buffer(sizeof expected, &readback);
        CHECK(raw);
        vk_surf_use(vk_cmd(), source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy read = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {8, 6, 1}};
        rec_CmdCopyImageToBuffer(vk_cmd(), source->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &read);
        vk_finish();
        vk_readback_ready();
        CHECK(!memcmp(raw, expected, sizeof expected));
        byte_checks += sizeof expected;
        u32 decoded[8 * 6 * 4] = {0};
        CHECK(!srgb_crop_read(source, native_view, sampler, 0, decoded));
        CHECK(!srgb_crop_sample(source, expected, NULL, 8, 0, sampler));
        CHECK(!srgb_crop_sample(source, expected, decoded, 8, 1, sampler));
        for (u32 shape = 0; shape < 3; shape++) {
            u32 tic[8];
            header.d.format = shape == 2 ? NVN_FORMAT_R32UI : NVN_FORMAT_RGBA8_SRGB;
            header.d.width = 4;
            header.d.height = 3;
            CHECK(nvn_tic_encode(&header, NULL, 0, 0, tic));
            srgb_crop_watch_begin(source->img, shape == 2 ? VK_FORMAT_R32_UINT : VK_FORMAT_R8G8B8A8_SRGB, shape != 2);
            Surf* copy = shape ? vk_tex_from_tic(tic) : vk_rt_snapshot(source);
            trace = srgb_crop_watch_end();
            CHECK(copy && copy != source && copy->crop_gen == source->wgen);
            CHECK(!srgb_crop_trace(trace, phase == 0, 1));
            CHECK(copy->f.vk == (shape == 2 ? VK_FORMAT_R32_UINT : VK_FORMAT_R8G8B8A8_SRGB));
            CHECK(copy->w == (shape ? 4u : 8u) && copy->h == (shape ? 3u : 6u));
            if (phase) CHECK(copy == kept[shape]);
            kept[shape] = copy;
            copies++;
            srgb_crop_watch_begin(source->img, shape == 2 ? VK_FORMAT_R32_UINT : VK_FORMAT_R8G8B8A8_SRGB, shape != 2);
            Surf* repeated = shape ? vk_tex_from_tic(tic) : vk_rt_snapshot(source);
            trace = srgb_crop_watch_end();
            CHECK(repeated == copy && !srgb_crop_trace(trace, 0, 0));
            CHECK(!srgb_crop_sample(copy, expected, NULL, 8, 0, sampler));
            if (shape != 2) CHECK(!srgb_crop_sample(copy, expected, decoded, 8, 1, sampler));
            byte_checks += copy->w * copy->h * 4;
        }
    }
    vkDestroyImageView(g_vk.dev, native_view, NULL);
    vk_sampler_destroy(g_vk.dev, sampler, NULL);
    vk_render_forget_state();
    CHECK(g_vk_validation_messages == validation);
    vk_unlock();
    printf("PASS sRGB crop backing: full/cropped reuse, generation refresh, sRGB views and R32UI copies=%u checked=%u exact=1\n",
           copies, byte_checks);
    return 0;
}

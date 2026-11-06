/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Interpret the emitted reduction shader with independent CPU image values. These are not Vulkan descriptor
   encodings. Invalid samples contain distinct colors, so selecting sample0 or averaging the colors cannot pass. */
#include "nx/gpu/shader/coverage_spirv.h"
#include "nx/gpu/vk/coverage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static struct { unsigned checks, failures; } g_test;
static void check(int pass, const char* text) {
    g_test.checks++;
    if (!pass && g_test.failures++ < 12) fprintf(stderr, "FAIL mixed coverage: %s\n", text);
}
static uint32_t word(float f) { uint32_t u; memcpy(&u, &f, sizeof u); return u; }
static float real(uint32_t u) { float f; memcpy(&f, &u, sizeof f); return f; }
typedef struct { uint32_t v[4]; } Value;
typedef struct {
    uint32_t* code;
    size_t words;
    Value* value;
    uint32_t *binding, *label;
    uint32_t fragcoord, output, reads[2], writes, kills;
} Module;
static Module module(uint32_t samples, uint32_t components, const uint8_t* factors) {
    Module m = {0};
    m.code = shader_coverage_fragment_spirv(samples, components, factors, &m.words);
    if (!m.code || m.words < 5) return m;
    uint32_t bound = m.code[3];
    m.value = calloc(bound, sizeof *m.value + 2 * sizeof *m.binding);
    if (!m.value) exit(1);
    m.binding = (uint32_t*)(m.value + bound);
    m.label = m.binding + bound;
    uint32_t previous = 0;
    for (size_t at = 5; at < m.words;) {
        const uint32_t* op = m.code + at;
        uint32_t n = op[0] >> 16, kind = op[0] & 0xffff;
        if (!n || n > m.words - at) { check(0, "well-formed instruction length"); break; }
        if (previous == SPV_OP_SELECTION_MERGE)
            check(kind == SPV_OP_BRANCH_CONDITIONAL || kind == SPV_OP_SWITCH,
                  "selection merge immediately precedes its conditional terminator");
        if (kind == SPV_OP_CONSTANT) m.value[op[2]].v[0] = op[3];
        if (kind == SPV_OP_CONSTANT_TRUE) m.value[op[2]].v[0] = 1;
        if (kind == SPV_OP_CONSTANT_FALSE) m.value[op[2]].v[0] = 0;
        if (kind == SPV_OP_DECORATE && n == 4 && op[2] == SPV_DECORATION_BINDING) m.binding[op[1]] = op[3] + 1;
        if (kind == SPV_OP_DECORATE && n == 4 && op[2] == SPV_DECORATION_BUILT_IN && op[3] == 15) m.fragcoord = op[1];
        if (kind == SPV_OP_VARIABLE && op[3] == SPV_STORAGE_OUTPUT) m.output = op[2];
        if (kind == SPV_OP_LABEL) m.label[op[1]] = (uint32_t)(at + n);
        if (kind == SPV_OP_IMAGE_READ) {
            check(n == 7 && op[5] == 64, "each captured image uses an explicit multisample read");
            m.reads[0]++;
        }
        if (kind == SPV_OP_IMAGE_WRITE) m.writes++;
        if (kind == SPV_OP_KILL) m.kills++;
        previous = kind;
        at += n;
    }
    check(m.fragcoord && m.output && m.reads[0] == 2 * samples && !m.writes && m.kills == 1,
          "reduction reads every source/validity sample, discards empty pixels and writes no storage images");
    return m;
}
static int evaluate(Module* m, uint32_t samples, uint32_t mask, const float source[4], uint32_t output[4]) {
    if (!m->code || !m->value || !m->fragcoord || !m->output) return -1;
    Value* v = m->value;
    for (size_t at = 5, steps = 0; at < m->words && steps++ < m->words;) {
        const uint32_t* op = m->code + at;
        uint32_t n = op[0] >> 16, kind = op[0] & 0xffff;
        if (!n || n > m->words - at) return -1;
        switch (kind) {
            case SPV_OP_LOAD:
                if (op[3] == m->fragcoord) v[op[2]] = (Value){{word(1.5f), word(2.5f), 0, word(1)}};
                else if (m->binding[op[3]]) v[op[2]].v[0] = m->binding[op[3]];
                else return -1;
                break;
            case SPV_OP_IMAGE_READ: {
                uint32_t image = v[op[3]].v[0], sample = v[op[6]].v[0];
                if ((image != 1 && image != 2) || sample >= samples || n != 7 || op[5] != 64) return -1;
                int covered = !!(mask & (1u << sample));
                for (uint32_t c = 0; c < 4; c++)
                    v[op[2]].v[c] = word(image == 2 ? (covered ? 1.0f : 0.0f)
                                                              : covered ? source[c] : (float)(100 + 4 * sample + c));
                break;
            }
            case SPV_OP_COMPOSITE_CONSTRUCT:
                for (uint32_t i = 3; i < n; i++) v[op[2]].v[i - 3] = v[op[i]].v[0];
                break;
            case SPV_OP_COMPOSITE_EXTRACT: v[op[2]].v[0] = v[op[3]].v[op[4]]; break;
            case SPV_OP_CONVERT_F_TO_U: {
                float f = real(v[op[3]].v[0]);
                if (!isfinite(f) || f < 0 || f > 4) return -1;
                v[op[2]].v[0] = (uint32_t)f;
                break;
            }
            case SPV_OP_CONVERT_U_TO_F: v[op[2]].v[0] = word((float)v[op[3]].v[0]); break;
            case SPV_OP_F_ORD_EQUAL: v[op[2]].v[0] = real(v[op[3]].v[0]) == real(v[op[4]].v[0]); break;
            case SPV_OP_I_EQUAL: v[op[2]].v[0] = v[op[3]].v[0] == v[op[4]].v[0]; break;
            case SPV_OP_LOGICAL_NOT: v[op[2]].v[0] = !v[op[3]].v[0]; break;
            case SPV_OP_LOGICAL_AND: v[op[2]].v[0] = v[op[3]].v[0] && v[op[4]].v[0]; break;
            case SPV_OP_SELECT: v[op[2]].v[0] = v[v[op[3]].v[0] ? op[4] : op[5]].v[0]; break;
            case SPV_OP_I_ADD: v[op[2]].v[0] = v[op[3]].v[0] + v[op[4]].v[0]; break;
            case SPV_OP_F_MUL: v[op[2]].v[0] = word(real(v[op[3]].v[0]) * real(v[op[4]].v[0])); break;
            case SPV_OP_BRANCH_CONDITIONAL:
                at = m->label[v[op[1]].v[0] ? op[2] : op[3]];
                if (!at) return -1;
                continue;
            case SPV_OP_KILL: return 0;
            case SPV_OP_STORE:
                if (op[1] != m->output) return -1;
                memcpy(output, v[op[2]].v, 4 * sizeof *output);
                return 1;
            default: break;
        }
        at += n;
    }
    return -1;
}
static void release(Module* m) { free(m->code); free(m->value); }
int main(int argc, char** argv) {
    const float source[4] = {1, 0.5f, 0.25f, 0.75f};
    for (uint32_t samples = 2; samples <= 16; samples *= 2)
        for (uint32_t mode = 0; mode < 4; mode++)
            for (uint32_t custom = 0; custom < 2; custom++) {
                uint8_t table[16], compact[16];
                for (uint32_t i = 0; i < 16; i++) table[i] = (uint8_t)((5 * i + 3) % 17);
                for (uint32_t i = 0; i < samples; i++) compact[i] = table[i * (16 / samples)];
                const uint32_t components[] = {0, 7, 8, 15};
                Module m = module(samples, components[mode], custom ? compact : NULL);
                uint32_t cases = samples <= 8 ? 1u << samples : 2048;
                for (uint32_t test = 0; test < cases; test++) {
                    uint32_t mask = samples <= 8 ? test : test < 17 ? (1u << test) - 1 : (test * 0x9e37u) & 65535;
                    if (samples == 16 && test == cases - 1) mask = 1u << (samples - 1); /* no sample0 survives */
                    uint32_t covered = 0;
                    for (uint32_t i = 0; i < samples; i++) if (mask & (1u << i)) covered++;
                    uint32_t actual[4] = {0};
                    int result = evaluate(&m, samples, mask, source, actual);
                    check(result == (covered ? 1 : 0), "zero coverage discards; any surviving sample writes exactly once");
                    if (!covered || result != 1) continue;
                    float factor = custom ? (float)table[(covered - 1) * (16 / samples)] / 16.0f
                                          : (float)covered / (float)samples;
                    for (uint32_t c = 0; c < 4; c++) {
                        int modulated = c == 3 ? !!(mode & 2) : !!(mode & 1);
                        check(actual[c] == word(source[c] * (modulated ? factor : 1)),
                              "independent covered-count/table/component oracle matches emitted pixel bits");
                    }
                }
                if (m.code && argc == 2) {
                    char path[4096];
                    int n = snprintf(path, sizeof path, "%s/mixed-coverage-%u-%u-%u.spv", argv[1], samples, mode, custom);
                    FILE* file = n < 0 || n >= (int)sizeof path ? NULL : fopen(path, "wb");
                    check(file && fwrite(m.code, 4, m.words, file) == m.words, "save reduction module for spirv-val");
                    if (file) check(!fclose(file), "close reduction module");
                }
                release(&m);
            }
    size_t words = 7;
    uint8_t invalid[16] = {17};
    check(!shader_coverage_fragment_spirv(3, 15, NULL, &words) && !words, "invalid raster count is rejected");
    check(!shader_coverage_fragment_spirv(4, 16, NULL, &words) && !words, "invalid components are rejected");
    check(!shader_coverage_fragment_spirv(4, 15, invalid, &words) && !words, "invalid coefficient is rejected");
    uint32_t* vertex = shader_coverage_vertex_spirv(&words);
    check(vertex && words >= 5, "fullscreen vertex module exists");
    if (vertex && argc == 2) {
        char path[4096];
        int n = snprintf(path, sizeof path, "%s/mixed-coverage-vertex.spv", argv[1]);
        FILE* file = n < 0 || n >= (int)sizeof path ? NULL : fopen(path, "wb");
        check(file && fwrite(vertex, 4, words, file) == words, "save vertex module for spirv-val");
        if (file) check(!fclose(file), "close vertex module");
    }
    free(vertex);
    DrawCall call = {.raster_samples = 4, .coverage_mode = 3};
    VkCoverage key;
    check(vk_coverage_portable_key(&key, &call, 1, 1, 0, 1, 31), "portable key admits proved color-only one-sample state");
    check(!vk_coverage_portable_key(&key, &call, 1, 1, 0, 0, 31), "sample-dependent sources require a different path");
    check(!vk_coverage_portable_key(&key, &call, 2, 1, 0, 1, 31), "multiple stored color samples remain gated");
    check(!vk_coverage_portable_key(&key, &call, 1, 2, 0, 1, 31), "multiple attachments remain gated");
    check(!vk_coverage_portable_key(&key, &call, 1, 1, 1, 1, 31), "depth attachments remain gated");
    check(!vk_coverage_portable_key(&key, &call, 1, 1, 0, 1, 1), "unsupported raster counts remain gated");
    printf("mixed coverage shader: %u checks, %u failures\n", g_test.checks, g_test.failures);
    return g_test.failures != 0;
}

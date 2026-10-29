/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SceNgs, the console's voice synthesizer. The system, its racks and their voices live in the memory the game gives
   them: a voice's handle is the address of its part of its rack's buffer, which holds its modules' parameters (a
   room each, which sceNgsVoiceLockParams hands the game) and its patches' handles. What a voice does is kept here.
   Each sceNgsSystemUpdate runs a granule (the system's granularity, in frames at 48 kHz): every simple voice playing
   reads its player's buffers (16-bit PCM or HE-ADPCM), resampled to the system's rate, and mixes them through its
   patches' volume matrices into the voices they lead to; a master buss's output module then holds the sum as 16-bit
   stereo, the state data the game sends to its audio port. A player's buffers loop and chain as their loop counts
   and next indices say, and the voice is available again once it runs out of them. The equalizer, envelope, pauser
   and filter modules pass the sound unchanged. As the vitasdk headers (MIT) declare, and as Vita3K's ngs/ (GPL-2.0-or-
   later) models them: modules/SceNgsUser/SceNgs.cpp, ngs/src/ngs.cpp, scheduler.cpp, route.cpp, modules/player.cpp
   and codec/src/pcm.cpp (HE-ADPCM, from vgmstream's HEVAG decoder) */
#include "vita/hle/hle.h"
#include "common/utilities/endian.h"
#include "common/utilities/bits.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCE_NGS_ERROR               0x804a0001u
#define SCE_NGS_ERROR_INVALID_PARAM 0x804a0002u
#define SCE_NGS_ERROR_PAUSED        0x804a0010u
#define RACK_HEADER   0x40u  /* the rack's voice count, then its voices */
#define VOICE_SIZE    0x400u /* a voice's handle word, its modules' parameters, its patches' handles */
#define VOICE_PARAMS  0x40u
#define MODULE_PARAMS 0x60u  /* a module's room for its parameters, the player's 0x54 bytes the largest */
#define MODULES       6u     /* a simple voice's: player, equalizer, envelope, pauser, two filters */
#define PATCH_AT      0x300u /* the patches' handles: PATCHES words an output */
#define PATCHES       4u
#define OUTPUTS       2u
#define RACKS         16u
#define VOICES        512u
#define MOST_GRANULARITY 512u
#define PLAYER_ID     0x01015ce6u /* SCE_NGS_PLAYER_PARAMS_STRUCT_ID */

/* SceNgsVoiceState's flags */
enum { AVAILABLE = 0, ACTIVE = 1 };
/* the voice definitions sceNgsVoiceDefGet* return, at the runtime's page a word apart: the game only hands them back */
enum { DEF_MASTER, DEF_SIMPLE, DEF_REVERB, DEFS };
#define DEFINITIONS (VITA_RUNTIME_PAGE + 0x800u)

typedef struct {
    bool used;
    uint32_t destination;   /* the voice's handle */
    float matrix[2][2];     /* [source channel][destination channel] */
} Patch;
typedef struct {
    uint32_t handle;
    unsigned rack;
    uint32_t state, updates;
    bool paused;
    /* the player's SceNgsPlayerStates: the byte in its buffer, the buffer, the samples and bytes since its key on,
       and in all; its current buffer's loops so far, and the resampler's place between its last two frames */
    uint32_t player[6], loops;
    float phase, last[2], next[2];
    /* an HE-ADPCM player's frame decoded (28 samples a channel, interleaved), the next of them, and each channel's
       last four samples, which the next frame predicts from */
    int16_t decoded[28 * 2];
    unsigned decoded_at, decoded_count;
    int32_t history[2][4];
    Patch patches[OUTPUTS][PATCHES];
    float input[MOST_GRANULARITY * 2];          /* what the voices patched to it give it this granule */
    unsigned char output[MOST_GRANULARITY * 4]; /* a master buss's: 16-bit stereo */
} Voice;
typedef struct {
    uint32_t address, count, channels, kind;
    unsigned first; /* its voices' index in VitaNgs.voices */
} Rack;
struct VitaNgs {
    uint32_t granularity, rate;
    Rack racks[RACKS];
    Voice voices[VOICES];
    bool logged_type;
};

static uint32_t arg(VitaProcess* p, VitaThread* t, unsigned n) { return vita_arg(p, t, n); }
static uint32_t ok(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return 0;
}
static float ld_float(const unsigned char* at) {
    uint32_t bits = snx_ld_le32(at);
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}
static struct VitaNgs* ngs(VitaProcess* p) {
    if (!p->ngs) p->ngs = calloc(1, sizeof *p->ngs);
    return p->ngs;
}
void vita_ngs_free(VitaProcess* p) {
    free(p->ngs);
    p->ngs = NULL;
}
static Voice* voice_of(VitaProcess* p, uint32_t handle) {
    struct VitaNgs* n = p->ngs;
    if (!n) return NULL;
    for (unsigned i = 0; i < RACKS; ++i) {
        const Rack* r = &n->racks[i];
        uint32_t at = handle - r->address - RACK_HEADER;
        if (r->count && handle >= r->address + RACK_HEADER && at / VOICE_SIZE < r->count && !(at % VOICE_SIZE))
            return &n->voices[r->first + at / VOICE_SIZE];
    }
    return NULL;
}
/* the patch a handle names, from the voice whose part of its rack holds it */
static Patch* patch_of(VitaProcess* p, uint32_t handle) {
    struct VitaNgs* n = p->ngs;
    if (!n) return NULL;
    for (unsigned i = 0; i < RACKS; ++i) {
        const Rack* r = &n->racks[i];
        if (!r->count || handle < r->address + RACK_HEADER) continue;
        uint32_t offset = handle - r->address - RACK_HEADER, index = offset / VOICE_SIZE, in = offset % VOICE_SIZE;
        if (index >= r->count || in < PATCH_AT || in >= PATCH_AT + OUTPUTS * PATCHES * 4 || in & 3) continue;
        in = (in - PATCH_AT) / 4;
        return &n->voices[r->first + index].patches[in / PATCHES][in % PATCHES];
    }
    return NULL;
}
static unsigned modules_of(const struct VitaNgs* n, const Voice* v) {
    return n->racks[v->rack].kind == DEF_SIMPLE ? MODULES : 2;
}

/* ---- the system and its racks ---- */
/* sceNgsSystemGetRequiredMemorySize(params, size*) */
static uint32_t system_get_required_memory_size(VitaProcess* p, VitaThread* t) {
    if (!arg(p, t, 1)) return SCE_NGS_ERROR_INVALID_PARAM;
    vita_write32(p, arg(p, t, 1), 0x1000);
    return 0;
}
/* sceNgsSystemInit(memory, size, SceNgsSystemInitParams*, handle*): the memory is the handle. The parameters: the most
   racks and voices, the granularity (frames an update, a multiple of 64 up to 512), the sample rate */
static uint32_t system_init(VitaProcess* p, VitaThread* t) {
    uint32_t params = arg(p, t, 2);
    if (!arg(p, t, 0) || !arg(p, t, 3) || !params) return SCE_NGS_ERROR_INVALID_PARAM;
    uint32_t granularity = vita_read32(p, params + 8), rate = vita_read32(p, params + 12);
    if (!granularity || granularity % 64 || granularity > MOST_GRANULARITY || !rate) return SCE_NGS_ERROR_INVALID_PARAM;
    struct VitaNgs* n = ngs(p);
    if (!n) return SCE_NGS_ERROR;
    memset(n, 0, sizeof *n);
    n->granularity = granularity, n->rate = rate;
    vita_write32(p, arg(p, t, 3), arg(p, t, 0));
    return 0;
}
static uint32_t system_release(VitaProcess* p, VitaThread* t) {
    (void)t;
    if (p->ngs) memset(p->ngs, 0, sizeof *p->ngs);
    return 0;
}
/* sceNgsRackGetRequiredMemorySize(system, SceNgsRackDescription*, size*): room for each voice the description names.
   The description: the voice definition, the voices, their channels, the patches an input and an output takes */
static uint32_t rack_get_required_memory_size(VitaProcess* p, VitaThread* t) {
    uint32_t description = arg(p, t, 1), out = arg(p, t, 2);
    if (!description || !out) return SCE_NGS_ERROR_INVALID_PARAM;
    int32_t voices = (int32_t)vita_read32(p, description + 4);
    if (voices <= 0 || voices > 0x10000) return SCE_NGS_ERROR_INVALID_PARAM;
    vita_write32(p, out, RACK_HEADER + (uint32_t)voices * VOICE_SIZE);
    return 0;
}
/* sceNgsRackInit(system, SceNgsBufferInfo*, description, rack*): its voices from the first free run of them */
static uint32_t rack_init(VitaProcess* p, VitaThread* t) {
    uint32_t buffer = arg(p, t, 1), description = arg(p, t, 2), out = arg(p, t, 3);
    struct VitaNgs* n = p->ngs;
    if (!n || !buffer || !description || !out) return SCE_NGS_ERROR_INVALID_PARAM;
    uint32_t rack = vita_read32(p, buffer), size = vita_read32(p, buffer + 4);
    uint32_t definition = vita_read32(p, description), voices = vita_read32(p, description + 4);
    uint32_t kind = (definition - DEFINITIONS) / 4;
    if (!rack || !voices || voices > VOICES || size < RACK_HEADER + voices * VOICE_SIZE ||
        !vita_mapped(p, rack, size) || definition < DEFINITIONS || kind >= DEFS)
        return SCE_NGS_ERROR_INVALID_PARAM;
    Rack* slot = NULL;
    for (unsigned i = 0; i < RACKS && !slot; ++i)
        if (!n->racks[i].count) slot = &n->racks[i];
    /* the voices after the last rack's that leave room: racks are few and live as long as the system */
    unsigned first = 0;
    for (unsigned i = 0; i < RACKS; ++i)
        if (n->racks[i].count && n->racks[i].first + n->racks[i].count > first)
            first = n->racks[i].first + n->racks[i].count;
    if (!slot || first + voices > VOICES) return SCE_NGS_ERROR;
    memset(vita_ptr(p, rack), 0, RACK_HEADER + voices * VOICE_SIZE);
    vita_write32(p, rack, voices);
    *slot = (Rack){rack, voices, vita_read32(p, description + 8), kind, first};
    for (uint32_t i = 0; i < voices; ++i) {
        Voice* v = &n->voices[first + i];
        memset(v, 0, sizeof *v);
        v->handle = rack + RACK_HEADER + i * VOICE_SIZE, v->rack = (unsigned)(slot - n->racks);
    }
    vita_write32(p, out, rack);
    return 0;
}
static uint32_t rack_release(VitaProcess* p, VitaThread* t) {
    uint32_t rack = arg(p, t, 0);
    struct VitaNgs* n = p->ngs;
    for (unsigned i = 0; n && i < RACKS; ++i)
        if (n->racks[i].count && n->racks[i].address == rack) n->racks[i] = (Rack){0};
    return 0;
}
/* sceNgsRackGetVoiceHandle(rack, index, voice*) */
static uint32_t rack_get_voice_handle(VitaProcess* p, VitaThread* t) {
    uint32_t rack = arg(p, t, 0), index = arg(p, t, 1), out = arg(p, t, 2);
    if (!rack || !out || index >= vita_read32(p, rack)) return SCE_NGS_ERROR_INVALID_PARAM;
    vita_write32(p, out, rack + RACK_HEADER + index * VOICE_SIZE);
    return 0;
}
static uint32_t master_buss(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return DEFINITIONS + DEF_MASTER * 4;
}
static uint32_t simple_voice(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return DEFINITIONS + DEF_SIMPLE * 4;
}
static uint32_t reverb_buss(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return DEFINITIONS + DEF_REVERB * 4;
}

/* ---- parameters ---- */
static uint32_t module_params(uint32_t voice, uint32_t module) { return voice + VOICE_PARAMS + module * MODULE_PARAMS; }
/* sceNgsVoiceLockParams(voice, module, struct ID, SceNgsBufferInfo*): the module's room, its descriptor naming the
   ID */
static uint32_t voice_lock_params(VitaProcess* p, VitaThread* t) {
    uint32_t voice = arg(p, t, 0), module = arg(p, t, 1), out = arg(p, t, 3);
    Voice* v = voice_of(p, voice);
    if (!v || !out || module >= modules_of(p->ngs, v)) return SCE_NGS_ERROR_INVALID_PARAM;
    vita_write32(p, module_params(voice, module), arg(p, t, 2));
    vita_write32(p, out, module_params(voice, module));
    vita_write32(p, out + 4, MODULE_PARAMS);
    return 0;
}
/* sceNgsVoiceSetParamsBlock(voice, block, size, error count*): records of a SceNgsModuleParamHeader (the module, its
   channel or -1) and the module's parameters, whose descriptor (an ID and their size) starts them, each into its
   module's room; the records that name no module, or too much for it, counted as errors */
static uint32_t voice_set_params_block(VitaProcess* p, VitaThread* t) {
    uint32_t voice = arg(p, t, 0), block = arg(p, t, 1), size = arg(p, t, 2), errors_out = arg(p, t, 3);
    Voice* v = voice_of(p, voice);
    if (!v || !block || !vita_mapped(p, block, size)) return SCE_NGS_ERROR_INVALID_PARAM;
    uint32_t errors = 0;
    for (uint32_t at = 0; at + 16 <= size;) {
        uint32_t module = vita_read32(p, block + at), bytes = vita_read32(p, block + at + 12);
        if (bytes < 8 || bytes > size - at - 8) {
            ++errors;
            break;
        }
        if (module < modules_of(p->ngs, v) && bytes <= MODULE_PARAMS)
            memmove(vita_ptr(p, module_params(voice, module)), vita_ptr(p, block + at + 8), bytes);
        else
            ++errors;
        at += 8 + bytes;
    }
    if (errors_out) vita_write32(p, errors_out, errors);
    return errors ? SCE_NGS_ERROR : 0;
}

/* ---- voices ---- */
/* sceNgsVoicePlay(voice): a player starts at its start buffer and byte */
static uint32_t voice_play(VitaProcess* p, VitaThread* t) {
    uint32_t voice = arg(p, t, 0);
    Voice* v = voice_of(p, voice);
    if (!v) return SCE_NGS_ERROR_INVALID_PARAM;
    if (v->state != AVAILABLE) return SCE_NGS_ERROR;
    v->state = ACTIVE;
    memset(v->player, 0, sizeof v->player);
    const unsigned char* player = vita_ptr(p, module_params(voice, 0));
    v->player[0] = snx_ld_le32(player + 0x48), v->player[1] = (uint32_t)(int32_t)(int8_t)player[0x51];
    v->loops = 0, v->phase = 1.0f;
    v->last[0] = v->last[1] = v->next[0] = v->next[1] = 0.0f;
    v->decoded_at = v->decoded_count = 0;
    memset(v->history, 0, sizeof v->history);
    return 0;
}
static uint32_t voice_pause(VitaProcess* p, VitaThread* t) {
    Voice* v = voice_of(p, arg(p, t, 0));
    if (!v) return SCE_NGS_ERROR_INVALID_PARAM;
    if (v->paused) return SCE_NGS_ERROR_PAUSED;
    v->paused = true;
    return 0;
}
static uint32_t voice_resume(VitaProcess* p, VitaThread* t) {
    Voice* v = voice_of(p, arg(p, t, 0));
    if (!v) return SCE_NGS_ERROR_INVALID_PARAM;
    v->paused = false;
    return 0;
}
static uint32_t voice_kill(VitaProcess* p, VitaThread* t) {
    Voice* v = voice_of(p, arg(p, t, 0));
    if (!v) return SCE_NGS_ERROR_INVALID_PARAM;
    v->state = AVAILABLE, v->paused = false;
    return 0;
}
/* sceNgsVoiceGetInfo(voice, SceNgsVoiceInfo*): its state (ACTIVE, PAUSED 0x20), its modules, inputs and outputs, the
   patches an output takes, the updates it ran in */
static uint32_t voice_get_info(VitaProcess* p, VitaThread* t) {
    uint32_t out = arg(p, t, 1);
    Voice* v = voice_of(p, arg(p, t, 0));
    if (!v || !out) return SCE_NGS_ERROR_INVALID_PARAM;
    uint32_t kind = p->ngs->racks[v->rack].kind;
    vita_write32(p, out, v->state | (v->paused ? 0x20u : 0));
    vita_write32(p, out + 4, modules_of(p->ngs, v));
    vita_write32(p, out + 8, 1);
    vita_write32(p, out + 12, kind == DEF_MASTER ? 1 : OUTPUTS);
    vita_write32(p, out + 16, PATCHES);
    vita_write32(p, out + 20, v->updates);
    return 0;
}
/* sceNgsVoiceGetStateData(voice, module, memory, size): a player's SceNgsPlayerStates, a master buss's output (its
   16-bit stereo of the last update); zeros for the others */
static uint32_t voice_get_state_data(VitaProcess* p, VitaThread* t) {
    uint32_t module = arg(p, t, 1), memory = arg(p, t, 2), size = arg(p, t, 3);
    Voice* v = voice_of(p, arg(p, t, 0));
    if (!v || !memory || !vita_mapped(p, memory, size)) return SCE_NGS_ERROR_INVALID_PARAM;
    unsigned char* to = vita_ptr(p, memory);
    memset(to, 0, size);
    uint32_t kind = p->ngs->racks[v->rack].kind;
    if (kind == DEF_SIMPLE && module == 0)
        for (unsigned i = 0; i < 6 && i * 4 + 4 <= size; ++i) snx_st_le32(to + i * 4, v->player[i]);
    else if (kind == DEF_MASTER && module == 1)
        memcpy(to, v->output, snx_min_u32(size, p->ngs->granularity * 4));
    return 0;
}

/* ---- patches ---- */
/* sceNgsPatchCreateRouting(SceNgsPatchSetupInfo*, patch*): the source voice, its output and the patch's index there
   (-1: the first free), the destination voice and its input; the patch's volumes all zero */
static uint32_t patch_create_routing(VitaProcess* p, VitaThread* t) {
    uint32_t info = arg(p, t, 0), out = arg(p, t, 1);
    if (!info || !out) return SCE_NGS_ERROR_INVALID_PARAM;
    uint32_t source = vita_read32(p, info), output = vita_read32(p, info + 4), index = vita_read32(p, info + 8);
    uint32_t destination = vita_read32(p, info + 12);
    Voice* v = voice_of(p, source);
    if (!v || !voice_of(p, destination) || output >= OUTPUTS) return SCE_NGS_ERROR_INVALID_PARAM;
    if (index == UINT32_MAX)
        for (index = 0; index < PATCHES && v->patches[output][index].used; ++index) continue;
    if (index >= PATCHES) return SCE_NGS_ERROR;
    v->patches[output][index] = (Patch){true, destination, {{0}}};
    vita_write32(p, out, source + PATCH_AT + (output * PATCHES + index) * 4);
    return 0;
}
static uint32_t patch_remove_routing(VitaProcess* p, VitaThread* t) {
    Patch* patch = patch_of(p, arg(p, t, 0));
    if (!patch) return SCE_NGS_ERROR_INVALID_PARAM;
    patch->used = false;
    return 0;
}
/* sceNgsVoicePatchSetVolumesMatrix(patch, SceNgsVolumeMatrix*): the volume each source channel gives each destination
   channel, [source][destination] */
static uint32_t patch_set_volumes_matrix(VitaProcess* p, VitaThread* t) {
    Patch* patch = patch_of(p, arg(p, t, 0));
    uint32_t matrix = arg(p, t, 1);
    if (!patch || !matrix || !vita_mapped(p, matrix, 16)) return SCE_NGS_ERROR_INVALID_PARAM;
    const unsigned char* m = vita_ptr(p, matrix);
    for (unsigned i = 0; i < 4; ++i) patch->matrix[i / 2][i % 2] = ld_float(m + i * 4);
    return 0;
}

/* ---- the update ---- */
/* HE-ADPCM (Sony's HEVAG): frames of 16 bytes a channel, the channels' in turn, each a predictor (byte 0's high
   nibble and byte 1's, 0 past the table), a shift (byte 0's low nibble), a flag (byte 1's low nibble: 7 and above
   silence) and 28 4-bit residuals, low nibble first. A sample is its residual scaled by 2^(20 - shift) plus the four
   before it weighted by the predictor's coefficients over 32, all over 256 */
static const int16_t hevag_coefficients[128][4] = {
    {0, 0, 0, 0}, {7680, 0, 0, 0}, {14720, -6656, 0, 0}, {12544, -7040, 0, 0}, {15616, -7680, 0, 0},
    {14731, -7059, 0, 0}, {14507, -7366, 0, 0}, {13920, -7522, 0, 0}, {13133, -7680, 0, 0}, {12028, -7680, 0, 0},
    {10764, -7680, 0, 0}, {9359, -7680, 0, 0}, {7832, -7680, 0, 0}, {6201, -7680, 0, 0}, {4488, -7680, 0, 0},
    {2717, -7680, 0, 0}, {910, -7680, 0, 0}, {-910, -7680, 0, 0}, {-2717, -7680, 0, 0}, {-4488, -7680, 0, 0},
    {-6201, -7680, 0, 0}, {-7832, -7680, 0, 0}, {-9359, -7680, 0, 0}, {-10764, -7680, 0, 0}, {-12028, -7680, 0, 0},
    {-13133, -7680, 0, 0}, {-13920, -7522, 0, 0}, {-14507, -7366, 0, 0}, {-14731, -7059, 0, 0},
    {5376, -9216, 3328, -3072}, {-6400, -7168, -3328, -2304}, {-10496, -7424, -3584, -1024},
    {-167, -2722, -494, -541}, {-7430, -2221, -2298, 424}, {-8001, -3166, -2814, 289}, {6018, -4750, 2649, -1298},
    {3798, -6946, 3875, -1216}, {-8237, -2596, -2071, 227}, {9199, 1982, -1382, -2316}, {13021, -3044, -3792, 1267},
    {13112, -4487, -2250, 1665}, {-1668, -3744, -6456, 840}, {7819, -4328, 2111, -506}, {9571, -1336, -757, 487},
    {10032, -2562, 300, 199}, {-4745, -4122, -5486, -1493}, {-5896, 2378, -4787, -6947}, {-1193, -9117, -1237, -3114},
    {2783, -7108, -1575, -1447}, {-7334, -2062, -2212, 446}, {6127, -2577, -315, -18}, {9457, -1858, 102, 258},
    {7876, -4483, 2126, -538}, {-7172, -1795, -2069, 482}, {-7358, -2102, -2233, 440}, {-9170, -3509, -2674, -391},
    {-2638, -2647, -1929, -1637}, {1873, 9183, 1860, -5746}, {9214, 1859, -1124, -2427}, {13204, -3012, -4139, 1370},
    {12437, -4792, -256, 622}, {-2653, -1144, -3182, -6878}, {9331, -1048, -828, 507}, {1642, -620, -946, -4229},
    {4246, -7585, -533, -2259}, {-8988, -3891, -2807, 44}, {-2562, -2735, -1730, -1899}, {3182, -483, -714, -1421},
    {7937, -3844, 2821, -1019}, {10069, -2609, 314, 195}, {8400, -3297, 1551, -155}, {-8529, -2775, -2432, -336},
    {9477, -1882, 108, 256}, {75, -2241, -298, -6937}, {-9143, -4160, -2963, 5}, {-7270, -1958, -2156, 460},
    {-2740, 3745, 5936, -1089}, {8993, 1948, -683, -2704}, {13101, -2835, -3854, 1055}, {9543, -1961, 130, 250},
    {5272, -4270, 3124, -3157}, {-7696, -3383, -2907, -456}, {7309, 2523, 434, -2461}, {10275, -2867, 391, 172},
    {10940, -3721, 665, 97}, {24, -310, -1262, 320}, {-8122, -2411, -2311, -271}, {-8511, -3067, -2337, 163},
    {326, -3846, 419, -933}, {8895, 2194, -541, -2880}, {12073, -1876, -2017, -601}, {8729, -3423, 1674, -169},
    {12950, -3847, -3007, 1946}, {10038, -2570, 302, 198}, {9385, -2757, 1008, 41}, {-4720, -5006, -2852, -1161},
    {7869, -4326, 2135, -501}, {2450, -8597, 1299, -2780}, {10192, -2763, 360, 181}, {11313, -4213, 833, 53},
    {10154, -2716, 345, 185}, {9638, -1417, -737, 482}, {3854, -4554, 2843, -3397}, {6699, -5659, 2249, -1074},
    {11082, -3908, 728, 80}, {-1026, -9810, -805, -3462}, {10396, -3746, 1367, -96}, {10287, 988, -1915, -1437},
    {7953, 3878, -764, -3263}, {12689, -3375, -3354, 2079}, {6641, 3166, 231, -2089}, {-2348, -7354, -1944, -4122},
    {9290, -4039, 1885, -246}, {4633, -6403, 1748, -1619}, {11247, -4125, 802, 61}, {9807, -2284, 219, 222},
    {9736, -1536, -706, 473}, {8440, -3436, 1562, -176}, {9307, -1021, -835, 509}, {1698, -9025, 688, -3037},
    {10214, -2791, 368, 179}, {8390, 3248, -758, -2989}, {7201, 3316, 46, -2614}, {-88, -7809, -538, -4571},
    {6193, -5189, 2760, -1245}, {12325, -1290, -3284, 253}, {13064, -4075, -2824, 1877}, {5333, 2999, 775, -1132}
};
/* x / d rounded down, as the decoder's arithmetic shifts round */
static int64_t floor_div(int64_t x, int64_t d) { return (x - (x < 0 ? d - 1 : 0)) / d; }
static void hevag_decode(Voice* v, const unsigned char* frames, unsigned channels) {
    for (unsigned c = 0; c < channels; ++c) {
        const unsigned char* f = frames + c * 16;
        unsigned predictor = (unsigned)(f[0] >> 4 | (f[1] & 0xf0)), shift = f[0] & 15u, flag = f[1] & 15u;
        if (predictor > 127) predictor = 0;
        const int16_t* k = hevag_coefficients[predictor];
        int32_t* h = v->history[c];
        for (unsigned j = 0; j < 28; ++j) {
            int32_t sample = 0;
            if (flag < 7) {
                int32_t residual = (int32_t)((f[2 + j / 2] >> (j & 1 ? 4 : 0)) & 15);
                if (residual >= 8) residual -= 16;
                int64_t predicted = (int64_t)h[0] * k[0] + (int64_t)h[1] * k[1] + (int64_t)h[2] * k[2] +
                                    (int64_t)h[3] * k[3];
                int64_t scaled = (int64_t)residual * ((int64_t)1 << (20 - shift));
                int64_t sum = floor_div(scaled + floor_div(predicted, 32), 256);
                sample = (int32_t)(sum < INT32_MIN ? INT32_MIN : sum > INT32_MAX ? INT32_MAX : sum);
            }
            v->decoded[j * channels + c] = (int16_t)snx_clamp_int(sample, -32768, 32767);
            h[3] = h[2], h[2] = h[1], h[1] = h[0], h[0] = sample;
        }
    }
    v->decoded_at = 0, v->decoded_count = 28 * channels;
}
/* the player's next frame from its buffers (SceNgsPlayerBufferParams, 12 bytes each from its parameters' byte 8: the
   data, its bytes, its loops (-1 for ever), the buffer after it), as floats; false once it ran out of them. A buffer
   played to its end starts again until it looped its count, then the next one plays */
static bool player_frame(VitaProcess* p, Voice* v, const unsigned char* params, unsigned channels, bool adpcm,
                         float out[2]) {
    if (adpcm && v->decoded_at < v->decoded_count) {
        out[0] = (float)v->decoded[v->decoded_at] / 32768.0f;
        out[1] = channels > 1 ? (float)v->decoded[v->decoded_at + 1] / 32768.0f : out[0];
        v->decoded_at += channels;
        return true;
    }
    uint32_t unit = adpcm ? 16 * channels : 2 * channels; /* the bytes read at once: a frame, or a sample a channel */
    for (unsigned hops = 0; hops < 16; ++hops) {
        int32_t current = (int32_t)v->player[1];
        if (current < 0 || current > 3) return false;
        const unsigned char* b = params + 8 + (uint32_t)current * 12;
        uint32_t data = snx_ld_le32(b), bytes = snx_ld_le32(b + 4), at = v->player[0];
        int32_t loops = (int16_t)snx_ld_le16(b + 8), next = (int16_t)snx_ld_le16(b + 10);
        if (!data || (int32_t)bytes <= 0) {
            v->player[1] = (uint32_t)next, v->player[0] = 0;
            continue;
        }
        if (at >= bytes || bytes - at < unit) {
            v->player[0] = 0;
            if (loops != -1 && ++v->loops > (uint32_t)loops) v->loops = 0, v->player[1] = (uint32_t)next;
            continue;
        }
        if (!vita_mapped(p, data + at, unit)) return false;
        const unsigned char* s = vita_ptr(p, data + at);
        v->player[0] += unit, v->player[3] += unit, v->player[5] += unit;
        if (adpcm) {
            hevag_decode(v, s, channels);
            return player_frame(p, v, params, channels, true, out);
        }
        out[0] = (float)(int16_t)snx_ld_le16(s) / 32768.0f;
        out[1] = channels > 1 ? (float)(int16_t)snx_ld_le16(s + 2) / 32768.0f : out[0];
        return true;
    }
    return false;
}
/* a simple voice's granule into out (stereo floats): its player's frames at the playback frequency and scalar,
   linearly resampled to the system's rate; false once it finished */
static bool simple_run(VitaProcess* p, struct VitaNgs* n, Voice* v, float* out) {
    const unsigned char* params = vita_ptr(p, module_params(v->handle, 0));
    unsigned channels = params[0x4c] == 1 ? 1 : 2;
    memset(out, 0, n->granularity * 2 * sizeof *out);
    if (snx_ld_le32(params) != PLAYER_ID) return false;
    if (params[0x4f] > 1) { /* SceNgsPlayerType: 16-bit PCM (0), HE-ADPCM (1) */
        if (!n->logged_type) printf("vita: an NGS player of type %u is silent\n", (unsigned)params[0x4f]);
        n->logged_type = true;
        return false;
    }
    bool adpcm = params[0x4f] == 1;
    float frequency = ld_float(params + 0x38), scalar = ld_float(params + 0x3c);
    float step = (frequency > 0.0f ? frequency / (float)n->rate : 1.0f) * (scalar >= 0.0f ? scalar : 1.0f);
    step = snx_clamp_float(step, 0.0f, 16.0f);
    bool playing = true;
    for (uint32_t k = 0; k < n->granularity && playing; ++k) {
        while (v->phase >= 1.0f && playing) {
            v->last[0] = v->next[0], v->last[1] = v->next[1];
            playing = player_frame(p, v, params, channels, adpcm, v->next);
            v->phase -= 1.0f;
        }
        if (!playing) break;
        for (unsigned c = 0; c < 2; ++c) out[k * 2 + c] = v->last[c] + (v->next[c] - v->last[c]) * v->phase;
        v->phase += step;
    }
    v->player[2] += n->granularity * channels, v->player[4] += n->granularity * channels;
    return playing;
}
/* a voice's granule through its patches into the voices they lead to, as their channels take it */
static void deliver(VitaProcess* p, struct VitaNgs* n, const Voice* v, const float* sound) {
    bool mono = n->racks[v->rack].channels == 1;
    for (unsigned o = 0; o < OUTPUTS; ++o)
        for (unsigned i = 0; i < PATCHES; ++i) {
            const Patch* patch = &v->patches[o][i];
            Voice* d = patch->used ? voice_of(p, patch->destination) : NULL;
            if (!d || d->state != ACTIVE || d->paused) continue;
            bool mono_out = n->racks[d->rack].channels == 1;
            float m00 = patch->matrix[0][0], m01 = mono_out ? 0.0f : patch->matrix[0][1];
            float m10 = mono ? 0.0f : patch->matrix[1][0], m11 = mono || mono_out ? 0.0f : patch->matrix[1][1];
            for (uint32_t k = 0; k < n->granularity; ++k) {
                float l = sound[k * 2], r = sound[k * 2 + 1];
                d->input[k * 2] = snx_clamp_float(d->input[k * 2] + l * m00 + r * m10, -1.0f, 1.0f);
                d->input[k * 2 + 1] = snx_clamp_float(d->input[k * 2 + 1] + l * m01 + r * m11, -1.0f, 1.0f);
            }
        }
}
/* sceNgsSystemUpdate(system): a granule of every voice playing, the sources before the busses they lead to */
static uint32_t system_update(VitaProcess* p, VitaThread* t) {
    (void)t;
    struct VitaNgs* n = p->ngs;
    if (!n || !n->granularity) return SCE_NGS_ERROR_INVALID_PARAM;
    for (unsigned i = 0; i < VOICES; ++i) memset(n->voices[i].input, 0, n->granularity * 2 * sizeof(float));
    static const uint32_t order[DEFS] = {DEF_SIMPLE, DEF_REVERB, DEF_MASTER};
    float sound[MOST_GRANULARITY * 2];
    for (unsigned k = 0; k < DEFS; ++k)
        for (unsigned r = 0; r < RACKS; ++r) {
            const Rack* rack = &n->racks[r];
            if (!rack->count || rack->kind != order[k]) continue;
            for (uint32_t i = 0; i < rack->count; ++i) {
                Voice* v = &n->voices[rack->first + i];
                if (v->state != ACTIVE || v->paused) continue;
                ++v->updates;
                if (rack->kind == DEF_MASTER) {
                    for (uint32_t s = 0; s < n->granularity * 2; ++s)
                        snx_st_le16(v->output + s * 2, (uint16_t)(int16_t)snx_clamp_float(v->input[s] * 32768.0f,
                                                                                         -32768.0f, 32767.0f));
                    continue;
                }
                bool playing = true;
                if (rack->kind == DEF_SIMPLE)
                    playing = simple_run(p, n, v, sound);
                else
                    memcpy(sound, v->input, n->granularity * 2 * sizeof *sound);
                deliver(p, n, v, sound);
                if (!playing) v->state = AVAILABLE;
            }
        }
    return 0;
}

const VitaHle vita_hle_ngs[] = {
    {0x6ce8b36fu, "sceNgsSystemGetRequiredMemorySize", system_get_required_memory_size},
    {0xed14cf4au, "sceNgsSystemInit", system_init},
    {0x4a25bebcu, "sceNgsSystemRelease", system_release},
    {0x64d80013u, "sceNgsSystemSetFlags", ok},
    {0x684f080cu, "sceNgsSystemUpdate", system_update},
    {0x477318c0u, "sceNgsRackGetRequiredMemorySize", rack_get_required_memory_size},
    {0x0a92e4ecu, "sceNgsRackInit", rack_init},
    {0xfe1a98e9u, "sceNgsRackGetVoiceHandle", rack_get_voice_handle},
    {0xdd5ca10bu, "sceNgsRackRelease", rack_release},
    {0x0d5399cfu, "sceNgsVoiceDefGetSimpleVoice", simple_voice},
    {0x79a121d1u, "sceNgsVoiceDefGetMasterBuss", master_buss},
    {0x9dcf50f5u, "sceNgsVoiceDefGetReverbBuss", reverb_buss},
    {0xab6bef8fu, "sceNgsVoiceLockParams", voice_lock_params},
    {0x3d46d8a7u, "sceNgsVoiceUnlockParams", ok},
    {0xfb8174b1u, "sceNgsVoiceSetParamsBlock", voice_set_params_block},
    {0xc9b8c0b4u, "sceNgsVoiceGetStateData", voice_get_state_data},
    {0x5551410du, "sceNgsVoiceGetInfo", voice_get_info},
    {0xfa0a0f34u, "sceNgsVoicePlay", voice_play},
    {0xd7786e99u, "sceNgsVoicePause", voice_pause},
    {0x54cfb981u, "sceNgsVoiceResume", voice_resume},
    {0x0e291aadu, "sceNgsVoiceKill", voice_kill},
    {0x9ab87e71u, "sceNgsVoiceBypassModule", ok},
    {0xd668b49cu, "sceNgsPatchCreateRouting", patch_create_routing},
    {0xd0c9ae5au, "sceNgsPatchRemoveRouting", patch_remove_routing},
    {0xa0f5402du, "sceNgsVoicePatchSetVolumesMatrix", patch_set_volumes_matrix},
    {0, NULL, NULL},
};

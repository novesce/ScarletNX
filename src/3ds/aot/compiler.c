/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The offline compiler, part of the driver: the extracted image's static blocks to a native module. Never creates an
   execution engine. */
#include "3ds/aot/ctr_compile.h"
#include "3ds/aot/ctr_module.h"
#include "3ds/aot/ctr_runtime.h"
#include "3ds/aot/ctr_discover.h"
#include "common/cpu/arm/llvm.h"
#include "common/utilities/common.h"
#include <llvm-c/Analysis.h>
#include <llvm-c/Error.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

_Static_assert(SNX_ARM_CODEGEN_OPTIONS_ABI == SNX_CTR_MODULE_CODEGEN_ABI, "codegen ABI");
_Static_assert(SNX_ARM_MEMORY_CHECKED == SNX_CTR_MODULE_CHECKED_MODE, "memory ABI");
_Static_assert(SNX_ARM_MEMORY_FLAT == SNX_CTR_MODULE_FLAT_MODE, "memory ABI");
#define CHUNK_ENTRIES 512u
#define OBJECT_LIMIT  (1024u * 1024u * 1024u) /* a commercial title's 330000 blocks take about 300 MiB */
#define MODULE_LIMIT  (1024u * 1024u * 1024u)
typedef struct {
    unsigned mode, index, length;
} Entry;
typedef struct {
    char* triple;
    LLVMTargetMachineRef machine;
    LLVMTargetDataRef layout;
} Host;
static uint32_t little(const unsigned char* p, unsigned size) {
    uint32_t result = 0;
    for (unsigned i = 0; i < size; ++i) result |= (uint32_t)p[i] << (i * 8);
    return result;
}
/* a staged file's name inside the staging directory */
static const char* leaf(const char* path) {
    const char* last = path;
    for (const char* p = path; *p; ++p)
        if (*p == '/' || *p == '\\') last = p + 1;
    return last;
}
static void name(char out[40], unsigned mode, unsigned index) {
    snprintf(out, 40, "ctr_%c_%u", mode ? 't' : 'a', index);
}
static LLVMValueRef table(LLVMModuleRef mod, const char* id, LLVMValueRef* functions, unsigned count) {
    LLVMTypeRef ptr = LLVMPointerTypeInContext(LLVMGetModuleContext(mod), 0);
    LLVMValueRef result = LLVMAddGlobal(mod, LLVMArrayType(ptr, count), id);
    LLVMSetInitializer(result, LLVMConstArray(ptr, functions, count));
    LLVMSetGlobalConstant(result, 1);
    LLVMSetLinkage(result, LLVMInternalLinkage);
    return result;
}
static bool descriptor(LLVMModuleRef mod, LLVMTargetDataRef layout, const SnxCtrImage* image, LLVMValueRef arm,
                       LLVMValueRef thumb) {
    LLVMContextRef c = LLVMGetModuleContext(mod);
    LLVMTypeRef i8 = LLVMInt8TypeInContext(c), i32 = LLVMInt32TypeInContext(c);
    LLVMTypeRef types[18];
    LLVMValueRef values[18], hash[32];
    types[0] = LLVMInt64TypeInContext(c);
    values[0] = LLVMConstInt(types[0], SNX_CTR_MODULE_MAGIC, 0);
    uint32_t integers[] = {SNX_CTR_MODULE_ABI,
                           sizeof(SnxCtrModule),
                           sizeof(SnxArmCPU),
                           sizeof(SnxArmCheckedCPU),
                           SNX_ARM_ABI,
                           SNX_ARM_CHECKED_ABI,
                           SNX_ARM_CODEGEN_OPTIONS_ABI,
                           SNX_ARM_PROFILE_ARM11,
                           snx_ctr_module_memory(),
                           sizeof(void*),
                           image->segment[0].address,
                           image->segment[0].bytes,
                           image->segment[0].bytes / 4,
                           image->segment[0].bytes / 2};
    const size_t offsets[] = {offsetof(SnxCtrModule, magic),           offsetof(SnxCtrModule, abi),
                              offsetof(SnxCtrModule, descriptor_size), offsetof(SnxCtrModule, cpu_size),
                              offsetof(SnxCtrModule, checked_size),    offsetof(SnxCtrModule, arm_abi),
                              offsetof(SnxCtrModule, checked_abi),     offsetof(SnxCtrModule, codegen_abi),
                              offsetof(SnxCtrModule, profile),         offsetof(SnxCtrModule, memory_mode),
                              offsetof(SnxCtrModule, pointer_size),    offsetof(SnxCtrModule, text_address),
                              offsetof(SnxCtrModule, text_bytes),      offsetof(SnxCtrModule, arm_count),
                              offsetof(SnxCtrModule, thumb_count),     offsetof(SnxCtrModule, image_identity),
                              offsetof(SnxCtrModule, arm_blocks),      offsetof(SnxCtrModule, thumb_blocks)};
    for (unsigned i = 0; i < 14; ++i) {
        types[i + 1] = i32;
        values[i + 1] = LLVMConstInt(i32, integers[i], 0);
    }
    types[15] = LLVMArrayType(i8, 32);
    for (unsigned i = 0; i < 32; ++i) hash[i] = LLVMConstInt(i8, image->identity[i], 0);
    values[15] = LLVMConstArray(i8, hash, 32);
    types[16] = types[17] = LLVMPointerTypeInContext(c, 0);
    values[16] = arm;
    values[17] = thumb;
    LLVMTypeRef structure = LLVMStructTypeInContext(c, types, 18, 0);
    if (LLVMABISizeOfType(layout, structure) != sizeof(SnxCtrModule)) return false;
    for (unsigned i = 0; i < 18; ++i)
        if (LLVMOffsetOfElement(layout, structure, i) != offsets[i]) return false;
    LLVMValueRef global = LLVMAddGlobal(mod, structure, "snx_ctr_module_v1");
    LLVMSetInitializer(global, LLVMConstNamedStruct(structure, values, 18));
    LLVMSetGlobalConstant(global, 1);
#ifdef _WIN32
    /* COFF has no hidden visibility. A DLL without any explicit export gets every global exported by lld, one per
       block function, past the 65535 exports PE allows. Once one symbol is exported explicitly lld exports nothing
       else, so the descriptor, the only symbol the loader looks up, is marked. The module is built for the host the
       compiler runs on, so the compiler's own _WIN32 says the module is a DLL (an MSYS2 or Cygwin build of the
       compiler targets Windows too and defines it). */
    LLVMSetDLLStorageClass(global, LLVMDLLExportStorageClass);
#endif
    return true;
}
static LLVMModuleRef new_module(Host* h, LLVMContextRef c) {
    LLVMModuleRef m = LLVMModuleCreateWithNameInContext("ctr-static-aot-v1", c);
    LLVMSetTarget(m, h->triple);
    LLVMSetModuleDataLayout(m, h->layout);
    return m;
}
static bool write_object(Host* h, LLVMModuleRef mod, char* output, const char* pipeline, char* error, size_t size) {
    char* detail = NULL;
    if (LLVMVerifyModule(mod, LLVMReturnStatusAction, &detail)) goto fail;
    LLVMDisposeMessage(detail);
    detail = NULL;
    if (pipeline) {
        LLVMPassBuilderOptionsRef passes = LLVMCreatePassBuilderOptions();
        LLVMErrorRef failure = LLVMRunPasses(mod, pipeline, h->machine, passes);
        LLVMDisposePassBuilderOptions(passes);
        if (failure) {
            char* message = LLVMGetErrorMessage(failure);
            snprintf(error, size, "%s", message);
            LLVMDisposeErrorMessage(message);
            return false;
        }
        if (LLVMVerifyModule(mod, LLVMReturnStatusAction, &detail)) goto fail;
        LLVMDisposeMessage(detail);
        detail = NULL;
    }
    if (LLVMTargetMachineEmitToFile(h->machine, mod, output, LLVMObjectFile, &detail)) goto fail;
    LLVMDisposeMessage(detail);
    return true;
fail:
    snprintf(error, size, "%s", detail ? detail : "cannot emit native ARM11 object");
    LLVMDisposeMessage(detail);
    return false;
}
/* The entries' blocks into one object. A block the backend ends sooner than discovery planned (at an MSR to the CPSR,
   which it ends blocks on) goes on as a block of its own at the instruction after, appended to *rest (*rest_count of
   them, the caller's to free) for the index */
static bool chunk(Host* h, const SnxCtrImage* image, const Entry* entries, unsigned count, char* output,
                  bool exhaustive, Entry** rest, unsigned* rest_count, char* error, size_t size) {
    LLVMContextRef c = LLVMContextCreate();
    LLVMModuleRef mod = new_module(h, c);
    bool ok = false;
    unsigned capacity = 0;
    SnxArmCodegenOptions options = {SNX_ARM_CODEGEN_OPTIONS_ABI, SNX_ARM_PROFILE_ARM11,
                                    (SnxArmMemoryMode)snx_ctr_module_memory()};
    *rest = NULL;
    *rest_count = 0;
    for (unsigned i = 0; i < count; ++i) {
        Entry e = entries[i];
        while (e.length) {
            unsigned width = e.mode ? 2 : 4;
            uint32_t pc = image->segment[0].address + e.index * width;
            uint32_t words[SNX_CTR_AOT_BLOCK_WORDS];
            uint16_t halves[SNX_CTR_AOT_BLOCK_WORDS];
            for (unsigned j = 0; j < e.length; ++j) {
                words[j] = little(image->code + e.index * width + j * width, width);
                halves[j] = (uint16_t)words[j];
            }
            char id[40];
            name(id, e.mode, e.index);
            size_t consumed = 0;
            LLVMValueRef f = e.mode ? snx_thumb_llvm_block_ex(mod, id, halves, e.length, pc, &consumed, &options)
                                    : snx_arm_llvm_block_ex(mod, id, words, e.length, pc, &consumed, &options);
            if (!f || !consumed || consumed > e.length) {
                snprintf(error, size, "static discovery/backend termination mismatch at 0x%08x (%zu/%u)", pc,
                         consumed, e.length);
                goto done;
            }
            LLVMSetVisibility(f, LLVMHiddenVisibility);
            if (e.index != entries[i].index) {
                if (*rest_count == capacity) {
                    Entry* grown = realloc(*rest, (capacity = capacity ? capacity * 2 : 16) * sizeof *grown);
                    if (!grown) {
                        snprintf(error, size, "out of memory for static entries");
                        goto done;
                    }
                    *rest = grown;
                }
                (*rest)[(*rest_count)++] = (Entry){e.mode, e.index, (unsigned)consumed};
            }
            e.index += (unsigned)consumed;
            e.length -= (unsigned)consumed;
        }
    }
    ok = write_object(h, mod, output, exhaustive ? "default<O2>" : "default<O1>", error, size);
done:
    LLVMDisposeModule(mod);
    LLVMContextDispose(c);
    return ok;
}
static bool index_object(Host* h, const SnxCtrImage* image, const Entry* entries, unsigned count, char* output,
                         char* error, size_t size) {
    LLVMContextRef c = LLVMContextCreate();
    LLVMModuleRef mod = new_module(h, c);
    LLVMTypeRef ptr = LLVMPointerTypeInContext(c, 0), ft = LLVMFunctionType(LLVMVoidTypeInContext(c), &ptr, 1, 0);
    LLVMValueRef tables[2];
    unsigned cursor = 0;
    bool ok = false;
    for (unsigned mode = 0; mode < 2; ++mode) {
        unsigned n = image->segment[0].bytes / (mode ? 2 : 4);
        LLVMValueRef* functions = malloc((size_t)n * sizeof *functions);
        if (!functions) {
            snprintf(error, size, "out of memory for static index");
            goto done;
        }
        for (unsigned i = 0; i < n; ++i) functions[i] = LLVMConstNull(ptr);
        while (cursor < count && entries[cursor].mode == mode) {
            const Entry* e = entries + cursor++;
            char id[40];
            name(id, mode, e->index);
            functions[e->index] = LLVMAddFunction(mod, id, ft);
            LLVMSetVisibility(functions[e->index], LLVMHiddenVisibility);
        }
        tables[mode] = table(mod, mode ? "ctr_thumb" : "ctr_arm", functions, n);
        free(functions);
    }
    if (!descriptor(mod, h->layout, image, tables[0], tables[1])) {
        snprintf(error, size, "incompatible native descriptor layout");
        goto done;
    }
    ok = write_object(h, mod, output, NULL, error, size);
done:
    LLVMDisposeModule(mod);
    LLVMContextDispose(c);
    return ok;
}
/* The title's hints (snx_ctr_jit_save_hints): the blocks earlier runs compiled at run time, one pc a line in hex, | 1
   for Thumb. *count of them, the caller's to free; none (NULL) without the file */
static uint32_t* read_hints(const char* directory, size_t* count) {
    char* path = snx_ctr_game_file(directory, SNX_CTR_HINTS);
    FILE* f = path ? fopen(path, "r") : NULL;
    uint32_t* hints = NULL;
    size_t capacity = 0;
    unsigned value;
    *count = 0;
    while (f && fscanf(f, "%x", &value) == 1) {
        if (*count == capacity) {
            uint32_t* grown = realloc(hints, (capacity = capacity ? capacity * 2 : 1024) * sizeof *grown);
            if (!grown) break;
            hints = grown;
        }
        hints[(*count)++] = value;
    }
    if (f) fclose(f);
    free(path);
    return hints;
}
static bool host_new(Host* h, char* error, size_t size) {
    LLVMTargetRef backend;
    char* detail = NULL;
    h->triple = LLVMGetDefaultTargetTriple();
    if (LLVMGetTargetFromTriple(h->triple, &backend, &detail)) {
        snprintf(error, size, "%s", detail);
        LLVMDisposeMessage(detail);
        return false;
    }
    h->machine = LLVMCreateTargetMachine(backend, h->triple, "generic", "", LLVMCodeGenLevelDefault, LLVMRelocPIC,
                                         LLVMCodeModelDefault);
    if (!h->machine) {
        snprintf(error, size, "cannot create an LLVM target machine for %s", h->triple);
        return false;
    }
    h->layout = LLVMCreateTargetDataLayout(h->machine);
    return true;
}
static void host_free(Host* h) {
    if (h->layout) LLVMDisposeTargetData(h->layout);
    if (h->machine) LLVMDisposeTargetMachine(h->machine);
    LLVMDisposeMessage(h->triple);
    memset(h, 0, sizeof *h);
}
/* The chunks shared out to a worker per host core, each with a target machine and LLVM contexts of its own: a
   commercial title's few hundred thousand blocks take an hour on one */
typedef struct {
    const SnxCtrImage* image;
    const Entry* entries;
    char** objects; /* chunk i's object is objects[i] */
    unsigned count, chunks, next, finished;
    bool exhaustive, failed;
    Entry* rest; /* the blocks the backend split off, every chunk's */
    unsigned rest_count;
    uint64_t output_bytes;
    char error[512];
#ifdef _WIN32
    CRITICAL_SECTION lock;
#else
    pthread_mutex_t lock;
#endif
} Work;
static void work_lock(Work* w) {
#ifdef _WIN32
    EnterCriticalSection(&w->lock);
#else
    pthread_mutex_lock(&w->lock);
#endif
}
static void work_unlock(Work* w) {
#ifdef _WIN32
    LeaveCriticalSection(&w->lock);
#else
    pthread_mutex_unlock(&w->lock);
#endif
}
static void worker(Work* w) {
    Host h = {0};
    char error[512] = "";
    bool ok = host_new(&h, error, sizeof error);
    for (;;) {
        work_lock(w);
        if (!ok && !w->failed) {
            w->failed = true;
            snprintf(w->error, sizeof w->error, "%s", error);
        }
        unsigned i = w->failed ? w->chunks : w->next++;
        work_unlock(w);
        if (i >= w->chunks) break;
        unsigned begin = i * CHUNK_ENTRIES, n = w->count - begin;
        if (n > CHUNK_ENTRIES) n = CHUNK_ENTRIES;
        Entry* rest = NULL;
        unsigned rest_count = 0;
        ok = chunk(&h, w->image, w->entries + begin, n, w->objects[i], w->exhaustive, &rest, &rest_count, error,
                   sizeof error);
        work_lock(w);
        Entry* merged = rest_count ? realloc(w->rest, (w->rest_count + rest_count) * sizeof *merged) : w->rest;
        if (ok && rest_count && !merged) {
            ok = false;
            snprintf(error, sizeof error, "out of memory for static entries");
        }
        if (ok) {
            w->rest = merged;
            if (rest_count) memcpy(w->rest + w->rest_count, rest, rest_count * sizeof *rest);
            w->rest_count += rest_count;
            w->output_bytes += snx_file_size(w->objects[i]);
            if (!(++w->finished % 16) || w->finished == w->chunks)
                printf("ARM11 AOT: %u/%u chunks of static blocks emitted\n", w->finished, w->chunks);
        }
        work_unlock(w);
        free(rest);
    }
    host_free(&h);
}
#ifdef _WIN32
static DWORD WINAPI worker_main(LPVOID w) {
    worker(w);
    return 0;
}
#else
static void* worker_main(void* w) {
    worker(w);
    return NULL;
}
#endif
static bool compile_chunks(Work* w) {
    unsigned jobs = snx_cpu_count(), created = 0;
    if (jobs > w->chunks) jobs = w->chunks;
    if (jobs < 1) jobs = 1;
#ifdef _WIN32
    HANDLE* threads = calloc(jobs, sizeof *threads);
    InitializeCriticalSection(&w->lock);
#else
    pthread_t* threads = calloc(jobs, sizeof *threads);
    pthread_mutex_init(&w->lock, NULL);
#endif
    for (unsigned i = 1; threads && i < jobs; ++i) {
#ifdef _WIN32
        if (!(threads[created] = CreateThread(NULL, 0, worker_main, w, 0, NULL))) break;
#else
        if (pthread_create(&threads[created], NULL, worker_main, w)) break;
#endif
        ++created;
    }
    worker(w);
    for (unsigned i = 0; i < created; ++i) {
#ifdef _WIN32
        WaitForSingleObject(threads[i], INFINITE);
        CloseHandle(threads[i]);
#else
        pthread_join(threads[i], NULL);
#endif
    }
#ifdef _WIN32
    DeleteCriticalSection(&w->lock);
#else
    pthread_mutex_destroy(&w->lock);
#endif
    free(threads);
    return !w->failed;
}
static int by_place(const void* a, const void* b) {
    const Entry *x = a, *y = b;
    if (x->mode != y->mode) return x->mode < y->mode ? -1 : 1;
    return (x->index > y->index) - (x->index < y->index);
}
int snx_ctr_aot_compile(const char* extracted, const SnxCtrSource* game, char* error, size_t error_size) {
    int result = 1;
    SnxCtrImage image = {0}, check = {0};
    SnxCtrPlan plan = {0};
    Host host = {0};
    char *directory = NULL, *target = NULL, *stage = NULL, *module = NULL, *runtime = NULL, **objects = NULL;
    char *previous = NULL, *cc = NULL;
    Entry* entries = NULL;
    unsigned count = 0, files = 0;
    const char** command = NULL;
    Work work = {0};
    if (error_size) error[0] = 0;
    if (!(game ? snx_ctr_image_from_source(game, &image, error, error_size)
              : snx_ctr_image_load(extracted, &image, error, error_size)))
        goto done;
    if (image.segment[0].bytes > SNX_CTR_AOT_TEXT_LIMIT) {
        snprintf(error, error_size, "static AOT text limit is %u bytes", SNX_CTR_AOT_TEXT_LIMIT);
        result = 3;
        goto done;
    }
    directory = snx_absolute(extracted);
    if (!directory) {
        char why[256];
        snprintf(error, error_size, "cannot resolve %s: %s", extracted, snx_os_error(why, sizeof why));
        goto done;
    }
    target = snx_ctr_game_file(directory, SNX_CTR_MODULE_SUFFIX);
    /* the compiler the link runs, found before the minutes of compiling: a release runs where the build's is not */
    {
        char *self = snx_executable("scarletnx"), *folder = self ? snx_dirname(self) : NULL, why[384];
        cc = snx_find_compiler(SNX_CTR_HOST_CC, folder, why, sizeof why);
        free(folder);
        free(self);
        if (!cc) {
            snprintf(error, error_size,
                     "no C compiler to link the native module: %s. Put Clang's bin folder on PATH (LLVM-MinGW's on "
                     "Windows), or clang beside this program",
                     why);
            goto done;
        }
    }
    bool exhaustive = image.segment[0].bytes <= SNX_CTR_AOT_EXHAUSTIVE_LIMIT;
    unsigned capacity = image.segment[0].bytes / 4 + image.segment[0].bytes / 2;
    size_t hint_count = 0;
    uint32_t* hints = read_hints(directory, &hint_count);
    if (hint_count) printf("Seeding ARM11 discovery with %zu blocks earlier runs compiled at run time\n", hint_count);
    bool discovered = exhaustive || snx_ctr_discover(&image, hints, hint_count, &plan, error, error_size);
    free(hints);
    if (!discovered) {
        result = 3;
        goto done;
    }
    entries = calloc(exhaustive ? capacity : plan.entries, sizeof *entries);
    if (!entries) {
        snprintf(error, error_size, "out of memory for static entries");
        goto done;
    }
    for (unsigned mode = 0; mode < 2; ++mode) {
        unsigned width = mode ? 2 : 4;
        for (unsigned i = 0; i < image.segment[0].bytes / width; ++i) {
            unsigned length = exhaustive ? 1 : snx_ctr_plan_length(&plan, image.segment[0].address + i * width, !!mode);
            if (length) entries[count++] = (Entry){mode, i, length};
        }
    }
    printf("Discovered ARM11 static coverage: %u blocks%s\n", count,
           exhaustive ? " (exhaustive tiny image)" : " (conservative CFG)");
    if (LLVMInitializeNativeTarget() || LLVMInitializeNativeAsmPrinter()) {
        snprintf(error, error_size, "cannot initialize native LLVM target");
        goto done;
    }
    if (!host_new(&host, error, error_size)) goto done;
    stage = snx_temp_dir(directory);
    if (!stage) {
        char why[256];
        snprintf(error, error_size, "cannot create a staging folder in %s: %s", directory,
                 snx_os_error(why, sizeof why));
        goto done;
    }
    files = (count + CHUNK_ENTRIES - 1) / CHUNK_ENTRIES + 1;
    objects = calloc(files, sizeof *objects);
    if (!objects) {
        snprintf(error, error_size, "out of memory for native chunks");
        goto done;
    }
    objects[0] = snx_path_join(stage, "code.o");
    for (unsigned i = 1; i < files; ++i) {
        char name[32];
        snprintf(name, sizeof name, "block-%05u.o", i);
        objects[i] = snx_path_join(stage, name);
    }
    work = (Work){.image = &image, .entries = entries, .objects = objects + 1, .count = count, .chunks = files - 1,
                  .exhaustive = exhaustive};
    if (!compile_chunks(&work)) {
        snprintf(error, error_size, "%s", work.error);
        goto done;
    }
    /* the index of every block, the ones the backend split off with the planned ones */
    if (work.rest_count) {
        Entry* all = realloc(entries, (count + work.rest_count) * sizeof *all);
        if (!all) {
            snprintf(error, error_size, "out of memory for static entries");
            goto done;
        }
        entries = all;
        memcpy(entries + count, work.rest, work.rest_count * sizeof *entries);
        count += work.rest_count;
        qsort(entries, count, sizeof *entries, by_place);
    }
    if (!index_object(&host, &image, entries, count, objects[0], error, error_size)) goto done;
    uint64_t output_bytes = work.output_bytes + snx_file_size(objects[0]);
    if (output_bytes > OBJECT_LIMIT || snx_ctr_aot_runtime_size > OBJECT_LIMIT - output_bytes) {
        snprintf(error, error_size, "native object output limit exceeded including semantic helpers");
        result = 3;
        goto done;
    }
    runtime = snx_path_join(stage, "arm-runtime.a");
    if (snx_write_file(runtime, snx_ctr_aot_runtime, snx_ctr_aot_runtime_size)) {
        char why[256];
        snprintf(error, error_size, "cannot write %s: %s", runtime, snx_os_error(why, sizeof why));
        goto done;
    }
    module = snx_path_join(stage, "code.so");
    command = calloc(files + 7, sizeof *command);
    if (!command) {
        snprintf(error, error_size, "out of memory for native linker arguments");
        goto done;
    }
    unsigned arg = 0;
    command[arg++] = cc;
    command[arg++] = "-shared";
#ifdef __APPLE__
    /* ld64's: a dylib refuses undefined symbols, as -z defs does (its default, said); its stacks aren't executable and
       its constant data is kept read-only without being asked, which noexecstack, relro and now ask GNU ld for */
    command[arg++] = "-Wl,-undefined,error";
#elif !defined(_WIN32)
    command[arg++] = "-Wl,-z,defs,-z,noexecstack,-z,relro,-z,now";
#else
    /* A PE DLL needs none of those: an unresolved symbol is always a link error, and it has no lazily bound PLT for
       relro and now to protect (lld's MinGW driver refuses -z). It is asked for DEP and ASLR compatibility outright
       rather than through the linker's defaults. lld writes no import library unless asked (--out-implib), so the
       published folder holds the DLL alone. */
    command[arg++] = "-Wl,--nxcompat,--dynamicbase";
#endif
    /* The linker runs in the staging directory and is given the files' names alone. With full paths a large image's
       hundreds of chunks pass the 32767 characters a Windows command line holds (CreateProcessW fails with
       ERROR_FILENAME_EXCED_RANGE). A commercial title's 700 chunks of 512 blocks take about 13000 by name, quotes
       included. */
    for (unsigned i = 0; i < files; ++i) command[arg++] = leaf(objects[i]);
    command[arg++] = leaf(runtime);
    command[arg++] = "-o";
    command[arg++] = leaf(module);
    if (snx_run_stage(command, stage, "native linking")) {
        snprintf(error, error_size, "the native linker failed, so no module was published");
        goto done;
    }
    if (!snx_is_file(module) || snx_is_link(module) || !snx_file_size(module)) {
        snprintf(error, error_size, "native linker did not produce a regular nonempty module");
        goto done;
    }
    if (snx_file_size(module) > MODULE_LIMIT) {
        snprintf(error, error_size, "native module output limit exceeded");
        result = 3;
        goto done;
    }
    /* the module is all the folder publishes: the objects and the helpers it was linked from go (a commercial title's
       are as large as the module) */
    for (unsigned i = 0; i < files; ++i) (void)remove(objects[i]);
    (void)remove(runtime);
    if (!(game ? snx_ctr_image_from_source(game, &check, error, error_size)
              : snx_ctr_image_load(directory, &check, error, error_size)))
        goto done; /* its own error says why */
    if (memcmp(image.identity, check.identity, 32)) {
        snprintf(error, error_size, "extracted image changed during compilation");
        goto done;
    }
    /* A module from an earlier compile is replaced only once the new one is complete, by a rename over it, so a
       failed publish leaves what was there. On Windows a DLL a running copy of the game has loaded cannot be
       replaced. */
    if (snx_replace(module, target)) {
        char why[256];
        snprintf(error, error_size, "cannot move %s to %s: %s", module, target, snx_os_error(why, sizeof why));
        goto done;
    }
    /* an older version's module, in native/ */
    previous = snx_path_join(directory, "native");
    if (previous && snx_is_dir(previous)) (void)snx_remove_tree(previous);
    (void)snx_remove_tree(stage);
    free(stage);
    stage = NULL;
    printf("Compiled ARM11 static module: %u blocks in %u objects\n", count, files);
    result = 0;
done:
    if (stage) snx_remove_tree(stage);
    if (objects)
        for (unsigned i = 0; i < files; ++i) free(objects[i]);
    free(command);
    free(objects);
    free(entries);
    free(work.rest);
    free(runtime);
    free(module);
    free(stage);
    free(target);
    free(previous);
    free(cc);
    free(directory);
    host_free(&host);
    snx_ctr_plan_free(&plan);
    snx_ctr_image_free(&check);
    snx_ctr_image_free(&image);
    return result;
}

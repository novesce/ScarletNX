/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What ir_fpu.c (scalar) and ir_vec_fp.c (AdvSIMD) share of the floating-point emitters: FPCR and FPSR in the CPU
   structure, i1 logic, the host's FP features the fast paths need, and a few IR pieces that work on scalars and
   vectors alike. Internal to those two files. */
#ifndef SNX_RECOMPILER_LLVM_IR_FP_H
#define SNX_RECOMPILER_LLVM_IR_FP_H

#include "common/cpu/recompiler/llvm/ir.h"
#include "common/cpu/aarch64/cpu.h"
#include <stddef.h>

#define FPCR_RMODE (3u << 22)
#define FPCR_FZ    (1u << 24)
#define FPSR_IXC   0x10u

static inline unsigned esz_of(unsigned width) { return width == 16 ? 1 : width == 32 ? 2 : 3; }

static inline LLVMValueRef fpcr(SnxIr* ir) { return ir_cpu_load(ir, ir->t_i32, offsetof(CPU, fpcr)); }
static inline LLVMValueRef fpsr(SnxIr* ir) { return ir_cpu_load(ir, ir->t_i32, offsetof(CPU, fpsr)); }
/* FPCR.RMode as the helpers' rounding mode (0-3) */
static inline LLVMValueRef fpcr_rmode(SnxIr* ir) {
    return LLVMBuildAnd(ir->b, LLVMBuildLShr(ir->b, fpcr(ir), ir_i32(ir, 22), ""), ir_i32(ir, 3), "");
}
/* i1: none of the FPCR bits of mask is set */
static inline LLVMValueRef fpcr_clear(SnxIr* ir, SnxU32 mask) {
    return LLVMBuildICmp(ir->b, LLVMIntEQ, LLVMBuildAnd(ir->b, fpcr(ir), ir_i32(ir, mask), ""), ir_i32(ir, 0), "");
}
static inline LLVMValueRef ixc_set(SnxIr* ir) {
    return LLVMBuildICmp(ir->b, LLVMIntNE, LLVMBuildAnd(ir->b, fpsr(ir), ir_i32(ir, FPSR_IXC), ""), ir_i32(ir, 0),
                         "");
}
static inline LLVMValueRef both(SnxIr* ir, LLVMValueRef a, LLVMValueRef b) { return LLVMBuildAnd(ir->b, a, b, ""); }
static inline LLVMValueRef either(SnxIr* ir, LLVMValueRef a, LLVMValueRef b) { return LLVMBuildOr(ir->b, a, b, ""); }

static inline int host_native(SnxIr* ir) { return ir->features->x86 || ir->features->aarch64; }
static inline int host_fma(SnxIr* ir) { return (ir->features->x86 && ir->features->fma) || ir->features->aarch64; }
/* roundss/roundsd (SSE4.1) or FRINT*: the rounding intrinsics never become libm calls */
static inline int host_round(SnxIr* ir) {
    return (ir->features->x86 && ir->features->sse42) || ir->features->aarch64;
}

/* an encoding (or a vector of them) as the helpers' u64 operand */
static inline LLVMValueRef u64_of(SnxIr* ir, LLVMValueRef bits) { return ir_cast(ir, bits, ir->t_i64, 0); }
static inline SnxU64 sign_mask(unsigned width) { return (SnxU64)1 << (width - 1); }
static inline LLVMValueRef fma3(SnxIr* ir, LLVMValueRef a, LLVMValueRef b, LLVMValueRef c) {
    LLVMValueRef args[3] = {a, b, c};
    return ir_intrinsic(ir, "llvm.fma", LLVMTypeOf(a), args, 3);
}
static inline LLVMValueRef float_ne(SnxIr* ir, LLVMValueRef a, LLVMValueRef b) {
    return LLVMBuildFCmp(ir->b, LLVMRealUNE, a, b, "");
}

#endif

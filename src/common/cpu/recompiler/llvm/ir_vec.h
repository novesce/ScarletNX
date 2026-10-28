/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The AdvSIMD and crypto instructions' emitters, keyed on the generated decoder's ops (ir_vec.c for the integer
   ones and the building blocks the others share, ir_vec_fp.c for floating point, ir_vec_crypto.c for the
   cryptographic extensions) */
#ifndef SNX_RECOMPILER_LLVM_IR_VEC_H
#define SNX_RECOMPILER_LLVM_IR_VEC_H

#include "common/cpu/recompiler/llvm/ir.h"
#include "common/cpu/aarch64/a64_decoder.h"

/* 1 when ir_vec.c, ir_vec_fp.c or ir_vec_crypto.c emits op, whatever feature it needs */
int snx_ir_vec_handles(SnxA64Op op);
/* The body of insn (word at pc), an op snx_ir_vec_handles */
void snx_ir_vec(SnxIr* ir, const SnxA64Insn* insn, SnxU32 word, SnxU64 pc);

/* ---- shared by the three files ---- */

/* iE for elements of 1 << esz bytes, and <lanes x iE> (lanes 1 included: scalar forms stay vectors) */
LLVMTypeRef ir_vec_lane(SnxIr* ir, unsigned esz);
LLVMTypeRef ir_vec_type(SnxIr* ir, unsigned esz, unsigned lanes);
/* the lanes of 8 << q bytes */
unsigned ir_vec_lanes(int q, unsigned esz);
/* lanes elements of Vn from element first on, as numbers on any host */
LLVMValueRef ir_vec_read(SnxIr* ir, unsigned n, unsigned esz, unsigned first, unsigned lanes);
/* Vd = value (up to 128 bits of lanes), the bytes above it zeroed */
void ir_vec_write(SnxIr* ir, unsigned d, LLVMValueRef value);
/* Vd.D[1] = value (64 bits of lanes), D[0] kept: the "2" forms of the narrowing instructions */
void ir_vec_write_high(SnxIr* ir, unsigned d, LLVMValueRef value);
/* type's value v in every lane (an integer type or a vector of integers) */
LLVMValueRef ir_vec_k(SnxIr* ir, LLVMTypeRef type, SnxU64 v);
/* v in every lane of like's type */
static inline LLVMValueRef ir_vec_like(SnxIr* ir, LLVMValueRef like, SnxU64 v) {
    return ir_vec_k(ir, LLVMTypeOf(like), v);
}
/* the lanes of a vector value */
static inline unsigned ir_vec_count(LLVMValueRef v) { return LLVMGetVectorSize(LLVMTypeOf(v)); }
/* <lanes x T> of the scalar value */
LLVMValueRef ir_vec_splat(SnxIr* ir, LLVMValueRef value, unsigned lanes);
/* shufflevector(a, b, mask) */
LLVMValueRef ir_vec_shuffle(SnxIr* ir, LLVMValueRef a, LLVMValueRef b, const unsigned* mask, unsigned n);
/* i1: any lane of the <N x i1> holds */
LLVMValueRef ir_vec_any(SnxIr* ir, LLVMValueRef lanes);
LLVMValueRef ir_vec_all(SnxIr* ir, LLVMValueRef lanes);
/* FPSR.QC |= saturated (i1) */
void ir_vec_qc(SnxIr* ir, LLVMValueRef saturated);
/* name(c, operands...) in the runtime library, declared from the operands' types; it reads and writes only what
   its pointer arguments reach (c->v, c->fpcr, c->fpsr) */
LLVMValueRef ir_vec_call(SnxIr* ir, const char* name, LLVMTypeRef ret, const LLVMValueRef* operands, unsigned count);

/* ---- ir_vec_fp.c ---- */
int ir_vec_fp_handles(SnxA64Op op);
void ir_vec_fp(SnxIr* ir, const SnxA64Insn* insn);

/* ---- ir_vec_crypto.c ---- */
int ir_vec_crypto_handles(SnxA64Op op);
void ir_vec_crypto(SnxIr* ir, const SnxA64Insn* insn);

#endif

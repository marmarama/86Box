#include <math.h>
#include <stdint.h>
#include <86box/86box.h>
#include "cpu.h"
#include <86box/mem.h>
#include <86box/plat_unused.h>

#include "x86.h"
#include "x86_flags.h"
#include "x86seg_common.h"
#include "x86seg.h"
#include "386_common.h"
#include "x87_sf.h"
#include "x87.h"
#include "codegen.h"
#include "codegen_accumulate.h"
#include "codegen_ir.h"
#include "codegen_ops.h"
#include "codegen_ops_branch.h"
#include "codegen_ops_fpu_misc.h"
#include "codegen_ops_helpers.h"
#include "codegen_ops_jit_wrappers.h"

static JIT_WRAPPER void
jit_fldcw(uint32_t new_cw)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        cpu_state.npxc = (uint16_t) new_cw;
        codegen_set_rounding_mode((cpu_state.npxc >> 10) & 3);
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
    } else {
        cpu_state.npxc = (uint16_t) new_cw;
        codegen_set_rounding_mode((cpu_state.npxc >> 10) & 3);
    }
}

static inline void
jit_fxam_body(void)
{
    double st0 = cpu_state.ST[cpu_state.TOP & 7];

    cpu_state.npxs &= ~(FPU_SW_C0 | FPU_SW_C1 | FPU_SW_C2 | FPU_SW_C3);
    if (cpu_state.tag[cpu_state.TOP & 7] == TAG_EMPTY)
        cpu_state.npxs |= (FPU_SW_C0 | FPU_SW_C3);
    else {
        switch (fpclassify(st0)) {
            case FP_SUBNORMAL:
                cpu_state.npxs |= FPU_SW_C2 | FPU_SW_C3;
                break;
            case FP_NAN:
                cpu_state.npxs |= FPU_SW_C0;
                break;
            case FP_INFINITE:
                cpu_state.npxs |= FPU_SW_C0 | FPU_SW_C2;
                break;
            case FP_ZERO:
                cpu_state.npxs |= FPU_SW_C3;
                break;
            case FP_NORMAL:
                cpu_state.npxs |= FPU_SW_C2;
                break;
        }
    }
    if (st0 < 0.0)
        cpu_state.npxs |= FPU_SW_C1;
}

static JIT_WRAPPER void
jit_fxam(void)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        jit_fxam_body();
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
    } else {
        jit_fxam_body();
    }
}

uint32_t
ropFFREE(UNUSED(codeblock_t *block), ir_data_t *ir, UNUSED(uint8_t opcode), uint32_t fetchdat, UNUSED(uint32_t op_32), uint32_t op_pc)
{
    int dest_reg = fetchdat & 7;

    uop_FP_ENTER(ir);
    uop_MOV(ir, IREG_tag(dest_reg), TAG_EMPTY);

    return op_pc;
}

uint32_t
ropFLD(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), uint32_t fetchdat, UNUSED(uint32_t op_32), uint32_t op_pc)
{
    int src_reg = fetchdat & 7;

    uop_FP_ENTER(ir);
    uop_MOV(ir, IREG_ST(-1), IREG_ST(src_reg));
    if (codegen_fpu_has_uint64)
        uop_MOV(ir, IREG_ST_i64(-1), IREG_ST_i64(src_reg));
    uop_MOV(ir, IREG_tag(-1), IREG_tag(src_reg));
    fpu_PUSH(block, ir);

    return op_pc;
}

uint32_t
ropFST(UNUSED(codeblock_t *block), ir_data_t *ir, UNUSED(uint8_t opcode), uint32_t fetchdat, UNUSED(uint32_t op_32), uint32_t op_pc)
{
    int dest_reg = fetchdat & 7;

    uop_FP_ENTER(ir);
    uop_MOV(ir, IREG_ST(dest_reg), IREG_ST(0));
    if (codegen_fpu_has_uint64)
        uop_MOV(ir, IREG_ST_i64(dest_reg), IREG_ST_i64(0));
    uop_MOV(ir, IREG_tag(dest_reg), IREG_tag(0));

    return op_pc;
}
uint32_t
ropFSTP(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), uint32_t fetchdat, UNUSED(uint32_t op_32), uint32_t op_pc)
{
    int dest_reg = fetchdat & 7;

    uop_FP_ENTER(ir);
    uop_MOV(ir, IREG_ST(dest_reg), IREG_ST(0));
    if (codegen_fpu_has_uint64)
        uop_MOV(ir, IREG_ST_i64(dest_reg), IREG_ST_i64(0));
    uop_MOV(ir, IREG_tag(dest_reg), IREG_tag(0));
    fpu_POP(block, ir);

    return op_pc;
}

uint32_t
ropFSTCW(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    x86seg *target_seg;

    uop_FP_ENTER(ir);
    uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
    op_pc--;
    target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);
    codegen_check_seg_write(block, ir, target_seg);
    uop_MEM_STORE_REG(ir, ireg_seg_base(target_seg), IREG_eaaddr, IREG_NPXC);

    return op_pc + 1;
}
/* The status word as FSTSW gives it: TOP (bits 13-11) is the stack's own,
   which npxs does not keep up to date. */
static void
fpu_status_word(ir_data_t *ir, int dest_reg)
{
    uop_AND_IMM(ir, IREG_temp1, IREG_FPU_TOP, 7);
    uop_SHL_IMM(ir, IREG_temp1, IREG_temp1, 11);
    uop_AND_IMM(ir, dest_reg, IREG_NPXS, 0xc7ff);
    uop_OR(ir, dest_reg, dest_reg, IREG_temp1_W);
}

uint32_t
ropFSTSW(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    x86seg *target_seg;

    uop_FP_ENTER(ir);
    uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
    op_pc--;
    target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);
    codegen_check_seg_write(block, ir, target_seg);
    fpu_status_word(ir, IREG_temp0_W);
    uop_MEM_STORE_REG(ir, ireg_seg_base(target_seg), IREG_eaaddr, IREG_temp0_W);

    return op_pc + 1;
}
uint32_t
ropFSTSW_AX(UNUSED(codeblock_t *block), ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), UNUSED(uint32_t op_32), uint32_t op_pc)
{
    uop_FP_ENTER(ir);
    fpu_status_word(ir, IREG_AX);

    return op_pc;
}

uint32_t
ropFXCH(UNUSED(codeblock_t *block), ir_data_t *ir, UNUSED(uint8_t opcode), uint32_t fetchdat, UNUSED(uint32_t op_32), uint32_t op_pc)
{
    int dest_reg = fetchdat & 7;

    uop_FP_ENTER(ir);
    uop_MOV(ir, IREG_temp0_D, IREG_ST(0));
    if (codegen_fpu_has_uint64)
        uop_MOV(ir, IREG_temp1_Q, IREG_ST_i64(0));
    uop_MOV(ir, IREG_temp2, IREG_tag(0));
    uop_MOV(ir, IREG_ST(0), IREG_ST(dest_reg));
    if (codegen_fpu_has_uint64)
        uop_MOV(ir, IREG_ST_i64(0), IREG_ST_i64(dest_reg));
    uop_MOV(ir, IREG_tag(0), IREG_tag(dest_reg));
    uop_MOV(ir, IREG_ST(dest_reg), IREG_temp0_D);
    if (codegen_fpu_has_uint64)
        uop_MOV(ir, IREG_ST_i64(dest_reg), IREG_temp1_Q);
    uop_MOV(ir, IREG_tag(dest_reg), IREG_temp2);

    return op_pc;
}

uint32_t
ropFLDCW(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    x86seg *target_seg;

    uop_FP_ENTER(ir);
    uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
    op_pc--;
    target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);
    codegen_check_seg_read(block, ir, target_seg);
    CHECK_SEG_LIMITS(block, ir, target_seg, IREG_eaaddr, 1);
    uop_MEM_LOAD_REG(ir, IREG_temp0_W, ireg_seg_base(target_seg), IREG_eaaddr);
    uop_LOAD_FUNC_ARG_REG(ir, 0, IREG_temp0_W);
    uop_CALL_FUNC(ir, jit_fldcw);
    /* End the block so any following FPU ops compile under the updated PC/RC block key. */
    CPU_BLOCK_END();

    return op_pc + 1;
}

uint32_t
ropFRNDINT(UNUSED(codeblock_t *block), ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), UNUSED(uint32_t op_32), uint32_t op_pc)
{
#if defined(__x86_64__) || defined(_M_X64)
    if (!host_has_sse41)
        return 0;
#endif
    uop_FP_ENTER(ir);
    uop_FRNDINT(ir, IREG_ST(0), IREG_ST(0));
    uop_MOV_IMM(ir, IREG_tag(0), TAG_VALID);

    return op_pc;
}

uint32_t
ropFXAM(UNUSED(codeblock_t *block), ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), UNUSED(uint32_t op_32), uint32_t op_pc)
{
    uop_FP_ENTER(ir);
    uop_CALL_FUNC(ir, jit_fxam);

    return op_pc;
}

static uint32_t
ropFCMOVcc_common(UNUSED(codeblock_t *block), ir_data_t *ir, uint32_t fetchdat, uint32_t op_pc, int is_db)
{
    static const int fcmov_conds[4] = { COND_B, COND_E, COND_BE, COND_P };
    const OpFn      *op_tbl         = is_db ? x86_dynarec_opcodes_db_a16 : x86_dynarec_opcodes_da_a16;
    if (op_tbl[fetchdat & 0xff] == op_tbl[0xff])
        return 0;

    uop_FP_ENTER(ir);

    int src_reg = fetchdat & 7;
    if (src_reg == 0)
        return op_pc;

    int cond = fcmov_conds[(fetchdat >> 3) & 3] | is_db;
    codegen_eval_cond_to_reg(ir, cond, IREG_temp1);
    uop_CMOV(ir, IREG_ST(0), IREG_ST(0), IREG_ST(src_reg), IREG_temp1);
    uop_CMOV(ir, IREG_tag(0), IREG_tag(0), IREG_tag(src_reg), IREG_temp1);
    if (codegen_fpu_has_uint64)
        uop_CMOV(ir, IREG_ST_i64(0), IREG_ST_i64(0), IREG_ST_i64(src_reg), IREG_temp1);

    return op_pc;
}

uint32_t
ropFCMOVcc_da(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), uint32_t fetchdat, UNUSED(uint32_t op_32), uint32_t op_pc)
{
    return ropFCMOVcc_common(block, ir, fetchdat, op_pc, 0);
}

uint32_t
ropFCMOVcc_db(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), uint32_t fetchdat, UNUSED(uint32_t op_32), uint32_t op_pc)
{
    return ropFCMOVcc_common(block, ir, fetchdat, op_pc, 1);
}

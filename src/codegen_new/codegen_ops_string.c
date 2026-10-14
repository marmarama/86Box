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
#include "codegen.h"
#include "codegen_ir.h"
#include "codegen_ops.h"
#include "codegen_ops_helpers.h"
#include "codegen_accumulate.h"
#include "codegen_ops_string.h"

/* Branchless pointer advance based on Direction Flag (DF):
   DF is bit 10 of flags (D_FLAG = 0x0400).
   Extract bit 10 to sign bit, arithmetic shift right by 31 to get mask:
   mask = (DF ? 0xFFFFFFFF : 0).
   delta = (step ^ mask) - mask:
   If DF == 0: (step ^ 0) - 0 = +step
   If DF != 0: (step ^ -1) - (-1) = -step
*/
static inline void
codegen_string_advance(ir_data_t *ir, int ptr_reg, int step, int is_32)
{
    uop_MOVZX(ir, IREG_temp0, IREG_flags);
    uop_SHL_IMM(ir, IREG_temp0, IREG_temp0, 21);
    uop_SAR_IMM(ir, IREG_temp0, IREG_temp0, 31);
    uop_XOR_IMM(ir, IREG_temp1, IREG_temp0, step);
    uop_SUB(ir, IREG_temp1, IREG_temp1, IREG_temp0);
    if (is_32)
        uop_ADD(ir, ptr_reg, ptr_reg, IREG_temp1);
    else
        uop_ADD(ir, ptr_reg, ptr_reg, IREG_temp1_W);
}

static inline void
codegen_movs_advance(ir_data_t *ir, int src_ptr, int dst_ptr, int step, int is_32)
{
    uop_MOVZX(ir, IREG_temp0, IREG_flags);
    uop_SHL_IMM(ir, IREG_temp0, IREG_temp0, 21);
    uop_SAR_IMM(ir, IREG_temp0, IREG_temp0, 31);
    uop_XOR_IMM(ir, IREG_temp1, IREG_temp0, step);
    uop_SUB(ir, IREG_temp1, IREG_temp1, IREG_temp0);
    if (is_32) {
        uop_ADD(ir, src_ptr, src_ptr, IREG_temp1);
        uop_ADD(ir, dst_ptr, dst_ptr, IREG_temp1);
    } else {
        uop_ADD(ir, src_ptr, src_ptr, IREG_temp1_W);
        uop_ADD(ir, dst_ptr, dst_ptr, IREG_temp1_W);
    }
}

static inline void
codegen_stos_single(codeblock_t *block, ir_data_t *ir, uint32_t op_32, int step, int val_reg)
{
    uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
    codegen_check_seg_write(block, ir, &cpu_state.seg_es);

    if (op_32 & 0x200)
        uop_MOV(ir, IREG_eaaddr, IREG_EDI);
    else
        uop_MOVZX(ir, IREG_eaaddr, IREG_DI);

    CHECK_SEG_LIMITS(block, ir, &cpu_state.seg_es, IREG_eaaddr, step - 1);
    uop_MEM_STORE_REG(ir, ireg_seg_base(&cpu_state.seg_es), IREG_eaaddr, val_reg);

    codegen_string_advance(ir, (op_32 & 0x200) ? IREG_EDI : IREG_DI, step, op_32 & 0x200);
}

static inline void
codegen_lods_single(codeblock_t *block, ir_data_t *ir, uint32_t op_32, int step, int val_reg)
{
    uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
    codegen_check_seg_read(block, ir, op_ea_seg);

    if (op_32 & 0x200)
        uop_MOV(ir, IREG_eaaddr, IREG_ESI);
    else
        uop_MOVZX(ir, IREG_eaaddr, IREG_SI);

    CHECK_SEG_LIMITS(block, ir, op_ea_seg, IREG_eaaddr, step - 1);
    uop_MEM_LOAD_REG(ir, val_reg, ireg_seg_base(op_ea_seg), IREG_eaaddr);

    codegen_string_advance(ir, (op_32 & 0x200) ? IREG_ESI : IREG_SI, step, op_32 & 0x200);
}

static inline void
codegen_movs_single(codeblock_t *block, ir_data_t *ir, uint32_t op_32, int step, int val_reg)
{
    uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
    codegen_check_seg_read(block, ir, op_ea_seg);
    codegen_check_seg_write(block, ir, &cpu_state.seg_es);

    /* Source address */
    if (op_32 & 0x200)
        uop_MOV(ir, IREG_eaaddr, IREG_ESI);
    else
        uop_MOVZX(ir, IREG_eaaddr, IREG_SI);

    CHECK_SEG_LIMITS(block, ir, op_ea_seg, IREG_eaaddr, step - 1);

    /* Destination address */
    if (op_32 & 0x200)
        uop_MOV(ir, IREG_temp2, IREG_EDI);
    else
        uop_MOVZX(ir, IREG_temp2, IREG_DI);

    CHECK_SEG_LIMITS(block, ir, &cpu_state.seg_es, IREG_temp2, step - 1);

    uop_MEM_LOAD_REG(ir, val_reg, ireg_seg_base(op_ea_seg), IREG_eaaddr);
    uop_MEM_STORE_REG(ir, ireg_seg_base(&cpu_state.seg_es), IREG_temp2, val_reg);

    codegen_movs_advance(ir, (op_32 & 0x200) ? IREG_ESI : IREG_SI, (op_32 & 0x200) ? IREG_EDI : IREG_DI, step, op_32 & 0x200);
}

static inline void
codegen_scas_single(codeblock_t *block, ir_data_t *ir, uint32_t op_32, int step, int acc_reg, int val_reg, int flags_sub_op)
{
    uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
    codegen_check_seg_read(block, ir, &cpu_state.seg_es);

    if (op_32 & 0x200)
        uop_MOV(ir, IREG_eaaddr, IREG_EDI);
    else
        uop_MOVZX(ir, IREG_eaaddr, IREG_DI);

    CHECK_SEG_LIMITS(block, ir, &cpu_state.seg_es, IREG_eaaddr, step - 1);
    uop_MEM_LOAD_REG(ir, val_reg, ireg_seg_base(&cpu_state.seg_es), IREG_eaaddr);

    if (step == 1) {
        uop_MOVZX(ir, IREG_flags_op1, acc_reg);
        uop_SUB(ir, IREG_flags_res_B, acc_reg, val_reg);
        uop_MOVZX(ir, IREG_flags_res, IREG_flags_res_B);
        uop_MOVZX(ir, IREG_flags_op2, val_reg);
    } else if (step == 2) {
        uop_MOVZX(ir, IREG_flags_op1, acc_reg);
        uop_SUB(ir, IREG_flags_res_W, acc_reg, val_reg);
        uop_MOVZX(ir, IREG_flags_res, IREG_flags_res_W);
        uop_MOVZX(ir, IREG_flags_op2, val_reg);
    } else {
        uop_MOV(ir, IREG_flags_op1, acc_reg);
        uop_SUB(ir, IREG_flags_res, acc_reg, val_reg);
        uop_MOV(ir, IREG_flags_op2, val_reg);
    }
    uop_MOV_IMM(ir, IREG_flags_op, flags_sub_op);
    codegen_flags_changed = 1;

    codegen_string_advance(ir, (op_32 & 0x200) ? IREG_EDI : IREG_DI, step, op_32 & 0x200);
}

static inline void
codegen_cmps_single(codeblock_t *block, ir_data_t *ir, uint32_t op_32, int step, int src_val, int dst_val, int flags_sub_op)
{
    uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
    codegen_check_seg_read(block, ir, op_ea_seg);
    codegen_check_seg_read(block, ir, &cpu_state.seg_es);

    /* Source address (DS or segment override) */
    if (op_32 & 0x200)
        uop_MOV(ir, IREG_eaaddr, IREG_ESI);
    else
        uop_MOVZX(ir, IREG_eaaddr, IREG_SI);

    CHECK_SEG_LIMITS(block, ir, op_ea_seg, IREG_eaaddr, step - 1);
    uop_MEM_LOAD_REG(ir, src_val, ireg_seg_base(op_ea_seg), IREG_eaaddr);

    /* Destination address (strictly ES) */
    if (op_32 & 0x200)
        uop_MOV(ir, IREG_eaaddr, IREG_EDI);
    else
        uop_MOVZX(ir, IREG_eaaddr, IREG_DI);

    CHECK_SEG_LIMITS(block, ir, &cpu_state.seg_es, IREG_eaaddr, step - 1);
    uop_MEM_LOAD_REG(ir, dst_val, ireg_seg_base(&cpu_state.seg_es), IREG_eaaddr);

    if (step == 1) {
        uop_MOVZX(ir, IREG_flags_op1, src_val);
        uop_SUB(ir, IREG_flags_res_B, src_val, dst_val);
        uop_MOVZX(ir, IREG_flags_res, IREG_flags_res_B);
        uop_MOVZX(ir, IREG_flags_op2, dst_val);
    } else if (step == 2) {
        uop_MOVZX(ir, IREG_flags_op1, src_val);
        uop_SUB(ir, IREG_flags_res_W, src_val, dst_val);
        uop_MOVZX(ir, IREG_flags_res, IREG_flags_res_W);
        uop_MOVZX(ir, IREG_flags_op2, dst_val);
    } else {
        uop_MOV(ir, IREG_flags_op1, src_val);
        uop_SUB(ir, IREG_flags_res, src_val, dst_val);
        uop_MOV(ir, IREG_flags_op2, dst_val);
    }
    uop_MOV_IMM(ir, IREG_flags_op, flags_sub_op);
    codegen_flags_changed = 1;

    codegen_movs_advance(ir, (op_32 & 0x200) ? IREG_ESI : IREG_SI, (op_32 & 0x200) ? IREG_EDI : IREG_DI, step, op_32 & 0x200);
}

static inline uint32_t
ropSTOS_common(codeblock_t *block, ir_data_t *ir, uint32_t op_32, uint32_t op_pc, int step, int val_reg)
{
    codegen_stos_single(block, ir, op_32, step, val_reg);
    return op_pc;
}

static inline uint32_t
ropLODS_common(codeblock_t *block, ir_data_t *ir, uint32_t op_32, uint32_t op_pc, int step, int val_reg)
{
    codegen_lods_single(block, ir, op_32, step, val_reg);
    return op_pc;
}

static inline uint32_t
ropMOVS_common(codeblock_t *block, ir_data_t *ir, uint32_t op_32, uint32_t op_pc, int step, int val_reg)
{
    codegen_movs_single(block, ir, op_32, step, val_reg);
    return op_pc;
}

static inline uint32_t
ropREP_STOS_common(codeblock_t *block, ir_data_t *ir, uint32_t op_32, uint32_t op_pc, int step, int val_reg)
{
    int count_reg = (op_32 & 0x200) ? IREG_ECX : IREG_CX;
    int step_cycles = is486 ? 3 : 4;

    codegen_accumulate_flush(ir);

    int jump_zero = uop_CMP_IMM_JZ_DEST(ir, count_reg, 0);

    int loop_label = uop_LOOP_LABEL(ir);

    codegen_stos_single(block, ir, op_32, step, val_reg);

    uop_SUB_IMM(ir, count_reg, count_reg, 1);
    uop_SUB_IMM(ir, IREG_cycles, IREG_cycles, step_cycles);

    uop_NOP_BARRIER(ir);

    int jump_cnt_zero = uop_CMP_IMM_JZ_DEST(ir, count_reg, 0);
    int jump_cycles   = uop_CMP_IMM_JLE_DEST(ir, IREG_cycles, 0);
    int jump_exit     = uop_CMP_IMM_JNZ_DEST(ir, IREG_exit_request, 0);

    uop_LOOP_JMP(ir, loop_label);

    uop_set_jump_dest(ir, jump_cycles);
    uop_set_jump_dest(ir, jump_exit);
    uop_MOV_IMM(ir, IREG_pc, cpu_state.oldpc);
    uop_JMP(ir, codegen_exit_rout);

    uop_set_jump_dest(ir, jump_zero);
    uop_set_jump_dest(ir, jump_cnt_zero);

    CPU_BLOCK_END();
    return op_pc;
}

static inline uint32_t
ropREP_LODS_common(codeblock_t *block, ir_data_t *ir, uint32_t op_32, uint32_t op_pc, int step, int val_reg)
{
    int count_reg = (op_32 & 0x200) ? IREG_ECX : IREG_CX;
    int step_cycles = is486 ? 3 : 4;

    codegen_accumulate_flush(ir);

    int jump_zero = uop_CMP_IMM_JZ_DEST(ir, count_reg, 0);

    int loop_label = uop_LOOP_LABEL(ir);

    codegen_lods_single(block, ir, op_32, step, val_reg);

    uop_SUB_IMM(ir, count_reg, count_reg, 1);
    uop_SUB_IMM(ir, IREG_cycles, IREG_cycles, step_cycles);

    uop_NOP_BARRIER(ir);

    int jump_cnt_zero = uop_CMP_IMM_JZ_DEST(ir, count_reg, 0);
    int jump_cycles   = uop_CMP_IMM_JLE_DEST(ir, IREG_cycles, 0);
    int jump_exit     = uop_CMP_IMM_JNZ_DEST(ir, IREG_exit_request, 0);

    uop_LOOP_JMP(ir, loop_label);

    uop_set_jump_dest(ir, jump_cycles);
    uop_set_jump_dest(ir, jump_exit);
    uop_MOV_IMM(ir, IREG_pc, cpu_state.oldpc);
    uop_JMP(ir, codegen_exit_rout);

    uop_set_jump_dest(ir, jump_zero);
    uop_set_jump_dest(ir, jump_cnt_zero);

    CPU_BLOCK_END();
    return op_pc;
}

static inline uint32_t
ropREP_MOVS_common(codeblock_t *block, ir_data_t *ir, uint32_t op_32, uint32_t op_pc, int step, int val_reg)
{
    int count_reg = (op_32 & 0x200) ? IREG_ECX : IREG_CX;
    int step_cycles = is486 ? 3 : 4;

    codegen_accumulate_flush(ir);

    int jump_zero = uop_CMP_IMM_JZ_DEST(ir, count_reg, 0);

    int loop_label = uop_LOOP_LABEL(ir);

    codegen_movs_single(block, ir, op_32, step, val_reg);

    uop_SUB_IMM(ir, count_reg, count_reg, 1);
    uop_SUB_IMM(ir, IREG_cycles, IREG_cycles, step_cycles);

    uop_NOP_BARRIER(ir);

    int jump_cnt_zero = uop_CMP_IMM_JZ_DEST(ir, count_reg, 0);
    int jump_cycles   = uop_CMP_IMM_JLE_DEST(ir, IREG_cycles, 0);
    int jump_exit     = uop_CMP_IMM_JNZ_DEST(ir, IREG_exit_request, 0);

    uop_LOOP_JMP(ir, loop_label);

    uop_set_jump_dest(ir, jump_cycles);
    uop_set_jump_dest(ir, jump_exit);
    uop_MOV_IMM(ir, IREG_pc, cpu_state.oldpc);
    uop_JMP(ir, codegen_exit_rout);

    uop_set_jump_dest(ir, jump_zero);
    uop_set_jump_dest(ir, jump_cnt_zero);

    CPU_BLOCK_END();
    return op_pc;
}

static inline uint32_t
ropREPE_SCAS_common(codeblock_t *block, ir_data_t *ir, uint32_t op_32, uint32_t op_pc, int step, int acc_reg, int val_reg, int flags_sub_op)
{
    int count_reg = (op_32 & 0x200) ? IREG_ECX : IREG_CX;
    int step_cycles = is486 ? 3 : 4;

    codegen_accumulate_flush(ir);

    int jump_zero = uop_CMP_IMM_JZ_DEST(ir, count_reg, 0);

    int loop_label = uop_LOOP_LABEL(ir);

    codegen_scas_single(block, ir, op_32, step, acc_reg, val_reg, flags_sub_op);

    uop_SUB_IMM(ir, count_reg, count_reg, 1);
    uop_SUB_IMM(ir, IREG_cycles, IREG_cycles, step_cycles);

    uop_NOP_BARRIER(ir);

    int jump_mismatch = uop_CMP_IMM_JNZ_DEST(ir, IREG_flags_res, 0);
    int jump_cnt_zero = uop_CMP_IMM_JZ_DEST(ir, count_reg, 0);
    int jump_cycles   = uop_CMP_IMM_JLE_DEST(ir, IREG_cycles, 0);
    int jump_exit     = uop_CMP_IMM_JNZ_DEST(ir, IREG_exit_request, 0);

    uop_LOOP_JMP(ir, loop_label);

    uop_set_jump_dest(ir, jump_cycles);
    uop_set_jump_dest(ir, jump_exit);
    uop_MOV_IMM(ir, IREG_pc, cpu_state.oldpc);
    uop_JMP(ir, codegen_exit_rout);

    uop_set_jump_dest(ir, jump_zero);
    uop_set_jump_dest(ir, jump_mismatch);
    uop_set_jump_dest(ir, jump_cnt_zero);

    CPU_BLOCK_END();
    return op_pc;
}

static inline uint32_t
ropREPNE_SCAS_common(codeblock_t *block, ir_data_t *ir, uint32_t op_32, uint32_t op_pc, int step, int acc_reg, int val_reg, int flags_sub_op)
{
    int count_reg = (op_32 & 0x200) ? IREG_ECX : IREG_CX;
    int step_cycles = is486 ? 3 : 4;

    codegen_accumulate_flush(ir);

    int jump_zero = uop_CMP_IMM_JZ_DEST(ir, count_reg, 0);

    int loop_label = uop_LOOP_LABEL(ir);

    codegen_scas_single(block, ir, op_32, step, acc_reg, val_reg, flags_sub_op);

    uop_SUB_IMM(ir, count_reg, count_reg, 1);
    uop_SUB_IMM(ir, IREG_cycles, IREG_cycles, step_cycles);

    uop_NOP_BARRIER(ir);

    int jump_match    = uop_CMP_IMM_JZ_DEST(ir, IREG_flags_res, 0);
    int jump_cnt_zero = uop_CMP_IMM_JZ_DEST(ir, count_reg, 0);
    int jump_cycles   = uop_CMP_IMM_JLE_DEST(ir, IREG_cycles, 0);
    int jump_exit     = uop_CMP_IMM_JNZ_DEST(ir, IREG_exit_request, 0);

    uop_LOOP_JMP(ir, loop_label);

    uop_set_jump_dest(ir, jump_cycles);
    uop_set_jump_dest(ir, jump_exit);
    uop_MOV_IMM(ir, IREG_pc, cpu_state.oldpc);
    uop_JMP(ir, codegen_exit_rout);

    uop_set_jump_dest(ir, jump_zero);
    uop_set_jump_dest(ir, jump_match);
    uop_set_jump_dest(ir, jump_cnt_zero);

    CPU_BLOCK_END();
    return op_pc;
}

static inline uint32_t
ropREPE_CMPS_common(codeblock_t *block, ir_data_t *ir, uint32_t op_32, uint32_t op_pc, int step, int src_val, int dst_val, int flags_sub_op)
{
    int count_reg = (op_32 & 0x200) ? IREG_ECX : IREG_CX;
    int step_cycles = is486 ? 3 : 4;

    codegen_accumulate_flush(ir);

    int jump_zero = uop_CMP_IMM_JZ_DEST(ir, count_reg, 0);

    int loop_label = uop_LOOP_LABEL(ir);

    codegen_cmps_single(block, ir, op_32, step, src_val, dst_val, flags_sub_op);

    uop_SUB_IMM(ir, count_reg, count_reg, 1);
    uop_SUB_IMM(ir, IREG_cycles, IREG_cycles, step_cycles);

    uop_NOP_BARRIER(ir);

    int jump_mismatch = uop_CMP_IMM_JNZ_DEST(ir, IREG_flags_res, 0);
    int jump_cnt_zero = uop_CMP_IMM_JZ_DEST(ir, count_reg, 0);
    int jump_cycles   = uop_CMP_IMM_JLE_DEST(ir, IREG_cycles, 0);
    int jump_exit     = uop_CMP_IMM_JNZ_DEST(ir, IREG_exit_request, 0);

    uop_LOOP_JMP(ir, loop_label);

    uop_set_jump_dest(ir, jump_cycles);
    uop_set_jump_dest(ir, jump_exit);
    uop_MOV_IMM(ir, IREG_pc, cpu_state.oldpc);
    uop_JMP(ir, codegen_exit_rout);

    uop_set_jump_dest(ir, jump_zero);
    uop_set_jump_dest(ir, jump_mismatch);
    uop_set_jump_dest(ir, jump_cnt_zero);

    CPU_BLOCK_END();
    return op_pc;
}

static inline uint32_t
ropREPNE_CMPS_common(codeblock_t *block, ir_data_t *ir, uint32_t op_32, uint32_t op_pc, int step, int src_val, int dst_val, int flags_sub_op)
{
    int count_reg = (op_32 & 0x200) ? IREG_ECX : IREG_CX;
    int step_cycles = is486 ? 3 : 4;

    codegen_accumulate_flush(ir);

    int jump_zero = uop_CMP_IMM_JZ_DEST(ir, count_reg, 0);

    int loop_label = uop_LOOP_LABEL(ir);

    codegen_cmps_single(block, ir, op_32, step, src_val, dst_val, flags_sub_op);

    uop_SUB_IMM(ir, count_reg, count_reg, 1);
    uop_SUB_IMM(ir, IREG_cycles, IREG_cycles, step_cycles);

    uop_NOP_BARRIER(ir);

    int jump_match    = uop_CMP_IMM_JZ_DEST(ir, IREG_flags_res, 0);
    int jump_cnt_zero = uop_CMP_IMM_JZ_DEST(ir, count_reg, 0);
    int jump_cycles   = uop_CMP_IMM_JLE_DEST(ir, IREG_cycles, 0);
    int jump_exit     = uop_CMP_IMM_JNZ_DEST(ir, IREG_exit_request, 0);

    uop_LOOP_JMP(ir, loop_label);

    uop_set_jump_dest(ir, jump_cycles);
    uop_set_jump_dest(ir, jump_exit);
    uop_MOV_IMM(ir, IREG_pc, cpu_state.oldpc);
    uop_JMP(ir, codegen_exit_rout);

    uop_set_jump_dest(ir, jump_zero);
    uop_set_jump_dest(ir, jump_match);
    uop_set_jump_dest(ir, jump_cnt_zero);

    CPU_BLOCK_END();
    return op_pc;
}

/* MOVS */
uint32_t
ropMOVS_8(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    return ropMOVS_common(block, ir, op_32, op_pc, 1, IREG_temp1_B);
}

uint32_t
ropMOVS_16(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    return ropMOVS_common(block, ir, op_32, op_pc, 2, IREG_temp1_W);
}

uint32_t
ropMOVS_32(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    return ropMOVS_common(block, ir, op_32, op_pc, 4, IREG_temp1);
}

/* STOS */
uint32_t
ropSTOS_8(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    return ropSTOS_common(block, ir, op_32, op_pc, 1, IREG_AL);
}

uint32_t
ropSTOS_16(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    return ropSTOS_common(block, ir, op_32, op_pc, 2, IREG_AX);
}

uint32_t
ropSTOS_32(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    return ropSTOS_common(block, ir, op_32, op_pc, 4, IREG_EAX);
}

/* LODS */
uint32_t
ropLODS_8(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    return ropLODS_common(block, ir, op_32, op_pc, 1, IREG_AL);
}

uint32_t
ropLODS_16(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    return ropLODS_common(block, ir, op_32, op_pc, 2, IREG_AX);
}

uint32_t
ropLODS_32(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    return ropLODS_common(block, ir, op_32, op_pc, 4, IREG_EAX);
}

/* REP MOVS */
uint32_t
ropREP_MOVS_8(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    return ropREP_MOVS_common(block, ir, op_32, op_pc, 1, IREG_temp1_B);
}

uint32_t
ropREP_MOVS_16(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    return ropREP_MOVS_common(block, ir, op_32, op_pc, 2, IREG_temp1_W);
}

uint32_t
ropREP_MOVS_32(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    return ropREP_MOVS_common(block, ir, op_32, op_pc, 4, IREG_temp1);
}

/* REP STOS */
uint32_t
ropREP_STOS_8(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    return ropREP_STOS_common(block, ir, op_32, op_pc, 1, IREG_AL);
}

uint32_t
ropREP_STOS_16(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    return ropREP_STOS_common(block, ir, op_32, op_pc, 2, IREG_AX);
}

uint32_t
ropREP_STOS_32(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    return ropREP_STOS_common(block, ir, op_32, op_pc, 4, IREG_EAX);
}

/* REP LODS */
uint32_t
ropREP_LODS_8(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    return ropREP_LODS_common(block, ir, op_32, op_pc, 1, IREG_AL);
}

uint32_t
ropREP_LODS_16(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    return ropREP_LODS_common(block, ir, op_32, op_pc, 2, IREG_AX);
}

uint32_t
ropREP_LODS_32(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    return ropREP_LODS_common(block, ir, op_32, op_pc, 4, IREG_EAX);
}

/* SCAS */
uint32_t
ropSCAS_8(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    codegen_scas_single(block, ir, op_32, 1, IREG_AL, IREG_temp2_B, FLAGS_SUB8);
    return op_pc;
}

uint32_t
ropSCAS_16(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    codegen_scas_single(block, ir, op_32, 2, IREG_AX, IREG_temp2_W, FLAGS_SUB16);
    return op_pc;
}

uint32_t
ropSCAS_32(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    codegen_scas_single(block, ir, op_32, 4, IREG_EAX, IREG_temp2, FLAGS_SUB32);
    return op_pc;
}

/* CMPS */
uint32_t
ropCMPS_8(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    codegen_cmps_single(block, ir, op_32, 1, IREG_temp1_B, IREG_temp2_B, FLAGS_SUB8);
    return op_pc;
}

uint32_t
ropCMPS_16(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    codegen_cmps_single(block, ir, op_32, 2, IREG_temp1_W, IREG_temp2_W, FLAGS_SUB16);
    return op_pc;
}

uint32_t
ropCMPS_32(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    codegen_cmps_single(block, ir, op_32, 4, IREG_temp1, IREG_temp2, FLAGS_SUB32);
    return op_pc;
}

/* REPE SCAS */
uint32_t
ropREPE_SCAS_8(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    return ropREPE_SCAS_common(block, ir, op_32, op_pc, 1, IREG_AL, IREG_temp2_B, FLAGS_SUB8);
}

uint32_t
ropREPE_SCAS_16(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    return ropREPE_SCAS_common(block, ir, op_32, op_pc, 2, IREG_AX, IREG_temp2_W, FLAGS_SUB16);
}

uint32_t
ropREPE_SCAS_32(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    return ropREPE_SCAS_common(block, ir, op_32, op_pc, 4, IREG_EAX, IREG_temp2, FLAGS_SUB32);
}

/* REPNE SCAS */
uint32_t
ropREPNE_SCAS_8(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    return ropREPNE_SCAS_common(block, ir, op_32, op_pc, 1, IREG_AL, IREG_temp2_B, FLAGS_SUB8);
}

uint32_t
ropREPNE_SCAS_16(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    return ropREPNE_SCAS_common(block, ir, op_32, op_pc, 2, IREG_AX, IREG_temp2_W, FLAGS_SUB16);
}

uint32_t
ropREPNE_SCAS_32(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    return ropREPNE_SCAS_common(block, ir, op_32, op_pc, 4, IREG_EAX, IREG_temp2, FLAGS_SUB32);
}

/* REPE CMPS */
uint32_t
ropREPE_CMPS_8(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    return ropREPE_CMPS_common(block, ir, op_32, op_pc, 1, IREG_temp1_B, IREG_temp2_B, FLAGS_SUB8);
}

uint32_t
ropREPE_CMPS_16(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    return ropREPE_CMPS_common(block, ir, op_32, op_pc, 2, IREG_temp1_W, IREG_temp2_W, FLAGS_SUB16);
}

uint32_t
ropREPE_CMPS_32(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    return ropREPE_CMPS_common(block, ir, op_32, op_pc, 4, IREG_temp1, IREG_temp2, FLAGS_SUB32);
}

/* REPNE CMPS */
uint32_t
ropREPNE_CMPS_8(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    return ropREPNE_CMPS_common(block, ir, op_32, op_pc, 1, IREG_temp1_B, IREG_temp2_B, FLAGS_SUB8);
}

uint32_t
ropREPNE_CMPS_16(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    return ropREPNE_CMPS_common(block, ir, op_32, op_pc, 2, IREG_temp1_W, IREG_temp2_W, FLAGS_SUB16);
}

uint32_t
ropREPNE_CMPS_32(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    return ropREPNE_CMPS_common(block, ir, op_32, op_pc, 4, IREG_temp1, IREG_temp2, FLAGS_SUB32);
}

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
#include "codegen_ops_string.h"

static inline void
codegen_string_advance(ir_data_t *ir, int ptr_reg, int step)
{
    uop_AND_IMM(ir, IREG_temp0_W, IREG_flags, D_FLAG);
    int jump_df = uop_CMP_IMM_JNZ_DEST(ir, IREG_temp0_W, 0);

    /* DF == 0: increment */
    uop_ADD_IMM(ir, ptr_reg, ptr_reg, step);
    int jump_end = uop_JMP_DEST(ir);

    /* DF != 0: decrement */
    uop_set_jump_dest(ir, jump_df);
    uop_SUB_IMM(ir, ptr_reg, ptr_reg, step);

    uop_set_jump_dest(ir, jump_end);
}

static inline void
codegen_movs_advance(ir_data_t *ir, int src_ptr, int dst_ptr, int step)
{
    uop_AND_IMM(ir, IREG_temp0_W, IREG_flags, D_FLAG);
    int jump_df = uop_CMP_IMM_JNZ_DEST(ir, IREG_temp0_W, 0);

    /* DF == 0: increment */
    uop_ADD_IMM(ir, src_ptr, src_ptr, step);
    uop_ADD_IMM(ir, dst_ptr, dst_ptr, step);
    int jump_end = uop_JMP_DEST(ir);

    /* DF != 0: decrement */
    uop_set_jump_dest(ir, jump_df);
    uop_SUB_IMM(ir, src_ptr, src_ptr, step);
    uop_SUB_IMM(ir, dst_ptr, dst_ptr, step);

    uop_set_jump_dest(ir, jump_end);
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

    codegen_string_advance(ir, (op_32 & 0x200) ? IREG_EDI : IREG_DI, step);
}

static inline void
codegen_lods_single(codeblock_t *block, ir_data_t *ir, uint32_t op_32, int step, int val_reg)
{
    uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);

    if (op_32 & 0x200)
        uop_MOV(ir, IREG_eaaddr, IREG_ESI);
    else
        uop_MOVZX(ir, IREG_eaaddr, IREG_SI);

    CHECK_SEG_LIMITS(block, ir, op_ea_seg, IREG_eaaddr, step - 1);
    uop_MEM_LOAD_REG(ir, val_reg, ireg_seg_base(op_ea_seg), IREG_eaaddr);

    codegen_string_advance(ir, (op_32 & 0x200) ? IREG_ESI : IREG_SI, step);
}

static inline void
codegen_movs_single(codeblock_t *block, ir_data_t *ir, uint32_t op_32, int step, int val_reg)
{
    uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
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

    codegen_movs_advance(ir, (op_32 & 0x200) ? IREG_ESI : IREG_SI, (op_32 & 0x200) ? IREG_EDI : IREG_DI, step);
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

    int jump_zero = uop_CMP_IMM_JZ_DEST(ir, count_reg, 0);

    codegen_stos_single(block, ir, op_32, step, val_reg);

    uop_SUB_IMM(ir, count_reg, count_reg, 1);

    int jump_cnt_zero = uop_CMP_IMM_JZ_DEST(ir, count_reg, 0);
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

    int jump_zero = uop_CMP_IMM_JZ_DEST(ir, count_reg, 0);

    codegen_lods_single(block, ir, op_32, step, val_reg);

    uop_SUB_IMM(ir, count_reg, count_reg, 1);

    int jump_cnt_zero = uop_CMP_IMM_JZ_DEST(ir, count_reg, 0);
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

    int jump_zero = uop_CMP_IMM_JZ_DEST(ir, count_reg, 0);

    codegen_movs_single(block, ir, op_32, step, val_reg);

    uop_SUB_IMM(ir, count_reg, count_reg, 1);

    int jump_cnt_zero = uop_CMP_IMM_JZ_DEST(ir, count_reg, 0);
    uop_MOV_IMM(ir, IREG_pc, cpu_state.oldpc);
    uop_JMP(ir, codegen_exit_rout);

    uop_set_jump_dest(ir, jump_zero);
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

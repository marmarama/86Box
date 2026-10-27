#include <stdint.h>
#include <86box/86box.h>
#include "cpu.h"
#include <86box/mem.h>
#include <86box/plat_unused.h>

#include "x86.h"
#include "x86_ops.h"
#include "x86_flags.h"
#include "x86seg_common.h"
#include "x86seg.h"
#include "386_common.h"
#include "codegen.h"
#include "codegen_accumulate.h"
#include "codegen_ir.h"
#include "codegen_ops.h"
#include "codegen_ops_helpers.h"
#include "codegen_ops_jit_wrappers.h"
#include "codegen_ops_misc.h"
#include "codegen_ops_branch.h"

uint32_t
ropLEA_16(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    int dest_reg = (fetchdat >> 3) & 7;

    if ((fetchdat & 0xc0) == 0xc0)
        return 0;

    codegen_mark_code_present(block, cs + op_pc, 1);
    codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);
    uop_MOV(ir, IREG_16(dest_reg), IREG_eaaddr_W);

    return op_pc + 1;
}
uint32_t
ropLEA_32(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    int dest_reg = (fetchdat >> 3) & 7;

    if ((fetchdat & 0xc0) == 0xc0)
        return 0;

    codegen_mark_code_present(block, cs + op_pc, 1);
    codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);
    uop_MOV(ir, IREG_32(dest_reg), IREG_eaaddr);

    return op_pc + 1;
}

uint32_t
ropF6(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    x86seg *target_seg = NULL;
    uint8_t imm_data;
    int     reg;

    if ((fetchdat & 0x38) > 0x30)
        return 0;

    codegen_mark_code_present(block, cs + op_pc, 1);
    if ((fetchdat & 0xc0) == 0xc0)
        reg = IREG_8(fetchdat & 7);
    else {
        uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
        target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);
        if ((fetchdat & 0x30) == 0x10) /*NEG/NOT*/
            codegen_check_seg_write(block, ir, target_seg);
        else
            codegen_check_seg_read(block, ir, target_seg);
        uop_MEM_LOAD_REG(ir, IREG_temp0_B, ireg_seg_base(target_seg), IREG_eaaddr);
        reg = IREG_temp0_B;
    }

    switch (fetchdat & 0x38) {
        case 0x00:
        case 0x08: /*TEST*/
            imm_data = fastreadb(cs + op_pc + 1);

            uop_AND_IMM(ir, IREG_flags_res_B, reg, imm_data);
            uop_MOVZX(ir, IREG_flags_res, IREG_flags_res_B);
            uop_MOV_IMM(ir, IREG_flags_op, FLAGS_ZN8);

            codegen_flags_changed = 1;
            codegen_mark_code_present(block, cs + op_pc + 1, 1);
            return op_pc + 2;

        case 0x10: /*NOT*/
            uop_XOR_IMM(ir, reg, reg, 0xff);
            if ((fetchdat & 0xc0) != 0xc0)
                uop_MEM_STORE_REG(ir, ireg_seg_base(target_seg), IREG_eaaddr, reg);

            codegen_flags_changed = 1;
            return op_pc + 1;

        case 0x18: /*NEG*/
            uop_MOV_IMM(ir, IREG_temp1_B, 0);

            if ((fetchdat & 0xc0) == 0xc0) {
                uop_MOVZX(ir, IREG_flags_op2, reg);
                uop_SUB(ir, IREG_temp1_B, IREG_temp1_B, reg);
                uop_MOVZX(ir, IREG_flags_res, IREG_temp1_B);
                uop_MOV(ir, reg, IREG_temp1_B);
            } else {
                uop_SUB(ir, IREG_temp1_B, IREG_temp1_B, reg);
                uop_MEM_STORE_REG(ir, ireg_seg_base(target_seg), IREG_eaaddr, IREG_temp1_B);
                uop_MOVZX(ir, IREG_flags_op2, IREG_temp0_B);
                uop_MOVZX(ir, IREG_flags_res, IREG_temp1_B);
            }
            uop_MOV_IMM(ir, IREG_flags_op, FLAGS_SUB8);
            uop_MOV_IMM(ir, IREG_flags_op1, 0);

            codegen_flags_changed = 1;
            return op_pc + 1;

        case 0x20: /*MUL AL, rm8*/
            uop_MOVZX(ir, IREG_temp1, reg);
            uop_MOVZX(ir, IREG_temp0, IREG_AL);
            uop_IMUL(ir, IREG_temp2, IREG_temp0, IREG_temp1);
            uop_MOV(ir, IREG_AX, IREG_temp2_W);
            uop_SHR_IMM(ir, IREG_temp3, IREG_temp2, 8);
            uop_CALL_FUNC(ir, jit_flags_rebuild);
            uop_AND_IMM(ir, IREG_flags, IREG_flags, ~(C_FLAG | V_FLAG));
            uop_MOV_IMM(ir, IREG_temp0_W, 0);
            uop_MOV_IMM(ir, IREG_temp1_W, C_FLAG | V_FLAG);
            uop_CMOV(ir, IREG_temp0_W, IREG_temp0_W, IREG_temp1_W, IREG_temp3_W);
            uop_OR(ir, IREG_flags, IREG_flags, IREG_temp0_W);
            uop_MOV_IMM(ir, IREG_flags_op, FLAGS_UNKNOWN);
            codegen_last_flags_op = FLAGS_UNKNOWN;

            codegen_flags_changed = 1;
            return op_pc + 1;

        case 0x30: /*DIV AL, rm8*/
            if ((fetchdat & 0xc0) == 0xc0)
                uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
            uop_LOAD_FUNC_ARG_REG(ir, 0, reg);
            uop_CALL_FUNC_RESULT(ir, IREG_temp1, jit_div8);
            uop_CMP_IMM_JZ(ir, IREG_temp1, 1, codegen_exit_rout);

            codegen_flags_changed = 0;
            return op_pc + 1;

        default:
            break;
    }
    return 0;
}
uint32_t
ropF7_16(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    x86seg  *target_seg = NULL;
    uint16_t imm_data;
    int      reg;

    if ((fetchdat & 0x38) > 0x30)
        return 0;

    codegen_mark_code_present(block, cs + op_pc, 1);
    if ((fetchdat & 0xc0) == 0xc0)
        reg = IREG_16(fetchdat & 7);
    else {
        uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
        target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);
        if ((fetchdat & 0x30) == 0x10) /*NEG/NOT*/
            codegen_check_seg_write(block, ir, target_seg);
        else
            codegen_check_seg_read(block, ir, target_seg);
        uop_MEM_LOAD_REG(ir, IREG_temp0_W, ireg_seg_base(target_seg), IREG_eaaddr);
        reg = IREG_temp0_W;
    }

    switch (fetchdat & 0x38) {
        case 0x00:
        case 0x08: /*TEST*/
            imm_data = fastreadw(cs + op_pc + 1);

            uop_AND_IMM(ir, IREG_flags_res_W, reg, imm_data);
            uop_MOVZX(ir, IREG_flags_res, IREG_flags_res_W);
            uop_MOV_IMM(ir, IREG_flags_op, FLAGS_ZN16);

            codegen_flags_changed = 1;
            codegen_mark_code_present(block, cs + op_pc + 1, 2);
            return op_pc + 3;

        case 0x10: /*NOT*/
            uop_XOR_IMM(ir, reg, reg, 0xffff);
            if ((fetchdat & 0xc0) != 0xc0)
                uop_MEM_STORE_REG(ir, ireg_seg_base(target_seg), IREG_eaaddr, reg);

            codegen_flags_changed = 1;
            return op_pc + 1;

        case 0x18: /*NEG*/
            uop_MOV_IMM(ir, IREG_temp1_W, 0);

            if ((fetchdat & 0xc0) == 0xc0) {
                uop_MOVZX(ir, IREG_flags_op2, reg);
                uop_SUB(ir, IREG_temp1_W, IREG_temp1_W, reg);
                uop_MOVZX(ir, IREG_flags_res, IREG_temp1_W);
                uop_MOV(ir, reg, IREG_temp1_W);
            } else {
                uop_SUB(ir, IREG_temp1_W, IREG_temp1_W, reg);
                uop_MEM_STORE_REG(ir, ireg_seg_base(target_seg), IREG_eaaddr, IREG_temp1_W);
                uop_MOVZX(ir, IREG_flags_op2, IREG_temp0_W);
                uop_MOVZX(ir, IREG_flags_res, IREG_temp1_W);
            }
            uop_MOV_IMM(ir, IREG_flags_op, FLAGS_SUB16);
            uop_MOV_IMM(ir, IREG_flags_op1, 0);

            codegen_flags_changed = 1;
            return op_pc + 1;

        case 0x20: /*MUL AX, rm16*/
            uop_MOVZX(ir, IREG_temp1, reg);
            uop_MOVZX(ir, IREG_temp0, IREG_AX);
            uop_IMUL(ir, IREG_temp2, IREG_temp0, IREG_temp1);
            uop_MOV(ir, IREG_AX, IREG_temp2_W);
            uop_SHR_IMM(ir, IREG_temp2, IREG_temp2, 16);
            uop_MOV(ir, IREG_DX, IREG_temp2_W);
            uop_CALL_FUNC(ir, jit_flags_rebuild);
            uop_AND_IMM(ir, IREG_flags, IREG_flags, ~(C_FLAG | V_FLAG));
            uop_MOV_IMM(ir, IREG_temp0_W, 0);
            uop_MOV_IMM(ir, IREG_temp1_W, C_FLAG | V_FLAG);
            uop_CMOV(ir, IREG_temp0_W, IREG_temp0_W, IREG_temp1_W, IREG_DX);
            uop_OR(ir, IREG_flags, IREG_flags, IREG_temp0_W);
            uop_MOV_IMM(ir, IREG_flags_op, FLAGS_UNKNOWN);
            codegen_last_flags_op = FLAGS_UNKNOWN;

            codegen_flags_changed = 1;
            return op_pc + 1;

        case 0x30: /*DIV AX, rm16*/
            if ((fetchdat & 0xc0) == 0xc0)
                uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
            uop_LOAD_FUNC_ARG_REG(ir, 0, reg);
            uop_CALL_FUNC_RESULT(ir, IREG_temp1, jit_div16);
            uop_CMP_IMM_JZ(ir, IREG_temp1, 1, codegen_exit_rout);

            codegen_flags_changed = 0;
            return op_pc + 1;

        default:
            break;
    }
    return 0;
}
uint32_t
ropF7_32(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    x86seg  *target_seg = NULL;
    uint32_t imm_data;
    int      reg;

    if ((fetchdat & 0x38) > 0x30)
        return 0;

    codegen_mark_code_present(block, cs + op_pc, 1);
    if ((fetchdat & 0xc0) == 0xc0)
        reg = IREG_32(fetchdat & 7);
    else {
        uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
        target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);
        if ((fetchdat & 0x30) == 0x10) /*NEG/NOT*/
            codegen_check_seg_write(block, ir, target_seg);
        else
            codegen_check_seg_read(block, ir, target_seg);
        uop_MEM_LOAD_REG(ir, IREG_temp0, ireg_seg_base(target_seg), IREG_eaaddr);
        reg = IREG_temp0;
    }

    switch (fetchdat & 0x38) {
        case 0x00:
        case 0x08: /*TEST*/
            imm_data = fastreadl(cs + op_pc + 1);

            uop_AND_IMM(ir, IREG_flags_res, reg, imm_data);
            uop_MOV_IMM(ir, IREG_flags_op, FLAGS_ZN32);

            codegen_flags_changed = 1;
            codegen_mark_code_present(block, cs + op_pc + 1, 4);
            return op_pc + 5;

        case 0x10: /*NOT*/
            uop_XOR_IMM(ir, reg, reg, 0xffffffff);
            if ((fetchdat & 0xc0) != 0xc0)
                uop_MEM_STORE_REG(ir, ireg_seg_base(target_seg), IREG_eaaddr, reg);

            codegen_flags_changed = 1;
            return op_pc + 1;

        case 0x18: /*NEG*/
            uop_MOV_IMM(ir, IREG_temp1, 0);

            if ((fetchdat & 0xc0) == 0xc0) {
                uop_MOV(ir, IREG_flags_op2, reg);
                uop_SUB(ir, IREG_temp1, IREG_temp1, reg);
                uop_MOV(ir, IREG_flags_res, IREG_temp1);
                uop_MOV(ir, reg, IREG_temp1);
            } else {
                uop_SUB(ir, IREG_temp1, IREG_temp1, reg);
                uop_MEM_STORE_REG(ir, ireg_seg_base(target_seg), IREG_eaaddr, IREG_temp1);
                uop_MOV(ir, IREG_flags_op2, IREG_temp0);
                uop_MOV(ir, IREG_flags_res, IREG_temp1);
            }
            uop_MOV_IMM(ir, IREG_flags_op, FLAGS_SUB32);
            uop_MOV_IMM(ir, IREG_flags_op1, 0);

            codegen_flags_changed = 1;
            return op_pc + 1;

        case 0x30: /*DIV EAX, rm32*/
            if ((fetchdat & 0xc0) == 0xc0)
                uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
            uop_LOAD_FUNC_ARG_REG(ir, 0, reg);
            uop_CALL_FUNC_RESULT(ir, IREG_temp1, jit_div32);
            uop_CMP_IMM_JZ(ir, IREG_temp1, 1, codegen_exit_rout);

            codegen_flags_changed = 0;
            return op_pc + 1;

        default:
            break;
    }
    return 0;
}

static void
rebuild_c(ir_data_t *ir)
{
    int needs_rebuild = 1;

    if (codegen_flags_changed) {
        switch (codegen_last_flags_op) {
            case FLAGS_INC8:
            case FLAGS_INC16:
            case FLAGS_INC32:
            case FLAGS_DEC8:
            case FLAGS_DEC16:
            case FLAGS_DEC32:
                needs_rebuild = 0;
                break;

            default:
                break;
        }
    }

    if (needs_rebuild) {
        uop_CALL_FUNC(ir, jit_flags_rebuild_c);
    }
}

uint32_t
ropFF_16(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    x86seg *target_seg = NULL;
    int     src_reg;
    int     sp_reg;

    if ((fetchdat & 0x38) != 0x00 && (fetchdat & 0x38) != 0x08 && (fetchdat & 0x38) != 0x10 && (fetchdat & 0x38) != 0x18 && (fetchdat & 0x38) != 0x20 && (fetchdat & 0x38) != 0x28 && (fetchdat & 0x38) != 0x30)
        return 0;

    codegen_mark_code_present(block, cs + op_pc, 1);
    if ((fetchdat & 0xc0) == 0xc0) {
        if ((fetchdat & 0x38) == 0x28 || (fetchdat & 0x38) == 0x18)
            return 0;
        src_reg = IREG_16(fetchdat & 7);
    } else {
        uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
        target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);
        if (!(fetchdat & 0x30)) /*INC/DEC*/
            codegen_check_seg_write(block, ir, target_seg);
        else
            codegen_check_seg_read(block, ir, target_seg);
        uop_MEM_LOAD_REG(ir, IREG_temp0_W, ireg_seg_base(target_seg), IREG_eaaddr);
        src_reg = IREG_temp0_W;
    }

    switch (fetchdat & 0x38) {
        case 0x00: /*INC*/
            rebuild_c(ir);
            codegen_flags_changed = 1;

            if ((fetchdat & 0xc0) == 0xc0) {
                uop_MOVZX(ir, IREG_flags_op1, src_reg);
                uop_ADD_IMM(ir, src_reg, src_reg, 1);
                uop_MOVZX(ir, IREG_flags_res, src_reg);
                uop_MOV_IMM(ir, IREG_flags_op2, 1);
                uop_MOV_IMM(ir, IREG_flags_op, FLAGS_INC16);
            } else {
                uop_ADD_IMM(ir, IREG_temp1_W, src_reg, 1);
                uop_MEM_STORE_REG(ir, ireg_seg_base(target_seg), IREG_eaaddr, IREG_temp1_W);
                uop_MOVZX(ir, IREG_flags_op1, src_reg);
                uop_MOVZX(ir, IREG_flags_res, IREG_temp1_W);
                uop_MOV_IMM(ir, IREG_flags_op2, 1);
                uop_MOV_IMM(ir, IREG_flags_op, FLAGS_INC16);
            }
            return op_pc + 1;

        case 0x08: /*DEC*/
            rebuild_c(ir);
            codegen_flags_changed = 1;

            if ((fetchdat & 0xc0) == 0xc0) {
                uop_MOVZX(ir, IREG_flags_op1, src_reg);
                uop_SUB_IMM(ir, src_reg, src_reg, 1);
                uop_MOVZX(ir, IREG_flags_res, src_reg);
                uop_MOV_IMM(ir, IREG_flags_op2, 1);
                uop_MOV_IMM(ir, IREG_flags_op, FLAGS_DEC16);
            } else {
                uop_SUB_IMM(ir, IREG_temp1_W, src_reg, 1);
                uop_MEM_STORE_REG(ir, ireg_seg_base(target_seg), IREG_eaaddr, IREG_temp1_W);
                uop_MOVZX(ir, IREG_flags_op1, src_reg);
                uop_MOVZX(ir, IREG_flags_res, IREG_temp1_W);
                uop_MOV_IMM(ir, IREG_flags_op2, 1);
                uop_MOV_IMM(ir, IREG_flags_op, FLAGS_DEC16);
            }
            return op_pc + 1;

        case 0x10: /*CALL*/
            if ((fetchdat & 0xc0) == 0xc0)
                uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
            sp_reg = LOAD_SP_WITH_OFFSET(ir, -2);
            uop_MEM_STORE_IMM_16(ir, IREG_SS_base, sp_reg, op_pc + 1);
            SUB_SP(ir, 2);
            uop_MOVZX(ir, IREG_pc, src_reg);
            return -1;

        case 0x18: /*CALL far*/
            uop_MEM_LOAD_REG_OFFSET(ir, IREG_temp1_W, ireg_seg_base(target_seg), IREG_eaaddr, 2);
            uop_LOAD_FUNC_ARG_REG(ir, 0, IREG_temp1_W);
            uop_LOAD_FUNC_ARG_REG(ir, 1, src_reg);
            uop_LOAD_FUNC_ARG_IMM(ir, 2, op_pc + 1);
            uop_CALL_FUNC(ir, call_far_16);
            return -1;

        case 0x20: /*JMP*/
            uop_MOVZX(ir, IREG_pc, src_reg);
            return -1;

        case 0x28: /*JMP far*/
            uop_MOVZX(ir, IREG_pc, src_reg);
            uop_MEM_LOAD_REG_OFFSET(ir, IREG_temp1_W, ireg_seg_base(target_seg), IREG_eaaddr, 2);
            uop_LOAD_FUNC_ARG_REG(ir, 0, IREG_temp1_W);
            uop_LOAD_FUNC_ARG_IMM(ir, 1, op_pc + 1);
            uop_CALL_FUNC(ir, loadcsjmp);
            return -1;

        case 0x30: /*PUSH*/
            if ((fetchdat & 0xc0) == 0xc0)
                uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
            sp_reg = LOAD_SP_WITH_OFFSET(ir, -2);
            uop_MEM_STORE_REG(ir, IREG_SS_base, sp_reg, src_reg);
            SUB_SP(ir, 2);
            return op_pc + 1;

        default:
            break;
    }
    return 0;
}

uint32_t
ropFF_32(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    x86seg *target_seg = NULL;
    int     src_reg;
    int     sp_reg;

    if ((fetchdat & 0x38) != 0x00 && (fetchdat & 0x38) != 0x08 && (fetchdat & 0x38) != 0x10 && (fetchdat & 0x38) != 0x18 && (fetchdat & 0x38) != 0x20 && (fetchdat & 0x38) != 0x28 && (fetchdat & 0x38) != 0x30)
        return 0;

    codegen_mark_code_present(block, cs + op_pc, 1);
    if ((fetchdat & 0xc0) == 0xc0) {
        if ((fetchdat & 0x38) == 0x28 || (fetchdat & 0x38) == 0x18)
            return 0;
        src_reg = IREG_32(fetchdat & 7);
    } else {
        uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
        target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);
        if (!(fetchdat & 0x30)) /*INC/DEC*/
            codegen_check_seg_write(block, ir, target_seg);
        else
            codegen_check_seg_read(block, ir, target_seg);
        uop_MEM_LOAD_REG(ir, IREG_temp0, ireg_seg_base(target_seg), IREG_eaaddr);
        src_reg = IREG_temp0;
    }

    switch (fetchdat & 0x38) {
        case 0x00: /*INC*/
            rebuild_c(ir);
            codegen_flags_changed = 1;

            if ((fetchdat & 0xc0) == 0xc0) {
                uop_MOV(ir, IREG_flags_op1, src_reg);
                uop_ADD_IMM(ir, src_reg, src_reg, 1);
                uop_MOV(ir, IREG_flags_res, src_reg);
                uop_MOV_IMM(ir, IREG_flags_op2, 1);
                uop_MOV_IMM(ir, IREG_flags_op, FLAGS_INC32);
            } else {
                uop_ADD_IMM(ir, IREG_temp1, src_reg, 1);
                uop_MEM_STORE_REG(ir, ireg_seg_base(target_seg), IREG_eaaddr, IREG_temp1);
                uop_MOV(ir, IREG_flags_op1, src_reg);
                uop_MOV(ir, IREG_flags_res, IREG_temp1);
                uop_MOV_IMM(ir, IREG_flags_op2, 1);
                uop_MOV_IMM(ir, IREG_flags_op, FLAGS_INC32);
            }
            return op_pc + 1;

        case 0x08: /*DEC*/
            rebuild_c(ir);
            codegen_flags_changed = 1;

            if ((fetchdat & 0xc0) == 0xc0) {
                uop_MOV(ir, IREG_flags_op1, src_reg);
                uop_SUB_IMM(ir, src_reg, src_reg, 1);
                uop_MOV(ir, IREG_flags_res, src_reg);
                uop_MOV_IMM(ir, IREG_flags_op2, 1);
                uop_MOV_IMM(ir, IREG_flags_op, FLAGS_DEC32);
            } else {
                uop_SUB_IMM(ir, IREG_temp1, src_reg, 1);
                uop_MEM_STORE_REG(ir, ireg_seg_base(target_seg), IREG_eaaddr, IREG_temp1);
                uop_MOV(ir, IREG_flags_op1, src_reg);
                uop_MOV(ir, IREG_flags_res, IREG_temp1);
                uop_MOV_IMM(ir, IREG_flags_op2, 1);
                uop_MOV_IMM(ir, IREG_flags_op, FLAGS_DEC32);
            }
            return op_pc + 1;

        case 0x10: /*CALL*/
            if ((fetchdat & 0xc0) == 0xc0)
                uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
            sp_reg = LOAD_SP_WITH_OFFSET(ir, -4);
            uop_MEM_STORE_IMM_32(ir, IREG_SS_base, sp_reg, op_pc + 1);
            SUB_SP(ir, 4);
            uop_MOV(ir, IREG_pc, src_reg);
            return -1;

        case 0x18: /*CALL far*/
            uop_MEM_LOAD_REG_OFFSET(ir, IREG_temp1_W, ireg_seg_base(target_seg), IREG_eaaddr, 4);
            uop_LOAD_FUNC_ARG_REG(ir, 0, IREG_temp1_W);
            uop_LOAD_FUNC_ARG_REG(ir, 1, src_reg);
            uop_LOAD_FUNC_ARG_IMM(ir, 2, op_pc + 1);
            uop_CALL_FUNC(ir, call_far_32);
            return -1;

        case 0x20: /*JMP*/
            uop_MOV(ir, IREG_pc, src_reg);
            return -1;

        case 0x28: /*JMP far*/
            uop_MOV(ir, IREG_pc, src_reg);
            uop_MEM_LOAD_REG_OFFSET(ir, IREG_temp1_W, ireg_seg_base(target_seg), IREG_eaaddr, 4);
            uop_LOAD_FUNC_ARG_REG(ir, 0, IREG_temp1_W);
            uop_LOAD_FUNC_ARG_IMM(ir, 1, op_pc + 1);
            uop_CALL_FUNC(ir, loadcsjmp);
            return -1;

        case 0x30: /*PUSH*/
            if ((fetchdat & 0xc0) == 0xc0)
                uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
            sp_reg = LOAD_SP_WITH_OFFSET(ir, -4);
            uop_MEM_STORE_REG(ir, IREG_SS_base, sp_reg, src_reg);
            SUB_SP(ir, 4);
            return op_pc + 1;

        default:
            break;
    }
    return 0;
}

uint32_t
ropNOP(UNUSED(codeblock_t *block), UNUSED(ir_data_t *ir), UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), UNUSED(uint32_t op_32), uint32_t op_pc)
{
    return op_pc;
}

uint32_t
ropCBW(UNUSED(codeblock_t *block), ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), UNUSED(uint32_t op_32), uint32_t op_pc)
{
    uop_MOVSX(ir, IREG_AX, IREG_AL);

    return op_pc;
}
uint32_t
ropCDQ(UNUSED(codeblock_t *block), ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), UNUSED(uint32_t op_32), uint32_t op_pc)
{
    uop_SAR_IMM(ir, IREG_EDX, IREG_EAX, 31);

    return op_pc;
}
uint32_t
ropCWD(UNUSED(codeblock_t *block), ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), UNUSED(uint32_t op_32), uint32_t op_pc)
{
    uop_SAR_IMM(ir, IREG_DX, IREG_AX, 15);

    return op_pc;
}
uint32_t
ropCWDE(UNUSED(codeblock_t *block), ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), UNUSED(uint32_t op_32), uint32_t op_pc)
{
    uop_MOVSX(ir, IREG_EAX, IREG_AX);

    return op_pc;
}

#define ropLxS(name, seg)                                                                      \
    uint32_t rop##name##_16(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode),         \
                            uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)                 \
    {                                                                                          \
        x86seg *target_seg = NULL;                                                             \
        int     dest_reg   = (fetchdat >> 3) & 7;                                              \
                                                                                               \
        if ((fetchdat & 0xc0) == 0xc0)                                                         \
            return 0;                                                                          \
                                                                                               \
        codegen_mark_code_present(block, cs + op_pc, 1);                                       \
        uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);                                          \
        target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0); \
        codegen_check_seg_read(block, ir, target_seg);                                         \
        uop_MEM_LOAD_REG(ir, IREG_temp0_W, ireg_seg_base(target_seg), (op_32 & 0x200) ? IREG_eaaddr : IREG_eaa16); \
        uop_MEM_LOAD_REG_OFFSET(ir, IREG_temp1_W, ireg_seg_base(target_seg), (op_32 & 0x200) ? IREG_eaaddr : IREG_eaa16, 2); \
        uop_LOAD_SEG(ir, seg, IREG_temp1_W);                                                   \
        uop_MOV(ir, IREG_16(dest_reg), IREG_temp0_W);                                          \
                                                                                               \
        if (seg == &cpu_state.seg_ss)                                                          \
            CPU_BLOCK_END();                                                                   \
                                                                                               \
        return op_pc + 1;                                                                      \
    }                                                                                          \
    uint32_t rop##name##_32(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode),         \
                            uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)                 \
    {                                                                                          \
        x86seg *target_seg = NULL;                                                             \
        int     dest_reg   = (fetchdat >> 3) & 7;                                              \
                                                                                               \
        if ((fetchdat & 0xc0) == 0xc0)                                                         \
            return 0;                                                                          \
                                                                                               \
        codegen_mark_code_present(block, cs + op_pc, 1);                                       \
        uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);                                          \
        target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0); \
        codegen_check_seg_read(block, ir, target_seg);                                         \
        uop_MEM_LOAD_REG(ir, IREG_temp0, ireg_seg_base(target_seg), (op_32 & 0x200) ? IREG_eaaddr : IREG_eaa16);              \
        uop_MEM_LOAD_REG_OFFSET(ir, IREG_temp1_W, ireg_seg_base(target_seg), (op_32 & 0x200) ? IREG_eaaddr : IREG_eaa16, 4);  \
        uop_LOAD_SEG(ir, seg, IREG_temp1_W);                                                   \
        uop_MOV(ir, IREG_32(dest_reg), IREG_temp0);                                            \
                                                                                               \
        if (seg == &cpu_state.seg_ss)                                                          \
            CPU_BLOCK_END();                                                                   \
                                                                                               \
        return op_pc + 1;                                                                      \
    }

// clang-format off
ropLxS(LDS, &cpu_state.seg_ds)
ropLxS(LES, &cpu_state.seg_es)
ropLxS(LFS, &cpu_state.seg_fs)
ropLxS(LGS, &cpu_state.seg_gs)
ropLxS(LSS, &cpu_state.seg_ss)
// clang-format on

uint32_t
ropCLC(UNUSED(codeblock_t *block), ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), UNUSED(uint32_t op_32), uint32_t op_pc)
{
    uop_CALL_FUNC(ir, jit_flags_rebuild);
    uop_AND_IMM(ir, IREG_flags, IREG_flags, ~C_FLAG);
    return op_pc;
}
uint32_t
ropCMC(UNUSED(codeblock_t *block), ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), UNUSED(uint32_t op_32), uint32_t op_pc)
{
    uop_CALL_FUNC(ir, jit_flags_rebuild);
    uop_XOR_IMM(ir, IREG_flags, IREG_flags, C_FLAG);
    return op_pc;
}
uint32_t
ropSTC(UNUSED(codeblock_t *block), ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), UNUSED(uint32_t op_32), uint32_t op_pc)
{
    uop_CALL_FUNC(ir, jit_flags_rebuild);
    uop_OR_IMM(ir, IREG_flags, IREG_flags, C_FLAG);
    return op_pc;
}

uint32_t
ropCLD(UNUSED(codeblock_t *block), ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), UNUSED(uint32_t op_32), uint32_t op_pc)
{
    uop_AND_IMM(ir, IREG_flags, IREG_flags, ~D_FLAG);
    return op_pc;
}
uint32_t
ropSTD(UNUSED(codeblock_t *block), ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), UNUSED(uint32_t op_32), uint32_t op_pc)
{
    uop_OR_IMM(ir, IREG_flags, IREG_flags, D_FLAG);
    return op_pc;
}

uint32_t
ropPREFETCH(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    if ((fetchdat & 0xc0) == 0xc0)
        return 0;

    codegen_mark_code_present(block, cs + op_pc, 1);
    codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);
    return op_pc + 1;
}

uint32_t
ropFEMMS(UNUSED(codeblock_t *block), ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), UNUSED(uint32_t op_32), uint32_t op_pc)
{
    uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
    uop_CALL_FUNC_RESULT(ir, IREG_temp0, codegen_femms);
    uop_CMP_IMM_JZ(ir, IREG_temp0, 1, codegen_exit_rout);

    codegen_mmx_entered = 0;
    codegen_fpu_entered = 0;
    return op_pc;
}

/* Blocks are keyed on protected mode but not on CPL or IOPL, so outside
   real mode the privilege check has to run with the block. CPL > IOPL takes
   the interpreter's CLI/STI, which raises #GP(0) or updates VIF under
   VME/PVI; Windows relies on that fault to virtualize IF. */
static uint32_t
ropCLI_STI(ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc, int set)
{
    if (cpu_cur_status & CPU_STATUS_PMODE) {
        int jump_uop;

        codegen_accumulate_flush(ir);
        uop_MOVZX_REG_PTR_8(ir, IREG_temp0, &cpu_state.seg_cs.access);
        uop_SHR_IMM(ir, IREG_temp0, IREG_temp0, 5);
        uop_AND_IMM(ir, IREG_temp0, IREG_temp0, 3);
        uop_MOVZX(ir, IREG_temp1, IREG_flags);
        uop_SHR_IMM(ir, IREG_temp1, IREG_temp1, 12);
        uop_AND_IMM(ir, IREG_temp1, IREG_temp1, 3);
        jump_uop = uop_CMP_JBE_DEST(ir, IREG_temp0, IREG_temp1);

        uop_MOV_IMM(ir, IREG_pc, op_pc);
        uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
        uop_CALL_FUNC(ir, set ? jit_sti_slow : jit_cli_slow);
        uop_JMP(ir, codegen_exit_rout);
        uop_NOP_BARRIER(ir);
        uop_set_jump_dest(ir, jump_uop);
    }

    if (set)
        uop_OR_IMM(ir, IREG_flags, IREG_flags, I_FLAG);
    else
        uop_AND_IMM(ir, IREG_flags, IREG_flags, ~I_FLAG);

    return op_pc;
}

uint32_t
ropCLI(UNUSED(codeblock_t *block), ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    return ropCLI_STI(ir, opcode, fetchdat, op_32, op_pc, 0);
}
uint32_t
ropSTI(UNUSED(codeblock_t *block), ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    return ropCLI_STI(ir, opcode, fetchdat, op_32, op_pc, 1);
}

uint32_t
ropRDTSC(UNUSED(codeblock_t *block), ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), UNUSED(uint32_t op_32), uint32_t op_pc)
{
    if (!cpu_has_feature(CPU_FEATURE_RDTSC))
        return 0;

    if (cr4 & CR4_TSD)
        return 0;

    codegen_accumulate_flush(ir);

    uop_CALL_FUNC(ir, jit_update_tsc);
    uop_MOV_REG_PTR(ir, IREG_EAX, (uint32_t *) &tsc);
    uop_MOV_REG_PTR(ir, IREG_EDX, ((uint32_t *) &tsc) + 1);

    return op_pc;
}

uint32_t
ropSETcc(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    int cond = opcode & 0x0f;

    /* Check CPU support: if illegal on this CPU, fall back to interpreter #UD handler */
    if (x86_dynarec_opcodes_0f[opcode | op_32] == x86_dynarec_opcodes_0f[0x0b])
        return 0;

    codegen_mark_code_present(block, cs + op_pc, 1);

    int fastpath_op1 = -1, fastpath_op2 = -1;
    if (codegen_get_condition_fastpath(cond, &fastpath_op1, &fastpath_op2)) {
        if (fastpath_op2 == -1) {
            uop_MOV_IMM(ir, IREG_temp0, 0);
            fastpath_op2 = IREG_temp0;
        }
        uop_SETCC(ir, IREG_temp1, fastpath_op1, fastpath_op2, cond);
    } else {
        uop_CALL_FUNC_RESULT(ir, IREG_temp1, jit_cond_helpers[cond & 0xf]);
    }

    if ((fetchdat & 0xc0) == 0xc0) {
        int dest_reg = fetchdat & 7;
        uop_MOV(ir, IREG_8(dest_reg), IREG_temp1_B);
    } else {
        x86seg *target_seg;

        uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
        target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);
        codegen_check_seg_write(block, ir, target_seg);
        CHECK_SEG_LIMITS(block, ir, target_seg, IREG_eaaddr, 0);

        uop_MEM_STORE_REG(ir, ireg_seg_base(target_seg), IREG_eaaddr, IREG_temp1_B);
    }

    return op_pc + 1;
}

enum {
    BT_OP_BT  = 0,
    BT_OP_BTS = 1,
    BT_OP_BTR = 2,
    BT_OP_BTC = 3
};

static uint32_t
ropBT_common(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc, int bt_op, int is_imm)
{
    /* Check CPU support: if illegal on this CPU, fall back to interpreter #UD handler */
    if (x86_dynarec_opcodes_0f[opcode | op_32] == x86_dynarec_opcodes_0f[0x0b])
        return 0;

    int is_op32 = !!(op_32 & 0x100);
    int is_ea32 = !!(op_32 & 0x200);
    int is_mem  = ((fetchdat & 0xc0) != 0xc0);
    x86seg *target_seg = NULL;
    int val_reg;
    uint8_t imm = 0;

    codegen_mark_code_present(block, cs + op_pc, 1);

    if (is_mem) {
        uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
        target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);

        if (!is_imm) {
            int bit_reg_idx = (fetchdat >> 3) & 7;
            if (is_op32) {
                uop_SAR_IMM(ir, IREG_temp1, IREG_32(bit_reg_idx), 5);
                uop_SHL_IMM(ir, IREG_temp1, IREG_temp1, 2);
                uop_ADD(ir, IREG_eaaddr, IREG_eaaddr, IREG_temp1);
            } else {
                uop_MOVSX(ir, IREG_temp1, IREG_16(bit_reg_idx));
                uop_SAR_IMM(ir, IREG_temp1, IREG_temp1, 4);
                uop_SHL_IMM(ir, IREG_temp1, IREG_temp1, 1);
                uop_ADD(ir, IREG_eaaddr, IREG_eaaddr, IREG_temp1);
            }
            if (!is_ea32)
                uop_MOVZX(ir, IREG_eaaddr, IREG_eaaddr_W);
        }

        if (bt_op == BT_OP_BT)
            codegen_check_seg_read(block, ir, target_seg);
        else
            codegen_check_seg_write(block, ir, target_seg);

        CHECK_SEG_LIMITS(block, ir, target_seg, IREG_eaaddr, is_op32 ? 3 : 1);

        val_reg = is_op32 ? IREG_temp0 : IREG_temp0_W;
        uop_MEM_LOAD_REG(ir, val_reg, ireg_seg_base(target_seg), IREG_eaaddr);
    } else {
        val_reg = is_op32 ? IREG_32(fetchdat & 7) : IREG_16(fetchdat & 7);
    }

    if (is_imm) {
        imm = fastreadb(cs + op_pc + 1);
        codegen_mark_code_present(block, cs + op_pc + 1, 1);
    }

    uop_CALL_FUNC(ir, jit_flags_rebuild);

    if (is_imm) {
        int bit_idx = imm & (is_op32 ? 31 : 15);
        if (is_op32) {
            uop_SHR_IMM(ir, IREG_temp2, val_reg, bit_idx);
            uop_AND_IMM(ir, IREG_temp2_W, IREG_temp2_W, 1);
            uop_AND_IMM(ir, IREG_flags, IREG_flags, ~C_FLAG);
            uop_OR(ir, IREG_flags, IREG_flags, IREG_temp2_W);

            if (bt_op == BT_OP_BTS)
                uop_OR_IMM(ir, val_reg, val_reg, (1u << bit_idx));
            else if (bt_op == BT_OP_BTR)
                uop_AND_IMM(ir, val_reg, val_reg, ~(1u << bit_idx));
            else if (bt_op == BT_OP_BTC)
                uop_XOR_IMM(ir, val_reg, val_reg, (1u << bit_idx));
        } else {
            uop_SHR_IMM(ir, IREG_temp2_W, val_reg, bit_idx);
            uop_AND_IMM(ir, IREG_temp2_W, IREG_temp2_W, 1);
            uop_AND_IMM(ir, IREG_flags, IREG_flags, ~C_FLAG);
            uop_OR(ir, IREG_flags, IREG_flags, IREG_temp2_W);

            if (bt_op == BT_OP_BTS)
                uop_OR_IMM(ir, val_reg, val_reg, (1u << bit_idx));
            else if (bt_op == BT_OP_BTR)
                uop_AND_IMM(ir, val_reg, val_reg, (~(1u << bit_idx)) & 0xffffu);
            else if (bt_op == BT_OP_BTC)
                uop_XOR_IMM(ir, val_reg, val_reg, (1u << bit_idx));
        }
    } else {
        int bit_reg_idx = (fetchdat >> 3) & 7;
        if (is_op32) {
            uop_AND_IMM(ir, IREG_temp2, IREG_32(bit_reg_idx), 31);
            uop_SHR(ir, IREG_temp3, val_reg, IREG_temp2);
            uop_AND_IMM(ir, IREG_temp3_W, IREG_temp3_W, 1);
            uop_AND_IMM(ir, IREG_flags, IREG_flags, ~C_FLAG);
            uop_OR(ir, IREG_flags, IREG_flags, IREG_temp3_W);

            if (bt_op != BT_OP_BT) {
                uop_MOV_IMM(ir, IREG_temp1, 1);
                uop_SHL(ir, IREG_temp1, IREG_temp1, IREG_temp2);
                if (bt_op == BT_OP_BTS)
                    uop_OR(ir, val_reg, val_reg, IREG_temp1);
                else if (bt_op == BT_OP_BTR) {
                    uop_XOR_IMM(ir, IREG_temp1, IREG_temp1, 0xffffffffu);
                    uop_AND(ir, val_reg, val_reg, IREG_temp1);
                } else if (bt_op == BT_OP_BTC)
                    uop_XOR(ir, val_reg, val_reg, IREG_temp1);
            }
        } else {
            uop_AND_IMM(ir, IREG_temp2_W, IREG_16(bit_reg_idx), 15);
            uop_SHR(ir, IREG_temp3_W, val_reg, IREG_temp2_W);
            uop_AND_IMM(ir, IREG_temp3_W, IREG_temp3_W, 1);
            uop_AND_IMM(ir, IREG_flags, IREG_flags, ~C_FLAG);
            uop_OR(ir, IREG_flags, IREG_flags, IREG_temp3_W);

            if (bt_op != BT_OP_BT) {
                uop_MOV_IMM(ir, IREG_temp1_W, 1);
                uop_SHL(ir, IREG_temp1_W, IREG_temp1_W, IREG_temp2_W);
                if (bt_op == BT_OP_BTS)
                    uop_OR(ir, val_reg, val_reg, IREG_temp1_W);
                else if (bt_op == BT_OP_BTR) {
                    uop_XOR_IMM(ir, IREG_temp1_W, IREG_temp1_W, 0xffffu);
                    uop_AND(ir, val_reg, val_reg, IREG_temp1_W);
                } else if (bt_op == BT_OP_BTC)
                    uop_XOR(ir, val_reg, val_reg, IREG_temp1_W);
            }
        }
    }

    if (is_mem && bt_op != BT_OP_BT) {
        uop_MEM_STORE_REG(ir, ireg_seg_base(target_seg), IREG_eaaddr, val_reg);
    }

    codegen_flags_changed = 0;
    return is_imm ? (op_pc + 2) : (op_pc + 1);
}

uint32_t
ropBT_16(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    return ropBT_common(block, ir, opcode, fetchdat, op_32, op_pc, BT_OP_BT, 0);
}

uint32_t
ropBT_32(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    return ropBT_common(block, ir, opcode, fetchdat, op_32, op_pc, BT_OP_BT, 0);
}

uint32_t
ropBTS_16(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    return ropBT_common(block, ir, opcode, fetchdat, op_32, op_pc, BT_OP_BTS, 0);
}

uint32_t
ropBTS_32(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    return ropBT_common(block, ir, opcode, fetchdat, op_32, op_pc, BT_OP_BTS, 0);
}

uint32_t
ropBTR_16(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    return ropBT_common(block, ir, opcode, fetchdat, op_32, op_pc, BT_OP_BTR, 0);
}

uint32_t
ropBTR_32(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    return ropBT_common(block, ir, opcode, fetchdat, op_32, op_pc, BT_OP_BTR, 0);
}

uint32_t
ropBTC_16(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    return ropBT_common(block, ir, opcode, fetchdat, op_32, op_pc, BT_OP_BTC, 0);
}

uint32_t
ropBTC_32(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    return ropBT_common(block, ir, opcode, fetchdat, op_32, op_pc, BT_OP_BTC, 0);
}

uint32_t
ropBA_16(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    int bt_op;
    switch (fetchdat & 0x38) {
        case 0x20: bt_op = BT_OP_BT;  break;
        case 0x28: bt_op = BT_OP_BTS; break;
        case 0x30: bt_op = BT_OP_BTR; break;
        case 0x38: bt_op = BT_OP_BTC; break;
        default:   return 0; /* Illegal opcode */
    }
    return ropBT_common(block, ir, opcode, fetchdat, op_32, op_pc, bt_op, 1);
}

uint32_t
ropBA_32(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    int bt_op;
    switch (fetchdat & 0x38) {
        case 0x20: bt_op = BT_OP_BT;  break;
        case 0x28: bt_op = BT_OP_BTS; break;
        case 0x30: bt_op = BT_OP_BTR; break;
        case 0x38: bt_op = BT_OP_BTC; break;
        default:   return 0; /* Illegal opcode */
    }
    return ropBT_common(block, ir, opcode, fetchdat, op_32, op_pc, bt_op, 1);
}

static uint32_t
ropBS_common(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc, int is_bsr)
{
    /* Check CPU support: if illegal on this CPU, fall back to interpreter #UD handler */
    if (x86_dynarec_opcodes_0f[opcode | op_32] == x86_dynarec_opcodes_0f[0x0b])
        return 0;

    int is_op32  = !!(op_32 & 0x100);
    int dest_reg = (fetchdat >> 3) & 7;
    int src_reg;

    codegen_mark_code_present(block, cs + op_pc, 1);

    if ((fetchdat & 0xc0) == 0xc0) {
        src_reg = is_op32 ? IREG_32(fetchdat & 7) : IREG_16(fetchdat & 7);
    } else {
        x86seg *target_seg;

        uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
        target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);
        codegen_check_seg_read(block, ir, target_seg);
        CHECK_SEG_LIMITS(block, ir, target_seg, IREG_eaaddr, is_op32 ? 3 : 1);

        src_reg = is_op32 ? IREG_temp0 : IREG_temp0_W;
        uop_MEM_LOAD_REG(ir, src_reg, ireg_seg_base(target_seg), IREG_eaaddr);
    }

    uop_CALL_FUNC(ir, jit_flags_rebuild);
    uop_OR_IMM(ir, IREG_flags, IREG_flags, Z_FLAG);
    int jump_zero = uop_CMP_IMM_JZ_DEST(ir, src_reg, 0);

    /* Non-zero path */
    uop_AND_IMM(ir, IREG_flags, IREG_flags, ~Z_FLAG);
    if (is_op32)
        uop_MOV(ir, IREG_flags_res, src_reg);
    else
        uop_MOVZX(ir, IREG_flags_res, src_reg);

    uop_CALL_FUNC_RESULT(ir, IREG_temp0, is_bsr ? jit_bsr : jit_bsf);

    if (is_op32)
        uop_MOV(ir, IREG_32(dest_reg), IREG_temp0);
    else
        uop_MOV(ir, IREG_16(dest_reg), IREG_temp0_W);

    /* Zero path destination (ZF is set, dest_reg unmodified) */
    uop_NOP_BARRIER(ir);
    uop_set_jump_dest(ir, jump_zero);

    codegen_flags_changed = 0;
    return op_pc + 1;
}

uint32_t
ropBSF_16(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    return ropBS_common(block, ir, opcode, fetchdat, op_32, op_pc, 0);
}

uint32_t
ropBSF_32(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    return ropBS_common(block, ir, opcode, fetchdat, op_32, op_pc, 0);
}

uint32_t
ropBSR_16(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    return ropBS_common(block, ir, opcode, fetchdat, op_32, op_pc, 1);
}

uint32_t
ropBSR_32(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    return ropBS_common(block, ir, opcode, fetchdat, op_32, op_pc, 1);
}

uint32_t
ropBSWAP(UNUSED(codeblock_t *block), ir_data_t *ir, uint8_t opcode, UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    /* Check CPU support: if illegal on this CPU, fall back to interpreter #UD handler */
    if (x86_dynarec_opcodes_0f[opcode | op_32] == x86_dynarec_opcodes_0f[0x0b])
        return 0;

    int reg = opcode & 7;
    uop_BSWAP(ir, IREG_32(reg), IREG_32(reg));
    return op_pc;
}

static uint32_t
ropCMPXCHG_common(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc, int size)
{
    /* Check CPU support: if illegal on this CPU, fall back to interpreter #UD handler */
    if (x86_dynarec_opcodes_0f[opcode | op_32] == x86_dynarec_opcodes_0f[0x0b])
        return 0;

    int src_reg = (fetchdat >> 3) & 7;

    codegen_mark_code_present(block, cs + op_pc, 1);

    if ((fetchdat & 0xc0) == 0xc0) {
        int rm_reg = fetchdat & 7;

        if (size == 0) {
            uop_MOVZX(ir, IREG_flags_op1, IREG_AL);
            uop_MOVZX(ir, IREG_flags_op2, IREG_8(rm_reg));
            uop_SUB(ir, IREG_temp1_B, IREG_AL, IREG_8(rm_reg));
            uop_MOVZX(ir, IREG_flags_res, IREG_temp1_B);
            uop_MOV_IMM(ir, IREG_flags_op, FLAGS_SUB8);
        } else if (size == 1) {
            uop_MOVZX(ir, IREG_flags_op1, IREG_AX);
            uop_MOVZX(ir, IREG_flags_op2, IREG_16(rm_reg));
            uop_SUB(ir, IREG_temp1_W, IREG_AX, IREG_16(rm_reg));
            uop_MOVZX(ir, IREG_flags_res, IREG_temp1_W);
            uop_MOV_IMM(ir, IREG_flags_op, FLAGS_SUB16);
        } else {
            uop_MOV(ir, IREG_flags_op1, IREG_EAX);
            uop_MOV(ir, IREG_flags_op2, IREG_32(rm_reg));
            uop_SUB(ir, IREG_flags_res, IREG_EAX, IREG_32(rm_reg));
            uop_MOV_IMM(ir, IREG_flags_op, FLAGS_SUB32);
        }
        codegen_flags_changed = 1;

        int jump_neq = uop_CMP_IMM_JNZ_DEST(ir, IREG_flags_res, 0);

        if (size == 0)
            uop_MOV(ir, IREG_8(rm_reg), IREG_8(src_reg));
        else if (size == 1)
            uop_MOV(ir, IREG_16(rm_reg), IREG_16(src_reg));
        else
            uop_MOV(ir, IREG_32(rm_reg), IREG_32(src_reg));

        uop_NOP_BARRIER(ir);
        int jump_end = uop_JMP_DEST(ir);

        uop_NOP_BARRIER(ir);
        uop_set_jump_dest(ir, jump_neq);
        if (size == 0)
            uop_MOV(ir, IREG_AL, IREG_8(rm_reg));
        else if (size == 1)
            uop_MOV(ir, IREG_AX, IREG_16(rm_reg));
        else
            uop_MOV(ir, IREG_EAX, IREG_32(rm_reg));

        uop_NOP_BARRIER(ir);
        uop_set_jump_dest(ir, jump_end);
    } else {
        x86seg *target_seg;

        uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
        target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);
        codegen_check_seg_write(block, ir, target_seg);
        CHECK_SEG_LIMITS(block, ir, target_seg, IREG_eaaddr, (size == 0) ? 0 : ((size == 1) ? 1 : 3));

        if (size == 0) {
            uop_MEM_LOAD_REG(ir, IREG_temp0_B, ireg_seg_base(target_seg), IREG_eaaddr);
            uop_MOVZX(ir, IREG_flags_op1, IREG_AL);
            uop_MOVZX(ir, IREG_flags_op2, IREG_temp0_B);
            uop_SUB(ir, IREG_temp1_B, IREG_AL, IREG_temp0_B);
            uop_MOVZX(ir, IREG_flags_res, IREG_temp1_B);
            uop_MOV_IMM(ir, IREG_flags_op, FLAGS_SUB8);
        } else if (size == 1) {
            uop_MEM_LOAD_REG(ir, IREG_temp0_W, ireg_seg_base(target_seg), IREG_eaaddr);
            uop_MOVZX(ir, IREG_flags_op1, IREG_AX);
            uop_MOVZX(ir, IREG_flags_op2, IREG_temp0_W);
            uop_SUB(ir, IREG_temp1_W, IREG_AX, IREG_temp0_W);
            uop_MOVZX(ir, IREG_flags_res, IREG_temp1_W);
            uop_MOV_IMM(ir, IREG_flags_op, FLAGS_SUB16);
        } else {
            uop_MEM_LOAD_REG(ir, IREG_temp0, ireg_seg_base(target_seg), IREG_eaaddr);
            uop_MOV(ir, IREG_flags_op1, IREG_EAX);
            uop_MOV(ir, IREG_flags_op2, IREG_temp0);
            uop_SUB(ir, IREG_flags_res, IREG_EAX, IREG_temp0);
            uop_MOV_IMM(ir, IREG_flags_op, FLAGS_SUB32);
        }
        codegen_flags_changed = 1;

        int jump_neq = uop_CMP_IMM_JNZ_DEST(ir, IREG_flags_res, 0);

        if (size == 0)
            uop_MEM_STORE_REG(ir, ireg_seg_base(target_seg), IREG_eaaddr, IREG_8(src_reg));
        else if (size == 1)
            uop_MEM_STORE_REG(ir, ireg_seg_base(target_seg), IREG_eaaddr, IREG_16(src_reg));
        else
            uop_MEM_STORE_REG(ir, ireg_seg_base(target_seg), IREG_eaaddr, IREG_32(src_reg));

        uop_NOP_BARRIER(ir);
        int jump_end = uop_JMP_DEST(ir);

        uop_NOP_BARRIER(ir);
        uop_set_jump_dest(ir, jump_neq);
        if (size == 0)
            uop_MOV(ir, IREG_AL, IREG_temp0_B);
        else if (size == 1)
            uop_MOV(ir, IREG_AX, IREG_temp0_W);
        else
            uop_MOV(ir, IREG_EAX, IREG_temp0);

        uop_NOP_BARRIER(ir);
        uop_set_jump_dest(ir, jump_end);
    }

    return op_pc + 1;
}

uint32_t
ropCMPXCHG_8(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    return ropCMPXCHG_common(block, ir, opcode, fetchdat, op_32, op_pc, 0);
}

uint32_t
ropCMPXCHG_16(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    return ropCMPXCHG_common(block, ir, opcode, fetchdat, op_32, op_pc, 1);
}

uint32_t
ropCMPXCHG_32(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    return ropCMPXCHG_common(block, ir, opcode, fetchdat, op_32, op_pc, 2);
}

static uint32_t
ropXADD_common(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc, int size)
{
    /* Check CPU support: if illegal on this CPU, fall back to interpreter #UD handler */
    if (x86_dynarec_opcodes_0f[opcode | op_32] == x86_dynarec_opcodes_0f[0x0b])
        return 0;

    int src_reg = (fetchdat >> 3) & 7;

    codegen_mark_code_present(block, cs + op_pc, 1);

    if ((fetchdat & 0xc0) == 0xc0) {
        int rm_reg = fetchdat & 7;

        if (size == 0) {
            uop_MOVZX(ir, IREG_flags_op1, IREG_8(rm_reg));
            uop_MOVZX(ir, IREG_flags_op2, IREG_8(src_reg));
            uop_ADD(ir, IREG_temp0_B, IREG_8(rm_reg), IREG_8(src_reg));
            uop_MOVZX(ir, IREG_flags_res, IREG_temp0_B);
            uop_MOV_IMM(ir, IREG_flags_op, FLAGS_ADD8);
            uop_MOV(ir, IREG_8(src_reg), IREG_8(rm_reg));
            uop_MOV(ir, IREG_8(rm_reg), IREG_temp0_B);
        } else if (size == 1) {
            uop_MOVZX(ir, IREG_flags_op1, IREG_16(rm_reg));
            uop_MOVZX(ir, IREG_flags_op2, IREG_16(src_reg));
            uop_ADD(ir, IREG_temp0_W, IREG_16(rm_reg), IREG_16(src_reg));
            uop_MOVZX(ir, IREG_flags_res, IREG_temp0_W);
            uop_MOV_IMM(ir, IREG_flags_op, FLAGS_ADD16);
            uop_MOV(ir, IREG_16(src_reg), IREG_16(rm_reg));
            uop_MOV(ir, IREG_16(rm_reg), IREG_temp0_W);
        } else {
            uop_MOV(ir, IREG_flags_op1, IREG_32(rm_reg));
            uop_MOV(ir, IREG_flags_op2, IREG_32(src_reg));
            uop_ADD(ir, IREG_temp0, IREG_32(rm_reg), IREG_32(src_reg));
            uop_MOV(ir, IREG_flags_res, IREG_temp0);
            uop_MOV_IMM(ir, IREG_flags_op, FLAGS_ADD32);
            uop_MOV(ir, IREG_32(src_reg), IREG_32(rm_reg));
            uop_MOV(ir, IREG_32(rm_reg), IREG_temp0);
        }
    } else {
        x86seg *target_seg;

        uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
        target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);
        codegen_check_seg_write(block, ir, target_seg);
        CHECK_SEG_LIMITS(block, ir, target_seg, IREG_eaaddr, (size == 0) ? 0 : ((size == 1) ? 1 : 3));

        if (size == 0) {
            uop_MEM_LOAD_REG(ir, IREG_temp0_B, ireg_seg_base(target_seg), IREG_eaaddr);
            uop_ADD(ir, IREG_temp1_B, IREG_temp0_B, IREG_8(src_reg));
            uop_MEM_STORE_REG(ir, ireg_seg_base(target_seg), IREG_eaaddr, IREG_temp1_B);
            uop_MOVZX(ir, IREG_flags_op1, IREG_temp0_B);
            uop_MOVZX(ir, IREG_flags_op2, IREG_8(src_reg));
            uop_MOVZX(ir, IREG_flags_res, IREG_temp1_B);
            uop_MOV_IMM(ir, IREG_flags_op, FLAGS_ADD8);
            uop_MOV(ir, IREG_8(src_reg), IREG_temp0_B);
        } else if (size == 1) {
            uop_MEM_LOAD_REG(ir, IREG_temp0_W, ireg_seg_base(target_seg), IREG_eaaddr);
            uop_ADD(ir, IREG_temp1_W, IREG_temp0_W, IREG_16(src_reg));
            uop_MEM_STORE_REG(ir, ireg_seg_base(target_seg), IREG_eaaddr, IREG_temp1_W);
            uop_MOVZX(ir, IREG_flags_op1, IREG_temp0_W);
            uop_MOVZX(ir, IREG_flags_op2, IREG_16(src_reg));
            uop_MOVZX(ir, IREG_flags_res, IREG_temp1_W);
            uop_MOV_IMM(ir, IREG_flags_op, FLAGS_ADD16);
            uop_MOV(ir, IREG_16(src_reg), IREG_temp0_W);
        } else {
            uop_MEM_LOAD_REG(ir, IREG_temp0, ireg_seg_base(target_seg), IREG_eaaddr);
            uop_ADD(ir, IREG_temp1, IREG_temp0, IREG_32(src_reg));
            uop_MEM_STORE_REG(ir, ireg_seg_base(target_seg), IREG_eaaddr, IREG_temp1);
            uop_MOV(ir, IREG_flags_op1, IREG_temp0);
            uop_MOV(ir, IREG_flags_op2, IREG_32(src_reg));
            uop_MOV(ir, IREG_flags_res, IREG_temp1);
            uop_MOV_IMM(ir, IREG_flags_op, FLAGS_ADD32);
            uop_MOV(ir, IREG_32(src_reg), IREG_temp0);
        }
    }

    codegen_flags_changed = 1;
    return op_pc + 1;
}

uint32_t
ropXADD_8(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    return ropXADD_common(block, ir, opcode, fetchdat, op_32, op_pc, 0);
}

uint32_t
ropXADD_16(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    return ropXADD_common(block, ir, opcode, fetchdat, op_32, op_pc, 1);
}

uint32_t
ropXADD_32(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    return ropXADD_common(block, ir, opcode, fetchdat, op_32, op_pc, 2);
}

uint32_t
rop0F00(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    /* Group 6: SLDT (/0), STR (/1), LLDT (/2), LTR (/3), VERR (/4), VERW (/5) */
    int sub_op = (fetchdat >> 3) & 7;
    if (sub_op != 4 && sub_op != 5)
        return 0; /* Fall back to interpreter for SLDT, STR, LLDT, LTR */

    int src_reg;
    codegen_mark_code_present(block, cs + op_pc, 1);

    if ((fetchdat & 0xc0) == 0xc0) {
        src_reg = IREG_16(fetchdat & 7);
    } else {
        x86seg *target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);
        codegen_check_seg_read(block, ir, target_seg);
        CHECK_SEG_LIMITS(block, ir, target_seg, IREG_eaaddr, 1);
        src_reg = IREG_temp0_W;
        uop_MEM_LOAD_REG(ir, src_reg, ireg_seg_base(target_seg), IREG_eaaddr);
    }

    codegen_accumulate_flush(ir);
    uop_LOAD_FUNC_ARG_REG(ir, 0, src_reg);
    uop_CALL_FUNC(ir, (sub_op == 5) ? jit_verw : jit_verr);
    codegen_flags_changed = 0;

    return op_pc + 1;
}

uint32_t
rop0F01(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    /* Group 7: SGDT (/0), SIDT (/1), LGDT (/2), LIDT (/3), SMSW (/4), LMSW (/6), INVLPG (/7) */
    int sub_op = (fetchdat >> 3) & 7;
    if (sub_op != 4)
        return 0; /* Fall back to interpreter for non-SMSW instructions */

    codegen_mark_code_present(block, cs + op_pc, 1);

    if ((fetchdat & 0xc0) == 0xc0) {
        codegen_accumulate_flush(ir);
        if (op_32 & 0x100) {
            uop_CALL_FUNC_RESULT(ir, IREG_32(fetchdat & 7), (void *) jit_smsw32);
        } else {
            uop_CALL_FUNC_RESULT(ir, IREG_temp0, (void *) jit_smsw16);
            uop_MOV(ir, IREG_16(fetchdat & 7), IREG_temp0_W);
        }
    } else {
        x86seg *target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);
        codegen_check_seg_write(block, ir, target_seg);
        CHECK_SEG_LIMITS(block, ir, target_seg, IREG_eaaddr, 1);
        codegen_accumulate_flush(ir);
        uop_CALL_FUNC_RESULT(ir, IREG_temp0, (void *) jit_smsw16);
        uop_MEM_STORE_REG(ir, ireg_seg_base(target_seg), IREG_eaaddr, IREG_temp0_W);
    }

    return op_pc + 1;
}


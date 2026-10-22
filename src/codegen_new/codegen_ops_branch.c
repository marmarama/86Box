#include <stdint.h>
#include <86box/86box.h>
#include "cpu.h"
#include <86box/mem.h>
#include <86box/plat_unused.h>

#include "x86.h"
#include "x86seg_common.h"
#include "x86seg.h"
#include "386_common.h"
#include "x86_flags.h"
#include "codegen.h"
#include "codegen_backend.h"
#include "codegen_ir.h"
#include "codegen_ops.h"
#include "codegen_ops_helpers.h"
#include "codegen_ops_jit_wrappers.h"
#include "codegen_ops_mov.h"
#include "codegen_ops_branch.h"


const jit_cond_fn_t jit_cond_helpers[16] = {
    jit_cond_O,   jit_cond_NO,  jit_cond_B,   jit_cond_NB,
    jit_cond_E,   jit_cond_NE,  jit_cond_BE,  jit_cond_NBE,
    jit_cond_S,   jit_cond_NS,  jit_cond_P,   jit_cond_NP,
    jit_cond_L,   jit_cond_NL,  jit_cond_LE,  jit_cond_NLE,
};

int
codegen_cond_jump_false(ir_data_t *ir, int cond)
{
    if (codegen_flags_changed && codegen_last_flags_op != FLAGS_UNKNOWN) {
        int cond_base = cond & 0xf;

        /* Fast-path 1: Zero Flag (COND_E / COND_NE) for all operations where ZF is (flags_res == 0) */
        if (is_zf_flags_res_op(codegen_last_flags_op)) {
            if (cond_base == COND_E) {
                /* Equal (ZF == 1 / flags_res == 0): jump false if not equal (flags_res != 0) */
                return uop_CMP_IMM_JNZ_DEST(ir, IREG_flags_res, 0);
            } else if (cond_base == COND_NE) {
                /* Not Equal (ZF == 0 / flags_res != 0): jump false if equal (flags_res == 0) */
                return uop_CMP_IMM_JZ_DEST(ir, IREG_flags_res, 0);
            }
        }

        /* Fast-path 2: Comparison conditions (B, NB, BE, NBE, L, NL, LE, NLE) after CMP and SUB */
        if (codegen_last_flags_op == FLAGS_SUB32 ||
            codegen_last_flags_op == FLAGS_SUB16 ||
            codegen_last_flags_op == FLAGS_SUB8) {
            int op1 = (codegen_last_flags_op == FLAGS_SUB32) ? IREG_flags_op1 :
                      (codegen_last_flags_op == FLAGS_SUB16) ? IREG_flags_op1_W : IREG_flags_op1_B;
            int op2 = (codegen_last_flags_op == FLAGS_SUB32) ? IREG_flags_op2 :
                      (codegen_last_flags_op == FLAGS_SUB16) ? IREG_flags_op2_W : IREG_flags_op2_B;

            switch (cond_base) {
                case COND_B:   /* op1 < op2 (unsigned) -> jump false if op1 >= op2 */
                    return uop_CMP_JNB_DEST(ir, op1, op2);
                case COND_NB:  /* op1 >= op2 (unsigned) -> jump false if op1 < op2 */
                    return uop_CMP_JB_DEST(ir, op1, op2);
                case COND_BE:  /* op1 <= op2 (unsigned) -> jump false if op1 > op2 */
                    return uop_CMP_JNBE_DEST(ir, op1, op2);
                case COND_NBE: /* op1 > op2 (unsigned) -> jump false if op1 <= op2 */
                    return uop_CMP_JBE_DEST(ir, op1, op2);
                case COND_L:   /* op1 < op2 (signed) -> jump false if op1 >= op2 */
                    return uop_CMP_JNL_DEST(ir, op1, op2);
                case COND_NL:  /* op1 >= op2 (signed) -> jump false if op1 < op2 */
                    return uop_CMP_JL_DEST(ir, op1, op2);
                case COND_LE:  /* op1 <= op2 (signed) -> jump false if op1 > op2 */
                    return uop_CMP_JNLE_DEST(ir, op1, op2);
                case COND_NLE: /* op1 > op2 (signed) -> jump false if op1 <= op2 */
                    return uop_CMP_JLE_DEST(ir, op1, op2);
                default:
                    break;
            }
        }

        /* Fast-path 3: Sign Flag (COND_S / COND_NS) */
        if (cond_base == COND_S || cond_base == COND_NS) {
            int res_sized = -1;
            switch (codegen_last_flags_op) {
                case FLAGS_ZN32:
                case FLAGS_ADD32:
                case FLAGS_SUB32:
                case FLAGS_INC32:
                case FLAGS_DEC32:
                case FLAGS_ADC32:
                case FLAGS_SBC32:
                case FLAGS_SHL32:
                case FLAGS_SHR32:
                case FLAGS_SAR32:
                case FLAGS_SHRD32:
                    res_sized = IREG_flags_res;
                    break;
                case FLAGS_ZN16:
                case FLAGS_ADD16:
                case FLAGS_SUB16:
                case FLAGS_INC16:
                case FLAGS_DEC16:
                case FLAGS_ADC16:
                case FLAGS_SBC16:
                case FLAGS_SHL16:
                case FLAGS_SHR16:
                case FLAGS_SAR16:
                case FLAGS_SHRD16:
                    res_sized = IREG_flags_res_W;
                    break;
                case FLAGS_ZN8:
                case FLAGS_ADD8:
                case FLAGS_SUB8:
                case FLAGS_INC8:
                case FLAGS_DEC8:
                case FLAGS_ADC8:
                case FLAGS_SBC8:
                case FLAGS_SHL8:
                case FLAGS_SHR8:
                case FLAGS_SAR8:
                    res_sized = IREG_flags_res_B;
                    break;
                default:
                    break;
            }
            if (res_sized != -1) {
                if (cond_base == COND_S) {
                    /* Negative (SF == 1): jump false if positive (SF == 0) */
                    return uop_TEST_JNS_DEST(ir, res_sized);
                } else {
                    /* Positive (SF == 0): jump false if negative (SF == 1) */
                    return uop_TEST_JS_DEST(ir, res_sized);
                }
            }
        }
    }

    uop_CALL_FUNC_RESULT(ir, IREG_temp0, jit_cond_helpers[cond & 0xf]);
    return uop_CMP_IMM_JZ_DEST(ir, IREG_temp0, 0);
}

#define ropJ(cond, cond_enum)                                           \
    uint32_t ropJ##cond##_8(codeblock_t *block,                         \
                            ir_data_t *ir,                              \
                            UNUSED(uint8_t opcode),                     \
                            UNUSED(uint32_t fetchdat),                  \
                            uint32_t op_32,                             \
                            uint32_t op_pc)                             \
    {                                                                   \
        uint32_t offset    = (int32_t) (int8_t) fastreadb(cs + op_pc);  \
        uint32_t dest_addr = op_pc + 1 + offset;                        \
        uint32_t next_pc   = op_pc + 1;                                 \
                                                                        \
        if (!(op_32 & 0x100)) {                                         \
            dest_addr &= 0xffff;                                        \
            next_pc   &= 0xffff;                                        \
        }                                                               \
        uop_MOV_IMM(ir, IREG_pc, next_pc);                              \
        codegen_mark_code_present(block, cs + op_pc, 1);                \
        block->exit_type          = BLOCK_EXIT_COND_2;                  \
        block->exit_cond          = (cond_enum);                        \
        block->links[0].target_pc = dest_addr;                          \
        block->links[1].target_pc = next_pc;                            \
        CPU_BLOCK_END();                                                \
        return -1;                                                      \
    }                                                                   \
    uint32_t ropJ##cond##_16(codeblock_t *block,                        \
                             ir_data_t *ir,                             \
                             UNUSED(uint8_t opcode),                    \
                             UNUSED(uint32_t fetchdat),                 \
                             UNUSED(uint32_t op_32),                    \
                             uint32_t op_pc)                            \
    {                                                                   \
        uint32_t offset    = (int32_t) (int16_t) fastreadw(cs + op_pc); \
        uint32_t dest_addr = (op_pc + 2 + offset) & 0xffff;             \
        uint32_t next_pc   = (op_pc + 2) & 0xffff;                      \
                                                                        \
        uop_MOV_IMM(ir, IREG_pc, next_pc);                              \
        codegen_mark_code_present(block, cs + op_pc, 2);                \
        block->exit_type          = BLOCK_EXIT_COND_2;                  \
        block->exit_cond          = (cond_enum);                        \
        block->links[0].target_pc = dest_addr;                          \
        block->links[1].target_pc = next_pc;                            \
        CPU_BLOCK_END();                                                \
        return -1;                                                      \
    }                                                                   \
    uint32_t ropJ##cond##_32(codeblock_t *block,                        \
                             ir_data_t *ir,                             \
                             UNUSED(uint8_t opcode),                    \
                             UNUSED(uint32_t fetchdat),                 \
                             UNUSED(uint32_t op_32),                    \
                             uint32_t op_pc)                            \
    {                                                                   \
        uint32_t offset    = fastreadl(cs + op_pc);                     \
        uint32_t dest_addr = op_pc + 4 + offset;                        \
        uint32_t next_pc   = op_pc + 4;                                 \
                                                                        \
        uop_MOV_IMM(ir, IREG_pc, next_pc);                              \
        codegen_mark_code_present(block, cs + op_pc, 4);                \
        block->exit_type          = BLOCK_EXIT_COND_2;                  \
        block->exit_cond          = (cond_enum);                        \
        block->links[0].target_pc = dest_addr;                          \
        block->links[1].target_pc = next_pc;                            \
        CPU_BLOCK_END();                                                \
        return -1;                                                      \
    }

// clang-format off
ropJ(O, COND_O)
ropJ(NO, COND_NO)
ropJ(B, COND_B)
ropJ(NB, COND_NB)
ropJ(E, COND_E)
ropJ(NE, COND_NE)
ropJ(BE, COND_BE)
ropJ(NBE, COND_NBE)
ropJ(S, COND_S)
ropJ(NS, COND_NS)
ropJ(P, COND_P)
ropJ(NP, COND_NP)
ropJ(L, COND_L)
ropJ(NL, COND_NL)
ropJ(LE, COND_LE)
ropJ(NLE, COND_NLE)
// clang-format on

uint32_t
ropJCXZ(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    uint32_t offset    = (int32_t) (int8_t) fastreadb(cs + op_pc);
    uint32_t dest_addr = op_pc + 1 + offset;
    int      jump_uop;

    if (!(op_32 & 0x100))
        dest_addr &= 0xffff;

    if (op_32 & 0x200)
        jump_uop = uop_CMP_IMM_JNZ_DEST(ir, IREG_ECX, 0);
    else
        jump_uop = uop_CMP_IMM_JNZ_DEST(ir, IREG_CX, 0);
    uop_MOV_IMM(ir, IREG_pc, dest_addr);
    uop_JMP(ir, codegen_exit_rout);
    uop_NOP_BARRIER(ir);
    uop_set_jump_dest(ir, jump_uop);

    codegen_mark_code_present(block, cs + op_pc, 1);
    return op_pc + 1;
}

uint32_t
ropLOOP(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    uint32_t offset    = (int32_t) (int8_t) fastreadb(cs + op_pc);
    uint32_t dest_addr = op_pc + 1 + offset;
    uint32_t ret_addr;
    int      jump_uop;

    if (!(op_32 & 0x100))
        dest_addr &= 0xffff;

    if (((op_32 & 0x200) ? ECX : CX) != 1 && codegen_can_unroll(block, ir, op_pc + 1, dest_addr)) {
        if (op_32 & 0x200) {
            uop_SUB_IMM(ir, IREG_ECX, IREG_ECX, 1);
            jump_uop = uop_CMP_IMM_JNZ_DEST(ir, IREG_ECX, 0);
        } else {
            uop_SUB_IMM(ir, IREG_CX, IREG_CX, 1);
            jump_uop = uop_CMP_IMM_JNZ_DEST(ir, IREG_CX, 0);
        }
        uop_MOV_IMM(ir, IREG_pc, op_pc + 1);
        ret_addr = dest_addr;
        CPU_BLOCK_END();
    } else {
        if (op_32 & 0x200) {
            uop_SUB_IMM(ir, IREG_ECX, IREG_ECX, 1);
            jump_uop = uop_CMP_IMM_JZ_DEST(ir, IREG_ECX, 0);
        } else {
            uop_SUB_IMM(ir, IREG_CX, IREG_CX, 1);
            jump_uop = uop_CMP_IMM_JZ_DEST(ir, IREG_CX, 0);
        }
        uop_MOV_IMM(ir, IREG_pc, dest_addr);
        ret_addr = op_pc + 1;
    }
    uop_JMP(ir, codegen_exit_rout);
    uop_NOP_BARRIER(ir);
    uop_set_jump_dest(ir, jump_uop);

    codegen_mark_code_present(block, cs + op_pc, 1);
    return ret_addr;
}

uint32_t
ropLOOPE(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    uint32_t offset    = (int32_t) (int8_t) fastreadb(cs + op_pc);
    uint32_t dest_addr = op_pc + 1 + offset;
    int      jump_uop;
    int      jump_uop2;

    if (!(op_32 & 0x100))
        dest_addr &= 0xffff;

    if (op_32 & 0x200) {
        uop_SUB_IMM(ir, IREG_ECX, IREG_ECX, 1);
        jump_uop = uop_CMP_IMM_JZ_DEST(ir, IREG_ECX, 0);
    } else {
        uop_SUB_IMM(ir, IREG_CX, IREG_CX, 1);
        jump_uop = uop_CMP_IMM_JZ_DEST(ir, IREG_CX, 0);
    }
    if (!codegen_flags_changed || !flags_res_valid()) {
        uop_CALL_FUNC_RESULT(ir, IREG_temp0, jit_ZF_SET);
        jump_uop2 = uop_CMP_IMM_JZ_DEST(ir, IREG_temp0, 0);
    } else {
        jump_uop2 = uop_CMP_IMM_JNZ_DEST(ir, IREG_flags_res, 0);
    }
    uop_MOV_IMM(ir, IREG_pc, dest_addr);
    uop_JMP(ir, codegen_exit_rout);
    uop_NOP_BARRIER(ir);
    uop_set_jump_dest(ir, jump_uop);
    uop_set_jump_dest(ir, jump_uop2);

    codegen_mark_code_present(block, cs + op_pc, 1);
    return op_pc + 1;
}

uint32_t
ropLOOPNE(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    uint32_t offset    = (int32_t) (int8_t) fastreadb(cs + op_pc);
    uint32_t dest_addr = op_pc + 1 + offset;
    int      jump_uop;
    int      jump_uop2;

    if (!(op_32 & 0x100))
        dest_addr &= 0xffff;

    if (op_32 & 0x200) {
        uop_SUB_IMM(ir, IREG_ECX, IREG_ECX, 1);
        jump_uop = uop_CMP_IMM_JZ_DEST(ir, IREG_ECX, 0);
    } else {
        uop_SUB_IMM(ir, IREG_CX, IREG_CX, 1);
        jump_uop = uop_CMP_IMM_JZ_DEST(ir, IREG_CX, 0);
    }
    if (!codegen_flags_changed || !flags_res_valid()) {
        uop_CALL_FUNC_RESULT(ir, IREG_temp0, jit_ZF_SET);
        jump_uop2 = uop_CMP_IMM_JNZ_DEST(ir, IREG_temp0, 0);
    } else {
        jump_uop2 = uop_CMP_IMM_JZ_DEST(ir, IREG_flags_res, 0);
    }
    uop_MOV_IMM(ir, IREG_pc, dest_addr);
    uop_JMP(ir, codegen_exit_rout);
    uop_NOP_BARRIER(ir);
    uop_set_jump_dest(ir, jump_uop);
    uop_set_jump_dest(ir, jump_uop2);

    codegen_mark_code_present(block, cs + op_pc, 1);
    return op_pc + 1;
}


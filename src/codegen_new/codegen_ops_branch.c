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


typedef int (*jit_cond_fn_t)(void);
static const jit_cond_fn_t jit_cond_helpers[16] = {
    jit_cond_O,   jit_cond_NO,  jit_cond_B,   jit_cond_NB,
    jit_cond_E,   jit_cond_NE,  jit_cond_BE,  jit_cond_NBE,
    jit_cond_S,   jit_cond_NS,  jit_cond_P,   jit_cond_NP,
    jit_cond_L,   jit_cond_NL,  jit_cond_LE,  jit_cond_NLE,
};

int
codegen_cond_jump_false(ir_data_t *ir, int cond)
{
    uop_CALL_FUNC_RESULT(ir, IREG_temp0, jit_cond_helpers[cond & 0xf]);
    return uop_CMP_IMM_JZ_DEST(ir, IREG_temp0, 0);
}

#define ropJ(cond, cond_enum)                                           \
    uint32_t ropJ##cond##_8(codeblock_t *block,                         \
                            UNUSED(ir_data_t *ir),                      \
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
        codegen_mark_code_present(block, cs + op_pc, 1);                \
        block->exit_type          = BLOCK_EXIT_COND_2;                  \
        block->exit_cond          = (cond_enum);                        \
        block->links[0].target_pc = dest_addr;                          \
        block->links[1].target_pc = next_pc;                            \
        CPU_BLOCK_END();                                                \
        return next_pc;                                                 \
    }                                                                   \
    uint32_t ropJ##cond##_16(codeblock_t *block,                        \
                             UNUSED(ir_data_t *ir),                     \
                             UNUSED(uint8_t opcode),                    \
                             UNUSED(uint32_t fetchdat),                 \
                             UNUSED(uint32_t op_32),                    \
                             uint32_t op_pc)                            \
    {                                                                   \
        uint32_t offset    = (int32_t) (int16_t) fastreadw(cs + op_pc); \
        uint32_t dest_addr = (op_pc + 2 + offset) & 0xffff;             \
        uint32_t next_pc   = (op_pc + 2) & 0xffff;                      \
                                                                        \
        codegen_mark_code_present(block, cs + op_pc, 2);                \
        block->exit_type          = BLOCK_EXIT_COND_2;                  \
        block->exit_cond          = (cond_enum);                        \
        block->links[0].target_pc = dest_addr;                          \
        block->links[1].target_pc = next_pc;                            \
        CPU_BLOCK_END();                                                \
        return next_pc;                                                 \
    }                                                                   \
    uint32_t ropJ##cond##_32(codeblock_t *block,                        \
                             UNUSED(ir_data_t *ir),                     \
                             UNUSED(uint8_t opcode),                    \
                             UNUSED(uint32_t fetchdat),                 \
                             UNUSED(uint32_t op_32),                    \
                             uint32_t op_pc)                            \
    {                                                                   \
        uint32_t offset    = fastreadl(cs + op_pc);                     \
        uint32_t dest_addr = op_pc + 4 + offset;                        \
        uint32_t next_pc   = op_pc + 4;                                 \
                                                                        \
        codegen_mark_code_present(block, cs + op_pc, 4);                \
        block->exit_type          = BLOCK_EXIT_COND_2;                  \
        block->exit_cond          = (cond_enum);                        \
        block->links[0].target_pc = dest_addr;                          \
        block->links[1].target_pc = next_pc;                            \
        CPU_BLOCK_END();                                                \
        return next_pc;                                                 \
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


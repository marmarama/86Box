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
    int flags_op = codegen_flags_changed ? cpu_state.flags_op : FLAGS_UNKNOWN;

    switch (cond & 0xf) {
        case 0: /* O: false when OF=0 */
            switch (flags_op) {
                case FLAGS_ZN8: case FLAGS_ZN16: case FLAGS_ZN32:
                    return uop_JMP_DEST(ir);
                case FLAGS_SUB8: case FLAGS_DEC8:
                    return uop_CMP_JNO_DEST(ir, IREG_flags_op1_B, IREG_flags_op2_B);
                case FLAGS_SUB16: case FLAGS_DEC16:
                    return uop_CMP_JNO_DEST(ir, IREG_flags_op1_W, IREG_flags_op2_W);
                case FLAGS_SUB32: case FLAGS_DEC32:
                    return uop_CMP_JNO_DEST(ir, IREG_flags_op1, IREG_flags_op2);
                default:
                    break;
            }
            break;

        case 1: /* NO: false when OF=1 */
            switch (flags_op) {
                case FLAGS_ZN8: case FLAGS_ZN16: case FLAGS_ZN32:
                    return -1;
                case FLAGS_SUB8: case FLAGS_DEC8:
                    return uop_CMP_JO_DEST(ir, IREG_flags_op1_B, IREG_flags_op2_B);
                case FLAGS_SUB16: case FLAGS_DEC16:
                    return uop_CMP_JO_DEST(ir, IREG_flags_op1_W, IREG_flags_op2_W);
                case FLAGS_SUB32: case FLAGS_DEC32:
                    return uop_CMP_JO_DEST(ir, IREG_flags_op1, IREG_flags_op2);
                default:
                    break;
            }
            break;

        case 2: /* B: false when CF=0 */
            switch (flags_op) {
                case FLAGS_ZN8: case FLAGS_ZN16: case FLAGS_ZN32:
                    return uop_JMP_DEST(ir);
                case FLAGS_SUB8:
                    return uop_CMP_JNB_DEST(ir, IREG_flags_op1_B, IREG_flags_op2_B);
                case FLAGS_SUB16:
                    return uop_CMP_JNB_DEST(ir, IREG_flags_op1_W, IREG_flags_op2_W);
                case FLAGS_SUB32:
                    return uop_CMP_JNB_DEST(ir, IREG_flags_op1, IREG_flags_op2);
                default:
                    break;
            }
            break;

        case 3: /* NB: false when CF=1 */
            switch (flags_op) {
                case FLAGS_ZN8: case FLAGS_ZN16: case FLAGS_ZN32:
                    return -1;
                case FLAGS_SUB8:
                    return uop_CMP_JB_DEST(ir, IREG_flags_op1_B, IREG_flags_op2_B);
                case FLAGS_SUB16:
                    return uop_CMP_JB_DEST(ir, IREG_flags_op1_W, IREG_flags_op2_W);
                case FLAGS_SUB32:
                    return uop_CMP_JB_DEST(ir, IREG_flags_op1, IREG_flags_op2);
                default:
                    break;
            }
            break;

        case 4: /* E / Z: false when ZF=0 */
            if (codegen_flags_changed && flags_res_valid())
                return uop_CMP_IMM_JNZ_DEST(ir, IREG_flags_res, 0);
            break;

        case 5: /* NE / NZ: false when ZF=1 */
            if (codegen_flags_changed && flags_res_valid())
                return uop_CMP_IMM_JZ_DEST(ir, IREG_flags_res, 0);
            break;

        case 6: /* BE: false when CF=0 && ZF=0 (JNBE) */
            switch (flags_op) {
                case FLAGS_ZN8: case FLAGS_ZN16: case FLAGS_ZN32:
                    return uop_CMP_IMM_JNZ_DEST(ir, IREG_flags_res, 0);
                case FLAGS_SUB8:
                    return uop_CMP_JNBE_DEST(ir, IREG_flags_op1_B, IREG_flags_op2_B);
                case FLAGS_SUB16:
                    return uop_CMP_JNBE_DEST(ir, IREG_flags_op1_W, IREG_flags_op2_W);
                case FLAGS_SUB32:
                    return uop_CMP_JNBE_DEST(ir, IREG_flags_op1, IREG_flags_op2);
                default:
                    break;
            }
            break;

        case 7: /* NBE: false when CF=1 || ZF=1 (JBE) */
            switch (flags_op) {
                case FLAGS_ZN8: case FLAGS_ZN16: case FLAGS_ZN32:
                    return uop_CMP_IMM_JZ_DEST(ir, IREG_flags_res, 0);
                case FLAGS_SUB8:
                    return uop_CMP_JBE_DEST(ir, IREG_flags_op1_B, IREG_flags_op2_B);
                case FLAGS_SUB16:
                    return uop_CMP_JBE_DEST(ir, IREG_flags_op1_W, IREG_flags_op2_W);
                case FLAGS_SUB32:
                    return uop_CMP_JBE_DEST(ir, IREG_flags_op1, IREG_flags_op2);
                default:
                    break;
            }
            break;

        case 8: /* S: false when SF=0 (JNS) */
            switch (flags_op) {
                case FLAGS_ZN8: case FLAGS_ADD8: case FLAGS_SUB8:
                case FLAGS_SHL8: case FLAGS_SHR8: case FLAGS_SAR8:
                case FLAGS_INC8: case FLAGS_DEC8:
                    return uop_TEST_JNS_DEST(ir, IREG_flags_res_B);
                case FLAGS_ZN16: case FLAGS_ADD16: case FLAGS_SUB16:
                case FLAGS_SHL16: case FLAGS_SHR16: case FLAGS_SAR16:
                case FLAGS_INC16: case FLAGS_DEC16:
                    return uop_TEST_JNS_DEST(ir, IREG_flags_res_W);
                case FLAGS_ZN32: case FLAGS_ADD32: case FLAGS_SUB32:
                case FLAGS_SHL32: case FLAGS_SHR32: case FLAGS_SAR32:
                case FLAGS_INC32: case FLAGS_DEC32:
                    return uop_TEST_JNS_DEST(ir, IREG_flags_res);
                default:
                    break;
            }
            break;

        case 9: /* NS: false when SF=1 (JS) */
            switch (flags_op) {
                case FLAGS_ZN8: case FLAGS_ADD8: case FLAGS_SUB8:
                case FLAGS_SHL8: case FLAGS_SHR8: case FLAGS_SAR8:
                case FLAGS_INC8: case FLAGS_DEC8:
                    return uop_TEST_JS_DEST(ir, IREG_flags_res_B);
                case FLAGS_ZN16: case FLAGS_ADD16: case FLAGS_SUB16:
                case FLAGS_SHL16: case FLAGS_SHR16: case FLAGS_SAR16:
                case FLAGS_INC16: case FLAGS_DEC16:
                    return uop_TEST_JS_DEST(ir, IREG_flags_res_W);
                case FLAGS_ZN32: case FLAGS_ADD32: case FLAGS_SUB32:
                case FLAGS_SHL32: case FLAGS_SHR32: case FLAGS_SAR32:
                case FLAGS_INC32: case FLAGS_DEC32:
                    return uop_TEST_JS_DEST(ir, IREG_flags_res);
                default:
                    break;
            }
            break;

        case 12: /* L: false when SF == OF (JNL) */
            switch (flags_op) {
                case FLAGS_ZN8:
                    return uop_TEST_JNS_DEST(ir, IREG_flags_res_B);
                case FLAGS_ZN16:
                    return uop_TEST_JNS_DEST(ir, IREG_flags_res_W);
                case FLAGS_ZN32:
                    return uop_TEST_JNS_DEST(ir, IREG_flags_res);
                case FLAGS_SUB8: case FLAGS_DEC8:
                    return uop_CMP_JNL_DEST(ir, IREG_flags_op1_B, IREG_flags_op2_B);
                case FLAGS_SUB16: case FLAGS_DEC16:
                    return uop_CMP_JNL_DEST(ir, IREG_flags_op1_W, IREG_flags_op2_W);
                case FLAGS_SUB32: case FLAGS_DEC32:
                    return uop_CMP_JNL_DEST(ir, IREG_flags_op1, IREG_flags_op2);
                default:
                    break;
            }
            break;

        case 13: /* NL: false when SF != OF (JL) */
            switch (flags_op) {
                case FLAGS_ZN8:
                    return uop_TEST_JS_DEST(ir, IREG_flags_res_B);
                case FLAGS_ZN16:
                    return uop_TEST_JS_DEST(ir, IREG_flags_res_W);
                case FLAGS_ZN32:
                    return uop_TEST_JS_DEST(ir, IREG_flags_res);
                case FLAGS_SUB8: case FLAGS_DEC8:
                    return uop_CMP_JL_DEST(ir, IREG_flags_op1_B, IREG_flags_op2_B);
                case FLAGS_SUB16: case FLAGS_DEC16:
                    return uop_CMP_JL_DEST(ir, IREG_flags_op1_W, IREG_flags_op2_W);
                case FLAGS_SUB32: case FLAGS_DEC32:
                    return uop_CMP_JL_DEST(ir, IREG_flags_op1, IREG_flags_op2);
                default:
                    break;
            }
            break;

        case 14: /* LE: false when ZF=0 && SF == OF (JNLE) */
            switch (flags_op) {
                case FLAGS_SUB8: case FLAGS_DEC8:
                    return uop_CMP_JNLE_DEST(ir, IREG_flags_op1_B, IREG_flags_op2_B);
                case FLAGS_SUB16: case FLAGS_DEC16:
                    return uop_CMP_JNLE_DEST(ir, IREG_flags_op1_W, IREG_flags_op2_W);
                case FLAGS_SUB32: case FLAGS_DEC32:
                    return uop_CMP_JNLE_DEST(ir, IREG_flags_op1, IREG_flags_op2);
                default:
                    break;
            }
            break;

        case 15: /* NLE: false when ZF=1 || SF != OF (JLE) */
            switch (flags_op) {
                case FLAGS_SUB8: case FLAGS_DEC8:
                    return uop_CMP_JLE_DEST(ir, IREG_flags_op1_B, IREG_flags_op2_B);
                case FLAGS_SUB16: case FLAGS_DEC16:
                    return uop_CMP_JLE_DEST(ir, IREG_flags_op1_W, IREG_flags_op2_W);
                case FLAGS_SUB32: case FLAGS_DEC32:
                    return uop_CMP_JLE_DEST(ir, IREG_flags_op1, IREG_flags_op2);
                default:
                    break;
            }
            break;

        default:
            break;
    }

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
ropJCXZ(codeblock_t *block, UNUSED(ir_data_t *ir), UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    uint32_t offset    = (int32_t) (int8_t) fastreadb(cs + op_pc);
    uint32_t dest_addr = op_pc + 1 + offset;
    uint32_t next_pc   = op_pc + 1;

    if (!(op_32 & 0x100)) {
        dest_addr &= 0xffff;
        next_pc   &= 0xffff;
    }

    codegen_mark_code_present(block, cs + op_pc, 1);
    block->exit_type          = BLOCK_EXIT_COND_2;
    block->exit_cond          = (op_32 & 0x200) ? COND_JCXZ_32 : COND_JCXZ_16;
    block->links[0].target_pc = dest_addr;
    block->links[1].target_pc = next_pc;
    CPU_BLOCK_END();
    return next_pc;
}

uint32_t
ropLOOP(codeblock_t *block, UNUSED(ir_data_t *ir), UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    uint32_t offset    = (int32_t) (int8_t) fastreadb(cs + op_pc);
    uint32_t dest_addr = op_pc + 1 + offset;
    uint32_t next_pc   = op_pc + 1;

    if (!(op_32 & 0x100)) {
        dest_addr &= 0xffff;
        next_pc   &= 0xffff;
    }

    codegen_mark_code_present(block, cs + op_pc, 1);
    block->exit_type          = BLOCK_EXIT_COND_2;
    block->exit_cond          = (op_32 & 0x200) ? COND_LOOP_32 : COND_LOOP_16;
    block->links[0].target_pc = dest_addr;
    block->links[1].target_pc = next_pc;
    CPU_BLOCK_END();
    return next_pc;
}

uint32_t
ropLOOPE(codeblock_t *block, UNUSED(ir_data_t *ir), UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    uint32_t offset    = (int32_t) (int8_t) fastreadb(cs + op_pc);
    uint32_t dest_addr = op_pc + 1 + offset;
    uint32_t next_pc   = op_pc + 1;

    if (!(op_32 & 0x100)) {
        dest_addr &= 0xffff;
        next_pc   &= 0xffff;
    }

    codegen_mark_code_present(block, cs + op_pc, 1);
    block->exit_type          = BLOCK_EXIT_COND_2;
    block->exit_cond          = (op_32 & 0x200) ? COND_LOOPE_32 : COND_LOOPE_16;
    block->links[0].target_pc = dest_addr;
    block->links[1].target_pc = next_pc;
    CPU_BLOCK_END();
    return next_pc;
}

uint32_t
ropLOOPNE(codeblock_t *block, UNUSED(ir_data_t *ir), UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), uint32_t op_32, uint32_t op_pc)
{
    uint32_t offset    = (int32_t) (int8_t) fastreadb(cs + op_pc);
    uint32_t dest_addr = op_pc + 1 + offset;
    uint32_t next_pc   = op_pc + 1;

    if (!(op_32 & 0x100)) {
        dest_addr &= 0xffff;
        next_pc   &= 0xffff;
    }

    codegen_mark_code_present(block, cs + op_pc, 1);
    block->exit_type          = BLOCK_EXIT_COND_2;
    block->exit_cond          = (op_32 & 0x200) ? COND_LOOPNE_32 : COND_LOOPNE_16;
    block->links[0].target_pc = dest_addr;
    block->links[1].target_pc = next_pc;
    CPU_BLOCK_END();
    return next_pc;
}

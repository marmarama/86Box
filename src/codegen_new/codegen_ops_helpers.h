#include "386_common.h"
#include "codegen_backend.h"
#include "x86_flags.h"
#include "codegen_ops_jit_wrappers.h"

/*
 * Emit flags_rebuild_zpna with compile-time skip optimization.
 * When codegen_flags_changed is set, the previous flags_op is known at
 * compile time.  For ROL/ROR/IMUL/UNKNOWN, ZPNA is already materialized in
 * cpu_state.flags so the call can be elided entirely — zero extra uops.
 */
static inline void
emit_flags_rebuild_zpna(ir_data_t *ir)
{
    if (codegen_flags_changed) {
        switch (codegen_last_flags_op) {
            case FLAGS_ROL8:  case FLAGS_ROL16: case FLAGS_ROL32:
            case FLAGS_ROR8:  case FLAGS_ROR16: case FLAGS_ROR32:
            case FLAGS_IMUL16: case FLAGS_IMUL32:
            case FLAGS_UNKNOWN:
                return;
            default:
                break;
        }
    }
    uop_CALL_FUNC(ir, jit_flags_rebuild_zpna);
}

/*
 * Rebuild / materialize the Carry Flag (CF) into cpu_state.flags / IREG_flags.
 * Used before INC and DEC instructions which preserve CF while modifying other flags.
 *
 * Fast paths:
 * - If codegen_flags_changed is set:
 *   - For FLAGS_INCx, FLAGS_DECx, or FLAGS_UNKNOWN: CF is already preserved/materialized in cpu_state.flags.
 *   - For FLAGS_ZN* (logic ops): CF is unconditionally cleared (1 uop).
 *   - For FLAGS_ADD*: CF is set if unsigned sum wrapped: (flags_res < flags_op1) (3 uops via uop_SETCC).
 *   - For FLAGS_SUB*: CF is set if unsigned borrow: (flags_op1 < flags_op2) (3 uops via uop_SETCC).
 *   - All fast paths avoid calling the C helper jit_flags_rebuild_c, eliminating register flush barriers.
 */
static inline void
rebuild_c(ir_data_t *ir)
{
    if (codegen_flags_changed) {
        switch (codegen_last_flags_op) {
            case FLAGS_INC8:
            case FLAGS_INC16:
            case FLAGS_INC32:
            case FLAGS_DEC8:
            case FLAGS_DEC16:
            case FLAGS_DEC32:
            case FLAGS_UNKNOWN:
                return;

            case FLAGS_ZN8:
            case FLAGS_ZN16:
            case FLAGS_ZN32:
                uop_AND_IMM(ir, IREG_flags, IREG_flags, ~C_FLAG);
                return;

            case FLAGS_ADD32:
                uop_SETCC(ir, IREG_temp2, IREG_flags_res, IREG_flags_op1, COND_B);
                uop_AND_IMM(ir, IREG_flags, IREG_flags, ~C_FLAG);
                uop_OR(ir, IREG_flags, IREG_flags, IREG_temp2_W);
                return;

            case FLAGS_ADD16:
                uop_SETCC(ir, IREG_temp2, IREG_flags_res_W, IREG_flags_op1_W, COND_B);
                uop_AND_IMM(ir, IREG_flags, IREG_flags, ~C_FLAG);
                uop_OR(ir, IREG_flags, IREG_flags, IREG_temp2_W);
                return;

            case FLAGS_ADD8:
                uop_SETCC(ir, IREG_temp2, IREG_flags_res_B, IREG_flags_op1_B, COND_B);
                uop_AND_IMM(ir, IREG_flags, IREG_flags, ~C_FLAG);
                uop_OR(ir, IREG_flags, IREG_flags, IREG_temp2_W);
                return;

            case FLAGS_SUB32:
                uop_SETCC(ir, IREG_temp2, IREG_flags_op1, IREG_flags_op2, COND_B);
                uop_AND_IMM(ir, IREG_flags, IREG_flags, ~C_FLAG);
                uop_OR(ir, IREG_flags, IREG_flags, IREG_temp2_W);
                return;

            case FLAGS_SUB16:
                uop_SETCC(ir, IREG_temp2, IREG_flags_op1_W, IREG_flags_op2_W, COND_B);
                uop_AND_IMM(ir, IREG_flags, IREG_flags, ~C_FLAG);
                uop_OR(ir, IREG_flags, IREG_flags, IREG_temp2_W);
                return;

            case FLAGS_SUB8:
                uop_SETCC(ir, IREG_temp2, IREG_flags_op1_B, IREG_flags_op2_B, COND_B);
                uop_AND_IMM(ir, IREG_flags, IREG_flags, ~C_FLAG);
                uop_OR(ir, IREG_flags, IREG_flags, IREG_temp2_W);
                return;

            default:
                break;
        }
    }

    uop_CALL_FUNC(ir, jit_flags_rebuild_c);
}

static inline int
LOAD_SP_WITH_OFFSET(ir_data_t *ir, int offset)
{
    if (stack32) {
        if (offset) {
            uop_ADD_IMM(ir, IREG_eaaddr, IREG_ESP, offset);
            return IREG_eaaddr;
        } else
            return IREG_ESP;
    } else {
        if (offset) {
            uop_ADD_IMM(ir, IREG_eaaddr_W, IREG_SP, offset);
            uop_MOVZX(ir, IREG_eaaddr, IREG_eaaddr_W);
            return IREG_eaaddr;
        } else {
            uop_MOVZX(ir, IREG_eaaddr, IREG_SP);
            return IREG_eaaddr;
        }
    }
}

static inline int
LOAD_SP(ir_data_t *ir)
{
    return LOAD_SP_WITH_OFFSET(ir, 0);
}

static inline void
ADD_SP(ir_data_t *ir, int offset)
{
    if (stack32)
        uop_ADD_IMM(ir, IREG_ESP, IREG_ESP, offset);
    else
        uop_ADD_IMM(ir, IREG_SP, IREG_SP, offset);
}
static inline void
SUB_SP(ir_data_t *ir, int offset)
{
    if (stack32)
        uop_SUB_IMM(ir, IREG_ESP, IREG_ESP, offset);
    else
        uop_SUB_IMM(ir, IREG_SP, IREG_SP, offset);
}

static inline void
fpu_POP(codeblock_t *block, ir_data_t *ir)
{
    if (block->flags & CODEBLOCK_STATIC_TOP)
        uop_MOV_IMM(ir, IREG_FPU_TOP, (cpu_state.TOP + 1) & 7);
    else {
        uop_ADD_IMM(ir, IREG_FPU_TOP, IREG_FPU_TOP, 1);
        uop_AND_IMM(ir, IREG_FPU_TOP, IREG_FPU_TOP, 7);
    }
}
static inline void
fpu_POP2(codeblock_t *block, ir_data_t *ir)
{
    if (block->flags & CODEBLOCK_STATIC_TOP)
        uop_MOV_IMM(ir, IREG_FPU_TOP, (cpu_state.TOP + 2) & 7);
    else {
        uop_ADD_IMM(ir, IREG_FPU_TOP, IREG_FPU_TOP, 2);
        uop_AND_IMM(ir, IREG_FPU_TOP, IREG_FPU_TOP, 7);
    }
}
static inline void
fpu_PUSH(codeblock_t *block, ir_data_t *ir)
{
    if (block->flags & CODEBLOCK_STATIC_TOP)
        uop_MOV_IMM(ir, IREG_FPU_TOP, (cpu_state.TOP - 1) & 7);
    else {
        uop_SUB_IMM(ir, IREG_FPU_TOP, IREG_FPU_TOP, 1);
        uop_AND_IMM(ir, IREG_FPU_TOP, IREG_FPU_TOP, 7);
    }
}

static inline void
CHECK_SEG_LIMITS(UNUSED(codeblock_t *block), ir_data_t *ir, x86seg *seg, int addr_reg, int end_offset)
{
    if (!(cr0 & 1) || (cpu_state.eflags & VM_FLAG))
        return;

    if (seg->limit_low == 0 && seg->limit_high == 0xffffffff)
        return;

    if ((seg == &cpu_state.seg_ds && codegen_flat_ds && !(cpu_cur_status & CPU_STATUS_NOTFLATDS)) || (seg == &cpu_state.seg_ss && codegen_flat_ss && !(cpu_cur_status & CPU_STATUS_NOTFLATSS)))
        return;

    uop_CMP_JB(ir, addr_reg, ireg_seg_limit_low(seg), codegen_gpf_rout);
    if (end_offset) {
        uop_ADD_IMM(ir, IREG_temp3, addr_reg, end_offset);
        uop_CMP_JNBE(ir, IREG_temp3, ireg_seg_limit_high(seg), codegen_gpf_rout);
    } else
        uop_CMP_JNBE(ir, addr_reg, ireg_seg_limit_high(seg), codegen_gpf_rout);
}

static inline void
LOAD_IMMEDIATE_FROM_RAM_8(UNUSED(codeblock_t *block), ir_data_t *ir, int dest_reg, uint32_t addr)
{
    uop_MOVZX_REG_PTR_8(ir, dest_reg, get_ram_ptr(addr));
}

void LOAD_IMMEDIATE_FROM_RAM_16_unaligned(codeblock_t *block, ir_data_t *ir, int dest_reg, uint32_t addr);
static inline void
LOAD_IMMEDIATE_FROM_RAM_16(codeblock_t *block, ir_data_t *ir, int dest_reg, uint32_t addr)
{
    if ((addr & 0xfff) == 0xfff)
        LOAD_IMMEDIATE_FROM_RAM_16_unaligned(block, ir, dest_reg, addr);
    else
        uop_MOVZX_REG_PTR_16(ir, dest_reg, get_ram_ptr(addr));
}

void LOAD_IMMEDIATE_FROM_RAM_32_unaligned(codeblock_t *block, ir_data_t *ir, int dest_reg, uint32_t addr);
static inline void
LOAD_IMMEDIATE_FROM_RAM_32(codeblock_t *block, ir_data_t *ir, int dest_reg, uint32_t addr)
{
    if ((addr & 0xfff) >= 0xffd)
        LOAD_IMMEDIATE_FROM_RAM_32_unaligned(block, ir, dest_reg, addr);
    else
        uop_MOV_REG_PTR(ir, dest_reg, get_ram_ptr(addr));
}

int codegen_can_unroll_full(codeblock_t *block, ir_data_t *ir, uint32_t next_pc, uint32_t dest_addr);
static inline int
codegen_can_unroll(codeblock_t *block, ir_data_t *ir, uint32_t next_pc, uint32_t dest_addr)
{
    if (block->flags & CODEBLOCK_BYTE_MASK)
        return 0;

    /*Is dest within block?*/
    if (dest_addr > next_pc)
        return 0;
    if ((cs + dest_addr) < block->pc)
        return 0;

    return codegen_can_unroll_full(block, ir, next_pc, dest_addr);
}

static inline int
codegen_get_condition_fastpath(int cond, int *out_op1, int *out_op2)
{
    if (!codegen_flags_changed || codegen_last_flags_op == FLAGS_UNKNOWN)
        return 0;

    int cond_base = cond & 0xf;

    /* Fast-path 1: Zero Flag (COND_E / COND_NE) for all operations where ZF is (flags_res == 0) */
    if (is_zf_flags_res_op(codegen_last_flags_op)) {
        if (cond_base == COND_E || cond_base == COND_NE) {
            *out_op1 = IREG_flags_res;
            *out_op2 = -1; /* compare with 0 */
            return 1;
        }
    }

    /* Fast-path 2: Comparison conditions after CMP and SUB */
    if (codegen_last_flags_op == FLAGS_SUB32 ||
        codegen_last_flags_op == FLAGS_SUB16 ||
        codegen_last_flags_op == FLAGS_SUB8) {
        if (cond_base == COND_B || cond_base == COND_NB ||
            cond_base == COND_E || cond_base == COND_NE ||
            cond_base == COND_BE || cond_base == COND_NBE ||
            cond_base == COND_L || cond_base == COND_NL ||
            cond_base == COND_LE || cond_base == COND_NLE) {
            *out_op1 = (codegen_last_flags_op == FLAGS_SUB32) ? IREG_flags_op1 :
                       (codegen_last_flags_op == FLAGS_SUB16) ? IREG_flags_op1_W : IREG_flags_op1_B;
            *out_op2 = (codegen_last_flags_op == FLAGS_SUB32) ? IREG_flags_op2 :
                       (codegen_last_flags_op == FLAGS_SUB16) ? IREG_flags_op2_W : IREG_flags_op2_B;
            return 1;
        }
    }

    return 0;
}

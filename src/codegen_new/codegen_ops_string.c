#include <stdint.h>
#include <string.h>
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
codegen_stos_loop_body(codeblock_t *block, ir_data_t *ir, uint32_t op_32, int step, int val_reg)
{
    if (op_32 & 0x200)
        uop_MOV(ir, IREG_eaaddr, IREG_EDI);
    else
        uop_MOVZX(ir, IREG_eaaddr, IREG_DI);

    CHECK_SEG_LIMITS(block, ir, &cpu_state.seg_es, IREG_eaaddr, step - 1);
    uop_MEM_STORE_REG(ir, ireg_seg_base(&cpu_state.seg_es), IREG_eaaddr, val_reg);

    codegen_string_advance(ir, (op_32 & 0x200) ? IREG_EDI : IREG_DI, step, op_32 & 0x200);
}

static inline void
codegen_stos_single(codeblock_t *block, ir_data_t *ir, uint32_t op_32, int step, int val_reg)
{
    uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
    codegen_check_seg_write(block, ir, &cpu_state.seg_es);
    codegen_stos_loop_body(block, ir, op_32, step, val_reg);
}

static inline void
codegen_lods_loop_body(codeblock_t *block, ir_data_t *ir, uint32_t op_32, int step, int val_reg)
{
    if (op_32 & 0x200)
        uop_MOV(ir, IREG_eaaddr, IREG_ESI);
    else
        uop_MOVZX(ir, IREG_eaaddr, IREG_SI);

    CHECK_SEG_LIMITS(block, ir, op_ea_seg, IREG_eaaddr, step - 1);
    uop_MEM_LOAD_REG(ir, val_reg, ireg_seg_base(op_ea_seg), IREG_eaaddr);

    codegen_string_advance(ir, (op_32 & 0x200) ? IREG_ESI : IREG_SI, step, op_32 & 0x200);
}

static inline void
codegen_lods_single(codeblock_t *block, ir_data_t *ir, uint32_t op_32, int step, int val_reg)
{
    uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
    codegen_check_seg_read(block, ir, op_ea_seg);
    codegen_lods_loop_body(block, ir, op_32, step, val_reg);
}

static inline void
codegen_movs_loop_body(codeblock_t *block, ir_data_t *ir, uint32_t op_32, int step, int val_reg)
{
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
codegen_movs_single(codeblock_t *block, ir_data_t *ir, uint32_t op_32, int step, int val_reg)
{
    uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
    codegen_check_seg_read(block, ir, op_ea_seg);
    codegen_check_seg_write(block, ir, &cpu_state.seg_es);
    codegen_movs_loop_body(block, ir, op_32, step, val_reg);
}

static inline void
codegen_scas_loop_body(codeblock_t *block, ir_data_t *ir, uint32_t op_32, int step, int acc_reg, int val_reg, int flags_sub_op)
{
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
codegen_scas_single(codeblock_t *block, ir_data_t *ir, uint32_t op_32, int step, int acc_reg, int val_reg, int flags_sub_op)
{
    uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
    codegen_check_seg_read(block, ir, &cpu_state.seg_es);
    codegen_scas_loop_body(block, ir, op_32, step, acc_reg, val_reg, flags_sub_op);
}

static inline void
codegen_cmps_loop_body(codeblock_t *block, ir_data_t *ir, uint32_t op_32, int step, int src_val, int dst_val, int flags_sub_op)
{
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

static inline void
codegen_cmps_single(codeblock_t *block, ir_data_t *ir, uint32_t op_32, int step, int src_val, int dst_val, int flags_sub_op)
{
    uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
    codegen_check_seg_read(block, ir, op_ea_seg);
    codegen_check_seg_read(block, ir, &cpu_state.seg_es);
    codegen_cmps_loop_body(block, ir, op_32, step, src_val, dst_val, flags_sub_op);
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

static inline void
do_rep_movs_bulk(int addr32, int step)
{
    if (cpu_state.abrt || cpu_state.exit_request || (cpu_state.flags & D_FLAG) || cpu_state._cycles <= 0)
        return;

    uint32_t count = addr32 ? cpu_state.regs[1].l : cpu_state.regs[1].w;
    if (count == 0)
        return;

    uint32_t src_ofs = addr32 ? cpu_state.regs[6].l : cpu_state.regs[6].w;
    uint32_t dst_ofs = addr32 ? cpu_state.regs[7].l : cpu_state.regs[7].w;
    x86seg  *src_seg = cpu_state.ea_seg;
    x86seg  *dst_seg = &cpu_state.seg_es;
    if (!src_seg)
        return;

    /* In Protected Mode, verify segment bounds if segments have limits */
    if ((cr0 & 1) && !(cpu_state.eflags & VM_FLAG)) {
        if (src_seg->limit_low != 0 || src_seg->limit_high != 0xffffffffU) {
            if (src_ofs < src_seg->limit_low || (uint64_t) src_ofs + step - 1 > src_seg->limit_high)
                return;
        }
        if (dst_seg->limit_low != 0 || dst_seg->limit_high != 0xffffffffU) {
            if (dst_ofs < dst_seg->limit_low || (uint64_t) dst_ofs + step - 1 > dst_seg->limit_high)
                return;
        }
    }

    uint32_t src_lin = src_seg->base + src_ofs;
    uint32_t dst_lin = dst_seg->base + dst_ofs;

    uintptr_t rl = readlookup2[src_lin >> 12];
    uintptr_t wl = writelookup2[dst_lin >> 12];
    if (rl == (uintptr_t) LOOKUP_INV || wl == (uintptr_t) LOOKUP_INV)
        return;

    uint32_t src_page_rem = (0x1000U - (src_lin & 0xfffU)) / step;
    uint32_t dst_page_rem = (0x1000U - (dst_lin & 0xfffU)) / step;
    uint32_t batch        = count;
    if (src_page_rem < batch)
        batch = src_page_rem;
    if (dst_page_rem < batch)
        batch = dst_page_rem;

    if (!addr32) {
        uint32_t si_rem = (0x10000U - src_ofs) / step;
        uint32_t di_rem = (0x10000U - dst_ofs) / step;
        if (si_rem < batch)
            batch = si_rem;
        if (di_rem < batch)
            batch = di_rem;
    }
    if ((cr0 & 1) && !(cpu_state.eflags & VM_FLAG)) {
        uint64_t src_lim_rem = ((uint64_t) src_seg->limit_high >= (uint64_t) src_ofs + step - 1)
                                   ? (((uint64_t) src_seg->limit_high - (uint64_t) src_ofs + 1ULL) / step) : 0;
        if (src_lim_rem < batch)
            batch = (uint32_t) src_lim_rem;
        uint64_t dst_lim_rem = ((uint64_t) dst_seg->limit_high >= (uint64_t) dst_ofs + step - 1)
                                   ? (((uint64_t) dst_seg->limit_high - (uint64_t) dst_ofs + 1ULL) / step) : 0;
        if (dst_lim_rem < batch)
            batch = (uint32_t) dst_lim_rem;
    }

    int      step_cycles = is486 ? 3 : 4;
    uint32_t max_cyc_cnt = (uint32_t) ((cpu_state._cycles + step_cycles - 1) / step_cycles);
    if (max_cyc_cnt < batch)
        batch = max_cyc_cnt;

    if (batch == 0)
        return;

    uint8_t *src_ptr   = (uint8_t *) (rl + src_lin);
    uint8_t *dst_ptr   = (uint8_t *) (wl + dst_lin);
    uint32_t num_bytes = batch * step;

    uintptr_t s_addr = (uintptr_t) src_ptr;
    uintptr_t d_addr = (uintptr_t) dst_ptr;
    if (d_addr > s_addr && d_addr < s_addr + num_bytes) {
        if (step == 1) {
            for (uint32_t i = 0; i < batch; i++)
                dst_ptr[i] = src_ptr[i];
        } else if (step == 2) {
            for (uint32_t i = 0; i < batch; i++) {
                uint16_t val;
                memcpy(&val, src_ptr + (i * 2), 2);
                memcpy(dst_ptr + (i * 2), &val, 2);
            }
        } else {
            for (uint32_t i = 0; i < batch; i++) {
                uint32_t val;
                memcpy(&val, src_ptr + (i * 4), 4);
                memcpy(dst_ptr + (i * 4), &val, 4);
            }
        }
    } else {
        memmove(dst_ptr, src_ptr, num_bytes);
    }

    if (addr32) {
        cpu_state.regs[6].l += num_bytes;
        cpu_state.regs[7].l += num_bytes;
        cpu_state.regs[1].l -= batch;
    } else {
        cpu_state.regs[6].w = (uint16_t) (src_ofs + num_bytes);
        cpu_state.regs[7].w = (uint16_t) (dst_ofs + num_bytes);
        cpu_state.regs[1].w = (uint16_t) (count - batch);
    }
    cpu_state._cycles -= (int) (batch * step_cycles);
}

static inline void
do_rep_stos_bulk(int addr32, int step)
{
    if (cpu_state.abrt || cpu_state.exit_request || (cpu_state.flags & D_FLAG) || cpu_state._cycles <= 0)
        return;

    uint32_t count = addr32 ? cpu_state.regs[1].l : cpu_state.regs[1].w;
    if (count == 0)
        return;

    uint32_t dst_ofs = addr32 ? cpu_state.regs[7].l : cpu_state.regs[7].w;
    x86seg  *dst_seg = &cpu_state.seg_es;

    if ((cr0 & 1) && !(cpu_state.eflags & VM_FLAG)) {
        if (dst_seg->limit_low != 0 || dst_seg->limit_high != 0xffffffffU) {
            if (dst_ofs < dst_seg->limit_low || (uint64_t) dst_ofs + step - 1 > dst_seg->limit_high)
                return;
        }
    }

    uint32_t dst_lin = dst_seg->base + dst_ofs;

    uintptr_t wl = writelookup2[dst_lin >> 12];
    if (wl == (uintptr_t) LOOKUP_INV)
        return;

    uint32_t dst_page_rem = (0x1000U - (dst_lin & 0xfffU)) / step;
    uint32_t batch        = count;
    if (dst_page_rem < batch)
        batch = dst_page_rem;

    if (!addr32) {
        uint32_t di_rem = (0x10000U - dst_ofs) / step;
        if (di_rem < batch)
            batch = di_rem;
    }
    if ((cr0 & 1) && !(cpu_state.eflags & VM_FLAG)) {
        uint64_t dst_lim_rem = ((uint64_t) dst_seg->limit_high >= (uint64_t) dst_ofs + step - 1)
                                   ? (((uint64_t) dst_seg->limit_high - (uint64_t) dst_ofs + 1ULL) / step) : 0;
        if (dst_lim_rem < batch)
            batch = (uint32_t) dst_lim_rem;
    }

    int      step_cycles = is486 ? 3 : 4;
    uint32_t max_cyc_cnt = (uint32_t) ((cpu_state._cycles + step_cycles - 1) / step_cycles);
    if (max_cyc_cnt < batch)
        batch = max_cyc_cnt;

    if (batch == 0)
        return;

    uint8_t *dst_ptr   = (uint8_t *) (wl + dst_lin);
    uint32_t num_bytes = batch * step;

    if (step == 1) {
        memset(dst_ptr, cpu_state.regs[0].b.l, num_bytes);
    } else if (step == 2) {
        uint16_t  val = cpu_state.regs[0].w;
        uint16_t *p16 = (uint16_t *) dst_ptr;
        for (uint32_t i = 0; i < batch; i++)
            p16[i] = val;
    } else {
        uint32_t  val = cpu_state.regs[0].l;
        uint32_t *p32 = (uint32_t *) dst_ptr;
        for (uint32_t i = 0; i < batch; i++)
            p32[i] = val;
    }

    if (addr32) {
        cpu_state.regs[7].l += num_bytes;
        cpu_state.regs[1].l -= batch;
    } else {
        cpu_state.regs[7].w = (uint16_t) (dst_ofs + num_bytes);
        cpu_state.regs[1].w = (uint16_t) (count - batch);
    }
    cpu_state._cycles -= (int) (batch * step_cycles);
}

static void jit_rep_movsb_a16(void) { do_rep_movs_bulk(0, 1); }
static void jit_rep_movsb_a32(void) { do_rep_movs_bulk(1, 1); }
static void jit_rep_movsw_a16(void) { do_rep_movs_bulk(0, 2); }
static void jit_rep_movsw_a32(void) { do_rep_movs_bulk(1, 2); }
static void jit_rep_movsl_a16(void) { do_rep_movs_bulk(0, 4); }
static void jit_rep_movsl_a32(void) { do_rep_movs_bulk(1, 4); }

static void jit_rep_stosb_a16(void) { do_rep_stos_bulk(0, 1); }
static void jit_rep_stosb_a32(void) { do_rep_stos_bulk(1, 1); }
static void jit_rep_stosw_a16(void) { do_rep_stos_bulk(0, 2); }
static void jit_rep_stosw_a32(void) { do_rep_stos_bulk(1, 2); }
static void jit_rep_stosl_a16(void) { do_rep_stos_bulk(0, 4); }
static void jit_rep_stosl_a32(void) { do_rep_stos_bulk(1, 4); }

static inline void *
select_bulk_movs_helper(uint32_t op_32, int step)
{
    int addr32 = (op_32 & 0x200) != 0;
    if (step == 1)
        return addr32 ? (void *) jit_rep_movsb_a32 : (void *) jit_rep_movsb_a16;
    if (step == 2)
        return addr32 ? (void *) jit_rep_movsw_a32 : (void *) jit_rep_movsw_a16;
    return addr32 ? (void *) jit_rep_movsl_a32 : (void *) jit_rep_movsl_a16;
}

static inline void *
select_bulk_stos_helper(uint32_t op_32, int step)
{
    int addr32 = (op_32 & 0x200) != 0;
    if (step == 1)
        return addr32 ? (void *) jit_rep_stosb_a32 : (void *) jit_rep_stosb_a16;
    if (step == 2)
        return addr32 ? (void *) jit_rep_stosw_a32 : (void *) jit_rep_stosw_a16;
    return addr32 ? (void *) jit_rep_stosl_a32 : (void *) jit_rep_stosl_a16;
}

static inline uint32_t
ropREP_STOS_common(codeblock_t *block, ir_data_t *ir, uint32_t op_32, uint32_t op_pc, int step, int val_reg)
{
    int count_reg = (op_32 & 0x200) ? IREG_ECX : IREG_CX;
    int step_cycles = is486 ? 3 : 4;

    codegen_accumulate_flush(ir);

    int jump_zero = uop_CMP_IMM_JZ_DEST(ir, count_reg, 0);

    /* Hoist loop-invariant setup before loop_label */
    uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
    codegen_check_seg_write(block, ir, &cpu_state.seg_es);

    int loop_label = uop_LOOP_LABEL(ir);

    codegen_stos_loop_body(block, ir, op_32, step, val_reg);

    uop_SUB_IMM(ir, count_reg, count_reg, 1);
    uop_SUB_IMM(ir, IREG_cycles, IREG_cycles, step_cycles);

    uop_NOP_BARRIER(ir);

    int jump_cnt_zero = uop_CMP_IMM_JZ_DEST(ir, count_reg, 0);

    void *bulk_helper = select_bulk_stos_helper(op_32, step);
    uop_CALL_FUNC(ir, bulk_helper);

    int jump_cnt_zero2 = uop_CMP_IMM_JZ_DEST(ir, count_reg, 0);
    int jump_cycles    = uop_CMP_IMM_JLE_DEST(ir, IREG_cycles, 0);
    int jump_exit      = uop_CMP_IMM_JNZ_DEST(ir, IREG_exit_request, 0);

    uop_LOOP_JMP(ir, loop_label);

    uop_set_jump_dest(ir, jump_cycles);
    uop_set_jump_dest(ir, jump_exit);
    uop_MOV_IMM(ir, IREG_pc, cpu_state.oldpc);
    uop_JMP(ir, codegen_exit_rout);

    uop_NOP_BARRIER(ir);
    uop_set_jump_dest(ir, jump_zero);
    uop_set_jump_dest(ir, jump_cnt_zero);
    uop_set_jump_dest(ir, jump_cnt_zero2);

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

    /* Hoist loop-invariant setup before loop_label */
    uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
    codegen_check_seg_read(block, ir, op_ea_seg);

    int loop_label = uop_LOOP_LABEL(ir);

    codegen_lods_loop_body(block, ir, op_32, step, val_reg);

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

    uop_NOP_BARRIER(ir);
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

    /* Hoist loop-invariant setup before loop_label */
    uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
    codegen_check_seg_read(block, ir, op_ea_seg);
    codegen_check_seg_write(block, ir, &cpu_state.seg_es);
    uop_MOV_PTR(ir, IREG_ea_seg, (void *) op_ea_seg);

    int loop_label = uop_LOOP_LABEL(ir);

    codegen_movs_loop_body(block, ir, op_32, step, val_reg);

    uop_SUB_IMM(ir, count_reg, count_reg, 1);
    uop_SUB_IMM(ir, IREG_cycles, IREG_cycles, step_cycles);

    uop_NOP_BARRIER(ir);

    int jump_cnt_zero = uop_CMP_IMM_JZ_DEST(ir, count_reg, 0);

    void *bulk_helper = select_bulk_movs_helper(op_32, step);
    uop_CALL_FUNC(ir, bulk_helper);

    int jump_cnt_zero2 = uop_CMP_IMM_JZ_DEST(ir, count_reg, 0);
    int jump_cycles    = uop_CMP_IMM_JLE_DEST(ir, IREG_cycles, 0);
    int jump_exit      = uop_CMP_IMM_JNZ_DEST(ir, IREG_exit_request, 0);

    uop_LOOP_JMP(ir, loop_label);

    uop_set_jump_dest(ir, jump_cycles);
    uop_set_jump_dest(ir, jump_exit);
    uop_MOV_IMM(ir, IREG_pc, cpu_state.oldpc);
    uop_JMP(ir, codegen_exit_rout);

    uop_NOP_BARRIER(ir);
    uop_set_jump_dest(ir, jump_zero);
    uop_set_jump_dest(ir, jump_cnt_zero);
    uop_set_jump_dest(ir, jump_cnt_zero2);

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

    uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
    codegen_check_seg_read(block, ir, &cpu_state.seg_es);

    int loop_label = uop_LOOP_LABEL(ir);

    codegen_scas_loop_body(block, ir, op_32, step, acc_reg, val_reg, flags_sub_op);

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

    uop_NOP_BARRIER(ir);
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

    uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
    codegen_check_seg_read(block, ir, &cpu_state.seg_es);

    int loop_label = uop_LOOP_LABEL(ir);

    codegen_scas_loop_body(block, ir, op_32, step, acc_reg, val_reg, flags_sub_op);

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

    uop_NOP_BARRIER(ir);
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

    uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
    codegen_check_seg_read(block, ir, op_ea_seg);
    codegen_check_seg_read(block, ir, &cpu_state.seg_es);

    int loop_label = uop_LOOP_LABEL(ir);

    codegen_cmps_loop_body(block, ir, op_32, step, src_val, dst_val, flags_sub_op);

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

    uop_NOP_BARRIER(ir);
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

    uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
    codegen_check_seg_read(block, ir, op_ea_seg);
    codegen_check_seg_read(block, ir, &cpu_state.seg_es);

    int loop_label = uop_LOOP_LABEL(ir);

    codegen_cmps_loop_body(block, ir, op_32, step, src_val, dst_val, flags_sub_op);

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

    uop_NOP_BARRIER(ir);
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

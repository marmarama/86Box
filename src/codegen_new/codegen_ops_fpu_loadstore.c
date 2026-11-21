#include <stdint.h>
#include <86box/86box.h>
#include "cpu.h"
#include <86box/mem.h>
#include <86box/machine.h>
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
#include "codegen_ops_fpu_loadstore.h"
#include "codegen_ops_fpu_arith.h"
#include "codegen_ops_helpers.h"
#include "codegen_ops_jit_wrappers.h"
#include "x87_ops_conv.h"

static double jit_fpu_ld80_tmp;

static inline int
jit_fld80_body(uint32_t seg_base, uint32_t eaaddr)
{
    x87_conv_t test;

    test.eind.ll = readmeml(seg_base, eaaddr);
    if (cpu_state.abrt)
        return 1;
    test.eind.ll |= (uint64_t) readmeml(seg_base, eaaddr + 4) << 32;
    if (cpu_state.abrt)
        return 1;
    test.begin = readmemw(seg_base, eaaddr + 8);
    if (cpu_state.abrt)
        return 1;
    jit_fpu_ld80_tmp = x87_from80(&test);
    return 0;
}

static JIT_WRAPPER int
jit_fld80(uint32_t seg_base, uint32_t eaaddr)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        int ret = jit_fld80_body(seg_base, eaaddr);
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
        return ret;
    } else {
        return jit_fld80_body(seg_base, eaaddr);
    }
}

static inline int
jit_fstp80_body(uint32_t seg_base, uint32_t eaaddr)
{
    x87_conv_t test;

    x87_to80(cpu_state.ST[cpu_state.TOP & 7], &test);
    writememl(seg_base, eaaddr, test.eind.ll & 0xffffffff);
    if (cpu_state.abrt)
        return 1;
    writememl(seg_base, eaaddr + 4, test.eind.ll >> 32);
    if (cpu_state.abrt)
        return 1;
    writememw(seg_base, eaaddr + 8, test.begin);
    return cpu_state.abrt ? 1 : 0;
}

static JIT_WRAPPER int
jit_fstp80(uint32_t seg_base, uint32_t eaaddr)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        int ret = jit_fstp80_body(seg_base, eaaddr);
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
        return ret;
    } else {
        return jit_fstp80_body(seg_base, eaaddr);
    }
}

uint32_t
ropFLDs(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    x86seg *target_seg;

    uop_FP_ENTER(ir);
    uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
    op_pc--;
    target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);
    codegen_check_seg_read(block, ir, target_seg);
    uop_MEM_LOAD_SINGLE(ir, IREG_ST(-1), ireg_seg_base(target_seg), IREG_eaaddr);
    uop_MOV_IMM(ir, IREG_tag(-1), TAG_VALID);
    fpu_PUSH(block, ir);

    return op_pc + 1;
}
uint32_t
ropFLDd(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    x86seg *target_seg;

    uop_FP_ENTER(ir);
    uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
    op_pc--;
    target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);
    codegen_check_seg_read(block, ir, target_seg);
    uop_MEM_LOAD_DOUBLE(ir, IREG_ST(-1), ireg_seg_base(target_seg), IREG_eaaddr);
    uop_MOV_IMM(ir, IREG_tag(-1), TAG_VALID);
    fpu_PUSH(block, ir);

    return op_pc + 1;
}

uint32_t
ropFSTs(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    x86seg *target_seg;

    uop_FP_ENTER(ir);
    uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
    op_pc--;
    target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);
    codegen_check_seg_write(block, ir, target_seg);
    uop_MEM_STORE_SINGLE(ir, ireg_seg_base(target_seg), IREG_eaaddr, IREG_ST(0));

    return op_pc + 1;
}
uint32_t
ropFSTPs(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    x86seg *target_seg;

    uop_FP_ENTER(ir);
    uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
    op_pc--;
    target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);
    codegen_check_seg_write(block, ir, target_seg);
    uop_MEM_STORE_SINGLE(ir, ireg_seg_base(target_seg), IREG_eaaddr, IREG_ST(0));
    uop_MOV_IMM(ir, IREG_tag(0), TAG_EMPTY);
    fpu_POP(block, ir);

    return op_pc + 1;
}
uint32_t
ropFSTd(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    x86seg *target_seg;

    uop_FP_ENTER(ir);
    uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
    op_pc--;
    target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);
    codegen_check_seg_write(block, ir, target_seg);
    CHECK_SEG_LIMITS(block, ir, target_seg, IREG_eaaddr, 7);
    uop_MEM_STORE_DOUBLE(ir, ireg_seg_base(target_seg), IREG_eaaddr, IREG_ST(0));

    return op_pc + 1;
}
uint32_t
ropFSTPd(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    x86seg *target_seg;

    uop_FP_ENTER(ir);
    uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
    op_pc--;
    target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);
    codegen_check_seg_write(block, ir, target_seg);
    CHECK_SEG_LIMITS(block, ir, target_seg, IREG_eaaddr, 7);
    uop_MEM_STORE_DOUBLE(ir, ireg_seg_base(target_seg), IREG_eaaddr, IREG_ST(0));
    uop_MOV_IMM(ir, IREG_tag(0), TAG_EMPTY);
    fpu_POP(block, ir);

    return op_pc + 1;
}

uint32_t
ropFILDw(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    x86seg *target_seg;

    uop_FP_ENTER(ir);
    uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
    op_pc--;
    target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);
    codegen_check_seg_read(block, ir, target_seg);
    uop_MEM_LOAD_REG(ir, IREG_temp0_W, ireg_seg_base(target_seg), IREG_eaaddr);
    uop_MOV_DOUBLE_INT(ir, IREG_ST(-1), IREG_temp0_W);
    uop_MOV_IMM(ir, IREG_tag(-1), TAG_VALID);
    fpu_PUSH(block, ir);

    return op_pc + 1;
}
uint32_t
ropFILDl(codeblock_t *block, ir_data_t *ir, uint8_t UNUSED(opcode), uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    x86seg *target_seg;

    uop_FP_ENTER(ir);
    uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
    op_pc--;
    target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);
    codegen_check_seg_read(block, ir, target_seg);
    uop_MEM_LOAD_REG(ir, IREG_temp0, ireg_seg_base(target_seg), IREG_eaaddr);
    uop_MOV_DOUBLE_INT(ir, IREG_ST(-1), IREG_temp0);
    uop_MOV_IMM(ir, IREG_tag(-1), TAG_VALID);
    fpu_PUSH(block, ir);

    return op_pc + 1;
}
uint32_t
ropFILDq(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    x86seg *target_seg;

    uop_FP_ENTER(ir);
    uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
    op_pc--;
    target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);
    codegen_check_seg_read(block, ir, target_seg);
    uop_MEM_LOAD_REG(ir, IREG_ST_i64(-1), ireg_seg_base(target_seg), IREG_eaaddr);
    uop_MOV_DOUBLE_INT(ir, IREG_ST(-1), IREG_ST_i64(-1));
    uop_MOV_IMM(ir, IREG_tag(-1), TAG_VALID | TAG_UINT64);
    fpu_PUSH(block, ir);
    codegen_fpu_has_uint64 = 1;
    if (!(block->status & CPU_STATUS_FPU_UINT64))
        CPU_BLOCK_END();

    return op_pc + 1;
}

uint32_t
ropFISTw(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    x86seg *target_seg;

    uop_FP_ENTER(ir);
    uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
    op_pc--;
    target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);
    codegen_check_seg_write(block, ir, target_seg);
    uop_MOV_INT_DOUBLE(ir, IREG_temp0_W, IREG_ST(0));
    uop_MEM_STORE_REG(ir, ireg_seg_base(target_seg), IREG_eaaddr, IREG_temp0_W);
    /* FIST leaves ST(0) where it is: its tag stays as it was. */

    return op_pc + 1;
}
uint32_t
ropFISTPw(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    x86seg *target_seg;

    uop_FP_ENTER(ir);
    uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
    op_pc--;
    target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);
    codegen_check_seg_write(block, ir, target_seg);
    uop_MOV_INT_DOUBLE(ir, IREG_temp0_W, IREG_ST(0));
    uop_MEM_STORE_REG(ir, ireg_seg_base(target_seg), IREG_eaaddr, IREG_temp0_W);
    uop_MOV_IMM(ir, IREG_tag(0), TAG_EMPTY);
    fpu_POP(block, ir);

    return op_pc + 1;
}
uint32_t
ropFISTl(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    x86seg *target_seg;

    uop_FP_ENTER(ir);
    uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
    op_pc--;
    target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);
    codegen_check_seg_write(block, ir, target_seg);
    uop_MOV_INT_DOUBLE(ir, IREG_temp0, IREG_ST(0));
    uop_MEM_STORE_REG(ir, ireg_seg_base(target_seg), IREG_eaaddr, IREG_temp0);
    /* FIST leaves ST(0) where it is: its tag stays as it was. */

    return op_pc + 1;
}
uint32_t
ropFISTPl(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    x86seg *target_seg;

    uop_FP_ENTER(ir);
    uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
    op_pc--;
    target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);
    codegen_check_seg_write(block, ir, target_seg);
    uop_MOV_INT_DOUBLE(ir, IREG_temp0, IREG_ST(0));
    uop_MEM_STORE_REG(ir, ireg_seg_base(target_seg), IREG_eaaddr, IREG_temp0);
    uop_MOV_IMM(ir, IREG_tag(0), TAG_EMPTY);
    fpu_POP(block, ir);

    return op_pc + 1;
}
uint32_t
ropFISTPq(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    if (machines[machine].init != machine_at_vect486n_init) {
        x86seg *target_seg;

        uop_FP_ENTER(ir);
        uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
        op_pc--;
        target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);
        codegen_check_seg_write(block, ir, target_seg);
        uop_MOV_INT_DOUBLE_64(ir, IREG_temp0_Q, IREG_ST(0), IREG_ST_i64(0), IREG_tag(0));
        uop_MEM_STORE_REG(ir, ireg_seg_base(target_seg), IREG_eaaddr, IREG_temp0_Q);
        uop_MOV_IMM(ir, IREG_tag(0), TAG_EMPTY);
        fpu_POP(block, ir);

        return op_pc + 1;
    }
    /* On the HP Vectra 486N, the generated store cannot report an empty-stack exception or suppress
       the store/pop. Use the common FISTP m64 handler to preserve exception behavior. */
    return 0;
}

uint32_t
ropFLDe(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    x86seg *target_seg;

    uop_FP_ENTER(ir);
    uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
    op_pc--;
    target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);
    codegen_check_seg_read(block, ir, target_seg);
    CHECK_SEG_LIMITS(block, ir, target_seg, IREG_eaaddr, 9);
    uop_LOAD_FUNC_ARG_REG(ir, 0, ireg_seg_base(target_seg));
    uop_LOAD_FUNC_ARG_REG(ir, 1, IREG_eaaddr);
    uop_CALL_FUNC_RESULT(ir, IREG_temp0, jit_fld80);
    uop_CMP_IMM_JZ(ir, IREG_temp0, 1, codegen_exit_rout);
    uop_MOV_REG_PTR(ir, IREG_ST(-1), &jit_fpu_ld80_tmp);
    uop_MOV_IMM(ir, IREG_tag(-1), TAG_VALID);
    fpu_PUSH(block, ir);

    return op_pc + 1;
}

uint32_t
ropFSTPe(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
    x86seg *target_seg;

    uop_FP_ENTER(ir);
    uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
    op_pc--;
    target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);
    codegen_check_seg_write(block, ir, target_seg);
    CHECK_SEG_LIMITS(block, ir, target_seg, IREG_eaaddr, 9);
    uop_LOAD_FUNC_ARG_REG(ir, 0, ireg_seg_base(target_seg));
    uop_LOAD_FUNC_ARG_REG(ir, 1, IREG_eaaddr);
    uop_CALL_FUNC_RESULT(ir, IREG_temp0, jit_fstp80);
    uop_CMP_IMM_JZ(ir, IREG_temp0, 1, codegen_exit_rout);
    uop_MOV_IMM(ir, IREG_tag(0), TAG_EMPTY);
    fpu_POP(block, ir);

    return op_pc + 1;
}

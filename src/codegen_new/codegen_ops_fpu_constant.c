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
#include "codegen_ops_fpu_constant.h"
#include "codegen_ops_helpers.h"

static uint32_t
ropFLD_const(codeblock_t *block, ir_data_t *ir, uint32_t op_pc, const uint64_t *bits)
{
    uop_FP_ENTER(ir);
    uop_MOV_REG_PTR(ir, IREG_ST(-1), (void *) bits);
    uop_MOV_IMM(ir, IREG_tag(-1), TAG_VALID);
    fpu_PUSH(block, ir);

    return op_pc;
}

uint32_t
ropFLD1(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), UNUSED(uint32_t op_32), uint32_t op_pc)
{
    uop_FP_ENTER(ir);
    uop_MOV_DOUBLE_IMM(ir, IREG_ST(-1), 1);
    uop_MOV_IMM(ir, IREG_tag(-1), TAG_VALID);
    fpu_PUSH(block, ir);

    return op_pc;
}
uint32_t
ropFLDL2T(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), UNUSED(uint32_t op_32), uint32_t op_pc)
{
    static const uint64_t val = 0x400a934f0979a371ULL;
    return ropFLD_const(block, ir, op_pc, &val);
}
uint32_t
ropFLDL2E(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), UNUSED(uint32_t op_32), uint32_t op_pc)
{
    static const uint64_t val = 0x3ff71547652b82feULL;
    return ropFLD_const(block, ir, op_pc, &val);
}
uint32_t
ropFLDPI(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), UNUSED(uint32_t op_32), uint32_t op_pc)
{
    static const uint64_t val = 0x400921fb54442d18ULL;
    return ropFLD_const(block, ir, op_pc, &val);
}
uint32_t
ropFLDLG2(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), UNUSED(uint32_t op_32), uint32_t op_pc)
{
    static const uint64_t val = 0x3fd34413509f79ffULL;
    return ropFLD_const(block, ir, op_pc, &val);
}
uint32_t
ropFLDLN2(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), UNUSED(uint32_t op_32), uint32_t op_pc)
{
    static const uint64_t val = 0x3fe62e42fefa39f0ULL;
    return ropFLD_const(block, ir, op_pc, &val);
}
uint32_t
ropFLDZ(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), UNUSED(uint32_t op_32), uint32_t op_pc)
{
    uop_FP_ENTER(ir);
    uop_MOV_DOUBLE_IMM(ir, IREG_ST(-1), 0);
    uop_MOV_IMM(ir, IREG_tag(-1), TAG_VALID);
    fpu_PUSH(block, ir);

    return op_pc;
}

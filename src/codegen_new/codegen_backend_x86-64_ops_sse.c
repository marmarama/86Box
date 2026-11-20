#if defined __amd64__ || defined _M_X64

#    include <stdint.h>
#    include <86box/86box.h>
#    include "cpu.h"
#    include <86box/mem.h>
#    include <86box/plat_unused.h>

#    include "codegen.h"
#    include "codegen_allocator.h"
#    include "codegen_backend.h"
#    include "codegen_backend_x86-64_defs.h"
#    include "codegen_backend_x86-64_ops_sse.h"
#    include "codegen_backend_x86-64_ops_helpers.h"

static inline void
emit_sse_2op(codeblock_t *block, uint8_t opcode, int dst_reg, int src_reg)
{
    codegen_alloc_bytes(block, 4);
    if ((dst_reg & 8) || (src_reg & 8))
        codegen_addbyte(block, rex_byte(0, dst_reg, 0, src_reg));
    codegen_addbyte3(block, 0x0f, opcode, 0xc0 | (src_reg & 7) | ((dst_reg & 7) << 3));
}

static inline void
emit_sse_pfx_2op(codeblock_t *block, uint8_t prefix, uint8_t opcode, int dst_reg, int src_reg)
{
    codegen_alloc_bytes(block, 5);
    codegen_addbyte(block, prefix);
    if ((dst_reg & 8) || (src_reg & 8))
        codegen_addbyte(block, rex_byte(0, dst_reg, 0, src_reg));
    codegen_addbyte3(block, 0x0f, opcode, 0xc0 | (src_reg & 7) | ((dst_reg & 7) << 3));
}

static inline void
emit_sse_pfx_w_2op(codeblock_t *block, uint8_t prefix, uint8_t opcode, int dst_reg, int src_reg)
{
    codegen_alloc_bytes(block, 5);
    codegen_addbyte2(block, prefix, rex_byte(1, dst_reg, 0, src_reg));
    codegen_addbyte3(block, 0x0f, opcode, 0xc0 | (src_reg & 7) | ((dst_reg & 7) << 3));
}

static inline void
emit_sse_pfx_2op_imm(codeblock_t *block, uint8_t prefix, uint8_t opcode, int dst_reg, int src_reg, uint8_t imm)
{
    emit_sse_pfx_2op(block, prefix, opcode, dst_reg, src_reg);
    codegen_alloc_bytes(block, 1);
    codegen_addbyte(block, imm);
}

static inline void
emit_sse_shift_imm(codeblock_t *block, uint8_t opcode, uint8_t subop, int dst_reg, int shift)
{
    codegen_alloc_bytes(block, 6);
    codegen_addbyte(block, 0x66);
    if (dst_reg & 8)
        codegen_addbyte(block, rex_byte(0, 0, 0, dst_reg));
    codegen_addbyte4(block, 0x0f, opcode, 0xc0 | subop | (dst_reg & 7), shift);
}

static inline void
emit_sse_base_index(codeblock_t *block, uint8_t prefix, uint8_t opcode, int reg, int base_reg, int idx_reg)
{
    codegen_alloc_bytes(block, 7);
    codegen_addbyte(block, prefix);
    if ((reg & 8) || (base_reg & 8) || (idx_reg & 8))
        codegen_addbyte(block, rex_byte(0, reg, idx_reg, base_reg));
    if ((base_reg & 7) == 5) {
        codegen_addbyte4(block, 0x0f, opcode, 0x44 | ((reg & 7) << 3),
                         (base_reg & 7) | ((idx_reg & 7) << 3));
        codegen_addbyte(block, 0x00);
    } else {
        codegen_addbyte3(block, 0x0f, opcode, 0x04 | ((reg & 7) << 3));
        codegen_addbyte(block, (base_reg & 7) | ((idx_reg & 7) << 3));
    }
}

static inline void
emit_sse_abs_xreg(codeblock_t *block, uint8_t prefix, uint8_t opcode, void *p, int xreg, const char *name)
{
    int offset = (uintptr_t) p - (((uintptr_t) &cpu_state) + 128);
    int has_rex = (xreg & 8) != 0;

    if (offset >= -128 && offset <= 127) {
        codegen_alloc_bytes(block, 6);
        codegen_addbyte(block, prefix);
        if (has_rex)
            codegen_addbyte(block, rex_byte(0, xreg, 0, 0));
        codegen_addbyte4(block, 0x0f, opcode, 0x45 | ((xreg & 7) << 3), offset);
    } else if ((uintptr_t) p >= (uintptr_t) &cpu_state && (uintptr_t) p < (uintptr_t) &cpu_state + sizeof(cpu_state_t)) {
        codegen_alloc_bytes(block, 9);
        codegen_addbyte(block, prefix);
        if (has_rex)
            codegen_addbyte(block, rex_byte(0, xreg, 0, 0));
        codegen_addbyte3(block, 0x0f, opcode, 0x85 | ((xreg & 7) << 3));
        codegen_addlong(block, offset);
    } else {
        if ((uintptr_t) p >> 32)
            fatal("%s - out of range %p\n", name, p);
        codegen_alloc_bytes(block, 10);
        codegen_addbyte(block, prefix);
        if (has_rex)
            codegen_addbyte(block, rex_byte(0, xreg, 0, 0));
        codegen_addbyte4(block, 0x0f, opcode, 0x04 | ((xreg & 7) << 3), 0x25);
        codegen_addlong(block, (uint32_t) (uintptr_t) p);
    }
}

static inline void
emit_sse_base_offset(codeblock_t *block, uint8_t prefix, uint8_t opcode, int xreg, int base_reg, int offset, const char *name)
{
    if (offset >= -128 && offset < 127) {
        int has_rex = (xreg & 8) || (base_reg & 8);
        if ((base_reg & 7) == 4) {
            codegen_alloc_bytes(block, 7);
            codegen_addbyte(block, prefix);
            if (has_rex)
                codegen_addbyte(block, rex_byte(0, xreg, 0, base_reg));
            codegen_addbyte3(block, 0x0f, opcode, 0x44 | ((xreg & 7) << 3));
            codegen_addbyte2(block, 0x24, offset);
        } else {
            codegen_alloc_bytes(block, 6);
            codegen_addbyte(block, prefix);
            if (has_rex)
                codegen_addbyte(block, rex_byte(0, xreg, 0, base_reg));
            codegen_addbyte4(block, 0x0f, opcode, 0x40 | (base_reg & 7) | ((xreg & 7) << 3), offset);
        }
    } else
        fatal("%s - offset %i\n", name, offset);
}

static inline void
emit_sse_rsp_movdqu(codeblock_t *block, uint8_t opcode, int xreg, int base_reg, int offset, const char *name)
{
    if (base_reg != REG_RSP)
        fatal("%s - base %i offset %i reg %i\n", name, base_reg, offset, xreg);

    if (offset >= -128 && offset <= 127) {
        codegen_alloc_bytes(block, 7);
        codegen_addbyte(block, 0xf3);
        if (xreg & 8)
            codegen_addbyte(block, rex_byte(0, xreg, 0, 0));
        codegen_addbyte2(block, 0x0f, opcode);
        codegen_addbyte3(block, 0x44 | ((xreg & 7) << 3), 0x24, offset);
    } else {
        codegen_alloc_bytes(block, 10);
        codegen_addbyte(block, 0xf3);
        if (xreg & 8)
            codegen_addbyte(block, rex_byte(0, xreg, 0, 0));
        codegen_addbyte4(block, 0x0f, opcode, 0x84 | ((xreg & 7) << 3), 0x24);
        codegen_addlong(block, offset);
    }
}

void
host_x86_ADDPS_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_2op(block, 0x58, dst_reg, src_reg); /*ADDPS dst_reg, src_reg*/
}
void
host_x86_ADDSD_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0xf2, 0x58, dst_reg, src_reg);
}

void
host_x86_CMPPS_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg, int type)
{
    emit_sse_2op(block, 0xc2, dst_reg, src_reg); /*CMPPS dst_reg, src_reg, type*/
    codegen_alloc_bytes(block, 1);
    codegen_addbyte(block, type);
}

void
host_x86_COMISD_XREG_XREG(codeblock_t *block, int src_reg_a, int src_reg_b)
{
    emit_sse_pfx_2op(block, 0x66, 0x2e, src_reg_a, src_reg_b);
}

void
host_x86_CVTDQ2PS_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_2op(block, 0x5b, dst_reg, src_reg); /*CVTDQ2PS dst_reg, src_reg*/
}
void
host_x86_CVTPS2DQ_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0x5b, dst_reg, src_reg); /*CVTPS2DQ dst_reg, src_reg*/
}

void
host_x86_CVTSD2SI_REG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0xf2, 0x2d, dst_reg, src_reg); /*CVTSD2SI dst_reg, src_reg*/
}
void
host_x86_CVTSD2SI_REG64_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_w_2op(block, 0xf2, 0x2d, dst_reg, src_reg); /*CVTSD2SI dst_reg, src_reg*/
}
void
host_x86_CVTTSD2SI_REG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0xf2, 0x2c, dst_reg, src_reg); /*CVTTSD2SI dst_reg, src_reg*/
}
void
host_x86_CVTTSD2SI_REG64_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_w_2op(block, 0xf2, 0x2c, dst_reg, src_reg); /*CVTTSD2SI dst_reg, src_reg*/
}
void
host_x86_CVTSD2SS_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0xf2, 0x5a, dst_reg, src_reg);
}

void
host_x86_CVTSI2SD_XREG_REG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0xf2, 0x2a, dst_reg, src_reg); /*CVTSI2SD dst_reg, src_reg*/
}
void
host_x86_CVTSI2SS_XREG_REG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0xf3, 0x2a, dst_reg, src_reg); /*CVTSI2SS dst_reg, src_reg*/
}
void
host_x86_CVTSI2SD_XREG_REG64(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_w_2op(block, 0xf2, 0x2a, dst_reg, src_reg); /*CVTSI2SD dst_reg, src_reg*/
}

void
host_x86_CVTSS2SD_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0xf3, 0x5a, dst_reg, src_reg);
}
void
host_x86_CVTSS2SD_XREG_BASE_INDEX(codeblock_t *block, int dst_reg, int base_reg, int idx_reg)
{
    emit_sse_base_index(block, 0xf3, 0x5a, dst_reg, base_reg, idx_reg);
}

void
host_x86_DIVSD_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0xf2, 0x5e, dst_reg, src_reg);
}
void
host_x86_DIVSS_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0xf3, 0x5e, dst_reg, src_reg); /*DIVSS dst_reg, src_reg*/
}

void
host_x86_LDMXCSR(codeblock_t *block, void *p)
{
    int offset = (uintptr_t) p - (((uintptr_t) &cpu_state) + 128);

    if (offset >= -128 && offset < 127) {
        codegen_alloc_bytes(block, 4);
        codegen_addbyte4(block, 0x0f, 0xae, 0x50 | REG_EBP, offset); /*LDMXCSR offset[EBP]*/
    } else if (offset < (1ULL << 32)) {
        codegen_alloc_bytes(block, 7);
        codegen_addbyte3(block, 0x0f, 0xae, 0x90 | REG_EBP); /*LDMXCSR offset[EBP]*/
        codegen_addlong(block, offset);
    } else {
        fatal("host_x86_LDMXCSR - out of range %p\n", p);
    }
}

void
host_x86_MAXSD_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0xf2, 0x5f, dst_reg, src_reg); /*MAXSD dst_reg, src_reg*/
}

void
host_x86_MOVD_BASE_INDEX_XREG(codeblock_t *block, int base_reg, int idx_reg, int src_reg)
{
    emit_sse_base_index(block, 0x66, 0x7e, src_reg, base_reg, idx_reg);
}
void
host_x86_MOVD_REG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0x7e, src_reg, dst_reg);
}
void
host_x86_MOVD_XREG_BASE_INDEX(codeblock_t *block, int dst_reg, int base_reg, int idx_reg)
{
    emit_sse_base_index(block, 0x66, 0x6e, dst_reg, base_reg, idx_reg);
}
void
host_x86_MOVD_XREG_REG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0x6e, dst_reg, src_reg);
}

void
host_x86_MOVQ_ABS_XREG(codeblock_t *block, void *p, int src_reg)
{
    emit_sse_abs_xreg(block, 0x66, 0xd6, p, src_reg, "host_x86_MOVQ_ABS_REG");
}
void
host_x86_MOVQ_ABS_REG_REG_SHIFT_XREG(codeblock_t *block, uint32_t addr, int src_reg_a, int src_reg_b, int shift, int src_reg)
{
    int has_rex = (src_reg & 8) || (src_reg_a & 8) || (src_reg_b & 8);

    if (addr < 0x80 || addr >= 0xffffff80) {
        codegen_alloc_bytes(block, 7);
        codegen_addbyte(block, 0x66);
        if (has_rex)
            codegen_addbyte(block, rex_byte(0, src_reg, src_reg_b, src_reg_a));
        codegen_addbyte2(block, 0x0f, 0xd6); /*MOVQ addr[src_reg_a + src_reg_b << shift], XMMx*/
        codegen_addbyte3(block, 0x44 | ((src_reg & 7) << 3),
                         (src_reg_a & 7) | ((src_reg_b & 7) << 3) | (shift << 6), addr & 0xff);
    } else {
        codegen_alloc_bytes(block, 10);
        codegen_addbyte(block, 0x66);
        if (has_rex)
            codegen_addbyte(block, rex_byte(0, src_reg, src_reg_b, src_reg_a));
        codegen_addbyte2(block, 0x0f, 0xd6); /*MOVQ addr[src_reg_a + src_reg_b << shift], XMMx*/
        codegen_addbyte2(block, 0x84 | ((src_reg & 7) << 3),
                         (src_reg_a & 7) | ((src_reg_b & 7) << 3) | (shift << 6));
        codegen_addlong(block, addr);
    }
}

void
host_x86_MOVQ_BASE_INDEX_XREG(codeblock_t *block, int base_reg, int idx_reg, int src_reg)
{
    emit_sse_base_index(block, 0x66, 0xd6, src_reg, base_reg, idx_reg);
}
void
host_x86_MOVQ_BASE_OFFSET_XREG(codeblock_t *block, int base_reg, int offset, int src_reg)
{
    emit_sse_base_offset(block, 0x66, 0xd6, src_reg, base_reg, offset, "MOVQ_BASE_OFFSET_XREG");
}

void
host_x86_MOVQ_XREG_ABS(codeblock_t *block, int dst_reg, void *p)
{
    emit_sse_abs_xreg(block, 0xf3, 0x7e, p, dst_reg, "host_x86_MOVQ_REG_ABS");
}
void
host_x86_MOVQ_XREG_ABS_REG_REG_SHIFT(codeblock_t *block, int dst_reg, uint32_t addr, int src_reg_a, int src_reg_b, int shift)
{
    int has_rex = (dst_reg & 8) || (src_reg_a & 8) || (src_reg_b & 8);

    if (addr < 0x80 || addr >= 0xffffff80) {
        codegen_alloc_bytes(block, 7);
        codegen_addbyte(block, 0xf3);
        if (has_rex)
            codegen_addbyte(block, rex_byte(0, dst_reg, src_reg_b, src_reg_a));
        codegen_addbyte2(block, 0x0f, 0x7e); /*MOVQ XMMx, addr[src_reg_a + src_reg_b << shift]*/
        codegen_addbyte3(block, 0x44 | ((dst_reg & 7) << 3),
                         (src_reg_a & 7) | ((src_reg_b & 7) << 3) | (shift << 6), addr & 0xff);
    } else {
        codegen_alloc_bytes(block, 10);
        codegen_addbyte(block, 0xf3);
        if (has_rex)
            codegen_addbyte(block, rex_byte(0, dst_reg, src_reg_b, src_reg_a));
        codegen_addbyte2(block, 0x0f, 0x7e); /*MOVQ XMMx, addr[src_reg_a + src_reg_b << shift]*/
        codegen_addbyte2(block, 0x84 | ((dst_reg & 7) << 3),
                         (src_reg_a & 7) | ((src_reg_b & 7) << 3) | (shift << 6));
        codegen_addlong(block, addr);
    }
}
void
host_x86_MOVQ_XREG_BASE_INDEX(codeblock_t *block, int dst_reg, int base_reg, int idx_reg)
{
    emit_sse_base_index(block, 0xf3, 0x7e, dst_reg, base_reg, idx_reg);
}
void
host_x86_MOVQ_XREG_BASE_OFFSET(codeblock_t *block, int dst_reg, int base_reg, int offset)
{
    emit_sse_base_offset(block, 0xf3, 0x7e, dst_reg, base_reg, offset, "MOVQ_REG_BASE_OFFSET");
}

void
host_x86_MOVQ_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0xf3, 0x7e, dst_reg, src_reg); /*MOVQ dst_reg, src_reg*/
}

void
host_x86_MOVQ_REG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    codegen_alloc_bytes(block, 5);
    codegen_addbyte3(block, 0x66, rex_byte(1, src_reg, 0, dst_reg), 0x0f); /*MOVQ dst_reg, src_reg*/
    codegen_addbyte2(block, 0x7e, 0xc0 | (dst_reg & 7) | ((src_reg & 7) << 3));
}
void
host_x86_MOVQ_XREG_REG(codeblock_t *block, int dst_reg, int src_reg)
{
    codegen_alloc_bytes(block, 5);
    codegen_addbyte3(block, 0x66, rex_byte(1, dst_reg, 0, src_reg), 0x0f); /*MOVQ dst_reg, src_reg*/
    codegen_addbyte2(block, 0x6e, 0xc0 | (src_reg & 7) | ((dst_reg & 7) << 3));
}

void
host_x86_MAXPS_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_2op(block, 0x5f, dst_reg, src_reg); /*MAXPS dst_reg, src_reg*/
}
void
host_x86_MINPS_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_2op(block, 0x5d, dst_reg, src_reg); /*MINPS dst_reg, src_reg*/
}

void
host_x86_MULPS_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_2op(block, 0x59, dst_reg, src_reg); /*MULPS dst_reg, src_reg*/
}
void
host_x86_MULSD_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0xf2, 0x59, dst_reg, src_reg);
}

void
host_x86_PACKSSWB_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0x63, dst_reg, src_reg); /*PACKSSWB dst_reg, src_reg*/
    emit_sse_pfx_2op_imm(block, 0x66, 0x70, dst_reg, dst_reg, 0x88); /*PSHUFD dst_reg, dst_reg, 0x88 (move bits 64-95 to 32-63)*/
}
void
host_x86_PACKSSDW_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0x6b, dst_reg, src_reg); /*PACKSSDW dst_reg, src_reg*/
    emit_sse_pfx_2op_imm(block, 0x66, 0x70, dst_reg, dst_reg, 0x88); /*PSHUFD dst_reg, dst_reg, 0x88 (move bits 64-95 to 32-63)*/
}
void
host_x86_PACKUSWB_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0x67, dst_reg, src_reg); /*PACKUSWB dst_reg, src_reg*/
    emit_sse_pfx_2op_imm(block, 0x66, 0x70, dst_reg, dst_reg, 0x88); /*PSHUFD dst_reg, dst_reg, 0x88 (move bits 64-95 to 32-63)*/
}

void
host_x86_PADDB_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0xfc, dst_reg, src_reg); /*PADDB dst_reg, src_reg*/
}
void
host_x86_PADDW_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0xfd, dst_reg, src_reg); /*PADDW dst_reg, src_reg*/
}
void
host_x86_PADDD_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0xfe, dst_reg, src_reg); /*PADDD dst_reg, src_reg*/
}
void
host_x86_PADDSB_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0xec, dst_reg, src_reg); /*PADDSB dst_reg, src_reg*/
}
void
host_x86_PADDSW_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0xed, dst_reg, src_reg); /*PADDSW dst_reg, src_reg*/
}
void
host_x86_PADDUSB_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0xdc, dst_reg, src_reg); /*PADDUSB dst_reg, src_reg*/
}
void
host_x86_PADDUSW_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0xdd, dst_reg, src_reg); /*PADDUSW dst_reg, src_reg*/
}

void
host_x86_PAND_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0xdb, dst_reg, src_reg); /*PAND dst_reg, src_reg*/
}
void
host_x86_PANDN_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0xdf, dst_reg, src_reg); /*PANDN dst_reg, src_reg*/
}
void
host_x86_POR_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0xeb, dst_reg, src_reg); /*POR dst_reg, src_reg*/
}
void
host_x86_PXOR_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0xef, dst_reg, src_reg); /*PXOR dst_reg, src_reg*/
}

void
host_x86_PCMPEQB_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0x74, dst_reg, src_reg); /*PCMPEQB dst_reg, src_reg*/
}
void
host_x86_PCMPEQW_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0x75, dst_reg, src_reg); /*PCMPEQW dst_reg, src_reg*/
}
void
host_x86_PCMPEQD_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0x76, dst_reg, src_reg); /*PCMPEQD dst_reg, src_reg*/
}
void
host_x86_PCMPGTB_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0x64, dst_reg, src_reg); /*PCMPGTB dst_reg, src_reg*/
}
void
host_x86_PCMPGTW_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0x65, dst_reg, src_reg); /*PCMPGTW dst_reg, src_reg*/
}
void
host_x86_PCMPGTD_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0x66, dst_reg, src_reg); /*PCMPGTD dst_reg, src_reg*/
}

void
host_x86_PMADDWD_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0xf5, dst_reg, src_reg); /*PMADDWD dst_reg, src_reg*/
}
void
host_x86_PMULHW_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0xe5, dst_reg, src_reg); /*PMULHW dst_reg, src_reg*/
}
void
host_x86_PMULLW_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0xd5, dst_reg, src_reg); /*PMULLW dst_reg, src_reg*/
}

void
host_x86_PSLLW_XREG_IMM(codeblock_t *block, int dst_reg, int shift)
{
    emit_sse_shift_imm(block, 0x71, 0x30, dst_reg, shift); /*PSLLW dst_reg, imm*/
}
void
host_x86_PSLLD_XREG_IMM(codeblock_t *block, int dst_reg, int shift)
{
    emit_sse_shift_imm(block, 0x72, 0x30, dst_reg, shift); /*PSLLD dst_reg, imm*/
}
void
host_x86_PSLLQ_XREG_IMM(codeblock_t *block, int dst_reg, int shift)
{
    emit_sse_shift_imm(block, 0x73, 0x30, dst_reg, shift); /*PSLLQ dst_reg, imm*/
}
void
host_x86_PSRAW_XREG_IMM(codeblock_t *block, int dst_reg, int shift)
{
    emit_sse_shift_imm(block, 0x71, 0x20, dst_reg, shift); /*PSRAW dst_reg, imm*/
}
void
host_x86_PSRAD_XREG_IMM(codeblock_t *block, int dst_reg, int shift)
{
    emit_sse_shift_imm(block, 0x72, 0x20, dst_reg, shift); /*PSRAD dst_reg, imm*/
}
void
host_x86_PSRAQ_XREG_IMM(codeblock_t *block, int dst_reg, int shift)
{
    emit_sse_shift_imm(block, 0x73, 0x20, dst_reg, shift); /*PSRAQ dst_reg, imm*/
}
void
host_x86_PSRLW_XREG_IMM(codeblock_t *block, int dst_reg, int shift)
{
    emit_sse_shift_imm(block, 0x71, 0x10, dst_reg, shift); /*PSRLW dst_reg, imm*/
}
void
host_x86_PSRLD_XREG_IMM(codeblock_t *block, int dst_reg, int shift)
{
    emit_sse_shift_imm(block, 0x72, 0x10, dst_reg, shift); /*PSRLD dst_reg, imm*/
}
void
host_x86_PSRLQ_XREG_IMM(codeblock_t *block, int dst_reg, int shift)
{
    emit_sse_shift_imm(block, 0x73, 0x10, dst_reg, shift); /*PSRLQ dst_reg, imm*/
}

void
host_x86_PSRLW_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0xd1, dst_reg, src_reg); /*PSRLW dst_reg, src_reg*/
}
void
host_x86_PSRLD_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0xd2, dst_reg, src_reg); /*PSRLD dst_reg, src_reg*/
}
void
host_x86_PSRLQ_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0xd3, dst_reg, src_reg); /*PSRLQ dst_reg, src_reg*/
}
void
host_x86_PSRAW_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0xe1, dst_reg, src_reg); /*PSRAW dst_reg, src_reg*/
}
void
host_x86_PSRAD_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0xe2, dst_reg, src_reg); /*PSRAD dst_reg, src_reg*/
}
void
host_x86_PSLLW_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0xf1, dst_reg, src_reg); /*PSLLW dst_reg, src_reg*/
}
void
host_x86_PSLLD_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0xf2, dst_reg, src_reg); /*PSLLD dst_reg, src_reg*/
}
void
host_x86_PSLLQ_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0xf3, dst_reg, src_reg); /*PSLLQ dst_reg, src_reg*/
}

void
host_x86_PSUBB_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0xf8, dst_reg, src_reg); /*PSUBB dst_reg, src_reg*/
}
void
host_x86_PSUBW_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0xf9, dst_reg, src_reg); /*PSUBW dst_reg, src_reg*/
}
void
host_x86_PSUBD_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0xfa, dst_reg, src_reg); /*PSUBD dst_reg, src_reg*/
}
void
host_x86_PSUBSB_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0xe8, dst_reg, src_reg); /*PSUBSB dst_reg, src_reg*/
}
void
host_x86_PSUBSW_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0xe9, dst_reg, src_reg); /*PSUBSW dst_reg, src_reg*/
}
void
host_x86_PSUBUSB_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0xd8, dst_reg, src_reg); /*PSUBUSB dst_reg, src_reg*/
}
void
host_x86_PSUBUSW_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0xd9, dst_reg, src_reg); /*PSUBUSW dst_reg, src_reg*/
}

void
host_x86_PUNPCKHBW_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0x60, dst_reg, src_reg); /*PUNPCKLBW dst_reg, src_reg*/
    emit_sse_pfx_2op_imm(block, 0x66, 0x70, dst_reg, dst_reg, 0xee); /*PSHUFD dst_reg, dst_reg, 0xee (move top 64-bits to low 64-bits)*/
}
void
host_x86_PUNPCKHWD_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0x61, dst_reg, src_reg); /*PUNPCKLWD dst_reg, src_reg*/
    emit_sse_pfx_2op_imm(block, 0x66, 0x70, dst_reg, dst_reg, 0xee); /*PSHUFD dst_reg, dst_reg, 0xee (move top 64-bits to low 64-bits)*/
}
void
host_x86_PUNPCKHDQ_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0x62, dst_reg, src_reg); /*PUNPCKLDQ dst_reg, src_reg*/
    emit_sse_pfx_2op_imm(block, 0x66, 0x70, dst_reg, dst_reg, 0xee); /*PSHUFD dst_reg, dst_reg, 0xee (move top 64-bits to low 64-bits)*/
}
void
host_x86_PUNPCKLBW_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0x60, dst_reg, src_reg); /*PUNPCKLBW dst_reg, src_reg*/
}
void
host_x86_PUNPCKLWD_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0x61, dst_reg, src_reg); /*PUNPCKLWD dst_reg, src_reg*/
}
void
host_x86_PUNPCKLDQ_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0x66, 0x62, dst_reg, src_reg); /*PUNPCKLDQ dst_reg, src_reg*/
}

void
host_x86_SQRTSD_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0xf2, 0x51, dst_reg, src_reg); /*SQRTSD dst_reg, src_reg*/
}
void
host_x86_SQRTSS_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0xf3, 0x51, dst_reg, src_reg); /*SQRTSS dst_reg, src_reg*/
}

void
host_x86_SUBPS_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_2op(block, 0x5c, dst_reg, src_reg); /*SUBPS dst_reg, src_reg*/
}
void
host_x86_SUBSD_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_pfx_2op(block, 0xf2, 0x5c, dst_reg, src_reg);
}

void
host_x86_UNPCKLPS_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg)
{
    emit_sse_2op(block, 0x14, dst_reg, src_reg);
}

void
host_x86_ROUNDSD_XREG_XREG(codeblock_t *block, int dst_reg, int src_reg, int mode)
{
    codegen_alloc_bytes(block, 7);
    codegen_addbyte(block, 0x66);
    if ((dst_reg & 8) || (src_reg & 8))
        codegen_addbyte(block, rex_byte(0, dst_reg, 0, src_reg));
    codegen_addbyte3(block, 0x0f, 0x3a, 0x0b);
    codegen_addbyte2(block, 0xc0 | (src_reg & 7) | ((dst_reg & 7) << 3), mode & 0xff); /*ROUNDSD dst_reg, src_reg, mode*/
}

/* MOVDQU [base_reg + offset], src_reg: a whole XMM register to the stack
   frame (Windows x64 prologue and SysV cold-slowpath spills). */
void
host_x86_MOVDQU_BASE_OFFSET_XREG(codeblock_t *block, int base_reg, int offset, int src_reg)
{
    emit_sse_rsp_movdqu(block, 0x7f, src_reg, base_reg, offset, "host_x86_MOVDQU_BASE_OFFSET_XREG");
}
void
host_x86_MOVDQU_XREG_BASE_OFFSET(codeblock_t *block, int dst_reg, int base_reg, int offset)
{
    emit_sse_rsp_movdqu(block, 0x6f, dst_reg, base_reg, offset, "host_x86_MOVDQU_XREG_BASE_OFFSET");
}

#endif

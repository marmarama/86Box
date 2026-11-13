#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <86box/86box.h>
#include "cpu.h"
#include <86box/mem.h>
#include <86box/plat_unused.h>

#include "codegen.h"
#include "codegen_allocator.h"
#include "codegen_backend.h"
#include "codegen_dump.h"
#include "codegen_ir.h"
#include "codegen_reg.h"

static uint32_t dump_orig_types[UOP_NR_MAX];
static uint32_t dump_orig_imms[UOP_NR_MAX];
static uint16_t dump_host_starts[UOP_NR_MAX];
static uint16_t dump_host_body_starts[UOP_NR_MAX];
static uint16_t dump_host_ends[UOP_NR_MAX];

extern int       has_ea;
static ir_data_t ir_block;

static int codegen_unroll_start;
int        codegen_unroll_count;
static int codegen_unroll_first_instruction;

ir_data_t *
codegen_ir_init(void)
{
    ir_block.wr_pos = 0;

    codegen_unroll_count = 0;

    return &ir_block;
}

void
codegen_ir_set_unroll(int count, int start, int first_instruction)
{
    codegen_unroll_count             = count;
    codegen_unroll_start             = start;
    codegen_unroll_first_instruction = first_instruction;
}

static void
duplicate_uop(ir_data_t *ir, uop_t *uop, int offset)
{
    uop_t *new_uop = uop_alloc_unroll(ir, uop->type);

    if (!ir_reg_is_invalid(uop->src_reg_a))
        new_uop->src_reg_a = codegen_reg_read(uop->src_reg_a.reg);
    if (!ir_reg_is_invalid(uop->src_reg_b))
        new_uop->src_reg_b = codegen_reg_read(uop->src_reg_b.reg);
    if (!ir_reg_is_invalid(uop->src_reg_c))
        new_uop->src_reg_c = codegen_reg_read(uop->src_reg_c.reg);
    if (!ir_reg_is_invalid(uop->dest_reg_a))
        new_uop->dest_reg_a = codegen_reg_write(uop->dest_reg_a.reg, ir->wr_pos - 1);

    new_uop->type           = uop->type;
    new_uop->imm_data       = uop->imm_data;
    new_uop->p              = uop->p;
    new_uop->pc             = uop->pc;
    new_uop->guest_insn_idx = uop->guest_insn_idx;
    new_uop->guest_pc_off   = uop->guest_pc_off;
#if defined __ARM_EABI__ || defined _ARM_ || defined _M_ARM || defined __aarch64__ || defined _M_ARM64 || defined __loongarch_lp64
    new_uop->is_a16   = uop->is_a16;
#endif

    if (uop->jump_dest_uop != -1) {
        new_uop->jump_dest_uop = uop->jump_dest_uop + offset;
    }
}

static inline void
peephole_release_src_reg(ir_reg_t ir_reg)
{
    if (ir_reg.reg != IREG_INVALID) {
        reg_version_t *src_regv = &reg_version[IREG_GET_REG(ir_reg.reg)][ir_reg.version];
        src_regv->refcount--;
        if (!src_regv->refcount && ir_reg.version > 0 && !(src_regv->flags & REG_FLAGS_REQUIRED))
            add_to_dead_list(src_regv, IREG_GET_REG(ir_reg.reg), ir_reg.version);
    }
}

#define FLAG_LIVE_OP   (1 << 0)
#define FLAG_LIVE_RES  (1 << 1)
#define FLAG_LIVE_OP1  (1 << 2)
#define FLAG_LIVE_OP2  (1 << 3)
#define FLAGS_ALL_LIVE (FLAG_LIVE_OP | FLAG_LIVE_RES | FLAG_LIVE_OP1 | FLAG_LIVE_OP2)

static inline void
invalidate_dead_uop(uop_t *uop)
{
    if (uop->dest_reg_a.reg != IREG_INVALID) {
        int dreg = IREG_GET_REG(uop->dest_reg_a.reg);
        reg_version_t *dest_regv = &reg_version[dreg][uop->dest_reg_a.version];
        dest_regv->flags &= ~REG_FLAGS_REQUIRED;
        dest_regv->flags |= REG_FLAGS_DEAD;
    }
    peephole_release_src_reg(uop->src_reg_a);
    peephole_release_src_reg(uop->src_reg_b);
    peephole_release_src_reg(uop->src_reg_c);
    uop->src_reg_a = invalid_ir_reg;
    uop->src_reg_b = invalid_ir_reg;
    uop->src_reg_c = invalid_ir_reg;
    uop->dest_reg_a = invalid_ir_reg;
    uop->type = UOP_INVALID;
}

void
codegen_ir_optimize_liveness(ir_data_t *ir)
{
    int flags_live = FLAGS_ALL_LIVE;
    int pc_live = 1;
    int oldpc_live = 0;

    for (int c = ir->wr_pos - 1; c >= 0; c--) {
        uop_t *uop = &ir->uops[c];

        if ((uop->type & UOP_MASK) == UOP_INVALID)
            continue;

        /* Invalidate liveness optimizations across barriers, branches, and loop boundaries.
           At these points, architectural flags and PC states must be live unless the
           barrier is a fast-path memory access that materializes oldpc in its cold stub
           or a non-faulting control barrier. */
        if (uop->type & (UOP_TYPE_BARRIER | UOP_TYPE_ORDER_BARRIER | UOP_TYPE_JUMP_DEST | UOP_TYPE_JUMP)) {
            flags_live = FLAGS_ALL_LIVE;
#if defined __amd64__ || defined _M_X64
            if (!uop_is_mem_access(uop->type) || (uop->type & UOP_TYPE_JUMP_DEST))
                pc_live = 1;
            if ((uop->type & (UOP_TYPE_BARRIER | UOP_TYPE_ORDER_BARRIER)) &&
                !uop_is_mem_access(uop->type) &&
                !uop_is_non_faulting_barrier(uop->type))
                oldpc_live = 1;
#else
            pc_live = 1;
            if ((uop->type & (UOP_TYPE_BARRIER | UOP_TYPE_ORDER_BARRIER)) &&
                !uop_is_non_faulting_barrier(uop->type))
                oldpc_live = 1;
#endif
            continue;
        }
        if ((uop->type & UOP_MASK) == (UOP_LOOP_LABEL & UOP_MASK)) {
            flags_live = FLAGS_ALL_LIVE;
            pc_live = 1;
            continue;
        }

        /* Check destination register write for dead store elimination */
        if (uop->dest_reg_a.reg != IREG_INVALID) {
            int dreg = IREG_GET_REG(uop->dest_reg_a.reg);
            int version = uop->dest_reg_a.version;
            int refcount = reg_version[dreg][version].refcount;
            int required = (reg_version[dreg][version].flags & REG_FLAGS_REQUIRED) != 0;
            int is_native = reg_is_native_size(uop->dest_reg_a);

            if (dreg == IREG_flags_op) {
                if (!(flags_live & FLAG_LIVE_OP) && is_native && refcount == 0 && !required) {
                    invalidate_dead_uop(uop);
                    continue;
                }
                flags_live &= ~FLAG_LIVE_OP;
            } else if (dreg == IREG_flags_res) {
                if (!(flags_live & FLAG_LIVE_RES) && is_native && refcount == 0 && !required) {
                    invalidate_dead_uop(uop);
                    continue;
                }
                flags_live &= ~FLAG_LIVE_RES;
            } else if (dreg == IREG_flags_op1) {
                if (!(flags_live & FLAG_LIVE_OP1) && is_native && refcount == 0 && !required) {
                    invalidate_dead_uop(uop);
                    continue;
                }
                flags_live &= ~FLAG_LIVE_OP1;
            } else if (dreg == IREG_flags_op2) {
                if (!(flags_live & FLAG_LIVE_OP2) && is_native && refcount == 0 && !required) {
                    invalidate_dead_uop(uop);
                    continue;
                }
                flags_live &= ~FLAG_LIVE_OP2;
            } else if (dreg == IREG_oldpc) {
                if (!oldpc_live && is_native && refcount == 0 && !required) {
                    invalidate_dead_uop(uop);
                    continue;
                }
                oldpc_live = 0;
            } else if (dreg == IREG_pc) {
                if (!pc_live && is_native && refcount == 0 && !required) {
                    invalidate_dead_uop(uop);
                    continue;
                }
                pc_live = 0;
            }

            /* Partial (non-native size) writes have an implicit dependency on the previous version */
            if (!is_native) {
                if (dreg == IREG_flags_op)
                    flags_live |= FLAG_LIVE_OP;
                else if (dreg == IREG_flags_res)
                    flags_live |= FLAG_LIVE_RES;
                else if (dreg == IREG_flags_op1)
                    flags_live |= FLAG_LIVE_OP1;
                else if (dreg == IREG_flags_op2)
                    flags_live |= FLAG_LIVE_OP2;
                else if (dreg == IREG_oldpc)
                    oldpc_live = 1;
                else if (dreg == IREG_pc)
                    pc_live = 1;
            }
        }

        /* Check source registers to mark inputs as live */
        if (uop->src_reg_a.reg != IREG_INVALID) {
            int sreg = IREG_GET_REG(uop->src_reg_a.reg);
            if (sreg == IREG_flags_op)
                flags_live |= FLAG_LIVE_OP;
            else if (sreg == IREG_flags_res)
                flags_live |= FLAG_LIVE_RES;
            else if (sreg == IREG_flags_op1)
                flags_live |= FLAG_LIVE_OP1;
            else if (sreg == IREG_flags_op2)
                flags_live |= FLAG_LIVE_OP2;
            else if (sreg == IREG_oldpc)
                oldpc_live = 1;
            else if (sreg == IREG_pc)
                pc_live = 1;
        }

        if (uop->src_reg_b.reg != IREG_INVALID) {
            int sreg = IREG_GET_REG(uop->src_reg_b.reg);
            if (sreg == IREG_flags_op)
                flags_live |= FLAG_LIVE_OP;
            else if (sreg == IREG_flags_res)
                flags_live |= FLAG_LIVE_RES;
            else if (sreg == IREG_flags_op1)
                flags_live |= FLAG_LIVE_OP1;
            else if (sreg == IREG_flags_op2)
                flags_live |= FLAG_LIVE_OP2;
            else if (sreg == IREG_oldpc)
                oldpc_live = 1;
            else if (sreg == IREG_pc)
                pc_live = 1;
        }

        if (uop->src_reg_c.reg != IREG_INVALID) {
            int sreg = IREG_GET_REG(uop->src_reg_c.reg);
            if (sreg == IREG_flags_op)
                flags_live |= FLAG_LIVE_OP;
            else if (sreg == IREG_flags_res)
                flags_live |= FLAG_LIVE_RES;
            else if (sreg == IREG_flags_op1)
                flags_live |= FLAG_LIVE_OP1;
            else if (sreg == IREG_flags_op2)
                flags_live |= FLAG_LIVE_OP2;
            else if (sreg == IREG_oldpc)
                oldpc_live = 1;
            else if (sreg == IREG_pc)
                pc_live = 1;
        }
    }
}


void
codegen_ir_peephole(ir_data_t *ir)
{
    uint32_t const_val[IREG_COUNT];
    uint8_t  const_version[IREG_COUNT];
    uint8_t  const_valid[IREG_COUNT];

    memset(const_valid, 0, sizeof(const_valid));

    for (int c = 0; c < ir->wr_pos; c++) {
        uop_t *uop = &ir->uops[c];

        if ((uop->type & UOP_MASK) == UOP_INVALID)
            continue;

        /* Invalidate constants across barriers, branches, and loop boundaries */
        if (uop->type & (UOP_TYPE_BARRIER | UOP_TYPE_ORDER_BARRIER | UOP_TYPE_JUMP_DEST | UOP_TYPE_JUMP)) {
            memset(const_valid, 0, sizeof(const_valid));
        }
        if ((uop->type & UOP_MASK) == (UOP_LOOP_LABEL & UOP_MASK)) {
            memset(const_valid, 0, sizeof(const_valid));
            continue;
        }

        uint32_t uop_masked = uop->type & UOP_MASK;

        /* 1. Self-cancelling operations: XOR R, R or SUB R, R -> MOV_IMM 0 (native size integer) */
        if (uop_masked == (UOP_XOR & UOP_MASK) || uop_masked == (UOP_SUB & UOP_MASK)) {
            if (uop->src_reg_a.reg != IREG_INVALID &&
                reg_is_integer(uop->dest_reg_a) &&
                reg_is_native_size(uop->dest_reg_a) &&
                reg_is_native_size(uop->src_reg_a) &&
                uop->src_reg_a.reg == uop->src_reg_b.reg &&
                uop->src_reg_a.version == uop->src_reg_b.version) {
                peephole_release_src_reg(uop->src_reg_a);
                peephole_release_src_reg(uop->src_reg_b);
                uop->type = (uop->type & ~UOP_MASK) | (UOP_MOV_IMM & UOP_MASK);
                uop->type |= UOP_TYPE_PARAMS_IMM;
                uop->imm_data  = 0;
                uop->src_reg_a = invalid_ir_reg;
                uop->src_reg_b = invalid_ir_reg;

                int dreg            = IREG_GET_REG(uop->dest_reg_a.reg);
                const_val[dreg]     = 0;
                const_version[dreg] = uop->dest_reg_a.version;
                const_valid[dreg]   = 1;
                continue;
            }
        }

        /* 2. Check if src_reg_a has a known constant (native size integer) */
        int      src_a_is_const = 0;
        uint32_t c_val_a        = 0;
        if (uop->src_reg_a.reg != IREG_INVALID && reg_is_integer(uop->src_reg_a) && reg_is_native_size(uop->src_reg_a)) {
            int sreg = IREG_GET_REG(uop->src_reg_a.reg);
            if (const_valid[sreg] && const_version[sreg] == uop->src_reg_a.version) {
                src_a_is_const = 1;
                c_val_a        = const_val[sreg];
            }
        }

        /* 3. Constant propagation into immediate ALU operations (native size integer) */
        if (src_a_is_const && (uop->dest_reg_a.reg != IREG_INVALID) &&
            reg_is_integer(uop->dest_reg_a) &&
            reg_is_native_size(uop->dest_reg_a)) {
            int      folded = 0;
            uint32_t res    = 0;

            if (uop_masked == (UOP_ADD_IMM & UOP_MASK)) {
                res    = c_val_a + uop->imm_data;
                folded = 1;
            } else if (uop_masked == (UOP_SUB_IMM & UOP_MASK)) {
                res    = c_val_a - uop->imm_data;
                folded = 1;
            } else if (uop_masked == (UOP_AND_IMM & UOP_MASK)) {
                res    = c_val_a & uop->imm_data;
                folded = 1;
            } else if (uop_masked == (UOP_OR_IMM & UOP_MASK)) {
                res    = c_val_a | uop->imm_data;
                folded = 1;
            } else if (uop_masked == (UOP_XOR_IMM & UOP_MASK)) {
                res    = c_val_a ^ uop->imm_data;
                folded = 1;
            } else if (uop_masked == (UOP_SHL_IMM & UOP_MASK)) {
                res    = c_val_a << (uop->imm_data & 31);
                folded = 1;
            } else if (uop_masked == (UOP_SHR_IMM & UOP_MASK)) {
                res    = c_val_a >> (uop->imm_data & 31);
                folded = 1;
            } else if (uop_masked == (UOP_SAR_IMM & UOP_MASK)) {
                res    = (uint32_t) (((int32_t) c_val_a) >> (uop->imm_data & 31));
                folded = 1;
            } else if (uop_masked == (UOP_IMUL_IMM & UOP_MASK)) {
                res    = (uint32_t) (((int32_t) c_val_a) * ((int32_t) uop->imm_data));
                folded = 1;
            }

            if (folded) {
                peephole_release_src_reg(uop->src_reg_a);
                uop->type = (uop->type & ~UOP_MASK) | (UOP_MOV_IMM & UOP_MASK);
                uop->type |= UOP_TYPE_PARAMS_IMM;
                uop->imm_data  = res;
                uop->src_reg_a = invalid_ir_reg;

                int dreg            = IREG_GET_REG(uop->dest_reg_a.reg);
                const_val[dreg]     = res;
                const_version[dreg] = uop->dest_reg_a.version;
                const_valid[dreg]   = 1;
                continue;
            }
        }

        /* 4. Update or invalidate constant tracking on write */
        if (uop->dest_reg_a.reg != IREG_INVALID) {
            int dreg = IREG_GET_REG(uop->dest_reg_a.reg);
            if ((uop->type & UOP_MASK) == (UOP_MOV_IMM & UOP_MASK) && reg_is_integer(uop->dest_reg_a) && reg_is_native_size(uop->dest_reg_a)) {
                const_val[dreg]     = uop->imm_data;
                const_version[dreg] = uop->dest_reg_a.version;
                const_valid[dreg]   = 1;
            } else {
                const_valid[dreg] = 0;
            }
        }
    }
}

int codegen_opt_bfx = 1;

static inline int
is_subreg_int(ir_reg_t r)
{
    if (ir_reg_is_invalid(r) || !reg_is_integer(r))
        return 0;
    int sz = IREG_GET_SIZE(r.reg);
    return (sz == IREG_SIZE_B || sz == IREG_SIZE_BH || sz == IREG_SIZE_W);
}

static inline int
subreg_lsb(int reg_with_size)
{
    return (IREG_GET_SIZE(reg_with_size) == IREG_SIZE_BH) ? 8 : 0;
}

static inline int
subreg_width(int reg_with_size)
{
    int sz = IREG_GET_SIZE(reg_with_size);
    if (sz == IREG_SIZE_B || sz == IREG_SIZE_BH)
        return 8;
    if (sz == IREG_SIZE_W)
        return 16;
    return 32;
}

static inline int
reg_needs_bfi(int r)
{
    /* Only architectural 32-bit GPRs (EAX..EDI) preserve upper bits across 8/16-bit writes. */
    return (r >= IREG_EAX && r <= IREG_EDI);
}

static uop_t orig_uops_buf[UOP_NR_MAX];
static int   uop_map_buf[UOP_NR_MAX + 1];

static inline uop_t *
lower_copy_uop(ir_data_t *ir, const uop_t *u, uint32_t new_type)
{
    uop_t *new_uop          = uop_alloc_unroll(ir, new_type);
    new_uop->type           = new_type;
    new_uop->imm_data       = u->imm_data;
    new_uop->p              = u->p;
    new_uop->pc             = u->pc;
    new_uop->guest_insn_idx = u->guest_insn_idx;
    new_uop->guest_pc_off   = u->guest_pc_off;
    new_uop->is_a16         = u->is_a16;
    new_uop->jump_dest_uop  = u->jump_dest_uop;
    new_uop->dest_reg_a     = u->dest_reg_a;
    new_uop->src_reg_a      = u->src_reg_a;
    new_uop->src_reg_b      = u->src_reg_b;
    new_uop->src_reg_c      = u->src_reg_c;
    return new_uop;
}

static int
lower_read_subreg(ir_data_t *ir, ir_reg_t src_reg, int is_signed, int temp_reg,
                  uint8_t *zext_width)
{
    int r     = IREG_GET_REG(src_reg.reg);
    int lsb   = subreg_lsb(src_reg.reg);
    int width = subreg_width(src_reg.reg);

    if (!reg_needs_bfi(r) && !is_signed && lsb == 0 && zext_width[r] != 0 && zext_width[r] <= width) {
        return r | IREG_SIZE_L;
    }

    uop_BFX(ir, temp_reg, r | IREG_SIZE_L, lsb, width, is_signed);
    zext_width[IREG_GET_REG(temp_reg)] = is_signed ? 0 : (uint8_t) width;
    return temp_reg;
}

static void
codegen_ir_lower_subregs(ir_data_t *ir)
{
    int      orig_count     = ir->wr_pos;
    uint16_t saved_insn_idx = ir->cur_guest_insn_idx;
    uint16_t saved_pc_off   = ir->cur_guest_pc_off;
    uint8_t  zext_width[IREG_COUNT];
    uint8_t  is_jump_dest[UOP_NR_MAX];

    if (orig_count <= 0)
        return;

    memcpy(orig_uops_buf, ir->uops, orig_count * sizeof(uop_t));
    memset(zext_width, 0, sizeof(zext_width));
    memset(is_jump_dest, 0, orig_count * sizeof(uint8_t));

    for (int i = 0; i < orig_count; i++) {
        if ((orig_uops_buf[i].type & UOP_TYPE_JUMP) &&
            orig_uops_buf[i].jump_dest_uop >= 0 &&
            orig_uops_buf[i].jump_dest_uop < orig_count) {
            is_jump_dest[orig_uops_buf[i].jump_dest_uop] = 1;
        }
        if ((orig_uops_buf[i].type & UOP_MASK) == (UOP_LOOP_JMP & UOP_MASK) && orig_uops_buf[i].p != NULL) {
            uintptr_t p_val     = (uintptr_t) orig_uops_buf[i].p;
            uintptr_t base_ir   = (uintptr_t) ir->uops;
            uintptr_t base_orig = (uintptr_t) orig_uops_buf;
            int       old_idx   = -1;
            if (p_val >= base_ir && p_val < base_ir + orig_count * sizeof(uop_t))
                old_idx = (p_val - base_ir) / sizeof(uop_t);
            else if (p_val >= base_orig && p_val < base_orig + orig_count * sizeof(uop_t))
                old_idx = (p_val - base_orig) / sizeof(uop_t);
            if (old_idx >= 0 && old_idx < orig_count)
                is_jump_dest[old_idx] = 1;
        }
    }

    codegen_reg_reset();
    ir->wr_pos = 0;

    for (int i = 0; i < orig_count; i++) {
        uop_t   *u          = &orig_uops_buf[i];
        uint32_t uop_masked = u->type & UOP_MASK;

        uop_map_buf[i]         = ir->wr_pos;
        ir->cur_guest_insn_idx = u->guest_insn_idx;
        ir->cur_guest_pc_off   = u->guest_pc_off;

        if (is_jump_dest[i] || (u->type & UOP_TYPE_BARRIER)) {
            memset(zext_width, 0, sizeof(zext_width));
            if (is_jump_dest[i])
                uop_mark_barrier_dirty_regs(UOP_TYPE_JUMP_DEST);
        }

        if (uop_masked == UOP_INVALID)
            continue;

        if (uop_masked == (UOP_LOOP_LABEL & UOP_MASK)) {
            memset(zext_width, 0, sizeof(zext_width));
            lower_copy_uop(ir, u, u->type);
            continue;
        }

        /* 1. UOP_MOVZX / UOP_MOVSX from sub-register */
        if ((uop_masked == (UOP_MOVZX & UOP_MASK) || uop_masked == (UOP_MOVSX & UOP_MASK)) &&
            is_subreg_int(u->src_reg_a)) {
            int is_signed = (uop_masked == (UOP_MOVSX & UOP_MASK));
            int sreg      = IREG_GET_REG(u->src_reg_a.reg);
            int s_lsb     = subreg_lsb(u->src_reg_a.reg);
            int s_width   = subreg_width(u->src_reg_a.reg);
            int dreg      = IREG_GET_REG(u->dest_reg_a.reg);
            int d_is_sub  = is_subreg_int(u->dest_reg_a);
            int d_lsb     = d_is_sub ? subreg_lsb(u->dest_reg_a.reg) : 0;
            int d_width   = d_is_sub ? subreg_width(u->dest_reg_a.reg) : 32;

            if (d_is_sub && reg_needs_bfi(dreg)) {
                if (!reg_needs_bfi(sreg) && !is_signed && s_lsb == 0 && zext_width[sreg] != 0 && zext_width[sreg] <= s_width) {
                    uop_MOV(ir, IREG_temp4, sreg | IREG_SIZE_L);
                } else {
                    uop_BFX(ir, IREG_temp4, sreg | IREG_SIZE_L, s_lsb, s_width, is_signed);
                }
                uop_BFI(ir, dreg | IREG_SIZE_L, dreg | IREG_SIZE_L, IREG_temp4, d_lsb, d_width);
                zext_width[dreg]       = 0;
                zext_width[IREG_temp4] = is_signed ? 0 : (uint8_t) s_width;
            } else {
                int target_dreg = dreg | IREG_SIZE_L;
                if (!reg_needs_bfi(sreg) && !is_signed && s_lsb == 0 && zext_width[sreg] != 0 && zext_width[sreg] <= s_width) {
                    if (dreg != sreg || is_jump_dest[i])
                        uop_MOV(ir, target_dreg, sreg | IREG_SIZE_L);
                } else {
                    uop_BFX(ir, target_dreg, sreg | IREG_SIZE_L, s_lsb, s_width, is_signed);
                }
                if (reg_is_dword_native(dreg))
                    zext_width[dreg] = is_signed ? 0 : (uint8_t) s_width;
                else if (dreg < IREG_COUNT)
                    zext_width[dreg] = 0;
            }
            continue;
        }

        /* 2. Memory accesses: preserve native memory operations intact */
        if (uop_is_mem_access(u->type)) {
            uop_t *new_uop = lower_copy_uop(ir, u, u->type);
            if (!ir_reg_is_invalid(u->src_reg_a))
                new_uop->src_reg_a = codegen_reg_read(u->src_reg_a.reg);
            if (!ir_reg_is_invalid(u->src_reg_b))
                new_uop->src_reg_b = codegen_reg_read(u->src_reg_b.reg);
            if (!ir_reg_is_invalid(u->src_reg_c))
                new_uop->src_reg_c = codegen_reg_read(u->src_reg_c.reg);
            if (!ir_reg_is_invalid(u->dest_reg_a)) {
                int dreg = IREG_GET_REG(u->dest_reg_a.reg);
                if (is_subreg_int(u->dest_reg_a) && reg_needs_bfi(dreg)) {
                    int d_lsb      = subreg_lsb(u->dest_reg_a.reg);
                    int d_width    = subreg_width(u->dest_reg_a.reg);
                    int temp_sized = IREG_temp4 | (d_width == 16 ? IREG_SIZE_W : IREG_SIZE_B);
                    new_uop->dest_reg_a = codegen_reg_write(temp_sized, ir->wr_pos - 1);
                    uop_BFI(ir, dreg | IREG_SIZE_L, dreg | IREG_SIZE_L, IREG_temp4, d_lsb, d_width);
                    zext_width[dreg]       = 0;
                    zext_width[IREG_temp4] = (uint8_t) d_width;
                } else {
                    new_uop->dest_reg_a = codegen_reg_write(u->dest_reg_a.reg, ir->wr_pos - 1);
                    if (dreg < IREG_COUNT)
                        zext_width[dreg] = 0;
                }
            }
            continue;
        }

        /* 4. Native Shifts and Rotates: keep hardware semantics intact */
        if (uop_masked == (UOP_ROL & UOP_MASK) || uop_masked == (UOP_ROL_IMM & UOP_MASK) ||
            uop_masked == (UOP_ROR & UOP_MASK) || uop_masked == (UOP_ROR_IMM & UOP_MASK) ||
            uop_masked == (UOP_SHL & UOP_MASK) || uop_masked == (UOP_SHL_IMM & UOP_MASK) ||
            uop_masked == (UOP_SHR & UOP_MASK) || uop_masked == (UOP_SHR_IMM & UOP_MASK) ||
            uop_masked == (UOP_SAR & UOP_MASK) || uop_masked == (UOP_SAR_IMM & UOP_MASK)) {
            uop_t *new_uop     = lower_copy_uop(ir, u, u->type);
            if (!ir_reg_is_invalid(u->src_reg_a))
                new_uop->src_reg_a = codegen_reg_read(u->src_reg_a.reg);
            if (!ir_reg_is_invalid(u->src_reg_b))
                new_uop->src_reg_b = codegen_reg_read(u->src_reg_b.reg);
            if (!ir_reg_is_invalid(u->src_reg_c))
                new_uop->src_reg_c = codegen_reg_read(u->src_reg_c.reg);
            if (!ir_reg_is_invalid(u->dest_reg_a)) {
                int dreg = IREG_GET_REG(u->dest_reg_a.reg);
                if (is_subreg_int(u->dest_reg_a) && reg_needs_bfi(dreg)) {
                    int d_lsb      = subreg_lsb(u->dest_reg_a.reg);
                    int d_width    = subreg_width(u->dest_reg_a.reg);
                    int temp_sized = IREG_temp4 | (d_width == 16 ? IREG_SIZE_W : IREG_SIZE_B);
                    new_uop->dest_reg_a = codegen_reg_write(temp_sized, ir->wr_pos - 1);
                    uop_BFI(ir, dreg | IREG_SIZE_L, dreg | IREG_SIZE_L, IREG_temp4, d_lsb, d_width);
                    zext_width[dreg]       = 0;
                    zext_width[IREG_temp4] = (uop_masked == (UOP_SHR & UOP_MASK) || uop_masked == (UOP_SHR_IMM & UOP_MASK)) ? (uint8_t) d_width : 0;
                } else {
                    new_uop->dest_reg_a = codegen_reg_write(u->dest_reg_a.reg, ir->wr_pos - 1);
                    if (dreg < IREG_COUNT)
                        zext_width[dreg] = 0;
                }
            }
            continue;
        }

        /* 5. Read-only subreg consumers (function args, branches, SETCC) */
        if (uop_masked >= (UOP_LOAD_FUNC_ARG_0 & UOP_MASK) && uop_masked <= (UOP_LOAD_FUNC_ARG_3 & UOP_MASK)) {
            uop_t *new_uop     = lower_copy_uop(ir, u, u->type);
            new_uop->src_reg_a = codegen_reg_read(u->src_reg_a.reg);
            continue;
        }
        if (uop_masked == (UOP_TEST_JNS_DEST & UOP_MASK) || uop_masked == (UOP_TEST_JS_DEST & UOP_MASK) ||
            uop_masked == (UOP_CMP_IMM_JZ & UOP_MASK) || uop_masked == (UOP_CMP_IMM_JZ_DEST & UOP_MASK) ||
            uop_masked == (UOP_CMP_IMM_JNZ_DEST & UOP_MASK) || uop_masked == (UOP_CMP_IMM_JLE_DEST & UOP_MASK) ||
            uop_masked == (UOP_CMP_IMM_JNLE_DEST & UOP_MASK)) {
            uop_t *new_uop     = lower_copy_uop(ir, u, u->type);
            new_uop->src_reg_a = codegen_reg_read(u->src_reg_a.reg);
            memset(zext_width, 0, sizeof(zext_width));
            continue;
        }
        if ((uop_masked >= (UOP_CMP_JB_DEST & UOP_MASK) && uop_masked <= (UOP_CMP_JNLE_DEST & UOP_MASK)) ||
            uop_masked == (UOP_CMP_JB & UOP_MASK) || uop_masked == (UOP_CMP_JNBE & UOP_MASK)) {
            int src_a_reg = u->src_reg_a.reg;
            int src_b_reg = u->src_reg_b.reg;
            int is_unsigned_cmp = (uop_masked == (UOP_CMP_JB_DEST & UOP_MASK) ||
                                   uop_masked == (UOP_CMP_JNB_DEST & UOP_MASK) ||
                                   uop_masked == (UOP_CMP_JZ_DEST & UOP_MASK) ||
                                   uop_masked == (UOP_CMP_JNZ_DEST & UOP_MASK) ||
                                   uop_masked == (UOP_CMP_JBE_DEST & UOP_MASK) ||
                                   uop_masked == (UOP_CMP_JNBE_DEST & UOP_MASK));
            if (is_unsigned_cmp && is_subreg_int(u->src_reg_a) && is_subreg_int(u->src_reg_b)) {
                int ra = IREG_GET_REG(src_a_reg), rb = IREG_GET_REG(src_b_reg);
                int wa = subreg_width(src_a_reg), wb = subreg_width(src_b_reg);
                if (subreg_lsb(src_a_reg) == 0 && subreg_lsb(src_b_reg) == 0 &&
                    zext_width[ra] != 0 && zext_width[ra] <= wa &&
                    zext_width[rb] != 0 && zext_width[rb] <= wb) {
                    src_a_reg = ra | IREG_SIZE_L;
                    src_b_reg = rb | IREG_SIZE_L;
                }
            }
            uop_t *new_uop     = lower_copy_uop(ir, u, u->type);
            new_uop->src_reg_a = codegen_reg_read(src_a_reg);
            new_uop->src_reg_b = codegen_reg_read(src_b_reg);
            memset(zext_width, 0, sizeof(zext_width));
            continue;
        }
        if (uop_masked == (UOP_SETCC & UOP_MASK)) {
            int src_a_reg = u->src_reg_a.reg;
            int src_b_reg = u->src_reg_b.reg;
            int cond_base = u->imm_data & 0xf;
            int is_unsigned_cmp = (cond_base == COND_B || cond_base == COND_NB ||
                                   cond_base == COND_E || cond_base == COND_NE ||
                                   cond_base == COND_BE || cond_base == COND_NBE);
            if (is_unsigned_cmp && is_subreg_int(u->src_reg_a) && is_subreg_int(u->src_reg_b)) {
                int ra = IREG_GET_REG(src_a_reg), rb = IREG_GET_REG(src_b_reg);
                int wa = subreg_width(src_a_reg), wb = subreg_width(src_b_reg);
                if (subreg_lsb(src_a_reg) == 0 && subreg_lsb(src_b_reg) == 0 &&
                    zext_width[ra] != 0 && zext_width[ra] <= wa &&
                    zext_width[rb] != 0 && zext_width[rb] <= wb) {
                    src_a_reg = ra | IREG_SIZE_L;
                    src_b_reg = rb | IREG_SIZE_L;
                }
            }
            int dreg      = IREG_GET_REG(u->dest_reg_a.reg);
            int d_is_sub  = is_subreg_int(u->dest_reg_a);
            int d_lsb     = d_is_sub ? subreg_lsb(u->dest_reg_a.reg) : 0;
            int d_width   = d_is_sub ? subreg_width(u->dest_reg_a.reg) : 32;

            uop_t *new_uop     = lower_copy_uop(ir, u, u->type);
            new_uop->src_reg_a = codegen_reg_read(src_a_reg);
            new_uop->src_reg_b = codegen_reg_read(src_b_reg);
            if (d_is_sub && reg_needs_bfi(dreg)) {
                new_uop->dest_reg_a = codegen_reg_write(IREG_temp4, ir->wr_pos - 1);
                uop_BFI(ir, dreg | IREG_SIZE_L, dreg | IREG_SIZE_L, IREG_temp4, d_lsb, d_width);
                zext_width[dreg]       = 0;
                zext_width[IREG_temp4] = 8;
            } else {
                new_uop->dest_reg_a = codegen_reg_write(dreg | IREG_SIZE_L, ir->wr_pos - 1);
                if (reg_is_dword_native(dreg))
                    zext_width[dreg] = 8;
                else if (dreg < IREG_COUNT)
                    zext_width[dreg] = 0;
            }
            continue;
        }

        /* 6. General uOp lowering */
        int src_a_norm   = u->src_reg_a.reg;
        int src_b_norm   = u->src_reg_b.reg;
        int src_c_norm   = u->src_reg_c.reg;
        int src_a_signed = (uop_masked == (UOP_SAR & UOP_MASK) || uop_masked == (UOP_SAR_IMM & UOP_MASK));

        if (is_subreg_int(u->src_reg_a)) {
            src_a_norm = lower_read_subreg(ir, u->src_reg_a, src_a_signed, IREG_temp5, zext_width);
        }
        if (is_subreg_int(u->src_reg_b)) {
            src_b_norm = lower_read_subreg(ir, u->src_reg_b, 0, IREG_temp6, zext_width);
        }
        if (is_subreg_int(u->src_reg_c)) {
            if (uop_masked == (UOP_CMOV & UOP_MASK)) {
                int rc = IREG_GET_REG(u->src_reg_c.reg);
                int wc = subreg_width(u->src_reg_c.reg);
                if (subreg_lsb(u->src_reg_c.reg) == 0 && zext_width[rc] != 0 && zext_width[rc] <= wc)
                    src_c_norm = rc | IREG_SIZE_L;
            } else {
                src_c_norm = lower_read_subreg(ir, u->src_reg_c, 0, IREG_temp3, zext_width);
            }
        }

        int dest_is_sub = is_subreg_int(u->dest_reg_a);
        int dreg        = dest_is_sub ? IREG_GET_REG(u->dest_reg_a.reg) : -1;
        int d_lsb       = dest_is_sub ? subreg_lsb(u->dest_reg_a.reg) : 0;
        int d_width     = dest_is_sub ? subreg_width(u->dest_reg_a.reg) : 32;

        uop_t *new_uop = lower_copy_uop(ir, u, u->type);
        if (!ir_reg_is_invalid(u->src_reg_a))
            new_uop->src_reg_a = codegen_reg_read(src_a_norm);
        if (!ir_reg_is_invalid(u->src_reg_b))
            new_uop->src_reg_b = codegen_reg_read(src_b_norm);
        if (!ir_reg_is_invalid(u->src_reg_c))
            new_uop->src_reg_c = codegen_reg_read(src_c_norm);

        if (dest_is_sub) {
            int result_is_zext = 0;
            if (uop_masked == (UOP_MOV_IMM & UOP_MASK) ||
                uop_masked == (UOP_AND_IMM & UOP_MASK) ||
                uop_masked == (UOP_OR_IMM & UOP_MASK) ||
                uop_masked == (UOP_XOR_IMM & UOP_MASK)) {
                new_uop->imm_data &= (d_width == 8) ? 0xffu : 0xffffu;
                result_is_zext = 1;
            } else if (uop_masked == (UOP_SHR_IMM & UOP_MASK) ||
                       uop_masked == (UOP_SHR & UOP_MASK) ||
                       uop_masked == (UOP_MOVZX_REG_PTR_8 & UOP_MASK) ||
                       uop_masked == (UOP_MOVZX_REG_PTR_16 & UOP_MASK) ||
                       uop_masked == (UOP_AND & UOP_MASK) ||
                       uop_masked == (UOP_OR & UOP_MASK) ||
                       uop_masked == (UOP_XOR & UOP_MASK) ||
                       uop_masked == (UOP_MOV & UOP_MASK) ||
                       uop_masked == (UOP_CMOV & UOP_MASK)) {
                result_is_zext = 1;
            }

            if (reg_needs_bfi(dreg)) {
                new_uop->dest_reg_a = codegen_reg_write(IREG_temp4, ir->wr_pos - 1);
                uop_BFI(ir, dreg | IREG_SIZE_L, dreg | IREG_SIZE_L, IREG_temp4, d_lsb, d_width);
                zext_width[dreg]       = 0;
                zext_width[IREG_temp4] = result_is_zext ? (uint8_t) d_width : 0;
            } else {
                /* Non-GPR sub-register dest (flags_res/temps/segs): compute in a
                   32-bit temp, then truncate so upper bits never leak. */
                new_uop->dest_reg_a = codegen_reg_write(IREG_temp4, ir->wr_pos - 1);
                uop_BFX(ir, dreg | IREG_SIZE_L, IREG_temp4, 0, d_width, 0);
                if (reg_is_dword_native(dreg))
                    zext_width[dreg] = (uint8_t) d_width;
                else if (dreg < IREG_COUNT)
                    zext_width[dreg] = 0;
                zext_width[IREG_temp4] = 0;
            }
        } else if (!ir_reg_is_invalid(u->dest_reg_a)) {
            int full_dreg       = IREG_GET_REG(u->dest_reg_a.reg);
            new_uop->dest_reg_a = codegen_reg_write(u->dest_reg_a.reg, ir->wr_pos - 1);
            if (reg_is_dword_native(full_dreg)) {
                if (uop_masked == (UOP_MOV_IMM & UOP_MASK) || uop_masked == (UOP_AND_IMM & UOP_MASK)) {
                    if (u->imm_data <= 0xffu)
                        zext_width[full_dreg] = 8;
                    else if (u->imm_data <= 0xffffu)
                        zext_width[full_dreg] = 16;
                    else
                        zext_width[full_dreg] = 0;
                } else if (uop_masked == (UOP_MOVZX_REG_PTR_8 & UOP_MASK)) {
                    zext_width[full_dreg] = 8;
                } else if (uop_masked == (UOP_MOVZX_REG_PTR_16 & UOP_MASK)) {
                    zext_width[full_dreg] = 16;
                } else if (uop_masked == (UOP_MOV & UOP_MASK) && !ir_reg_is_invalid(new_uop->src_reg_a)) {
                    zext_width[full_dreg] = zext_width[IREG_GET_REG(new_uop->src_reg_a.reg)];
                } else {
                    zext_width[full_dreg] = 0;
                }
            } else if (full_dreg < IREG_COUNT) {
                zext_width[full_dreg] = 0;
            }
        }

        if (u->type & (UOP_TYPE_BARRIER | UOP_TYPE_ORDER_BARRIER | UOP_TYPE_JUMP)) {
            if (!uop_is_mem_access(u->type))
                memset(zext_width, 0, sizeof(zext_width));
        }
    }

    uop_map_buf[orig_count] = ir->wr_pos;
    if (codegen_unroll_count && codegen_unroll_start >= 0 && codegen_unroll_start <= orig_count) {
        codegen_unroll_start = uop_map_buf[codegen_unroll_start];
    }
    for (int c = 0; c < ir->wr_pos; c++) {
        if (ir->uops[c].jump_dest_uop >= 0 && ir->uops[c].jump_dest_uop <= orig_count) {
            ir->uops[c].jump_dest_uop = uop_map_buf[ir->uops[c].jump_dest_uop];
        }
        if ((ir->uops[c].type & UOP_MASK) == (UOP_LOOP_JMP & UOP_MASK) && ir->uops[c].p != NULL) {
            uintptr_t p_val     = (uintptr_t) ir->uops[c].p;
            uintptr_t base_ir   = (uintptr_t) ir->uops;
            uintptr_t base_orig = (uintptr_t) orig_uops_buf;
            int       old_idx   = -1;
            if (p_val >= base_ir && p_val < base_ir + orig_count * sizeof(uop_t))
                old_idx = (p_val - base_ir) / sizeof(uop_t);
            else if (p_val >= base_orig && p_val < base_orig + orig_count * sizeof(uop_t))
                old_idx = (p_val - base_orig) / sizeof(uop_t);
            if (old_idx >= 0 && old_idx < orig_count) {
                int new_idx = uop_map_buf[old_idx];
                ir->uops[c].p = &ir->uops[new_idx];
            }
        }
    }

    ir->cur_guest_insn_idx = saved_insn_idx;
    ir->cur_guest_pc_off   = saved_pc_off;
}

static int
block_has_lahf_sahf(ir_data_t *ir)
{
    for (int i = 0; i < ir->wr_pos; i++) {
        uop_t   *u = &ir->uops[i];
        uint32_t t = u->type & UOP_MASK;
        if (t == (UOP_AND_IMM & UOP_MASK)) {
            int d = IREG_GET_REG(u->dest_reg_a.reg);
            if (d == IREG_EAX && u->imm_data == 0xffff00ff)
                return 1;   /* LAHF: AND EAX, 0xffff00ff */
            if (d == IREG_flagsx && u->imm_data == 0xff00)
                return 1;   /* SAHF: AND FLAGS, 0xff00 */
        }
    }
    return 0;
}

void
codegen_ir_compile(ir_data_t *ir, codeblock_t *block)
{
    block->unroll_count = (uint8_t) codegen_unroll_count;
    static int opt_bfx_initialized = 0;
    static int opt_bfx_limit = -1;
    static int opt_bfx_count = 0;
    if (!opt_bfx_initialized) {
        opt_bfx_initialized = 1;
        const char *env = getenv("BOX86_OPT_BFX");
        if (env)
            codegen_opt_bfx = atoi(env);
        const char *lim = getenv("BOX86_BFX_LIMIT");
        if (lim)
            opt_bfx_limit = atoi(lim);
    }
    if (codegen_opt_bfx && (opt_bfx_limit < 0 || opt_bfx_count < opt_bfx_limit)) {
        opt_bfx_count++;
        /* Blocks containing LAHF/SAHF keep the known-good unlowered flag path
           until the 16-bit FLAGS sub-register lowering is correct. */
        if (getenv("BOX86_BFX_FORCE_LAHF") || !block_has_lahf_sahf(ir))
            codegen_ir_lower_subregs(ir);
    }

    int jump_target_at_end = -1;
    int c;

    if (codegen_unroll_count) {
        int unroll_end;

        codegen_set_loop_start(ir, codegen_unroll_first_instruction);
        unroll_end = ir->wr_pos;

        for (int unroll_count = 1; unroll_count < codegen_unroll_count; unroll_count++) {
            int offset = ir->wr_pos - codegen_unroll_start;
            //                        pclog("Unroll from %i to %i, offset %i - iteration %i\n", codegen_unroll_start, ir->wr_pos, offset, unroll_count);
            for (c = codegen_unroll_start; c < unroll_end; c++) {
                //                                pclog(" Duplicate uop %i\n", c);
                duplicate_uop(ir, &ir->uops[c], offset);
            }
        }
    }

    int dump_on = codegen_dump_is_active();
    if (dump_on) {
        for (c = 0; c < ir->wr_pos; c++) {
            dump_orig_types[c]       = ir->uops[c].type;
            dump_orig_imms[c]        = (uint32_t) ir->uops[c].imm_data;
            dump_host_starts[c]      = 0;
            dump_host_body_starts[c] = 0;
            dump_host_ends[c]        = 0;
        }
    }

    for (c = 0; c < ir->wr_pos; c++) {
        if ((ir->uops[c].type & UOP_TYPE_JUMP) && ir->uops[c].jump_dest_uop >= 0 && ir->uops[c].jump_dest_uop < ir->wr_pos) {
            ir->uops[ir->uops[c].jump_dest_uop].type |= UOP_TYPE_JUMP_DEST;
        }
    }

    codegen_reg_mark_as_required();
    codegen_ir_peephole(ir);
    codegen_ir_optimize_liveness(ir);
    codegen_reg_process_dead_list(ir);
    codegen_dump_begin_block();
    block_write_data = codeblock_allocator_get_ptr(block->head_mem_block);
    block_pos        = 0;
    codegen_backend_prologue(block);
    uint16_t prologue_end_off = codegen_dump_get_pos(block_pos);

    for (c = 0; c < ir->wr_pos; c++) {
        uop_t *uop = &ir->uops[c];

        if (dump_on) {
            uint16_t cur_pos         = codegen_dump_get_pos(block_pos);
            dump_host_starts[c]      = cur_pos;
            dump_host_body_starts[c] = cur_pos;
            dump_host_ends[c]        = cur_pos;
        }

        //                pclog("uOP %i : %08x\n", c, uop->type);

        if (uop->type & UOP_TYPE_BARRIER)
            codegen_reg_flush_invalidate(ir, block);

        if ((uop->type & UOP_MASK) == (UOP_LOOP_LABEL & UOP_MASK))
            uop->jump_dest = &block_write_data[block_pos];

        if (uop->type & UOP_TYPE_JUMP_DEST) {
            codegen_reg_flush_invalidate(ir, block);
            uop_t *uop_dest = uop;

            while (uop_dest->jump_list_next != -1) {
                uop_dest = &ir->uops[uop_dest->jump_list_next];
                codegen_set_jump_dest(block, uop_dest->p);
            }
        }

        if ((uop->type & UOP_MASK) == UOP_INVALID)
            continue;

#ifdef CODEGEN_BACKEND_HAS_MOV_IMM
        if ((uop->type & UOP_MASK) == (UOP_MOV_IMM & UOP_MASK) && reg_is_integer(uop->dest_reg_a) && reg_is_native_size(uop->dest_reg_a) && !codegen_reg_is_loaded(uop->dest_reg_a) && reg_version[IREG_GET_REG(uop->dest_reg_a.reg)][uop->dest_reg_a.version].refcount <= 0) {
            /*Special case for UOP_MOV_IMM - if destination not already in host register
              and won't be used again then just store directly to memory*/
            if (dump_on)
                dump_host_body_starts[c] = codegen_dump_get_pos(block_pos);
            codegen_reg_write_imm(block, uop->dest_reg_a, uop->imm_data);
            if (dump_on)
                dump_host_ends[c] = codegen_dump_get_pos(block_pos);
        } else
#endif
            if ((uop->type & UOP_MASK) == (UOP_MOV & UOP_MASK) && reg_version[IREG_GET_REG(uop->src_reg_a.reg)][uop->src_reg_a.version].refcount <= 1 && reg_is_native_size(uop->src_reg_a) && reg_is_native_size(uop->dest_reg_a)) {
            /*Special case for UOP_MOV - if source register won't be used again then
              just rename it to dest register instead of moving*/
            codegen_reg_alloc_register(invalid_ir_reg, uop->src_reg_a, invalid_ir_reg, invalid_ir_reg);
            uop->src_reg_a_real = codegen_reg_alloc_read_reg(block, uop->src_reg_a, NULL);
            codegen_reg_rename(block, uop->src_reg_a, uop->dest_reg_a);
            if (uop->type & UOP_TYPE_ORDER_BARRIER)
                codegen_reg_flush(ir, block);
            if (dump_on) {
                uint16_t cur_pos         = codegen_dump_get_pos(block_pos);
                dump_host_body_starts[c] = cur_pos;
                dump_host_ends[c]        = cur_pos;
            }
        } else {
            if (uop->type & UOP_TYPE_PARAMS_REGS) {
                codegen_reg_alloc_register(uop->dest_reg_a, uop->src_reg_a, uop->src_reg_b, uop->src_reg_c);
                if (uop->src_reg_a.reg != IREG_INVALID) {
                    uop->src_reg_a_real = codegen_reg_alloc_read_reg(block, uop->src_reg_a, NULL);
                }
                if (uop->src_reg_b.reg != IREG_INVALID) {
                    uop->src_reg_b_real = codegen_reg_alloc_read_reg(block, uop->src_reg_b, NULL);
                }
                if (uop->src_reg_c.reg != IREG_INVALID) {
                    uop->src_reg_c_real = codegen_reg_alloc_read_reg(block, uop->src_reg_c, NULL);
                }
            }

            if (uop->type & UOP_TYPE_ORDER_BARRIER)
                codegen_reg_flush(ir, block);

            if (uop->type & UOP_TYPE_PARAMS_REGS) {
                if (uop->dest_reg_a.reg != IREG_INVALID) {
                    uop->dest_reg_a_real = codegen_reg_alloc_write_reg(block, uop->dest_reg_a);
                }
            }
#ifndef RELEASE_BUILD
            if (!uop_handlers[uop->type & UOP_MASK])
                fatal("!uop_handlers[uop->type & UOP_MASK] %08x\n", uop->type);
#endif
            if (dump_on)
                dump_host_body_starts[c] = codegen_dump_get_pos(block_pos);
            uop_handlers[uop->type & UOP_MASK](block, uop);
            if (dump_on)
                dump_host_ends[c] = codegen_dump_get_pos(block_pos);
        }

        if (uop->type & UOP_TYPE_JUMP) {
            if (uop->jump_dest_uop == ir->wr_pos) {
                if (jump_target_at_end == -1)
                    jump_target_at_end = c;
                else {
                    uop_t *uop_dest = &ir->uops[jump_target_at_end];

                    while (uop_dest->jump_list_next != -1)
                        uop_dest = &ir->uops[uop_dest->jump_list_next];

                    uop_dest->jump_list_next = c;
                }
            } else {
                uop_t *uop_dest = &ir->uops[uop->jump_dest_uop];

                while (uop_dest->jump_list_next != -1)
                    uop_dest = &ir->uops[uop_dest->jump_list_next];

                uop_dest->jump_list_next = c;
                ir->uops[uop->jump_dest_uop].type |= UOP_TYPE_JUMP_DEST;
            }
        }
    }

    codegen_reg_flush_invalidate(ir, block);

    if (jump_target_at_end != -1) {
        uop_t *uop_dest = &ir->uops[jump_target_at_end];

        while (1) {
            codegen_set_jump_dest(block, uop_dest->p);
            if (uop_dest->jump_list_next == -1)
                break;
            uop_dest = &ir->uops[uop_dest->jump_list_next];
        }
    }

    uint16_t epilogue_pos_off = codegen_dump_get_pos(block_pos);
    codegen_backend_epilogue(block);
    codegen_dump_record_chunk(block_write_data, (uint32_t) block_pos);

    if (dump_on) {
        codegen_dump_compile_block(block, ir, prologue_end_off, epilogue_pos_off,
                                   dump_orig_types, dump_orig_imms,
                                   dump_host_starts, dump_host_body_starts, dump_host_ends);
    }
    if (codegen_dump_perf_map_active()) {
        codegen_dump_perf_map_block(block);
    }

    block_write_data = NULL;
#if 0
    if (has_ea)
        fatal("IR compilation complete\n");
#endif
}

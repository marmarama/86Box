/*
 * codegen_ops_jit_wrappers.h — noinline wrappers for JIT function pointers
 *
 * GCC on ARM64 may merge or eliminate out-of-line copies of static __inline
 * functions. When the address of such a function is taken and stored as a
 * JIT uop operand (via uop_CALL_FUNC / uop_CALL_FUNC_RESULT), the emitted
 * code ends up calling a merged symbol that may point to the wrong function
 * or may be eliminated entirely.
 *
 * Every static __inline function whose address is passed to uop_CALL_FUNC*
 * MUST have an noinline wrapper here.  The wrapper is what the JIT calls at
 * runtime — the real inline body is still available for normal C call sites.
 *
 * Include this header in each codegen_ops_*.c that needs one of the wrappers.
 * The including .c file MUST already include x86_flags.h before this header.
 */
#ifndef CODEGEN_OPS_JIT_WRAPPERS_H
#define CODEGEN_OPS_JIT_WRAPPERS_H

#include <86box/io.h>
#include "386_common.h"
#include "codegen_dump.h"

#define JIT_WRAPPER __attribute__((noinline, used))

/* --- flags_rebuild family (void → void) --- */

static JIT_WRAPPER void
jit_update_tsc(void)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        update_tsc();
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
    } else {
        update_tsc();
    }
}

static JIT_WRAPPER void
jit_cli_slow(void)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        x86_dynarec_opcodes[0xfa](0);
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
    } else {
        x86_dynarec_opcodes[0xfa](0);
    }
}

static JIT_WRAPPER void
jit_sti_slow(void)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        x86_dynarec_opcodes[0xfb](0);
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
    } else {
        x86_dynarec_opcodes[0xfb](0);
    }
}

static JIT_WRAPPER void
jit_popf(uint32_t val)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        uint16_t tempw = (uint16_t) val;
        if (!(CPL) || !(msw & 1))
            cpu_state.flags = (tempw & 0x7fd5) | 2;
        else if (IOPLp)
            cpu_state.flags = (cpu_state.flags & 0x3000) | (tempw & 0x4fd5) | 2;
        else
            cpu_state.flags = (cpu_state.flags & 0x3200) | (tempw & 0x4dd5) | 2;
        cpu_state.flags_op = FLAGS_UNKNOWN;
#ifdef USE_DEBUG_REGS_486
        rf_flag_no_clear = 1;
#endif
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
    } else {
        uint16_t tempw = (uint16_t) val;
        if (!(CPL) || !(msw & 1))
            cpu_state.flags = (tempw & 0x7fd5) | 2;
        else if (IOPLp)
            cpu_state.flags = (cpu_state.flags & 0x3000) | (tempw & 0x4fd5) | 2;
        else
            cpu_state.flags = (cpu_state.flags & 0x3200) | (tempw & 0x4dd5) | 2;
        cpu_state.flags_op = FLAGS_UNKNOWN;
#ifdef USE_DEBUG_REGS_486
        rf_flag_no_clear = 1;
#endif
    }
}

static JIT_WRAPPER void
jit_popfd(uint32_t templ)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        if (!(CPL) || !(msw & 1))
            cpu_state.flags = (templ & 0x7fd5) | 2;
        else if (IOPLp)
            cpu_state.flags = (cpu_state.flags & 0x3000) | (templ & 0x4fd5) | 2;
        else
            cpu_state.flags = (cpu_state.flags & 0x3200) | (templ & 0x4dd5) | 2;

        templ &= (is486 || isibm486) ? 0x3c0000 : 0;
        templ |= ((cpu_state.eflags & 3) << 16);
        if (cpu_CR4_mask & CR4_VME)
            cpu_state.eflags = (templ >> 16) & 0x3f;
        else if (CPUID)
            cpu_state.eflags = (templ >> 16) & 0x27;
        else if (is486 || isibm486)
            cpu_state.eflags = (templ >> 16) & 7;
        else
            cpu_state.eflags = (templ >> 16) & 3;

        cpu_state.flags_op = FLAGS_UNKNOWN;
#ifdef USE_DEBUG_REGS_486
        rf_flag_no_clear = 1;
#endif
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
    } else {
        if (!(CPL) || !(msw & 1))
            cpu_state.flags = (templ & 0x7fd5) | 2;
        else if (IOPLp)
            cpu_state.flags = (cpu_state.flags & 0x3000) | (templ & 0x4fd5) | 2;
        else
            cpu_state.flags = (cpu_state.flags & 0x3200) | (templ & 0x4dd5) | 2;

        templ &= (is486 || isibm486) ? 0x3c0000 : 0;
        templ |= ((cpu_state.eflags & 3) << 16);
        if (cpu_CR4_mask & CR4_VME)
            cpu_state.eflags = (templ >> 16) & 0x3f;
        else if (CPUID)
            cpu_state.eflags = (templ >> 16) & 0x27;
        else if (is486 || isibm486)
            cpu_state.eflags = (templ >> 16) & 7;
        else
            cpu_state.eflags = (templ >> 16) & 3;

        cpu_state.flags_op = FLAGS_UNKNOWN;
#ifdef USE_DEBUG_REGS_486
        rf_flag_no_clear = 1;
#endif
    }
}

static JIT_WRAPPER void
jit_verr(uint32_t sel_val)
{
    uint16_t sel = (uint16_t) sel_val;
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        flags_rebuild();
        cpu_state.flags &= ~Z_FLAG;
        if ((cr0 & 1) && (sel & 0xfffc)) {
            cpl_override = 1;
            int valid = (sel & ~7) < ((sel & 4) ? ldt.limit : gdt.limit);
            uint16_t desc = readmemw(0, ((sel & 4) ? ldt.base : gdt.base) + (sel & ~7) + 4);
            cpl_override = 0;
            if (!cpu_state.abrt) {
                if (!(desc & 0x1000))
                    valid = 0;
                if ((desc & 0xC00) != 0xC00) {
                    int dpl = (desc >> 13) & 3;
                    if (dpl < CPL || dpl < (sel & 3))
                        valid = 0;
                }
                if ((desc & 0x0800) && !(desc & 0x0200))
                    valid = 0;
                if (valid)
                    cpu_state.flags |= Z_FLAG;
            }
        }
        cpu_state.flags_op = FLAGS_UNKNOWN;
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
    } else {
        flags_rebuild();
        cpu_state.flags &= ~Z_FLAG;
        if ((cr0 & 1) && (sel & 0xfffc)) {
            cpl_override = 1;
            int valid = (sel & ~7) < ((sel & 4) ? ldt.limit : gdt.limit);
            uint16_t desc = readmemw(0, ((sel & 4) ? ldt.base : gdt.base) + (sel & ~7) + 4);
            cpl_override = 0;
            if (!cpu_state.abrt) {
                if (!(desc & 0x1000))
                    valid = 0;
                if ((desc & 0xC00) != 0xC00) {
                    int dpl = (desc >> 13) & 3;
                    if (dpl < CPL || dpl < (sel & 3))
                        valid = 0;
                }
                if ((desc & 0x0800) && !(desc & 0x0200))
                    valid = 0;
                if (valid)
                    cpu_state.flags |= Z_FLAG;
            }
        }
        cpu_state.flags_op = FLAGS_UNKNOWN;
    }
}

static JIT_WRAPPER void
jit_verw(uint32_t sel_val)
{
    uint16_t sel = (uint16_t) sel_val;
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        flags_rebuild();
        cpu_state.flags &= ~Z_FLAG;
        if ((cr0 & 1) && (sel & 0xfffc)) {
            cpl_override = 1;
            int valid = (sel & ~7) < ((sel & 4) ? ldt.limit : gdt.limit);
            uint16_t desc = readmemw(0, ((sel & 4) ? ldt.base : gdt.base) + (sel & ~7) + 4);
            cpl_override = 0;
            if (!cpu_state.abrt) {
                if (!(desc & 0x1000))
                    valid = 0;
                int dpl = (desc >> 13) & 3;
                if (dpl < CPL || dpl < (sel & 3))
                    valid = 0;
                if (desc & 0x0800)
                    valid = 0;
                if (!(desc & 0x0200))
                    valid = 0;
                if (valid)
                    cpu_state.flags |= Z_FLAG;
            }
        }
        cpu_state.flags_op = FLAGS_UNKNOWN;
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
    } else {
        flags_rebuild();
        cpu_state.flags &= ~Z_FLAG;
        if ((cr0 & 1) && (sel & 0xfffc)) {
            cpl_override = 1;
            int valid = (sel & ~7) < ((sel & 4) ? ldt.limit : gdt.limit);
            uint16_t desc = readmemw(0, ((sel & 4) ? ldt.base : gdt.base) + (sel & ~7) + 4);
            cpl_override = 0;
            if (!cpu_state.abrt) {
                if (!(desc & 0x1000))
                    valid = 0;
                int dpl = (desc >> 13) & 3;
                if (dpl < CPL || dpl < (sel & 3))
                    valid = 0;
                if (desc & 0x0800)
                    valid = 0;
                if (!(desc & 0x0200))
                    valid = 0;
                if (valid)
                    cpu_state.flags |= Z_FLAG;
            }
        }
        cpu_state.flags_op = FLAGS_UNKNOWN;
    }
}

static JIT_WRAPPER void
jit_iret(void)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        x86_dynarec_opcodes[0xcf](0);
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
    } else {
        x86_dynarec_opcodes[0xcf](0);
    }
}

static JIT_WRAPPER void
jit_iretd(void)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        x86_dynarec_opcodes[0xcf | 0x100](0);
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
    } else {
        x86_dynarec_opcodes[0xcf | 0x100](0);
    }
}

static JIT_WRAPPER void
jit_retf16(void)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        op_pmoderetf(0, 0);
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
    } else {
        op_pmoderetf(0, 0);
    }
}

static JIT_WRAPPER void
jit_retf32(void)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        op_pmoderetf(1, 0);
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
    } else {
        op_pmoderetf(1, 0);
    }
}

static JIT_WRAPPER void
jit_retf16_imm(uint32_t offset)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        op_pmoderetf(0, (uint16_t) offset);
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
    } else {
        op_pmoderetf(0, (uint16_t) offset);
    }
}

static JIT_WRAPPER void
jit_retf32_imm(uint32_t offset)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        op_pmoderetf(1, (uint16_t) offset);
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
    } else {
        op_pmoderetf(1, (uint16_t) offset);
    }
}

static JIT_WRAPPER uint32_t
jit_smsw16(void)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        uint32_t val;
        if (is486 || isibm486)
            val = msw;
        else if (is386 && !cpu_16bitbus)
            val = msw | 0xFFE0;
        else
            val = msw | 0xFFF0;
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
        return val;
    } else {
        if (is486 || isibm486)
            return msw;
        else if (is386 && !cpu_16bitbus)
            return msw | 0xFFE0;
        else
            return msw | 0xFFF0;
    }
}

static JIT_WRAPPER uint32_t
jit_smsw32(void)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        uint32_t val;
        if (is486 || isibm486)
            val = cr0;
        else if (is386 && !cpu_16bitbus)
            val = cr0 | 0x7FFFFFE0;
        else
            val = cr0 | 0x7FFFFFF0;
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
        return val;
    } else {
        if (is486 || isibm486)
            return cr0;
        else if (is386 && !cpu_16bitbus)
            return cr0 | 0x7FFFFFE0;
        else
            return cr0 | 0x7FFFFFF0;
    }
}

static JIT_WRAPPER uint32_t
jit_inb(uint32_t port)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        check_io_perm(port, 1);
        if (cpu_state.abrt) {
            codegen_profile_record_helper_tsc(host_rdtsc() - t0);
            return 1;
        }
        AL = inb((uint16_t) port);
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
        if (cpu_state.exit_request || (nmi && nmi_enable && nmi_mask))
            return 1;
        return 0;
    } else {
        check_io_perm(port, 1);
        if (cpu_state.abrt)
            return 1;
        AL = inb((uint16_t) port);
        if (cpu_state.exit_request || (nmi && nmi_enable && nmi_mask))
            return 1;
        return 0;
    }
}

static JIT_WRAPPER uint32_t
jit_inw(uint32_t port)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        check_io_perm(port, 2);
        if (cpu_state.abrt) {
            codegen_profile_record_helper_tsc(host_rdtsc() - t0);
            return 1;
        }
        AX = inw((uint16_t) port);
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
        if (cpu_state.exit_request || (nmi && nmi_enable && nmi_mask))
            return 1;
        return 0;
    } else {
        check_io_perm(port, 2);
        if (cpu_state.abrt)
            return 1;
        AX = inw((uint16_t) port);
        if (cpu_state.exit_request || (nmi && nmi_enable && nmi_mask))
            return 1;
        return 0;
    }
}

static JIT_WRAPPER uint32_t
jit_inl(uint32_t port)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        check_io_perm(port, 4);
        if (cpu_state.abrt) {
            codegen_profile_record_helper_tsc(host_rdtsc() - t0);
            return 1;
        }
        EAX = inl((uint16_t) port);
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
        if (cpu_state.exit_request || (nmi && nmi_enable && nmi_mask))
            return 1;
        return 0;
    } else {
        check_io_perm(port, 4);
        if (cpu_state.abrt)
            return 1;
        EAX = inl((uint16_t) port);
        if (cpu_state.exit_request || (nmi && nmi_enable && nmi_mask))
            return 1;
        return 0;
    }
}

static JIT_WRAPPER uint32_t
jit_outb(uint32_t port)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        check_io_perm(port, 1);
        if (cpu_state.abrt) {
            codegen_profile_record_helper_tsc(host_rdtsc() - t0);
            return 1;
        }
        outb((uint16_t) port, AL);
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
        if (x86_was_reset || cpu_state.exit_request || (nmi && nmi_enable && nmi_mask))
            return 1;
        return 0;
    } else {
        check_io_perm(port, 1);
        if (cpu_state.abrt)
            return 1;
        outb((uint16_t) port, AL);
        if (x86_was_reset || cpu_state.exit_request || (nmi && nmi_enable && nmi_mask))
            return 1;
        return 0;
    }
}

static JIT_WRAPPER uint32_t
jit_outw(uint32_t port)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        check_io_perm(port, 2);
        if (cpu_state.abrt) {
            codegen_profile_record_helper_tsc(host_rdtsc() - t0);
            return 1;
        }
        outw((uint16_t) port, AX);
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
        if (x86_was_reset || cpu_state.exit_request || (nmi && nmi_enable && nmi_mask))
            return 1;
        return 0;
    } else {
        check_io_perm(port, 2);
        if (cpu_state.abrt)
            return 1;
        outw((uint16_t) port, AX);
        if (x86_was_reset || cpu_state.exit_request || (nmi && nmi_enable && nmi_mask))
            return 1;
        return 0;
    }
}

static JIT_WRAPPER uint32_t
jit_outl(uint32_t port)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        check_io_perm(port, 4);
        if (cpu_state.abrt) {
            codegen_profile_record_helper_tsc(host_rdtsc() - t0);
            return 1;
        }
        outl((uint16_t) port, EAX);
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
        if (x86_was_reset || cpu_state.exit_request || (nmi && nmi_enable && nmi_mask))
            return 1;
        return 0;
    } else {
        check_io_perm(port, 4);
        if (cpu_state.abrt)
            return 1;
        outl((uint16_t) port, EAX);
        if (x86_was_reset || cpu_state.exit_request || (nmi && nmi_enable && nmi_mask))
            return 1;
        return 0;
    }
}

static JIT_WRAPPER void
jit_flags_rebuild(void)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        flags_rebuild();
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
    } else {
        flags_rebuild();
    }
}

static JIT_WRAPPER void
jit_flags_rebuild_c(void)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        flags_rebuild_c();
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
    } else {
        flags_rebuild_c();
    }
}

static JIT_WRAPPER void
jit_flags_rebuild_zpna(void)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        flags_rebuild_zpna();
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
    } else {
        flags_rebuild_zpna();
    }
}

#define JIT_PROFILE_CALL_RET(type, expr)                  \
    do {                                                  \
        if (codegen_profile_active) {                     \
            uint64_t t0  = host_rdtsc();                  \
            type     res = (expr);                        \
            codegen_profile_record_helper_tsc(host_rdtsc() - t0); \
            return res;                                   \
        }                                                 \
        return (expr);                                    \
    } while (0)

/* --- individual flag test functions (void → int) --- */

static JIT_WRAPPER int
jit_CF_SET(void)
{
    JIT_PROFILE_CALL_RET(int, CF_SET());
}

static JIT_WRAPPER int
jit_NF_SET(void)
{
    JIT_PROFILE_CALL_RET(int, NF_SET());
}

static JIT_WRAPPER int
jit_PF_SET(void)
{
    JIT_PROFILE_CALL_RET(int, PF_SET());
}

static JIT_WRAPPER int
jit_VF_SET(void)
{
    JIT_PROFILE_CALL_RET(int, VF_SET());
}

static JIT_WRAPPER int
jit_ZF_SET(void)
{
    JIT_PROFILE_CALL_RET(int, ZF_SET());
}

/* --- condition test functions (void → int) --- */

static JIT_WRAPPER int
jit_cond_O(void)
{
    JIT_PROFILE_CALL_RET(int, VF_SET() ? 1 : 0);
}
static JIT_WRAPPER int
jit_cond_NO(void)
{
    JIT_PROFILE_CALL_RET(int, !VF_SET() ? 1 : 0);
}
static JIT_WRAPPER int
jit_cond_B(void)
{
    JIT_PROFILE_CALL_RET(int, CF_SET() ? 1 : 0);
}
static JIT_WRAPPER int
jit_cond_NB(void)
{
    JIT_PROFILE_CALL_RET(int, !CF_SET() ? 1 : 0);
}
static JIT_WRAPPER int
jit_cond_E(void)
{
    JIT_PROFILE_CALL_RET(int, ZF_SET() ? 1 : 0);
}
static JIT_WRAPPER int
jit_cond_NE(void)
{
    JIT_PROFILE_CALL_RET(int, !ZF_SET() ? 1 : 0);
}
static JIT_WRAPPER int
jit_cond_BE(void)
{
    JIT_PROFILE_CALL_RET(int, (CF_SET() || ZF_SET()) ? 1 : 0);
}
static JIT_WRAPPER int
jit_cond_NBE(void)
{
    JIT_PROFILE_CALL_RET(int, (!CF_SET() && !ZF_SET()) ? 1 : 0);
}
static JIT_WRAPPER int
jit_cond_S(void)
{
    JIT_PROFILE_CALL_RET(int, NF_SET() ? 1 : 0);
}
static JIT_WRAPPER int
jit_cond_NS(void)
{
    JIT_PROFILE_CALL_RET(int, !NF_SET() ? 1 : 0);
}
static JIT_WRAPPER int
jit_cond_P(void)
{
    JIT_PROFILE_CALL_RET(int, PF_SET() ? 1 : 0);
}
static JIT_WRAPPER int
jit_cond_NP(void)
{
    JIT_PROFILE_CALL_RET(int, !PF_SET() ? 1 : 0);
}
static JIT_WRAPPER int
jit_cond_L(void)
{
    JIT_PROFILE_CALL_RET(int, ((NF_SET() ? 1 : 0) != (VF_SET() ? 1 : 0)) ? 1 : 0);
}
static JIT_WRAPPER int
jit_cond_NL(void)
{
    JIT_PROFILE_CALL_RET(int, ((NF_SET() ? 1 : 0) == (VF_SET() ? 1 : 0)) ? 1 : 0);
}
static JIT_WRAPPER int
jit_cond_LE(void)
{
    JIT_PROFILE_CALL_RET(int, (((NF_SET() ? 1 : 0) != (VF_SET() ? 1 : 0)) || ZF_SET()) ? 1 : 0);
}
static JIT_WRAPPER int
jit_cond_NLE(void)
{
    JIT_PROFILE_CALL_RET(int, (((NF_SET() ? 1 : 0) == (VF_SET() ? 1 : 0)) && !ZF_SET()) ? 1 : 0);
}

/* --- bit scan helpers --- */

static JIT_WRAPPER uint32_t
jit_bsf(void)
{
    JIT_PROFILE_CALL_RET(uint32_t, (uint32_t) __builtin_ctz(cpu_state.flags_res));
}

static JIT_WRAPPER uint32_t
jit_bsr(void)
{
    JIT_PROFILE_CALL_RET(uint32_t, (uint32_t) (31 - __builtin_clz(cpu_state.flags_res)));
}

/* --- MUL / IMUL / DIV / IDIV helpers --- */

static JIT_WRAPPER void
jit_mul8(uint32_t dst32)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        uint8_t dst = (uint8_t) dst32;
        uint16_t tempw = (uint16_t) AL * (uint16_t) dst;
        AX = tempw;
        flags_rebuild();
        if (AH)
            cpu_state.flags |= (C_FLAG | V_FLAG);
        else
            cpu_state.flags &= ~(C_FLAG | V_FLAG);
        cpu_state.flags_op = FLAGS_UNKNOWN;
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
    } else {
        uint8_t dst = (uint8_t) dst32;
        uint16_t tempw = (uint16_t) AL * (uint16_t) dst;
        AX = tempw;
        flags_rebuild();
        if (AH)
            cpu_state.flags |= (C_FLAG | V_FLAG);
        else
            cpu_state.flags &= ~(C_FLAG | V_FLAG);
        cpu_state.flags_op = FLAGS_UNKNOWN;
    }
}

static JIT_WRAPPER void
jit_imul8(uint32_t dst32)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        int8_t dst = (int8_t) dst32;
        int16_t tempws = (int16_t) ((int8_t) AL) * (int16_t) dst;
        AX = (uint16_t) tempws;
        flags_rebuild();
        if (((int16_t) AX >> 7) != 0 && ((int16_t) AX >> 7) != -1)
            cpu_state.flags |= (C_FLAG | V_FLAG);
        else
            cpu_state.flags &= ~(C_FLAG | V_FLAG);
        cpu_state.flags_op = FLAGS_UNKNOWN;
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
    } else {
        int8_t dst = (int8_t) dst32;
        int16_t tempws = (int16_t) ((int8_t) AL) * (int16_t) dst;
        AX = (uint16_t) tempws;
        flags_rebuild();
        if (((int16_t) AX >> 7) != 0 && ((int16_t) AX >> 7) != -1)
            cpu_state.flags |= (C_FLAG | V_FLAG);
        else
            cpu_state.flags &= ~(C_FLAG | V_FLAG);
        cpu_state.flags_op = FLAGS_UNKNOWN;
    }
}

static JIT_WRAPPER int
jit_div8(uint32_t dst32)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        uint8_t dst = (uint8_t) dst32;
        if (dst == 0) {
            x86de(NULL, 0);
            codegen_profile_record_helper_tsc(host_rdtsc() - t0);
            return 1;
        }
        uint16_t src16 = AX;
        uint16_t tempw = src16 / dst;
        if (tempw & 0xff00) {
            x86de(NULL, 0);
            codegen_profile_record_helper_tsc(host_rdtsc() - t0);
            return 1;
        }
        AH = src16 % dst;
        AL = (uint8_t) tempw;
        if (!cpu_iscyrix && !is6117) {
            flags_rebuild();
            cpu_state.flags |= 0x8D5;
            cpu_state.flags &= ~1;
        }
        cpu_state.flags_op = FLAGS_UNKNOWN;
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
        return 0;
    } else {
        uint8_t dst = (uint8_t) dst32;
        if (dst == 0) {
            x86de(NULL, 0);
            return 1;
        }
        uint16_t src16 = AX;
        uint16_t tempw = src16 / dst;
        if (tempw & 0xff00) {
            x86de(NULL, 0);
            return 1;
        }
        AH = src16 % dst;
        AL = (uint8_t) tempw;
        if (!cpu_iscyrix && !is6117) {
            flags_rebuild();
            cpu_state.flags |= 0x8D5;
            cpu_state.flags &= ~1;
        }
        cpu_state.flags_op = FLAGS_UNKNOWN;
        return 0;
    }
}

static JIT_WRAPPER int
jit_idiv8(uint32_t dst32)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        int8_t dst = (int8_t) dst32;
        if (dst == 0) {
            x86de(NULL, 0);
            codegen_profile_record_helper_tsc(host_rdtsc() - t0);
            return 1;
        }
        int16_t tempws = (int16_t) AX;
        int16_t tempw = tempws / dst;
        if (tempw < -128 || tempw > 127) {
            x86de(NULL, 0);
            codegen_profile_record_helper_tsc(host_rdtsc() - t0);
            return 1;
        }
        AH = (uint8_t) (tempws % dst);
        AL = (uint8_t) tempw;
        if (!cpu_iscyrix && !is6117) {
            flags_rebuild();
            cpu_state.flags |= 0x8D5;
            cpu_state.flags &= ~1;
        }
        cpu_state.flags_op = FLAGS_UNKNOWN;
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
        return 0;
    } else {
        int8_t dst = (int8_t) dst32;
        if (dst == 0) {
            x86de(NULL, 0);
            return 1;
        }
        int16_t tempws = (int16_t) AX;
        int16_t tempw = tempws / dst;
        if (tempw < -128 || tempw > 127) {
            x86de(NULL, 0);
            return 1;
        }
        AH = (uint8_t) (tempws % dst);
        AL = (uint8_t) tempw;
        if (!cpu_iscyrix && !is6117) {
            flags_rebuild();
            cpu_state.flags |= 0x8D5;
            cpu_state.flags &= ~1;
        }
        cpu_state.flags_op = FLAGS_UNKNOWN;
        return 0;
    }
}

static JIT_WRAPPER void
jit_mul16(uint32_t dst32)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        uint16_t dst = (uint16_t) dst32;
        uint32_t templ = (uint32_t) AX * (uint32_t) dst;
        AX = templ & 0xffff;
        DX = templ >> 16;
        flags_rebuild();
        if (DX)
            cpu_state.flags |= (C_FLAG | V_FLAG);
        else
            cpu_state.flags &= ~(C_FLAG | V_FLAG);
        cpu_state.flags_op = FLAGS_UNKNOWN;
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
    } else {
        uint16_t dst = (uint16_t) dst32;
        uint32_t templ = (uint32_t) AX * (uint32_t) dst;
        AX = templ & 0xffff;
        DX = templ >> 16;
        flags_rebuild();
        if (DX)
            cpu_state.flags |= (C_FLAG | V_FLAG);
        else
            cpu_state.flags &= ~(C_FLAG | V_FLAG);
        cpu_state.flags_op = FLAGS_UNKNOWN;
    }
}

static JIT_WRAPPER void
jit_imul16(uint32_t dst32)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        int16_t dst = (int16_t) dst32;
        int32_t templ = (int32_t) ((int16_t) AX) * (int32_t) dst;
        AX = templ & 0xffff;
        DX = templ >> 16;
        flags_rebuild();
        if (((int32_t) templ >> 15) != 0 && ((int32_t) templ >> 15) != -1)
            cpu_state.flags |= (C_FLAG | V_FLAG);
        else
            cpu_state.flags &= ~(C_FLAG | V_FLAG);
        cpu_state.flags_op = FLAGS_UNKNOWN;
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
    } else {
        int16_t dst = (int16_t) dst32;
        int32_t templ = (int32_t) ((int16_t) AX) * (int32_t) dst;
        AX = templ & 0xffff;
        DX = templ >> 16;
        flags_rebuild();
        if (((int32_t) templ >> 15) != 0 && ((int32_t) templ >> 15) != -1)
            cpu_state.flags |= (C_FLAG | V_FLAG);
        else
            cpu_state.flags &= ~(C_FLAG | V_FLAG);
        cpu_state.flags_op = FLAGS_UNKNOWN;
    }
}

static JIT_WRAPPER int
jit_div16(uint32_t dst32)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        uint16_t dst = (uint16_t) dst32;
        if (dst == 0) {
            x86de(NULL, 0);
            codegen_profile_record_helper_tsc(host_rdtsc() - t0);
            return 1;
        }
        uint32_t templ = ((uint32_t) DX << 16) | AX;
        uint32_t templ2 = templ / dst;
        if (templ2 & 0xffff0000) {
            x86de(NULL, 0);
            codegen_profile_record_helper_tsc(host_rdtsc() - t0);
            return 1;
        }
        DX = templ % dst;
        AX = templ2 & 0xffff;
        if (!cpu_iscyrix && !is6117)
            setznp16(AX);
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
        return 0;
    } else {
        uint16_t dst = (uint16_t) dst32;
        if (dst == 0) {
            x86de(NULL, 0);
            return 1;
        }
        uint32_t templ = ((uint32_t) DX << 16) | AX;
        uint32_t templ2 = templ / dst;
        if (templ2 & 0xffff0000) {
            x86de(NULL, 0);
            return 1;
        }
        DX = templ % dst;
        AX = templ2 & 0xffff;
        if (!cpu_iscyrix && !is6117)
            setznp16(AX);
        return 0;
    }
}

static JIT_WRAPPER int
jit_idiv16(uint32_t dst32)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        int16_t dst = (int16_t) dst32;
        if (dst == 0) {
            x86de(NULL, 0);
            codegen_profile_record_helper_tsc(host_rdtsc() - t0);
            return 1;
        }
        int32_t tempws = (int32_t) (((uint32_t) DX << 16) | AX);
        int32_t templ2 = tempws / dst;
        if (templ2 < -32768 || templ2 > 32767) {
            x86de(NULL, 0);
            codegen_profile_record_helper_tsc(host_rdtsc() - t0);
            return 1;
        }
        DX = (uint16_t) (tempws % dst);
        AX = templ2 & 0xffff;
        if (!cpu_iscyrix && !is6117)
            setznp16(AX);
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
        return 0;
    } else {
        int16_t dst = (int16_t) dst32;
        if (dst == 0) {
            x86de(NULL, 0);
            return 1;
        }
        int32_t tempws = (int32_t) (((uint32_t) DX << 16) | AX);
        int32_t templ2 = tempws / dst;
        if (templ2 < -32768 || templ2 > 32767) {
            x86de(NULL, 0);
            return 1;
        }
        DX = (uint16_t) (tempws % dst);
        AX = templ2 & 0xffff;
        if (!cpu_iscyrix && !is6117)
            setznp16(AX);
        return 0;
    }
}

static JIT_WRAPPER void
jit_mul32(uint32_t dst)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        uint64_t temp64 = (uint64_t) EAX * (uint64_t) dst;
        EAX = temp64 & 0xffffffff;
        EDX = temp64 >> 32;
        flags_rebuild();
        if (EDX)
            cpu_state.flags |= (C_FLAG | V_FLAG);
        else
            cpu_state.flags &= ~(C_FLAG | V_FLAG);
        cpu_state.flags_op = FLAGS_UNKNOWN;
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
    } else {
        uint64_t temp64 = (uint64_t) EAX * (uint64_t) dst;
        EAX = temp64 & 0xffffffff;
        EDX = temp64 >> 32;
        flags_rebuild();
        if (EDX)
            cpu_state.flags |= (C_FLAG | V_FLAG);
        else
            cpu_state.flags &= ~(C_FLAG | V_FLAG);
        cpu_state.flags_op = FLAGS_UNKNOWN;
    }
}

static JIT_WRAPPER void
jit_imul32(uint32_t dst32)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        int32_t dst = (int32_t) dst32;
        int64_t temp64 = (int64_t) (int32_t) EAX * (int64_t) dst;
        EAX = temp64 & 0xffffffff;
        EDX = temp64 >> 32;
        flags_rebuild();
        if (((int64_t) temp64 >> 31) != 0 && ((int64_t) temp64 >> 31) != -1)
            cpu_state.flags |= (C_FLAG | V_FLAG);
        else
            cpu_state.flags &= ~(C_FLAG | V_FLAG);
        cpu_state.flags_op = FLAGS_UNKNOWN;
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
    } else {
        int32_t dst = (int32_t) dst32;
        int64_t temp64 = (int64_t) (int32_t) EAX * (int64_t) dst;
        EAX = temp64 & 0xffffffff;
        EDX = temp64 >> 32;
        flags_rebuild();
        if (((int64_t) temp64 >> 31) != 0 && ((int64_t) temp64 >> 31) != -1)
            cpu_state.flags |= (C_FLAG | V_FLAG);
        else
            cpu_state.flags &= ~(C_FLAG | V_FLAG);
        cpu_state.flags_op = FLAGS_UNKNOWN;
    }
}

static JIT_WRAPPER int
jit_div32(uint32_t dst)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        if (divl(dst)) {
            codegen_profile_record_helper_tsc(host_rdtsc() - t0);
            return 1;
        }
        if (!cpu_iscyrix && !is6117)
            setznp32(EAX);
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
        return 0;
    } else {
        if (divl(dst))
            return 1;
        if (!cpu_iscyrix && !is6117)
            setznp32(EAX);
        return 0;
    }
}

static JIT_WRAPPER int
jit_idiv32(uint32_t dst32)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        if (idivl((int32_t) dst32)) {
            codegen_profile_record_helper_tsc(host_rdtsc() - t0);
            return 1;
        }
        if (!cpu_iscyrix && !is6117)
            setznp32(EAX);
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
        return 0;
    } else {
        if (idivl((int32_t) dst32))
            return 1;
        if (!cpu_iscyrix && !is6117)
            setznp32(EAX);
        return 0;
    }
}

/* --- RCL / RCR helpers --- */

static JIT_WRAPPER uint32_t
jit_rcl8(uint32_t val32, uint32_t count32)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        uint8_t count = count32 & 0x1f;
        count %= 9;
        if (!count) {
            codegen_profile_record_helper_tsc(host_rdtsc() - t0);
            return val32 & 0xff;
        }
        flags_rebuild();
        uint8_t val = (uint8_t) val32;
        int temp2 = cpu_state.flags & C_FLAG;
        while (count > 0) {
            int tempc = temp2 ? 1 : 0;
            temp2 = val & 0x80;
            val = (val << 1) | tempc;
            count--;
        }
        cpu_state.flags &= ~(C_FLAG | V_FLAG);
        if (temp2)
            cpu_state.flags |= C_FLAG;
        if ((cpu_state.flags & C_FLAG) ^ (val >> 7))
            cpu_state.flags |= V_FLAG;
        cpu_state.flags_op = FLAGS_UNKNOWN;
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
        return val;
    } else {
        uint8_t count = count32 & 0x1f;
        count %= 9;
        if (!count)
            return val32 & 0xff;
        flags_rebuild();
        uint8_t val = (uint8_t) val32;
        int temp2 = cpu_state.flags & C_FLAG;
        while (count > 0) {
            int tempc = temp2 ? 1 : 0;
            temp2 = val & 0x80;
            val = (val << 1) | tempc;
            count--;
        }
        cpu_state.flags &= ~(C_FLAG | V_FLAG);
        if (temp2)
            cpu_state.flags |= C_FLAG;
        if ((cpu_state.flags & C_FLAG) ^ (val >> 7))
            cpu_state.flags |= V_FLAG;
        cpu_state.flags_op = FLAGS_UNKNOWN;
        return val;
    }
}

static JIT_WRAPPER uint32_t
jit_rcr8(uint32_t val32, uint32_t count32)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        uint8_t count = count32 & 0x1f;
        count %= 9;
        if (!count) {
            codegen_profile_record_helper_tsc(host_rdtsc() - t0);
            return val32 & 0xff;
        }
        flags_rebuild();
        uint8_t val = (uint8_t) val32;
        int temp2 = cpu_state.flags & C_FLAG;
        while (count > 0) {
            int tempc = temp2 ? 0x80 : 0;
            temp2 = val & 1;
            val = (val >> 1) | tempc;
            count--;
        }
        cpu_state.flags &= ~(C_FLAG | V_FLAG);
        if (temp2)
            cpu_state.flags |= C_FLAG;
        if ((val ^ (val >> 1)) & 0x40)
            cpu_state.flags |= V_FLAG;
        cpu_state.flags_op = FLAGS_UNKNOWN;
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
        return val;
    } else {
        uint8_t count = count32 & 0x1f;
        count %= 9;
        if (!count)
            return val32 & 0xff;
        flags_rebuild();
        uint8_t val = (uint8_t) val32;
        int temp2 = cpu_state.flags & C_FLAG;
        while (count > 0) {
            int tempc = temp2 ? 0x80 : 0;
            temp2 = val & 1;
            val = (val >> 1) | tempc;
            count--;
        }
        cpu_state.flags &= ~(C_FLAG | V_FLAG);
        if (temp2)
            cpu_state.flags |= C_FLAG;
        if ((val ^ (val >> 1)) & 0x40)
            cpu_state.flags |= V_FLAG;
        cpu_state.flags_op = FLAGS_UNKNOWN;
        return val;
    }
}

static JIT_WRAPPER uint32_t
jit_rcl16(uint32_t val32, uint32_t count32)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        uint8_t count = count32 & 0x1f;
        count %= 17;
        if (!count) {
            codegen_profile_record_helper_tsc(host_rdtsc() - t0);
            return val32 & 0xffff;
        }
        flags_rebuild();
        uint16_t val = (uint16_t) val32;
        int temp2 = cpu_state.flags & C_FLAG;
        while (count > 0) {
            int tempc = temp2 ? 1 : 0;
            temp2 = val & 0x8000;
            val = (val << 1) | tempc;
            count--;
        }
        cpu_state.flags &= ~(C_FLAG | V_FLAG);
        if (temp2)
            cpu_state.flags |= C_FLAG;
        if ((cpu_state.flags & C_FLAG) ^ (val >> 15))
            cpu_state.flags |= V_FLAG;
        cpu_state.flags_op = FLAGS_UNKNOWN;
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
        return val;
    } else {
        uint8_t count = count32 & 0x1f;
        count %= 17;
        if (!count)
            return val32 & 0xffff;
        flags_rebuild();
        uint16_t val = (uint16_t) val32;
        int temp2 = cpu_state.flags & C_FLAG;
        while (count > 0) {
            int tempc = temp2 ? 1 : 0;
            temp2 = val & 0x8000;
            val = (val << 1) | tempc;
            count--;
        }
        cpu_state.flags &= ~(C_FLAG | V_FLAG);
        if (temp2)
            cpu_state.flags |= C_FLAG;
        if ((cpu_state.flags & C_FLAG) ^ (val >> 15))
            cpu_state.flags |= V_FLAG;
        cpu_state.flags_op = FLAGS_UNKNOWN;
        return val;
    }
}

static JIT_WRAPPER uint32_t
jit_rcr16(uint32_t val32, uint32_t count32)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        uint8_t count = count32 & 0x1f;
        count %= 17;
        if (!count) {
            codegen_profile_record_helper_tsc(host_rdtsc() - t0);
            return val32 & 0xffff;
        }
        flags_rebuild();
        uint16_t val = (uint16_t) val32;
        int temp2 = cpu_state.flags & C_FLAG;
        while (count > 0) {
            int tempc = temp2 ? 0x8000 : 0;
            temp2 = val & 1;
            val = (val >> 1) | tempc;
            count--;
        }
        cpu_state.flags &= ~(C_FLAG | V_FLAG);
        if (temp2)
            cpu_state.flags |= C_FLAG;
        if ((val ^ (val >> 1)) & 0x4000)
            cpu_state.flags |= V_FLAG;
        cpu_state.flags_op = FLAGS_UNKNOWN;
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
        return val;
    } else {
        uint8_t count = count32 & 0x1f;
        count %= 17;
        if (!count)
            return val32 & 0xffff;
        flags_rebuild();
        uint16_t val = (uint16_t) val32;
        int temp2 = cpu_state.flags & C_FLAG;
        while (count > 0) {
            int tempc = temp2 ? 0x8000 : 0;
            temp2 = val & 1;
            val = (val >> 1) | tempc;
            count--;
        }
        cpu_state.flags &= ~(C_FLAG | V_FLAG);
        if (temp2)
            cpu_state.flags |= C_FLAG;
        if ((val ^ (val >> 1)) & 0x4000)
            cpu_state.flags |= V_FLAG;
        cpu_state.flags_op = FLAGS_UNKNOWN;
        return val;
    }
}

static JIT_WRAPPER uint32_t
jit_rcl32(uint32_t val, uint32_t count32)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        uint8_t count = count32 & 0x1f;
        if (!count) {
            codegen_profile_record_helper_tsc(host_rdtsc() - t0);
            return val;
        }
        flags_rebuild();
        int temp2 = cpu_state.flags & C_FLAG;
        while (count > 0) {
            int tempc = temp2 ? 1 : 0;
            temp2 = val & 0x80000000;
            val = (val << 1) | tempc;
            count--;
        }
        cpu_state.flags &= ~(C_FLAG | V_FLAG);
        if (temp2)
            cpu_state.flags |= C_FLAG;
        if ((cpu_state.flags & C_FLAG) ^ (val >> 31))
            cpu_state.flags |= V_FLAG;
        cpu_state.flags_op = FLAGS_UNKNOWN;
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
        return val;
    } else {
        uint8_t count = count32 & 0x1f;
        if (!count)
            return val;
        flags_rebuild();
        int temp2 = cpu_state.flags & C_FLAG;
        while (count > 0) {
            int tempc = temp2 ? 1 : 0;
            temp2 = val & 0x80000000;
            val = (val << 1) | tempc;
            count--;
        }
        cpu_state.flags &= ~(C_FLAG | V_FLAG);
        if (temp2)
            cpu_state.flags |= C_FLAG;
        if ((cpu_state.flags & C_FLAG) ^ (val >> 31))
            cpu_state.flags |= V_FLAG;
        cpu_state.flags_op = FLAGS_UNKNOWN;
        return val;
    }
}

static JIT_WRAPPER uint32_t
jit_rcr32(uint32_t val, uint32_t count32)
{
    if (codegen_profile_active) {
        uint64_t t0 = host_rdtsc();
        uint8_t count = count32 & 0x1f;
        if (!count) {
            codegen_profile_record_helper_tsc(host_rdtsc() - t0);
            return val;
        }
        flags_rebuild();
        int temp2 = cpu_state.flags & C_FLAG;
        while (count > 0) {
            uint32_t tempc = temp2 ? 0x80000000 : 0;
            temp2 = val & 1;
            val = (val >> 1) | tempc;
            count--;
        }
        cpu_state.flags &= ~(C_FLAG | V_FLAG);
        if (temp2)
            cpu_state.flags |= C_FLAG;
        if ((val ^ (val >> 1)) & 0x40000000)
            cpu_state.flags |= V_FLAG;
        cpu_state.flags_op = FLAGS_UNKNOWN;
        codegen_profile_record_helper_tsc(host_rdtsc() - t0);
        return val;
    } else {
        uint8_t count = count32 & 0x1f;
        if (!count)
            return val;
        flags_rebuild();
        int temp2 = cpu_state.flags & C_FLAG;
        while (count > 0) {
            uint32_t tempc = temp2 ? 0x80000000 : 0;
            temp2 = val & 1;
            val = (val >> 1) | tempc;
            count--;
        }
        cpu_state.flags &= ~(C_FLAG | V_FLAG);
        if (temp2)
            cpu_state.flags |= C_FLAG;
        if ((val ^ (val >> 1)) & 0x40000000)
            cpu_state.flags |= V_FLAG;
        cpu_state.flags_op = FLAGS_UNKNOWN;
        return val;
    }
}

#endif /* CODEGEN_OPS_JIT_WRAPPERS_H */

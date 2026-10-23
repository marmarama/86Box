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

#include "codegen_dump.h"

#define JIT_WRAPPER __attribute__((noinline, used))

/* --- flags_rebuild family (void → void) --- */

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

#endif /* CODEGEN_OPS_JIT_WRAPPERS_H */

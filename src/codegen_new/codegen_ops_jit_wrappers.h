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

#define JIT_WRAPPER __attribute__((noinline, used))

/* --- flags_rebuild family (void → void) --- */

static JIT_WRAPPER void
jit_flags_rebuild(void)
{
    flags_rebuild();
}

static JIT_WRAPPER void
jit_flags_rebuild_c(void)
{
    flags_rebuild_c();
}

static JIT_WRAPPER void
jit_flags_rebuild_zpna(void)
{
    flags_rebuild_zpna();
}

/* --- individual flag test functions (void → int) --- */

static JIT_WRAPPER int
jit_CF_SET(void)
{
    return CF_SET();
}

static JIT_WRAPPER int
jit_NF_SET(void)
{
    return NF_SET();
}

static JIT_WRAPPER int
jit_PF_SET(void)
{
    return PF_SET();
}

static JIT_WRAPPER int
jit_VF_SET(void)
{
    return VF_SET();
}

static JIT_WRAPPER int
jit_ZF_SET(void)
{
    return ZF_SET();
}

/* --- condition test functions (void → int) --- */

static JIT_WRAPPER int
jit_cond_O(void)
{
    return VF_SET() ? 1 : 0;
}
static JIT_WRAPPER int
jit_cond_NO(void)
{
    return !VF_SET() ? 1 : 0;
}
static JIT_WRAPPER int
jit_cond_B(void)
{
    return CF_SET() ? 1 : 0;
}
static JIT_WRAPPER int
jit_cond_NB(void)
{
    return !CF_SET() ? 1 : 0;
}
static JIT_WRAPPER int
jit_cond_E(void)
{
    return ZF_SET() ? 1 : 0;
}
static JIT_WRAPPER int
jit_cond_NE(void)
{
    return !ZF_SET() ? 1 : 0;
}
static JIT_WRAPPER int
jit_cond_BE(void)
{
    return (CF_SET() || ZF_SET()) ? 1 : 0;
}
static JIT_WRAPPER int
jit_cond_NBE(void)
{
    return (!CF_SET() && !ZF_SET()) ? 1 : 0;
}
static JIT_WRAPPER int
jit_cond_S(void)
{
    return NF_SET() ? 1 : 0;
}
static JIT_WRAPPER int
jit_cond_NS(void)
{
    return !NF_SET() ? 1 : 0;
}
static JIT_WRAPPER int
jit_cond_P(void)
{
    return PF_SET() ? 1 : 0;
}
static JIT_WRAPPER int
jit_cond_NP(void)
{
    return !PF_SET() ? 1 : 0;
}
static JIT_WRAPPER int
jit_cond_L(void)
{
    return ((NF_SET() ? 1 : 0) != (VF_SET() ? 1 : 0)) ? 1 : 0;
}
static JIT_WRAPPER int
jit_cond_NL(void)
{
    return ((NF_SET() ? 1 : 0) == (VF_SET() ? 1 : 0)) ? 1 : 0;
}
static JIT_WRAPPER int
jit_cond_LE(void)
{
    return (((NF_SET() ? 1 : 0) != (VF_SET() ? 1 : 0)) || ZF_SET()) ? 1 : 0;
}
static JIT_WRAPPER int
jit_cond_NLE(void)
{
    return (((NF_SET() ? 1 : 0) == (VF_SET() ? 1 : 0)) && !ZF_SET()) ? 1 : 0;
}

#endif /* CODEGEN_OPS_JIT_WRAPPERS_H */

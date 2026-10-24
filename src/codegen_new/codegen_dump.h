#ifndef CODEGEN_DUMP_H
#define CODEGEN_DUMP_H

#include <stdint.h>
#include <stddef.h>
#include <86box/plat_unused.h>
#include "codegen.h"
#include "codegen_backend.h"
#include "codegen_ir.h"

#define BDUMP_MAGIC   0x44423638U /* "86BD" */
#define BDUMP_VERSION 1

enum bdump_host_arch {
    BDUMP_ARCH_X86_64      = 1,
    BDUMP_ARCH_ARM64       = 2,
    BDUMP_ARCH_LOONGARCH64 = 3
};

#if defined(__x86_64__) || defined(_M_X64)
#    define BDUMP_CURRENT_ARCH BDUMP_ARCH_X86_64
#elif defined(__aarch64__) || defined(_M_ARM64)
#    define BDUMP_CURRENT_ARCH BDUMP_ARCH_ARM64
#elif defined(__loongarch_lp64)
#    define BDUMP_CURRENT_ARCH BDUMP_ARCH_LOONGARCH64
#else
#    define BDUMP_CURRENT_ARCH 0
#endif

enum bdump_record_type {
    BDUMP_REC_COMPILE      = 1,
    BDUMP_REC_EXEC         = 2,
    BDUMP_REC_HOST_PROFILE = 3,
    BDUMP_REC_HANG         = 4
};

enum bdump_hang_reason {
    BDUMP_HANG_NONE          = 0,
    BDUMP_HANG_CHAIN_LOCKUP  = 1, /* Excess chained transfers without returning to C */
    BDUMP_HANG_SPIN_LOOP     = 2, /* Tight spin loop in <= 32 unique blocks */
    BDUMP_HANG_SIGNAL_ABORT  = 3  /* User SIGINT / SIGTERM interrupt */
};

#pragma pack(push, 1)

typedef struct bdump_file_header_t {
    uint32_t magic;            /* BDUMP_MAGIC */
    uint16_t version;          /* BDUMP_VERSION */
    uint16_t host_arch;        /* bdump_host_arch */
    uint64_t cpu_state_addr;   /* Base address of cpu_state */
    uint64_t readlookup2_addr; /* Base address of readlookup2 */
    uint64_t ram_addr;         /* Base address of guest ram[] */
    uint16_t symbol_count;     /* Number of bdump_symbol_entry_t entries following header */
} bdump_file_header_t;

typedef struct bdump_symbol_entry_t {
    int32_t offset;            /* Offset relative to cpu_state (signed) */
    char    name[28];          /* Null-terminated field name */
} bdump_symbol_entry_t;

typedef struct bdump_compile_record_t {
    uint8_t  rec_type;         /* BDUMP_REC_COMPILE */
    uint8_t  exit_type;        /* BLOCK_EXIT_NONE / DIRECT_1 / COND_2 / RET */
    uint16_t block_nr;         /* Index in codeblock[] */
    uint32_t compile_seq;      /* Monotonic compilation ID */
    uint32_t pc;               /* Linear entry address (cs + eip) */
    uint32_t _cs;              /* CS base */
    uint32_t phys;             /* Physical entry address */
    uint32_t phys_2;           /* Second page physical address */
    uint32_t status;           /* cpu_cur_status */
    uint16_t flags;            /* CODEBLOCK_HAS_FPU, STATIC_TOP, etc. */
    uint8_t  top;              /* block->TOP at compile time */
    uint8_t  unroll_count;     /* Loop unroll factor */
    uint16_t guest_insn_count; /* block->ins */
    uint16_t guest_byte_len;   /* Number of raw guest x86 bytes */
    uint16_t uop_count;        /* ir->wr_pos */
    uint16_t host_byte_len;    /* Emitted host machine code length */
    uint16_t prologue_end_off; /* Host byte offset where prologue ends */
    uint16_t epilogue_pos_off; /* Host byte offset where epilogue begins */
    uint64_t host_code_addr;   /* Runtime pointer to block_write_data */
    uint64_t page_mask;        /* Dirty mask page 1 */
    uint64_t page_mask2;       /* Dirty mask page 2 */
} bdump_compile_record_t;

typedef struct bdump_insn_span_t {
    uint16_t guest_off;        /* Byte offset into guest_bytes[] */
    uint8_t  guest_len;        /* Byte length of this guest x86 instruction */
    uint8_t  op_32;            /* Effective operand/address size flags */
} bdump_insn_span_t;

typedef struct bdump_uop_entry_t {
    uint32_t orig_type;        /* uOp type before peephole/DSE */
    uint32_t final_type;       /* uOp type after peephole/DSE */
    uint32_t imm_data;         /* Immediate payload */
    uint64_t func_ptr;         /* Target C helper or instruction pointer */
    uint16_t dest_reg_a;       /* ir_reg_t.reg */
    uint16_t src_reg_a;
    uint16_t src_reg_b;
    uint16_t src_reg_c;
    int16_t  dest_host_reg;    /* Allocated host reg (uop->dest_reg_a_real) */
    int16_t  src_host_reg_a;
    int16_t  src_host_reg_b;
    int16_t  src_host_reg_c;
    uint16_t guest_insn_idx;   /* Index into guest_insns[] */
    uint16_t host_start_off;   /* Start of barrier/allocator glue */
    uint16_t host_body_off;    /* Start of uOp handler emission */
    uint16_t host_end_off;     /* End of uOp emission */
} bdump_uop_entry_t;

typedef struct bdump_exec_record_t {
    uint8_t  rec_type;         /* BDUMP_REC_EXEC */
    uint8_t  invalidate_reason;/* 0 = VM exit, 1 = SMC dirty flush, 2 = eviction */
    uint16_t block_nr;
    uint32_t compile_seq;      /* Matches bdump_compile_record_t.compile_seq */
    uint64_t exec_count;       /* Number of times this block was entered */
    uint64_t host_tsc_total;   /* Inclusive host TSC cycles spent in this block */
    uint64_t host_tsc_callouts;/* Host TSC cycles spent in C callouts from this block */
    uint32_t link0_target_pc;
    uint32_t link1_target_pc;
    uint16_t link0_target_blk;
    uint16_t link1_target_blk;
} bdump_exec_record_t;

typedef struct bdump_host_profile_t {
    uint8_t  rec_type;               /* BDUMP_REC_HOST_PROFILE */
    uint8_t  reserved[7];
    uint64_t tsc_total_cpu_thread;   /* Total host TSC cycles in exec386_dynarec_dyn() */
    uint64_t tsc_jit_pure;           /* Pure native JIT execution (excluding C callouts) */
    uint64_t tsc_c_callout_mem_io;   /* Intra-block C slow-path loads/stores (MMIO, SVGA VRAM) */
    uint64_t tsc_c_callout_insn_fn;  /* Intra-block UOP_CALL_INSTRUCTION_FUNC (IN/OUT, INT, etc.) */
    uint64_t tsc_c_callout_helper;   /* Intra-block UOP_CALL_FUNC (flags_rebuild, FPU helpers) */
    uint64_t tsc_c_dispatcher;       /* C dispatch loop (hash probe, dirty checks, linking) */
    uint64_t tsc_c_compiler;         /* JIT compilation, IR passes, and block unlinking */
    uint64_t tsc_c_interp_fallback;  /* Full-interpreter blocks (!block->valid / SMC fallback) */
    uint64_t tsc_c_timers_devices;   /* timer_process_inline(), IRQ/NMI handling, DMA */
    uint64_t count_c_dispatch_loops; /* Number of returns to the C outer loop */
    uint64_t count_chained_transfers;/* Number of direct block-to-block transfers */
    uint64_t count_mem_slow_loads;   /* Number of TLB-miss / MMIO / misaligned C read calls */
    uint64_t count_mem_slow_stores;  /* Number of TLB-miss / MMIO / misaligned C write calls */
    /* Real-Time (RT) Emulation Speed Telemetry */
    uint32_t rt_sample_count;        /* Total number of RT speed samples logged */
    uint32_t rt_speed_min;           /* Minimum recorded % RT (e.g. 18) */
    uint32_t rt_speed_max;           /* Maximum recorded % RT (e.g. 105) */
    uint32_t rt_samples_below_100;   /* Number of samples where % RT < 100 */
    uint64_t rt_speed_sum;           /* Cumulative sum of % RT values (average = sum / count) */
    uint32_t rt_series_len;          /* Number of trailing bdump_rt_sample_t entries */
    uint32_t reserved2;              /* 8-byte alignment padding */
} bdump_host_profile_t;

typedef struct bdump_rt_sample_t {
    uint32_t elapsed_ms;             /* Cumulative elapsed ms since emulation started */
    uint16_t speed_percent;          /* Emulation speed percentage (e.g. 100, 70, 18) */
    uint16_t fps;                    /* Frames executed in the sample interval */
} bdump_rt_sample_t;

typedef struct bdump_trace_entry_t {
    uint32_t tr_compile_seq;         /* Compile sequence ID of entered block */
    uint16_t tr_block_nr;            /* Index in codeblock[] */
    uint16_t tr_flags_op;            /* Lazy flags operation ID */
    uint32_t tr_pc;                  /* Linear PC (cs + eip) */
    uint32_t tr_phys;                /* Physical address */
    uint32_t tr_eax;
    uint32_t tr_ecx;
    uint32_t tr_edx;
    uint32_t tr_ebx;
    uint32_t tr_esp;
    uint32_t tr_ebp;
    uint32_t tr_esi;
    uint32_t tr_edi;
    int32_t  tr_cycles;              /* cpu_state._cycles at block entry */
    uint32_t tr_flags_res;           /* Lazy flags result operand */
} bdump_trace_entry_t;

typedef struct bdump_hang_record_t {
    uint8_t  rec_type;                /* BDUMP_REC_HANG (4) */
    uint8_t  hang_reason;             /* bdump_hang_reason */
    uint16_t unique_blocks_in_window; /* Number of unique blocks in active spin window */
    uint32_t trace_count;             /* Number of trailing bdump_trace_entry_t records */
    uint64_t spin_iterations;         /* Consecutive block entries in the current spin window */
    uint64_t chained_streak;          /* Consecutive chained block transfers without returning to C */
    uint32_t trigger_compile_seq;     /* compile_seq of block where hang triggered */
    uint16_t trigger_block_nr;        /* block_nr where hang triggered */
    uint16_t reserved;
} bdump_hang_record_t;

#pragma pack(pop)

#if defined(__x86_64__) || defined(_M_X64)
#    if defined(_MSC_VER)
#        include <intrin.h>
#        define host_rdtsc() __rdtsc()
#    else
#        include <x86intrin.h>
#        define host_rdtsc() __rdtsc()
#    endif
#elif defined(__aarch64__) || defined(_M_ARM64)
static inline uint64_t host_rdtsc(void) {
    uint64_t val;
    asm volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
}
#elif defined(__loongarch_lp64)
static inline uint64_t host_rdtsc(void) {
    uint64_t val;
    asm volatile("rdtime.d %0, $zero" : "=r"(val));
    return val;
}
#else
static inline uint64_t host_rdtsc(void) {
    return 0;
}
#endif

extern bdump_host_profile_t codegen_host_profile;
extern int codegen_profile_active;
extern int codegen_hang_detect_active;
extern codeblock_t *codegen_current_executing_block;

void codegen_dump_init(void);
void codegen_dump_close(void);
int  codegen_dump_is_active(void);
int  codegen_dump_perf_map_active(void);

void     codegen_dump_begin_block(void);
void     codegen_dump_record_chunk(const uint8_t *chunk_ptr, uint32_t chunk_len);
uint16_t codegen_dump_get_pos(uint32_t current_block_pos);

void codegen_dump_compile_block(codeblock_t *block, ir_data_t *ir,
                                uint16_t prologue_end, uint16_t epilogue_pos,
                                const uint32_t *orig_uop_types, const uint32_t *orig_uop_imms,
                                const uint16_t *uop_host_starts, const uint16_t *uop_host_body_starts,
                                const uint16_t *uop_host_ends);

void codegen_dump_exec_block(codeblock_t *block, uint8_t invalidate_reason);
void codegen_dump_perf_map_block(codeblock_t *block);
void codegen_dump_perf_map_symbol(uintptr_t addr, size_t size, const char *name);

void codegen_dump_enter_block(codeblock_t *block);
void codegen_dump_notify_idle(void);
void codegen_profile_exit_chain(void);
void codegen_dump_record_rt_sample(int speed_percent, int fps, uint32_t elapsed_ms);

uint8_t  codegen_profile_readmembl(uint32_t a);
uint16_t codegen_profile_readmemwl(uint32_t a);
uint32_t codegen_profile_readmemll(uint32_t a);
uint64_t codegen_profile_readmemql(uint32_t a);
void     codegen_profile_writemembl(uint32_t a, uint8_t v);
void     codegen_profile_writememwl(uint32_t a, uint16_t v);
void     codegen_profile_writememll(uint32_t a, uint32_t v);
void     codegen_profile_writememql(uint32_t a, uint64_t v);
int      codegen_profile_call_instruction_func(uint32_t fetchdat, int (*fn)(uint32_t));
void     codegen_profile_record_helper_tsc(uint64_t elapsed);

#endif /* CODEGEN_DUMP_H */

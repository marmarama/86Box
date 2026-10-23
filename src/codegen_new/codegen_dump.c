#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <signal.h>

#include <86box/86box.h>
#include "cpu.h"
#include <86box/mem.h>
#include <86box/plat_unused.h>

#include "x86.h"
#include "x86seg_common.h"
#include "x86seg.h"
#include "x87_sf.h"
#include "x87.h"
#include "386_common.h"

#include "codegen.h"
#include "codegen_backend.h"
#include "codegen_ir.h"
#include "codegen_dump.h"

static FILE    *dump_fp               = NULL;
static FILE    *perf_map_fp           = NULL;
static int      dump_active           = 0;
static int      perf_map_active       = 0;
static uint32_t compile_seq_counter   = 0;

bdump_host_profile_t codegen_host_profile             = {0};
int                  codegen_profile_active           = 0;
int                  codegen_hang_detect_active       = 0;
codeblock_t         *codegen_current_executing_block  = NULL;
static uint64_t      codegen_last_block_entry_tsc     = 0;
static uint64_t      codegen_last_block_callouts      = 0;

#define FLIGHT_RECORDER_SIZE 128
static bdump_trace_entry_t flight_recorder[FLIGHT_RECORDER_SIZE];
static uint64_t            flight_recorder_total      = 0;
static int                 flight_recorder_enabled    = 0;

static uint64_t            hang_spin_limit            = 50000000ULL;
static uint64_t            hang_chain_limit           = 1000000ULL;
static uint64_t            hang_chained_streak        = 0;
static uint64_t            hang_spin_iterations       = 0;
static uint64_t            hang_window_entries        = 0;
static uint64_t            hang_seen_bitmap[(BLOCK_SIZE + 63) / 64];
static uint16_t            hang_last_unique_blocks    = 0;
static volatile sig_atomic_t hang_triggered           = 0;

static void codegen_trigger_hang(uint8_t reason, codeblock_t *trigger_blk, int from_signal);
static void codegen_hang_sig_handler(int sig);

static bdump_rt_sample_t *rt_sample_series            = NULL;
static uint32_t           rt_sample_capacity          = 0;
static uint32_t           rt_sample_cumulative_ms     = 0;

#define MAX_DUMP_CHUNKS 32
typedef struct dump_chunk_t {
    const uint8_t *ptr;
    uint32_t       len;
} dump_chunk_t;

static dump_chunk_t dump_chunks[MAX_DUMP_CHUNKS];
static int          dump_chunk_count = 0;
static uint32_t     dump_cum_offset  = 0;

static uint32_t dump_pc_start         = 0;
static uint32_t dump_pc_end           = 0xffffffff;
static int      dump_pc_filter        = 0;

static uint32_t dump_phys_start       = 0;
static uint32_t dump_phys_end         = 0xffffffff;
static int      dump_phys_filter      = 0;

static uint64_t dump_min_exec         = 0;

/* Buffer structure for deferred dumping when BOX86_DUMP_MIN_EXEC is set */
typedef struct buffered_block_t {
    size_t   total_size;
    uint8_t *data;
} buffered_block_t;

static buffered_block_t *buffered_blocks = NULL;

static const bdump_symbol_entry_t cpu_state_symbols[] = {
    { (int32_t) offsetof(cpu_state_t, regs[0].l), "EAX" },
    { (int32_t) offsetof(cpu_state_t, regs[1].l), "ECX" },
    { (int32_t) offsetof(cpu_state_t, regs[2].l), "EDX" },
    { (int32_t) offsetof(cpu_state_t, regs[3].l), "EBX" },
    { (int32_t) offsetof(cpu_state_t, regs[4].l), "ESP" },
    { (int32_t) offsetof(cpu_state_t, regs[5].l), "EBP" },
    { (int32_t) offsetof(cpu_state_t, regs[6].l), "ESI" },
    { (int32_t) offsetof(cpu_state_t, regs[7].l), "EDI" },
    { (int32_t) offsetof(cpu_state_t, flags_op),  "flags_op" },
    { (int32_t) offsetof(cpu_state_t, flags_res), "flags_res" },
    { (int32_t) offsetof(cpu_state_t, flags_op1), "flags_op1" },
    { (int32_t) offsetof(cpu_state_t, flags_op2), "flags_op2" },
    { (int32_t) offsetof(cpu_state_t, pc),        "pc" },
    { (int32_t) offsetof(cpu_state_t, oldpc),     "oldpc" },
    { (int32_t) offsetof(cpu_state_t, eaaddr),    "eaaddr" },
    { (int32_t) offsetof(cpu_state_t, ea_seg),    "ea_seg" },
    { (int32_t) offsetof(cpu_state_t, op32),      "op32" },
    { (int32_t) offsetof(cpu_state_t, ssegs),     "ssegs" },
    { (int32_t) offsetof(cpu_state_t, ismmx),     "ismmx" },
    { (int32_t) offsetof(cpu_state_t, _cycles),   "_cycles" },
    { (int32_t) offsetof(cpu_state_t, exit_request), "exit_request" },
    { (int32_t) offsetof(cpu_state_t, TOP),       "TOP" },
    { (int32_t) offsetof(cpu_state_t, temp_CS),   "temp_CS" },
    { (int32_t) offsetof(cpu_state_t, temp_cs),   "temp_cs" },
    { (int32_t) offsetof(cpu_state_t, temp_pc),   "temp_pc" },
    { (int32_t) offsetof(cpu_state_t, seg_cs.base), "seg_cs.base" },
    { (int32_t) offsetof(cpu_state_t, seg_ds.base), "seg_ds.base" },
    { (int32_t) offsetof(cpu_state_t, seg_es.base), "seg_es.base" },
    { (int32_t) offsetof(cpu_state_t, seg_fs.base), "seg_fs.base" },
    { (int32_t) offsetof(cpu_state_t, seg_gs.base), "seg_gs.base" },
    { (int32_t) offsetof(cpu_state_t, seg_ss.base), "seg_ss.base" },
    { (int32_t) offsetof(cpu_state_t, seg_cs.seg),  "seg_cs.seg" },
    { (int32_t) offsetof(cpu_state_t, seg_ds.seg),  "seg_ds.seg" },
    { (int32_t) offsetof(cpu_state_t, seg_es.seg),  "seg_es.seg" },
    { (int32_t) offsetof(cpu_state_t, seg_fs.seg),  "seg_fs.seg" },
    { (int32_t) offsetof(cpu_state_t, seg_gs.seg),  "seg_gs.seg" },
    { (int32_t) offsetof(cpu_state_t, seg_ss.seg),  "seg_ss.seg" },
    { (int32_t) offsetof(cpu_state_t, ST[0]),     "ST[0]" },
    { (int32_t) offsetof(cpu_state_t, ST[1]),     "ST[1]" },
    { (int32_t) offsetof(cpu_state_t, ST[2]),     "ST[2]" },
    { (int32_t) offsetof(cpu_state_t, ST[3]),     "ST[3]" },
    { (int32_t) offsetof(cpu_state_t, ST[4]),     "ST[4]" },
    { (int32_t) offsetof(cpu_state_t, ST[5]),     "ST[5]" },
    { (int32_t) offsetof(cpu_state_t, ST[6]),     "ST[6]" },
    { (int32_t) offsetof(cpu_state_t, ST[7]),     "ST[7]" },
    { (int32_t) offsetof(cpu_state_t, tag[0]),    "tag[0]" },
    { (int32_t) offsetof(cpu_state_t, tag[4]),    "tag[4]" },
    { (int32_t) offsetof(cpu_state_t, MM[0]),     "MM[0]" },
    { (int32_t) offsetof(cpu_state_t, MM[1]),     "MM[1]" },
    { (int32_t) offsetof(cpu_state_t, MM[2]),     "MM[2]" },
    { (int32_t) offsetof(cpu_state_t, MM[3]),     "MM[3]" },
    { (int32_t) offsetof(cpu_state_t, MM[4]),     "MM[4]" },
    { (int32_t) offsetof(cpu_state_t, MM[5]),     "MM[5]" },
    { (int32_t) offsetof(cpu_state_t, MM[6]),     "MM[6]" },
    { (int32_t) offsetof(cpu_state_t, MM[7]),     "MM[7]" },
};

int
codegen_dump_is_active(void)
{
    return dump_active;
}

int
codegen_dump_perf_map_active(void)
{
    return perf_map_active;
}

void
codegen_dump_init(void)
{
    const char *dump_path   = getenv("BOX86_DUMP_BLOCKS");
    const char *perf_env    = getenv("BOX86_PERF_MAP");
    const char *pc_env      = getenv("BOX86_DUMP_PC");
    const char *phys_env    = getenv("BOX86_DUMP_PHYS");
    const char *min_exec    = getenv("BOX86_DUMP_MIN_EXEC");
    const char *profile_env = getenv("BOX86_DUMP_PROFILE");
    const char *hang_env    = getenv("BOX86_HANG_DETECT");

    if (profile_env && (strcmp(profile_env, "1") == 0 || strcasecmp(profile_env, "true") == 0)) {
        codegen_profile_active = 1;
        pclog("codegen_dump: host TSC profiling enabled (BOX86_DUMP_PROFILE=1)\n");
    }

    if (hang_env && hang_env[0]) {
        if (strcmp(hang_env, "0") == 0 || strcasecmp(hang_env, "false") == 0) {
            codegen_hang_detect_active = 0;
        } else if (strcmp(hang_env, "1") == 0 || strcasecmp(hang_env, "true") == 0) {
            codegen_hang_detect_active = 1;
            flight_recorder_enabled    = 1;
            hang_spin_limit            = 20000000ULL;
            pclog("codegen_dump: hang detector enabled (spin_limit=%" PRIu64 ", chain_limit=%" PRIu64 ")\n",
                  hang_spin_limit, hang_chain_limit);
        } else {
            uint64_t val = strtoull(hang_env, NULL, 0);
            if (val > 0) {
                codegen_hang_detect_active = 1;
                flight_recorder_enabled    = 1;
                hang_spin_limit            = val;
                if (val < hang_chain_limit)
                    hang_chain_limit = val;
                pclog("codegen_dump: hang detector enabled with custom limit=%" PRIu64 "\n", hang_spin_limit);
            }
        }
    }

    if (pc_env) {
        uint32_t s = 0, e = 0;
        if (sscanf(pc_env, "0x%" SCNx32 "-0x%" SCNx32, &s, &e) == 2 ||
            sscanf(pc_env, "%" SCNu32 "-%" SCNu32, &s, &e) == 2) {
            dump_pc_start  = s;
            dump_pc_end    = e;
            dump_pc_filter = 1;
        }
    }

    if (phys_env) {
        uint32_t s = 0, e = 0;
        if (sscanf(phys_env, "0x%" SCNx32 "-0x%" SCNx32, &s, &e) == 2 ||
            sscanf(phys_env, "%" SCNu32 "-%" SCNu32, &s, &e) == 2) {
            dump_phys_start  = s;
            dump_phys_end    = e;
            dump_phys_filter = 1;
        }
    }

    if (min_exec) {
        dump_min_exec = strtoull(min_exec, NULL, 0);
    }

    if (perf_env && (strcmp(perf_env, "1") == 0 || strcasecmp(perf_env, "true") == 0)) {
        char map_path[64];
        snprintf(map_path, sizeof(map_path), "/tmp/perf-%d.map", getpid());
        perf_map_fp = fopen(map_path, "w");
        if (perf_map_fp) {
            perf_map_active = 1;
            setvbuf(perf_map_fp, NULL, _IOLBF, 0);
            pclog("codegen_dump: writing Linux perf map to %s\n", map_path);
        }
    }

    if (dump_path && dump_path[0]) {
        dump_fp = fopen(dump_path, "wb");
        if (dump_fp) {
            dump_active = 1;
            pclog("codegen_dump: writing binary block trace to %s\n", dump_path);

            bdump_file_header_t hdr;
            memset(&hdr, 0, sizeof(hdr));
            hdr.magic            = BDUMP_MAGIC;
            hdr.version          = BDUMP_VERSION;
            hdr.host_arch        = BDUMP_CURRENT_ARCH;
            hdr.cpu_state_addr   = (uint64_t) (uintptr_t) &cpu_state;
            hdr.readlookup2_addr = (uint64_t) (uintptr_t) readlookup2;
            hdr.ram_addr         = (uint64_t) (uintptr_t) ram;
            hdr.symbol_count     = (uint16_t) (sizeof(cpu_state_symbols) / sizeof(cpu_state_symbols[0]));

            fwrite(&hdr, sizeof(hdr), 1, dump_fp);
            fwrite(cpu_state_symbols, sizeof(cpu_state_symbols[0]), hdr.symbol_count, dump_fp);
            fflush(dump_fp);

            if (dump_min_exec > 0) {
                buffered_blocks = calloc(BLOCK_SIZE, sizeof(buffered_block_t));
            }

            flight_recorder_enabled = 1;
            if (!hang_env || !hang_env[0]) {
                /* Enable automatic hang detection by default when block dumping is active */
                codegen_hang_detect_active = 1;
                hang_spin_limit            = 50000000ULL;
            }
        }
    }

    if (dump_active || perf_map_active || codegen_hang_detect_active) {
        atexit(codegen_dump_close);
        signal(SIGINT, codegen_hang_sig_handler);
        signal(SIGTERM, codegen_hang_sig_handler);
    }
}

void
codegen_dump_close(void)
{
    if (dump_active && dump_fp) {
        /* Flush active blocks as VM exit records (reason = 0) */
        if (codeblock) {
            for (uint32_t c = 0; c < BLOCK_SIZE; c++) {
                if (codeblock[c].valid && (codeblock[c].flags & CODEBLOCK_WAS_RECOMPILED)) {
                    codegen_dump_exec_block(&codeblock[c], 0);
                }
            }
        }

        if (buffered_blocks) {
            for (uint32_t c = 0; c < BLOCK_SIZE; c++) {
                if (buffered_blocks[c].data) {
                    free(buffered_blocks[c].data);
                    buffered_blocks[c].data = NULL;
                }
            }
            free(buffered_blocks);
            buffered_blocks = NULL;
        }

        if (codegen_profile_active || codegen_host_profile.rt_sample_count > 0) {
            codegen_host_profile.rec_type = BDUMP_REC_HOST_PROFILE;
            fwrite(&codegen_host_profile, sizeof(codegen_host_profile), 1, dump_fp);
            if (codegen_host_profile.rt_series_len > 0 && rt_sample_series) {
                fwrite(rt_sample_series, sizeof(bdump_rt_sample_t), codegen_host_profile.rt_series_len, dump_fp);
            }
            fflush(dump_fp);
        }

        fclose(dump_fp);
        dump_fp     = NULL;
        dump_active = 0;
    }

    if (rt_sample_series) {
        free(rt_sample_series);
        rt_sample_series   = NULL;
        rt_sample_capacity = 0;
    }

    if (perf_map_active && perf_map_fp) {
        fclose(perf_map_fp);
        perf_map_fp     = NULL;
        perf_map_active = 0;
    }
}

void
codegen_dump_begin_block(void)
{
    dump_chunk_count = 0;
    dump_cum_offset  = 0;
}

void
codegen_dump_record_chunk(const uint8_t *chunk_ptr, uint32_t chunk_len)
{
    if (dump_chunk_count < MAX_DUMP_CHUNKS) {
        dump_chunks[dump_chunk_count].ptr = chunk_ptr;
        dump_chunks[dump_chunk_count].len = chunk_len;
        dump_chunk_count++;
    }
    dump_cum_offset += chunk_len;
}

uint16_t
codegen_dump_get_pos(uint32_t current_block_pos)
{
    uint32_t total = dump_cum_offset + current_block_pos;
    return (total > 0xffff) ? 0xffff : (uint16_t) total;
}

void
codegen_dump_perf_map_block(codeblock_t *block)
{
    if (!perf_map_fp || !block || dump_chunk_count <= 0)
        return;

    uint16_t block_nr = (codeblock) ? (uint16_t) (block - codeblock) : 0;
    for (int i = 0; i < dump_chunk_count; i++) {
        if (dump_chunks[i].len == 0)
            continue;
        if (i == 0) {
            fprintf(perf_map_fp, "%lx %x jit_b%04u_%04x:%08x_p%08x\n",
                    (unsigned long) (uintptr_t) dump_chunks[i].ptr,
                    (unsigned int) dump_chunks[i].len,
                    block_nr,
                    block->_cs >> 4,
                    block->pc - block->_cs,
                    block->phys);
        } else {
            fprintf(perf_map_fp, "%lx %x jit_b%04u_%04x:%08x_p%08x_part%d\n",
                    (unsigned long) (uintptr_t) dump_chunks[i].ptr,
                    (unsigned int) dump_chunks[i].len,
                    block_nr,
                    block->_cs >> 4,
                    block->pc - block->_cs,
                    block->phys,
                    i);
        }
    }
    fflush(perf_map_fp);
}

void
codegen_dump_perf_map_symbol(uintptr_t addr, size_t size, const char *name)
{
    if (!perf_map_fp)
        return;

    fprintf(perf_map_fp, "%lx %lx %s\n",
            (unsigned long) addr,
            (unsigned long) size,
            name);
    fflush(perf_map_fp);
}

void
codegen_dump_compile_block(codeblock_t *block, ir_data_t *ir,
                           uint16_t prologue_end, uint16_t epilogue_pos,
                           const uint32_t *orig_uop_types, const uint32_t *orig_uop_imms,
                           const uint16_t *uop_host_starts, const uint16_t *uop_host_body_starts,
                           const uint16_t *uop_host_ends)
{
    if (!dump_active || !dump_fp || !block || !ir)
        return;

    block->compile_seq       = 0;
    block->exec_count        = 0;
    block->host_tsc_total    = 0;
    block->host_tsc_callouts = 0;

    /* Address filters */
    if (dump_pc_filter && (block->pc < dump_pc_start || block->pc > dump_pc_end))
        return;
    if (dump_phys_filter && (block->phys < dump_phys_start || block->phys > dump_phys_end))
        return;

    block->compile_seq = ++compile_seq_counter;

    uint16_t block_nr = (codeblock) ? (uint16_t) (block - codeblock) : 0;

    /* Calculate guest bytes length from instruction spans */
    uint16_t insn_count = block->ins;
    uint16_t guest_bytes_len = 0;
    uint8_t  resolved_lens[MAX_INSTRUCTION_COUNT];

    for (uint16_t i = 0; i < insn_count; i++) {
        uint8_t len = codegen_instructions[i].len;
        if (i + 1 < insn_count) {
            uint32_t next_pc = codegen_instructions[i + 1].pc;
            uint32_t cur_pc  = codegen_instructions[i].pc;
            if (next_pc > cur_pc && (next_pc - cur_pc) <= 15)
                len = (uint8_t) (next_pc - cur_pc);
        }
        if (len == 0 || len > 15)
            len = 1;
        resolved_lens[i] = len;
        guest_bytes_len += len;
    }
    if (guest_bytes_len == 0)
        guest_bytes_len = 1;

    /* Allocate temporary guest bytes buffer */
    uint8_t *guest_bytes = malloc(guest_bytes_len);
    if (!guest_bytes)
        return;

    uint16_t curr_off = 0;
    bdump_insn_span_t *spans = malloc(insn_count * sizeof(bdump_insn_span_t));
    if (!spans) {
        free(guest_bytes);
        return;
    }

    uint8_t seg_op32 = (block->status & CPU_STATUS_USE32) ? 2 : 0;
    for (uint16_t i = 0; i < insn_count; i++) {
        uint8_t len = resolved_lens[i];
        spans[i].guest_off = curr_off;
        spans[i].guest_len = len;
        spans[i].op_32     = seg_op32;

        uint32_t insn_lin = block->_cs + codegen_instructions[i].pc;
        for (uint8_t b = 0; b < len; b++) {
            guest_bytes[curr_off + b] = fastreadb(insn_lin + b);
        }
        curr_off += len;
    }

    /* Build uop entries */
    uint16_t uop_count = (uint16_t) ir->wr_pos;
    bdump_uop_entry_t *uops = malloc(uop_count * sizeof(bdump_uop_entry_t));
    if (!uops) {
        free(spans);
        free(guest_bytes);
        return;
    }

    for (uint16_t c = 0; c < uop_count; c++) {
        uop_t *uop = &ir->uops[c];
        uops[c].orig_type      = orig_uop_types ? orig_uop_types[c] : uop->type;
        uops[c].final_type     = uop->type;
        uops[c].imm_data       = orig_uop_imms ? orig_uop_imms[c] : (uint32_t) uop->imm_data;
        uops[c].func_ptr       = (uint64_t) (uintptr_t) uop->p;
        uops[c].dest_reg_a     = (uint16_t) uop->dest_reg_a.reg;
        uops[c].src_reg_a      = (uint16_t) uop->src_reg_a.reg;
        uops[c].src_reg_b      = (uint16_t) uop->src_reg_b.reg;
        uops[c].src_reg_c      = (uint16_t) uop->src_reg_c.reg;
        uops[c].dest_host_reg  = (int16_t) uop->dest_reg_a_real;
        uops[c].src_host_reg_a = (int16_t) uop->src_reg_a_real;
        uops[c].src_host_reg_b = (int16_t) uop->src_reg_b_real;
        uops[c].src_host_reg_c = (int16_t) uop->src_reg_c_real;
        uops[c].guest_insn_idx = uop->guest_insn_idx;
        uops[c].host_start_off = uop_host_starts ? uop_host_starts[c] : 0;
        uops[c].host_body_off  = uop_host_body_starts ? uop_host_body_starts[c] : 0;
        uops[c].host_end_off   = uop_host_ends ? uop_host_ends[c] : 0;
    }

    uint16_t host_len = (dump_cum_offset > 0xffff) ? 0xffff : (uint16_t) dump_cum_offset;
    const uint8_t *first_chunk_ptr = (dump_chunk_count > 0) ? dump_chunks[0].ptr : NULL;

    bdump_compile_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.rec_type         = BDUMP_REC_COMPILE;
    rec.exit_type        = block->exit_type;
    rec.block_nr         = block_nr;
    rec.compile_seq      = block->compile_seq;
    rec.pc               = block->pc;
    rec._cs              = block->_cs;
    rec.phys             = block->phys;
    rec.phys_2           = block->phys_2;
    rec.status           = block->status;
    rec.flags            = block->flags;
    rec.top              = block->TOP;
    rec.unroll_count     = (uint8_t) codegen_unroll_count;
    rec.guest_insn_count = insn_count;
    rec.guest_byte_len   = guest_bytes_len;
    rec.uop_count        = uop_count;
    rec.host_byte_len    = host_len;
    rec.prologue_end_off = prologue_end;
    rec.epilogue_pos_off = epilogue_pos;
    rec.host_code_addr   = (uint64_t) (uintptr_t) first_chunk_ptr;
    rec.page_mask        = block->page_mask;
    rec.page_mask2       = block->page_mask2;

    size_t total_size = sizeof(rec) + guest_bytes_len +
                        insn_count * sizeof(bdump_insn_span_t) +
                        uop_count * sizeof(bdump_uop_entry_t) +
                        host_len;

    if (dump_min_exec > 0 && buffered_blocks) {
        if (buffered_blocks[block_nr].data)
            free(buffered_blocks[block_nr].data);

        buffered_blocks[block_nr].data = malloc(total_size);
        if (buffered_blocks[block_nr].data) {
            buffered_blocks[block_nr].total_size = total_size;
            uint8_t *ptr = buffered_blocks[block_nr].data;

            memcpy(ptr, &rec, sizeof(rec));
            ptr += sizeof(rec);
            memcpy(ptr, guest_bytes, guest_bytes_len);
            ptr += guest_bytes_len;
            memcpy(ptr, spans, insn_count * sizeof(bdump_insn_span_t));
            ptr += insn_count * sizeof(bdump_insn_span_t);
            memcpy(ptr, uops, uop_count * sizeof(bdump_uop_entry_t));
            ptr += uop_count * sizeof(bdump_uop_entry_t);
            uint32_t copied = 0;
            for (int i = 0; i < dump_chunk_count && copied < host_len; i++) {
                uint32_t to_copy = dump_chunks[i].len;
                if (copied + to_copy > host_len)
                    to_copy = host_len - copied;
                memcpy(ptr + copied, dump_chunks[i].ptr, to_copy);
                copied += to_copy;
            }
        }
    } else {
        fwrite(&rec, sizeof(rec), 1, dump_fp);
        fwrite(guest_bytes, 1, guest_bytes_len, dump_fp);
        fwrite(spans, sizeof(bdump_insn_span_t), insn_count, dump_fp);
        fwrite(uops, sizeof(bdump_uop_entry_t), uop_count, dump_fp);
        uint32_t written = 0;
        for (int i = 0; i < dump_chunk_count && written < host_len; i++) {
            uint32_t to_write = dump_chunks[i].len;
            if (written + to_write > host_len)
                to_write = host_len - written;
            fwrite(dump_chunks[i].ptr, 1, to_write, dump_fp);
            written += to_write;
        }
        fflush(dump_fp);
    }

    free(uops);
    free(spans);
    free(guest_bytes);
}

void
codegen_dump_exec_block(codeblock_t *block, uint8_t invalidate_reason)
{
    if (!dump_active || !dump_fp || !block || block->compile_seq == 0)
        return;

    uint16_t block_nr = (codeblock) ? (uint16_t) (block - codeblock) : 0;

    if (dump_min_exec > 0 && buffered_blocks) {
        if (!buffered_blocks[block_nr].data) {
            block->compile_seq = 0;
            return;
        }

        if (block->exec_count >= dump_min_exec) {
            fwrite(buffered_blocks[block_nr].data, 1, buffered_blocks[block_nr].total_size, dump_fp);

            bdump_exec_record_t exec_rec;
            memset(&exec_rec, 0, sizeof(exec_rec));
            exec_rec.rec_type          = BDUMP_REC_EXEC;
            exec_rec.invalidate_reason = invalidate_reason;
            exec_rec.block_nr          = block_nr;
            exec_rec.compile_seq       = block->compile_seq;
            exec_rec.exec_count        = block->exec_count;
            exec_rec.host_tsc_total    = block->host_tsc_total;
            exec_rec.host_tsc_callouts = block->host_tsc_callouts;
            exec_rec.link0_target_pc   = block->links[0].target_pc;
            exec_rec.link1_target_pc   = block->links[1].target_pc;
            exec_rec.link0_target_blk  = block->links[0].target_block;
            exec_rec.link1_target_blk  = block->links[1].target_block;

            fwrite(&exec_rec, sizeof(exec_rec), 1, dump_fp);
            fflush(dump_fp);
        }

        free(buffered_blocks[block_nr].data);
        buffered_blocks[block_nr].data       = NULL;
        buffered_blocks[block_nr].total_size = 0;
        block->compile_seq                   = 0;
    } else {
        bdump_exec_record_t exec_rec;
        memset(&exec_rec, 0, sizeof(exec_rec));
        exec_rec.rec_type          = BDUMP_REC_EXEC;
        exec_rec.invalidate_reason = invalidate_reason;
        exec_rec.block_nr          = block_nr;
        exec_rec.compile_seq       = block->compile_seq;
        exec_rec.exec_count        = block->exec_count;
        exec_rec.host_tsc_total    = block->host_tsc_total;
        exec_rec.host_tsc_callouts = block->host_tsc_callouts;
        exec_rec.link0_target_pc   = block->links[0].target_pc;
        exec_rec.link1_target_pc   = block->links[1].target_pc;
        exec_rec.link0_target_blk  = block->links[0].target_block;
        exec_rec.link1_target_blk  = block->links[1].target_block;

        fwrite(&exec_rec, sizeof(exec_rec), 1, dump_fp);
        fflush(dump_fp);
        block->compile_seq = 0;
    }
}

static uint16_t
count_seen_blocks(void)
{
    uint16_t count = 0;
    for (size_t i = 0; i < sizeof(hang_seen_bitmap) / sizeof(hang_seen_bitmap[0]); i++) {
        if (hang_seen_bitmap[i])
            count += (uint16_t) __builtin_popcountll(hang_seen_bitmap[i]);
    }
    return count;
}

static void
codegen_trigger_hang(uint8_t reason, codeblock_t *trigger_blk, int from_signal)
{
    if (hang_triggered)
        return;
    hang_triggered = 1;

    uint16_t trig_nr  = (trigger_blk && codeblock) ? (uint16_t) (trigger_blk - codeblock) : 0;
    uint32_t trig_seq = trigger_blk ? trigger_blk->compile_seq : 0;
    uint16_t cur_uniq = count_seen_blocks();
    if (cur_uniq == 0 && hang_last_unique_blocks > 0)
        cur_uniq = hang_last_unique_blocks;

    uint32_t trace_count = (flight_recorder_total < FLIGHT_RECORDER_SIZE)
                               ? (uint32_t) flight_recorder_total
                               : FLIGHT_RECORDER_SIZE;
    bdump_trace_entry_t ordered_trace[FLIGHT_RECORDER_SIZE];
    uint64_t start_idx = (flight_recorder_total >= FLIGHT_RECORDER_SIZE)
                             ? (flight_recorder_total - FLIGHT_RECORDER_SIZE)
                             : 0;
    for (uint32_t i = 0; i < trace_count; i++) {
        ordered_trace[i] = flight_recorder[(start_idx + i) & (FLIGHT_RECORDER_SIZE - 1)];
    }

    const char *reason_str = "UNKNOWN";
    if (reason == BDUMP_HANG_CHAIN_LOCKUP)
        reason_str = "HARD CHAINED-LOOP LOCKUP (never returned to C dispatcher)";
    else if (reason == BDUMP_HANG_SPIN_LOOP)
        reason_str = "TIGHT GUEST SPIN LOOP (stuck in <= 32 unique blocks)";
    else if (reason == BDUMP_HANG_SIGNAL_ABORT)
        reason_str = "USER SIGNAL INTERRUPT (SIGINT/SIGTERM)";

    fprintf(stderr, "\n================================================================================\n");
    fprintf(stderr, "[86BOX DYNAREC HANG DETECTOR] %s\n", reason_str);
    fprintf(stderr, "================================================================================\n");
    if (trigger_blk) {
        fprintf(stderr, "Trigger Block : #%u (seq %u) | PC=%04x:%08x (phys %08x) | exec_count=%" PRIu64 "\n",
                trig_nr, trig_seq,
                trigger_blk->_cs >> 4, trigger_blk->pc - trigger_blk->_cs,
                trigger_blk->phys, trigger_blk->exec_count);
    }
    fprintf(stderr, "Spin Metrics  : spin_iterations=%" PRIu64 " | chained_streak=%" PRIu64 " | unique_blocks_in_window=%u\n",
            hang_spin_iterations + hang_window_entries, hang_chained_streak, cur_uniq);
    fprintf(stderr, "Guest Regs    : EAX=%08x ECX=%08x EDX=%08x EBX=%08x\n",
            cpu_state.regs[0].l, cpu_state.regs[1].l, cpu_state.regs[2].l, cpu_state.regs[3].l);
    fprintf(stderr, "                ESP=%08x EBP=%08x ESI=%08x EDI=%08x\n",
            cpu_state.regs[4].l, cpu_state.regs[5].l, cpu_state.regs[6].l, cpu_state.regs[7].l);
    fprintf(stderr, "Flags / Cyc   : flags_op=%u flags_res=%08x flags_op1=%08x flags_op2=%08x cycles=%d\n",
            cpu_state.flags_op, cpu_state.flags_res, cpu_state.flags_op1, cpu_state.flags_op2, cpu_state._cycles);

    if (trace_count > 0) {
        uint32_t show = (trace_count > 16) ? 16 : trace_count;
        fprintf(stderr, "\nLast %u Block Transitions (Flight Recorder):\n", show);
        for (uint32_t i = trace_count - show; i < trace_count; i++) {
            const bdump_trace_entry_t *e = &ordered_trace[i];
            fprintf(stderr, "  [-%02u] blk #%04u (seq %u) lin=%08x phys=%08x | EAX=%08x ECX=%08x EDX=%08x flg_op=%02u flg_res=%08x cyc=%d\n",
                    trace_count - 1 - i, e->tr_block_nr, e->tr_compile_seq, e->tr_pc, e->tr_phys,
                    e->tr_eax, e->tr_ecx, e->tr_edx, e->tr_flags_op, e->tr_flags_res, e->tr_cycles);
        }
    }
    fprintf(stderr, "================================================================================\n");
    fflush(stderr);

    if (dump_active && dump_fp) {
        /* Force-flush any buffered blocks that participated in the flight recorder even if < dump_min_exec */
        if (dump_min_exec > 0 && buffered_blocks && codeblock) {
            for (uint32_t i = 0; i < trace_count; i++) {
                uint16_t bnr = ordered_trace[i].tr_block_nr;
                if (bnr < BLOCK_SIZE && codeblock[bnr].valid && codeblock[bnr].exec_count < dump_min_exec) {
                    codeblock[bnr].exec_count = dump_min_exec;
                }
            }
        }

        /* Flush all live blocks so the .86bdump has their final exec_count and link topology */
        if (codeblock) {
            for (uint32_t c = 0; c < BLOCK_SIZE; c++) {
                if (codeblock[c].valid && (codeblock[c].flags & CODEBLOCK_WAS_RECOMPILED)) {
                    codegen_dump_exec_block(&codeblock[c], 0);
                }
            }
        }

        bdump_hang_record_t hrec;
        memset(&hrec, 0, sizeof(hrec));
        hrec.rec_type                = BDUMP_REC_HANG;
        hrec.hang_reason             = reason;
        hrec.unique_blocks_in_window = cur_uniq;
        hrec.trace_count             = trace_count;
        hrec.spin_iterations         = hang_spin_iterations + hang_window_entries;
        hrec.chained_streak          = hang_chained_streak;
        hrec.trigger_compile_seq     = trig_seq;
        hrec.trigger_block_nr        = trig_nr;

        fwrite(&hrec, sizeof(hrec), 1, dump_fp);
        if (trace_count > 0) {
            fwrite(ordered_trace, sizeof(bdump_trace_entry_t), trace_count, dump_fp);
        }
        fflush(dump_fp);
        codegen_dump_close();
    }

    if (from_signal)
        _exit(130);
    else
        exit(1);
}

static void
codegen_hang_sig_handler(int sig)
{
    (void) sig;
    codegen_trigger_hang(BDUMP_HANG_SIGNAL_ABORT, codegen_current_executing_block, 1);
}

void
codegen_dump_enter_block(codeblock_t *block)
{
    if (!block)
        return;

    int is_chained = (codegen_current_executing_block != NULL);

    if (codegen_profile_active) {
        uint64_t now = host_rdtsc();
        if (codegen_current_executing_block) {
            uint64_t elapsed  = now - codegen_last_block_entry_tsc;
            uint64_t callouts = codegen_current_executing_block->host_tsc_callouts - codegen_last_block_callouts;
            codegen_current_executing_block->host_tsc_total += elapsed;
            if (elapsed > callouts)
                codegen_host_profile.tsc_jit_pure += (elapsed - callouts);
            codegen_host_profile.count_chained_transfers++;
        }
        block->exec_count++;
        codegen_current_executing_block = block;
        codegen_last_block_entry_tsc    = host_rdtsc();
        codegen_last_block_callouts     = block->host_tsc_callouts;
    } else {
        if (codegen_current_executing_block)
            codegen_host_profile.count_chained_transfers++;
        block->exec_count++;
        codegen_current_executing_block = block;
    }

    uint16_t block_nr = (codeblock) ? (uint16_t) (block - codeblock) : 0;

    if (flight_recorder_enabled) {
        bdump_trace_entry_t *e = &flight_recorder[flight_recorder_total & (FLIGHT_RECORDER_SIZE - 1)];
        flight_recorder_total++;
        e->tr_compile_seq = block->compile_seq;
        e->tr_block_nr    = block_nr;
        e->tr_flags_op    = (uint16_t) cpu_state.flags_op;
        e->tr_pc          = block->pc;
        e->tr_phys        = block->phys;
        e->tr_eax         = cpu_state.regs[0].l;
        e->tr_ecx         = cpu_state.regs[1].l;
        e->tr_edx         = cpu_state.regs[2].l;
        e->tr_ebx         = cpu_state.regs[3].l;
        e->tr_esp         = cpu_state.regs[4].l;
        e->tr_ebp         = cpu_state.regs[5].l;
        e->tr_esi         = cpu_state.regs[6].l;
        e->tr_edi         = cpu_state.regs[7].l;
        e->tr_cycles      = cpu_state._cycles;
        e->tr_flags_res   = cpu_state.flags_res;
    }

    if (codegen_hang_detect_active) {
        if (is_chained) {
            hang_chained_streak++;
            if (hang_chained_streak >= hang_chain_limit) {
                codegen_trigger_hang(BDUMP_HANG_CHAIN_LOCKUP, block, 0);
            }
        }

        if (block_nr < BLOCK_SIZE) {
            hang_seen_bitmap[block_nr >> 6] |= (1ULL << (block_nr & 63));
        }
        hang_window_entries++;

        uint64_t check_interval = (hang_spin_limit < 262144ULL) ? hang_spin_limit : 262144ULL;
        if (hang_window_entries >= check_interval) {
            uint16_t uniq = count_seen_blocks();
            hang_last_unique_blocks = uniq;
            if (uniq <= 32) {
                hang_spin_iterations += hang_window_entries;
                hang_window_entries   = 0;
                if (hang_spin_iterations >= hang_spin_limit) {
                    codegen_trigger_hang(BDUMP_HANG_SPIN_LOOP, block, 0);
                }
            } else {
                hang_spin_iterations = 0;
                hang_window_entries  = 0;
            }
            memset(hang_seen_bitmap, 0, sizeof(hang_seen_bitmap));
        }
    }
}

void
codegen_profile_exit_chain(void)
{
    if (codegen_profile_active && codegen_current_executing_block) {
        uint64_t now      = host_rdtsc();
        uint64_t elapsed  = now - codegen_last_block_entry_tsc;
        uint64_t callouts = codegen_current_executing_block->host_tsc_callouts - codegen_last_block_callouts;
        codegen_current_executing_block->host_tsc_total += elapsed;
        if (elapsed > callouts)
            codegen_host_profile.tsc_jit_pure += (elapsed - callouts);
    }
    codegen_current_executing_block = NULL;
    hang_chained_streak             = 0;
}

uint8_t
codegen_profile_readmembl(uint32_t a)
{
    uint64_t t0 = host_rdtsc();
    uint8_t res = readmembl(a);
    uint64_t elapsed = host_rdtsc() - t0;
    codegen_host_profile.tsc_c_callout_mem_io += elapsed;
    codegen_host_profile.count_mem_slow_loads++;
    if (codegen_current_executing_block)
        codegen_current_executing_block->host_tsc_callouts += elapsed;
    return res;
}

uint16_t
codegen_profile_readmemwl(uint32_t a)
{
    uint64_t t0 = host_rdtsc();
    uint16_t res = readmemwl(a);
    uint64_t elapsed = host_rdtsc() - t0;
    codegen_host_profile.tsc_c_callout_mem_io += elapsed;
    codegen_host_profile.count_mem_slow_loads++;
    if (codegen_current_executing_block)
        codegen_current_executing_block->host_tsc_callouts += elapsed;
    return res;
}

uint32_t
codegen_profile_readmemll(uint32_t a)
{
    uint64_t t0 = host_rdtsc();
    uint32_t res = readmemll(a);
    uint64_t elapsed = host_rdtsc() - t0;
    codegen_host_profile.tsc_c_callout_mem_io += elapsed;
    codegen_host_profile.count_mem_slow_loads++;
    if (codegen_current_executing_block)
        codegen_current_executing_block->host_tsc_callouts += elapsed;
    return res;
}

uint64_t
codegen_profile_readmemql(uint32_t a)
{
    uint64_t t0 = host_rdtsc();
    uint64_t res = readmemql(a);
    uint64_t elapsed = host_rdtsc() - t0;
    codegen_host_profile.tsc_c_callout_mem_io += elapsed;
    codegen_host_profile.count_mem_slow_loads++;
    if (codegen_current_executing_block)
        codegen_current_executing_block->host_tsc_callouts += elapsed;
    return res;
}

void
codegen_profile_writemembl(uint32_t a, uint8_t v)
{
    uint64_t t0 = host_rdtsc();
    writemembl(a, v);
    uint64_t elapsed = host_rdtsc() - t0;
    codegen_host_profile.tsc_c_callout_mem_io += elapsed;
    codegen_host_profile.count_mem_slow_stores++;
    if (codegen_current_executing_block)
        codegen_current_executing_block->host_tsc_callouts += elapsed;
}

void
codegen_profile_writememwl(uint32_t a, uint16_t v)
{
    uint64_t t0 = host_rdtsc();
    writememwl(a, v);
    uint64_t elapsed = host_rdtsc() - t0;
    codegen_host_profile.tsc_c_callout_mem_io += elapsed;
    codegen_host_profile.count_mem_slow_stores++;
    if (codegen_current_executing_block)
        codegen_current_executing_block->host_tsc_callouts += elapsed;
}

void
codegen_profile_writememll(uint32_t a, uint32_t v)
{
    uint64_t t0 = host_rdtsc();
    writememll(a, v);
    uint64_t elapsed = host_rdtsc() - t0;
    codegen_host_profile.tsc_c_callout_mem_io += elapsed;
    codegen_host_profile.count_mem_slow_stores++;
    if (codegen_current_executing_block)
        codegen_current_executing_block->host_tsc_callouts += elapsed;
}

void
codegen_profile_writememql(uint32_t a, uint64_t v)
{
    uint64_t t0 = host_rdtsc();
    writememql(a, v);
    uint64_t elapsed = host_rdtsc() - t0;
    codegen_host_profile.tsc_c_callout_mem_io += elapsed;
    codegen_host_profile.count_mem_slow_stores++;
    if (codegen_current_executing_block)
        codegen_current_executing_block->host_tsc_callouts += elapsed;
}

int
codegen_profile_call_instruction_func(uint32_t fetchdat, int (*fn)(uint32_t))
{
    uint64_t t0 = host_rdtsc();
    int ret = fn(fetchdat);
    uint64_t elapsed = host_rdtsc() - t0;
    codegen_host_profile.tsc_c_callout_insn_fn += elapsed;
    if (codegen_current_executing_block)
        codegen_current_executing_block->host_tsc_callouts += elapsed;
    return ret;
}

void
codegen_profile_record_helper_tsc(uint64_t elapsed)
{
    codegen_host_profile.tsc_c_callout_helper += elapsed;
    if (codegen_current_executing_block)
        codegen_current_executing_block->host_tsc_callouts += elapsed;
}

void
codegen_dump_record_rt_sample(int speed_percent, int fps, uint32_t elapsed_ms)
{
    rt_sample_cumulative_ms += elapsed_ms;

    if (codegen_host_profile.rt_sample_count == 0) {
        codegen_host_profile.rt_speed_min = (uint32_t) (speed_percent < 0 ? 0 : speed_percent);
        codegen_host_profile.rt_speed_max = (uint32_t) (speed_percent < 0 ? 0 : speed_percent);
    } else {
        if ((uint32_t) speed_percent < codegen_host_profile.rt_speed_min)
            codegen_host_profile.rt_speed_min = (uint32_t) (speed_percent < 0 ? 0 : speed_percent);
        if ((uint32_t) speed_percent > codegen_host_profile.rt_speed_max)
            codegen_host_profile.rt_speed_max = (uint32_t) (speed_percent < 0 ? 0 : speed_percent);
    }

    codegen_host_profile.rt_sample_count++;
    codegen_host_profile.rt_speed_sum += (uint64_t) (speed_percent < 0 ? 0 : speed_percent);
    if (speed_percent < 100)
        codegen_host_profile.rt_samples_below_100++;

    if (dump_active || codegen_profile_active) {
        if (codegen_host_profile.rt_series_len >= rt_sample_capacity) {
            uint32_t new_cap = rt_sample_capacity ? (rt_sample_capacity * 2) : 128;
            bdump_rt_sample_t *new_buf = realloc(rt_sample_series, new_cap * sizeof(bdump_rt_sample_t));
            if (new_buf) {
                rt_sample_series = new_buf;
                rt_sample_capacity = new_cap;
            }
        }
        if (rt_sample_series && codegen_host_profile.rt_series_len < rt_sample_capacity) {
            bdump_rt_sample_t *s = &rt_sample_series[codegen_host_profile.rt_series_len++];
            s->elapsed_ms    = rt_sample_cumulative_ms;
            s->speed_percent = (uint16_t) (speed_percent < 0 ? 0 : (speed_percent > 65535 ? 65535 : speed_percent));
            s->fps           = (uint16_t) (fps < 0 ? 0 : (fps > 65535 ? 65535 : fps));
        }
    }
}

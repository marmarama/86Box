#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>

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

bdump_host_profile_t codegen_host_profile         = {0};
int                  codegen_profile_active       = 0;
codeblock_t         *codegen_current_executing_block = NULL;

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

    if (profile_env && (strcmp(profile_env, "1") == 0 || strcasecmp(profile_env, "true") == 0)) {
        codegen_profile_active = 1;
        pclog("codegen_dump: host TSC profiling enabled (BOX86_DUMP_PROFILE=1)\n");
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
        }
    }

    if (dump_active || perf_map_active)
        atexit(codegen_dump_close);
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

        if (codegen_profile_active) {
            codegen_host_profile.rec_type = BDUMP_REC_HOST_PROFILE;
            fwrite(&codegen_host_profile, sizeof(codegen_host_profile), 1, dump_fp);
            fflush(dump_fp);
        }

        fclose(dump_fp);
        dump_fp     = NULL;
        dump_active = 0;
    }

    if (perf_map_active && perf_map_fp) {
        fclose(perf_map_fp);
        perf_map_fp     = NULL;
        perf_map_active = 0;
    }
}

void
codegen_dump_perf_map_block(codeblock_t *block, const uint8_t *host_code, uint16_t host_len)
{
    if (!perf_map_fp || !block)
        return;

    uint16_t block_nr = (codeblock) ? (uint16_t) (block - codeblock) : 0;
    fprintf(perf_map_fp, "%lx %x jit_b%04u_%04x:%08x_p%08x\n",
            (unsigned long) (uintptr_t) host_code,
            (unsigned int) host_len,
            block_nr,
            block->_cs >> 4,
            block->pc - block->_cs,
            block->phys);
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
                           uint16_t host_len, const uint8_t *host_code,
                           const uint32_t *orig_uop_types, const uint32_t *orig_uop_imms,
                           const uint16_t *uop_host_starts, const uint16_t *uop_host_body_starts,
                           const uint16_t *uop_host_ends)
{
    if (!dump_active || !dump_fp || !block || !ir)
        return;

    /* Address filters */
    if (dump_pc_filter && (block->pc < dump_pc_start || block->pc > dump_pc_end))
        return;
    if (dump_phys_filter && (block->phys < dump_phys_start || block->phys > dump_phys_end))
        return;

    block->compile_seq = ++compile_seq_counter;
    block->exec_count  = 0;

    uint16_t block_nr = (codeblock) ? (uint16_t) (block - codeblock) : 0;

    /* Calculate guest bytes length from instruction spans */
    uint16_t insn_count = block->ins;
    uint16_t guest_bytes_len = 0;
    if (insn_count > 0) {
        for (uint16_t i = 0; i < insn_count; i++) {
            uint8_t len = codegen_instructions[i].len;
            if (len == 0)
                len = 1;
            guest_bytes_len += len;
        }
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

    for (uint16_t i = 0; i < insn_count; i++) {
        uint8_t len = codegen_instructions[i].len;
        if (len == 0)
            len = 1;
        spans[i].guest_off = curr_off;
        spans[i].guest_len = len;
        spans[i].op_32     = (uint8_t) (codegen_instructions[i].op_32 & 0x300);

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
    rec.host_code_addr   = (uint64_t) (uintptr_t) host_code;
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
            memcpy(ptr, host_code, host_len);
        }
    } else {
        fwrite(&rec, sizeof(rec), 1, dump_fp);
        fwrite(guest_bytes, 1, guest_bytes_len, dump_fp);
        fwrite(spans, sizeof(bdump_insn_span_t), insn_count, dump_fp);
        fwrite(uops, sizeof(bdump_uop_entry_t), uop_count, dump_fp);
        fwrite(host_code, 1, host_len, dump_fp);
        fflush(dump_fp);
    }

    free(uops);
    free(spans);
    free(guest_bytes);
}

void
codegen_dump_exec_block(codeblock_t *block, uint8_t invalidate_reason)
{
    if (!dump_active || !dump_fp || !block)
        return;

    uint16_t block_nr = (codeblock) ? (uint16_t) (block - codeblock) : 0;

    if (dump_min_exec > 0 && buffered_blocks) {
        if (!buffered_blocks[block_nr].data)
            return;

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
    }
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

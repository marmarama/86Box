#if defined __amd64__ || defined _M_X64

#    include <inttypes.h>
#    include <stdlib.h>
#    include <stdint.h>
#    include <86box/86box.h>
#    include "cpu.h"
#    include <86box/mem.h>
#    include <86box/plat.h>

#    include "codegen.h"
#    include "codegen_allocator.h"
#    include "codegen_backend.h"
#    include "codegen_backend_x86-64_defs.h"
#    include "codegen_backend_x86-64_ops.h"
#    include "codegen_backend_x86-64_ops_helpers.h"
#    include "codegen_backend_x86-64_ops_sse.h"
#    include "codegen_dump.h"
#    include "codegen_reg.h"
#    include "x86.h"
#    include "x86seg_common.h"
#    include "x86seg.h"

#    if defined(__linux__) || defined(__APPLE__)
#        include <sys/mman.h>
#        include <unistd.h>
#    endif
#    if defined WIN32 || defined _WIN32 || defined _WIN32
#        include <windows.h>
#    endif
#    include <string.h>

void *codegen_mem_load_byte;
void *codegen_mem_load_word;
void *codegen_mem_load_long;
void *codegen_mem_load_quad;
void *codegen_mem_load_single;
void *codegen_mem_load_double;

void *codegen_mem_store_byte;
void *codegen_mem_store_word;
void *codegen_mem_store_long;
void *codegen_mem_store_quad;
void *codegen_mem_store_single;
void *codegen_mem_store_double;

void *codegen_gpf_rout;
void *codegen_exit_rout;
void *codegen_event_stub;

host_reg_def_t codegen_host_reg_list[CODEGEN_HOST_REGS] = {
    /* Note: while EAX, EDX, R10, R11 are normally volatile registers under x86
       calling conventions, the recompiler will explicitly save and restore
       them across memory helper calls on the slow path.
       EBX, R13 are callee-saved and preserved across calls by ABI.
       R14 is pinned permanently across blocks to hold cpu_state._cycles.
       R15 is pinned permanently across blocks to hold guest ESP. */
    { REG_EAX, 0 },
    { REG_EBX, 0 },
    { REG_EDX, 0 },
    { REG_R10, 0 },
    { REG_R11, 0 },
    { REG_R13, 0 }
};

/* The Windows x64 frame: 0x38 bytes of block temporaries as before, then
   XMM6 and XMM7 as the caller left them. 0x58 keeps RSP 16-byte aligned
   after the eight pushes and the return address. */
#define CODEGEN_WIN64_FRAME 0x58
#define CODEGEN_XMM6_SAVE   0x38
#define CODEGEN_XMM7_SAVE   0x48

host_reg_def_t codegen_host_fp_reg_list[CODEGEN_HOST_FP_REGS] = {
#    if _WIN64
  /*Windows x86-64 calling convention preserves XMM6-XMM15*/
    {REG_XMM6,  0                     },
    { REG_XMM7, 0                     },
#    else
    /*System V AMD64 calling convention does not preserve any XMM registers*/
    { REG_XMM6, HOST_REG_FLAG_VOLATILE },
    { REG_XMM7, HOST_REG_FLAG_VOLATILE },
#    endif
    { REG_XMM1, HOST_REG_FLAG_VOLATILE},
    { REG_XMM2, HOST_REG_FLAG_VOLATILE},
    { REG_XMM3, HOST_REG_FLAG_VOLATILE},
    { REG_XMM4, HOST_REG_FLAG_VOLATILE},
    { REG_XMM5, HOST_REG_FLAG_VOLATILE}
};

static void
build_load_routine(codeblock_t *block, int size, int is_float)
{
    uint8_t *branch_offset;
    uint8_t *misaligned_offset = NULL;

    /*In - ESI = address
      Out - ECX = data, ESI = abrt*/
    /*MOV ECX, ESI
      SHR ESI, 12
      MOV RSI, [readlookup2+ESI*4]
      CMP ESI, -1
      JNZ +
      MOVZX ECX, B[RSI+RCX]
      XOR ESI,ESI
      RET
    * PUSH EAX
      PUSH EDX
      PUSH ECX
      CALL readmembl
      POP ECX
      POP EDX
      POP EAX
      MOVZX ECX, AL
      RET
    */
    host_x86_MOV32_REG_REG(block, REG_ECX, REG_ESI);
    host_x86_SHR32_IMM(block, REG_ESI, 12);
    host_x86_MOV64_REG_BASE_INDEX_SHIFT(block, REG_RSI, REG_R12, REG_RSI, 3);
    if (size != 1) {
        host_x86_TEST32_REG_IMM(block, REG_ECX, size - 1);
        misaligned_offset = host_x86_JNZ_short(block);
    }
    host_x86_CMP64_REG_IMM(block, REG_RSI, (uint32_t) -1);
    branch_offset = host_x86_JZ_short(block);
    if (size == 1 && !is_float)
        host_x86_MOVZX_BASE_INDEX_32_8(block, REG_ECX, REG_RSI, REG_RCX);
    else if (size == 2 && !is_float)
        host_x86_MOVZX_BASE_INDEX_32_16(block, REG_ECX, REG_RSI, REG_RCX);
    else if (size == 4 && !is_float)
        host_x86_MOV32_REG_BASE_INDEX(block, REG_ECX, REG_RSI, REG_RCX);
    else if (size == 4 && is_float)
        host_x86_CVTSS2SD_XREG_BASE_INDEX(block, REG_XMM_TEMP, REG_RSI, REG_RCX);
    else if (size == 8)
        host_x86_MOVQ_XREG_BASE_INDEX(block, REG_XMM_TEMP, REG_RSI, REG_RCX);
    else
        fatal("build_load_routine: size=%i\n", size);
    host_x86_XOR32_REG_REG(block, REG_ESI, REG_ESI);
    host_x86_RET(block);

    *branch_offset = (uint8_t) ((uintptr_t) &block_write_data[block_pos] - (uintptr_t) branch_offset) - 1;
    if (size != 1)
        *misaligned_offset = (uint8_t) ((uintptr_t) &block_write_data[block_pos] - (uintptr_t) misaligned_offset) - 1;
    host_x86_PUSH(block, REG_RAX);
    host_x86_PUSH(block, REG_RDX);
    host_x86_PUSH(block, REG_R10);
    host_x86_PUSH(block, REG_R11);
    int cycles_disp = (int) offsetof(cpu_state_t, _cycles) - 128;
    host_x86_MOV32_BASE_OFFSET_REG(block, REG_RBP, cycles_disp, REG_R14);
#    if _WIN64
    host_x86_SUB64_REG_IMM(block, REG_RSP, 0x28);
#    else
    /* Align RSP to 16: entry RSP%16=8 (after CALL from JIT block), four PUSHes
       (RAX, RDX, R10, R11) subtract 32 (multiple of 16), so RSP%16 is still 8;
       subtract 8 more to satisfy the SysV ABI before calling C. */
    host_x86_SUB64_REG_IMM(block, REG_RSP, 0x8);
    host_x86_MOV32_REG_REG(block, REG_EDI, REG_ECX);
#    endif
    if (size == 1 && !is_float) {
        host_x86_CALL(block, codegen_profile_active ? (void *) codegen_profile_readmembl : (void *) readmembl);
        host_x86_MOVZX_REG_32_8(block, REG_ECX, REG_EAX);
    } else if (size == 2 && !is_float) {
        host_x86_CALL(block, codegen_profile_active ? (void *) codegen_profile_readmemwl : (void *) readmemwl);
        host_x86_MOVZX_REG_32_16(block, REG_ECX, REG_EAX);
    } else if (size == 4 && !is_float) {
        host_x86_CALL(block, codegen_profile_active ? (void *) codegen_profile_readmemll : (void *) readmemll);
        host_x86_MOV32_REG_REG(block, REG_ECX, REG_EAX);
    } else if (size == 4 && is_float) {
        host_x86_CALL(block, codegen_profile_active ? (void *) codegen_profile_readmemll : (void *) readmemll);
        host_x86_MOVD_XREG_REG(block, REG_XMM_TEMP, REG_EAX);
        host_x86_CVTSS2SD_XREG_XREG(block, REG_XMM_TEMP, REG_XMM_TEMP);
    } else if (size == 8) {
        host_x86_CALL(block, codegen_profile_active ? (void *) codegen_profile_readmemql : (void *) readmemql);
        host_x86_MOVQ_XREG_REG(block, REG_XMM_TEMP, REG_RAX);
    }
    host_x86_MOV32_REG_BASE_OFFSET(block, REG_R14, REG_RBP, cycles_disp);
#    if _WIN64
    host_x86_ADD64_REG_IMM(block, REG_RSP, 0x28);
#    else
    host_x86_ADD64_REG_IMM(block, REG_RSP, 0x8);
#    endif
    host_x86_POP(block, REG_R11);
    host_x86_POP(block, REG_R10);
    host_x86_POP(block, REG_RDX);
    host_x86_POP(block, REG_RAX);
    host_x86_MOVZX_REG_ABS_32_8(block, REG_ESI, &cpu_state.abrt);
    host_x86_RET(block);
}

static void
build_store_routine(codeblock_t *block, int size, int is_float)
{
    uint8_t *branch_offset;
    uint8_t *misaligned_offset = NULL;

    /*In - ECX = data, ESI = address
      Out - ESI = abrt
      Corrupts EDI*/
    /*MOV EDI, ESI
      SHR ESI, 12
      MOV ESI, [writelookup2+ESI*4]
      CMP ESI, -1
      JNZ +
      MOV [RSI+RDI], ECX
      XOR ESI,ESI
      RET
    * PUSH EAX
      PUSH EDX
      PUSH ECX
      CALL writemembl
      POP ECX
      POP EDX
      POP EAX
      MOVZX ECX, AL
      RET
    */
    intptr_t write_offset = (intptr_t) writelookup2 - (intptr_t) readlookup2;
    host_x86_MOV32_REG_REG(block, REG_EDI, REG_ESI);
    host_x86_SHR32_IMM(block, REG_ESI, 12);
    if (write_offset >= INT32_MIN && write_offset <= INT32_MAX) {
        host_x86_MOV64_REG_BASE_INDEX_SHIFT_OFFSET(block, REG_RSI, REG_R12, REG_RSI, 3, (int32_t) write_offset);
    } else {
        host_x86_MOV64_REG_IMM(block, REG_R8, (uint64_t) (uintptr_t) writelookup2);
        host_x86_MOV64_REG_BASE_INDEX_SHIFT(block, REG_RSI, REG_R8, REG_RSI, 3);
    }
    if (size != 1) {
        host_x86_TEST32_REG_IMM(block, REG_EDI, size - 1);
        misaligned_offset = host_x86_JNZ_short(block);
    }
    host_x86_CMP64_REG_IMM(block, REG_RSI, (uint32_t) -1);
    branch_offset = host_x86_JZ_short(block);
    if (size == 1 && !is_float)
        host_x86_MOV8_BASE_INDEX_REG(block, REG_RSI, REG_RDI, REG_ECX);
    else if (size == 2 && !is_float)
        host_x86_MOV16_BASE_INDEX_REG(block, REG_RSI, REG_RDI, REG_ECX);
    else if (size == 4 && !is_float)
        host_x86_MOV32_BASE_INDEX_REG(block, REG_RSI, REG_RDI, REG_ECX);
    else if (size == 4 && is_float)
        host_x86_MOVD_BASE_INDEX_XREG(block, REG_RSI, REG_RDI, REG_XMM_TEMP);
    else if (size == 8)
        host_x86_MOVQ_BASE_INDEX_XREG(block, REG_RSI, REG_RDI, REG_XMM_TEMP);
    else
        fatal("build_store_routine: size=%i\n", size);
    host_x86_XOR32_REG_REG(block, REG_ESI, REG_ESI);
    host_x86_RET(block);

    *branch_offset = (uint8_t) ((uintptr_t) &block_write_data[block_pos] - (uintptr_t) branch_offset) - 1;
    if (size != 1)
        *misaligned_offset = (uint8_t) ((uintptr_t) &block_write_data[block_pos] - (uintptr_t) misaligned_offset) - 1;
    host_x86_PUSH(block, REG_RAX);
    host_x86_PUSH(block, REG_RDX);
    host_x86_PUSH(block, REG_R10);
    host_x86_PUSH(block, REG_R11);
    int cycles_disp = (int) offsetof(cpu_state_t, _cycles) - 128;
    host_x86_MOV32_BASE_OFFSET_REG(block, REG_RBP, cycles_disp, REG_R14);
#    if _WIN64
    host_x86_SUB64_REG_IMM(block, REG_RSP, 0x28);
    if (size == 4 && is_float)
        host_x86_MOVD_REG_XREG(block, REG_EDX, REG_XMM_TEMP); // data
    else if (size == 8)
        host_x86_MOVQ_REG_XREG(block, REG_RDX, REG_XMM_TEMP); // data
    else
        host_x86_MOV32_REG_REG(block, REG_EDX, REG_ECX); // data
    host_x86_MOV32_REG_REG(block, REG_ECX, REG_EDI);     // address
#    else
    /* Align RSP to 16: entry RSP%16=8 (after CALL from JIT block), four PUSHes
       (RAX, RDX, R10, R11) subtract 32 (multiple of 16), so RSP%16 is still 8;
       subtract 8 more to satisfy the SysV ABI before calling C. */
    host_x86_SUB64_REG_IMM(block, REG_RSP, 0x8);
    if (size == 4 && is_float)
        host_x86_MOVD_REG_XREG(block, REG_ESI, REG_XMM_TEMP); // data
    else if (size == 8)
        host_x86_MOVQ_REG_XREG(block, REG_RSI, REG_XMM_TEMP); // data
    else
        host_x86_MOV32_REG_REG(block, REG_ESI, REG_ECX); // data
#    endif
    if (size == 1)
        host_x86_CALL(block, codegen_profile_active ? (void *) codegen_profile_writemembl : (void *) writemembl);
    else if (size == 2)
        host_x86_CALL(block, codegen_profile_active ? (void *) codegen_profile_writememwl : (void *) writememwl);
    else if (size == 4)
        host_x86_CALL(block, codegen_profile_active ? (void *) codegen_profile_writememll : (void *) writememll);
    else if (size == 8)
        host_x86_CALL(block, codegen_profile_active ? (void *) codegen_profile_writememql : (void *) writememql);
    host_x86_MOV32_REG_BASE_OFFSET(block, REG_R14, REG_RBP, cycles_disp);
#    if _WIN64
    host_x86_ADD64_REG_IMM(block, REG_RSP, 0x28);
#    else
    host_x86_ADD64_REG_IMM(block, REG_RSP, 0x8);
#    endif
    host_x86_POP(block, REG_R11);
    host_x86_POP(block, REG_R10);
    host_x86_POP(block, REG_RDX);
    host_x86_POP(block, REG_RAX);
    host_x86_MOVZX_REG_ABS_32_8(block, REG_ESI, &cpu_state.abrt);
    host_x86_RET(block);
}

static void
build_loadstore_routines(codeblock_t *block)
{
    codegen_mem_load_byte = &block_write_data[block_pos];
    build_load_routine(block, 1, 0);
    codegen_mem_load_word = &block_write_data[block_pos];
    build_load_routine(block, 2, 0);
    codegen_mem_load_long = &block_write_data[block_pos];
    build_load_routine(block, 4, 0);
    codegen_mem_load_quad = &block_write_data[block_pos];
    build_load_routine(block, 8, 0);
    codegen_mem_load_single = &block_write_data[block_pos];
    build_load_routine(block, 4, 1);
    codegen_mem_load_double = &block_write_data[block_pos];
    build_load_routine(block, 8, 1);

    codegen_mem_store_byte = &block_write_data[block_pos];
    build_store_routine(block, 1, 0);
    codegen_mem_store_word = &block_write_data[block_pos];
    build_store_routine(block, 2, 0);
    codegen_mem_store_long = &block_write_data[block_pos];
    build_store_routine(block, 4, 0);
    codegen_mem_store_quad = &block_write_data[block_pos];
    build_store_routine(block, 8, 0);
    codegen_mem_store_single = &block_write_data[block_pos];
    build_store_routine(block, 4, 1);
    codegen_mem_store_double = &block_write_data[block_pos];
    build_store_routine(block, 8, 1);
}

void
codegen_backend_init(void)
{
    codeblock_t *block;
    int          c;
    uint8_t      large_block = 0;
    uint8_t      large_hash = 0;

    codeblock      = plat_mmap(BLOCK_SIZE * sizeof(codeblock_t), 0, &large_block);
    codeblock_hash = plat_mmap(HASH_SIZE * sizeof(codeblock_t *), 0, &large_hash);

    if (large_block)
        pclog("Allocated %" PRIu64 " bytes of large pages for codeblock pointers\n", (uint64_t) (BLOCK_SIZE * sizeof(codeblock_t)));
    if (large_hash)
        pclog("Allocated %" PRIu64 " bytes of large pages for codeblock hashes\n", (uint64_t) (HASH_SIZE * sizeof(codeblock_t *)));

    for (c = 0; c < BLOCK_SIZE; c++)
        codeblock[c].valid = 0;

    block_current                           = 0;
    block_pos                               = 0;
    block                                   = &codeblock[block_current];
    codeblock[block_current].head_mem_block = codegen_allocator_allocate(NULL, block_current);
    codeblock[block_current].data           = codeblock_allocator_get_ptr(codeblock[block_current].head_mem_block);
    block_write_data                        = codeblock[block_current].data;
    build_loadstore_routines(&codeblock[block_current]);

    codegen_alloc_bytes(block, 512);
    int cycles_disp = (int) offsetof(cpu_state_t, _cycles) - 128;
#ifdef CODEGEN_BACKEND_PINS_ESP
    int esp_disp = (int) offsetof(cpu_state_t, regs[4].l) - 128;
#endif
    codegen_gpf_rout = &block_write_data[block_pos];
#ifdef CODEGEN_BACKEND_PINS_ESP
    host_x86_MOV32_BASE_OFFSET_REG(block, REG_RBP, esp_disp, REG_PINNED_ESP);
#endif
#    if _WIN64
    host_x86_XOR32_REG_REG(block, REG_ECX, REG_ECX);
    host_x86_XOR32_REG_REG(block, REG_EDX, REG_EDX);
#    else
    host_x86_XOR32_REG_REG(block, REG_EDI, REG_EDI);
    host_x86_XOR32_REG_REG(block, REG_ESI, REG_ESI);
#    endif
    host_x86_CALL(block, (void *) x86gpf);
    codegen_exit_rout = &block_write_data[block_pos];
    host_x86_MOV32_BASE_OFFSET_REG(block, REG_RBP, cycles_disp, REG_R14);
#ifdef CODEGEN_BACKEND_PINS_ESP
    host_x86_MOV32_BASE_OFFSET_REG(block, REG_RBP, esp_disp, REG_PINNED_ESP);
#endif
#ifdef _WIN64
    /* XMM6 and XMM7 hold guest FPU/MMX values in blocks, and the Windows
       x64 ABI makes them the caller's: put back what the caller had. */
    host_x86_MOVDQU_XREG_BASE_OFFSET(block, REG_XMM6, REG_RSP, CODEGEN_XMM6_SAVE);
    host_x86_MOVDQU_XREG_BASE_OFFSET(block, REG_XMM7, REG_RSP, CODEGEN_XMM7_SAVE);
    host_x86_ADD64_REG_IMM(block, REG_RSP, CODEGEN_WIN64_FRAME);
#else
    host_x86_ADD64_REG_IMM(block, REG_RSP, 0x48);
#endif
    host_x86_POP(block, REG_R15);
    host_x86_POP(block, REG_R14);
    host_x86_POP(block, REG_R13);
    host_x86_POP(block, REG_R12);
#ifdef _WIN64
    host_x86_POP(block, REG_RDI);
    host_x86_POP(block, REG_RSI);
#endif
    host_x86_POP(block, REG_RBP);
    host_x86_POP(block, REG_RBX);
    host_x86_RET(block);

    /* Continuous In-JIT Event Loop Stub:
       Invoked from UOP_EXIT_BLOCK / UOP_EXIT_BLOCK_RET when cycles <= 0 or an event occurs.
       Preserves volatile caller registers, aligns stack, calls codegen_handle_jit_event().
       If codegen_handle_jit_event() returns non-zero, resumes the JIT by returning to caller;
       if it returns zero (cold exit requested), jumps to codegen_exit_rout. */
    codegen_event_stub = &block_write_data[block_pos];
    host_x86_PUSH(block, REG_RAX);
    host_x86_PUSH(block, REG_RDX);
    host_x86_PUSH(block, REG_R10);
    host_x86_PUSH(block, REG_R11);
    host_x86_MOV32_BASE_OFFSET_REG(block, REG_RBP, cycles_disp, REG_R14);
#ifdef CODEGEN_BACKEND_PINS_ESP
    host_x86_MOV32_BASE_OFFSET_REG(block, REG_RBP, esp_disp, REG_PINNED_ESP);
#endif
#ifdef _WIN64
    host_x86_SUB64_REG_IMM(block, REG_RSP, 0x28);
#else
    host_x86_SUB64_REG_IMM(block, REG_RSP, 0x8);
#endif
    host_x86_CALL(block, (void *) codegen_handle_jit_event);
#ifdef _WIN64
    host_x86_ADD64_REG_IMM(block, REG_RSP, 0x28);
#else
    host_x86_ADD64_REG_IMM(block, REG_RSP, 0x8);
#endif
#ifdef CODEGEN_BACKEND_PINS_ESP
    host_x86_MOV32_REG_BASE_OFFSET(block, REG_PINNED_ESP, REG_RBP, esp_disp);
#endif
    host_x86_MOV32_REG_BASE_OFFSET(block, REG_R14, REG_RBP, cycles_disp);
    host_x86_TEST32_REG(block, REG_EAX, REG_EAX);
    uint8_t *cold_exit_branch = host_x86_JZ_short(block);

    /* Event handled in JIT: restore registers and return to continue execution */
    host_x86_POP(block, REG_R11);
    host_x86_POP(block, REG_R10);
    host_x86_POP(block, REG_RDX);
    host_x86_POP(block, REG_RAX);
    host_x86_RET(block);

    /* Cold exit: drop saved registers and CALL return address, reset last_block_nr, and jump to exit routine */
    *cold_exit_branch = (uint8_t) ((uintptr_t) &block_write_data[block_pos] - (uintptr_t) (cold_exit_branch + 1));
    host_x86_POP(block, REG_R11);
    host_x86_POP(block, REG_R10);
    host_x86_POP(block, REG_RDX);
    host_x86_POP(block, REG_RAX);
    host_x86_ADD64_REG_IMM(block, REG_RSP, 8);
    int last_block_nr_disp = (int) offsetof(cpu_state_t, last_block_nr) - 128;
    host_x86_MOV32_BASE_OFFSET_IMM(block, REG_RBP, last_block_nr_disp, 0);
    host_x86_JMP(block, codegen_exit_rout);

    /* In-JIT Dispatch Stub (Proposal 2.2 / Phase 8.2):
       Invoked from block exits (unpredicted RET, indirect branches, block epilogue).
       Executes inlined native assembly hash probe on fast path without leaving JIT:
         1. Validates cycles > 0, exit_request == 0, and last_block_nr == 0.
         2. Verifies codegen_code_write_pending == 0 and codegen_profile_active == 0.
         3. Calculates target linear PC: cs + cpu_state.pc.
         4. Probes non-faulting TLB array readlookup2[pc >> 12] (or rammask in non-paged mode).
         5. Computes HASH(phys_addr) and fetches codeblock_hash[hash].
         6. Validates codeblock tags: valid, pc, _cs, phys, status, clean dirty mask,
            single page (page_mask2 == 0), and integer flags (no FPU / dirty list).
         7. On hit: direct jump to block->data + block->direct_entry_pos.
         8. On miss / event / compile needed: falls back to codegen_handle_dispatch(). */
    codegen_dispatch_stub = &block_write_data[block_pos];

    int exit_disp          = (int) offsetof(cpu_state_t, exit_request) - 128;
    int pc_disp            = (int) offsetof(cpu_state_t, pc) - 128;
    int cs_base_disp       = (int) offsetof(cpu_state_t, seg_cs.base) - 128;

    /* 1. Cycle & asynchronous event checks */
    host_x86_CMP32_REG_IMM(block, REG_R14, 0);
    uint32_t *miss_cycles = host_x86_JLE_long(block);

    host_x86_CMP32_BASE_OFFSET_IMM(block, REG_RBP, exit_disp, 0);
    uint32_t *miss_exit = host_x86_JNZ_long(block);

    host_x86_CMP32_BASE_OFFSET_IMM(block, REG_RBP, last_block_nr_disp, 0);
    uint32_t *miss_last_block = host_x86_JNZ_long(block);

    /* 2. Buffering and profiling checks */
    host_x86_MOV64_REG_IMM(block, REG_R11, (uint64_t) (uintptr_t) &codegen_code_write_pending);
    host_x86_MOV32_REG_BASE_OFFSET(block, REG_EAX, REG_R11, 0);
    host_x86_TEST32_REG(block, REG_EAX, REG_EAX);
    uint32_t *miss_cwp = host_x86_JNZ_long(block);

    host_x86_MOV64_REG_IMM(block, REG_R11, (uint64_t) (uintptr_t) &codegen_profile_active);
    host_x86_MOV32_REG_BASE_OFFSET(block, REG_EAX, REG_R11, 0);
    host_x86_TEST32_REG(block, REG_EAX, REG_EAX);
    uint32_t *miss_prof = host_x86_JNZ_long(block);

    /* 3. Compute linear PC: RDX = pc = cs + cpu_state.pc */
    host_x86_MOV32_REG_BASE_OFFSET(block, REG_EDX, REG_RBP, cs_base_disp);
    host_x86_MOV32_REG_BASE_OFFSET(block, REG_EAX, REG_RBP, pc_disp);
    host_x86_ADD32_REG_REG(block, REG_EDX, REG_EAX);

    /* 4. Physical address translation */
    host_x86_MOV64_REG_IMM(block, REG_R11, (uint64_t) (uintptr_t) &cr0);
    host_x86_MOV32_REG_BASE_OFFSET(block, REG_EAX, REG_R11, 0);
    host_x86_TEST32_REG_IMM(block, REG_EAX, 0x80000000);
    uint8_t *is_not_paged = host_x86_JZ_short(block);

    /* Paged translation via readlookup2 (in R12) */
    host_x86_MOV32_REG_REG(block, REG_EAX, REG_EDX);
    host_x86_SHR32_IMM(block, REG_EAX, 12);
    host_x86_MOV64_REG_BASE_INDEX_SHIFT(block, REG_RAX, REG_R12, REG_RAX, 3);
    host_x86_CMP64_REG_IMM(block, REG_RAX, (uint64_t) -1);
    uint32_t *miss_tlb = host_x86_JZ_long(block);
    host_x86_ADD64_REG_REG(block, REG_RAX, REG_RDX);
    host_x86_MOV64_REG_IMM(block, REG_R11, (uint64_t) (uintptr_t) &ram);
    host_x86_MOV64_REG_BASE_OFFSET(block, REG_RCX, REG_R11, 0);
    host_x86_SUB64_REG_REG(block, REG_RAX, REG_RCX);
    uint8_t *paged_done = host_x86_JMP_short(block);

    /* Real / unpaged mode */
    *is_not_paged = (uint8_t) ((uintptr_t) &block_write_data[block_pos] - (uintptr_t) (is_not_paged + 1));
    host_x86_MOV32_REG_REG(block, REG_EAX, REG_EDX);
    host_x86_MOV64_REG_IMM(block, REG_R11, (uint64_t) (uintptr_t) &rammask);
    host_x86_MOV32_REG_BASE_OFFSET(block, REG_ECX, REG_R11, 0);
    host_x86_AND32_REG_REG(block, REG_EAX, REG_ECX);

    *paged_done = (uint8_t) ((uintptr_t) &block_write_data[block_pos] - (uintptr_t) (paged_done + 1));
    host_x86_MOV32_REG_REG(block, REG_EAX, REG_EAX);

    /* 5. Compute HASH(phys_addr) */
    host_x86_MOV32_REG_REG(block, REG_ECX, REG_EAX);
    host_x86_SHR32_IMM(block, REG_ECX, 12);
    host_x86_XOR32_REG_REG(block, REG_ECX, REG_EAX);
    host_x86_MOV32_REG_REG(block, REG_R8, REG_EAX);
    host_x86_SHR32_IMM(block, REG_R8, 20);
    host_x86_XOR32_REG_REG(block, REG_ECX, REG_R8);
    host_x86_AND32_REG_IMM(block, REG_ECX, HASH_MASK);

    /* 6. Probe codeblock_hash table */
    host_x86_MOV64_REG_IMM(block, REG_R11, (uint64_t) (uintptr_t) &codeblock_hash);
    host_x86_MOV64_REG_BASE_OFFSET(block, REG_R11, REG_R11, 0);
    host_x86_ADD32_REG_REG(block, REG_ECX, REG_ECX);
    host_x86_MOVZX_BASE_INDEX_32_16(block, REG_ECX, REG_R11, REG_ECX);

    /* 7. Load target codeblock */
    host_x86_MOV64_REG_IMM(block, REG_R10, (uint64_t) (uintptr_t) &codeblock);
    host_x86_MOV64_REG_BASE_OFFSET(block, REG_R10, REG_R10, 0);
    host_x86_IMUL32_REG_REG_IMM(block, REG_ECX, REG_ECX, sizeof(codeblock_t));
    host_x86_ADD64_REG_REG(block, REG_R10, REG_RCX);

    /* 8. Validate block tags and invariants */
    host_x86_CMP32_BASE_OFFSET_IMM(block, REG_R10, offsetof(codeblock_t, valid), 0);
    uint32_t *miss_valid = host_x86_JZ_long(block);

    host_x86_MOV32_REG_BASE_OFFSET(block, REG_ECX, REG_R10, offsetof(codeblock_t, pc));
    host_x86_CMP32_REG_REG(block, REG_ECX, REG_EDX);
    uint32_t *miss_pc = host_x86_JNZ_long(block);

    host_x86_MOV32_REG_BASE_OFFSET(block, REG_ECX, REG_R10, offsetof(codeblock_t, _cs));
    host_x86_MOV32_REG_BASE_OFFSET(block, REG_R8, REG_RBP, cs_base_disp);
    host_x86_CMP32_REG_REG(block, REG_ECX, REG_R8);
    uint32_t *miss_cs = host_x86_JNZ_long(block);

    host_x86_MOV32_REG_BASE_OFFSET(block, REG_ECX, REG_R10, offsetof(codeblock_t, phys));
    host_x86_CMP32_REG_REG(block, REG_ECX, REG_EAX);
    uint32_t *miss_phys = host_x86_JNZ_long(block);

    host_x86_MOV16_REG_BASE_OFFSET(block, REG_CX, REG_R10, offsetof(codeblock_t, status));
    host_x86_MOV64_REG_IMM(block, REG_R11, (uint64_t) (uintptr_t) &cpu_cur_status);
    host_x86_MOV16_REG_BASE_OFFSET(block, REG_R8, REG_R11, 0);
    host_x86_CMP16_REG_REG(block, REG_CX, REG_R8);
    uint32_t *miss_status = host_x86_JNZ_long(block);

    host_x86_MOV64_REG_BASE_OFFSET(block, REG_RCX, REG_R10, offsetof(codeblock_t, page_mask2));
    host_x86_TEST64_REG(block, REG_RCX, REG_RCX);
    uint32_t *miss_pm2 = host_x86_JNZ_long(block);

    host_x86_MOV64_REG_BASE_OFFSET(block, REG_RCX, REG_R10, offsetof(codeblock_t, dirty_mask));
    host_x86_MOV64_REG_BASE_OFFSET(block, REG_RCX, REG_RCX, 0);
    host_x86_MOV64_REG_BASE_OFFSET(block, REG_R8, REG_R10, offsetof(codeblock_t, page_mask));
    host_x86_TEST64_REG(block, REG_RCX, REG_R8);
    uint32_t *miss_dirty = host_x86_JNZ_long(block);

    host_x86_MOV16_REG_BASE_OFFSET(block, REG_CX, REG_R10, offsetof(codeblock_t, flags));
    host_x86_MOVZX_REG_32_16(block, REG_ECX, REG_CX);
    host_x86_TEST32_REG_IMM(block, REG_ECX, CODEBLOCK_STATIC_TOP | CODEBLOCK_IN_DIRTY_LIST | CODEBLOCK_BYTE_MASK | CODEBLOCK_HAS_FPU);
    uint32_t *miss_flags1 = host_x86_JNZ_long(block);
    host_x86_TEST32_REG_IMM(block, REG_ECX, CODEBLOCK_WAS_RECOMPILED);
    uint32_t *miss_flags2 = host_x86_JZ_long(block);

    /* Fast-path Hit: jump directly to block->data[block->direct_entry_pos] */
    host_x86_MOV64_REG_BASE_OFFSET(block, REG_RAX, REG_R10, offsetof(codeblock_t, data));
    host_x86_MOV16_REG_BASE_OFFSET(block, REG_CX, REG_R10, offsetof(codeblock_t, direct_entry_pos));
    host_x86_MOVZX_REG_32_16(block, REG_ECX, REG_CX);
    host_x86_ADD64_REG_REG(block, REG_RAX, REG_RCX);
    host_x86_JMP_REG(block, REG_RAX);

    /* Slow-path Miss / Fallback */
    uint8_t *dispatch_slow = &block_write_data[block_pos];
    patch_disp32(miss_cycles, dispatch_slow);
    patch_disp32(miss_exit, dispatch_slow);
    patch_disp32(miss_last_block, dispatch_slow);
    patch_disp32(miss_cwp, dispatch_slow);
    patch_disp32(miss_prof, dispatch_slow);
    patch_disp32(miss_tlb, dispatch_slow);
    patch_disp32(miss_valid, dispatch_slow);
    patch_disp32(miss_pc, dispatch_slow);
    patch_disp32(miss_cs, dispatch_slow);
    patch_disp32(miss_phys, dispatch_slow);
    patch_disp32(miss_status, dispatch_slow);
    patch_disp32(miss_pm2, dispatch_slow);
    patch_disp32(miss_dirty, dispatch_slow);
    patch_disp32(miss_flags1, dispatch_slow);
    patch_disp32(miss_flags2, dispatch_slow);

#ifdef _WIN64
    host_x86_SUB64_REG_IMM(block, REG_RSP, 0x20);
    host_x86_MOV32_BASE_OFFSET_REG(block, REG_RBP, cycles_disp, REG_R14);
#ifdef CODEGEN_BACKEND_PINS_ESP
    host_x86_MOV32_BASE_OFFSET_REG(block, REG_RBP, esp_disp, REG_PINNED_ESP);
#endif
    host_x86_CALL(block, (void *) codegen_handle_dispatch);
#ifdef CODEGEN_BACKEND_PINS_ESP
    host_x86_MOV32_REG_BASE_OFFSET(block, REG_PINNED_ESP, REG_RBP, esp_disp);
#endif
    host_x86_MOV32_REG_BASE_OFFSET(block, REG_R14, REG_RBP, cycles_disp);
    host_x86_ADD64_REG_IMM(block, REG_RSP, 0x20);
#else
    host_x86_MOV32_BASE_OFFSET_REG(block, REG_RBP, cycles_disp, REG_R14);
#ifdef CODEGEN_BACKEND_PINS_ESP
    host_x86_MOV32_BASE_OFFSET_REG(block, REG_RBP, esp_disp, REG_PINNED_ESP);
#endif
    host_x86_CALL(block, (void *) codegen_handle_dispatch);
#ifdef CODEGEN_BACKEND_PINS_ESP
    host_x86_MOV32_REG_BASE_OFFSET(block, REG_PINNED_ESP, REG_RBP, esp_disp);
#endif
    host_x86_MOV32_REG_BASE_OFFSET(block, REG_R14, REG_RBP, cycles_disp);
#endif
    host_x86_TEST64_REG(block, REG_RAX, REG_RAX);
    uint8_t *dispatch_miss = host_x86_JZ_short(block);
    host_x86_JMP_REG(block, REG_RAX);

    /* Dispatch miss: exit JIT to C dispatcher */
    *dispatch_miss = (uint8_t) ((uintptr_t) &block_write_data[block_pos] - (uintptr_t) (dispatch_miss + 1));
    host_x86_JMP(block, codegen_exit_rout);

    /* Persistent JIT Entry Trampoline:
       Takes target block entry address in first argument (RDI on SysV, RCX on Win64).
       Saves host callee-saved registers, sets up host stack frame, sets RBP to cpu_state+128
       and R12 to readlookup2, then jumps directly to the target block's direct_entry_pos.
       When any block executes codegen_exit_rout, it unwinds this frame and returns to C. */
    codegen_enter_jit = (void (*)(void *)) &block_write_data[block_pos];
    host_x86_PUSH(block, REG_RBX);
    host_x86_PUSH(block, REG_RBP);
#ifdef _WIN64
    host_x86_PUSH(block, REG_RSI);
    host_x86_PUSH(block, REG_RDI);
#endif
    host_x86_PUSH(block, REG_R12);
    host_x86_PUSH(block, REG_R13);
    host_x86_PUSH(block, REG_R14);
    host_x86_PUSH(block, REG_R15);
#ifdef _WIN64
    host_x86_SUB64_REG_IMM(block, REG_RSP, CODEGEN_WIN64_FRAME);
    host_x86_MOVDQU_BASE_OFFSET_XREG(block, REG_RSP, CODEGEN_XMM6_SAVE, REG_XMM6);
    host_x86_MOVDQU_BASE_OFFSET_XREG(block, REG_RSP, CODEGEN_XMM7_SAVE, REG_XMM7);
#else
    host_x86_SUB64_REG_IMM(block, REG_RSP, 0x48);
#endif
    host_x86_MOV64_REG_IMM(block, REG_RBP, ((uintptr_t) &cpu_state) + 128);
    host_x86_MOV64_REG_IMM(block, REG_R12, (uint64_t) (uintptr_t) readlookup2);
    host_x86_MOV32_REG_BASE_OFFSET(block, REG_R14, REG_RBP, cycles_disp);
#ifdef CODEGEN_BACKEND_PINS_ESP
    host_x86_MOV32_REG_BASE_OFFSET(block, REG_PINNED_ESP, REG_RBP, esp_disp);
#endif
#ifdef _WIN64
    host_x86_JMP_REG(block, REG_RCX);
#else
    host_x86_JMP_REG(block, REG_RDI);
#endif

    if (codegen_dump_perf_map_active()) {
        codegen_dump_perf_map_symbol((uintptr_t) codegen_gpf_rout, (uintptr_t) codegen_exit_rout - (uintptr_t) codegen_gpf_rout, "jit_gpf_rout");
        codegen_dump_perf_map_symbol((uintptr_t) codegen_exit_rout, (uintptr_t) codegen_event_stub - (uintptr_t) codegen_exit_rout, "jit_exit_rout");
        codegen_dump_perf_map_symbol((uintptr_t) codegen_event_stub, (uintptr_t) codegen_dispatch_stub - (uintptr_t) codegen_event_stub, "jit_event_stub");
        codegen_dump_perf_map_symbol((uintptr_t) codegen_dispatch_stub, (uintptr_t) codegen_enter_jit - (uintptr_t) codegen_dispatch_stub, "jit_dispatch_stub");
        codegen_dump_perf_map_symbol((uintptr_t) codegen_enter_jit, (uintptr_t) &block_write_data[block_pos] - (uintptr_t) codegen_enter_jit, "jit_enter_trampoline");
    }

    block_write_data = NULL;

    asm(
        "stmxcsr %0\n"
        : "=m"(cpu_state.old_fp_control));
    cpu_state.trunc_fp_control = cpu_state.old_fp_control | 0x6000;
}

void
codegen_set_rounding_mode(int mode)
{
    cpu_state.new_fp_control = (cpu_state.old_fp_control & ~0x6000) | (mode << 13);
}

void
codegen_backend_prologue(codeblock_t *block)
{
    codegen_mem_slowpath_reset();
    block_pos = BLOCK_START; /*Entry code*/
    block->direct_entry_pos = block_pos;
    if (codegen_dump_is_active() || codegen_profile_active || codegen_hang_detect_active) {
        int cycles_disp = (int) offsetof(cpu_state_t, _cycles) - 128;
        host_x86_MOV32_BASE_OFFSET_REG(block, REG_RBP, cycles_disp, REG_R14);
#ifdef CODEGEN_BACKEND_PINS_ESP
        int esp_disp = (int) offsetof(cpu_state_t, regs[4].l) - 128;
        host_x86_MOV32_BASE_OFFSET_REG(block, REG_RBP, esp_disp, REG_PINNED_ESP);
#endif
#ifdef _WIN64
        host_x86_MOV64_REG_IMM(block, REG_RCX, (uint64_t) (uintptr_t) block);
#else
        host_x86_MOV64_REG_IMM(block, REG_RDI, (uint64_t) (uintptr_t) block);
#endif
        host_x86_CALL(block, (void *) codegen_dump_enter_block);
#ifdef CODEGEN_BACKEND_PINS_ESP
        host_x86_MOV32_REG_BASE_OFFSET(block, REG_PINNED_ESP, REG_RBP, esp_disp);
#endif
    }
    if (block->flags & CODEBLOCK_HAS_FPU) {
        host_x86_MOV32_REG_ABS(block, REG_EAX, &cpu_state.TOP);
        host_x86_SUB32_REG_IMM(block, REG_EAX, block->TOP);
        host_x86_MOV32_BASE_OFFSET_REG(block, REG_RSP, IREG_TOP_diff_stack_offset, REG_EAX);
    }
}

void
codegen_backend_epilogue(codeblock_t *block)
{
    if (codegen_dispatch_stub)
        host_x86_JMP(block, codegen_dispatch_stub);
    else
        host_x86_JMP(block, codegen_exit_rout);

    codegen_mem_emit_slowpaths(block);
}
#endif

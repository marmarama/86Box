#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define BLOCK_MAX 65536
static uint8_t block_write_data_buf[BLOCK_MAX];
uint8_t *block_write_data = block_write_data_buf;
int block_pos = 0;

#include "codegen.h"

static codeblock_t dummy_block;
codeblock_t *block = &dummy_block;

struct {
    uint8_t abrt;
    uint32_t TOP;
    uint32_t old_fp_control;
    uint32_t trunc_fp_control;
    uint32_t new_fp_control;
} cpu_state;

uint8_t ram[1024];
void *readlookup2[1024];
void *writelookup2[1024];

void fatal(const char *fmt, ...) {
    printf("FATAL\n");
    exit(1);
}
void CPU_BLOCK_END(void) {}
void x86gpf(void) {}
void readmembl(void) {}
void readmemwl(void) {}
void readmemll(void) {}
void readmemql(void) {}
void writemembl(void) {}
void writememwl(void) {}
void writememll(void) {}
void writememql(void) {}

#define codegen_alloc_bytes(b, n) do {} while(0)

#include "codegen_backend_x86-64_defs.h"
#include "codegen_backend_x86-64_ops_helpers.h"
#include "codegen_backend_x86-64_ops.c"
#include "codegen_backend_x86-64_ops_sse.c"

static void dump_hex(const char *name, int start, int end) {
    printf("%s: ", name);
    for (int i = start; i < end; i++) {
        printf("%02x ", block_write_data_buf[i]);
    }
    printf("\n");
}

int main(void) {
    dummy_block.flags = 0;
    
    // We will run tests for all key emitters
    #define TEST_1REG(func, r1, ...) do { \
        int start = block_pos; \
        func(block, r1, ##__VA_ARGS__); \
        dump_hex(#func "_" #r1, start, block_pos); \
    } while(0)

    #define TEST_2REG(func, r1, r2, ...) do { \
        int start = block_pos; \
        func(block, r1, r2, ##__VA_ARGS__); \
        dump_hex(#func "_" #r1 "_" #r2, start, block_pos); \
    } while(0)

    #define TEST_3REG(func, r1, r2, r3, ...) do { \
        int start = block_pos; \
        func(block, r1, r2, r3, ##__VA_ARGS__); \
        dump_hex(#func "_" #r1 "_" #r2 "_" #r3, start, block_pos); \
    } while(0)

    // Ops test
    TEST_2REG(host_x86_ADD32_REG_REG, REG_EAX, REG_EBX);
    TEST_2REG(host_x86_ADD32_REG_REG, REG_R14, REG_R15);
    TEST_2REG(host_x86_ADD32_REG_REG, REG_EAX, REG_R14);
    TEST_2REG(host_x86_ADD32_REG_REG, REG_R14, REG_EBX);

    TEST_2REG(host_x86_SUB32_REG_REG, REG_EAX, REG_EBX);
    TEST_2REG(host_x86_SUB32_REG_REG, REG_R14, REG_R15);
    TEST_2REG(host_x86_SUB32_REG_REG, REG_EAX, REG_R14);
    TEST_2REG(host_x86_SUB32_REG_REG, REG_R14, REG_EBX);

    TEST_2REG(host_x86_CMP32_REG_REG, REG_EAX, REG_EBX);
    TEST_2REG(host_x86_CMP32_REG_REG, REG_R14, REG_R15);
    TEST_2REG(host_x86_CMP32_REG_REG, REG_EAX, REG_R14);
    TEST_2REG(host_x86_CMP32_REG_REG, REG_R14, REG_EBX);

    TEST_2REG(host_x86_MOV32_REG_REG, REG_EAX, REG_EBX);
    TEST_2REG(host_x86_MOV32_REG_REG, REG_R14, REG_R15);
    TEST_2REG(host_x86_MOV32_REG_REG, REG_EAX, REG_R14);
    TEST_2REG(host_x86_MOV32_REG_REG, REG_R14, REG_EBX);

    TEST_2REG(host_x86_MOVZX_REG_32_8, REG_EAX, REG_EBX);
    TEST_2REG(host_x86_MOVZX_REG_32_8, REG_R14, REG_R15);
    TEST_2REG(host_x86_MOVZX_REG_32_8, REG_EAX, REG_R14);
    TEST_2REG(host_x86_MOVZX_REG_32_8, REG_R14, REG_EBX);

    TEST_2REG(host_x86_MOVZX_REG_32_16, REG_EAX, REG_EBX);
    TEST_2REG(host_x86_MOVZX_REG_32_16, REG_R14, REG_R15);
    TEST_2REG(host_x86_MOVZX_REG_32_16, REG_EAX, REG_R14);
    TEST_2REG(host_x86_MOVZX_REG_32_16, REG_R14, REG_EBX);

    TEST_2REG(host_x86_MOVSX_REG_32_8, REG_EAX, REG_EBX);
    TEST_2REG(host_x86_MOVSX_REG_32_8, REG_R14, REG_R15);
    TEST_2REG(host_x86_MOVSX_REG_32_16, REG_EAX, REG_EBX);
    TEST_2REG(host_x86_MOVSX_REG_32_16, REG_R14, REG_R15);

    TEST_2REG(host_x86_SHL32_IMM, REG_EAX, 4);
    TEST_2REG(host_x86_SHL32_IMM, REG_R14, 4);
    TEST_1REG(host_x86_SHL32_CL, REG_EAX);
    TEST_1REG(host_x86_SHL32_CL, REG_R14);

    TEST_2REG(host_x86_SHR32_IMM, REG_EAX, 4);
    TEST_2REG(host_x86_SHR32_IMM, REG_R14, 4);
    TEST_1REG(host_x86_SHR32_CL, REG_EAX);
    TEST_1REG(host_x86_SHR32_CL, REG_R14);

    TEST_2REG(host_x86_SAR32_IMM, REG_EAX, 4);
    TEST_2REG(host_x86_SAR32_IMM, REG_R14, 4);
    TEST_1REG(host_x86_SAR32_CL, REG_EAX);
    TEST_1REG(host_x86_SAR32_CL, REG_R14);

    TEST_2REG(host_x86_TEST32_REG, REG_EAX, REG_EBX);
    TEST_2REG(host_x86_TEST32_REG, REG_R14, REG_R15);
    TEST_2REG(host_x86_TEST32_REG, REG_EAX, REG_R14);
    TEST_2REG(host_x86_TEST32_REG, REG_R14, REG_EBX);

    TEST_2REG(host_x86_TEST32_REG_IMM, REG_EAX, 0x1234);
    TEST_2REG(host_x86_TEST32_REG_IMM, REG_EBX, 0x1234);
    TEST_2REG(host_x86_TEST32_REG_IMM, REG_R14, 0x1234);

    TEST_2REG(host_x86_LEA_REG_IMM, REG_EAX, REG_EBX, 0x20);
    TEST_2REG(host_x86_LEA_REG_IMM, REG_R14, REG_R15, 0x20);
    TEST_2REG(host_x86_LEA_REG_IMM, REG_EAX, REG_R13, 0x20);

    TEST_3REG(host_x86_LEA_REG_REG, REG_EAX, REG_EBX, REG_EDX);
    TEST_3REG(host_x86_LEA_REG_REG, REG_R14, REG_R15, REG_R13);
    TEST_3REG(host_x86_LEA_REG_REG, REG_EAX, REG_R13, REG_EDX);

    TEST_3REG(host_x86_LEA_REG_REG_SHIFT, REG_EAX, REG_EBX, REG_EDX, 2);
    TEST_3REG(host_x86_LEA_REG_REG_SHIFT, REG_R14, REG_R15, REG_R13, 2);
    TEST_3REG(host_x86_LEA_REG_REG_SHIFT, REG_EAX, REG_R13, REG_EDX, 2);

    // SSE test
    TEST_2REG(host_x86_CVTSD2SI_REG_XREG, REG_EAX, REG_XMM1);
    TEST_2REG(host_x86_CVTSD2SI_REG_XREG, REG_R14, REG_XMM1);
    TEST_2REG(host_x86_CVTSD2SI_REG_XREG, REG_R13, REG_XMM1);

    TEST_2REG(host_x86_CVTSI2SD_XREG_REG, REG_XMM1, REG_EAX);
    TEST_2REG(host_x86_CVTSI2SD_XREG_REG, REG_XMM1, REG_R14);
    TEST_2REG(host_x86_CVTSI2SD_XREG_REG, REG_XMM1, REG_R13);

    TEST_2REG(host_x86_CVTSI2SS_XREG_REG, REG_XMM1, REG_EAX);
    TEST_2REG(host_x86_CVTSI2SS_XREG_REG, REG_XMM1, REG_R14);

    TEST_2REG(host_x86_MOVD_REG_XREG, REG_EAX, REG_XMM1);
    TEST_2REG(host_x86_MOVD_REG_XREG, REG_R14, REG_XMM1);

    TEST_2REG(host_x86_MOVD_XREG_REG, REG_XMM1, REG_EAX);
    TEST_2REG(host_x86_MOVD_XREG_REG, REG_XMM1, REG_R14);

    TEST_2REG(host_x86_MOVQ_REG_XREG, REG_EAX, REG_XMM1);
    TEST_2REG(host_x86_MOVQ_REG_XREG, REG_R14, REG_XMM1);

    TEST_2REG(host_x86_MOVQ_XREG_REG, REG_XMM1, REG_EAX);
    TEST_2REG(host_x86_MOVQ_XREG_REG, REG_XMM1, REG_R14);

    return 0;
}

#define JMP_LEN_BYTES 5

extern void codegen_dump_record_chunk(const uint8_t *chunk_ptr, uint32_t chunk_len);

static inline uint8_t
rex_byte(int w, int r, int x, int b)
{
    return 0x40 | ((!!w) << 3) | (((r & 8) >> 1)) | (((x & 8) >> 2)) | (((b & 8) >> 3));
}

static inline void
codegen_addbyte(UNUSED(codeblock_t *block), uint8_t val)
{
    if (block_pos >= BLOCK_MAX) {
        fatal("codegen_addbyte over! %i\n", block_pos);
#if 0
        CPU_BLOCK_END();
#endif
    }
    block_write_data[block_pos++] = val;
}
static inline void
codegen_addbyte2(UNUSED(codeblock_t *block), uint8_t vala, uint8_t valb)
{
    if (block_pos > (BLOCK_MAX - 2)) {
        fatal("codegen_addbyte2 over! %i\n", block_pos);
        CPU_BLOCK_END();
    }
    block_write_data[block_pos++] = vala;
    block_write_data[block_pos++] = valb;
}
static inline void
codegen_addbyte3(UNUSED(codeblock_t *block), uint8_t vala, uint8_t valb, uint8_t valc)
{
    if (block_pos > (BLOCK_MAX - 3)) {
        fatal("codegen_addbyte3 over! %i\n", block_pos);
        CPU_BLOCK_END();
    }
    block_write_data[block_pos++] = vala;
    block_write_data[block_pos++] = valb;
    block_write_data[block_pos++] = valc;
}
static inline void
codegen_addbyte4(UNUSED(codeblock_t *block), uint8_t vala, uint8_t valb, uint8_t valc, uint8_t vald)
{
    if (block_pos > (BLOCK_MAX - 4)) {
        fatal("codegen_addbyte4 over! %i\n", block_pos);
        CPU_BLOCK_END();
    }
    block_write_data[block_pos++] = vala;
    block_write_data[block_pos++] = valb;
    block_write_data[block_pos++] = valc;
    block_write_data[block_pos++] = vald;
}

static inline void
codegen_addword(UNUSED(codeblock_t *block), uint16_t val)
{
    if (block_pos > (BLOCK_MAX - 2)) {
        fatal("codegen_addword over! %i\n", block_pos);
        CPU_BLOCK_END();
    }
    *(uint16_t *) &block_write_data[block_pos] = val;
    block_pos += 2;
}

static inline void
codegen_addlong(UNUSED(codeblock_t *block), uint32_t val)
{
    if (block_pos > (BLOCK_MAX - 4)) {
        fatal("codegen_addlong over! %i\n", block_pos);
        CPU_BLOCK_END();
    }
    *(uint32_t *) &block_write_data[block_pos] = val;
    block_pos += 4;
}

static inline void
codegen_addquad(UNUSED(codeblock_t *block), uint64_t val)
{
    if (block_pos > (BLOCK_MAX - 8)) {
        fatal("codegen_addquad over! %i\n", block_pos);
        CPU_BLOCK_END();
    }
    *(uint64_t *) &block_write_data[block_pos] = val;
    block_pos += 8;
}

static inline void
codegen_alloc_bytes(codeblock_t *block, int size)
{
    if (block_pos > ((BLOCK_MAX - size) - JMP_LEN_BYTES)) {
        /*Current block is full. Allocate a new block*/
        struct mem_block_t *new_block = codegen_allocator_allocate(block->head_mem_block, get_block_nr(block));
        uint8_t            *new_ptr   = codeblock_allocator_get_ptr(new_block);

        /*Add a jump instruction to the new block*/
        codegen_addbyte(block, 0xe9); /*JMP*/
        codegen_addlong(block, (uintptr_t) new_ptr - (uintptr_t) &block_write_data[block_pos + 4]);

        codegen_dump_record_chunk(block_write_data, (uint32_t) block_pos);

        /*Set write address to start of new block*/
        block_pos        = 0;
        block_write_data = new_ptr;
    }
}

static inline int
is_imm8(uint32_t imm_data)
{
    if (imm_data <= 0x7f || imm_data >= 0xffffff80)
        return 1;
    return 0;
}

#ifndef _CODEGEN_H_
#define _CODEGEN_H_

#include <86box/mem.h>
#include <stddef.h>
#include "x86_ops.h"
#include "x86_flags.h"

/*Handling self-modifying code (of which there is a lot on x86) :

  PCem tracks a 'dirty mask' for each physical page, in which each bit
  represents 64 bytes. This is only tracked for pages that have code in - when a
  page first has a codeblock generated, it is evicted from the writelookup and
  added to the page_lookup for this purpose. When in the page_lookup, each write
  will go through the mem_write_ram*_page() functions and set the dirty mask
  appropriately.

  Each codeblock also contains a code mask (actually two masks, one for each
  page the block is/may be in), again with each bit representing 64 bytes.

  Each page has a list of codeblocks present in it. As each codeblock can span
  up to two pages, two lists are present.

  When a codeblock is about to be executed, the code masks are compared with the
  dirty masks for the relevant pages. If either intersect, then
  codegen_check_flush() is called on the affected page(s), and all affected
  blocks are evicted.

  The 64 byte granularity appears to work reasonably well for most cases,
  avoiding most unnecessary evictions (eg when code & data are stored in the
  same page).
*/

#define CODEBLOCK_MAX_EXITS 2
#define LINK_NONE            0

#define BLOCK_EXIT_NONE      0
#define BLOCK_EXIT_DIRECT_1  1
#define BLOCK_EXIT_COND_2    2
#define BLOCK_EXIT_RET       3

enum {
    COND_O   = 0,
    COND_NO  = 1,
    COND_B   = 2,
    COND_NB  = 3,
    COND_E   = 4,
    COND_NE  = 5,
    COND_BE  = 6,
    COND_NBE = 7,
    COND_S   = 8,
    COND_NS  = 9,
    COND_P   = 10,
    COND_NP  = 11,
    COND_L   = 12,
    COND_NL  = 13,
    COND_LE  = 14,
    COND_NLE = 15,
    COND_JCXZ_16  = 16,
    COND_JCXZ_32  = 17,
    COND_LOOP_16  = 18,
    COND_LOOP_32  = 19,
    COND_LOOPE_16 = 20,
    COND_LOOPE_32 = 21,
    COND_LOOPNE_16 = 22,
    COND_LOOPNE_32 = 23,
};

#define MAX_INSTRUCTION_COUNT 50

struct x86seg_t;

typedef struct codegen_instruction_t {
    uint32_t pc;
    int      op_ssegs;
    struct x86seg_t *op_ea_seg;
    uint32_t op_32;
    int      first_uop;
    int      TOP;
    uint8_t  len;
} codegen_instruction_t;

extern codegen_instruction_t codegen_instructions[MAX_INSTRUCTION_COUNT];

typedef struct codeblock_link_t {
    uint32_t  target_pc;          /* Guest PC of target (or BLOCK_PC_INVALID) */
    uint32_t *jmp_disp_ptr;      /* Pointer to 32-bit displacement in host JMP */
    void     *unlinked_target;   /* Pointer to unlinked stub code */
    uint16_t  target_block;      /* Block index of target, or BLOCK_INVALID */
    uint32_t  prev_link;         /* Previous in target's incoming link list (encoded) */
    uint32_t  next_link;         /* Next in target's incoming link list (encoded) */
} codeblock_link_t;

typedef struct codeblock_t {
    uint32_t pc;
    uint32_t _cs;
    uint32_t phys, phys_2;
    uint16_t status;
    uint16_t flags;
    uint8_t  ins;
    uint8_t  TOP;
    /* Per-block retry counter used to delay NO_IMMEDIATES promotion until
       churn repeats, reducing premature slow-immediate mode. */
    uint8_t  dirty_list_recompile_hits;
    /* Tracks the most recent dirty-list epoch seen by this block so promotion
       can require a dense burst, not stale spaced-out hits. */
    uint16_t dirty_list_last_epoch;
    int      valid;
    uint64_t exec_count;
    uint32_t compile_seq;

    /* Direct block chaining data */
    uint16_t direct_entry_pos;
    uint8_t  exit_type;
    uint8_t  exit_cond;
    codeblock_link_t links[CODEBLOCK_MAX_EXITS];
    uint32_t first_incoming_link;

    /*Pointers for codeblock tree, used to search for blocks when hash lookup
      fails.*/
    uint16_t parent, left, right;

    uint8_t *data;

    uint64_t  page_mask, page_mask2;
    uint64_t *dirty_mask, *dirty_mask2;

    /*Previous and next pointers, for the codeblock list associated with
      each physical page. Two sets of pointers, as a codeblock can be
      present in two pages.*/
    uint16_t prev, next;
    uint16_t prev_2, next_2;

    /*First mem_block_t used by this block. Any subsequent mem_block_ts
      will be in the list starting at head_mem_block->next.*/
    struct mem_block_t *head_mem_block;
} codeblock_t;

extern codeblock_t *codeblock;

extern uint16_t *codeblock_hash;

extern uint8_t *block_write_data;


/*Code block uses FPU*/
#define CODEBLOCK_HAS_FPU 1
/*Code block is always entered with the same FPU top-of-stack*/
#define CODEBLOCK_STATIC_TOP 2
/*Code block has been compiled*/
#define CODEBLOCK_WAS_RECOMPILED 4
/*Code block is in free list and is not valid*/
#define CODEBLOCK_IN_FREE_LIST 8
/*Code block spans two pages, page_mask2 and dirty_mask2 are valid*/
#define CODEBLOCK_HAS_PAGE2 0x10
/*Code block is using a byte mask for code present and dirty*/
#define CODEBLOCK_BYTE_MASK 0x20
/*Code block is in dirty list*/
#define CODEBLOCK_IN_DIRTY_LIST 0x40
/*Code block is not inlining immediate parameters, parameters must be fetched from memory*/
#define CODEBLOCK_NO_IMMEDIATES 0x80

#define BLOCK_PC_INVALID        0xffffffff

#define BLOCK_INVALID           0

static inline int
get_block_nr(codeblock_t *block)
{
    return ((uintptr_t) block - (uintptr_t) codeblock) / sizeof(codeblock_t);
}

static inline codeblock_t *
codeblock_tree_find(uint32_t phys, uint32_t _cs)
{
    codeblock_t *block;
    uint64_t     a = _cs | ((uint64_t) phys << 32);

    if (!pages[phys >> 12].head)
        return NULL;

    block = &codeblock[pages[phys >> 12].head];
    while (block) {
        uint64_t block_cmp = block->_cs | ((uint64_t) block->phys << 32);
        if (a == block_cmp) {
            if (!((block->status ^ cpu_cur_status) & CPU_STATUS_FLAGS) && ((block->status & cpu_cur_status & CPU_STATUS_MASK) == (cpu_cur_status & CPU_STATUS_MASK)))
                break;
        }
        if (a < block_cmp)
            block = block->left ? &codeblock[block->left] : NULL;
        else
            block = block->right ? &codeblock[block->right] : NULL;
    }

    return block;
}

static inline void
codeblock_tree_add(codeblock_t *new_block)
{
    codeblock_t *block = &codeblock[pages[new_block->phys >> 12].head];
    uint64_t     a     = new_block->_cs | ((uint64_t) new_block->phys << 32);

    if (!pages[new_block->phys >> 12].head) {
        pages[new_block->phys >> 12].head = get_block_nr(new_block);
        new_block->parent = new_block->left = new_block->right = BLOCK_INVALID;
    } else {
        codeblock_t *old_block     = NULL;
        uint64_t     old_block_cmp = 0;

        while (block) {
            old_block     = block;
            old_block_cmp = old_block->_cs | ((uint64_t) old_block->phys << 32);

            if (a < old_block_cmp)
                block = block->left ? &codeblock[block->left] : NULL;
            else
                block = block->right ? &codeblock[block->right] : NULL;
        }

        if (old_block != NULL) {
            if (a < old_block_cmp)
                old_block->left = get_block_nr(new_block);
            else
                old_block->right = get_block_nr(new_block);

            new_block->parent = get_block_nr(old_block);
        } else
            new_block->parent = BLOCK_INVALID;

        new_block->left = new_block->right = BLOCK_INVALID;
    }
}

static inline void
codeblock_tree_delete(codeblock_t *block)
{
    uint16_t     parent_nr = block->parent;
    codeblock_t *parent;

    if (block->parent)
        parent = &codeblock[block->parent];
    else
        parent = NULL;

    if (!block->left && !block->right) {
        /*Easy case - remove from parent*/
        if (!parent)
            pages[block->phys >> 12].head = BLOCK_INVALID;
        else {
            uint16_t block_nr = get_block_nr(block);

            if (parent->left == block_nr)
                parent->left = BLOCK_INVALID;
            if (parent->right == block_nr)
                parent->right = BLOCK_INVALID;
        }
        return;
    } else if (!block->left) {
        /*Only right node*/
        if (!parent_nr) {
            pages[block->phys >> 12].head                   = block->right;
            codeblock[pages[block->phys >> 12].head].parent = BLOCK_INVALID;
        } else {
            uint16_t block_nr = get_block_nr(block);

            if (parent->left == block_nr) {
                parent->left                   = block->right;
                codeblock[parent->left].parent = parent_nr;
            }
            if (parent->right == block_nr) {
                parent->right                   = block->right;
                codeblock[parent->right].parent = parent_nr;
            }
        }
        return;
    } else if (!block->right) {
        /*Only left node*/
        if (!parent_nr) {
            pages[block->phys >> 12].head                   = block->left;
            codeblock[pages[block->phys >> 12].head].parent = BLOCK_INVALID;
        } else {
            uint16_t block_nr = get_block_nr(block);

            if (parent->left == block_nr) {
                parent->left                   = block->left;
                codeblock[parent->left].parent = parent_nr;
            }
            if (parent->right == block_nr) {
                parent->right                   = block->left;
                codeblock[parent->right].parent = parent_nr;
            }
        }
        return;
    } else {
        /*Difficult case - node has two children. Walk right child to find lowest node*/
        codeblock_t *lowest = &codeblock[block->right];
        codeblock_t *highest;
        codeblock_t *old_parent;
        uint16_t     lowest_nr;

        while (lowest->left)
            lowest = &codeblock[lowest->left];
        lowest_nr = get_block_nr(lowest);

        old_parent = &codeblock[lowest->parent];

        /*Replace deleted node with lowest node*/
        if (!parent_nr)
            pages[block->phys >> 12].head = lowest_nr;
        else {
            uint16_t block_nr = get_block_nr(block);

            if (parent->left == block_nr)
                parent->left = lowest_nr;
            if (parent->right == block_nr)
                parent->right = lowest_nr;
        }

        lowest->parent = parent_nr;
        lowest->left   = block->left;
        if (lowest->left)
            codeblock[lowest->left].parent = lowest_nr;

        old_parent->left = BLOCK_INVALID;

        highest = &codeblock[lowest->right];
        if (!lowest->right) {
            if (lowest_nr != block->right) {
                lowest->right                  = block->right;
                codeblock[block->right].parent = lowest_nr;
            }
            return;
        }

        while (highest->right)
            highest = &codeblock[highest->right];

        if (block->right && block->right != lowest_nr) {
            highest->right                 = block->right;
            codeblock[block->right].parent = get_block_nr(highest);
        }
    }
}

#define PAGE_MASK_MASK  63
#define PAGE_MASK_SHIFT 6

void codegen_mark_code_present_multibyte(codeblock_t *block, uint32_t start_pc, int len);

static inline void
codegen_mark_code_present(codeblock_t *block, uint32_t start_pc, int len)
{
    if (len == 1) {
        if (block->flags & CODEBLOCK_BYTE_MASK) {
            if (!((start_pc ^ block->pc) & ~0x3f)) /*Starts in second page*/
                block->page_mask |= ((uint64_t) 1 << (start_pc & PAGE_MASK_MASK));
            else
                block->page_mask2 |= ((uint64_t) 1 << (start_pc & PAGE_MASK_MASK));
        } else {
            if (!((start_pc ^ block->pc) & ~0xfff)) /*Starts in second page*/
                block->page_mask |= ((uint64_t) 1 << ((start_pc >> PAGE_MASK_SHIFT) & PAGE_MASK_MASK));
            else
                block->page_mask2 |= ((uint64_t) 1 << ((start_pc >> PAGE_MASK_SHIFT) & PAGE_MASK_MASK));
        }
    } else
        codegen_mark_code_present_multibyte(block, start_pc, len);
}

extern void codegen_init(void);
extern void codegen_reset(void);
extern void codegen_block_init(uint32_t phys_addr);
extern void codegen_block_remove(void);
extern void codegen_block_start_recompile(codeblock_t *block);
extern void codegen_block_end_recompile(codeblock_t *block);
extern void codegen_block_end(void);
extern void codegen_delete_block(codeblock_t *block);
extern void codegen_generate_call(uint8_t opcode, OpFn op, uint32_t fetchdat, uint32_t new_pc, uint32_t old_pc);
extern void codegen_generate_seg_restore(void);
extern void codegen_set_op32(void);
extern void codegen_flush(void);
extern void codegen_check_flush(struct page_t *page, uint64_t mask, uint32_t phys_addr);
extern int  codegen_code_write_pending;
extern void codegen_note_code_write(struct page_t *page);
extern void codegen_flush_code_writes(void);
struct ir_data_t;
x86seg     *codegen_generate_ea(struct ir_data_t *ir, x86seg *op_ea_seg, uint32_t fetchdat, int op_ssegs, uint32_t *op_pc, uint32_t op_32, int stack_offset);
extern void codegen_check_seg_read(codeblock_t *block, struct ir_data_t *ir, x86seg *seg);
extern void codegen_check_seg_write(codeblock_t *block, struct ir_data_t *ir, x86seg *seg);
extern void codegen_check_regs(void);

extern int codegen_purge_purgable_list(void);
/*Delete a random code block to free memory. This is obviously quite expensive, and
  will only be called when the allocator is out of memory*/
extern void codegen_delete_random_block(int required_mem_block);

extern int      cpu_block_end;
extern uint32_t codegen_endpc;

extern int cpu_reps;
extern int cpu_notreps;

extern int codegen_block_cycles;

extern void (*codegen_timing_start)(void);
extern void (*codegen_timing_prefix)(uint8_t prefix, uint32_t fetchdat);
extern void (*codegen_timing_opcode)(uint8_t opcode, uint32_t fetchdat, int op_32, uint32_t op_pc);
extern void (*codegen_timing_block_start)(void);
extern void (*codegen_timing_block_end)(void);
extern int (*codegen_timing_jump_cycles)(void);

typedef struct codegen_timing_t {
    void (*start)(void);
    void (*prefix)(uint8_t prefix, uint32_t fetchdat);
    void (*opcode)(uint8_t opcode, uint32_t fetchdat, int op_32, uint32_t op_pc);
    void (*block_start)(void);
    void (*block_end)(void);
    int (*jump_cycles)(void);
} codegen_timing_t;

extern codegen_timing_t codegen_timing_pentium;
extern codegen_timing_t codegen_timing_686;
extern codegen_timing_t codegen_timing_486;
extern codegen_timing_t codegen_timing_winchip;
extern codegen_timing_t codegen_timing_winchip2;
extern codegen_timing_t codegen_timing_k5;
extern codegen_timing_t codegen_timing_k6;
extern codegen_timing_t codegen_timing_p6;

void codegen_timing_set(codegen_timing_t *timing);

extern int block_current;
extern int block_pos;

#define CPU_BLOCK_END() cpu_block_end = 1

/*Current physical page of block being recompiled. -1 if no recompilation taking place */
extern uint32_t recomp_page;

extern x86seg  *op_ea_seg;
extern int      op_ssegs;
extern uint32_t op_old_pc;

/*Set to 1 if flags have been changed in the block being recompiled, and hence
  flags_op is known and can be relied on */
extern int codegen_flags_changed;
extern int codegen_last_flags_op;

static inline int
is_zf_flags_res_op(int flags_op)
{
    switch (flags_op) {
        case FLAGS_ZN8:
        case FLAGS_ZN16:
        case FLAGS_ZN32:
        case FLAGS_ADD8:
        case FLAGS_ADD16:
        case FLAGS_ADD32:
        case FLAGS_SUB8:
        case FLAGS_SUB16:
        case FLAGS_SUB32:
        case FLAGS_SHL8:
        case FLAGS_SHL16:
        case FLAGS_SHL32:
        case FLAGS_SHR8:
        case FLAGS_SHR16:
        case FLAGS_SHR32:
        case FLAGS_SAR8:
        case FLAGS_SAR16:
        case FLAGS_SAR32:
        case FLAGS_INC8:
        case FLAGS_INC16:
        case FLAGS_INC32:
        case FLAGS_DEC8:
        case FLAGS_DEC16:
        case FLAGS_DEC32:
#ifdef USE_NEW_DYNAREC
        case FLAGS_ADC8:
        case FLAGS_ADC16:
        case FLAGS_ADC32:
        case FLAGS_SBC8:
        case FLAGS_SBC16:
        case FLAGS_SBC32:
        case FLAGS_SHRD16:
        case FLAGS_SHRD32:
#endif
            return 1;
        default:
            return 0;
    }
}

extern int codegen_fpu_entered;
extern int codegen_mmx_entered;

extern int codegen_fpu_loaded_iq[8];
extern int codegen_reg_loaded[8];

extern int codegen_in_recompile;

void codegen_generate_reset(void);

int  codegen_get_instruction_uop(codeblock_t *block, uint32_t pc, int *first_instruction, int *TOP);
void codegen_set_loop_start(struct ir_data_t *ir, int first_instruction);

void codegen_link_exit(codeblock_t *src_block, int exit_idx, codeblock_t *dst_block);
void codegen_unlink_block(codeblock_t *block);

/* Block-linking policy. Evaluated at link time only (see codegen_link_exit), so
   it can be changed while running. Defaults come from the environment:
     BOX86_LINK_MASK  bitmask of LINK_* below (decimal or 0x hex)
     BOX86_LINK_LIMIT only create the first N links (0 = unlimited); the Nth link
                      is logged, which makes bisecting a bad link easy. */
#define LINK_JMP       (1 << 0) /* direct JMP rel */
#define LINK_JCC       (1 << 1) /* Jcc rel */
#define LINK_LOOP      (1 << 2) /* JCXZ / LOOP / LOOPE / LOOPNE */
#define LINK_REAL      (1 << 3) /* link blocks compiled in real mode or V86 */
#define LINK_PMODE     (1 << 4) /* link blocks compiled in protected mode */
#define LINK_SAME_PAGE (1 << 5) /* restriction: only if source and target share a guest physical page */
#define LINK_NO_FPU    (1 << 6) /* restriction: never link blocks that use the FPU */
#define LINK_RET       (1 << 7) /* direct return link for near RET */
#define LINK_DEFAULT   (LINK_JMP | LINK_JCC | LINK_LOOP | LINK_REAL | LINK_PMODE | LINK_SAME_PAGE | LINK_RET)

extern uint32_t codegen_link_mask;
extern uint32_t codegen_link_limit;

/* Change policy at runtime and unpatch every existing link so the new policy takes
   effect. Call only from the CPU thread outside a running block. */
void codegen_link_set_policy(uint32_t mask, uint32_t limit);

/* Unpatch every existing link back to its unlinked stub so that control returns
   to the C dispatcher on block exit (called on CR3/TLB flushes via codegen_flush). */
void codegen_unpatch_all_links(void);

#ifdef DEBUG_EXTRA
extern uint32_t instr_counts[256 * 256];
#endif

#endif

# Comprehensive Code Review (`REVIEW3`): `upstream/master...HEAD`

- **Review Base:** `upstream/master` (`1741108a721b6f7fea3db191a4403f4271b26a3b`)
- **Review Head:** `HEAD` (`69616ef05` — *"fix fp bug"*)
- **Scope:** 142 files changed (`+163,991` / `-1,021` lines across `src/codegen_new/`, `src/cpu/`, `src/mem/`, `src/disk/minivhd/`, `src/dma.c`, `src/char/`, `src/nmi.c`, `src/nvr_at.c`, `src/pit.c`, `benchmarks/`, and workspace root).

---

## 1. Executive Summary

Since forking from `upstream/master`, the new dynamic recompiler (`src/codegen_new/`) and adjacent CPU/memory/I/O subsystems have undergone six major optimization phases plus three rounds of bug-fix hardening (`1e4454df5`, `5e07e1cd6`, `69616ef05`).

### Major Fixes Landed Since `REVIEW.md` & `REVIEW2.md`
1. **Critical FPU Register Indexing & `TOP` Wrapping Fix (`69616ef05`)**:
   - Inserting `IREG_exit_request` into the `IREG_*` enum in `src/codegen_new/codegen_reg.h` shifted `IREG_ST0` (`66`), `IREG_tag0` (`74`), and `IREG_ST0_i64` (`82`) from `8n + 1` to `8n + 2`.
   - `codegen_reg_load` and `codegen_reg_writeback` (`src/codegen_new/codegen_reg.c:218-296`) previously indexed `ST`, `tag`, and `MM` via `ir_reg.reg & 7`—which was already off-by-one upstream when `IREG_ST0` was `65`, and shifted by another slot when `IREG_exit_request` was added, mapping `ST(0)..ST(7)` to `6, 7, 0, 1, 2, 3, 4, 5` and mismatching `offsetof(cpu_state, ST[i])` when `CODEBLOCK_STATIC_TOP` was clear.
   - Commit `69616ef05` replaced `ir_reg.reg & 7` with `IREG_GET_REG(ir_reg.reg) - IREG_ST0` (and `- IREG_tag0`, `- IREG_ST0_i64`), and masked `IREG_FPU_TOP` updates in `fpu_POP`, `fpu_POP2`, and `fpu_PUSH` (`src/codegen_new/codegen_ops_helpers.h:59-83`) with `& 7` so `cpu_state.TOP` stays within `0..7` across all FPU paths.
2. **Native `UOP_SETCC` and `UOP_CMOV` (`5e07e1cd6`)**:
   - Replaced the C helper call in `ropSETcc` and the 7-uop arithmetic multiplexing sequence in `ropCMOVcc_common` with `codegen_get_condition_fastpath()` + `UOP_SETCC` / `UOP_CMOV`.
3. **2-Page Block Linking Guard (`5e07e1cd6`)**:
   - `codegen_link_allowed` (`src/codegen_new/codegen_block.c:308-309`) now rejects linking into targets where `dst_block->page_mask2 != 0`, preventing direct cross-block jumps from bypassing the second-page physical/dirty check in `exec386_dynarec_dyn`.
4. **macOS ARM64 W^X & I-Cache Maintenance in `patch_jmp_disp` (`5e07e1cd6`)**:
   - `patch_jmp_disp` (`src/codegen_new/codegen_block.c:237-263`) now wraps live code patching with `pthread_jit_write_protect_np(0)/(1)` on Apple Silicon, flushes the instruction cache via `__builtin___clear_cache` on ARM64 and LoongArch64, and replaces `assert()` with `fatal()` for `int32_t` displacement overflow.
5. **Intra-Block Jump Destination Barrier (`5e07e1cd6`)**:
   - `codegen_ir_compile` (`src/codegen_new/codegen_ir.c:240`) now calls `codegen_reg_flush_invalidate(ir, block)` at `UOP_TYPE_JUMP_DEST` targets so values modified inside conditionally skipped diamond branches are reloaded from `cpu_state` at the merge point.
6. **Unified Dirty-List Retry Hysteresis (`5e07e1cd6`)**:
   - `dynarec_s03e_dirty_epoch` and `DYNAREC_S03B_NO_IMM_THRESHOLD` (`src/cpu/386_dynarec.c`, `src/codegen_new/codegen.h`) are now enabled across all architectures under `USE_NEW_DYNAREC` rather than `#ifdef __aarch64__` only.

### Top Remaining / Newly Introduced Issues
Despite those fixes, a fresh line-by-line audit of `HEAD` (`69616ef05`) uncovered **5 critical/high-severity bugs** (including 2 new bugs introduced in `5e07e1cd6`) and several medium-severity correctness and hygiene issues:

| ID | Severity | Location | Summary |
| :--- | :--- | :--- | :--- |
| **C-1** | **CRITICAL** | `codegen_backend_loongarch64_uops.c:831-833` | `codegen_CMOV` corrupts the low byte of the 32-bit LoongArch64 `BEQ` instruction via an x86-style `*skip = (uint8_t)...` displacement write instead of `host_loong64_branch_set_offset`. |
| **C-2** | **HIGH** | `codegen_backend_arm64_uops.c:523`, `codegen_backend_loongarch64_uops.c:832` | 16-bit `CMOVcc` (`IREG_SIZE_W`) on ARM64 and LoongArch64 overwrites all 32/64 bits of the destination host register (`EAX`), clobbering bits `31:16` with garbage from `src_new`. |
| **C-3** | **HIGH** | `codegen_ops_string.c`, `codegen_ir_defs.h`, `codegen_reg.c` | In-block `REP` string loops hold `IREG_cycles` in a persistent host register across `UOP_MEM_STORE_REG` / `UOP_MEM_LOAD_REG` (`UOP_TYPE_ORDER_BARRIER`), overwriting MMIO/SVGA wait-state cycle deductions in `cpu_state._cycles`. |
| **C-4** | **HIGH** | `codegen_ops_shift.c:518-530` | `ropC1_l` under `CODEBLOCK_NO_IMMEDIATES` unconditionally updates compile-time `codegen_last_flags_op` inside a runtime `count != 0` conditional branch, corrupting subsequent fastpath `Jcc`/`SETcc`/`CMOVcc` when dynamic shift count is `0`. |
| **C-5** | **MEDIUM-HIGH** | `codegen.c:730-733`, `codegen_ops_branch.c:160,179,198` | Residual `new_pc` sentinel ambiguity: `if (new_pc && new_pc != -1)` skips emitting `uop_MOV_IMM(ir, IREG_pc, new_pc)` when a legitimate target or fallthrough PC wraps to `0x00000000` or `0xffffffff`. |

---

## 2. Critical & High-Severity Bugs in Current `HEAD`

### C-1. LoongArch64 `codegen_CMOV` Corrupts Machine Code (`codegen_backend_loongarch64_uops.c:824-834`)

- **Location:** `src/codegen_new/codegen_backend_loongarch64_uops.c:824-834`
- **Code:**
  ```c
  void
  codegen_CMOV(codeblock_t *block, uop_t *uop)
  {
      int dest_reg = HOST_REG_PARAM_1(uop->dest_reg_a);
      int src_old  = HOST_REG_PARAM_1(uop->src_reg_a);
      int src_new  = HOST_REG_PARAM_1(uop->src_reg_b);
      int cond_reg = HOST_REG_PARAM_1(uop->src_reg_c);
      if (dest_reg != src_old)
          host_loong64_MOV_D(block, dest_reg, src_old);
      uint8_t *skip = host_loong64_BEQ_(block, cond_reg, REG_ZERO);
      host_loong64_MOV_D(block, dest_reg, src_new);
      *skip = (uint8_t) ((uintptr_t) &block_write_data[block_pos] - (uintptr_t) skip) - 4;
  }
  ```
- **Bug:**
  1. `host_loong64_BEQ_` returns `uint32_t *` pointing to a 32-bit LoongArch instruction word (`0x58000000 | (rj << 5) | rd`), where the branch displacement is a 16-bit word-shifted offset in bits `25:10`.
  2. Assigning the return value to `uint8_t *skip` and writing `*skip = (uint8_t) ... - 4;` (copied from x86-64's 2-byte `74 rel8` encoding) overwrites bits `7:0` of the 32-bit LoongArch instruction—corrupting `rd` (`REG_ZERO`, bits `4:0`) and the low 3 bits of `rj` (`cond_reg`, bits `9:5`) while leaving the 16-bit branch offset at `0`!
- **Fix:** Patch the branch via `host_loong64_branch_set_offset(skip, &block_write_data[block_pos]);` (comparing `uint32_t *skip = host_loong64_BEQ_(...)` just like `codegen_SETCC` right above it at line 817), or use branchless `MASKEQZ` / `MASKNEZ` + `OR`.

---

### C-2. ARM64 & LoongArch64 16-Bit `CMOVcc` Clobbers Upper 16 Bits of 32-Bit GPRs (`codegen_backend_arm64_uops.c:515-524`, `codegen_backend_loongarch64_uops.c:824-834`)

- **Locations:**
  - `src/codegen_new/codegen_ops_mov.c:5448-5501` (`ropCMOVcc_common`)
  - `src/codegen_new/codegen_backend_arm64_uops.c:515-524` (`codegen_CMOV`)
  - `src/codegen_new/codegen_backend_loongarch64_uops.c:824-834` (`codegen_CMOV`)
- **Bug:**
  - On x86, a 16-bit `CMOVcc reg16, r/m16` (`0F 4x /r` with `op_32 == 0`) modifies only bits `15:0` of the destination register when the condition is true, and leaves bits `31:16` of the 32-bit register intact.
  - In `ropCMOVcc_common`, when `is_op32 == 0`, `dst_ireg = IREG_16(dest_reg)` (`IREG_SIZE_W`) and `src_ireg = IREG_16(fetchdat & 7)` (or `IREG_temp0_W` for memory operands).
  - Because `IREG_SIZE_W` is sub-dword, the register allocator (`codegen_reg.c`) allocates a 32-bit host register holding the full 32-bit parent GPR (`EAX`, etc.) for both `dest_reg` and `src_new`.
  - On **x86-64** (`codegen_backend_x86-64_uops.c:2185`), `codegen_CMOV` checks `REG_IS_W(size)` and emits `host_x86_CMOVcc16_REG_REG`, which preserves bits `31:16` of `dest_reg`.
  - On **ARM64** (`codegen_backend_arm64_uops.c:523`), `codegen_CMOV` unconditionally emits 32-bit `host_arm64_CSEL_NE(block, dest_reg, src_new, src_old);`. When `is_op32 == 0` and the condition is true, bits `31:16` of `dest_reg` are overwritten with bits `31:16` of `src_new`!
  - On **LoongArch64** (`codegen_backend_loongarch64_uops.c:832`), `host_loong64_MOV_D(block, dest_reg, src_new)` similarly overwrites all 64 bits of `dest_reg` without preserving bits `31:16` on `IREG_SIZE_W`.
- **Fix:**
  - Either handle `REG_IS_W(IREG_GET_SIZE(uop->dest_reg_a))` inside `codegen_CMOV` on ARM64 (`BFXIL` or branch + `BFI`) and LoongArch64 (`BSTRINS_D`), or normalize `src_new` in `ropCMOVcc_common` when `!is_op32`.

---

### C-3. In-Block `REP` String Loops Overwrite MMIO / Video Wait-State Cycle Deductions (`codegen_ops_string.c`)

- **Locations:**
  - `src/codegen_new/codegen_ops_string.c:571-857` (`ropREP_STOS_common`, `ropREP_MOVS_common`, `ropREP_LODS_common`, `ropREPE_SCAS_common`, `ropREPNE_SCAS_common`, `ropREPE_CMPS_common`, `ropREPNE_CMPS_common`)
  - `src/codegen_new/codegen_ir_defs.h:15-31` (`UOP_MEM_LOAD_REG`, `UOP_MEM_STORE_REG`)
  - `src/codegen_new/codegen_reg.c:966-1028` (`codegen_reg_process_uop`)
- **Bug:**
  - Inside the in-block `REP` loop body, each iteration executes `uop_MEM_LOAD_REG` / `uop_MEM_STORE_REG` followed by `uop_SUB_IMM(ir, IREG_cycles, IREG_cycles, step_cycles)` and `uop_NOP_BARRIER(ir)`.
  - `UOP_MEM_LOAD_REG` and `UOP_MEM_STORE_REG` carry `UOP_TYPE_ORDER_BARRIER` (not `UOP_TYPE_BARRIER`). In `codegen_reg_process_uop`, `UOP_TYPE_ORDER_BARRIER` calls `codegen_reg_flush(ir, block)`—which writes dirty registers back to `cpu_state`—**after** allocating the uop's source/dest registers, and **never invalidates clean host registers** (`IREG_cycles` is allocated in a persistent callee-saved register `REG_PERMANENT` on ARM64/LoongArch64 and a non-volatile register on x86-64).
  - When a `REP MOVS` or `REP STOS` accesses Video RAM (SVGA banked or Linear Frame Buffer) or MMIO, the memory helper (`svga_writel_linear`, `mem_write_ramb_page`, etc.) deducts video/bus wait states directly from `cpu_state._cycles` in memory (e.g., `cycles -= video_timing_write_l`).
  - Immediately after the store returns, `uop_SUB_IMM(ir, IREG_cycles, IREG_cycles, step_cycles)` subtracts `3` or `4` from the **stale host register** loaded before the store, and `uop_NOP_BARRIER(ir)` writes that stale value back to `cpu_state._cycles`, **erasing all video/MMIO wait-state cycle deductions**!
- **Impact:**
  - Guest software copying framebuffers to VRAM via `REP MOVSD` / `REP STOSD` (such as DOS games and benchmarks like PCPBench) experiences virtually zero video wait states, inflating guest-measured framerates (explaining the PCPBench `60.4 -> 208.7` guest FPS anomaly noted in `ROADMAP.md`) and breaking cycle-accurate ISA/PCI/VLB bus timing.
- **Fix:**
  - Either invalidate `IREG_cycles` on `UOP_MEM_LOAD_*` / `UOP_MEM_STORE_*` slow-path calls (or treat `IREG_cycles` as volatile across memory uops inside `REP` loops), or reload `IREG_cycles` from `cpu_state._cycles` before `uop_SUB_IMM(ir, IREG_cycles, ...)` inside the `REP` loop body.

---

### C-4. Compile-Time `codegen_last_flags_op` Desynchronization in `ropC1_l` (`codegen_ops_shift.c:518-530`)

- **Location:** `src/codegen_new/codegen_ops_shift.c:518-530`
- **Code:**
  ```c
  if (block->flags & CODEBLOCK_NO_IMMEDIATES) {
      uint32_t new_pc;
      int      jump_uop;

      LOAD_IMMEDIATE_FROM_RAM_8(block, ir, IREG_temp2, cs + op_pc + 1);
      uop_AND_IMM(ir, IREG_temp2, IREG_temp2, 0x1f);
      jump_uop = uop_CMP_IMM_JZ_DEST(ir, IREG_temp2, 0);

      new_pc = shift_common_variable_32(ir, fetchdat, op_pc, target_seg, IREG_temp2) + 1;
      uop_NOP_BARRIER(ir);
      uop_set_jump_dest(ir, jump_uop);
      return new_pc;
  }
  ```
- **Bug:**
  - On x86, a shift with a count of `0` leaves architectural `EFLAGS` unmodified. At runtime, `uop_CMP_IMM_JZ_DEST(ir, IREG_temp2, 0)` jumps over `shift_common_variable_32`, preserving `cpu_state.flags_op` and `cpu_state.flags_res` from the preceding instruction.
  - However, at **compile time**, `shift_common_variable_32` emits `uop_MOV_IMM(ir, IREG_flags_op, FLAGS_SHL32)` (etc.), which triggers `uop_gen_reg_dst_imm` (`src/codegen_new/codegen_ir_defs.h:524-527`) to set `codegen_last_flags_op = FLAGS_SHL32`.
  - If the shift is followed in the same block by a `Jcc`, `SETcc`, or `CMOVcc`, `codegen_cond_jump_false()` or `codegen_get_condition_fastpath()` sees `codegen_last_flags_op == FLAGS_SHL32` and emits a direct fastpath comparison on `IREG_flags_res` instead of consulting `cpu_state.flags_op`, producing wrong branch/condition results whenever the dynamic shift count at runtime is `0` (and for `ROL`/`ROR`, where `FLAGS_ROL32`/`FLAGS_ROR32` isn't even handled by fastpath, or if the prior op was `CMP` and the shift is `SHL` with count `0`, where `COND_E`/`COND_NE` fastpath tests `IREG_flags_res == 0` successfully only by coincidence if `CMP` also used `IREG_flags_res`, unless the prior op was `FLAGS_UNKNOWN` or `CLI`/`POPF`/`SAHF`!).
- **Fix:**
  - After `uop_set_jump_dest(ir, jump_uop);` in `ropC1_l`, set `codegen_last_flags_op = FLAGS_UNKNOWN;` because the runtime value of `IREG_flags_op` at the merge point is data-dependent on the dynamic immediate.
  - Note also that if `(fetchdat & 0xc0) != 0xc0` (memory operand) and `IREG_temp2 == 0`, skipping `shift_common_variable_32` skips the memory store, whereas real x86 `SHL [mem], 0` still performs the read-modify-write cycle; resetting `codegen_last_flags_op = FLAGS_UNKNOWN` is the minimum required for flag correctness.

---

### C-5. Residual `new_pc` Sentinel Edge Cases (`codegen.c:730-733`, `codegen_ops_branch.c:160,179,198`)

- **Locations:**
  - `src/codegen_new/codegen.c:730-740`
  - `src/codegen_new/codegen_ops_branch.c:160, 179, 198` (`ropJcc_8`, `ropJcc_16`, `ropJcc_32`)
- **Code (`src/codegen_new/codegen.c:730-733`):**
  ```c
  if (new_pc || block->exit_type != BLOCK_EXIT_NONE) {
      if (new_pc && new_pc != -1)
          uop_MOV_IMM(ir, IREG_pc, new_pc);
      codegen_Specify_EIP = 0;
  ```
- **Bug:**
  - Commit `5e07e1cd6` fixed `ropJMP_r8/16/32` and `ropCALL_r32` to explicitly emit `uop_MOV_IMM(ir, IREG_pc, dest_addr)` and return `-1`.
  - However, `ropJcc_8`, `ropJcc_16`, and `ropJcc_32` still return `next_pc` (the fallthrough PC) and rely on `codegen.c:731` (`if (new_pc && new_pc != -1) uop_MOV_IMM(ir, IREG_pc, new_pc);`) to emit the fallthrough `IREG_pc` update!
  - Because `block->exit_type` is `BLOCK_EXIT_COND_2`, if `next_pc == 0` (e.g., a 16-bit `Jcc` at `0xfffe` whose fallthrough wraps to `0x0000`) or `next_pc == 0xffffffff` (`(uint32_t)-1`), line 730 enters the `if` branch, **but line 731 (`if (new_pc && new_pc != -1)`) evaluates to false and silently omits `uop_MOV_IMM(ir, IREG_pc, new_pc)`!** When the conditional branch is not taken at runtime, `cpu_state.pc` remains stuck at the previous instruction's PC instead of advancing to `next_pc`.
  - Similarly, if any non-branch instruction has a fallthrough PC of `0xffffffff` (`-1`), `new_pc != -1` is false and `IREG_pc` is not updated.
- **Fix:**
  - Have `ropJcc_8`, `ropJcc_16`, and `ropJcc_32` explicitly emit `uop_MOV_IMM(ir, IREG_pc, next_pc)` and return `-1` (matching `ropJMP_r*`), or replace the in-band `0` / `-1` sentinels in `op_fn` return values.

---

## 3. Medium-Severity & Subsystem Concerns

### M-1. Phase 6 Bulk Memory & VHD I/O (`src/mem/mem.c`, `src/dma.c`, `src/disk/minivhd/minivhd_io.c`)

1. **Partial Sector Read Zero-Fill in `mvhd_sparse_read` (`src/disk/minivhd/minivhd_io.c:218-221`)**:
   - In `mvhd_sparse_read`:
     ```c
     size_t rd = fread(buf_ptr, MVHD_SECTOR_SIZE, run, vhdm->f);
     if (rd < (size_t)run)
         memset(buf_ptr + rd * MVHD_SECTOR_SIZE, 0, (size_t)(run - rd) * MVHD_SECTOR_SIZE);
     ```
   - Standard C `fread(ptr, size, nmemb, stream)` returns the number of *complete* elements (`MVHD_SECTOR_SIZE` sectors) read. If the underlying VHD file is truncated mid-sector, `fread` advances into `buf_ptr + rd * MVHD_SECTOR_SIZE` by the partial bytes and returns `rd`, and `memset` properly zeroes the entire remainder starting at `buf_ptr + rd * MVHD_SECTOR_SIZE` (matching the old per-sector loop's `if (fread(..., 1, f) != 1) memset(..., 0, 512)` behavior). This is functionally equivalent to upstream.
2. **Hardcoded `0xfff` Mask vs. `MEM_GRANULARITY_MASK` (`src/mem/mem.c:1716, 1756`)**:
   - In `mem_read_phys_block` and `mem_write_phys_block`:
     ```c
     uint32_t page_off = addr & 0xfff;
     uint32_t chunk    = 0x1000 - page_off;
     ```
   - While 4 KB pages match `pages[addr >> 12]`, `src/mem/mem.c` defines `MEM_GRANULARITY_BITS` (`12`), `MEM_GRANULARITY_SIZE` (`0x1000`), and `MEM_GRANULARITY_MASK` (`0xfff`). Using those named constants improves consistency with the rest of `mem.c`.
3. **Code-Page Detection in `page_has_code` (`src/mem/mem.c:1704-1709`)**:
   - `page_has_code(page_t *p)` checks `p->code_present_mask || p->head || p->block || p->block_2`.
   - Under `USE_NEW_DYNAREC`, `codegen_mark_code_present_multibyte` always sets bits in `p->code_present_mask` and links blocks into `p->block` / `p->block_2` (and `p->head` on the old dynarec). Thus skipping `page_write_ram_mark_dirty` when `!page_has_code(p)` is sound as long as every compiled block registers on its physical page(s), which `codegen_Check_Block_Pages` / `codegen_mark_code_present` guarantees.

---

### M-2. Block Linking vs. `CODEBLOCK_STATIC_TOP` (`src/cpu/386_dynarec.c:512`, `src/codegen_new/codegen_block.c:302`)

- **Locations:**
  - `src/cpu/386_dynarec.c:512` (`can_enter_block_linked`)
  - `src/codegen_new/codegen_block.c:302` (`codegen_link_allowed`)
- **Observation:**
  - Both functions reject linking into *any* target block that has `CODEBLOCK_STATIC_TOP` set (`if (dst_block->flags & CODEBLOCK_STATIC_TOP) return 0;`).
  - Because `CODEBLOCK_STATIC_TOP` is set on virtually every block that executes an FPU instruction (`codegen_ops_fpu_*.c`), FPU-heavy loops (such as Quake's perspective texture mapping and transformation loops) cannot use direct cross-block linking (`patch_jmp`) or `LINK_SAME_PAGE` and must fall back to the C dispatcher loop on every block boundary.
  - Now that commit `69616ef05` fixed the `IREG_ST0` / `IREG_tag0` enum-shift bug and `TOP & 7` wrapping, if a source block also has `CODEBLOCK_STATIC_TOP` (or if `dst_block->TOP == cpu_state.TOP` at link time and `src_block` preserves `TOP`), linking between `STATIC_TOP` blocks with matching `TOP` expectations could be safely enabled in the future to recover FPU block-chaining performance.

---

### M-3. Direct `nmi = 1` Writers Outside `nmi_raise()` (`src/cpu/386_common.h:583`)

- **Locations:**
  - `src/cpu/386_common.h:583` (`cpu_update_exit_request`)
  - `src/nmi.c`, `src/nvr_at.c`, `src/pit.c`
- **Observation:**
  - While `nmi_raise()`, `nvr_at.c`, and `pit.c` now call `cpu_update_exit_request()` after setting `nmi = 1`, ~18 machine/chipset devices (`src/machine/m_at_t3100e.c`, `src/chipset/ali1429.c`, `src/sio/sio_fdc37c665.c`, etc.) still write `nmi = 1;` directly instead of calling `nmi_raise()`.
  - Because linked blocks exit whenever `cpu_state._cycles < 0` (at most a few hundred cycles), the worst-case impact on those specific machines is a sub-microsecond NMI recognition delay rather than a hang, but routing those direct `nmi = 1` assignments through `nmi_raise()` is recommended for completeness.

---

### M-4. Unconditional `fflush()` on Every Serial/Log Byte (`src/char/char_file.c:66`)

- **Location:** `src/char/char_file.c:66`
- **Code:**
  ```c
  size_t ret = fwrite(buf, 1, count, dev->f);
  fflush(dev->f);
  return ret;
  ```
- **Observation:**
  - Calling `fflush(dev->f)` on every 1-byte `char_file_write()` adds a synchronous flush on every character transmitted over a file-backed COM/LPT port. Consider flushing only on `\n` or gating behind a debug/benchmark env var.

---

## 4. Repository Hygiene & Upstreaming Readiness

1. **102,885-Line `.vscode-ctags` File Committed in `5e07e1cd6`**:
   - `.vscode-ctags` (`+102,885` lines) was accidentally staged and committed in `5e07e1cd6`. It should be removed via `git rm .vscode-ctags` and added to `.gitignore`.
2. **Tracked `perf.data` and `benchmarks/__pycache__/`**:
   - `perf.data` (0 bytes in the repo root) and `benchmarks/__pycache__/run_benchmarks.cpython-312.pyc` are tracked in Git and should be removed (`git rm perf.data benchmarks/__pycache__/run_benchmarks.cpython-312.pyc`) and ignored.
3. **Tracked `scratch/` Directory (67 Files)**:
   - `scratch/` contains 67 tracked files including proprietary/guest binaries (`scratch/DIBENG.DLL`, `scratch/PCPBENCH.EXE`, `scratch/mem_dump.bin`, `scratch/pcp_code.bin`, `scratch/seg14.bin`, `scratch/types.o`), 38 PNG screenshots (`scratch/bp_doom_*.png`, `scratch/bp_quake_*.png`), and temporary GDB/Python scripts. These must be untracked before upstreaming.

---

## 5. Prioritized Action Plan

1. **[Immediate / Critical]** Fix LoongArch64 `codegen_CMOV` (`src/codegen_new/codegen_backend_loongarch64_uops.c:831-833`) to use `uint32_t *skip = host_loong64_BEQ_(...)` and `host_loong64_branch_set_offset(skip, &block_write_data[block_pos])`.
2. **[Immediate / High]** Fix 16-bit `CMOVcc` (`IREG_SIZE_W`) on ARM64 (`src/codegen_new/codegen_backend_arm64_uops.c:515-524`) and LoongArch64 (`src/codegen_new/codegen_backend_loongarch64_uops.c:824-834`) so bits `31:16` of the destination 32-bit GPR are preserved when the condition is true.
3. **[High]** Fix `IREG_cycles` caching across `UOP_MEM_LOAD_REG` / `UOP_MEM_STORE_REG` inside in-block `REP` loops (`src/codegen_new/codegen_ops_string.c` / `src/codegen_new/codegen_reg.c`) so MMIO/SVGA wait-state cycle deductions in `cpu_state._cycles` are not overwritten.
4. **[High]** Reset `codegen_last_flags_op = FLAGS_UNKNOWN;` after the `count == 0` conditional skip in `ropC1_l` (`src/codegen_new/codegen_ops_shift.c:528`).
5. **[Medium]** Emit `uop_MOV_IMM(ir, IREG_pc, next_pc)` and return `-1` in `ropJcc_8`, `ropJcc_16`, and `ropJcc_32` (`src/codegen_new/codegen_ops_branch.c:160,179,198`) to eliminate the `next_pc == 0` / `0xffffffff` sentinel ambiguity in `codegen.c:731`.
6. **[Cleanup]** `git rm` `.vscode-ctags`, `perf.data`, `benchmarks/__pycache__/`, and `scratch/` artifacts, and add them to `.gitignore`.


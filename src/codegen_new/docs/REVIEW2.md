# Architectural Review 2: Subsystem Design & Technical Debt in `codegen_new`

**Scope**: Entire `codegen_new` dynamic recompiler subsystem across front-end decode, intermediate representation (IR), register allocation, execution loop, memory management, and backends.  
**Date**: October 2026  
**Context**: Complement to [`REVIEW.md`](file:///home/marm/src/86Box/src/codegen_new/REVIEW.md) (which focused on the Phase 1–4 x86-64 fork diffs). This document analyzes the holistic architecture, structural trade-offs, conceptual tensions, and latent design defects across the entire subsystem.

---

## Executive Summary

`codegen_new` represents a major architectural upgrade over the legacy single-pass JIT (`codegen`), introducing an intermediate representation (IR), SSA-like virtual register versioning, dead flag/store elimination, portable multi-architecture backends (x86-64, ARM64, LoongArch64), inlined soft-TLB fast paths, in-block string loops, and direct block chaining.

However, as the subsystem grew to accommodate x86 architectural quirks and bare-metal PC timing requirements, several structural tensions, over-complex workarounds, and abstraction leaks emerged. This review documents the areas that stand out as **architecturally confused**, **overly complex**, or **broken / high-risk**.

---

## 1. Architecturally Confused Areas

### 1.1 The "Linear Basic-Block IR" vs. "Real-World x86 Control Flow"
* **The Root Conflict**: The IR layer ([`codegen_ir.c`](file:///home/marm/src/86Box/src/codegen_new/codegen_ir.c)) and register allocator ([`codegen_reg.c`](file:///home/marm/src/86Box/src/codegen_new/codegen_reg.c)) are designed around a strict **linear forward scan** over a single basic block. The allocator assumes every instruction executes sequentially, tracking reference counts and liveness from top to bottom. There is no Control Flow Graph (CFG), no basic-block graph, and no phi-node representation.
* **The Clash**: Real x86 instructions are inherently branching. `SETcc`, `CMOVcc`, `JCXZ`, `LOOP`, and string `REP` ops all contain internal control flow.
* **The Resulting Inconsistency**: Because the register allocator cannot understand branches, the codebase handles control flow using three conflicting paradigms:
  1. **Arithmetic Multiplexing**: `SETcc` and `CMOVcc` evaluate condition flags into a temporary, negate it into a full bitmask (`0 - cond`), and multiplex register contents via `dest = dest ^ ((src ^ dest) & mask)` to avoid emitting an intra-block jump.
  2. **Ad-hoc Pipeline Barriers**: `REP` loops emit `uop_LOOP_LABEL`, `uop_LOOP_JMP`, and `uop_NOP_BARRIER` register flushes to force the allocator to dump and reload all host registers to memory across loop boundaries.
  3. **IR Bypasses**: Block-level branches and block linking bypass the IR completely and manipulate host machine code displacements directly.
* **Impact**: Emitting a standard conditional jump inside a block silently corrupts the register allocator's liveness state (the root cause of the FreeDOS `ropSETcc` crash). Contributors must navigate ad-hoc rules about where control flow is legal.

---

### 1.2 Leaky Abstraction: Synchronous C Dispatcher vs. Autonomous Block Graph
* **The Root Conflict**: 86Box's core CPU model was architected around a synchronous C loop ([`exec386_dynarec_dyn()`](file:///home/marm/src/86Box/src/cpu/386_dynarec.c#L511)). The C loop served as the central arbiter: between every single block execution, it evaluated pending interrupts, NMIs, SMIs, cycle expiration, reset requests, TLB changes, and dirty page lists.
* **The Clash**: Direct Block Chaining (Phase 4) connects blocks via raw host `jmp rel32` instructions, bypassing the C dispatcher entirely for linked chains.
* **The Retrofitted Shims**: Because the core emulator was never designed for an autonomous execution graph, the JIT had to retroactively patch safety mechanisms into the block exit paths:
  - Unified `cpu_state.exit_request` polling emitted into every block exit sequence.
  - Eager dirty-write flushing ([`codegen_flush_code_writes()`](file:///home/marm/src/86Box/src/cpu/386_dynarec.c#L517)) on entry to the dispatcher.
  - Doubly-linked incoming link lists (`first_incoming_link`, `prev_link`, `next_link`) to unpatch machine code when blocks are evicted or dirtied.
* **Impact & Residual Risk**:
  - This architectural disconnect directly caused the **Windows 98 Device Manager `#GP` crash** ([`DEVICEMANAGER-CRASH.md`](file:///home/marm/src/86Box/src/codegen_new/DEVICEMANAGER-CRASH.md)). `DIBENG.DLL` generated new blit code into RAM, dirtied the page, but running blocks jumped directly past the C dispatcher into stale compiled blocks because the JIT execution graph operated asynchronously from the emulator's memory invalidation subsystem.
  - **Unresolved 2-Page Block Hazard**: Even after `codegen_flush_code_writes()`, blocks spanning a page boundary (`block->page_mask2` / `phys_2`) only validate their second page's physical mapping inside [`exec386_dynarec_dyn()`](file:///home/marm/src/86Box/src/cpu/386_dynarec.c#L607-L637). Entering a cross-page block via a direct link still bypasses the `phys_2` check.

---

### 1.3 Sentinel Overloading in Opcode Recompilation (`new_pc == 0` & `-1` Collisions)
* **The Pattern**: In [`codegen.c`](file:///home/marm/src/86Box/src/codegen_new/codegen.c#L733), handlers in `recomp_op_table` return an unsigned integer `new_pc`:
  - `0` is interpreted as: *"Instruction could not be recompiled; fall back to the C interpreter function (`codegen_skip`)."*
  - Non-zero values are interpreted as: *"Recompiled successfully; update `cpu_state.pc` to this target PC."*
  - `-1` (`0xFFFFFFFF`) is interpreted as: *"Handled, target PC already updated internally or dynamic."*
* **The Defect**: In x86 (especially segmented 16-bit real/protected mode or newly loaded 32-bit DLL modules), **offset `0x00000000` is a completely standard entry point and jump target** (and `0xFFFFFFFF` is a reachable wrap-around offset).
* **Impact & Remaining Exposure**:
  - This caused the **Windows 98 SE Control Panel Hang** ([`BUGS.md`](file:///home/marm/src/86Box/src/codegen_new/BUGS.md#L7)), where `call 0x0` in `ropCALL_r32` returned `0`, causing both the JIT uOps and interpreter `opCALL_r32` to execute and double-push the return address.
  - **Still Unfixed Handlers**: While `ropCALL_r32` was patched to return `-1`, [`ropJMP_r8()`](file:///home/marm/src/86Box/src/codegen_new/codegen_ops_jump.c#L31), [`ropJMP_r16()`](file:///home/marm/src/86Box/src/codegen_new/codegen_ops_jump.c#L46), and [`ropJMP_r32()`](file:///home/marm/src/86Box/src/codegen_new/codegen_ops_jump.c#L59) **still return `dest_addr`**, and [`ropJcc_16()`](file:///home/marm/src/86Box/src/codegen_new/codegen_ops_branch.c#L179) returns `next_pc = (op_pc + 2) & 0xffff`. A jump targeting `0x0` (or a 16-bit branch wrapping at `0xFFFF -> 0x0000`) mutates `block->exit_type` and then returns `0`, triggering either the `#ifndef NDEBUG` `fatal()` assertion (if `codegen_can_unroll` emitted uOps) or an interpreter fallback with corrupted block exit metadata. Conversely, `dest_addr == 0xFFFFFFFF` collides with `-1` and skips updating `IREG_pc`. Mixing error signaling and valid address domain values in a single scalar return value is an architectural defect.

---

## 2. Overly Complex Areas

### 2.1 Uninlined Condition Calls & Bitwise Multiplexing in `CMOVcc` / `SETcc`
* **The Current Implementation**: [`ropCMOVcc_common()`](file:///home/marm/src/86Box/src/codegen_new/codegen_ops_mov.c#L736) evaluates conditional moves branchlessly using:
  1. `uop_CALL_FUNC_RESULT(ir, IREG_temp1, jit_cond_helpers[cond & 0xf])`
  2. `uop_MOV_IMM(ir, IREG_temp2, 0)`
  3. `uop_SUB(ir, IREG_temp2, IREG_temp2, IREG_temp1)` (generating mask `0` or `~0`)
  4. `uop_MOV(ir, IREG_temp3, src_reg)`
  5. `uop_XOR(ir, IREG_temp3, IREG_temp3, dst_ireg)`
  6. `uop_AND(ir, IREG_temp3, IREG_temp3, IREG_temp2)`
  7. `uop_XOR(ir, dst_ireg, dst_ireg, IREG_temp3)`
* **Why it's over-complex and slow**:
  1. **Barrier C Call Overhead**: `uop_CALL_FUNC_RESULT` (also used by [`ropSETcc()`](file:///home/marm/src/86Box/src/codegen_new/codegen_ops_misc.c#L699)) is a full `UOP_TYPE_BARRIER`—it flushes and invalidates *all* cached guest registers to `cpu_state` and calls a C helper on every execution, bypassing the Phase 5 inline flag fast paths in [`codegen_cond_jump_false()`](file:///home/marm/src/86Box/src/codegen_new/codegen_ops_branch.c#L30).
  2. **7-uOp Bitwise Select**: It consumes 6 arithmetic uOps and 3 temporary scratch registers to perform a conditional select that modern host CPUs (x86-64 `cmovcc`, ARM64 `csel`, LoongArch64 `masknez`/`maskeqz`) perform natively.
* **Better Design**: Share inline condition evaluation with `SETcc`/`CMOVcc` and provide a native `UOP_SELECT` / `UOP_CMOV` uOp so each backend emits its native conditional select without a C call barrier.

---

### 2.2 Host-Specific Sub-Register Constraints Leaking into the Middle-End
* On x86-64, registers `R8`–`R15` cannot access high-byte sub-registers (`AH`, `BH`, `CH`, `DH`)—nor can `AH`–`DH` be encoded in *any* instruction that uses a REX prefix (because REX turns byte register encodings `4`–`7` into `SPL`, `BPL`, `SIL`, `DIL`).
* Because the x86-64 backend directly emits native `AH`/`BH`/`CH`/`DH` instructions ([`codegen_backend_x86-64_uops.c:34`](file:///home/marm/src/86Box/src/codegen_new/codegen_backend_x86-64_uops.c#L34)), this encoding restriction permeates the target-independent register allocator in [`codegen_reg.c`](file:///home/marm/src/86Box/src/codegen_new/codegen_reg.c#L624):
  - `uop_has_bh` global flag
  - `HOST_REG_FLAG_NO_BH` flag checks
  - Host register lockout and eviction loops inside `codegen_reg_alloc_register()` when any operand uses `IREG_SIZE_BH`
* **Trade-off**: Note that `HOST_REG_FLAG_NO_BH` *cannot* simply be moved into the x86-64 emitter unless the x86-64 backend stops emitting native `AH`/`BH`/`CH`/`DH` instructions on general registers (and instead extracts/inserts bits `8..15` via shifts or a dedicated non-REX scratch register like `ECX`, similar to how [`codegen_backend_arm64_uops.c`](file:///home/marm/src/86Box/src/codegen_new/codegen_backend_arm64_uops.c#L50-L60) uses `LSR #8` and `BFI`). Doing so in the x86-64 backend would eliminate `uop_has_bh` and prevent `R10`–`R15` from being locked/evicted across high-byte operations.

---

### 2.3 Divergent Epoch & Burst Tracking on ARM64 vs. x86-64
* In [`386_dynarec.c`](file:///home/marm/src/86Box/src/cpu/386_dynarec.c#L60-L75), ARM64 incorporates a complex temporal burst-tracking state machine:
  - `dynarec_s03e_dirty_epoch`
  - `DYNAREC_S03B_NO_IMM_THRESHOLD` (3 consecutive retries)
  - `DYNAREC_S03E_BURST_GAP_MAX` (temporal burst gap limit of 64)
* Meanwhile, x86-64 immediately promotes dirty-list hits to `CODEBLOCK_NO_IMMEDIATES`.
* Dirty-list retry hysteresis models *guest* self-modifying code patterns, not host CPU properties. Maintaining separate invalidation heuristics across host architectures in the shared dispatcher introduces maintenance friction and makes reproducing timing-dependent SMC bugs difficult across platforms.

---

## 3. Broken, Fragile, or Risky Areas

### 3.1 Guest Cycle Accounting & Timing Desynchronization (The PCPBench Red Flag)
* **The Phenomenon**: In [`ROADMAP.md`](file:///home/marm/src/86Box/src/codegen_new/ROADMAP.md#L58), PCPBench (VESA Mode 100, 640x400 LFB) under `new_dynarec` jumped from **60.4 FPS** (and 60.2 FPS after Phase 1) to **208.7 FPS** (+245.5%).
* **Why this indicates a critical defect**: Benchmark FPS in 86Box is measured in **emulated guest time** (guest CPU cycles and timer ticks), not host wall-clock time. A faster JIT reduces host wall-clock execution time, but should *never* alter how many frames the emulated Pentium II renders per guest second.
* **Primary Technical Suspects**:
  1. **SVGA LFB Wait-State Cycle Clobber in In-Block `REP` Loops**: In [`ropREP_MOVS_common()`](file:///home/marm/src/86Box/src/codegen_new/codegen_ops_string.c#L300) and [`ropREP_STOS_common()`](file:///home/marm/src/86Box/src/codegen_new/codegen_ops_string.c#L224), `uop_MEM_STORE_REG` is emitted inside the loop body before `uop_SUB_IMM(ir, IREG_cycles, IREG_cycles, step_cycles)`. Because `UOP_MEM_STORE_REG` is only a `UOP_TYPE_ORDER_BARRIER` (which writes back dirty registers to `cpu_state` *without* invalidating host register caching), `IREG_cycles` stays cached in a host register across the store. When the store hits the VESA Linear Frame Buffer, the C handler (`writememll` $\rightarrow$ `svga_writel_linear`) deducts video memory wait-state cycles (`video_timing_write_l`) from `cpu_state.cycles` in memory—which the JIT immediately **overwrites** with its stale host register value (`IREG_cycles - 3`) at `uop_NOP_BARRIER`!
  2. **Flag Fast-Path / Peephole Miscompilation**: Alternatively or additionally, a Phase 5 flag fast-path or peephole pass may be miscompiling a timing/polling loop or skipping rendering work.
* In a PC emulator, an artificial 3.4x jump in guest-time throughput is an **emulation fidelity bug**, not a speedup.

---

### 3.2 W^X Violations and Unprotected Live Code Patching
* In [`codegen_allocator.c`](file:///home/marm/src/86Box/src/codegen_new/codegen_allocator.c#L107), executable memory is mapped with `PROT_READ | PROT_WRITE | PROT_EXEC` (RWX).
* In [`patch_jmp_disp()`](file:///home/marm/src/86Box/src/codegen_new/codegen_block.c#L531), relative jump displacements are patched live in-memory via `memcpy()`.
* **The Failure Modes**:
  1. **macOS Apple Silicon Bug**: While [`exec386_dynarec_dyn()`](file:///home/marm/src/86Box/src/cpu/386_dynarec.c#L741) wraps block compilation in `pthread_jit_write_protect_np(0)` / `(1)`, **both [`dynarec_link_previous_block()`](file:///home/marm/src/86Box/src/cpu/386_dynarec.c#L720) and [`codegen_flush_code_writes()`](file:///home/marm/src/86Box/src/cpu/386_dynarec.c#L517) call `patch_jmp_disp()` outside that window** while JIT write protection is active (`1`).
  2. **Hardened OS Compatibility**: Security environments enforcing strict W^X (OpenBSD, hardened Linux kernels with PaX/SELinux, and Windows with Arbitrary Code Guard) forbid RWX mappings entirely. Full compliance requires either `pthread_jit_write_protect_np` guards around all link/unlink patching or a **dual-mapped memory pool** (one RW mapping for emission/patching, one RX mapping for execution) plus instruction cache invalidation on non-coherent architectures.

---

### 3.3 FPU `CODEBLOCK_STATIC_TOP` Blanket Restriction in Linked Chains
* The x87 FPU uses a circular register stack (`ST(0)`–`ST(7)`) indexed by `cpu_state.TOP`.
* By default, `codegen_new` compiles FPU blocks with `CODEBLOCK_STATIC_TOP`, baking in static register offsets assuming `TOP` remains equal to `block->TOP`. If a block is later entered via the dispatcher with a different `cpu_state.TOP`, [`exec386_dynarec_dyn()`](file:///home/marm/src/86Box/src/cpu/386_dynarec.c#L693-L706) clears `CODEBLOCK_STATIC_TOP` and recompiles the block using `IREG_TOP_diff` (dynamic `TOP` indexing).
* Currently, [`dynarec_link_previous_block()`](file:///home/marm/src/86Box/src/cpu/386_dynarec.c#L497) unconditionally refuses to link to *any* destination block that has `CODEBLOCK_STATIC_TOP` set (`!(dst_block->flags & CODEBLOCK_STATIC_TOP)`). Because almost all FPU blocks start with `CODEBLOCK_STATIC_TOP`, this blanket check disables block chaining into nearly all FPU blocks—even when `src_block` and `dst_block` share the exact same static `TOP`!
* **Fix**: Allow linking to `CODEBLOCK_STATIC_TOP` blocks when `src_block` has a known static exit `TOP` matching `dst_block->TOP`; if a link is attempted with a mismatched `TOP`, clear `CODEBLOCK_STATIC_TOP | CODEBLOCK_WAS_RECOMPILED` on `dst_block` so it recompiles with dynamic `IREG_TOP_diff` support.

---

### 3.4 32-bit Displacement Range Assumptions
* While [`codegen_link_exit()`](file:///home/marm/src/86Box/src/codegen_new/codegen_block.c#L630) checks `delta < INT32_MIN || delta > INT32_MAX` in release builds, [`patch_jmp_disp()`](file:///home/marm/src/86Box/src/codegen_new/codegen_block.c#L535) itself relies on `assert(delta >= INT32_MIN && delta <= INT32_MAX)` (which compiles out under `NDEBUG`).
* If the allocator is ever expanded to manage multiple non-contiguous memory slabs, or if an unlinked stub target sits outside `int32_t` range on a 64-bit host, `patch_jmp_disp()` should enforce a release-safe `fatal()` check rather than `assert()`.

---

## 4. Priority Action Items

| Priority | Issue | Action Item |
| :--- | :--- | :--- |
| **P0** | **Return-0 / `-1` Sentinel Collision** | Refactor `recomp_op_table` handlers (especially `ropJMP_r8/16/32` and `ropJcc_16`) to use an explicit status protocol (`RECOMP_HANDLED`, `RECOMP_FALLBACK`, or always emit `uop_MOV_IMM(ir, IREG_pc, ...)` and return a non-address sentinel) to eliminate PC `0` and `0xFFFFFFFF` aliasing bugs. |
| **P0** | **PCPBench Guest-Time Anomaly & `IREG_cycles` Clobber** | Fix `IREG_cycles` caching across `UOP_MEM_STORE_REG` / `UOP_MEM_LOAD_REG` inside in-block `REP` loops so C video/MMIO handlers' cycle deductions (`video_timing_write_l`) are not overwritten; bisect PCPBench to verify guest FPS returns to ~60.4 FPS. |
| **P1** | **Cross-Page Block Linking (`phys_2`)** | Either disallow direct linking into 2-page blocks (`block->page_mask2 != 0`) or validate `phys_2` on page map changes so cross-page linked blocks cannot execute stale second pages. |
| **P1** | **Inline Condition Fast-Paths & `UOP_SELECT` for `CMOVcc`/`SETcc`** | Replace the `UOP_CALL_FUNC_RESULT` barrier call in `ropCMOVcc_common` and `ropSETcc` with inline flag fast-paths and a native `UOP_SELECT` (`cmovcc` / `csel`) uOp. |
| **P1** | **W^X Protection & Dual Mapping** | Guard `patch_jmp_disp()` with `pthread_jit_write_protect_np` on macOS ARM64, and move `codegen_allocator.c` toward dual RW/RX mappings for strict W^X hosts. |
| **P2** | **Enable Static/Dynamic `TOP` FPU Block Chaining** | Track per-block FPU `TOP` delta so `dynarec_link_previous_block` can link into `CODEBLOCK_STATIC_TOP` blocks when exit `TOP` matches `dst_block->TOP`, or trigger a non-`STATIC_TOP` recompile when `TOP` rotates. |
| **P2** | **Unify Dirty-List Hysteresis & Isolate `IREG_SIZE_BH`** | Make the ARM64 dirty-list retry burst policy in `386_dynarec.c` architecture-independent, and consider lowering `IREG_SIZE_BH` in the x86-64 backend without REX/BH allocator lockouts. |

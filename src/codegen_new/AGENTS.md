# `codegen_new` Agent & Debugging Guide

This document captures essential architectural knowledge, debugging workflows, test harnesses, and hard-won post-mortem lessons for AI pair-programmers and engineers working on the `codegen_new` (New Dynarec) codebase in 86Box.

---

## 1. Quick Architectural Reference

### Block Compilation & Execution Lifecycle
1. **Lookup / Cache**:
   - `exec386_dynarec_dyn()` in [`src/cpu/386_dynarec.c`](file:///home/marm/src/86Box/src/cpu/386_dynarec.c).
   - `phys_addr = get_phys(cs + cpu_state.pc)`.
   - Block match requires: `block->pc == cs + cpu_state.pc`, `block->_cs == cs`, `block->phys == phys_addr`, status flags, and segment flatness.
2. **IR Generation**:
   - Instruction opcodes dispatch to `recomp_op_table` handlers (e.g. `ropJ`, `ropJMP_r8`, `ropMOV`, `ropALU`).
   - Instructions emit intermediate uOps into `ir_data_t` via `uop_*` functions.
   - Lazy flags are maintained in `cpu_state.flags_op`, `flags_res`, `flags_op1`, `flags_op2`.
3. **Backend Compilation**:
   - `codegen_ir_compile()` allocates physical host registers and emits host machine code (x86-64 in [`codegen_backend_x86-64_uops.c`](file:///home/marm/src/86Box/src/codegen_new/codegen_backend_x86-64_uops.c)).
4. **Direct Block Chaining (Phase 4)**:
   - `codeblock_link_t links[2]` in `codeblock_t`:
     - Exit 0: Taken branch (unconditional JMP or conditional Jcc taken).
     - Exit 1: Fallthrough branch (conditional Jcc not taken).
   - `direct_entry_pos`: Host code offset in the block prologue directly after caller-saved register pushes and base setup (skips frame allocation and base reload).
   - `codegen_EXIT_BLOCK`: Emits cycle expiry check (`cmp [rbp + cycles], 0; jle`), exit request / async event check (`cmp [rbp + exit_request], 0; jne`), maskable interrupt check (`pic.int_pending && IF`), and the direct branch jump `jmp rel32` (patched to destination's `direct_entry_pos` when linked, or to `unlinked_stub`).

---

## 2. Hard-Won Gotchas & Post-Mortem Lessons

### 1. The Windows 98 Premature Shutdown Bug ("It is now safe to turn off your computer")
- **Symptom**: Windows 98 boots past the splash screen, initializes hardware, but right as the interactive desktop is about to appear, it suddenly aborts to `LOGOW.SYS` ("It is now safe to turn off your computer").
- **Root Cause**: When migrating branch instructions (`ropLOOP`, `ropLOOPE`, `ropLOOPNE`) to direct branch linking (`BLOCK_EXIT_COND_2`), `uop_SUB_IMM(..., 1)` was omitted. `CX`/`ECX` was never decremented.
- **Mechanism**: Windows 98 VxDs use driver timeout loops:
  ```asm
  mov cx, 1000
  .wait:
      in al, dx
      test al, 1
      loop .wait
  ```
  If `CX` never decrements, the loop spins indefinitely until a driver timeout triggers. `VMM32.VXD` fails initialization and returns error code `0` to `WIN.COM`, which invokes `INT 2Fh AX=160Eh BL=3` (clean shutdown) and halts.
- **Rule**: In `codegen_ops_branch.c` and `codegen_block.c`, loop instructions **must** emit register decrements before condition checks.

### 2. Runtime Lazy Flags vs. Speculative Compile-Time Evaluation
- **Symptom**: Inverted conditional jumps, crashes, or premature loop exits.
- **Root Cause**: Speculatively reading `cpu_state.flags_op` inside `codegen_cond_jump_false` at block compile-time.
- **Rule**: `cpu_state.flags_op` represents the **runtime guest CPU state**, which changes dynamically during guest execution. Never evaluate flags with a compile-time C `switch (cpu_state.flags_op)`. Always emit runtime calls to `jit_cond_helpers[cond & 0xf]` or backend conditional branch uOps.

### 3. Real-Mode Address Masking (`CS:IP` vs `_cs`)
- In real mode, `_cs = CS << 4` (linear base).
- Guest `IP` is 16-bit. When jumping or calculating target PC in 16-bit / real mode:
  ```c
  uint32_t dst_pc = (src_block->status & CPU_STATUS_USE32) ?
                    (dst_block->pc - dst_block->_cs) :
                    ((dst_block->pc - dst_block->_cs) & 0xffff);
  ```
### 4. Self-Modifying Code (SMC) & Direct Block Linking
- **Symptom**: In 16-bit real mode / DOS (FreeDOS), disk/filesystem corruption, MCB errors, or "can't get drive parameters" during early boot under `LINK_REAL`.
- **Root Cause**: DOS kernels and BIOS callers use a classic serialization trick for dynamic software interrupts:
  ```asm
  mov [cs:patch_int+1], al    ; overwrite INT vector number
  jmp short $+2               ; flush prefetch queue
  patch_int:
  int 00h                     ; dynamically patched interrupt
  ```
  With direct block linking, the `JMP short $+2` was linked directly to `patch_int`. Because the execution jumped directly across blocks on the host without returning to C dispatch (`exec386_dynarec_dyn()`), `page->dirty_mask` checks were skipped and `patch_int` executed with stale compiled code (e.g. `INT 11h` equipment check instead of `INT 13h` disk I/O).
- **Rule**: In `src/mem/mem.c`, any store writing into memory with active compiled code (`page->code_present_mask & mask` or `byte_code_present_mask & byte_mask`) **must** assert `cpu_state.exit_request |= EXIT_STATUS_BLOCK_END`. `codegen_EXIT_BLOCK` checks `exit_request`, immediately aborting any direct linked branch and forcing a return to C dispatch, which detects `page->dirty_mask`, invalidates the stale block, and recompiles with fresh guest bytes.

### 5. Single-Basic-Block Assumption & Intra-Block Conditional Branching (`SETcc` / `CMOVcc`)
- **Symptom**: FreeDOS crashes on boot with `JemmEx: exception 0D occured at CS:EIP=F000:0000ADF2, ERRC=00000000 [CS:IP] = E4 64 E6 EB 24 02 E0 F8 (in al, 64h)`.
- **Root Cause**: `ropSETcc` emitted an intra-block conditional branch (`codegen_cond_jump_false`) jumping over `uop_MOV_IMM(dest_reg, 1)`.
- **Mechanism**: The `codegen_new` register allocator (`codegen_reg.c`) strictly assumes **linear single basic blocks** without internal control-flow splits or merges. When `uop_MOV_IMM(dest_reg, 1)` was compiled linearly, the compiler assigned `dest_reg` to a host register, updated its version, and marked the host register dirty. At runtime, when the condition was false, the jump bypassed the host `MOV`. At the end of the block (or on flush), the recompiler flushed the dirty host register—which still held uninitialized stale garbage—back to `cpu_state`, corrupting the guest register. In Jemm's V86 GPF handler, `setz ah` corrupted `EAX` (clobbering `AL` which held the opcode `0xE4`), causing Jemm to mistake the instruction for an unhandled opcode and route it to `INT 06h` / breakpoint halt.
- **Rule**:
  1. Never emit intra-block conditional jumps.
  2. Implement `SETcc` branchlessly: `jit_cond_helpers[cond & 0xf]()` already returns `1` (true) or `0` (false). Use `uop_CALL_FUNC_RESULT(ir, IREG_temp0, jit_cond_helpers[cond & 0xf])` followed directly by `uop_MOV(ir, IREG_8(dest_reg), IREG_temp0_B)` or `uop_MEM_STORE_REG`.
  3. Instructions requiring non-trivial branching control flow like `CMOVcc` must fall back to the interpreter (`return 0`) unless a branchless IR representation or proper block splitting is used.

---

## 3. Build & Deployment Rules

1. **Always build both configurations**:
   ```bash
   ninja -C build/new_dynarec -j$(nproc)
   ninja -C build/new_dynarec_dev_debug -j$(nproc)
   ```
2. **Deploy atomically to test installation**:
   ```bash
   cp build/new_dynarec/src/86Box /home/marm/Emulation/86Box/86Box.new && \
   mv -f /home/marm/Emulation/86Box/86Box.new /home/marm/Emulation/86Box/86Box
   ```
3. **Sandbox execution**:
   - If sandbox peer reset (`recvmsg: connection reset by peer`) occurs in IDE commands, execute with `BypassSandbox: true`.
   - Never leave orphan 86Box processes running in background (`pkill -9 86Box`).

---

## 4. Test Harnesses & Telemetry

### 1. Automated DOS Benchmark Suite (`benchmarks/run_benchmarks.py`)
- Automated headless runner reading real-time serial COM1 logs:
  ```bash
  python3 benchmarks/run_benchmarks.py \
      --only new \
      --vm-dir /home/marm/Emulation/86Box/benchmark \
      --roms /home/marm/Emulation/86Box/roms \
      --new-emu build/new_dynarec/src/86Box \
      --timeout 90
  ```
- Fast Quake sanity check:
  - Completes in ~45s.
  - Normal baseline under Pentium II 450: ~159.3 FPS (969 frames in 6.1s).
  - Headless environment relies on `QT_QPA_PLATFORM=offscreen` and `ALSOFT_DRIVERS=null`.

### 2. Windows 98 Headless Desktop Verification
- Full Windows 98 guest boot test:
  - Takes ~35–42s to reach interactive desktop with network and Explorer active.
  - Headless screenshot trigger via GDB injecting monitor screenshot counter increment:
    ```python
    # monitors[0].mon_screenshots offset is 0x27c (636 decimal)
    target_addr = hex(base + mon_off + 636)
    subprocess.run(["gdb", "-p", str(pid), "-batch", "-ex", f"p (*(int*){target_addr})++"])
    ```
  - Screenshots are saved into `<vm_dir>/screenshots/` and can be inspected as PNG artifacts.

---

## 5. Block-Linking Policy Bisection (`BOX86_LINK_MASK`)

Runtime policy can be configured without recompiling using the `BOX86_LINK_MASK` environment variable:

| Bit | Name | Meaning |
| :--- | :--- | :--- |
| `0x01` | `LINK_JMP` | Unconditional jumps (`JMP rel8/16/32`) |
| `0x02` | `LINK_JCC` | Conditional jumps (`Jcc rel8/16/32`) |
| `0x04` | `LINK_LOOP` | `LOOP`, `LOOPE`, `LOOPNE`, `JCXZ` |
| `0x08` | `LINK_REAL` | Real-mode and V86 blocks |
| `0x10` | `LINK_PMODE` | Protected-mode blocks |
| `0x20` | `LINK_SAME_PAGE`| Intra-page links only (source and target share physical page) |
| `0x40` | `LINK_NO_FPU` | Never link blocks using x87 FPU |

### Common Diagnostic Masks:
- `0x00`: Linking completely disabled (uses unlinked C dispatch stubs only).
- `0x31`: Unconditional `JMP` in PMODE, intra-page (`LINK_JMP | LINK_PMODE | LINK_SAME_PAGE`).
- `0x33`: `JMP` + `Jcc` in PMODE, intra-page.
- `0x37`: Protected-mode policy without `LINK_REAL` (`LINK_JMP | LINK_JCC | LINK_LOOP | LINK_PMODE | LINK_SAME_PAGE`).
- `0x3f`: **Standard Default Policy** (`LINK_JMP | LINK_JCC | LINK_LOOP | LINK_REAL | LINK_PMODE | LINK_SAME_PAGE`). Verified 100% stable on Windows 98 and DOS benchmarks.
- `BOX86_LINK_LIMIT=N`: Cuts off linking after N links and logs the Nth link details to identify diverging or corrupting blocks.

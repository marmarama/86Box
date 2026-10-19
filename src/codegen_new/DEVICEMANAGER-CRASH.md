# Windows 98 SE Device Manager `#GP` Crash Investigation (`DEVICEMANAGER-CRASH.md`)

This document consolidates all known forensics, runtime evidence, disassembly traces, tested hypotheses, and candidate root causes for the General Protection Fault (`#GP`) in Windows 98 SE when opening Device Manager under `new_dynarec`.

> **RESOLVED.** Root cause: stale blocks stayed reachable through direct links after the guest rewrote code (dirty bits are only processed lazily by the dispatcher, which linked jumps bypass). Fixed by eagerly flushing written pages at dispatcher entry (`codegen_flush_code_writes`). See Bug 4 in `BUGS.md`. The hypotheses below (including `is_a16`) were not the cause.

---

## 1. Summary of the Problem

- **Guest OS**: Windows 98 SE (4.10.2222 A) running in 86Box.
- **Trigger**: Opening the **Device Manager** tab in System Properties (`SYSDM.CPL` via `RUNDLL32`).
- **Symptom**: An illegal operation dialog appears:
  ```text
  RUNDLL32 caused a general protection fault
  in module <unknown> at 0000:0000093e.
  ```

---

## 2. Register & Architectural State at Fault

From the live crash dialog and register dumps:
```text
EAX = a0000a04   CS = 04df   EIP = 0000093e   EFLGS = 00000202
EBX = 00000000   SS = 04c7   ESP = 00000040   EBP   = 00000008
ECX = 000004c7   DS = 36a7   ESI = 00000604   FS    = 04bf
EDX = 04df0934   ES = 10ef   EDI = 00000a00   GS    = 0000

Bytes at CS:EIP:
26 89 07 83 c7 04 4d 75 f2 81 c7 c0 ff ff ff 81
Stack dump:
00000000 7a762aa7 00000000 00000000 00000000 00000000
```

### Segment Descriptors
- **CS Selector `0x04df`**: Base `0x803385c0`, Limit `0x313f`, USE32, Code R/E.
- **ES Selector `0x10ef`**: Base `0x81cf0fd8`, Limit `0x000009ff` (2560 bytes total buffer: `0x000` to `0x9ff`), Data R/W.
- **FS Selector `0x04bf`**: Data selector alias pointing to the same linear memory as CS `0x04df`.

### The Faulting Instruction
```asm
0000093E: 26 89 07    mov [es:edi], eax
```
- At the time of execution, **`EDI = 0x00000a00`**.
- The instruction attempts to write a 4-byte dword (`EAX`) at `ES:0x0a00` (`0x0a00 .. 0x0a03`).
- Because `ES.limit = 0x09ff`, offset `0x0a00` exceeds `ES.limit` by 1 to 4 bytes.
- The 86Box segment limit check correctly triggers an architectural General Protection Fault (`#GP(0)`).

---

## 3. Disassembly of the Guest Code

The code executing at `04df:0934` is a **dynamically generated blit routine** compiled into RAM by the Windows 98 DIB engine (`DIBENG.DLL`) to render icons and graphics with transparency:

```asm
; --- Blit Loop Body ---
00000939: ...                   ; Per-pixel or per-dword blending setup
0000093E: 26 89 07              mov [es:edi], eax     ; <--- #GP FAULT (EDI = 0x0a00)
00000941: 83 C7 04              add edi, 4
00000944: 4D                    dec ebp
00000945: 75 F2                 jnz 0x939             ; Loop back 14 bytes to 0x939
00000947: 81 C7 C0 FF FF FF     add edi, -64          ; Scanline wrap / stride adjustment
```

### Buffer Geometry & Loop Parameters
- The destination row width is **64 bytes** (16 dwords of 4 bytes).
- Buffer total size: **40 scanlines** $\times$ **64 bytes** = **2,560 bytes** (`0xa00` bytes, limits `0x000 .. 0x9ff`).
- The inner loop decrements `EBP` until zero (`16 * 4 = 64 bytes`).
- At the point of crash, `EDI` has advanced to `0x0a00` and `EBP = 8`, indicating either:
  1. The loop executed an unintended extra iteration (outer scanline counter overflow).
  2. The initial `EDI` value was offset/miscalculated.
  3. The loop terminating branch condition was mispredicted or miscalculated.

---

## 4. Solid Ground Truths Established

1. **Interpreter Works Perfectly**:
   - Setting `cpu_force_interpreter = 1` allows Device Manager to open 100% cleanly without fault.
   - This proves the guest OS code and DIB engine logic are correct; the defect is strictly in `codegen_new` dynamic recompilation or execution.

2. **Block Linking Mask Controls the Bug**:
   - Setting `BOX86_LINK_MASK = 0` (or `codegen_link_mask = 0`) **prevents the crash**.
   - Setting default linking (`BOX86_LINK_MASK = 0xbf`) **reproduces the crash reliably**.
   - This proves the failure depends directly on direct block linking (`jmp rel32` chaining).

---

## 5. Hypotheses Tested & Status

### A. Hypothesis 1: Physical Page Write Invalidation (`writelookup2`)
- **Theory**: When `DIBENG.DLL` generates new blit code into RAM, writes via data selectors (`DS`/`ES`) fail to invalidate cached code blocks if `writelookup2` page tracking doesn't match cross-selector aliases.
- **Action**: Modified `mem_flush_write_page` in `src/mem/mem.c` to match against physical `pages[addr >> 12].mem` directly rather than guest virtual addresses.
- **Outcome**: Correct and essential fix for MMU lookup, but Device Manager still crashed.

### B. Hypothesis 2: 16-bit Addressing in Far Pointer Loads (`is_a16`)
- **Theory**: In `DIBENG.DLL` Segment 14, `66 C4 BE F6 FE (les edi, [bp-0x10a])` uses 32-bit operand size with 16-bit addressing.
- **Action**:
  - In `codegen_ir_defs.h`, removed `uop->is_a16 = 0;` which was clobbering `IREG_eaa16`.
  - In `codegen_backend_x86-64_uops.c` (and ARM64/LoongArch64), fixed `codegen_MEM_LOAD_REG` to calculate `seg_reg + ((addr_reg + imm_data) & 0xffff)` instead of masking the segment base.
- **Outcome**: Eliminates genuine selector/pointer corruption in 16-bit `LES`/`LDS`, but Device Manager still crashes at `04df:0000093e`.

---

## 6. Prime Candidate Root Causes to Investigate

Given that `BOX86_LINK_MASK = 0` avoids the bug and `0xbf` triggers it, the issue is directly inside the **Block Linking** mechanism. The bits in `BOX86_LINK_MASK` are:
```c
#define LINK_JMP       (1 << 0) /* 0x01: direct JMP rel */
#define LINK_JCC       (1 << 1) /* 0x02: Jcc rel */
#define LINK_LOOP      (1 << 2) /* 0x04: JCXZ / LOOP / LOOPE / LOOPNE */
#define LINK_REAL      (1 << 3) /* 0x08: real mode / V86 */
#define LINK_PMODE     (1 << 4) /* 0x10: protected mode */
#define LINK_SAME_PAGE (1 << 5) /* 0x20: source and target share physical page */
#define LINK_NO_FPU    (1 << 6) /* 0x40: never link blocks with FPU */
#define LINK_RET       (1 << 7) /* 0x80: monomorphic near RET linking */
```
Default: `0xbf` (`LINK_JMP | LINK_JCC | LINK_LOOP | LINK_REAL | LINK_PMODE | LINK_SAME_PAGE | LINK_RET`).

### Candidate 1: `LINK_RET` (Return Linking / Monomorphic Return Prediction)
- `LINK_RET` (`0x80`) was added in Phase 4b.
- When a subroutine returns, `dynarec_link_previous_block` links the `BLOCK_EXIT_RET` block directly to the return address's block.
- In `codegen_EXIT_BLOCK_RET`:
  ```c
  /* Compares popped return address in cpu_state.pc against predicted target_pc */
  host_x86_MOV32_REG_BASE_OFFSET(block, REG_EAX, REG_RBP, pc_disp);
  host_x86_MOV32_REG_ABS(block, REG_EDX, &block->links[exit_idx].target_pc);
  host_x86_CMP32_REG_REG(block, REG_EAX, REG_EDX);
  uint32_t *jne_unlinked = host_x86_JNZ_long(block);
  ...
  block->links[exit_idx].jmp_disp_ptr = host_x86_JMP_long(block);
  ```
- **Risk**: If a helper function in `DIBENG.DLL` returns to a caller, and later that same helper is invoked from another location, does the prediction check fail safely, or does a race/stale target jump to the wrong blit setup code?
- **Test**: Set `BOX86_LINK_MASK = 0x3f` (disables only `LINK_RET`, keeps JMP, JCC, LOOP).

### Candidate 2: Self-Modifying Code Inside Linked Direct Jumps (`LINK_JCC` / `LINK_JMP`)
- When `block A` links to `block B` via a direct `jmp rel32`, `block A` completely bypasses the C dispatcher (`exec386_dynarec_dyn`).
- When Windows writes new code into the page occupied by `block B`:
  - `codegen_unlink_block` unpatches incoming links pointing to `block B`.
  - **However**: If `block A` and `block B` are executing in an active loop or if `block A` has already jumped to `block B`, or if unlinking misses an alias, old machine code continues to run.
  - Specifically, if `block B` was compiled with an older row/stride count, executing it against a newly allocated 2560-byte buffer causes out-of-bounds writes.

### Candidate 3: Conditional Branch Flag Resolution in Linked Blocks
- In `codegen_ops_branch.c`:
  `codegen_cond_jump_false` uses fast-paths for Zero Flag (`COND_E`, `COND_NE`) and Sign Flag (`COND_S`, `COND_NS`).
- When blocks are linked:
  - Exit 0 (`links[0]`) is taken when branch condition is TRUE.
  - Exit 1 (`links[1]`) is taken when branch condition is FALSE (fall-through).
- In `codegen_block.c`:
  ```c
  jump_false = codegen_cond_jump_false(ir_data, block->exit_cond);
  ...
  uop_EXIT_BLOCK(ir_data, 0, block->links[0].target_pc);
  uop_set_jump_dest(ir_data, jump_false);
  uop_EXIT_BLOCK(ir_data, 1, block->links[1].target_pc);
  ```
- If `jump_false` evaluates flags differently from the interpreter (for example, if `codegen_last_flags_op` is stale across block boundaries or if `IREG_flags_res` contains non-zero data in upper bits from previous operations), the loop may take an extra iteration.

---

## 7. Next Bisection Steps

1. **Test `BOX86_LINK_MASK` Masks**:
   - `BOX86_LINK_MASK = 0x3f`: Disables `LINK_RET` (bit 7). Isolates whether return linking causes the crash.
   - `BOX86_LINK_MASK = 0xbd`: Disables `LINK_JCC` (bit 1). Isolates whether conditional branch linking causes the loop overflow.
   - `BOX86_LINK_MASK = 0xbe`: Disables `LINK_JMP` (bit 0).

2. **Instrument Blit Loop Block Exits**:
   Log every transition into and out of guest blocks in `04df:0934 .. 04df:0960` with register values (`EDI`, `EBP`, `EAX`) to observe the exact step where `EDI` crosses from `0x09fc` to `0x0a00`.

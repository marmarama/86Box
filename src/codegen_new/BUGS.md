# Dynarec Known Bugs and Investigations (`codegen_new`)

This document records confirmed bugs, deep root-cause investigations, reproduction traces, and fixes in `codegen_new`.

---

## Bug 1: Windows 98 SE Control Panel Hang (Stack Misalignment / Double CALL Push)

- **Status**: RESOLVED & VERIFIED
- **Component**: `src/codegen_new/codegen.c`, `src/codegen_new/codegen_ops_jump.c`
- **Severity**: Critical (Breaks Windows 98 SE Explorer / Control Panel, causes silent stack corruption and infinite loops)
- **First Discovered**: October 2026
- **Resolution**: `ropCALL_r32` updated to emit `uop_MOV_IMM(ir, IREG_pc, dest_addr)` and return `-1`. Guard added to `codegen.c` in `#ifndef NDEBUG` asserting that handlers returning 0 (fallback) emit 0 uOps. Manually verified in Windows 98 SE Control Panel.

---

### 1. Symptom

In Windows 98 SE running on `new_dynarec`, opening the **Control Panel** opens a window frame with a blank white client area. The icons never render, and the window never finishes loading.
- The mouse pointer remains responsive.
- The emulator does not crash.
- The Explorer GUI thread becomes trapped in an infinite loop in the polygon / GDI rasterizer.

---

### 2. Runtime Forensic Evidence (Live VM PID 172234)

Attaching GDB to the live VM revealed the guest CPU was trapped in an infinite DDA scanline edge interpolation loop inside the graphics subsystem:

#### The Hang Loop (Guest PC `0x785`):
```asm
0x785: cmp dx, 0
0x789: jg  0x793
0x78b: add ax, cx
0x78e: add dx, bx
0x791: jmp 0x785
```
At runtime:
- `esi = 0x00000c72`
- `ebx = 0x00000000`
- `edx = 0x00000000`
- `esp = 0xd104bb04` (`SP = 0xbb04`)

#### Why It Hung:
The outer loop at `0x6a2` iterates through an edge table:
```asm
0x6a2: cmp esi, ebx
0x6a5: jz  0x6bb
```
- `ESI` was initialized to the edge table start: `0x212` (530).
- `EBX` was supposed to hold the edge table end: `0x252` (594).
- However, at the start of the loop, **`EBX` contained `0xc5` (197)**!
- Because `530 > 197`, `ESI` never matched `EBX`, walked past the end of the edge table, and hit uninitialized / zero memory where `dx = 0` and `bx = 0`.
- The inner DDA loop at `0x785` then executed `add dx, 0; jmp 0x785` forever.

---

### 3. Tracing The Cause of Register Corruption: The 4-Byte Stack Shift

Tracing backwards to where `EBX` and `ECX` were restored revealed the caller block at guest PC `0x996` (Dynarec Block `0xd5f7`):

```asm
0x996: push ebx        ; Save edge table end (0x252 = 594)
0x997: push ecx        ; Save ECX (0xc5 = 197)
0x998: push eax        ; Save EAX (0xf13e)
0x999: push [ebp-0x24] ; Push argument (0x212)
0x99c: push eax        ; Push argument (0x0002)
0x99d: pushw $0x0      ; Push argument (0x0000)
0x9a1: call 0x0        ; CALL target offset 0x0 (Function 0)
0x9a6: pop eax
0x9a7: test byte [ebp+0x1a], 1
0x9ab: jz 0x9b6
0x9b6: pop ecx
0x9b7: pop ebx
```

#### Inspection of Physical Stack Memory at `SS:0xbaf0` .. `SS:0xbb10`:
```text
Address     Value        Description
-------------------------------------------------------------------------
SS:0xbb0c   0x00000252   Pushed EBX (edge table end: 594)
SS:0xbb08   0x000000c5   Pushed ECX (197)
SS:0xbb04   0x0000f13e   Pushed EAX
SS:0xbb00   0x00000212   Pushed [ebp-0x24]
SS:0xbafc   0x00000002   Pushed argument
SS:0xbafa   0x0000       Pushedw argument
SS:0xbaf6   0x000009a6   RETURN ADDRESS #1 (Emitted by JIT uops)
SS:0xbaf2   0x000009a6   RETURN ADDRESS #2 (Pushed by interpreter opCALL_r32!)
SS:0xbaf0   0xbb72       Callee saved BP
```

#### The Stack Misalignment Cascade:
1. Callee `Function 0` executes `leave; ret 6`. It pops BP, and `ret 6` pops the return address and 6 argument bytes.
2. Because of the duplicate return address on the stack, `ret 6` popped `0x9a6` from `0xbaf2` and added 6 to SP.
3. This left `SP` pointing to **`0xbb00`** instead of **`0xbb04`**!
4. The caller resume sequence then executed:
   - `0x9a6: pop eax`: Popped from `0xbb00` (`0x212` instead of `0xf13e`), advancing `SP` to `0xbb04`.
   - `0x9b6: pop ecx`: Popped from `0xbb04` (`0xf13e` instead of `0xc5`), advancing `SP` to `0xbb08`.
   - `0x9b7: pop ebx`: Popped from `0xbb08` (**`0xc5`** instead of `0x252`), advancing `SP` to `0xbb0c`!
5. **`0x252` at `0xbb0c` was never popped**, and `EBX` received `0xc5` (197)!

---

### 4. Root Cause in JIT Code Generation (`src/codegen_new/codegen.c`)

Disassembly of host machine code emitted for Block `0xd5f7` (host `0x7a4767b3a740`):
```x86asm
; Uops emitted by ropCALL_r32:
0x7a4767b3aa3c: mov    $0x9a6, %ecx
0x7a4767b3aa64: mov    %ecx, (%rdi,%rsi,1)     ; Stores 0x9a6 to stack
0x7a4767b3aa79: sub    $0x4, %dx               ; Decrements SP by 4
0x7a4767b3aaae: mov    %edx, -0x70(%rbp)       ; Writeback ESP

; Followed immediately by:
0x7a4767b3aab6: movabs $0x64312d66d0b0, %r9   ; Address of interpreter opCALL_r32!
0x7a4767b3aac0: call   *%r9                    ; Calls opCALL_r32!
```

In `src/cpu/x86_ops_jump.h`:
```c
static int
opCALL_r32(UNUSED(uint32_t fetchdat))
{
    int32_t addr = getlong();
    if (cpu_state.abrt) return 1;
    PUSH_L(cpu_state.pc);   /* <--- DUPLICATE PUSH TO STACK! */
    cpu_state.pc += addr;
    CPU_BLOCK_END();
    return 0;
}
```

#### Why Did `codegen.c` Emit BOTH the Inlined Push AND the Fallback Call?

Look at `src/codegen_new/codegen.c` (lines 720–755):
```c
    if (recomp_op_table && recomp_op_table[(opcode | op_32) & recomp_opcode_mask]) {
        uint32_t new_pc = recomp_op_table[(opcode | op_32) & recomp_opcode_mask](block, ir, opcode, fetchdat, op_32, op_pc);
        if (cpu_dyn_accurate_fpu_env && is_fpu) {
            ...
        }
        if (new_pc) {
            if (new_pc != -1)
                uop_MOV_IMM(ir, IREG_pc, new_pc);

            codegen_endpc = (cs + cpu_state.pc) + 8;
            block->ins++;
            if (block->ins >= MAX_INSTRUCTION_COUNT)
                CPU_BLOCK_END();
            return;
        }
    }

codegen_skip:
    ...
    op = op_table[((opcode >> opcode_shift) | op_32) & opcode_mask];
    ...
    uop_CALL_INSTRUCTION_FUNC(ir, op, fetchdat);
```

And look at `ropCALL_r32` in `src/codegen_new/codegen_ops_jump.c`:
```c
uint32_t
ropCALL_r32(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), UNUSED(uint32_t fetchdat), UNUSED(uint32_t op_32), uint32_t op_pc)
{
    uint32_t offset    = fastreadl(cs + op_pc);
    uint32_t ret_addr  = op_pc + 4;
    uint32_t dest_addr = ret_addr + offset;
    int      sp_reg;

    uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
    sp_reg = LOAD_SP_WITH_OFFSET(ir, -4);
    uop_MEM_STORE_IMM_32(ir, IREG_SS_base, sp_reg, ret_addr);
    SUB_SP(ir, 4);

    codegen_mark_code_present(block, cs + op_pc, 4);
    block->exit_type          = BLOCK_EXIT_DIRECT_1;
    block->links[0].target_pc = dest_addr;
    return dest_addr;
}
```

#### The Smoking Gun: Return Value Overloading
- The return value of handlers in `recomp_op_table` was designed with two conflicting conventions:
  1. Return `0` to signal **"Instruction could not be recompiled; fall back to interpreter function (`codegen_skip`)"**.
  2. Return `dest_addr` to tell `codegen.c` to emit `uop_MOV_IMM(ir, IREG_pc, dest_addr)` and finish the instruction.
  3. Return `-1` (`0xFFFFFFFF`) when the handler itself has already updated `IREG_pc` or when target PC is indirect/dynamic.
- At guest PC `0x9a1`, the instruction is:
  `0x9a1: call 0x0`
  Target is `0x0`.
  `ret_addr = 0x9a6`.
  `offset = -0x9a6`.
  `dest_addr = ret_addr + offset = 0x0`!
- Therefore, `ropCALL_r32` returned **`0`**!
- In `codegen.c`:
  `if (new_pc)` evaluated to **`FALSE`** because `new_pc == 0`!
- `codegen.c` falsely assumed that `ropCALL_r32` had rejected the instruction, fell through to `codegen_skip`, and emitted a call to `opCALL_r32`!
- At runtime, both the inlined uops AND the interpreter function executed, causing:
  1. Inlined uops pushed `0x9a6` and decremented `SP` by 4.
  2. `opCALL_r32` pushed `0x9a6` and decremented `SP` by 4 again!

---

### 5. Scope of Affected Instructions

Any recompiled branch, jump, or call instruction that returns `dest_addr` is vulnerable whenever the target PC within the code segment is `0x0`:
- `ropJMP_r8` (`codegen_ops_jump.c:31`)
- `ropJMP_r16` (`codegen_ops_jump.c:46`)
- `ropJMP_r32` (`codegen_ops_jump.c:59`)
- `ropCALL_r16` (Note: `ropCALL_r16` manually emits `uop_MOV_IMM(ir, IREG_pc, dest_addr)` and returns `-1`, avoiding this specific bug, but `ropCALL_r32` returned `dest_addr`).
- Any conditional branch or other jump where `dest_addr == 0`.

In x86 (especially segmented 16-bit or mixed 16/32-bit Windows code), code segments frequently have base addresses set such that the module entry point or function 0 begins at `offset 0x00000000`. Treating `0` as an error/fallback sentinel was an architectural bug.

---

### 6. The Fix

#### Approach A (Recommended & Uniform):
Make `ropCALL_r32` (and all other direct branch/call handlers) explicitly emit `uop_MOV_IMM(ir, IREG_pc, dest_addr)` and return `-1` (or use a dedicated non-zero sentinel / distinct return protocol), exactly like `ropCALL_r16`:

In `ropCALL_r32`:
```c
    uop_MOV_IMM(ir, IREG_pc, dest_addr);
    codegen_mark_code_present(block, cs + op_pc, 4);
    block->exit_type          = BLOCK_EXIT_DIRECT_1;
    block->links[0].target_pc = dest_addr;
    return -1;
```

#### Approach B (In `codegen.c`):
Alternatively or in addition, check `block->exit_type != BLOCK_EXIT_NONE` or define a distinct sentinel (such as `RECOMP_FALLBACK` / `0xfffffffe`) so that valid PC `0` is never mistaken for a fallback request.

---

### 7. Review comments (added in review)

**Verdict: agree with the root cause.** It matches the code. In the Phase 4b change, `ropCALL_r32`
went from `uop_MOV_IMM(ir, IREG_pc, dest_addr); ... return -1;` to `... return dest_addr;`. That is
a regression introduced by CALL linking (commit "implement Phase 4b subroutine and return
linking"), not an older bug. The duplicated return address on the stack (`0xbaf6` and `0xbaf2`)
is the classic fingerprint of "uops emitted *and* interpreter fallback called". `git blame` on
`ropCALL_r32` should confirm.

**Corrections to scope (section 5):**
- `ropJMP_r8/16/32` are *not* affected in the corrupting sense. They emit no uops before
  returning, so a `dest_addr == 0` fallback just runs the interpreter `JMP` (which ends the block);
  the only cost is a missed optimization. The bug needs a handler that has **already emitted
  side-effecting uops** and then returns a target PC. Today that is only `ropCALL_r32`
  (`ropCALL_r16` returns `-1`, and `shift.c:529` should be checked for the same pattern).
- The collision exists at the other end too: a target of `0xFFFFFFFF` is read as "handler already
  set `IREG_pc`", so the `MOV_IMM` of `IREG_pc` is skipped and `pc` is left stale. Rare, but the same
  sentinel overloading.
- Why it shows up in Win98: it needs a 32-bit-operand `CALL` (`0x66` prefix in 16-bit code, or a
  `CALL` in a mixed 16/32-bit module) whose target is offset 0 of the code segment. Flat 32-bit
  code almost never calls offset 0, which is why this survived boot and benchmarks.

**Fix recommendation:**
1. Do Approach A now. Emitting `MOV_IMM IREG_pc` is redundant with the `EXIT_BLOCK` stub's
   `pc` store but harmless, and it keeps `pc` correct on any path that doesn't reach an
   `EXIT_BLOCK` (for example when linking is disabled by policy).
2. Make the protocol unambiguous instead of relying on every handler remembering. Either:
   - give handlers a helper, e.g. `return rop_branch_target(ir, dest_addr);`, which emits the
     `MOV_IMM IREG_pc` and returns `-1`, and ban returning raw PCs; or
   - change the "not recompiled" signal to a dedicated constant (not `0`), and only check that.
3. Add a debug guard in `codegen.c` that catches the whole class: record the uop count before
   calling the handler; if the handler returns `0` (fallback) but emitted uops, `fatal()` in
   non-release builds. This would have caught this bug the first time `CALL 0` was compiled.
4. Add a regression test: a 16-bit code segment, `66 E8` `CALL` to offset 0 (and one to
   `0xFFFFFFFF`), then `RET`/`RET 6`, comparing `ESP` and return address against the interpreter.
   A differential fuzzer for branch targets near `0`, `0xFFFF`, `0xFFFFFFFF` would cover it.
5. After the fix, re-run the Control Panel repro and add it to the manual test list.

**Related items worth a look:** `BLOCK_PC_INVALID` (used for RET targets) should be a value that can
never be a legitimate popped PC, and any other place comparing PCs to `0` or `-1` as "none"
(for example `links[].target_pc` defaults) is the same trap.

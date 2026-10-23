# Windows 98 Boot Dynarec Telemetry & Bottleneck Profile

**Date**: October 23, 2026  
**Target VM**: `/home/marm/Emulation/86Box/Windows98`  
**Dynarec Engine**: `new_dynarec` (`x86-64`)  
**Dump File**: `scratch/win98_boot.86bdump` (222.88 MB, 126,660 compiled blocks, 612,022,137 recorded block entries)  
**Analysis Tools**: `src/codegen_new/codegen_dump.c`, `scripts/analyze_blocks.py`

---

## 1. Executive Summary

A full end-to-end boot profile of Windows 98 Second Edition was recorded from initial power-on through BIOS POST, MS-DOS real-mode boot, protected mode kernel transition, Windows GUI/desktop initialization, until the desktop settled at idle ($t = 75.5\text{ s}$).

* **Total Samples Logged**: 73 seconds
* **Average Real-Time (RT) Speed**: **70.9%**
* **Minimum Real-Time (RT) Speed**: **18%** (175 FPS at $t = 50.5\text{ s} - 51.5\text{ s}$)
* **Samples Below 100% RT**: 41 seconds (**56.2% of the boot**)
* **Desktop Idle RT**: **100%** (1000 FPS rock-solid starting at $t = 56.5\text{ s}$)

The emulation slowdown is concentrated into two distinct drop windows:
1. **Drop Window 1 ($t = 19.5\text{ s} - 41.5\text{ s}$)**: Speed drops to **32% – 55% RT** during `VMM32.VXD`, `IOS.386`, and driver loading.
2. **Drop Window 2 ($t = 44.5\text{ s} - 55.5\text{ s}$)**: Speed drops to **18% – 26% RT** during the 16-bit Windows GUI subsystem loading (`KRNL386.EXE`, `GDI.EXE`, `USER.EXE`, `EXPLORER.EXE`).

```
Real-Time Speed (%)
100% ────┐                                             ┌────────────────
         │                                             │ (Desktop Idle)
 80%     │                                             │
 60%     │          ┌─┐                                │
 40%     │  ┌───────┘ └──────┐                         │
 20%     └──┘ (Drop 1: Kernel)└───┬────────────────────┘
                                  │ (Drop 2: Win GUI)
  0% ────┴──────┴──────┴──────┴───┴──────┴──────┴──────┴───────
         0s    10s    20s    30s 40s    50s    60s    70s
```

---

## 2. VM Configuration

* **Machine**: AOpen AX6BC (`machine = ax6bc`)
* **CPU Family**: Intel Celeron Mendocino (`cpu_family = celeron_mendocino`)
* **CPU Speed**: 533 MHz (`cpu_speed = 533333333`)
* **RAM**: 128 MB (`mem_size = 131072`)
* **Graphics Card**: 3dfx Voodoo3 3000 AGP (`gfxcard = voodoo3_3k_agp`)
* **Hard Disk**: IDE Virtual Hard Disk (`windows98.vhd`)
* **Host Platform**: Linux x86-64 offscreen

---

## 3. Second-by-Second Boot Telemetry Log

| Elapsed (s) | RT Speed (%) | FPS (Target 1000) | Status | Boot Phase Description |
|---|---|---|---|---|
| $0.0 - 1.6$ | — | — | — | VM Startup & Memory Allocation |
| $2.5$ | 70% | 698 | **[LOW RT]** | BIOS Cold Disk Init & Controller Probe |
| $3.5$ | 100% | 1000 | OK | BIOS Memory Check & PnP Scan |
| $4.5$ | 98% | 982 | OK | PCI Device Configuration |
| $5.5 - 15.5$ | 100% | 997 - 1002 | OK | Real-Mode MS-DOS Boot & `IO.SYS` Init |
| $16.5 - 18.5$ | 80% - 82% | 801 - 821 | **[LOW RT]** | Floppy & IDE Bus Driver Detection |
| **19.5** | **45%** | **447** | **[LOW RT]** | **Drop 1 Starts**: `WIN.COM` execution, switching to protected mode |
| $20.5 - 24.5$ | 45% - 64% | 449 - 637 | **[LOW RT]** | `VMM32.VXD` Initialization, GDT/IDT/Paging Setup |
| $25.5 - 27.5$ | 46% - 54% | 460 - 538 | **[LOW RT]** | Core VxD Driver Loading (`IFSMGR.386`, `VFAT.386`) |
| $28.5 - 36.5$ | **32% - 39%** | **318 - 391** | **[LOW RT]** | `IOS.386` Disk Subsystem & INT 15h AH=87h Memory Copying |
| $37.5 - 41.5$ | 51% - 87% | 513 - 870 | **[LOW RT]** | VxD Init Complete, Rebound to Transition |
| $42.5 - 43.5$ | 100% - 105% | 999 - 1045 | OK | Inter-Phase Transition |
| **44.5** | **65%** | **646** | **[LOW RT]** | **Drop 2 Starts**: 16-bit Windows GUI Subsystem (`KRNL386.EXE`) |
| $45.5 - 48.5$ | 24% - 31% | 238 - 313 | **[LOW RT]** | `GDI.EXE` & `USER.EXE` Loading, Window Manager Setup |
| **50.5** | **18%** | **175** | **[LOW RT]** | **Absolute Lowest Point**: Desktop Shell (`EXPLORER.EXE`) Initialization |
| **51.5** | **18%** | **181** | **[LOW RT]** | Font Rasterization, Icon Extraction, Desktop Shell |
| $52.5 - 54.5$ | 21% - 26% | 208 - 257 | **[LOW RT]** | Taskbar, System Tray, Network Stack Init |
| $55.5$ | 66% | 663 | **[LOW RT]** | Startup Sounds & Final Applet Loading |
| **56.5 - 75.5**| **100%** | **998 - 1002** | **OK** | **Desktop Settled Idle (100% RT solid)** |

---

## 4. Top Hotspot Analysis

The top 30 hottest blocks account for over 25% of all host CPU execution time during boot.

### Top Hotspot Blocks Leaderboard

| Rank | Block | Guest PC (CS:EIP) | Physical PC | Dynamic Execs | Host Insns | Total Host Cycles | Pure JIT Cycles | Avg Cyc/Exec | Bottleneck Nature |
|---|---|---|---|---|---|---|---|---|---|
| **#1** | `blk#55814` | `14840:00003321` | `0014b721` | 988,004 | 110 | **1,637,463,721** | 1,560,642,360 | 1657.3 | Inlined unvectorized `rep movsd` |
| **#2** | `blk#55811` | `14840:0000315c` | `0014b55c` | 994,140 | 116 | **1,124,529,401** | 66,569,202 | 1131.2 | **94.1% C callout** (`iretd` fallback) |
| **#3** | `blk#58571` | `14840:000005eb` | `001489eb` | 696,576 | 586 | **988,709,747** | 172,162,274 | 1419.4 | **82.6% C callout** (`iretd` fallback) |
| **#4** | `blk#55834` | `fff8:0000d492` | `0010d412` | 988,048 | 321 | **831,351,885** | 245,049,302 | 841.4 | **70.5% C callout** (INT 15h AH=87h) |
| **#5** | `blk#55611` | `27ee:00001a5b` | `0002993b` | 2,753,486 | 765 | **644,448,262** | 503,802,795 | 234.0 | Segment loads (`retf` calling `load_seg`) |
| **#6** | `blk#37347` | `0000:c1470f24` | `07f6cf24` | 5,770,648 | 540 | **551,385,854** | 417,356,033 | 95.6 | VMM32 Ring 0 kernel frame push/pop |
| **#7** | `blk#63250` | `3d83:00001520` | `0003ed50` | 7,075,991 | 109 | **540,863,598** | 362,583,303 | 76.4 | GUI message dispatch loop |
| **#8** | `blk#37352` | `0000:c1470f13` | `07f6cf13` | 5,777,681 | 288 | **430,719,566** | 274,247,442 | 74.5 | Ring 0 interrupt exit sequence |
| **#9** | `blk#55651` | `27ee:0000118a` | `0002906a` | 2,753,485 | 316 | **399,339,779** | 235,210,169 | 145.0 | GDI drawing helper |
| **#10**| `blk#55761` | `3d83:00001554` | `0003ed84` | 3,354,627 | 154 | **386,245,658** | 232,500,987 | 115.1 | User window message loop |
| **#11**| `blk#55849` | `3d83:000014fa` | `0003ed2a` | 5,034,033 | 134 | **349,929,017** | 232,388,087 | 69.5 | Message queue event fetch |
| **#12**| `blk#55647` | `27ee:0000178f` | `0002966f` | 2,753,486 | 321 | **337,905,972** | 206,010,335 | 122.7 | 16-bit selector validation |
| **#13**| `blk#55782` | `14840:00003323` | `0014b723` | 296,876 | 98 | **334,508,057** | 302,899,080 | 1126.8 | Unrolled `rep movsd` tail |
| **#14**| `blk#55972` | `024c:000003ee` | `000028ae` | 1,869,284 | 292 | **328,890,631** | 184,390,394 | 175.9 | Timer tick handler |
| **#15**| `blk#59042` | `14840:00000e2e` | `0014922e` | 255,870 | 198 | **323,487,317** | 25,013,857 | 1264.3 | `iretd` fallback loop |

---

## 5. Detailed Hotspot Inspection

### Hotspot #1: Block #55814 (`14840:00003321`) — 1.64 Billion Cycles
```nasm
[Guest #0] 14840:00003321: d1 e9          shr cx, 1
[Guest #1] 14840:00003323: f3 67 66 a5    rep movsd dword ptr es:[edi], dword ptr [esi]
```
* **Executed**: 988,004 times
* **Total Host Cycles**: 1,637,463,721 (avg 1,657.3 cycles per block entry)
* **Root Cause**:
  * `rep movsd` is compiled as a basic loop in JIT IR (`uOp #08 .. #28`).
  * On **every single 4-byte dword transfer**, the generated host code:
    1. Flushes `ECX`, `ESI`, `EDI`, `cycles`, and flags to `cpu_state`.
    2. Reloads `cpu_state.seg_ds.base` and `cpu_state.seg_es.base`.
    3. Performs two software TLB lookups (`readlookup2` and `writelookup2`).
    4. Evaluates direction flag arithmetic.
    5. Branches back to `uOp #08`.
  * **Result**: Zero vectorization. 1.64 billion host cycles spent transferring buffers that could be accelerated via native `memcpy` or host `rep movsd`.

### Hotspot #2: Block #55811 (`14840:0000315c`) — 1.12 Billion Cycles
```nasm
[Guest #0] 14840:0000315c: fa             cli
[Guest #1] 14840:0000315d: 66 5a          pop edx
[Guest #2] 14840:0000315f: 83 c4 02       add sp, 2
[Guest #3] 14840:00003162: 66 5d          pop ebp
[Guest #4] 14840:00003164: 66 cf          iretd
```
* **Executed**: 994,140 times
* **Pure JIT Cycles**: 66,569,202 (5.9%)
* **C Callout Cycles**: **1,057,960,199 (94.1%)**
* **Root Cause**:
  * `iretd` has no JIT IR implementation. It emits `uOp #37: UOP_CALL_INSTRUCTION_FUNC imm=0xf523c`.
  * Every execution exits native host execution, calls the C interpreter opcode handler, re-decodes the frame, updates descriptor caches, and checks virtual interrupts.
  * Over **1.05 billion host cycles** wasted in C callout dispatch in this single block.

### Hotspot #3: Block #58571 (`14840:000005eb`) — 988 Million Cycles
```nasm
[Guest #28] 14840:00000637: 66 cf          iretd
```
* **Executed**: 696,576 times
* **Pure JIT Cycles**: 172,162,274 (17.4%)
* **C Callout Cycles**: **816,547,473 (82.6%)**
* **Root Cause**: Identical to Block #55811. `iretd` at the end of the block forces a C callout, costing another 816 million cycles.

### Hotspot #4: Block #55834 (`fff8:0000d492`) — 831 Million Cycles
```nasm
[Guest #16] fff8:0000d4be: f8             clc
[Guest #17] fff8:0000d4bf: b4 87          mov ah, 0x87
[Guest #18] fff8:0000d4c1: cd             .byte cd (int 15h)
```
* **Executed**: 988,048 times
* **C Callout Cycles**: **586,302,583 (70.5%)**
* **Root Cause**:
  * INT 15h AH=87h is the BIOS Extended Memory Copy service used by HIMEM.SYS / VxD initialization.
  * `clc` calls `flags_rebuild()` helper via `UOP_CALL_FUNC`.
  * `cd 15` emits `UOP_CALL_INSTRUCTION_FUNC`, dropping back to the interpreter to process the software interrupt.

---

## 6. Dynamic Instruction Mix & Code Generation Breakdown

Analysis of all 612,022,137 dynamic block executions reveals that **less than 36% of generated host instructions perform real computation**:

```
DYNAMIC HOST INSTRUCTION BREAKDOWN:
  Core ALU / FPU Computation:    35.65%  █████████████
  Inlined Memory TLB Fast-Path:  29.27%  ███████████
  Register Allocator Glue:       11.76%  ████
  PC / Flags Bookkeeping:        10.40%  ████
  Prologue / Frame Setup:        10.10%  ████
  Epilogue / Exit Dispatch:       2.66%  █
```

* **TLB Fast-Path Dominance (29.27%)**: Nearly one-third of all instructions executed across the entire CPU thread are segment base lookups, page directory shifts, and memory table indexing.
* **Emulation Overhead (64.35%)**: Summing TLB lookups, register allocator spill/reloads, PC/flags tracking, and block prologues/epilogues shows that nearly two-thirds of the JIT host execution time is bookkeeping overhead.

---

## 7. Interpreter Fallback Leaderboard (`UOP_CALL_INSTRUCTION_FUNC`)

The following guest instructions executed with high frequency during boot but had **no compiled JIT implementation**, forcing a complete fallback to the interpreter function pointer table:

| Guest Instruction | Dynamic Executions | Why It Is Uncompiled | Impact During Boot |
|---|---|---|---|
| **`.byte 9a` (`CALL ptr16:16` / `CALL far`)** | **22,821,489** | Far calls currently return `0` from `rop9A` | Dominates Drop Window 2 (16-bit Windows GUI inter-module calls) |
| **`cli`** | **19,944,220** | Uncompiled privilege check / CPL comparison | Kernel task switching in VMM32 |
| **`sti`** | **10,691,834** | Uncompiled privilege check / CPL comparison | Kernel task switching in VMM32 |
| **`rcl dx, 1`** | **7,078,768** | `codegen_ops_shift.c:463` hard-coded `return 0` | Bit manipulation in graphics drivers |
| **`div cx`** | **6,916,286** | `codegen_ops_misc.c:128` hard-coded `return 0` | Math calculations in GDI font scaling |
| **`rdtsc`** | **5,777,682** | Fallback to interpreter timing helper | High-resolution driver timestamping |
| **`mul cx`** | **5,340,573** | `codegen_ops_misc.c:128` hard-coded `return 0` | Coordinate scaling in window layouts |
| **`in al, 0x61`** | **5,262,930** | Port I/O callout | Timer/speaker delay loops |
| **`rcr ax, 1`** | **4,570,391** | `codegen_ops_shift.c:463` hard-coded `return 0` | Bit manipulation in graphics drivers |
| **`popf`** | **4,157,789** | Flags restoration fallback | Interrupt enable/disable restoration |

---

## 8. Dynarec Codegen Linter & Anti-Pattern Audit

The automated linter audited all 126,660 compiled blocks in the profile dump:

```
================================================================================
DYNAREC CODEGEN LINTER & ANTI-PATTERN AUDIT
================================================================================
[*] Total Blocks Audited: 126,660

1. Dead Flag Materialization Across Barriers:
   - Wasted cpu_state.flags_* Stores: 108,104 static, 662,827,732 dynamic cycles
   - Impact: [WARNING] High redundant flag spills detected

2. Fast-Path PC / OldPC Write Overhead:
   - Overwritten / Redundant PC Stores: 123,002,600 dynamic writes
   - Impact: [NOTICE] pc/oldpc fast-path pruning recommended

3. Intra-Block Register Thrashing:
   - Spill-then-Reload Occurrences: 151,707
   - Impact: [WARNING] High register pressure detected

4. IREG_cycles Stale-Register Hazard (Bug C-3 Pattern):
   - Unreloaded IREG_cycles Across Memory Helpers: 0
   - Status: [PASS] No stale IREG_cycles hazards

5. Host Instruction Encoding Sanity:
   - Disassembly Failures: 0
   - Status: [PASS] 100% of host code disassembles cleanly
================================================================================
```

### Critical Findings:
1. **662.8 Million Wasted Cycles in Dead Flag Spills**:
   * When an instruction like `ADD` is followed by a memory load and then a `MOV` or another ALU op, `UOP_TYPE_ORDER_BARRIER` marks all registers dirty.
   * This forces `IREG_flags_op`, `IREG_flags_res`, `IREG_flags_op1`, and `IREG_flags_op2` to be written to `cpu_state` even when the next instruction immediately overwrites them without any conditional branch or flag consumer in between.
2. **123 Million Redundant PC Updates**:
   * Every guest instruction updates `IREG_pc` and `IREG_oldpc`. In linear blocks without memory faults or branches, these stores are immediately overwritten by the next instruction in the block.
3. **151,707 Spill-then-Reload Chains**:
   * The 8-register host allocation pool runs out of registers during complex addressing modes, spilling guest registers to the stack frame and reloading them within 2–3 uOps.

---

## 9. Prioritized Remediation Roadmap (To Reach 100% RT)

| Priority | Optimization Item | Target Impact | Implementation Approach |
|---|---|---|---|
| **P1** | **JIT-compile `0x9A` (Far Call / `CALL ptr16:16`)** | Eliminates **22.8M** interpreter exits during Drop Window 2 (Windows GUI startup). | Emit inlined segment selector push, CS base update, and jump to direct/indirect target. |
| **P2** | **Implement Vectorized / Bulk `rep movsd` / `rep movsb`** | Recovers **~1.5 - 2.0 Billion host cycles** in memory copies (`blk#55814`, `blk#55782`). | Detect flat/unpaged segments or contiguous TLB spans; emit native host `rep movsd` or C `memcpy` callout for counts $> 16$. |
| **P3** | **JIT-compile `div`, `mul`, `rcl`, `rcr`** | Eliminates **23.9M** interpreter callouts (`div cx` 6.9M, `rcl dx, 1` 7.1M, `mul cx` 5.3M, `rcr ax, 1` 4.6M). | Replace `return 0` in `codegen_ops_shift.c:463` and `codegen_ops_misc.c:128` with native uOps (`UOP_SHR`, `UOP_SHL` with carry, host `div`/`mul`). |
| **P4** | **Dead Flag Elimination (DSE) Pass Across Barriers** | Recovers **662.8M host cycles** across 108,104 blocks. | Perform backward liveness pass before register allocation: if `IREG_flags_*` are killed before any flag consumer (`Jcc`, `ADC`, `PUSHF`, fault), skip the store. |
| **P5** | **JIT-compile Protected Mode `iretd` Fast-Path** | Recovers **~1.8 Billion host cycles** in top hotspot blocks #2 and #3. | Implement common-case same-privilege `iretd` directly in JIT (pop EIP, CS, EFLAGS), falling back to C only on task/ring change. |
| **P6** | **Prune Redundant PC / OldPC Stores** | Eliminates **123M** memory writes to `cpu_state`. | Only emit `uop_MOV_IMM(IREG_pc)` before potential exit points, barriers, and memory ops that can fault. |

---

## 10. Remediation Verification & Progress Tracking

### Phase 1: JIT-compile `0x9A` (`CALL ptr16:16` / `CALL far`) — [COMPLETED & VERIFIED]

#### Implementation Summary
1. **Core Far Call Dispatch**:
   - Implemented `call_far_16()` and `call_far_32()` (and `_2386` variants) in [`src/cpu/x86seg.c`](file:///home/marm/src/86Box/src/cpu/x86seg.c) and [`src/cpu/x86seg.h`](file:///home/marm/src/86Box/src/cpu/x86seg.h).
   - Fully handles Real Mode far jumps, Protected Mode Call Gates, descriptor validation, parameter copying, privilege-level transitions, and stack switches.
2. **x86-64 Backend Arguments**:
   - Implemented `codegen_LOAD_FUNC_ARG2_IMM` (`REG_EDX` / `REG_R8`) and `codegen_LOAD_FUNC_ARG3_IMM` (`REG_ECX` / `REG_R9`) in [`src/codegen_new/codegen_backend_x86-64_uops.c`](file:///home/marm/src/86Box/src/codegen_new/codegen_backend_x86-64_uops.c) to support 3-argument C callouts with immediate operands.
3. **Dynarec JIT Integration**:
   - Implemented `ropCALL_far_16` and `ropCALL_far_32` in [`src/codegen_new/codegen_ops_jump.c`](file:///home/marm/src/86Box/src/codegen_new/codegen_ops_jump.c).
   - Registered handlers in `recomp_opcodes[0x9a]` (16-bit) and `recomp_opcodes[256 + 0x9a]` (32-bit) in [`src/codegen_new/codegen_ops.c`](file:///home/marm/src/86Box/src/codegen_new/codegen_ops.c).
   - Updated block cycle timing and instruction bounds (`insn_end = op_pc + 5` for 16-bit, `op_pc + 7` for 32-bit) in [`src/codegen_new/codegen.c`](file:///home/marm/src/86Box/src/codegen_new/codegen.c).

#### Empirical Benchmark Comparison (75.5s Windows 98 Boot)

| Metric | Baseline (Pre-Remediation) | Post-Phase 1 (`0x9A` Far Call JIT) | Delta |
|---|---|---|---|
| **Opcode `0x9A` Fallbacks** | **22,821,489** (Rank #1) | **0** (Eliminated) | **-22,821,489 (-100%)** |
| **Average RT Speed** | **70.3%** | **72.6%** | **+2.3% overall** |
| **Drop Window 2 @ t=51.5s** | 18% RT (181 FPS) | 27% RT (270 FPS) | **+49.2% speedup** |
| **Drop Window 2 @ t=52.5s** | 21% RT (208 FPS) | 24% RT (238 FPS) | **+14.4% speedup** |
| **Drop Window 2 @ t=54.5s** | 26% RT (256 FPS) | **98% RT (983 FPS)** | **+284% speedup (cleared bottleneck 2s earlier)** |
| **Desktop Ready Time** | $t \approx 56.5\text{s}$ | $t \approx 54.5\text{s}$ | **~2.0s faster boot to desktop** |



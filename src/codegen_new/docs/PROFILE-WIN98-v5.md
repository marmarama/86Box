# Windows 98 Dynarec Telemetry & Interactive Application Profiling (v5)

**Date**: 2026-11-21  
**Build**: Commit `3eec97bf3` (`/home/marm/Emulation/86Box/86Box`, Release x86-64)  
**Platform**: Linux x86-64 (Wayland native GUI, `Qt 5.15.18`, `[i440BX] AOpen AX6BC`)  
**Workload**: Interactive Windows 98 SE session (`119s` capture: boot through manual desktop app execution to exit)  
**Binary Block Trace**: [`scratch/win98_manual_v5.86bdump`](file:///home/marm/src/86Box/scratch/win98_manual_v5.86bdump) (`511 MB`, `264,062` compiled blocks, `1,123,904,148` dynamic block executions)  
**Telemetry Tool**: [`scripts/analyze_blocks.py`](file:///home/marm/src/86Box/scripts/analyze_blocks.py) (`BOX86_DUMP_PROFILE=1`, `BOX86_PERF_MAP=1`)

---

## 1. Executive Summary & Progress vs. v4

Since [`PROFILE-WIN98-v4.md`](file:///home/marm/src/86Box/src/codegen_new/docs/PROFILE-WIN98-v4.md) (October 29, 2026), multiple compiler, opcode-lowering, segment-cache, and FPU improvements have landed:
- Native `SAHF`/`LAHF`, variable shifts `0xD2`/`0xD3`, `LAR`/`LSL`, software interrupts (`INT`), and one-operand `MUL`/`IMUL` (`0xF6 /5`, `0xF7 /4,/5`).
- Lowered FPU constants, `FLDCW`, `FCOMI`/`FCMOVcc`, `FRNDINT`, `FXAM`, `tbyte FLD/FSTP`, and bitwise `FCHS`/`FABS` / `UOP_FP_ENTER` elision.
- Out-of-line cold memory slowpath stubs, branch/cmp fast paths, and `SEG_DESC_CACHE` improvements (`78.0%` hit rate: `90,765,079` hits, `25,650,484` misses, `39,851,651` accessed skips, `0` overflows).

| Metric | Profile v4 (`385s` session) | Profile v5 (`119s` session) | Delta |
|---|---:|---:|---:|
| **Average Real-Time (RT) Speed** | 54.1% | **72.8%** | **+18.7 pts** |
| **Minimum RT Speed** | 11% | **17%** | **+6 pts** |
| **Maximum RT Speed** | 104% | **136%** | — |
| **Core ALU / FPU Compute Mix** | 37.19% | **43.13%** | **+5.94 pts** |
| **Register Allocator Glue Mix** | 11.46% | **7.35%** | **-4.11 pts** |
| **PC / Flags Bookkeeping Mix** | 9.68% | **5.70%** | **-3.98 pts** |
| **Prologue / Frame Setup Mix** | 10.71% | **4.98%** | **-5.73 pts** |
| **Inlined Memory TLB Fast-Path** | 28.02% | **22.26%** | **-5.76 pts** |
| **Direct Chaining Hit Rate** | 100.00% | **100.00%** (`1.85B` linked, `4,994` unlinked) | Preserved |
| **Top v4 Fallbacks (`mov ss`, `shl cl`, `sahf`, `int`, `lsl`, `lar`)** | 27.58M+ combined | **0** | **Eliminated** |

---

## 2. Global Execution & Real-Time (RT) Timeline

```text
Host Architecture:               x86-64
Total Compiled Blocks:           264,062
Total Recorded Block Executions: 1,123,904,148 (~1.12 Billion)
Total Telemetry Samples:         119 seconds
Average Emulation Speed:         72.8% Real-Time
Minimum Emulation Speed:         17% Real-Time
Maximum Emulation Speed:         136% Real-Time
Intervals Below 100% RT:         72 (60.5% of total runtime)
```

```text
Real-Time Speed Timeline (% RT over 119s):
100% | █▄████████▄▄█ ▄     █           ███   ██▄█                 
 80% | █████████████▄██   ███          ████ ▄████                 
 60% |█████████████████ ▄████         ████████████                
 40% |█████████████████▄█████         ████████████▄        ▄▄▄▄▄▄▄
 20% |█████████████████████████▄▄▄▄███████████████████████████████
  0% └────────────────────────────────────────────────────────────
     0.0s                                              119.0s
```

Key low-RT phases (`18` windows total):
- **Boot Driver / Kernel Init**: `t=15.0s–21.0s` (`33%–92%`, avg `70.9%`), `t=23.0s–33.0s` (`17%–88%`, avg `31.9%`, min `17%` at `28s`).
- **Windows GUI / Explorer Startup**: `t=44.0s–62.0s` (`23%–66%`, avg `31.8%`).
- **Interactive App Bursts**: `t=79.0s–83.0s` (`18%–74%`, avg `33.4%`), `t=93.0s–98.0s` (`21%–78%`, avg `36.3%`), settling cleanly at `100%–102%` RT when idle (`110s–119s`, except `87%` at `116s`).

---

## 3. Host CPU Cycle Breakdown (`164,289,990,900` Total Host TSC Cycles)

| Execution Subsystem | Host Cycles | % of CPU Thread Time | Notes |
|---|---:|---:|---|
| **Pure Native JIT Execution** | **54,467,737,263** | **33.15%** | Fast-path native code |
| **Hardware Timers / Devices** | **17,676,005,934** | **10.76%** | PIT/PIC/VGA/Voodoo/device emulation |
| **JIT Compiler** | **13,267,649,599** | **8.08%** | Compiling `264,062` unique codeblocks over `119s` |
| **C Dispatcher Overhead** | **12,206,008,567** | **7.43%** | `140,560,699` returns vs `1,077,419,402` chained transfers |
| **Helpers (`flags_rebuild`)** | **8,796,120,661** | **5.35%** | **`#1` intra-block C callout bottleneck** (esp. `jit_flags_rebuild_c`) |
| **Memory Slow-Path I/O** | **6,871,285,397** | **4.18%** | `19,679,440` slow loads, `130,449,058` slow stores |
| **Interpreter Fallbacks (`UOP_CALL_INSTRUCTION_FUNC`)** | **2,416,651,524** | **1.47%** | Down to `1.47%` |
| **Opcode C Callouts (`IN` / `OUT`)** | **974,824,247** | **0.59%** | Port I/O handlers (`0x40`, `0x61`, etc.) |

---

## 4. Top 25 Hottest Code Blocks

| Rank | Block | Guest PC (`CS:EIP`) | Phys | Executions | Host Insns | Host TSC | Pure JIT TSC | Avg Cyc | % Tot |
|---:|---|---|---|---:|---:|---:|---:|---:|---:|
| #1 | `blk#21431` | `0000:c1457835` | `07f89835` | 35,579,518 | 197 | 4,185,761,768 | 1,884,689,951 | 117.6 | 4.69% |
| #2 | `blk#7418` | `0000:c14544fe` | `07f8c4fe` | 34,063,426 | 109 | 3,688,046,109 | 1,681,388,329 | 108.3 | 4.13% |
| #3 | `blk#44110` | `0000:c0053b83` | `00813b83` | 914,695 | 35 | 1,769,060,798 | 65,651,776 | 1934.0 | 1.98% |
| #4 | `blk#44108` | `0000:c0053b89` | `00813b89` | 914,695 | 88 | 1,572,030,944 | 67,270,300 | 1718.6 | 1.76% |
| #5 | `blk#21306` | `0000:0040acb8` | `02c98cb8` | 23,222,270 | 93 | 1,533,927,435 | 1,022,729,801 | 66.1 | 1.72% |
| #6 | `blk#21307` | `0000:0040acc3` | `02c98cc3` | 20,276,459 | 88 | 1,348,696,936 | 893,222,861 | 66.5 | 1.51% |
| #7 | `blk#44112` | `0000:c0053b7a` | `00813b7a` | 914,695 | 66 | 1,327,346,377 | 67,389,463 | 1451.1 | 1.49% |
| #8 | `blk#58167` | `f000:0000ed54` | `000fed54` | 633,390 | 60 | 1,142,210,194 | 41,652,774 | 1803.3 | 1.28% |
| #9 | `blk#58164` | `f000:0000ed4c` | `000fed4c` | 627,967 | 60 | 1,082,662,516 | 40,992,464 | 1724.1 | 1.21% |
| #10 | `blk#64806` | `f000:0000ed78` | `000fed78` | 431,396 | 60 | 985,732,460 | 33,589,678 | 2285.0 | 1.10% |
| #11 | `blk#64730` | `f000:0000ed80` | `000fed80` | 403,242 | 60 | 918,868,283 | 31,518,318 | 2278.7 | 1.03% |
| #12 | `blk#6364` | `305a:000006a0` | `00554c40` | 12,882,388 | 223 | 902,181,401 | 584,691,496 | 70.0 | 1.01% |
| #13 | `blk#62765` | `0000:c0001521` | `00146521` | 483,694 | 176 | 613,546,411 | 35,429,636 | 1268.5 | 0.69% |
| #14 | `blk#62723` | `0000:bff74217` | `00825217` | 3,840,550 | 127 | 612,697,837 | 358,901,921 | 159.5 | 0.69% |
| #15 | `blk#28653` | `0000:c1470f54` | `07f6cf54` | 5,783,159 | 594 | 489,744,793 | 363,622,135 | 84.7 | 0.55% |
| #16 | `blk#9999` | `8036600:00002b7b` | `0056db7b` | 7,200,453 | 74 | 478,199,137 | 324,698,153 | 66.4 | 0.54% |
| #17 | `blk#28658` | `0000:c1470f43` | `07f6cf43` | 5,790,192 | 293 | 424,555,360 | 275,719,462 | 73.3 | 0.48% |
| #18 | `blk#63782` | `f000:0000bd1c` | `000fbd1c` | 169,047 | 60 | 385,355,893 | 13,106,164 | 2279.6 | 0.43% |
| #19 | `blk#54374` | `0000:bff741cf` | `008251cf` | 3,288,956 | 140 | 372,981,193 | 225,102,496 | 113.4 | 0.42% |
| #20 | `blk#59964` | `0000:bff741c0` | `008251c0` | 3,840,782 | 183 | 371,563,581 | 223,857,045 | 96.7 | 0.42% |
| #21 | `blk#63784` | `f000:0000bd24` | `000fbd24` | 158,211 | 60 | 356,805,187 | 12,557,158 | 2255.2 | 0.40% |
| #22 | `blk#65487` | `2000:00007aa3` | `00027aa3` | 163,178 | 58 | 313,988,217 | 19,644,578 | 1924.2 | 0.35% |
| #23 | `blk#64570` | `f000:0000ed78` | `000fed78` | 136,023 | 60 | 312,816,409 | 10,647,309 | 2299.7 | 0.35% |
| #24 | `blk#53070` | `0000:bff74212` | `00825212` | 3,443,040 | 100 | 303,318,586 | 191,158,812 | 88.1 | 0.34% |
| #25 | `blk#44143` | `0000:c000a7b6` | `001207b6` | 3,899,604 | 95 | 302,585,218 | 174,408,552 | 77.6 | 0.34% |

---

## 5. Dynamic Instruction Mix & Chaining Efficiency

```text
  Prologue / Frame Setup:         4.98%
  Epilogue / Exit Dispatch:      16.48%  (note: includes appended cold slowpath stub bytes after epilogue_pos_off)
  Register Allocator Glue:        7.35%
  Inlined Memory TLB Fast-Path:  22.26%
  PC / Flags Bookkeeping:         5.70%
  Core ALU / FPU Computation:    43.13%

  BLOCK_EXIT_NONE         : 128,206,363  (11.41%)
  BLOCK_EXIT_DIRECT_1     : 156,674,781  (13.94%)
  BLOCK_EXIT_COND_2       : 730,333,175  (64.98%)
  BLOCK_EXIT_RET          : 108,725,070  ( 9.67%)
  Chaining Direct Hit Rate:  100.00% (1,854,267,570 linked branches, 4,994 unlinked)
```

> [!NOTE]
> `Epilogue / Exit Dispatch` shows `16.48%` (vs `2.82%` in v4) because `scripts/analyze_blocks.py` computes `(h_len - b.epilogue_pos_off) * cnt`, which currently counts the out-of-line cold memory slowpath stubs emitted at the tail of each block after `epilogue_pos_off` as if they were executed epilogue bytes.

---

## 6. Interpreter Fallback Leaderboard (`UOP_CALL_INSTRUCTION_FUNC`)

All top fallbacks from v4 (`mov ss`, `shl reg, cl`, `sahf`, `int imm8`, `lsl`, `lar`, `shrd`, one-operand `mul`/`imul`) are gone from the leaderboard. Only 10 instructions remain (`1.47%` of total CPU thread time combined):

| Rank | Instruction | Dynamic Executions | Notes / Tracked Ticket |
|---:|---|---:|---|
| #1 | `idiv bx` | `1,580,756` | Task #20 (`0xF7 /7`, extend to 16-bit & 32-bit `IDIV`) |
| #2 | `fdecstp` | `633,458` | Task #17 (`0xD9 0xF6` / `0xF7`) |
| #3 | `mov ecx, cr0` | `326,356` | Mode switch / PM thunk |
| #4 | `mov cr0, ecx` | `326,356` | Mode switch / PM thunk (flushes block/TLB) |
| #5 | `fnop` | `175,616` | `0xD9 0xD0` (trivial FPU no-op lowering) |
| #6 | `lgdt [0x57a2]` | `163,178` | Mode switch (`2000:00007aa3`) |
| #7 | `hlt` | `93,389` | Idle sleep (`93k` over `119s`) |
| #8 | `arpl word ptr [bx + si], di` | `64,280` | 16-bit Windows thunking |
| #9 | `lidt [ebx - 0x36]` | `61,401` | Interrupt table switch |
| #10 | `lidt [0xc000d494]` | `61,400` | Interrupt table switch |

---

## 7. Automated Linter Audit (`analyze_blocks.py --lint`)

- **Total Blocks Audited**: `264,062`
- **Dead Flag Materialization Across Barriers**: `250,604` static stores, `1,042,284,028` dynamic cycles (`[WARNING]`, tracked by `86box.compiler` `IR / Barriers` ticket)
- **Fast-Path PC / OldPC Write Overhead**: `27,999,014` redundant dynamic writes (`[NOTICE]`)
- **Intra-Block Register Thrashing**: `183,688` spill-then-reload occurrences (`[WARNING]`)
- **`IREG_cycles` Stale-Register Hazards**: `0` `[PASS]`
- **Host Instruction Encoding Sanity**: `0` disassembly failures `[PASS]`

---

## 8. Key Actionable Bottlenecks Identified in v5

1. **`jit_flags_rebuild_c` Before Flag-Clobbering Instructions (`8.80B` cycles, `5.35%`)**:
   - Inspecting `#5` `0000:0040acb8` (`23.22M` execs) and `#6` `0000:0040acc3` (`20.28M` execs): `inc esi` is the first flag-touching op (`codegen_flags_changed == 0`), so `rebuild_c()` emits `uop_CALL_FUNC(jit_flags_rebuild_c)` (full host register flush/reload barrier), even though the very next instruction `cmp al, ah` immediately overwrites `CF` (`FLAGS_SUB8`). The same pattern hits `#14` `0000:bff74217` (`inc dword ptr [edx+0x10]`, `3.84M` execs).
   - Eliminating `rebuild_c` when a subsequent op in the block kills `C_FLAG` without reading it — and/or inlining `jit_flags_rebuild_c` — targets `5.35%` of total CPU time.
2. **PIT `0x40` & Port `0x61` Polling (`~11.0%` of total host TSC across Ranks `#3, #4, #7, #8–#11, #18, #21, #23`)**:
   - `#3/#4/#7` (`c0053b83`, `c0053b89`, `c0053b7a`: `in al, 0x40`, `5.23%`) and BIOS `#8–#11/#18/#21/#23` (`f000:ed4c..ed80` / `bd1c..bd24`: `in al, 0x61; test al, 0x10`, `5.80%`) remain the dominant I/O polling hotspot (Task #8).
3. **`16-bit IDIV` (`1.58M` fallbacks) & `FNOP` (`175k` fallbacks)**:
   - Extending native `IDIV` (Task #20) to 16-bit `idiv rm16` and adding `FDECSTP`/`FINCSTP` + `FNOP` (Task #17) leaves only privileged/mode-switch instructions (`mov cr0`, `lgdt`, `lidt`, `hlt`, `arpl`) in the fallback table.
4. **Tight Flat Memory / Kernel Loops (`#1` `c1457835`, `#2` `c14544fe` = `8.82%` total TSC)**:
   - Both loops spend >50% of time on `UOP_MEM_LOAD_REG`/`UOP_MEM_STORE_REG` + host/TLB dispatch (`add edx` segment base + page-crossing check + `readlookup2`/`writelookup2`), reinforcing Flat DS/SS base skip (Task #21) and cross-block GPR pinning (Task #7).

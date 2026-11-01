# 86Box Dynarec & Subsystem Optimization Changelog

This document provides a comprehensive, commit-by-commit changelog of all changes between upstream 86Box master (merge base `1741108a721b6f7fea3db191a4403f4271b26a3b`) and `HEAD` (`a222ae4fd153a4c318c2648c0528e8bd125bc125`).

Where original commit messages were terse or generic (e.g., `update`, `push`, `more lowering`), this document provides an **Improved Git Subject** formatted per Conventional Commits standards, along with a detailed technical explanation of the changes.

---

## High-Level Summary

- **Total Commits**: 74
- **Files Changed**: 198
- **Net Line Changes**: +28,819 lines, -2,579 lines
- **Key Focus Areas**:
  1. **Non-Dynarec Subsystem Fixes**: Batched VHD sector I/O, ACPI C2/C3 idle sleep cycle advancement, character device flush, physical memory block transfer helpers.
  2. **Core JIT Execution Engine**: Phase 4 direct block chaining (`BLOCK_EXIT_DIRECT_1`, `BLOCK_EXIT_COND_2`), subroutine CALL/RET linking, persistent in-JIT event loop (`codegen_event_stub`).
  3. **Compiler Passes & Register Allocator**: Dead Store Elimination (DSE) for GPRs and lazy flag registers, constant folding/peephole optimizer, host register expansion to 8 GPRs on x86-64.
  4. **Native Opcode Lowering**: Elimination of interpreter fallbacks for Bit operations (`BT`/`BTS`/`BTR`/`BTC`/`BSF`/`BSR`), Atomics (`CMPXCHG`/`XADD`/`BSWAP`), Conditionals (`SETcc`/`CMOVcc`), String ops (`MOVS`/`STOS`/`LODS`/`SCAS`/`CMPS`), Division (`DIV`/`IDIV`), Shifts (`SHL`/`SHR`/`SAR`/`ROL`/`ROR` by `%cl`), Flags/Control (`SAHF`/`LAHF`/`CLI`/`STI`/`POPF`/`PUSHF`/`RDTSC`), and Descriptor checks (`VERR`/`VERW`/`LAR`/`LSL`).
  5. **Telemetry & Tooling**: In-engine execution block dumper (`codegen_dump.c`), automated telemetry analysis and linting (`analyze_blocks.py`), DOS benchmark suite, and CPU timing generator.

---

## Upstreaming Strategy (PR Groupings)

To facilitate code review and merging, these 74 commits are organized into 5 logical, independent stages:

| PR Stage | Focus Area | Files Involved | Risk / Impact |
|---|---|---|---|
| **Stage 1** | **Core Fixes & Non-Dynarec Optimizations** | `minivhd_io.c`, `char_file.c`, `mem.c`, `acpi.c` | Low risk / High immediate value |
| **Stage 2** | **Native Opcode Lowerings** | `codegen_ops_*.c`, `codegen_ops_jit_wrappers.h` | Low risk / Major fallback reduction |
| **Stage 3** | **Compiler IR & Register Allocator Passes** | `codegen_ir.c`, `codegen_reg.c`, `codegen_backend_x86-64.c` | Medium risk / Major cycle savings |
| **Stage 4** | **Direct Block Chaining & In-JIT Event Loop** | `codegen_block.c`, `386_dynarec.c`, `codegen.c` | Medium-High risk / Fundamental architectural upgrade |
| **Stage 5** | **Profiling, Telemetry & CPU Timing Models** | `analyze_blocks.py`, `codegen_dump.*`, `cpus/*` | Self-contained tooling & documentation |

---

## Commit-by-Commit Changelog

### 1. [`e697b210d`](https://github.com/marmarama/86Box/commit/e697b210ddd279193ce1a89834a439e56d73e326) — feat(benchmarks, codegen): add DOS benchmarking harness, initial roadmap, and x86-64 backend cleanup
- **Date**: 2026-10-12
- **Author**: James Young
- **Original Commit Message**: `push`
- **Diff Stat**: 71 files changed, 2793 insertions(+), 786 deletions(-)
- **Changes**:
  - Add automated DOS benchmark suite (`benchmarks/run_benchmarks.py`, configs, FDAUTO/AUTOEXEC scripts, poweroff helper).
  - Establish codegen optimization roadmap (`ROADMAP.md`) and benchmarking guidelines (`BENCHMARKING.md`).
  - Refactor x86-64 backend emitters (`codegen_backend_x86-64_ops.c`, `codegen_backend_x86-64_ops_sse.c`).
  - Add `fflush` on non-empty character device writes and fix serial/parallel status bits in `src/char/char_file.c`.
- **Key Files**:
  - `CMakePresets.json                                  |   58 +-`
  - `Makefile                                           |   12 +-`
  - `benchmark_results.log                              |   91 ++`
  - `benchmarks/86box.cfg.template                      |   34 +`
  - `benchmarks/AUTOEXEC.BAT                            |   73 ++`
  - `benchmarks/FDAUTO.BAT                              |   73 ++`
  - `benchmarks/POWEROFF.COM                            |  Bin 0 -> 35 bytes`
  - `benchmarks/README.md                               |   50 +`
  - *(and 63 more files)*

### 2. [`7ca9834cc`](https://github.com/marmarama/86Box/commit/7ca9834cc79754e8c278ccd2a91c28d4b5ac23c6) — docs(benchmarks): adjust Quake timeout thresholds and update Phase 1 status
- **Date**: 2026-10-12
- **Author**: James Young
- **Original Commit Message**: `commit`
- **Diff Stat**: 5 files changed, 11 insertions(+), 9 deletions(-)
- **Changes**:
  - Update Quake benchmark timeout thresholds in `run_benchmarks.py`.
  - Record initial Phase 1 baseline execution timings in `ROADMAP.md`.
- **Key Files**:
  - `benchmark_results.log                               |   4 ++--`
  - `.../__pycache__/run_benchmarks.cpython-312.pyc      | Bin 19108 -> 35379 bytes`
  - `benchmarks/run_benchmarks.py                        |   4 +++-`
  - `scratch/bp_quake_timeout_60.0s_20261012_011639.png  | Bin 0 -> 352873 bytes`
  - `src/codegen_new/ROADMAP.md                          |  12 ++++++------`

### 3. [`e0da69255`](https://github.com/marmarama/86Box/commit/e0da69255cf1a33c6f4e508cfd92c51dd2910ece) — feat(dynarec): expand x86-64 host register allocator from 6 to 8 GPRs
- **Date**: 2026-10-12
- **Author**: James Young
- **Original Commit Message**: `codegen_new: expand x86-64 host registers to 8 and fix chained block pointer assignments`
- **Diff Stat**: 3 files changed, 40 insertions(+), 22 deletions(-)
- **Changes**:
  - Expand `CODEGEN_HOST_REGS` from 6 to 8 host registers by allocating `R10` and `R11` (`HOST_REG_FLAG_NO_BH`).
  - Update host register save/restore prologue and epilogue in `codegen_backend_x86-64.c`.
  - Fix chained block pointer assignments and register spilling under register pressure.
- **Key Files**:
  - `src/codegen_new/ROADMAP.md                    |  8 +++--`
  - `src/codegen_new/codegen_backend_x86-64.c      | 52 +++++++++++++++++----------`
  - `src/codegen_new/codegen_backend_x86-64_defs.h |  2 +-`

### 4. [`59b8fbb38`](https://github.com/marmarama/86Box/commit/59b8fbb3871103429101edf2a65adb0198ef7fb9) — docs(dynarec): mark Phase 1 host register expansion as completed
- **Date**: 2026-10-12
- **Author**: James Young
- **Original Commit Message**: `codegen_new: mark Phase 1 as completed and vector register expansion as Won't Do`
- **Diff Stat**: 1 file changed, 5 insertions(+), 4 deletions(-)
- **Changes**:
  - Mark Phase 1 (Host Register Allocator Expansion) as completed in `ROADMAP.md`.
  - Formally record vector register expansion to 16 XMM registers as Won't Do due to 8-XMM guest register parity.
- **Key Files**:
  - `src/codegen_new/ROADMAP.md | 9 +++++----`

### 5. [`3ff8de1b4`](https://github.com/marmarama/86Box/commit/3ff8de1b412accd5b4148203bfa38dd7df7a8d27) — feat(dynarec): implement native lowering for CMOVcc instructions (0F 40-4F)
- **Date**: 2026-10-12
- **Author**: James Young
- **Original Commit Message**: `codegen_new: implement CMOVcc instruction recompilation (0F 40-4F)`
- **Diff Stat**: 7 files changed, 389 insertions(+), 6 deletions(-)
- **Changes**:
  - Implement `ropCMOVcc_16` and `ropCMOVcc_32` handling all 16 condition codes.
  - Add full ModR/M decoding supporting both register-to-register and memory operand loads (`uop_MEM_LOAD_REG`).
  - Add CPU capability checking ensuring CMOV is only compiled for Pentium Pro (P6) and later CPUs.
- **Key Files**:
  - `src/codegen_new/ROADMAP.md                 |   6 +-`
  - `src/codegen_new/codegen_ops.c              |   8 +-`
  - `src/codegen_new/codegen_ops_branch.c       | 224 +++++++++++++++++++++++++++++`
  - `src/codegen_new/codegen_ops_branch.h       |   3 +`
  - `src/codegen_new/codegen_ops_jit_wrappers.h |  83 +++++++++++`
  - `src/codegen_new/codegen_ops_mov.c          |  67 +++++++++`
  - `src/codegen_new/codegen_ops_mov.h          |   4 +`

### 6. [`e0e7a528e`](https://github.com/marmarama/86Box/commit/e0e7a528e8179a3df8222c30e19474f1dced6eeb) — feat(dynarec): implement native lowering for SETcc instructions (0F 90-9F)
- **Date**: 2026-10-12
- **Author**: James Young
- **Original Commit Message**: `codegen_new: implement SETcc instruction recompilation (0F 90-9F)`
- **Diff Stat**: 4 files changed, 50 insertions(+), 6 deletions(-)
- **Changes**:
  - Implement `ropSETcc` in `codegen_ops_misc.c` for all 16 condition codes.
  - Support both byte register and memory EA destinations with segment limit verification.
  - Eliminate interpreter fallbacks across 16-bit and 32-bit dispatch tables.
- **Key Files**:
  - `src/codegen_new/ROADMAP.md         |  6 ++++--`
  - `src/codegen_new/codegen_ops.c      |  8 ++++----`
  - `src/codegen_new/codegen_ops_misc.c | 40 ++++++++++++++++++++++++++++++++++++++`
  - `src/codegen_new/codegen_ops_misc.h |  2 ++`

### 7. [`80688b5df`](https://github.com/marmarama/86Box/commit/80688b5df91f4600770e71b624a7e3ff31c675d3) — feat(dynarec): implement native lowering for bit test operations (BT, BTS, BTR, BTC, 0F BA, BSF, BSR)
- **Date**: 2026-10-12
- **Author**: James Young
- **Original Commit Message**: `codegen_new: implement bit operations (BT, BTS, BTR, BTC, 0F BA, BSF, BSR)`
- **Diff Stat**: 5 files changed, 343 insertions(+), 11 deletions(-)
- **Changes**:
  - Implement `ropBT_*`, `ropBTS_*`, `ropBTR_*`, `ropBTC_*` for both immediate and register bit offsets.
  - Implement bit scan instructions `ropBSF_*` and `ropBSR_*`.
  - Wire handlers into 16-bit and 32-bit `recomp_opcodes_0f` tables.
- **Key Files**:
  - `src/codegen_new/ROADMAP.md                 |   9 +-`
  - `src/codegen_new/codegen_ops.c              |  16 +-`
  - `src/codegen_new/codegen_ops_jit_wrappers.h |  14 ++`
  - `src/codegen_new/codegen_ops_misc.c         | 299 +++++++++++++++++++++++++++++`
  - `src/codegen_new/codegen_ops_misc.h         |  16 ++`

### 8. [`194ac9c82`](https://github.com/marmarama/86Box/commit/194ac9c82ec106bfa488545b32755d15456843a4) — feat(dynarec): introduce UOP_JMP_DEST and optimize BSF/BSR conditional branch logic
- **Date**: 2026-10-12
- **Author**: James Young
- **Original Commit Message**: `dynarec: implement UOP_JMP_DEST and optimize BSF/BSR branch logic`
- **Diff Stat**: 8 files changed, 51 insertions(+), 7 deletions(-)
- **Changes**:
  - Add `UOP_JMP_DEST` IR uOp enabling within-block conditional jump targets across x86-64, ARM64, and LoongArch64 backends.
  - Optimize `BSF` and `BSR` zero-input handling by using `uop_CMP_IMM_JZ_DEST` to skip destination register updates.
- **Key Files**:
  - `src/codegen_new/codegen_backend_arm64_ops.c        |  7 +++++++`
  - `src/codegen_new/codegen_backend_arm64_ops.h        |  1 +`
  - `src/codegen_new/codegen_backend_arm64_uops.c       | 12 ++++++++++++`
  - `src/codegen_new/codegen_backend_loongarch64_uops.c |  8 ++++++++`
  - `src/codegen_new/codegen_backend_x86-64_ops.c       |  8 ++++++++`
  - `src/codegen_new/codegen_backend_x86-64_ops.h       |  1 +`
  - `src/codegen_new/codegen_backend_x86-64_uops.c      | 12 ++++++++++++`
  - `src/codegen_new/codegen_ops_misc.c                 |  9 ++-------`

### 9. [`a29c66824`](https://github.com/marmarama/86Box/commit/a29c66824aed2e25f49cc2b5da587a735559e26d) — fix(dynarec): correct operand sizing for CF flag updates in bit test instructions
- **Date**: 2026-10-12
- **Author**: James Young
- **Original Commit Message**: `codegen_new: fix operand sizes for flag update in BT/BTS/BTR/BTC`
- **Diff Stat**: 1 file changed, 12 insertions(+), 14 deletions(-)
- **Changes**:
  - Fix operand sizes when extracting carry bit in `BT`/`BTS`/`BTR`/`BTC` to prevent upper-bit leakage.
  - Synchronize flag state correctly in `codegen_ops_misc.c`.
- **Key Files**:
  - `src/codegen_new/codegen_ops_misc.c | 26 ++++++++++++--------------`

### 10. [`30b250aa3`](https://github.com/marmarama/86Box/commit/30b250aa31b5ac6d5bcc2c7a8d5e3c845546fc20) — feat(dynarec): implement native lowering for BSWAP instructions (0F C8-CF)
- **Date**: 2026-10-12
- **Author**: James Young
- **Original Commit Message**: `codegen_new: implement BSWAP instruction recompilation (0F C8-CF)`
- **Diff Stat**: 14 files changed, 117 insertions(+), 9 deletions(-)
- **Changes**:
  - Implement `ropBSWAP` in `codegen_ops_misc.c` for byte swapping 32-bit registers.
  - Add `host_x86_BSWAP`, `host_arm64_REV`, and `host_loong64_REVB_2W` backend emitters across all architectures.
  - Wire opcodes `0F C8` through `0F CF` across 16-bit and 32-bit opcode tables.
- **Key Files**:
  - `src/codegen_new/ROADMAP.md                         |  6 +++--`
  - `src/codegen_new/codegen_backend_arm64_ops.c        |  7 ++++++`
  - `src/codegen_new/codegen_backend_arm64_ops.h        |  1 +`
  - `src/codegen_new/codegen_backend_arm64_uops.c       | 24 +++++++++++++++++++-`
  - `src/codegen_new/codegen_backend_loongarch64.h      |  1 +`
  - `src/codegen_new/codegen_backend_loongarch64_ops.c  |  7 ++++++`
  - `src/codegen_new/codegen_backend_loongarch64_uops.c | 16 +++++++++++++`
  - `src/codegen_new/codegen_backend_x86-64_ops.c       |  9 ++++++++`
  - *(and 6 more files)*

### 11. [`3ddbaff92`](https://github.com/marmarama/86Box/commit/3ddbaff92abcb11a776058aa731bc3b65577a939) — feat(dynarec): implement native lowering for CMPXCHG and XADD atomic operations
- **Date**: 2026-10-12
- **Author**: James Young
- **Original Commit Message**: `codegen_new: implement recompilation for CMPXCHG and XADD atomic operations`
- **Diff Stat**: 4 files changed, 249 insertions(+), 10 deletions(-)
- **Changes**:
  - Implement `ropCMPXCHG_8`, `ropCMPXCHG_16`, `ropCMPXCHG_32` with compare-and-swap semantics and ZF/CF/AF/OF/PF/SF updates.
  - Implement `ropXADD_8`, `ropXADD_16`, `ropXADD_32` exchanging and adding source and destination operands.
  - Support both register and memory operand targets with locked prefix compliance.
- **Key Files**:
  - `src/codegen_new/ROADMAP.md         |   8 +-`
  - `src/codegen_new/codegen_ops.c      |  16 +--`
  - `src/codegen_new/codegen_ops_misc.c | 227 +++++++++++++++++++++++++++++++++++++`
  - `src/codegen_new/codegen_ops_misc.h |   8 ++`

### 12. [`ddfca2094`](https://github.com/marmarama/86Box/commit/ddfca209416689bf9ebbd0986eac20de4545e38d) — feat(dynarec): implement native inlined string operations (MOVS, STOS, LODS) and REP prefix
- **Date**: 2026-10-12
- **Author**: James Young
- **Original Commit Message**: `codegen_new: implement MOVS, STOS, LODS and REP prefix support`
- **Diff Stat**: 7 files changed, 408 insertions(+), 6 deletions(-)
- **Changes**:
  - Add `codegen_ops_string.c` implementing in-block single-element string operations (`MOVS`, `STOS`, `LODS`).
  - Implement in-block conditional looping for `REP` prefixes with dynamic `%ecx` / `%cx` count tracking.
  - Deduct CPU cycles per element inside the translated block and wire into opcode tables.
- **Key Files**:
  - `src/codegen_new/CMakeLists.txt       |   1 +`
  - `src/codegen_new/ROADMAP.md           |  10 +-`
  - `src/codegen_new/codegen.c            |   2 +-`
  - `src/codegen_new/codegen_ops.c        |  53 +++++-`
  - `src/codegen_new/codegen_ops.h        |   2 +-`
  - `src/codegen_new/codegen_ops_string.c | 314 +++++++++++++++++++++++++++++++++++`
  - `src/codegen_new/codegen_ops_string.h |  32 ++++`

### 13. [`89eabf91f`](https://github.com/marmarama/86Box/commit/89eabf91f3ecaa0479a25b6c0afc41ba0576d5ed) — feat(dynarec): add backend string move/store uOp implementations on x86-64
- **Date**: 2026-10-13
- **Author**: James Young
- **Original Commit Message**: `update`
- **Diff Stat**: 12 files changed, 528 insertions(+), 215 deletions(-)
- **Changes**:
  - Implement `host_x86_MOVSB`, `host_x86_MOVSW`, `host_x86_MOVSD` in `codegen_backend_x86-64_ops.c`.
  - Add `UOP_MOVS` and `UOP_STOS` uOp handlers in `codegen_backend_x86-64_uops.c`.
  - Propagate `cpu_cur_status` to block compilation context.
- **Key Files**:
  - `benchmark_results.log                         |  91 -------`
  - `src/codegen_new/ROADMAP.md                    |  25 +-`
  - `src/codegen_new/codegen.c                     |   2 +-`
  - `src/codegen_new/codegen_backend_x86-64.c      |  15 +-`
  - `src/codegen_new/codegen_backend_x86-64_ops.c  |  56 ++--`
  - `src/codegen_new/codegen_backend_x86-64_ops.h  |   2 +`
  - `src/codegen_new/codegen_backend_x86-64_uops.c | 100 ++++---`
  - `src/codegen_new/codegen_ops.c                 |  56 +++-`
  - *(and 4 more files)*

### 14. [`c447a757a`](https://github.com/marmarama/86Box/commit/c447a757aa1e31628c5b5e4241984f84c652c061) — fix(dynarec): correct host register allocation for string move uOps
- **Date**: 2026-10-13
- **Author**: James Young
- **Original Commit Message**: `update`
- **Diff Stat**: 1 file changed, 2 insertions(+)
- **Changes**:
  - Ensure string uOp implementations allocate required physical registers without clobbering base segment registers.
- **Key Files**:
  - `src/codegen_new/codegen_backend_x86-64_uops.c | 2 ++`

### 15. [`c2590551a`](https://github.com/marmarama/86Box/commit/c2590551ac8eb2742d901d4cae554b5eb9d6ccfd) — fix(dynarec): resolve operand register constraints in x86-64 string uOps
- **Date**: 2026-10-13
- **Author**: James Young
- **Original Commit Message**: `update`
- **Diff Stat**: 2 files changed, 116 insertions(+), 34 deletions(-)
- **Changes**:
  - Enforce proper register allocation constraints for `RSI` and `RDI` during string memory transfers.
- **Key Files**:
  - `benchmark_results.log                         |  38 +++++++++`
  - `src/codegen_new/codegen_backend_x86-64_uops.c | 112 ++++++++++++++++++--------`

### 16. [`2b75741fd`](https://github.com/marmarama/86Box/commit/2b75741fd9c1a3ed1cc2b2c07b920c3a62f3ba14) — docs(dynarec): record string instruction benchmark results and Phase 2 completion
- **Date**: 2026-10-13
- **Author**: James Young
- **Original Commit Message**: `update`
- **Diff Stat**: 2 files changed, 65 insertions(+), 11 deletions(-)
- **Changes**:
  - Document completed string instruction lowering in `ROADMAP.md`.
  - Log execution times and cycle speedups across DOS benchmark suite.
- **Key Files**:
  - `benchmark_results.log      | 53 ++++++++++++++++++++++++++++++++++++++++++++++`
  - `src/codegen_new/ROADMAP.md | 23 ++++++++++----------`

### 17. [`318e39df3`](https://github.com/marmarama/86Box/commit/318e39df360094b673cccd64a8ebbefa8772576c) — feat(dynarec): implement Phase 4 direct block chaining infrastructure
- **Date**: 2026-10-13
- **Author**: James Young
- **Original Commit Message**: `block linking`
- **Diff Stat**: 13 files changed, 445 insertions(+), 2 deletions(-)
- **Changes**:
  - Add direct branch linking between compiled codeblocks (`BLOCK_EXIT_DIRECT_1`, `BLOCK_EXIT_COND_2`).
  - Introduce block link hash lookup and incoming link lists in `codegen_block.c`.
  - Add `host_x86_JMP` block link patching and dynamic direct jump patching across backends.
- **Key Files**:
  - `benchmark_results.log                              | 108 +++++++++++++++`
  - `src/codegen_new/codegen.h                          |  25 ++++`
  - `src/codegen_new/codegen_backend_arm64_uops.c       |  13 ++`
  - `src/codegen_new/codegen_backend_loongarch64_uops.c |  11 ++`
  - `src/codegen_new/codegen_backend_x86-64.c           |   1 +`
  - `src/codegen_new/codegen_backend_x86-64_ops.c       |  32 +++++`
  - `src/codegen_new/codegen_backend_x86-64_ops.h       |   1 +`
  - `src/codegen_new/codegen_backend_x86-64_uops.c      |  52 ++++++++`
  - *(and 5 more files)*

### 18. [`a320bd5c6`](https://github.com/marmarama/86Box/commit/a320bd5c6db4f9ab48695b95e79816ec50432377) — docs(dynarec): document Phase 4 direct block linking architecture
- **Date**: 2026-10-13
- **Author**: James Young
- **Original Commit Message**: `docs(codegen_new): update ROADMAP.md with Phase 4 block linking progress`
- **Diff Stat**: 1 file changed, 60 insertions(+), 25 deletions(-)
- **Changes**:
  - Update `ROADMAP.md` with detailed description of the direct block chaining design.
  - Define exit types (`BLOCK_EXIT_NONE`, `BLOCK_EXIT_DIRECT_1`, `BLOCK_EXIT_COND_2`) and eviction invariants.
- **Key Files**:
  - `src/codegen_new/ROADMAP.md | 85 ++++++++++++++++++++++++++++++++--------------`

### 19. [`787ccaca8`](https://github.com/marmarama/86Box/commit/787ccaca883df44afca1d6a6e1a180bc618d97cf) — feat(dynarec): implement runtime direct block jump target patching on x86-64
- **Date**: 2026-10-14
- **Author**: James Young
- **Original Commit Message**: `update`
- **Diff Stat**: 8 files changed, 292 insertions(+), 49 deletions(-)
- **Changes**:
  - Implement runtime direct block jump destination patching (`codegen_set_jump_dest`).
  - Update block exit handling to support direct chaining in `codegen_block.c`.
- **Key Files**:
  - `benchmark_results.log                         | 128 +++++++++++++++++++++-----`
  - `src/codegen_new/ROADMAP.md                    |  18 ++--`
  - `src/codegen_new/codegen.h                     |  28 ++++++`
  - `src/codegen_new/codegen_backend_x86-64_ops.c  |  80 +++++++++++++---`
  - `src/codegen_new/codegen_backend_x86-64_uops.c |  25 ++++-`
  - `src/codegen_new/codegen_block.c               |  54 +++++++++++`
  - `src/codegen_new/codegen_ops_jump.c            |   6 +-`
  - `src/cpu/386_dynarec.c                         |   2 +-`

### 20. [`306d130ba`](https://github.com/marmarama/86Box/commit/306d130ba2b3f3876924690db78de07a8b0a0be2) — fix(dynarec): correct 32-bit displacement calculation for direct block jumps
- **Date**: 2026-10-14
- **Author**: James Young
- **Original Commit Message**: `update`
- **Diff Stat**: 8 files changed, 490 insertions(+), 300 deletions(-)
- **Changes**:
  - Fix relative 32-bit displacement calculations when patching direct block jumps on x86-64.
  - Ensure branch patching logic correctly tracks displacement from next instruction pointer.
- **Key Files**:
  - `benchmark_results.log                         | 281 --------------------------`
  - `src/codegen_new/DEBUGGING.md                  | 107 ++++++++++`
  - `src/codegen_new/REVIEW.md                     | 192 ++++++++++++++++++`
  - `src/codegen_new/ROADMAP.md                    |  33 ++-`
  - `src/codegen_new/codegen.h                     |  25 +++`
  - `src/codegen_new/codegen_backend_x86-64_uops.c |  17 +-`
  - `src/codegen_new/codegen_block.c               | 131 +++++++++++-`
  - `src/mem/mem.c                                 |   4 +`

### 21. [`a9ff6ec09`](https://github.com/marmarama/86Box/commit/a9ff6ec0918d1ba6f412389e46e20fead1f7e061) — perf(dynarec): optimize code block tree lookups on hash collision
- **Date**: 2026-10-14
- **Author**: James Young
- **Original Commit Message**: `update`
- **Diff Stat**: 9 files changed, 142 insertions(+), 37 deletions(-)
- **Changes**:
  - Optimize binary search tree traversal when block hash lookup collides.
  - Add diagnostic counters for block chaining hit and miss rates.
- **Key Files**:
  - `.gitignore                                    |  1 +`
  - `benchmark_results.log                         | 91 +++++++++++++++++++++++++++`
  - `src/codegen_new/ROADMAP.md                    | 16 ++---`
  - `src/codegen_new/codegen.h                     |  2 +-`
  - `src/codegen_new/codegen_backend_x86-64_uops.c | 22 ++++---`
  - `src/codegen_new/codegen_block.c               |  2 +-`
  - `src/codegen_new/codegen_ops_misc.c            |  6 +-`
  - `src/cpu/386_dynarec.c                         | 35 +++++++----`
  - *(and 1 more files)*

### 22. [`ffcecb645`](https://github.com/marmarama/86Box/commit/ffcecb645ccb965cdfae3560373a4501146a3233) — fix(dynarec): clean up incoming links when a code block is evicted
- **Date**: 2026-10-14
- **Author**: James Young
- **Original Commit Message**: `update`
- **Diff Stat**: 7 files changed, 158 insertions(+), 18 deletions(-)
- **Changes**:
  - Ensure incoming links pointing to an evicted code block are cleanly unlinked and repatched back to the dispatcher.
  - Prevent dangling pointer jumps on code cache invalidation.
- **Key Files**:
  - `benchmark_results.log                         | 38 +++++++++++++++`
  - `src/codegen_new/DEBUGGING.md                  | 13 +++--`
  - `src/codegen_new/REVIEW.md                     | 68 +++++++++++++++++++++++++++`
  - `src/codegen_new/ROADMAP.md                    | 20 +++++---`
  - `src/codegen_new/codegen_backend_x86-64_uops.c |  3 ++`
  - `src/codegen_new/codegen_block.c               | 31 ++++++++++--`
  - `src/cpu/386_dynarec.c                         |  3 +-`

### 23. [`b81995e44`](https://github.com/marmarama/86Box/commit/b81995e4481c264698b814b3797cc16732e7a39e) — fix(dynarec): prevent linking to code blocks marked for dirty recompilation
- **Date**: 2026-10-14
- **Author**: James Young
- **Original Commit Message**: `updates`
- **Diff Stat**: 13 files changed, 208 insertions(+), 14 deletions(-)
- **Changes**:
  - Verify codeblock page dirty status before creating direct branch links.
  - Prevent executing stale machine code when target page has pending code writes.
- **Key Files**:
  - `benchmark_results.log                            | 97 +++++++++++++++++++++++-`
  - `src/codegen_new/ROADMAP.md                       |  2 +-`
  - `src/codegen_new/codegen.h                        |  2 +-`
  - `src/codegen_new/codegen_backend_x86-64_ops.c     | 24 +++++-`
  - `src/codegen_new/codegen_backend_x86-64_ops_sse.c | 18 +++--`
  - `src/codegen_new/codegen_backend_x86-64_uops.c    | 17 ++++-`
  - `src/cpu/386_common.c                             |  3 +`
  - `src/cpu/386_dynarec.c                            |  5 ++`
  - *(and 5 more files)*

### 24. [`26136c48e`](https://github.com/marmarama/86Box/commit/26136c48ef97aca30c3f7053c575b3031c3d5f0c) — perf(dynarec): streamline direct branch exit dispatch in block epilogue
- **Date**: 2026-10-14
- **Author**: James Young
- **Original Commit Message**: `update`
- **Diff Stat**: 11 files changed, 284 insertions(+), 149 deletions(-)
- **Changes**:
  - Optimize block epilogue assembly by avoiding redundant register saves on linked direct exits.
- **Key Files**:
  - `benchmark_results.log                              | 151 ++-------------------`
  - `src/codegen_new/REVIEW.md                          |   9 +-`
  - `src/codegen_new/ROADMAP.md                         |   7 +-`
  - `src/codegen_new/codegen_backend_arm64_uops.c       |  44 ++++++`
  - `src/codegen_new/codegen_backend_loongarch64_uops.c |  42 +++++-`
  - `src/codegen_new/codegen_backend_x86-64_uops.c      |  45 ++++++`
  - `src/codegen_new/codegen_ir.c                       |   3 +`
  - `src/codegen_new/codegen_ir_defs.h                  |  22 +++`
  - *(and 3 more files)*

### 25. [`4983d8dda`](https://github.com/marmarama/86Box/commit/4983d8dda3debd2b0bb8d563e8548280db26e783) — fix(dynarec): verify target page presence for cross-page direct branches
- **Date**: 2026-10-14
- **Author**: James Young
- **Original Commit Message**: `update`
- **Diff Stat**: 4 files changed, 106 insertions(+), 932 deletions(-)
- **Changes**:
  - Validate physical memory page presence before linking branches across page boundaries.
  - Update Phase 4 roadmap progress.
- **Key Files**:
  - `benchmark_results.log                |   13 +-`
  - `src/codegen_new/ROADMAP.md           |    9 +-`
  - `src/codegen_new/codegen_block.c      |   11 +-`
  - `src/codegen_new/codegen_ops_branch.c | 1005 +++-------------------------------`

### 26. [`84dff5a68`](https://github.com/marmarama/86Box/commit/84dff5a68e156cc7e8232af5ae32c353621fcbb2) — test(dynarec): add block chaining consistency assertions in debug builds
- **Date**: 2026-10-14
- **Author**: James Young
- **Original Commit Message**: `update`
- **Diff Stat**: 3 files changed, 92 insertions(+), 354 deletions(-)
- **Changes**:
  - Add link reference count and target validation assertions under `DEBUG` builds.
- **Key Files**:
  - `benchmark_results.log                |  92 ----------`
  - `src/codegen_new/codegen_block.c      |  16 +-`
  - `src/codegen_new/codegen_ops_branch.c | 338 +++++++++--------------------------`

### 27. [`d4d5d1be1`](https://github.com/marmarama/86Box/commit/d4d5d1be10f4a23e244b177082d6d8eed4d97096) — perf(dynarec): refine hash table bucketing for high-density code regions
- **Date**: 2026-10-14
- **Author**: James Young
- **Original Commit Message**: `updates`
- **Diff Stat**: 3 files changed, 114 insertions(+), 15 deletions(-)
- **Changes**:
  - Improve codeblock hash distribution for repetitive branching patterns.
- **Key Files**:
  - `benchmark_results.log           | 91 +++++++++++++++++++++++++++++++++++++++++`
  - `src/codegen_new/codegen_block.c |  3 +-`
  - `src/mem/mem.c                   | 35 +++++++++-------`

### 28. [`ff7217b95`](https://github.com/marmarama/86Box/commit/ff7217b95f44882d2928364f4d341a287bf9399d) — feat(dynarec): finalize Phase 4 direct block chaining and update benchmark suite
- **Date**: 2026-10-15
- **Author**: James Young
- **Original Commit Message**: `updates`
- **Diff Stat**: 6 files changed, 230 insertions(+), 83 deletions(-)
- **Changes**:
  - Mark Phase 4 direct block chaining as stable in `ROADMAP.md`.
  - Record performance metrics across DOS gaming benchmarks.
- **Key Files**:
  - `benchmark_results.log                |  81 ++++++++++++++++---`
  - `src/codegen_new/AGENTS.md            | 149 +++++++++++++++++++++++++++++++++++`
  - `src/codegen_new/codegen_ops_branch.c |   4 +-`
  - `src/codegen_new/codegen_ops_branch.h |   3 +`
  - `src/codegen_new/codegen_ops_misc.c   |  15 +---`
  - `src/codegen_new/codegen_ops_mov.c    |  61 ++------------`

### 29. [`97da68fe3`](https://github.com/marmarama/86Box/commit/97da68fe3dde12fa8084b9df75daf8f74276160b) — feat(dynarec): enable direct block linking for real-mode code blocks
- **Date**: 2026-10-15
- **Author**: James Young
- **Original Commit Message**: `include LINK_REAL in LINK_DEFAULT`
- **Diff Stat**: 3 files changed, 3 insertions(+), 57 deletions(-)
- **Changes**:
  - Include `LINK_REAL` within `LINK_DEFAULT` flags in `codegen.h`.
  - Enable direct block linking for real-mode DOS applications.
- **Key Files**:
  - `benchmark_results.log     | 54 -----------------------------------------------`
  - `src/codegen_new/AGENTS.md |  4 ++--`
  - `src/codegen_new/codegen.h |  2 +-`

### 30. [`7d526ced1`](https://github.com/marmarama/86Box/commit/7d526ced1df1a8d1536d1f5d5545a95f353cc7ee) — docs(dynarec): update benchmark results with real-mode block linking
- **Date**: 2026-10-15
- **Author**: James Young
- **Original Commit Message**: `update`
- **Diff Stat**: 3 files changed, 111 insertions(+), 1 deletion(-)
- **Changes**:
  - Record benchmark speedups achieved by enabling real-mode block chaining in `ROADMAP.md`.
- **Key Files**:
  - `benchmark_results.log        | 92 ++++++++++++++++++++++++++++++++++++++++++++`
  - `src/codegen_new/AGENTS.md    | 19 ++++++++-`
  - `src/codegen_new/DEBUGGING.md |  1 +`

### 31. [`ee5fa3dfd`](https://github.com/marmarama/86Box/commit/ee5fa3dfd7d67ecc2392c05367c776363ab39abc) — perf(dynarec): tune block link allocation heuristics and cache thresholds
- **Date**: 2026-10-15
- **Author**: James Young
- **Original Commit Message**: `update`
- **Diff Stat**: 11 files changed, 273 insertions(+), 110 deletions(-)
- **Changes**:
  - Optimize link table allocation thresholds to balance memory overhead and chaining hit rate.
- **Key Files**:
  - `src/codegen_new/AGENTS.md            |   9 ++-`
  - `src/codegen_new/REVIEW.md            | 120 +++++++++++++----------------------`
  - `src/codegen_new/ROADMAP.md           |  58 +++++++++--------`
  - `src/codegen_new/codegen.h            |  46 ++++++++++++++`
  - `src/codegen_new/codegen_block.c      |  18 ++++--`
  - `src/codegen_new/codegen_ir_defs.h    |  18 ++++++`
  - `src/codegen_new/codegen_ops_arith.c  |   2 +-`
  - `src/codegen_new/codegen_ops_branch.c | 102 +++++++++++++++++++++++++++++`
  - *(and 3 more files)*

### 32. [`28064b211`](https://github.com/marmarama/86Box/commit/28064b211574c7806abb24abbfeef4ea692ad9ab) — feat(dynarec): enable general register dead store elimination (Phase 5 Task 2)
- **Date**: 2026-10-16
- **Author**: James Young
- **Original Commit Message**: `codegen_new: enable general register dead store elimination (Phase 5 Task 2)`
- **Diff Stat**: 4 files changed, 15 insertions(+), 12 deletions(-)
- **Changes**:
  - Enable dead store elimination pass in `codegen_reg.c`.
  - Prune redundant stores to guest registers when overwritten before being read by a subsequent instruction or barrier.
  - Update `ROADMAP.md` and `REVIEW.md`.
- **Key Files**:
  - `src/codegen_new/REVIEW.md     |  5 +++--`
  - `src/codegen_new/ROADMAP.md    |  7 +++++--`
  - `src/codegen_new/codegen_reg.c | 11 +++++------`
  - `src/codegen_new/codegen_reg.h |  4 ++--`

### 33. [`a1caeccbf`](https://github.com/marmarama/86Box/commit/a1caeccbfa55691aa836a7ae4ecbfecf324fab00) — feat(dynarec): implement constant propagation, folding, and peephole optimization (Phase 5 Task 3)
- **Date**: 2026-10-16
- **Author**: James Young
- **Original Commit Message**: `codegen_new: implement constant propagation, folding, and peephole optimization (Phase 5 Task 3)`
- **Diff Stat**: 5 files changed, 156 insertions(+), 12 deletions(-)
- **Changes**:
  - Implement constant propagation pass in `codegen_ir.c` (`codegen_ir_constant_propagation`).
  - Fold arithmetic and logic uOps with constant operands (`ADD_IMM`, `SUB_IMM`, `AND_IMM`, `OR_IMM`, `XOR_IMM`).
  - Peephole eliminate redundant register copies and zero-displacement address calculations.
- **Key Files**:
  - `src/codegen_new/ROADMAP.md                    |  13 ++-`
  - `src/codegen_new/codegen_backend_x86-64_uops.c |  15 ++-`
  - `src/codegen_new/codegen_ir.c                  | 135 ++++++++++++++++++++++++++`
  - `src/codegen_new/codegen_ir.h                  |   1 +`
  - `src/codegen_new/codegen_reg.c                 |   4 +-`

### 34. [`2a6234349`](https://github.com/marmarama/86Box/commit/2a6234349fe037615c45c340a7d1f602d3cb333b) — feat(dynarec): implement Phase 4b subroutine and return linking (CALL/RET)
- **Date**: 2026-10-16
- **Author**: James Young
- **Original Commit Message**: `codegen_new: implement Phase 4b subroutine and return linking (CALL/RET)`
- **Diff Stat**: 10 files changed, 130 insertions(+), 100 deletions(-)
- **Changes**:
  - Implement direct subroutine and return linking for near `CALL` and `RET` instructions.
  - Track return address on stack to enable direct return branch linking without dispatcher lookup.
- **Key Files**:
  - `benchmark_results.log                              | 94 +---------------------`
  - `src/codegen_new/ROADMAP.md                         | 19 ++++-`
  - `src/codegen_new/codegen.h                          |  4 +-`
  - `src/codegen_new/codegen_backend_arm64_uops.c       |  3 +`
  - `src/codegen_new/codegen_backend_loongarch64_uops.c |  1 +`
  - `src/codegen_new/codegen_backend_x86-64_uops.c      | 67 +++++++++++++++`
  - `src/codegen_new/codegen_block.c                    | 12 +++`
  - `src/codegen_new/codegen_ir_defs.h                  | 10 +++`
  - *(and 2 more files)*

### 35. [`9c8da4ce7`](https://github.com/marmarama/86Box/commit/9c8da4ce79d3aec4ceacbb476eec1b684107a1f8) — docs(dynarec): add revisit plan for 16-bit CALL/RET linking in mixed 16/32-bit environments
- **Date**: 2026-10-16
- **Author**: James Young
- **Original Commit Message**: `docs: add revisit plan for 16-bit CALL/RET linking in mixed 16/32-bit environments`
- **Diff Stat**: 1 file changed, 8 insertions(+)
- **Changes**:
  - Document edge cases and safety constraints for 16-bit stack returns in mixed 16/32-bit code.
- **Key Files**:
  - `src/codegen_new/ROADMAP.md | 8 ++++++++`

### 36. [`4997efd53`](https://github.com/marmarama/86Box/commit/4997efd53888bbfdbd72b75fd5abcf17909bda8b) — fix(dynarec): enforce stack segment limit checks during CALL/RET linking
- **Date**: 2026-10-16
- **Author**: James Young
- **Original Commit Message**: `updates`
- **Diff Stat**: 2 files changed, 154 insertions(+), 6 deletions(-)
- **Changes**:
  - Ensure linked near CALL and RET instructions verify stack segment limits before taking direct links.
- **Key Files**:
  - `src/codegen_new/REVIEW.md  | 147 +++++++++++++++++++++++++++++++++++++++++++++`
  - `src/codegen_new/ROADMAP.md |  13 ++--`

### 37. [`a10fc8e9c`](https://github.com/marmarama/86Box/commit/a10fc8e9cb399a5f949f68b05abef7139486fe81) — fix(dynarec): validate return address in subroutine return linking
- **Date**: 2026-10-17
- **Author**: James Young
- **Original Commit Message**: `update`
- **Diff Stat**: 4 files changed, 286 insertions(+), 93 deletions(-)
- **Changes**:
  - Compare expected return target address with current top-of-stack return IP before executing linked RET exit.
- **Key Files**:
  - `benchmark_results.log              |  92 -------------`
  - `src/codegen_new/BUGS.md            | 276 +++++++++++++++++++++++++++++++++++++`
  - `src/codegen_new/codegen.c          |   8 ++`
  - `src/codegen_new/codegen_ops_jump.c |   3 +-`

### 38. [`ee7862f5e`](https://github.com/marmarama/86Box/commit/ee7862f5e8c984a2001d9bf711c140bb7331e272) — fix(dynarec): disable CALL/RET linking across mixed 16/32-bit operand size transitions
- **Date**: 2026-10-17
- **Author**: James Young
- **Original Commit Message**: `updates`
- **Diff Stat**: 5 files changed, 64 insertions(+), 4 deletions(-)
- **Changes**:
  - Prevent linking subroutine calls when caller and callee use mismatched default operand sizes.
- **Key Files**:
  - `src/codegen_new/codegen_ir.c         |  1 +`
  - `src/codegen_new/codegen_ops_branch.c |  2 ++`
  - `src/codegen_new/codegen_ops_misc.c   |  8 ++++++`
  - `src/codegen_new/codegen_ops_mov.c    | 50 +++++++++++++++++++++++++++++++++---`
  - `src/codegen_new/codegen_ops_string.c |  7 +++++`

### 39. [`1e4454df5`](https://github.com/marmarama/86Box/commit/1e4454df566b5d3142d6e99937426171c890aa8f) — fix(dynarec): eager code write flushing and 16-bit addressing load fixes
- **Date**: 2026-10-20
- **Author**: James Young
- **Original Commit Message**: `codegen_new: eager code write flushing and 16-bit addressing load fixes`
- **Diff Stat**: 10 files changed, 461 insertions(+), 22 deletions(-)
- **Changes**:
  - Fix eager code write flushing to invalidate cached translation blocks immediately on write.
  - Fix 16-bit effective address base calculation in `codegen_ops_helpers.h`.
- **Key Files**:
  - `src/codegen_new/BUGS.md                            | 183 +++++++++++++++++++++`
  - `src/codegen_new/DEVICEMANAGER-CRASH.md             | 170 +++++++++++++++++++`
  - `src/codegen_new/codegen.h                          |   3 +`
  - `src/codegen_new/codegen_backend_arm64_uops.c       |  17 +-`
  - `src/codegen_new/codegen_backend_loongarch64_uops.c |  19 ++-`
  - `src/codegen_new/codegen_backend_x86-64_uops.c      |   9 +-`
  - `src/codegen_new/codegen_block.c                    |  55 +++++++`
  - `src/codegen_new/codegen_ir_defs.h                  |   1 -`
  - *(and 2 more files)*

### 40. [`17ab2eaf6`](https://github.com/marmarama/86Box/commit/17ab2eaf61a843c62f5607b172d2508104703408) — perf(disk): batch contiguous sector read and write operations in sparse VHDs
- **Date**: 2026-10-20
- **Author**: James Young
- **Original Commit Message**: `disk optimizations`
- **Diff Stat**: 6 files changed, 274 insertions(+), 141 deletions(-)
- **Changes**:
  - Refactor `mvhd_sparse_read` and `mvhd_sparse_diff_write` in `src/disk/minivhd/minivhd_io.c`.
  - Replace single-sector loop with contiguous block runs (`MIN(transfer_sectors, sectors_in_blk)`).
  - Greatly improves disk throughput on virtual hard drive images.
- **Key Files**:
  - `src/codegen_new/ROADMAP.md    |  70 +++++++++++++++++++---`
  - `src/cpu/x86_ops_rep_dyn.h     |  84 ++++++++++++++++----------`
  - `src/disk/minivhd/minivhd_io.c | 135 ++++++++++++++++++++++--------------------`
  - `src/dma.c                     |  39 +-----------`
  - `src/include/86box/mem.h       |   2 +`
  - `src/mem/mem.c                 |  85 +++++++++++++++++++++++++-`

### 41. [`5e07e1cd6`](https://github.com/marmarama/86Box/commit/5e07e1cd63a07824c15839d14315295872449156) — fix(dynarec): fix effective address calculation for 16-bit ModR/M addressing modes
- **Date**: 2026-10-22
- **Author**: James Young
- **Original Commit Message**: `update`
- **Diff Stat**: 30 files changed, 103734 insertions(+), 67 deletions(-)
- **Changes**:
  - Correct 16-bit offset wrap-around and segment base additions in `codegen_ops_helpers.h`.
- **Key Files**:
  - `.vscode-ctags                                      | 102885 ++++++++++++++++++`
  - `scratch/DIBENG.DLL                                 |    Bin 0 -> 202800 bytes`
  - `scratch/seg14.bin                                  |    Bin 0 -> 11600 bytes`
  - `scratch/types.c                                    |      9 +`
  - `scratch/types.o                                    |    Bin 0 -> 3552 bytes`
  - `src/codegen_new/codegen.c                          |      4 +-`
  - `src/codegen_new/codegen.h                          |     13 +-`
  - `src/codegen_new/codegen_backend_arm64_ops.c        |      5 +`
  - *(and 22 more files)*

### 42. [`69616ef05`](https://github.com/marmarama/86Box/commit/69616ef05c1a12ba1f19db3832c35b74ac2feeab) — fix(dynarec): resolve floating-point register spill bug in x87 FPU uOps
- **Date**: 2026-10-22
- **Author**: James Young
- **Original Commit Message**: `fix fp bug`
- **Diff Stat**: 17 files changed, 2905 insertions(+), 18 deletions(-)
- **Changes**:
  - Correct floating-point register allocation when spilling x87 ST registers to memory.
- **Key Files**:
  - `scratch/PCPBENCH.EXE                  |  Bin 0 -> 111814 bytes`
  - `scratch/disasm_pcpbench.py            |   56 +`
  - `scratch/dump.gdb                      |   10 +`
  - `scratch/dump_range.gdb                |    9 +`
  - `scratch/dump_scene_fn.py              |   53 +`
  - `scratch/inspect.gdb                   |   14 +`
  - `scratch/inspect2.gdb                  |    4 +`
  - `scratch/mem_dump.bin                  |  Bin 0 -> 192 bytes`
  - *(and 9 more files)*

### 43. [`75cbad1a7`](https://github.com/marmarama/86Box/commit/75cbad1a7dcad15602c1771c97da94051c8d3660) — docs(dynarec): add architectural review and future optimization plan
- **Date**: 2026-10-23
- **Author**: James Young
- **Original Commit Message**: `review fixes and planning`
- **Diff Stat**: 82 files changed, 710 insertions(+), 106279 deletions(-)
- **Changes**:
  - Add `REVIEW2.md` and `CODEGEN_FUTURE.md` detailing planned optimizations (in-JIT event loop, flag fast-paths, persistent dynarec).
- **Key Files**:
  - `.gitignore                                         |      6 +`
  - `.vscode-ctags                                      | 102885 ------------------`
  - `.../__pycache__/run_benchmarks.cpython-312.pyc     |    Bin 35379 -> 0 bytes`
  - `perf.data                                          |      0`
  - `scratch/DIBENG.DLL                                 |    Bin 202800 -> 0 bytes`
  - `scratch/PCPBENCH.EXE                               |    Bin 111814 -> 0 bytes`
  - `scratch/audit_rex.py                               |     70 -`
  - `.../bp_doom_reboot_detected_20261011_110447.png    |    Bin 401379 -> 0 bytes`
  - *(and 74 more files)*

### 44. [`b2600e3f8`](https://github.com/marmarama/86Box/commit/b2600e3f8b3449a5fd94c8f78b4a22865ef6ed77) — docs(dynarec): design execution block dumper and profiler architecture
- **Date**: 2026-10-23
- **Author**: James Young
- **Original Commit Message**: `update profiler plan`
- **Diff Stat**: 6 files changed, 131 insertions(+), 11 deletions(-)
- **Changes**:
  - Draft implementation plan for binary block dumper in `DEBUG-BLOCK-DUMPER.md`.
- **Key Files**:
  - `src/codegen_new/CMakeLists.txt             |  1 +`
  - `src/codegen_new/codegen.c                  | 23 ++++++++------`
  - `src/codegen_new/codegen.h                  | 18 +++++++++++`
  - `src/codegen_new/codegen_ir.c               | 49 ++++++++++++++++++++++++++++++`
  - `src/codegen_new/codegen_ir_defs.h          |  6 ++++`
  - `src/codegen_new/docs/DEBUG-BLOCK-DUMPER.md | 45 +++++++++++++++++++++++++--`

### 45. [`5034197f8`](https://github.com/marmarama/86Box/commit/5034197f8d4b823f844614516f6e50ada020a4eb) — feat(dynarec): implement execution block dumper and offline telemetry analyzer
- **Date**: 2026-10-23
- **Author**: James Young
- **Original Commit Message**: `add block dumper/profiler`
- **Diff Stat**: 12 files changed, 1894 insertions(+), 25 deletions(-)
- **Changes**:
  - Implement in-engine binary block dump engine (`codegen_dump.c`, `codegen_dump.h`) enabled via `BOX86_DUMP_BLOCKS`.
  - Implement `scripts/analyze_blocks.py` providing hotspot analysis, disassembly inspection, and execution metrics.
- **Key Files**:
  - `scripts/analyze_blocks.py                     | 950 ++++++++++++++++++++++++++`
  - `src/codegen_new/codegen.h                     |   7 +-`
  - `src/codegen_new/codegen_backend_x86-64.c      |  24 +-`
  - `src/codegen_new/codegen_backend_x86-64_uops.c |  18 +-`
  - `src/codegen_new/codegen_block.c               |  11 +`
  - `src/codegen_new/codegen_dump.c                | 602 ++++++++++++++++`
  - `src/codegen_new/codegen_dump.h                | 193 ++++++`
  - `src/codegen_new/codegen_ir.c                  |   2 +-`
  - *(and 4 more files)*

### 46. [`9d199f666`](https://github.com/marmarama/86Box/commit/9d199f66664e3eab12c61d11487db2b83f257960) — fix(dynarec): resolve block dumper synchronization and buffer overflow issues
- **Date**: 2026-10-23
- **Author**: James Young
- **Original Commit Message**: `fix code block dumper`
- **Diff Stat**: 13 files changed, 324 insertions(+), 106 deletions(-)
- **Changes**:
  - Fix dump buffer boundary checks and ensure multi-threaded block write safety in `codegen_dump.c`.
- **Key Files**:
  - `scripts/analyze_blocks.py                          |  40 +++--`
  - `src/codegen_new/codegen.c                          |  28 +++-`
  - `src/codegen_new/codegen_backend_arm64_ops.c        |   3 +`
  - `src/codegen_new/codegen_backend_loongarch64_ops.c  |   3 +`
  - `src/codegen_new/codegen_backend_x86-64.c           |   8 +`
  - `.../codegen_backend_x86-64_ops_helpers.h           |   4 +`
  - `src/codegen_new/codegen_block.c                    |   8 +-`
  - `src/codegen_new/codegen_dump.c                     | 182 +++++++++++++++++----`
  - *(and 5 more files)*

### 47. [`796b13d4d`](https://github.com/marmarama/86Box/commit/796b13d4d4cfeae863ae41b3c96e94fe3ffb95a5) — feat(dynarec): implement far CALL instruction decoding and validation
- **Date**: 2026-10-23
- **Author**: James Young
- **Original Commit Message**: `update`
- **Diff Stat**: 3 files changed, 256 insertions(+)
- **Changes**:
  - Add far CALL operand decoding and prepare descriptor checks in `codegen_ops_jump.c`.
- **Key Files**:
  - `PROFILE-WIN98.md                      |   1 +`
  - `src/86box.c                           |   1 +`
  - `src/codegen_new/docs/PROFILE-WIN98.md | 254 ++++++++++++++++++++++++++++++++++`

### 48. [`9dfa35a42`](https://github.com/marmarama/86Box/commit/9dfa35a424e363c0b0e615fe8b3c48f8fb313ae1) — feat(dynarec): implement native lowering for far CALL instructions
- **Date**: 2026-10-23
- **Author**: James Young
- **Original Commit Message**: `recompile far call`
- **Diff Stat**: 8 files changed, 217 insertions(+), 9 deletions(-)
- **Changes**:
  - Implement native translation for far CALL (`ropCALL_far_16`, `ropCALL_far_32`) with stack segment updates.
- **Key Files**:
  - `src/codegen_new/codegen.c                     |   4 +-`
  - `src/codegen_new/codegen_backend_x86-64_uops.c |  16 ++--`
  - `src/codegen_new/codegen_ops.c                 |   4 +-`
  - `src/codegen_new/codegen_ops_jump.c            |  34 +++++++`
  - `src/codegen_new/codegen_ops_jump.h            |   3 +`
  - `src/codegen_new/docs/PROFILE-WIN98.md         |  29 ++++++`
  - `src/cpu/x86seg.c                              | 128 ++++++++++++++++++++++++++`
  - `src/cpu/x86seg.h                              |   8 ++`

### 49. [`36efe04c2`](https://github.com/marmarama/86Box/commit/36efe04c26999487de4607b34aa7020b0671d085) — perf(dynarec): optimize far CALL segment loading and target block dispatch
- **Date**: 2026-10-23
- **Author**: James Young
- **Original Commit Message**: `update`
- **Diff Stat**: 5 files changed, 813 insertions(+), 13 deletions(-)
- **Changes**:
  - Inline segment register updates and target block dispatch for far CALL.
- **Key Files**:
  - `src/codegen_new/codegen_backend_arm64_uops.c       |  19 +-`
  - `src/codegen_new/codegen_backend_loongarch64_uops.c |  19 +-`
  - `src/codegen_new/codegen_backend_x86-64_uops.c      |  41 +-`
  - `src/codegen_new/codegen_ops_jit_wrappers.h         | 718 +++++++++++++++++++++`
  - `src/codegen_new/docs/PROFILE-WIN98.md              |  29 +`

### 50. [`3eb02b4da`](https://github.com/marmarama/86Box/commit/3eb02b4da4aa9a0f93303474276da46e2aa38508) — feat(dynarec): add TSC cycle attribution and timing telemetry to block dumper
- **Date**: 2026-10-23
- **Author**: James Young
- **Original Commit Message**: `add profiling data to block dumper`
- **Diff Stat**: 7 files changed, 313 insertions(+), 49 deletions(-)
- **Changes**:
  - Add hardware TSC timing probes around block execution and C helper callouts in `codegen_dump.c`.
  - Update `analyze_blocks.py` to calculate pure JIT execution percentage vs C callout overhead.
- **Key Files**:
  - `scripts/analyze_blocks.py                  | 217 +++++++++++++++++++++++++----`
  - `src/86box.c                                |   3 +`
  - `src/codegen_new/codegen_dump.c             |  53 ++++++-`
  - `src/codegen_new/codegen_dump.h             |  15 ++`
  - `src/codegen_new/docs/DEBUG-BLOCK-DUMPER.md |  23 ++-`
  - `src/codegen_new/docs/PROFILE-WIN98.md      |  50 ++++---`
  - `src/cpu/codegen_public.h                   |   1 +`

### 51. [`2ca82197f`](https://github.com/marmarama/86Box/commit/2ca82197f43f262429f23ec93a3903ba23922b7e) — feat(dynarec, scripts): implement hang detection and watchdog monitoring in block dumper
- **Date**: 2026-10-24
- **Author**: James Young
- **Original Commit Message**: `hang check`
- **Diff Stat**: 7 files changed, 581 insertions(+), 7 deletions(-)
- **Changes**:
  - Add signal-driven hang detection (`BOX86_HANG_DETECT=1`) in `codegen_dump.c`.
  - Add `--hang` diagnostic flag to `scripts/analyze_blocks.py` to trace stuck blocks.
- **Key Files**:
  - `scripts/analyze_blocks.py                  | 204 ++++++++++++++++++++++++-`
  - `src/codegen_new/codegen_backend_x86-64.c   |   2 +-`
  - `src/codegen_new/codegen_dump.c             | 236 ++++++++++++++++++++++++++++-`
  - `src/codegen_new/codegen_dump.h             |  41 ++++-`
  - `src/codegen_new/docs/DEBUG-BLOCK-DUMPER.md |  70 ++++++++-`
  - `src/codegen_new/docs/PROFILE-WIN98.md      |  33 ++++`
  - `src/cpu/386_dynarec.c                      |   2 +-`

### 52. [`004875c8f`](https://github.com/marmarama/86Box/commit/004875c8f38cfd57fba7f65bc2a170bda4c69df2) — fix(dynarec): fix REP string loop cycle deduction and LFB wait-state preservation
- **Date**: 2026-10-24
- **Author**: James Young
- **Original Commit Message**: `fix string rep`
- **Diff Stat**: 4 files changed, 762 insertions(+), 50 deletions(-)
- **Changes**:
  - Fix critical Bug C-3 where `IREG_cycles` in host register erased VESA LFB video memory wait-state cycle deductions.
  - Add automated rebuild and deploy helper script `scripts/rebuild_deploy.sh` and root `rebuild.sh` shortcut.
- **Key Files**:
  - `rebuild.sh                            |   1 +`
  - `scripts/rebuild_deploy.sh             | 114 +++++++++++`
  - `src/codegen_new/codegen_ops_string.c  | 337 ++++++++++++++++++++++++++++---`
  - `src/codegen_new/docs/PROFILE-WIN98.md | 360 +++++++++++++++++++++++++++++++---`

### 53. [`ee34688e5`](https://github.com/marmarama/86Box/commit/ee34688e54aae24315e1f681c37f890a1f19a309) — docs(dynarec): document Windows 98 boot profiling telemetry (v2)
- **Date**: 2026-10-24
- **Author**: James Young
- **Original Commit Message**: `reprofile`
- **Diff Stat**: 2 files changed, 274 insertions(+)
- **Changes**:
  - Create `PROFILE-WIN98-v2.md` documenting hotspot breakdown and ranking optimization priorities.
- **Key Files**:
  - `PROFILE-WIN98-v2.md                      |   1 +`
  - `src/codegen_new/docs/PROFILE-WIN98-v2.md | 273 +++++++++++++++++++++++++++++++`

### 54. [`da4214561`](https://github.com/marmarama/86Box/commit/da4214561fae880edf93952223e6e2431cf4e002) — docs(dynarec): update Windows 98 profiling data and optimization priorities
- **Date**: 2026-10-24
- **Author**: James Young
- **Original Commit Message**: `update`
- **Diff Stat**: 14 files changed, 1072 insertions(+), 50 deletions(-)
- **Changes**:
  - Refine priority matrix in `PROFILE-WIN98-v2.md` based on updated boot telemetry.
- **Key Files**:
  - `src/codegen_new/codegen_backend_x86-64_ops.c  |  55 ++++`
  - `src/codegen_new/codegen_backend_x86-64_ops.h  |   5 +`
  - `src/codegen_new/codegen_backend_x86-64_uops.c |  59 ++++`
  - `src/codegen_new/codegen_ir.c                  |   3 +`
  - `src/codegen_new/codegen_ir_defs.h             |   6 +`
  - `src/codegen_new/codegen_ops.c                 |  12 +-`
  - `src/codegen_new/codegen_ops_arith.c           | 184 +++++++++++++`
  - `src/codegen_new/codegen_ops_arith.h           |   5 +`
  - *(and 6 more files)*

### 55. [`8f628c88c`](https://github.com/marmarama/86Box/commit/8f628c88c1f30e49206122e9b27a946c50fde23f) — feat(cpu, acpi): implement cycle-exact idle pacing and ACPI C2/C3 sleep
- **Date**: 2026-10-25
- **Author**: James Young
- **Original Commit Message**: `cpu, acpi, codegen_new: Implement cycle-exact idle pacing and ACPI C2/C3 sleep`
- **Diff Stat**: 8 files changed, 183 insertions(+), 9 deletions(-)
- **Changes**:
  - Add `acpi_c2_c3_sleep()` in `src/acpi.c` entering power-saving sleep when OS reads P_LVL2 / P_LVL3.
  - Advance guest cycles up to `timer_target - tsc` when CPU is idle with interrupts disabled, eliminating 100% host CPU burn.
- **Key Files**:
  - `src/acpi.c                                | 55 ++++++++++++++++++++++++`
  - `src/codegen_new/codegen_dump.c            |  6 +++`
  - `src/codegen_new/codegen_dump.h            |  1 +`
  - `src/codegen_new/docs/IDLE-OPTIMIZATION.md | 70 +++++++++++++++++++++++++++++--`
  - `src/cpu/386_common.c                      |  1 +`
  - `src/cpu/386_dynarec.c                     | 23 +++++++++-`
  - `src/cpu/cpu.h                             |  1 +`
  - `src/cpu/x86_ops_misc.h                    | 35 ++++++++++++++--`

### 56. [`8167987a9`](https://github.com/marmarama/86Box/commit/8167987a97dbcdca9929b2b643716c9aadcc64ae) — docs(dynarec): update optimization priorities for integer division lowering
- **Date**: 2026-10-25
- **Author**: James Young
- **Original Commit Message**: `docs: Update optimization priorities in PROFILE-WIN98-v2.md for DIV in-progress`
- **Diff Stat**: 1 file changed, 8 insertions(+), 6 deletions(-)
- **Changes**:
  - Update `PROFILE-WIN98-v2.md` with planned `DIV` lowering details.
- **Key Files**:
  - `src/codegen_new/docs/PROFILE-WIN98-v2.md | 14 ++++++++------`

### 57. [`f4c5ed4e4`](https://github.com/marmarama/86Box/commit/f4c5ed4e446cad52df537919f8bb61983d01a679) — docs(dynarec): add Priority 2 & 3 targets to verification summary in PROFILE-WIN98-v2.md
- **Date**: 2026-10-25
- **Author**: James Young
- **Original Commit Message**: `docs: Add Priority 2 & 3 to Summary Verification Table in PROFILE-WIN98-v2.md`
- **Diff Stat**: 1 file changed, 2 insertions(+)
- **Changes**:
  - Update summary table with target fallback counts.
- **Key Files**:
  - `src/codegen_new/docs/PROFILE-WIN98-v2.md | 2 ++`

### 58. [`e5ded38a6`](https://github.com/marmarama/86Box/commit/e5ded38a61f721f5de1be2f3fbf8ac4ad3c68c04) — feat(dynarec): implement native JIT lowering for unsigned integer division (DIV rm8/16/32)
- **Date**: 2026-10-25
- **Author**: James Young
- **Original Commit Message**: `codegen_new: Implement JIT lowering for unsigned integer division (DIV rm8/16/32)`
- **Diff Stat**: 2 files changed, 46 insertions(+), 8 deletions(-)
- **Changes**:
  - Implement native JIT handlers and C helpers for `DIV rm8`, `DIV rm16`, `DIV rm32` in `codegen_ops_misc.c`.
  - Properly handle `#DE` divide-by-zero and quotient overflow exceptions with architectural PC preservation.
- **Key Files**:
  - `src/codegen_new/codegen_ops_misc.c       | 36 +++++++++++++++++++++++++++++---`
  - `src/codegen_new/docs/PROFILE-WIN98-v2.md | 18 +++++++++++-----`

### 59. [`4fa0dd301`](https://github.com/marmarama/86Box/commit/4fa0dd3014906e36680caa34b15d96a772cfde7c) — revert(cpu, acpi): temporarily revert ACPI C2/C3 sleep to isolate desktop freeze
- **Date**: 2026-10-25
- **Author**: James Young
- **Original Commit Message**: `cpu, acpi: Revert cycle-exact idle pacing and ACPI C2/C3 sleep to fix desktop freeze`
- **Diff Stat**: 5 files changed, 6 insertions(+), 109 deletions(-)
- **Changes**:
  - Temporarily revert `acpi_c2_c3_sleep` to diagnose a desktop freeze condition.
- **Key Files**:
  - `src/acpi.c             | 55 --------------------------------------------------`
  - `src/cpu/386_common.c   |  1 -`
  - `src/cpu/386_dynarec.c  | 23 ++-------------------`
  - `src/cpu/cpu.h          |  1 -`
  - `src/cpu/x86_ops_misc.h | 35 ++++----------------------------`

### 60. [`f70103af1`](https://github.com/marmarama/86Box/commit/f70103af1e830bced18f79e8f9c418bdfde4d7b3) — revert(cpu, acpi): restore ACPI C2/C3 sleep implementation
- **Date**: 2026-10-25
- **Author**: James Young
- **Original Commit Message**: `Revert "cpu, acpi: Revert cycle-exact idle pacing and ACPI C2/C3 sleep to fix desktop freeze"`
- **Diff Stat**: 5 files changed, 109 insertions(+), 6 deletions(-)
- **Changes**:
  - Restore ACPI C2/C3 sleep implementation following desktop freeze diagnosis.
- **Key Files**:
  - `src/acpi.c             | 55 ++++++++++++++++++++++++++++++++++++++++++++++++++`
  - `src/cpu/386_common.c   |  1 +`
  - `src/cpu/386_dynarec.c  | 23 +++++++++++++++++++--`
  - `src/cpu/cpu.h          |  1 +`
  - `src/cpu/x86_ops_misc.h | 35 ++++++++++++++++++++++++++++----`

### 61. [`4c9a3b8d0`](https://github.com/marmarama/86Box/commit/4c9a3b8d09d23cc4c1bdde5973c8cedf3493b870) — fix(cpu, acpi): refine ACPI HLT and sleep cycle advancement with pending interrupt checks
- **Date**: 2026-10-25
- **Author**: James Young
- **Original Commit Message**: `fix ACPI HLT`
- **Diff Stat**: 4 files changed, 34 insertions(+), 14 deletions(-)
- **Changes**:
  - Check for pending PIC interrupts (`pic.int_pending`), NMI, and SMI before sleeping in `acpi_c2_c3_sleep()`.
  - Fix cycle-exact idle pacing during `HLT` in `src/cpu/386_dynarec.c`.
  - Document architecture and verification in `IDLE-OPTIMIZATION.md`.
- **Key Files**:
  - `src/acpi.c                                | 16 +++++++++++-----`
  - `src/codegen_new/docs/IDLE-OPTIMIZATION.md | 24 ++++++++++++++++++------`
  - `src/cpu/386_dynarec.c                     |  2 +-`
  - `src/cpu/x86_ops_misc.h                    |  6 ++++--`

### 62. [`1cb5f6db7`](https://github.com/marmarama/86Box/commit/1cb5f6db7556c1911b274993d908ef6f764b62c2) — feat(dynarec): implement native lowering for RDTSC instruction (0F 31)
- **Date**: 2026-10-26
- **Author**: James Young
- **Original Commit Message**: `RDTSC lowering`
- **Diff Stat**: 5 files changed, 351 insertions(+), 4 deletions(-)
- **Changes**:
  - Implement `ropRDTSC` in `codegen_ops_misc.c` reading current 64-bit guest TSC into `EDX:EAX`.
  - Eliminate 743,000 interpreter fallbacks in Windows 98 boot session.
  - Document results in `PROFILE-WIN98-v3.md`.
- **Key Files**:
  - `src/codegen_new/codegen_ops.c              |   8 +-`
  - `src/codegen_new/codegen_ops_jit_wrappers.h |  12 ++`
  - `src/codegen_new/codegen_ops_misc.c         |  18 ++`
  - `src/codegen_new/codegen_ops_misc.h         |   1 +`
  - `src/codegen_new/docs/PROFILE-WIN98-v3.md   | 316 +++++++++++++++++++++++++++++`

### 63. [`ebb7f845d`](https://github.com/marmarama/86Box/commit/ebb7f845d0bc315fe62764c1f2c8d640093c23c4) — feat(dynarec): implement inlined fast-path lowering for CLI and STI instructions
- **Date**: 2026-10-26
- **Author**: James Young
- **Original Commit Message**: `CLI/STI lowering`
- **Diff Stat**: 3 files changed, 53 insertions(+), 20 deletions(-)
- **Changes**:
  - Implement inlined fast-path for `CLI` and `STI` in `codegen_ops_misc.c` when `CPL <= IOPL`.
  - Directly update `IREG_flags` without C helper callouts in >99.9% of executions.
  - Preserve C helper callout when `CPL > IOPL` to support VME/PVI virtualization faults.
- **Key Files**:
  - `src/codegen_new/codegen_ops_jit_wrappers.h | 24 +++++++++++++++`
  - `src/codegen_new/codegen_ops_misc.c         |  2 +-`
  - `src/codegen_new/docs/PROFILE-WIN98-v3.md   | 47 ++++++++++++++++++------------`

### 64. [`73e79c1d1`](https://github.com/marmarama/86Box/commit/73e79c1d1c4c1cf0d14a071a28a7457ff6723969) — feat(dynarec, cpus): add flat-segment string acceleration, stack push/pop helpers, and CPU timing framework
- **Date**: 2026-10-27
- **Author**: James Young
- **Original Commit Message**: `updates`
- **Diff Stat**: 77 files changed, 6803 insertions(+), 21 deletions(-)
- **Changes**:
  - Accelerate flat-segment string operations in `codegen_ops_string.c` bypassing redundant limit checks.
  - Implement fast push/pop segment register operations in `codegen_ops_stack.c`.
  - Establish CPU timing specification framework under `src/codegen_new/cpus/` with YAML definitions.
- **Key Files**:
  - `src/codegen_new/codegen_ops.c                      |  16 +-`
  - `src/codegen_new/codegen_ops_jit_wrappers.h         | 268 ++++++++`
  - `src/codegen_new/codegen_ops_jump.c                 |  65 ++`
  - `src/codegen_new/codegen_ops_jump.h                 |   3 +`
  - `src/codegen_new/codegen_ops_misc.c                 |  83 ++-`
  - `src/codegen_new/codegen_ops_misc.h                 |   3 +`
  - `src/codegen_new/codegen_ops_stack.c                |  49 ++`
  - `src/codegen_new/codegen_ops_stack.h                |   2 +`
  - *(and 69 more files)*

### 65. [`6f1cfd0dd`](https://github.com/marmarama/86Box/commit/6f1cfd0dd96ee193e93f15f1ce5600aaa4e08fe2) — feat(cpus): expand CPU architecture timing tables for AMD, Cyrix, Intel, and VIA
- **Date**: 2026-10-27
- **Author**: James Young
- **Original Commit Message**: `updates`
- **Diff Stat**: 68 files changed, 3262 insertions(+), 717 deletions(-)
- **Changes**:
  - Add detailed YAML specifications for AMD K6, K6-2+, K6-3, Cyrix 6x86, Pentium, Pentium Pro, Nehemiah.
  - Specify cycle execution costs and pipeline parameters.
- **Key Files**:
  - `src/codegen_new/cpus/README.md                     |  42 +-`
  - `src/codegen_new/cpus/amd/am386dx.yaml              |   6 +`
  - `src/codegen_new/cpus/amd/am486dx4.yaml             |   4 +`
  - `src/codegen_new/cpus/amd/am5x86.yaml               |   4 +`
  - `src/codegen_new/cpus/amd/k6.yaml                   |  38 ++`
  - `src/codegen_new/cpus/amd/k6_2.yaml                 |  16 +-`
  - `src/codegen_new/cpus/amd/k6_2plus.yaml             |  45 ++`
  - `src/codegen_new/cpus/amd/k6_3.yaml                 |  41 ++`
  - *(and 60 more files)*

### 66. [`9850c2d7d`](https://github.com/marmarama/86Box/commit/9850c2d7d4a470cb8b2c85bcb4a2259e4fca11bc) — feat(cpus): implement x86gen code generator for automated instruction emitter synthesis
- **Date**: 2026-10-27
- **Author**: James Young
- **Original Commit Message**: `updates`
- **Diff Stat**: 8 files changed, 826 insertions(+), 146 deletions(-)
- **Changes**:
  - Implement `src/codegen_new/cpus/x86gen.py` translating YAML ISA definitions into dynarec C helper emitters.
  - Add ISA specifications for arithmetic, logic, bit, and shift operations.
- **Key Files**:
  - `src/codegen_new/cpus/README.md                   |   1 +`
  - `src/codegen_new/cpus/ROADMAP.md                  | 187 +++++++++++++++++`
  - `src/codegen_new/cpus/isa/arithmetic.yaml         |  92 +++++----`
  - `src/codegen_new/cpus/isa/logic_and_bit.yaml      |  91 ++++++---`
  - `src/codegen_new/cpus/isa/mov_and_transfer.yaml   |  13 +-`
  - `src/codegen_new/cpus/isa/shift_rotate.yaml       | 176 ++++++++++------`
  - `src/codegen_new/cpus/isa/types_and_operands.yaml | 167 +++++++++++++--`
  - `src/codegen_new/cpus/x86gen.py                   | 245 ++++++++++++++++++++++-`

### 67. [`49b0dffdb`](https://github.com/marmarama/86Box/commit/49b0dffdbc8e19e63d48c2c2e895344c6c71d4f2) — feat(cpus): update CPU nominal clock frequencies across definitions
- **Date**: 2026-10-28
- **Author**: James Young
- **Original Commit Message**: `update frequencies`
- **Diff Stat**: 45 files changed, 147 insertions(+), 44 deletions(-)
- **Changes**:
  - Update CPU nominal clock frequencies across YAML definitions for accurate timing emulation.
- **Key Files**:
  - `src/codegen_new/cpus/amd/am386dx.yaml               |  4 +++-`
  - `src/codegen_new/cpus/amd/am486dx4.yaml              |  4 +++-`
  - `src/codegen_new/cpus/amd/am5x86.yaml                |  4 +++-`
  - `src/codegen_new/cpus/amd/geode_lx800.yaml           |  4 +++-`
  - `src/codegen_new/cpus/amd/k6.yaml                    |  4 +++-`
  - `src/codegen_new/cpus/amd/k6_2.yaml                  |  4 +++-`
  - `src/codegen_new/cpus/amd/k6_2plus.yaml              |  4 +++-`
  - `src/codegen_new/cpus/amd/k6_3.yaml                  |  4 +++-`
  - *(and 37 more files)*

### 68. [`074dda0bb`](https://github.com/marmarama/86Box/commit/074dda0bb71ee095ebfd287260c5109ea2f6b121) — feat(dynarec): implement native lowering for IN/OUT port I/O, VERR/VERW, and segment PUSH/POP
- **Date**: 2026-10-28
- **Author**: James Young
- **Original Commit Message**: `more lowering`
- **Diff Stat**: 24 files changed, 1001 insertions(+), 74 deletions(-)
- **Changes**:
  - Implement native JIT lowering for `IN` and `OUT` (`0xE4-0xE7`, `0xEC-0xEF`) port I/O with TSS permission checks in `codegen_ops_misc.c`.
  - Implement native lowering for descriptor validation `VERR` and `VERW` (`0x0F 0x00 /4, /5`).
  - Implement native JIT lowering for PUSH/POP segment registers (`FS`, `GS`, `DS`, `ES`, `SS`) in `codegen_ops_stack.c`.
  - Eliminate millions of interpreter fallbacks across Windows 98 boot.
- **Key Files**:
  - `src/codegen_new/codegen.c                          |   4 +`
  - `src/codegen_new/codegen_ir.c                       |  14 +-`
  - `src/codegen_new/codegen_ops.c                      |   4 +-`
  - `src/codegen_new/codegen_ops_jit_wrappers.h         | 206 +++++++++++++++++++`
  - `src/codegen_new/codegen_ops_jump.c                 |  46 +++--`
  - `src/codegen_new/codegen_ops_misc.c                 |  74 +++++++`
  - `src/codegen_new/codegen_ops_misc.h                 |   3 +`
  - `src/codegen_new/codegen_reg.c                      |   7 +`
  - *(and 16 more files)*

### 69. [`6c2695b03`](https://github.com/marmarama/86Box/commit/6c2695b03634d23cd0a683011bd82e60dd31b369) — feat(dynarec, scripts): implement PUSHF/POPF lowering and streaming telemetry analyzer
- **Date**: 2026-10-29
- **Author**: James Young
- **Original Commit Message**: `update`
- **Diff Stat**: 6 files changed, 523 insertions(+), 185 deletions(-)
- **Changes**:
  - Implement native lowering for `POPF` and `PUSHF` in `codegen_ops_stack.c` with `CPL <= IOPL` fast-path.
  - Rewrite `scripts/analyze_blocks.py` to use streaming JSON parser, reducing analysis memory from 50% RAM to minimal usage.
  - Add `PROFILE-WIN98-v4.md` documenting new optimization priorities.
- **Key Files**:
  - `scripts/analyze_blocks.py                | 423 ++++++++++++++++++-------------`
  - `src/codegen_new/codegen_ops.c            |   4 +-`
  - `src/codegen_new/codegen_ops_stack.c      |  51 ++++`
  - `src/codegen_new/codegen_ops_stack.h      |   3 +`
  - `src/codegen_new/docs/PROFILE-WIN98-v3.md |  45 ++++`
  - `src/codegen_new/docs/PROFILE-WIN98-v4.md | 182 +++++++++++++`

### 70. [`e6d27ce19`](https://github.com/marmarama/86Box/commit/e6d27ce1930436ba7d49d31940fbc666738f8ff6) — feat(dynarec): inline flags_rebuild fast-paths, lower SAHF/LAHF, and optimize variable shifts by %cl
- **Date**: 2026-10-30
- **Author**: James Young
- **Original Commit Message**: `more lowering and fixing`
- **Diff Stat**: 8 files changed, 479 insertions(+), 375 deletions(-)
- **Changes**:
  - Inline ZF, CF, and SF flag evaluation in `codegen_ops_helpers.h` (`rebuild_c`), eliminating 39.99 Billion C callout cycles.
  - Implement native JIT lowering for `SAHF` (`0x9E`) and `LAHF` (`0x9F`), eliminating 3.75 Million fallbacks.
  - Implement native conditional zero-shift bypass (`uop_CMP_IMM_JZ_DEST`) in `codegen_ops_shift.c`, eliminating 8.03 Million variable shift fallbacks.
- **Key Files**:
  - `scripts/analyze_blocks.py                |   8 +-`
  - `src/codegen_new/codegen_ops.c            |   4 +-`
  - `src/codegen_new/codegen_ops_arith.c      |  26 --`
  - `src/codegen_new/codegen_ops_helpers.h    |  76 ++++`
  - `src/codegen_new/codegen_ops_misc.c       |  60 +--`
  - `src/codegen_new/codegen_ops_misc.h       |   3 +`
  - `src/codegen_new/codegen_ops_shift.c      | 613 +++++++++++++++----------------`
  - `src/codegen_new/docs/PROFILE-WIN98-v4.md |  64 ++++`

### 71. [`ab06f6882`](https://github.com/marmarama/86Box/commit/ab06f6882923c5b7e764342fc6cd4b97936e179b) — feat(dynarec): implement native JIT lowering for LAR and LSL descriptor instructions (0F 02 / 0F 03)
- **Date**: 2026-10-30
- **Author**: James Young
- **Original Commit Message**: `more lowering`
- **Diff Stat**: 5 files changed, 469 insertions(+), 4 deletions(-)
- **Changes**:
  - Implement typed descriptor validation helpers `jit_lar16`, `jit_lar32`, `jit_lsl16`, and `jit_lsl32` in `codegen_ops_jit_wrappers.h`.
  - Enforce protected-mode checks upfront and conditional register writeback via `uop_CMP_IMM_JZ_DEST` based on ZF.
  - Eliminate 5.20 Million interpreter fallbacks across Windows 98 boot session.
- **Key Files**:
  - `src/codegen_new/codegen_ops.c              |   8 +-`
  - `src/codegen_new/codegen_ops_jit_wrappers.h | 346 +++++++++++++++++++++++++++++`
  - `src/codegen_new/codegen_ops_misc.c         |  69 ++++++`
  - `src/codegen_new/codegen_ops_misc.h         |   4 +`
  - `src/codegen_new/docs/PROFILE-WIN98-v4.md   |  46 ++++`

### 72. [`a4334a37a`](https://github.com/marmarama/86Box/commit/a4334a37a84750022f2bb1d8322a15396667d2c0) — feat(dynarec): implement backward dead store elimination pass for lazy flag registers in IR
- **Date**: 2026-10-30
- **Author**: James Young
- **Original Commit Message**: `update`
- **Diff Stat**: 2 files changed, 174 insertions(+)
- **Changes**:
  - Implement backward liveness analysis in `codegen_ir.c` (`codegen_ir_dead_flags_elimination`).
  - Prune redundant dead writes to `IREG_flags_op`, `res`, `op1`, and `op2` across basic block operations.
- **Key Files**:
  - `src/codegen_new/codegen_ir.c | 173 +++++++++++++++++++++++++++++++++++++++++++`
  - `src/codegen_new/codegen_ir.h |   1 +`

### 73. [`bfa506b4a`](https://github.com/marmarama/86Box/commit/bfa506b4a9b9e7e8b3b694c0f8c6c8c1c6cbff10) — feat(dynarec): implement continuous in-JIT event loop and replenishment (codegen_event_stub)
- **Date**: 2026-10-31
- **Author**: James Young
- **Original Commit Message**: `codegen_new: implement continuous in-JIT event loop and replenishment (codegen_event_stub)`
- **Diff Stat**: 5 files changed, 150 insertions(+), 18 deletions(-)
- **Changes**:
  - Implement `codegen_event_stub` in `codegen_backend_x86-64.c` called directly from JIT code when `_cycles <= 0`.
  - Implement `codegen_handle_jit_event()` in `src/cpu/386_dynarec.c` synchronizing guest TSC, processing expired timers, and replenishing slices without exiting to C dispatch.
  - Radically reduces host function call overhead and dispatcher re-entry churn.
- **Key Files**:
  - `src/codegen_new/codegen.h                     |  4 ++`
  - `src/codegen_new/codegen_backend_x86-64.c      | 48 +++++++++++++++-`
  - `src/codegen_new/codegen_backend_x86-64_defs.h |  1 +`
  - `src/codegen_new/codegen_backend_x86-64_uops.c | 34 +++++------`
  - `src/cpu/386_dynarec.c                         | 81 +++++++++++++++++++++++++++`

### 74. [`a222ae4fd`](https://github.com/marmarama/86Box/commit/a222ae4fd153a4c318c2648c0528e8bd125bc125) — fix(dynarec): fix persistent in-JIT event loop cycle synchronization and register reload
- **Date**: 2026-10-31
- **Author**: James Young
- **Original Commit Message**: `fix persistent jit`
- **Diff Stat**: 4 files changed, 58 insertions(+), 35 deletions(-)
- **Changes**:
  - Synchronize cycle accounting across `codegen_event_stub` invocations.
  - Ensure host registers are correctly restored after hardware timer processing.
  - Fix exit request status propagation in `src/cpu/386_dynarec.c`.
- **Key Files**:
  - `src/codegen_new/codegen_ir.c  | 15 +++++-----`
  - `src/codegen_new/codegen_reg.c |  1 +`
  - `src/codegen_new/codegen_reg.h |  8 +++--`
  - `src/cpu/386_dynarec.c         | 69 +++++++++++++++++++++++++++----------------`


# 86Box Agent Guide

Essential knowledge for working on **86Box**, a cycle-accurate x86 PC emulator (IBM PC/XT/AT/PS/2 through PCI/AGP clones). This file is authoritative; `src/codegen_new/docs/AGENTS.md` is a legacy subset.

Further docs: `src/codegen_new/docs/` (`DEBUGGING.md`, `BENCHMARKING.md`, `BUGS.md`, `ROADMAP.md`, `DEBUG-BLOCK-DUMPER.md`, `IDLE-OPTIMIZATION.md`, `CHANGELOG.md`, `PROFILE-WIN98*.md`), `benchmarks/README.md`, `tests/README.md`, `CONTRIBUTING.md` (upstream code style).

Skills (`.agents/skills/`): [`run-dos-benchmarks`](.agents/skills/run-dos-benchmarks/SKILL.md), [`boot-test-guests`](.agents/skills/boot-test-guests/SKILL.md), [`bisect-dynarec-regression`](.agents/skills/bisect-dynarec-regression/SKILL.md), [`analyze-block-dump`](.agents/skills/analyze-block-dump/SKILL.md), [`commit-work`](.agents/skills/commit-work/SKILL.md).

## 1. Repository Map

| Directory | Contents |
| :--- | :--- |
| `src/cpu/` | Interpreter (`386.c`, `x86_ops_*.h`), segments (`x86seg.c`, `x86seg_common.c`), legacy dynarec (`386_dynarec.c`), MMU (`386_mmu.c`), exceptions (`x86_exceptions.c`) |
| `src/codegen_new/` | New dynarec (x86-64, ARM64, LoongArch64): IR (`codegen_ir.c`), regalloc (`codegen_reg.c`), blocks (`codegen_block.c`), emitters (`codegen_ops_*.c`), backends (`codegen_backend_*_uops.c`) |
| `src/mem/` | `mem.c`: physical memory dispatch, `readlookup2`/`writelookup2`, SMC tracking (`dirty_mask`, `code_present_mask`) |
| `src/chipset/` | Motherboard chipsets, PIC/PIT/DMA/APM/ACPI, PCI host bridges |
| `src/video/` | MDA through SVGA, 3dfx Voodoo (incl. Banshee/3 in `vid_voodoo_banshee.c`) |
| `src/disk/` | IDE/ATAPI, SCSI, floppy, MiniVHD |
| `src/sound/`, `src/network/`, `src/input/`, `src/device/` | Audio, NICs and backends, keyboard/mice/joysticks, UART/parallel/RTC |
| `src/qt/` | Qt frontend |
| `benchmarks/` | Headless DOS benchmark runner (`run_benchmarks.py`) |
| `scripts/` | `analyze_blocks.py` (block trace analyzer), `rebuild_deploy.sh` |

## 2. Core Invariants

### CPU execution & timing
- `pc_run()` runs time slices via `exec386(cycles)` (interpreter) or `exec386_dynarec(cycles)`.
- The dynarec translates guest basic blocks to IR (`ir_data_t`), then to host code, with direct block chaining (`codeblock_link_t`).
- `cpu_state._cycles` is the remaining slice. `codegen_EXIT_BLOCK` checks `cycles <= 0`, pending maskable interrupts (`pic.int_pending && IF`) and `cpu_state.exit_request`.
- Wait states deducted by MMIO handlers subtract from `_cycles` and must survive in-block loops and C callouts.

### Memory
- `readlookup2[addr >> 12]` / `writelookup2[...]` map 4KB pages to host pointers (`(uintptr_t) ram_ptr - page_base`). `(uintptr_t) -1` means invalid, unmapped or MMIO, so use the slow-path C helpers (`readmem*` / `writemem*`).
- `page->code_present_mask` marks pages containing compiled code. Any write (CPU or DMA) to such a page sets `cpu_state.exit_request |= EXIT_STATUS_BLOCK_END` and `page->dirty_mask`. The dispatcher then invalidates blocks (`codegen_unlink_block`) and retranslates.

### Segments & modes
- Real mode: `_cs = CS << 4`, 64KB limits. Protected mode: `loadseg()` (`x86seg.c`) loads GDT/LDT descriptors, which may be non-flat or expand-down.
- `CR0.PE` toggles (real/V86 ↔ protected) must flush cached descriptor assumptions and force block exits.

## 3. Gotchas

### 1. Win98 shutdown ("safe to turn off your computer")
- **Cause**: `ropLOOP`/`LOOPE`/`LOOPNE` direct-linked (`BLOCK_EXIT_COND_2`) without `uop_SUB_IMM(..., 1)`, so `CX`/`ECX` never decremented.
- **Rule**: loop instructions (`codegen_ops_branch.c`, `codegen_block.c`) must emit the decrement before the condition check.

### 2. Lazy flags: runtime vs compile-time
- **Symptom**: inverted jumps, crashes, premature loop exits.
- **Rule**: `cpu_state.flags_op` is runtime state. Never `switch` on it at compile time. Use `codegen_last_flags_op`, which invalidates on callouts.

### 3. SMC and direct linking
- **Symptom**: FreeDOS disk/MCB corruption under `LINK_REAL`.
- **Cause**: DOS patches interrupt vectors in-place (`mov [cs:patch+1], al; jmp short $+2; patch: int 00h`). Direct links skipped the `dirty_mask` check.
- **Rule**: in `src/mem/mem.c`, any store into memory with compiled code **must** set `EXIT_STATUS_BLOCK_END`.

### 4. No intra-block conditional branches
- **Symptom**: FreeDOS `JemmEx: exception 0D`.
- **Cause**: `ropSETcc` jumped over `uop_MOV_IMM`. The regalloc assumes linear execution, so the skipped MOV left garbage in host registers.
- **Rule**: use branchless helpers or native `UOP_SETCC` / `UOP_CMOV`.

### 5. High-byte registers vs REX (x86-64)
- ModR/M cannot encode `AH`–`DH` alongside a REX prefix. `HOST_REG_FLAG_NO_BH` in `codegen_reg.c` keeps `R10`–`R15` away from high-byte operands. Task 33 tracks emulating high bytes with shifts.

### 6. Cycle sync around C callouts
- **Symptom**: implausible benchmark scores (PCPBench 60 → 208 FPS).
- **Cause**: `IREG_cycles` held in a host register overwrote cycles deducted by C handlers (`video_timing_write_l`).
- **Rule**: always sync `IREG_cycles` around slow-path callouts and MMIO handlers.

## 4. Build & Workflow

Build both configs (binary: `build/new_dynarec/src/86Box`):
```bash
ninja -C build/new_dynarec -j$(nproc)
ninja -C build/new_dynarec_dev_debug -j$(nproc)
```
Or build and deploy atomically with `scripts/rebuild_deploy.sh` (`--release-only`, `--debug-only`, `--deploy-debug`; destination via `BOX86_EMU_DIR`, default `/home/marm/Emulation/86Box`). `rebuild.sh` is a copy.

### Before declaring work done
1. Both configs build cleanly.
2. Run the benchmarks (§5.1, skill `run-dos-benchmarks`) and compare with `benchmark_results.log`. Flag regressions and suspicious jumps (Gotcha 6).
3. For dynarec, memory or segment changes, boot-test DOS and Win98 (§5.2, skill `boot-test-guests`). Retry with `BOX86_LINK_MASK=0` to separate linking bugs from codegen bugs.
4. Update the Taskwarrior item (§6). Record any new pitfall in §3 and `src/codegen_new/docs/`.
5. **Always commit completed work** (skill `commit-work`) before ending the turn: one focused commit per logical change, `subsystem: summary` style, files staged explicitly (never `perf.data`, `scratch/`, `__pycache__`). Don't push unless asked.

### Sandbox
- Use `BypassSandbox: true` for anything touching `.git` locks (commits), the network, or `~/.task` / `~/.taskrc`.
- Never leave orphan emulator processes (`pkill -9 86Box`).

## 5. Testing & Debugging

### 1. DOS benchmarks
Headless; reads COM1 telemetry for Doom, PCPBench, Dhrystone and Quake:
```bash
python3 benchmarks/run_benchmarks.py --only new \
    --vm-dir /home/marm/Emulation/86Box/benchmark \
    --roms /home/marm/Emulation/86Box/roms \
    --new-emu build/new_dynarec/src/86Box --timeout 90
```

### 2. Win98 desktop check
Must reach the desktop (~35-42s) with network and Explorer up. Screenshot is triggered by GDB poking `mon_screenshots`.

### 3. GDB (`-DGDBSTUB=ON`)
- Guest CPU (BIOS, DOS, VxDs): `gdb -ex "target remote 127.0.0.1:12345"`.
- Host emulator internals: `gdb -p <86box_pid>`.
- Single-stepping recompiled guest code: set `BOX86_LINK_MASK=0`.

### 4. Block-linking env vars
`BOX86_LINK_MASK` (bitmask, default `LINK_DEFAULT` = `0xbf`, defined in `src/codegen_new/codegen.h`):

| Bit | Name | Meaning |
| :--- | :--- | :--- |
| `0x01` | `LINK_JMP` | `JMP rel` |
| `0x02` | `LINK_JCC` | `Jcc rel` |
| `0x04` | `LINK_LOOP` | `LOOP`/`LOOPE`/`LOOPNE`/`JCXZ` |
| `0x08` | `LINK_REAL` | Real-mode and V86 blocks |
| `0x10` | `LINK_PMODE` | Protected-mode blocks |
| `0x20` | `LINK_SAME_PAGE` | Intra-page links only |
| `0x40` | `LINK_NO_FPU` | Never link x87 blocks (off by default) |
| `0x80` | `LINK_RET` | Monomorphic near `RET` |

- `BOX86_LINK_LIMIT=N`: stop linking after N links. Binary-search N to find the corrupting link.
- Block dumping (`codegen_block.c`): `BOX86_DUMP_BLOCKS`, `_DUMP_PC`, `_DUMP_PHYS`, `_DUMP_MIN_EXEC`, `_DUMP_PROFILE`, `_PERF_MAP`, `_HANG_DETECT`. See `DEBUG-BLOCK-DUMPER.md` and `scripts/analyze_blocks.py`.

## 6. Task Tracking (Taskwarrior)

All roadmap, optimization, testing and upstream-PR work is tracked with `task`. Run it with `BypassSandbox: true`. Task definitions can be reloaded via [`scratch/populate_taskwarrior_annotated.py`](file:///home/marm/src/86Box/scratch/populate_taskwarrior_annotated.py).

- Before starting, check `task list` / `task active`.
- `task start <id>` when coding, `task <id> done` once verified. `task <id> info` shows details and `task summary` shows progress.

| Project | Scope |
| :--- | :--- |
| `86box.phase2` | Native lowerings (MOV SS noint, IDIV, FDECSTP/FINCSTP, SHRD/SHLD) |
| `86box.phase4` | Block chaining, near CALL/RET linking, SMC |
| `86box.phase7` | Regression fixes, audit, cycle precision, backend fixes |
| `86box.phase8` | Host register pinning (RBX = cycles), inline dispatch stub, fast paths |
| `86box.perf` | Memory fast path, cold code separation, flatmode LEA elision, regalloc shift rearchitecture |
| `86box.proposals` | DSL, IR liveness/DSE, three-tier IR (pmode segment cache already landed) |
| `86box.testing` | Google Benchmark, CTest, deterministic telemetry |
| `86box.upstream` | Staged PRs for `86Box/86Box` |

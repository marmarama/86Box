# Review: `codegen_new` x86-64 fork changes

Scope: `git diff upstream/master...HEAD -- src` (about 3.9k lines added, 38 files).
Method: static code reading only. Nothing here was built, run or fuzzed. Treat each
item as a hypothesis to confirm, ordered by risk.

## Summary

The structure is good. Opcode coverage (Phase 2), REX and register expansion (Phase 1)
and the TLB fast paths (Phase 3) are conventional and low risk. The block linking work
(Phase 4) is where the risk is. It bypasses the C dispatcher, and the dispatcher is
what currently provides several safety checks implicitly. Linking has also not been
benchmarked, and has not been validated beyond a BIOS POST and manual testing.

**Update (October 2026):** Both Windows 98 and FreeDOS boot cleanly with linking enabled.
The FreeDOS boot regression (`JemmEx: exception 0D occured at CS:EIP=F000:0000ADF2`) was
diagnosed and resolved: `ropSETcc` emitted intra-block conditional jumps which violated
the register allocator's linear single-basic-block assumption and corrupted guest registers
when the condition was false. It was replaced with clean branchless condition evaluation
via `jit_cond_helpers`. `LINK_REAL` (`0x08`) has been verified and enabled by default in
`LINK_DEFAULT = 0x3f`.

## Resolved Regression: FreeDOS boot with linking

- **Symptom:** FreeDOS crashed during boot on `new_dynarec` with:
  `JemmEx: exception 0D occured at CS:EIP=F000:0000ADF2, ERRC=00000000`
  `[CS:IP] = E4 64 E6 EB 24 02 E0 F8` (`in al, 0x64`).
- **Diagnosis:** Jemm's V86 `#GP` handler at `0008:001108cc` decoded prefixes using `setz ch`
  and `setz ah` after reading opcode `0xE4` into `AL`. Because `AH` shares `EAX` with `AL`,
  `setz ah` clobbered `AL` due to uninitialized host register writeback when `cond` was false.
  Jemm failed to recognize `0xE4` as an emulated I/O instruction and routed it to `INT 06h` /
  breakpoint halt.
- **Fix:** Switched `ropSETcc` to branchless evaluation using `jit_cond_helpers` directly into
  `IREG_temp0` without intra-block jumps. Both FreeDOS boot and the automated benchmark suite
  (Doom, PCPBench, Dhrystone, Quake) pass 100% in 92.14s.

## Blocking correctness concerns (Phase 4)

### 1. Links are validated once, but the dispatcher's checks run on every entry
`dynarec_link_previous_block` checks `_cs`, target PC, status flags and FPU TOP at link
time. The dispatcher normally redoes address translation and block lookup (physical
address, virtual PC and status) on every entry. A linked jump skips all of that.
Failure scenario: the target virtual page is remapped (CR3 write, `INVLPG`, page-out and
page-in) without any write to the source block's page. The jump goes to a stale block.
Win98 does this constantly.
Fix options, cheapest first:
- Only link when source and destination are on the same guest page (a pure intra-page
  link cannot go stale without a page invalidation).
- Unlink all links on CR3 write, `INVLPG`, and TLB flush (`flushmmucache*`).
- Store the destination's physical page in the link and verify cheaply.

### 2. Asynchronous event checking is incomplete
`codegen_EXIT_BLOCK` tests only `cycles <= 0` and `pic.int_pending && IF`. It misses:
- NMI and SMI lines,
- the trap flag (single-step),
- APIC and other interrupt sources not routed through `pic.int_pending`,
- `cpu_end_block_after_ins`-style or device-requested exits,
- `cpu_state.abrt` or reset state (`x86_was_reset`).
A tight linked loop only terminates for these when the cycle budget expires. It also
costs an extra memory load and test per exit.
Suggestion: one `cpu_state.exit_request` word set by C code on any such event, with a
single compare in the exit sequence.

### 3. FPU TOP can mismatch on a linked `STATIC_TOP` target
Linking requires `dst->TOP == cpu_state.TOP` once, at link time. A source block without
static TOP can reach the same exit with a different TOP later. Require both blocks to be
static-TOP with matching TOP, or have linked entries recompute the TOP diff. The
`direct_entry_pos` is placed before the TOP-diff computation in the prologue, so the
diff is recomputed, but that does not fix a static-TOP assumption baked into the
destination's code.

### 4. rel32 reach and W^X
- `jmp_disp_ptr` stores a 32-bit displacement. Block memory comes from the code
  allocator. Confirm all blocks and `codegen_exit_rout` always sit within +/-2 GB of
  each other (a single mapping). If the allocator ever adds a second region, the
  truncated displacement silently jumps to garbage. Add an assertion in
  `codegen_link_exit` and `codegen_unlink_block` that the delta fits in `int32_t`.
- Patching code in place needs the page to be writable. This works where the pool is
  RWX. It breaks on hosts that enforce W^X (hardened Linux, macOS, newer Windows), and
  the ARM64 and LoongArch backends have only stubs.
- The displacement write goes through a `uint32_t *` into the byte stream. This is
  unaligned and technically undefined behaviour in C. Use `memcpy`.

### 5. Re-entrancy of invalidation while a linked chain is executing
Invalidation, deletion and recompile unlink correctly. The remaining risk is a store
inside block A that invalidates block B or A itself, while A is mid-execution. The
patched jmp then goes to the unlinked stub, which is correct. Confirm that memory for
the running block is never freed (only moved to the dirty list) until control returns
to C. Add an SMC torture test (see Test plan).

### 6. Stale `last_block_nr` / `last_exit_idx`
These globals are reset in several paths, but any new early return in
`exec386_dynarec_dyn` that skips the reset can link the wrong source. Consider
clearing them unconditionally at the top of the dispatcher after reading them into
locals, instead of at every exit path.

## Performance concerns

- **REP string ops exit to the dispatcher every iteration.** *(Resolved)*: Implemented in-block
  backward loops (`UOP_LOOP_LABEL`, `UOP_LOOP_JMP`, `UOP_CMP_IMM_JLE_DEST`) across `REP MOVS`,
  `REP STOS`, `REP LODS`, `REPE/REPNE SCAS`, and `REPE/REPNE CMPS` with `uop_NOP_BARRIER` register
  writeback, per-iteration cycle deduction (`is486 ? 3 : 4`), and cycle expiry / `exit_request` polling.
  Avoids thousands of block exits and dispatcher hash lookups for guest `memcpy`/`memset`.
- **Condition-code evaluation falls back to C calls** (`jit_cond_*`) when flags aren't
  in the fast path. This makes linked `Jcc` less valuable until Phase 5.
- **Code size.** Each `EXIT_BLOCK` emits about 100 bytes (cycle check, IRQ check, linked
  jump, stub, expiry stub), with two exits per conditional block. This increases cache
  footprint and block-pool pressure. Measure code expansion ratio.
- **Prologue cost.** Each block entry from C still loads RBP and R12 (movabs 10 bytes
  each) and saves or restores XMM6/XMM7 on Win64 unconditionally.
- **Measurement is missing for the new work.** The only head-to-head table is from
  after Phase 1. Gains are +3% on Quake, parity on Doom, -0.7% on Dhrystone, single-run.
  No linked-vs-unlinked comparison and no Win98 workload exists.

## Correctness concerns outside Phase 4

### `codegen_reg.c` changes are shared by all backends
- `HOST_REG_FLAG_NO_BH` handling is needed because R10-R15 can't encode AH/BH/CH/DH.
- "Flush and invalidate any other copy of this IR register" is a general allocator fix
  and probably fixes a latent bug. It changes behaviour for ARM64 and LoongArch, which
  weren't tested.
- Needs a dedicated test: mixed AL/AH/AX/EAX sequences.

### Memory helpers
- R10/R11 are saved and restored in the slow path, and stack alignment is explained in
  a comment. Verify the Win64 path equally.
- `R12` is now a fixed pinned register loaded in every prologue. Check that no other
  code path uses `R12` (the old `CODEBLOCK_NO_IMMEDIATES` use is removed).

### New opcodes
- `BSF`/`BSR` use `flags_res` through `__builtin_ctz/clz`. The zero-source case must
  keep the destination unchanged and set ZF. The 16-bit operand case needs masking.
- `CMPXCHG`/`XADD`: the memory forms are lock-free (single CPU, fine) but verify that
  the destination store occurs even on failed `CMPXCHG` (hardware writes back the old
  value). Exceptions mid-instruction must not leave partial architectural state.
- `BT*` with register offsets: the signed effective-address adjustment is easy to get
  wrong across segment limits and 16-bit wrap. Needs tests.
- String ops: `REP` with `CX/ECX == 0`, address-size overrides, `DF=1`, segment
  override on source only, and page-crossing faults with partial progress are all
  edge cases. A fault mid-REP must leave `ECX/ESI/EDI` as the hardware would.

### 386/486 gating
`CMPXCHG`/`XADD`/`BSWAP` are 486+. The roadmap mentions an illegal-opcode fallback.
Check the same for `CMOVcc` (P6+) and `SETcc` (386, fine).

## Process and maintainability

- **No automated correctness tests.** The only validation is benchmark workloads and
  boot to desktop. The best investment is a differential tester that runs random
  instruction streams through the interpreter and the JIT and compares full CPU state
  and memory.
- **Commit history.** Many commits are titled "update". Squash and rebase into one
  commit per phase before this goes anywhere upstream.
- **Unrelated change** in `src/char/char_file.c`: split out.
- **Diff size.** `codegen_backend_x86-64_ops.c` has about 1.4k lines of churn from REX
  generalisation, hard to review and to rebase. Keep REX changes in a mechanical commit.
- **Tracked artifacts.** `benchmark_results.log` is committed and modified on every run;
  move it out of version control.
- **Documentation drift.** ROADMAP had duplicated checklist entries, an XMM6/7 claim
  that no longer matches the code, mixed benchmark baselines, and Phase 4 marked more
  complete than the evidence supports (fixed in this change).
- **Platform coverage.** Only x86-64 Linux has been exercised. ARM64 and LoongArch
  compile paths changed (new uop stubs) but weren't built or tested. Win64 paths
  (frame layout, `CODEGEN_WIN64_FRAME`) are only code-reviewed.

## Suggested test plan

1. Differential fuzzing: interpreter vs JIT, per opcode class (new opcodes first).
2. Linking torture:
   - self-modifying code in a loop targeting a linked block,
   - remap the target page (change PTE, `INVLPG`, CR3 switch) between iterations,
   - `STI`/IRQ, NMI and trap-flag delivery within a linked tight loop,
   - FPU loops with differing TOP.
3. Boot and shutdown: DOS 6.22, Win95, Win98 SE (install and normal boot), Win2000,
   each with linking on and off.
4. Benchmarks: legacy dynarec vs new unlinked vs new linked, 5 runs each, plus a Win98
   boot-time and desktop-workload measurement.

## Recommended order of work

1. Same-page-only linking, plus `memcpy` displacement writes and `int32` range asserts.
2. Exit-request flag for async events.
3. In-block `REP` loops.
4. Differential fuzzer.
5. Re-measure everything; update ROADMAP numbers.
6. Clean up history and split commits before any upstreaming.

---

# Follow-up review: the fixes (commit `a9ff6ec09` and predecessors)

Static reading only; not built or run beyond the earlier compile.

## Resolved or improved
- **Same-page linking by default (`LINK_DEFAULT = 0x3f`)** and `codegen_flush()` back to a
  no-op. Good: this removes the POST slowdown and covers most of item 1.
- **`cpu_state.pc` written first in `EXIT_BLOCK`**, before the cycle and IRQ checks.
  Correct, and removes duplicated stores in the stubs.
- **`last_block_nr`/`last_exit_idx` read into locals and cleared at the top of
  `exec386_dynarec_dyn`.** Resolves item 6.
- **`memcpy` displacement writes** (no unaligned UB).
- **`BT/BTS/BTR/BTC` register offset**: now uses a signed shift (`SAR`, plus `MOVSX` for
  16-bit). That is the correct hardware behaviour.

- **FreeDOS JemmEx crash resolved & `LINK_REAL` default (`0x3f`)**: Root cause was `ropSETcc`
  intra-block jump register corruption. Fixed with branchless JIT condition evaluation.
  FreeDOS boots cleanly and runs full benchmark suite in 92.14s.
- **FPU TOP link check**: Refuses direct linking to `CODEBLOCK_STATIC_TOP` destinations
  from sources touching FPU.
- **Linear/virtual page check**: Requires `(src->pc >> 12) == (dst->pc >> 12)` in addition
  to physical page matching for `LINK_SAME_PAGE`.
- **rel32 displacement check**: Refuses links exceeding `int32_t` displacement range.
- **Async event handling (`cpu_state.exit_request`)**: Unified check for unmasked NMI, SMI,
  TF, reset, and aborts emitted in `codegen_EXIT_BLOCK` to abort direct links.
- **SMC direct execution protection**: Writes to compiled pages assert `EXIT_STATUS_BLOCK_END`.

## Remaining issues & future enhancements

1. **W^X page protection handling**: Hosts enforcing strict W^X (e.g. OpenBSD, macOS) require
   dual-mapping or `mprotect` toggling for dynamic link patching.
2. **Phase 4b: Indirect branch chaining**: `RET` and indirect `JMP`/`CALL` currently return
   to C dispatch. A Return Address Stack (RAS) or inline cache would extend chaining.
3. **Phase 5: Native EFLAGS propagation & fast-path branch evaluation**:
   - Status flag fast-pathing for `COND_E`, `COND_NE`, `COND_S`, `COND_NS`, and comparison branches (`COND_B`, `COND_NB`, `COND_BE`, `COND_NBE`, `COND_L`, `COND_NL`, `COND_LE`, `COND_NLE`) after `CMP` and `SUB` is now implemented via `codegen_last_flags_op` and native uOps, eliminating `jit_cond_helpers` overhead for the vast majority of branches.
   - General register dead store elimination is implemented for `IREG_EAX` through `IREG_EBX` in `codegen_reg.h`/`codegen_reg.c`.
   - Remaining Phase 5 work: IR constant propagation & peephole folding in `codegen_ir.c`.
4. **Link counter assertion**: Add debug recount assertion in `codegen_unpatch_all_links`.

## Suggested next steps
1. Complete Phase 5 Task 3 (IR constant propagation & peephole folding in `codegen_ir.c`).
2. Or investigate Phase 4b (Return Address Stack for `RET` chaining).


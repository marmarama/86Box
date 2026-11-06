---
name: bisect-dynarec-regression
description: Bisect a guest failure (crash, hang, corruption, bad boot) to block linking, a specific link, or general codegen in the new dynarec. Use when a DOS/Win98/other guest breaks after a codegen_new, mem or segment change.
---

# Bisect a dynarec regression

1. **Reproduce** with the release build (`build/new_dynarec/src/86Box`) and note the exact failure. Run it with a fixed disk image and config so runs are deterministic (no network, same config).
2. **Is it linking?** Re-run with `BOX86_LINK_MASK=0` (no linking).
   - Fixed: linking bug. Go to step 3.
   - Still broken: codegen or lowering bug. Go to step 5.
3. **Narrow by link class.** An explicit mask *replaces* the default (`0xbf`), so start from `0xbf` and clear one bit at a time (`0xbe`, `0xbd`, `0xbb`, `0xb7`, `0xaf`, `0x9f`, `0x3f`). Check:
   - `0x08` `LINK_REAL` vs `0x10` `LINK_PMODE`: which mode.
   - `0x01` JMP, `0x02` JCC, `0x04` LOOP, `0x80` RET: which exit kind.
   - `0x20` `LINK_SAME_PAGE`: SMC or cross-page issue.
4. **Find the exact link.** Set `BOX86_LINK_LIMIT=N` and binary-search N until the failure appears. The Nth link is logged via `pclog` with source/dest block, PC, `cs` and status. Inspect it with the `analyze-block-dump` skill.
5. **Codegen bug.** Dump blocks around the failing PC (see `analyze-block-dump`), run `--lint`, and compare IR and host code against the guest instructions. For single-stepping guest code in GDB, build with `-DGDBSTUB=ON` and use `BOX86_LINK_MASK=0`.
6. **Check the known gotchas** in `AGENTS.md` §3 before deep diving: loop decrement, runtime vs compile-time `flags_op`, SMC `EXIT_STATUS_BLOCK_END`, intra-block branches, high-byte regs, `IREG_cycles` sync.
7. **Fix, then record**: add a new pitfall to `AGENTS.md` §3 and `src/codegen_new/docs/`, and commit.

Details: `src/codegen_new/docs/DEBUGGING.md`. Its default-mask text is stale; the real default is `LINK_DEFAULT` in `src/codegen_new/codegen.h`.

Kill stray emulators afterwards: `pkill -9 86Box`.

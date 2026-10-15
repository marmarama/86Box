# Debugging block linking (`codegen_new`)

Block linking (Phase 4, see [ROADMAP.md](ROADMAP.md)) can be restricted at runtime
without rebuilding. This is meant for bisecting guest failures, for example the
FreeDOS boot regression described in [REVIEW.md](REVIEW.md).

## How the policy works

The policy is checked when a link is created, in `codegen_link_exit`
(`codegen_block.c`). Blocks always emit their exit stubs. When a link is refused, the
exit stays pointed at its unlinked stub and returns to the C dispatcher, exactly as
before linking existed. So the policy costs nothing on the hot path.

Changing the policy only affects links created afterwards, unless you use
`codegen_link_set_policy()` (below), which also unpatches existing links.

## Environment variables

Read once, on the first link attempt. Both are optional.

### `BOX86_LINK_MASK`

A bitmask (decimal or `0x` hex). Default is `0x3f` (all permissions plus
`LINK_SAME_PAGE`). `0` disables linking entirely. **An explicit mask replaces the
default, so the masks in the examples below (`0x19`, `0x1a`, ...) do not include
`LINK_SAME_PAGE`; add `0x20` to them to bisect with the default safety policy.**

| Bit    | Name             | Meaning                                                                 |
| :----- | :--------------- | :---------------------------------------------------------------------- |
| `0x01` | `LINK_JMP`       | direct `JMP rel8/16/32`                                                 |
| `0x02` | `LINK_JCC`       | `Jcc rel`                                                               |
| `0x04` | `LINK_LOOP`      | `JCXZ`, `LOOP`, `LOOPE`, `LOOPNE`                                       |
| `0x08` | `LINK_REAL`      | link blocks compiled in real mode or V86                                |
| `0x10` | `LINK_PMODE`     | link blocks compiled in protected mode                                  |
| `0x20` | `LINK_SAME_PAGE` | restriction: only link when source and target share guest physical and linear pages |
| `0x40` | `LINK_NO_FPU`    | restriction: never link a block that uses the FPU (source or target)    |

The first five bits are permissions (all must allow the link). Bit `0x20` (`LINK_SAME_PAGE`)
is a safety restriction enabled by default in `0x3f`. `0x40` (`LINK_NO_FPU`) is off by default.
A link needs its exit kind bit **and** its mode bit, so `0x08` alone links nothing.

### `BOX86_LINK_LIMIT`

Create only the first N links (`0` or unset means unlimited). When the Nth link is
created it is logged (via `pclog`) with the source and destination block pointers and
PC, the source `cs`, and the block status. Further links are refused.

The count includes only links that pass the mask, and counts every link created, so
the result is deterministic only if the guest run is (same disk image, no
network, same config).

## Runtime API

```c
void codegen_link_set_policy(uint32_t mask, uint32_t limit);
```

Sets the mask and limit, resets the link counter, and unpatches every existing link
back to its stub so the new policy takes effect. Stubs are kept, so exits can relink
under the new policy. This differs from `codegen_unlink_block`, which is for dead
blocks and clears the stubs. Call it from the CPU thread, outside a running block.

It is not yet wired to the config file or the Qt settings UI.

## Example runs

```sh
# Linking off (stubs only). Should match the pre-linking behaviour.
BOX86_LINK_MASK=0 ./build/new_dynarec/src/86Box ...

# Only direct JMPs, real mode only
BOX86_LINK_MASK=0x09 ./build/new_dynarec/src/86Box ...

# Everything, but same-page links only
BOX86_LINK_MASK=0x3f ./build/new_dynarec/src/86Box ...

# Stop linking after the 5000th link and log it
BOX86_LINK_LIMIT=5000 ./build/new_dynarec/src/86Box ...
```

Enable `pclog` output (debug build or logging enabled for the emulator) to see the
"Block linking policy" line at startup and the "last allowed" link line.

## Bisecting a guest failure (FreeDOS example)

1. **Sanity check the stub path:** `BOX86_LINK_MASK=0`. If this fails, the problem is
   not in linking itself but in the new `EXIT_BLOCK` stubs, exit sequence or register
   writeback.
2. **Which exit kind:** try `0x19` (JMP, real and protected), `0x1a` (Jcc),
   `0x1c` (LOOP/JCXZ). The one that fails is the culprit.
3. **Which mode:** compare `0x0f` (all kinds, real mode and V86 only) with `0x17` (all
   kinds, protected mode only). To narrow further, combine with the kind from step 2
   (e.g. `0x09` is JMP in real mode only).
4. **Which link:** with the failing mask, binary-search `BOX86_LINK_LIMIT`. Find the
   smallest N that fails. The log line for link N identifies the source and target
   blocks (PC, `cs`, status). Check that line for 16-bit wrap, an unexpected `cs`, or
   a target in a page being modified.
5. **Page-related causes:** add `0x20` to the failing mask. If the failure goes away,
   suspect stale links or self-modifying code (REVIEW.md item 1 and hypothesis 5).
6. **FPU-related causes:** add `0x40`. If the failure goes away, suspect TOP handling
   (REVIEW.md item 3).

## Tips

- Keep one fixed config (same disk image, same machine, no random inputs) so the link
  count is reproducible between runs.
- To compare behaviour against the interpreter at the first diverging block, run the
  interpreter build side by side and diff the register state after N blocks.
- The benchmark harness (`benchmarks/run_benchmarks.py`) can be used for this as long as
  the environment variables are inherited by the 86Box process.
- To inspect live guest state (BIOS, DOS drivers, kernels), build with `-DGDBSTUB=ON` and connect with `target remote 127.0.0.1:12345` (set `BOX86_LINK_MASK=0` when single-stepping so control returns to the C dispatcher on every block).

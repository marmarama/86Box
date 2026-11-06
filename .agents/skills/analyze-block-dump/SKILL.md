---
name: analyze-block-dump
description: Dump compiled dynarec blocks from a running guest and analyze them (hotspots, lint, single block inspection, hang detection, perf attribution). Use for codegen quality, correctness or performance investigations.
---

# Dump and analyze dynarec blocks

## Capture
Set env vars (read in `src/codegen_new/codegen_block.c`) when launching 86Box:

| Variable | Purpose |
| :--- | :--- |
| `BOX86_DUMP_BLOCKS=<file>.86bdump` | Write the binary block dump |
| `BOX86_DUMP_PC`, `BOX86_DUMP_PHYS` | Restrict to a guest PC or physical address |
| `BOX86_DUMP_MIN_EXEC` | Skip blocks executed fewer than N times |
| `BOX86_DUMP_PROFILE` | Add profiling data |
| `BOX86_HANG_DETECT` | Flight recorder for hangs |
| `BOX86_PERF_MAP=1` | Write `/tmp/perf-<pid>.map` so `perf` attributes samples to guest blocks |

Write dumps under `scratch/` (never commit them).

## Analyze
```bash
python3 scripts/analyze_blocks.py scratch/trace.86bdump --summary --top 25
python3 scripts/analyze_blocks.py scratch/trace.86bdump --inspect-block 0x<PC or block_nr>
python3 scripts/analyze_blocks.py scratch/trace.86bdump --lint
python3 scripts/analyze_blocks.py scratch/trace.86bdump --hang
python3 scripts/analyze_blocks.py scratch/trace.86bdump --rt
```
- `--summary`: hotspots (exec count × host bytes) and instruction mix.
- `--inspect-block`: guest x86 ↔ IR uops ↔ host asm.
- `--lint`: redundant spills, dead PC/flag writes, interpreter fallbacks.
- `--hang`: root-cause analysis of a stuck guest.
- `--rt`: per-second speed telemetry.

## Profile with perf
Run with `BOX86_PERF_MAP=1` under `perf record`, then `perf report`/`perf annotate`. Don't commit `perf.data`.

Design details: `src/codegen_new/docs/DEBUG-BLOCK-DUMPER.md`.

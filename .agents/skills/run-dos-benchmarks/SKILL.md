---
name: run-dos-benchmarks
description: Build and run the headless DOS benchmark suite (Doom, PCPBench, Dhrystone, Quake) for the new dynarec and compare against benchmark_results.log. Use before declaring dynarec, memory or segment work done, or to check performance.
---

# Run the DOS benchmarks

1. **Build** the release config (benchmarks must use it):
   ```bash
   ninja -C build/new_dynarec -j$(nproc)
   ```
2. **Run** (headless, reads COM1 telemetry):
   ```bash
   python3 benchmarks/run_benchmarks.py --only new \
       --vm-dir /home/marm/Emulation/86Box/benchmark \
       --roms /home/marm/Emulation/86Box/roms \
       --new-emu build/new_dynarec/src/86Box --timeout 90
   ```
   Useful extras: `--output-json <file>`, `--perf`, `--per-test-timeout N`, `--screenshot-dir <dir>`, `--break-before/--break-after/--stop-after` (see `--help`). Don't pass `--no-headless` unless you need the GUI.
3. **Compare** with `benchmark_results.log` in the repo root. Flag:
   - Regressions in any score.
   - Suspicious jumps (e.g. PCPBench 60 → 208 FPS usually means `IREG_cycles` was overwritten around a C callout; see `AGENTS.md` Gotcha 6).
4. If a test hangs or crashes, use the `bisect-dynarec-regression` skill.
5. **Clean up**: `pkill -9 86Box`. Never leave orphan emulators.

Don't overwrite `benchmark_results.log` unless the user asks to update the baseline.

Details: `benchmarks/README.md`, `src/codegen_new/docs/BENCHMARKING.md`.

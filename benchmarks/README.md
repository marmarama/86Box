# Automated Full-System Dynarec Benchmarks

This directory contains the automated test harness, guest scripts, and configuration templates to run headless, unattended DOS benchmarks and compare the performance of the **old dynarec** (`codegen`) against the **new dynarec** (`codegen_new`).

For complete architecture and design rationale, see [`src/codegen_new/BENCHMARKING.md`](../src/codegen_new/BENCHMARKING.md).

---

## Files in this Directory

| File | Purpose |
|---|---|
| `run_benchmarks.py` | Host runner script that executes 86Box headlessly, captures real-time COM1 (serial) telemetry, and generates comparison tables. |
| `AUTOEXEC.BAT` | Guest DOS startup script that runs benchmarks (`Quake`, `Doom`, `PCPBench`, `Dhrystone`), streams output to `COM1`, and triggers APM shutdown. |
| `POWEROFF.COM` | 35-byte DOS utility that uses APM 1.2 (`INT 15h, AX=5307h`) to power off the virtual machine cleanly when the benchmark finishes. |
| `poweroff.asm` | NASM assembly source for `POWEROFF.COM`. |
| `86box.cfg.template` | Sample 86Box configuration for an Intel 440BX + Pentium II 400 MHz machine with COM1 file redirection enabled. |

---

## Quick Usage

### 1. Build Both Dynarec Variants
```bash
# Old dynarec (Release)
cmake --preset regular
cmake --build --preset regular

# New dynarec (Dev Debug or Regular)
cmake --preset new_dynarec_dev_debug
cmake --build --preset new_dynarec_dev_debug
```

### 2. Inject Guest Scripts into a DOS Disk Image (Optional)
If you have `mtools` installed:
```bash
python3 benchmarks/run_benchmarks.py --disk-image /path/to/dos_bench.img
```
This automatically copies `AUTOEXEC.BAT` and `POWEROFF.COM` into the root of the disk image.

### 3. Run the Comparison
```bash
python3 benchmarks/run_benchmarks.py \
    --old-emu build/regular/src/86Box \
    --new-emu build/new_dynarec_dev_debug/src/86Box \
    --vm-dir /path/to/benchmark_vm \
    --perf
```

The script runs both builds offscreen (`QT_QPA_PLATFORM=offscreen ALSOFT_DRIVERS=null`), watches for `=== BENCHMARK SUITE COMPLETE ===`, and prints a side-by-side comparison table of guest FPS, host wall-clock time, and hardware performance counters.

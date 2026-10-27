# Declarative x86 Floating-Point Units & Coprocessors Specification

This directory defines declarative hardware models for x86 Floating-Point Units (FPUs) and companion math coprocessors spanning the discrete era (1980–1994) to high-performance integrated superpipelined and superscalar units.

---

## 1. Coprocessor Taxonomy

| Category | Example Hardware | Interface Protocol | Register Organization | Math Standard |
| :--- | :--- | :--- | :--- | :--- |
| **Bus Snooping x87** | Intel 8087 | QS0/QS1 Queue Status Pins | 8x 80-bit Stack (ST0..ST7) | IEEE 754 Draft (Projective $\infty$) |
| **I/O Mapped x87** | Intel 80287, 80287XL | I/O Ports `0x00F8`-`0x00FF` | 8x 80-bit Stack (ST0..ST7) | Projective (287) / Affine (287XL) |
| **Dedicated Coprocessor Bus** | Intel 80387DX/SX, Cyrix 83D87, IIT 3C87 | Synchronous 32-bit / 16-bit Coprocessor Bus | 8x 80-bit Stack (IIT: 4 banks $\times$ 8 = 32) | IEEE 754-1985 (Affine $\infty$) |
| **Memory-Mapped RISC (Non-x87)** | Weitek Abacus 1167 / 3167 / 4167 | 64 KB MMIO Physical Window (`0xC0000000`) | 32 Flat 32-bit (`WS0`..`WS31`) / 16 64-bit (`WD0`..`WD15`) | IEEE 754 Single & Double (No 80-bit) |
| **Integrated On-Die FPU** | Intel 486DX, Pentium, P6, AMD K7 | Internal Micro-op / Superpipeline / OOO Bus | 8x 80-bit Stack (renamed into physical pools) | IEEE 754-1985 + FCMOV / FCOMI |

---

## 2. Hardware Interface Protocols

### A. Intel 8087 (Bus Snooping)
- Directly taps the multiplexed AD0..AD15 bus and status pins (S0..S2).
- Tracks CPU instruction queue decoding in lockstep using **QS0** and **QS1** pins.
- Signals completion / synchronizes using the host CPU's `TEST#` pin (monitored by the `WAIT` / `FWAIT` instruction).

### B. Intel 80287 (I/O Port Decoding)
- Host 80286 communicates with the 80287 via dedicated internal I/O addresses:
  - `0x00F8`: Coprocessor Status / Control Write
  - `0x00FA`: Coprocessor Data Read / Write
  - `0x00FC`: Coprocessor Opcode Transfer
- Runs on a dedicated clock divided by 3 (producing 2/3 CPU clock rate), eliminating the strict synchronous lockstep of the 8087.
- Exception signaling triggers IRQ13 routed through master PIC.

### C. Intel 80387 & 3rd Party Coprocessors (Dedicated Bus)
- Full 32-bit synchronous coprocessor bus operating at 1:1 CPU frequency.
- Hardware support for **Affine Infinity** ($+\infty \ne -\infty$).
- Hardware transcendental primitives (`FSIN`, `FCOS`, `FSINCOS`) with expanded angle ranges.
- **IIT 3C87**: Added 4 register banks (Bank 0-3 switched via `FSBP0`..`FSBP3`) and a 4x4 matrix vector transform acceleration instruction (`F4X4`).
- **Cyrix FasMath 83D87**: Clean-room parallel multiplier/divider array providing 4x-8x speedups over Intel 387.

### D. Weitek Abacus 1167 / 3167 / 4167 (MMIO Architecture)
- **Completely distinct from x87**: Does not intercept ESC (`0xD8`-`0xDF`) opcodes and does not use a register stack.
- Mapped into a **64 KB physical memory window** (typically `0xC0000000` or `0x70000000`).
- CPU executes floating point by reading and writing to addresses within this window:
  - Memory Write (`MOV [addr], reg`): initiates operation or stores source data.
  - Memory Read (`MOV reg, [addr]`): loads result back to integer register.
- Address bits encode opcode, destination, source 1, and source 2:
  - Offset `[15:10]`: Function Code (FADD, FSUB, FMUL, FDIV, FSQRT, etc.)
  - Offset `[9:7]`: Destination register & precision mode
  - Offset `[6:2]`: Source register 1
  - Offset `[1:0]`: Source register 2
- Register file: 32 flat 32-bit single-precision registers (`WS0`..`WS31`), addressable as 16 double-precision 64-bit registers (`WD0`..`WD15`).
- Coexists alongside internal 486DX x87 FPU without conflict!

### E. Integrated FPUs (486DX, Pentium, P6, K7)
- **Intel 486DX**: Eliminates bus overhead; single-cycle zero-wait cache and register operand fetch.
- **Intel Pentium**: 8-stage superpipeline; 1-cycle throughput for single/double FADD and FMUL. Introduces **FXCH pairing in the V-pipe** (0 effective cycles for stack swaps).
- **Intel P6 (Pentium Pro / II / III)**: Out-of-order execution engine. FXCH is handled entirely in the **Register Alias Table (RAT)** with 0 latency and 0 execution uops. Direct branchless comparisons (`FCOMI`, `FUCOMI`) update EFLAGS directly.
- **AMD K7 (Athlon)**: Triple-pipeline out-of-order engine. Features independent, concurrent FADD, FMUL, and FSTORE pipelines capable of issuing and retiring 1 FADD, 1 FMUL, and 1 FSTORE in every single cycle.

---

## 3. Directory Structure

```
fpus/
├── README.md                     # This master architectural documentation
├── discrete/                     # Discrete / Socketed Math Coprocessors
│   ├── intel_8087.yaml           # First x87 coprocessor, bus snooping, projective infinity
│   ├── intel_80287.yaml          # I/O port mapped 287 (ports 0xF8-0xFC)
│   ├── intel_80287xl.yaml        # 387SX core in 287 pinout, affine infinity
│   ├── intel_80387dx.yaml        # 32-bit synchronous coprocessor bus, FSIN/FCOS
│   ├── intel_80387sx.yaml        # 16-bit coprocessor bus for 386SX
│   ├── intel_80487sx.yaml        # 169-pin replacement chip disabling host 486SX
│   ├── cyrix_83d87.yaml          # 4x faster arithmetic array coprocessor
│   ├── iit_3c87.yaml             # 4 banks of 8 registers (32 total), matrix transform
│   ├── nexgen_nx587.yaml         # NexBus companion coprocessor for Nx586
│   ├── weitek_3167.yaml          # 64KB MMIO window, 32 flat registers WS0..WS31
│   └── weitek_4167.yaml          # 142-pin MMIO coprocessor for 486, coexists with x87
└── integrated/                   # Built-in / On-Die FPUs
    ├── intel_486dx.yaml          # Integrated zero-wait on-die FPU
    ├── intel_pentium.yaml        # 8-stage pipelined FPU, FXCH V-pipe pairing
    ├── intel_p6.yaml             # Out-of-order execution, zero-cycle FXCH renaming
    └── amd_k7.yaml               # Triple-pipeline FPU (simultaneous FADD + FMUL + FSTORE)
```


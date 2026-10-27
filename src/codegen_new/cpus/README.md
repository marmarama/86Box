# Declarative CPU Architecture & Specification Framework (`codegen_new/cpus`)

This directory is the single source of truth for all CPU emulation in 86Box, spanning the entire PC era from the original 1981 IBM PC (Intel 8088 4.77 MHz) through the end of the 32-bit era (Pentium 4 Northwood 3.4 GHz, Athlon XP Barton, VIA C3):

1. **Instruction Set Architectures ([`isa/`](file:///home/marm/src/86Box/src/codegen_new/cpus/isa/))**: Declarative semantics, opcodes, lazy flags, and exception rules for x86 instructions from the 8086 through SSE, SSE2, and Enhanced 3DNow!.
2. **Microarchitecture Timings ([`timings/`](file:///home/marm/src/86Box/src/codegen_new/cpus/timings/))**: Hardware execution profiles (microcode cycles, bus penalties, scalar pipelines, superscalar U/V pairing, out-of-order $\mu$Op dispatch, RISC86).
3. **Declarative CPU Manifests (`intel/`, `amd/`, `nec/`, `via/`, `cyrix/`)**: Self-contained manifests defining each chip's ISA level, feature flags, CPUID signatures, and cache configuration.

---

## 1. Directory Structure

```
src/codegen_new/cpus/
├── README.md               # Master architecture documentation
├── ROADMAP.md              # Modernization and code-generation implementation roadmap
│
├── isa/                    # Abstract Instruction Set Architecture (ISA) specifications
│   ├── types_and_operands.yaml
│   ├── arithmetic.yaml
│   ├── logic_and_bit.yaml
│   ├── mov_and_transfer.yaml
│   ├── shift_rotate.yaml
│   ├── branch_and_call.yaml
│   ├── stack_and_frame.yaml
│   ├── string_and_io.yaml
│   ├── system_and_misc.yaml
│   ├── simd_mmx.yaml       # MMX integer SIMD
│   ├── simd_sse.yaml       # SSE single-precision float SIMD (Pentium III / Athlon XP)
│   ├── simd_sse2.yaml      # SSE2 double-precision float & 128-bit integer SIMD (Pentium 4)
│   ├── amd_3dnow.yaml      # AMD 3DNow! & Enhanced 3DNow! (K6-2 / Athlon)
│   └── sysenter.yaml       # Fast system calls (SYSENTER, SYSEXIT, FXSAVE, FXRSTOR)
│
├── timings/                # Microarchitecture pipeline and cycle profiles
│   ├── classes.yaml        # Abstract execution categories
│   ├── 8086.yaml           # Intel 8086 / 8088 unpipelined microcode, EA calculation, bus width
│   ├── 186.yaml            # Intel 80186 / 80188 hardware EA, accelerated multiply, 186 instructions
│   ├── 286.yaml            # Intel 80286 pipelined bus, hardware EA, 16-bit protected mode
│   ├── 386.yaml            # Intel 80386 unpipelined microcode, bus width (DX 32 vs SX 16)
│   ├── 486.yaml            # 5-stage scalar in-order pipeline & AGI stalls
│   ├── pentium.yaml        # Pentium P5/P54C/P55C superscalar U/V pipe pairing
│   ├── cyrix_686.yaml      # Cyrix 6x86 X/Y pairing with speculative data forwarding
│   ├── k6.yaml             # AMD K6 / K6-2 decoupled RISC86 dual-ALU engine
│   ├── k7.yaml             # AMD K7 (Athlon / Athlon XP) 9-pipe symmetric execution
│   ├── p6.yaml             # Intel P6 (Pentium Pro/II/III) 4-1-1 decoders, ROB, Ports 0-4
│   └── pentium4.yaml       # Intel Pentium 4 (NetBurst) 20-stage pipeline, Trace Cache, 2x ALUs
│
├── registers/              # Declarative Architectural Register Specifications
│   ├── README.md           # Register hierarchy and C code generation guide
│   ├── gpr.yaml            # General Purpose Registers (EAX..EDI, AX..DI, AL..BH)
│   ├── segments.yaml       # Segment Registers (ES, CS, SS, DS, FS, GS) & Descriptor Caches
│   ├── flags.yaml          # FLAGS (16-bit) and EFLAGS (32-bit) Architectural Bitfields
│   ├── control_and_system.yaml # Control Registers (CR0..CR4), Debug (DR0..DR7), Tables (GDTR..TR)
│   ├── fpu_and_mmx.yaml    # x87 FPU Stack (ST0..ST7), Status/Control Words, MMX Aliasing
│   ├── simd_xmm.yaml       # SSE / SSE2 128-bit Vector Registers (XMM0..XMM7) & MXCSR
│   └── msr.yaml            # Model-Specific Registers (TSC, APIC, SYSENTER, MTRR, AMD STAR, VIA FCR)
│
├── intel/                  # Intel CPU Manifests
│   ├── 8088.yaml           # Intel 8088 4.77 MHz (IBM PC 5150 / PC-XT 5160)
│   ├── 8086.yaml           # Intel 8086 4.77-10 MHz (Compaq Deskpro)
│   ├── 80186.yaml          # Intel 80186 8-16 MHz (16-bit bus, integrated peripherals)
│   ├── 80188.yaml          # Intel 80188 8-16 MHz (8-bit bus)
│   ├── 80286.yaml          # Intel 80286 6-25 MHz (IBM PC/AT 5170)
│   ├── 386dx.yaml          # Intel 80386DX 16-33 MHz (Compaq Deskpro 386)
│   ├── 386sx.yaml          # Intel 80386SX 16-25 MHz
│   ├── rapidcad.yaml       # Intel RapidCAD (486DX in 132-pin 386 socket, on-die 487)
│   ├── 486dx.yaml          # Intel 80486DX 25-50 MHz
│   ├── 486dx2.yaml         # Intel 80486DX2 50-66 MHz (Clock-doubled)
│   ├── pentium_p54c.yaml   # Intel Pentium Classic 75-200 MHz (Socket 5/7)
│   ├── pentium_mmx.yaml    # Intel Pentium MMX (P55C) 166-233 MHz (Socket 7)
│   ├── pentium_pro.yaml    # Intel Pentium Pro (P6 debut, Socket 8, full-speed L2 MCM, NO MMX)
│   ├── pentium2_klamath.yaml # Pentium II 233-300 MHz (Slot 1, 0.35 um, NO SEP/SYSENTER)
│   ├── pentium2_deschutes.yaml # Pentium II 266-450 MHz (Slot 1, 0.25 um, SYSENTER, FXSR)
│   ├── celeron_mendocino.yaml # Celeron 300-533 MHz (First on-die full-speed 128KB L2)
│   ├── pentium3_katmai.yaml # Pentium III 450-600 MHz (Slot 1, SSE debut, off-die L2)
│   ├── pentium3_coppermine.yaml # Pentium III 500-1133 MHz (Socket 370 / Slot 1, on-die 256KB ATC)
│   ├── pentium3_tualatin.yaml # Pentium III-S 1.0-1.4 GHz (Socket 370, 0.13 um, Hardware Prefetcher)
│   ├── pentium4_northwood.yaml # Pentium 4 1.6-3.4 GHz (Socket 478)
│   └── pentium_m_dothan.yaml # Pentium M 90nm 1.0-2.26 GHz (Socket 479)
│
├── amd/                    # AMD CPU Manifests
│   ├── am386dx.yaml        # AMD Am386DX-40 (40 MHz)
│   ├── am486dx4.yaml       # AMD Am486DX4-100 (100-120 MHz)
│   ├── am5x86.yaml         # AMD Am5x86-P75 (133-160 MHz)
│   ├── k6.yaml             # AMD K6 (Model 6/7 Little Foot, MMX only, NO 3DNow!)
│   ├── k6_2.yaml           # AMD K6-2 (Model 8 Chomper / K6-3D debut, 3DNow! + SYSCALL)
│   ├── k6_3.yaml           # AMD K6-III (Model 9 Sharptooth, on-die 256KB Tri-Level cache)
│   ├── k6_2plus.yaml       # AMD K6-2+ / K6-III+ (Model 13, 0.18 um, Enhanced 3DNow!)
│   ├── k7_thunderbird.yaml # AMD Athlon 650-1400 MHz (Socket A / Slot A)
│   ├── k7_barton.yaml      # AMD Athlon XP Barton (Socket A, 512KB L2)
│   └── geode_lx800.yaml    # AMD Geode LX 800 (500 MHz, 3.3W TDP, OLPC XO-1)
│
├── ibm/                    # IBM Microelectronics Manifests
│   ├── 386slc.yaml         # IBM 386SLC (386 with 8KB L1 cache)
│   └── blue_lightning.yaml # IBM 486BL Blue Lightning (Clock-tripled 100 MHz, 16KB L1)
│
├── nexgen/                 # NexGen Manifests
│   └── nx586.yaml          # NexGen Nx586 (First x86-to-RISC86 engine, proprietary NexBus)
│
├── rise/                   # Rise Technology Manifests
│   └── mp6.yaml            # Rise mP6 (3-issue superscalar, low-power Socket 7)
│
├── transmeta/              # Transmeta Manifests
│   ├── crusoe_tm5800.yaml  # Transmeta Crusoe TM5800 (Code Morphing VLIW, Sony VAIO PictureBook)
│   └── efficeon_tm8600.yaml # Transmeta Efficeon TM8600 (256-bit VLIW Engine, CMS 3.0)
│
├── dmp/                    # DM&P Electronics Manifests
│   └── vortex86dx.yaml     # DM&P Vortex86DX (Embedded System-on-Chip)
│
├── nec/                    # NEC CPU Manifests
│   ├── v20.yaml            # NEC V20 (uPD70108, 8088 8-bit bus drop-in + 186 instructions)
│   └── v30.yaml            # NEC V30 (uPD70116, 8086 16-bit bus drop-in + 186 instructions)
│
├── via/                    # VIA / Centaur CPU Manifests
│   └── c3_nehemiah.yaml    # VIA C3 800-1400 MHz (Socket 370)
│
└── cyrix/                  # Cyrix CPU Manifests
    ├── 5x86.yaml           # Cyrix 5x86 M1sc (6x86 core in Socket 3 486 with branch prediction)
    ├── mediagx.yaml        # Cyrix MediaGX (Virtual System Architecture / VSA PC-on-a-chip)
    └── 6x86mx.yaml         # Cyrix 6x86MX / MII (Socket 7)
```

---

## 2. Complete Chronological Coverage

| Year | Vendor | Model Name | Architecture Tier | Timing Profile |
| :--- | :--- | :--- | :--- | :--- |
| **1978** | Intel | 8086 | `CPU_8086` | [`timings/8086.yaml`](file:///home/marm/src/86Box/src/codegen_new/cpus/timings/8086.yaml) |
| **1979** | Intel | 8088 | `CPU_8086` | [`timings/8086.yaml`](file:///home/marm/src/86Box/src/codegen_new/cpus/timings/8086.yaml) |
| **1982** | Intel | 80186 / 80188 | `CPU_186` | [`timings/186.yaml`](file:///home/marm/src/86Box/src/codegen_new/cpus/timings/186.yaml) |
| **1982** | Intel | 80286 | `CPU_286` | [`timings/286.yaml`](file:///home/marm/src/86Box/src/codegen_new/cpus/timings/286.yaml) |
| **1984** | NEC | V20 / V30 | `CPU_186` | [`timings/8086.yaml`](file:///home/marm/src/86Box/src/codegen_new/cpus/timings/8086.yaml) |
| **1985** | Intel | 80386DX | `CPU_386` | [`timings/386.yaml`](file:///home/marm/src/86Box/src/codegen_new/cpus/timings/386.yaml) |
| **1988** | Intel | 80386SX | `CPU_386` | [`timings/386.yaml`](file:///home/marm/src/86Box/src/codegen_new/cpus/timings/386.yaml) |
| **1989** | Intel | 80486DX | `CPU_486` | [`timings/486.yaml`](file:///home/marm/src/86Box/src/codegen_new/cpus/timings/486.yaml) |
| **1991** | AMD | Am386DX-40 | `CPU_386` | [`timings/386.yaml`](file:///home/marm/src/86Box/src/codegen_new/cpus/timings/386.yaml) |
| **1991** | IBM | 386SLC | `CPU_386` | [`timings/386.yaml`](file:///home/marm/src/86Box/src/codegen_new/cpus/timings/386.yaml) |
| **1992** | Intel | 80486DX2 / RapidCAD | `CPU_486` | [`timings/486.yaml`](file:///home/marm/src/86Box/src/codegen_new/cpus/timings/486.yaml) |
| **1993** | IBM | 486BL Blue Lightning | `CPU_486` | [`timings/486.yaml`](file:///home/marm/src/86Box/src/codegen_new/cpus/timings/486.yaml) |
| **1994** | AMD | Am486DX4-100 | `CPU_486` | [`timings/486.yaml`](file:///home/marm/src/86Box/src/codegen_new/cpus/timings/486.yaml) |
| **1994** | Intel | Pentium (P54C) | `CPU_PENTIUM` | [`timings/pentium.yaml`](file:///home/marm/src/86Box/src/codegen_new/cpus/timings/pentium.yaml) |
| **1994** | NexGen | Nx586 | `CPU_PENTIUM` | [`timings/k6.yaml`](file:///home/marm/src/86Box/src/codegen_new/cpus/timings/k6.yaml) |
| **1995** | AMD | Am5x86-P75 | `CPU_486` | [`timings/486.yaml`](file:///home/marm/src/86Box/src/codegen_new/cpus/timings/486.yaml) |
| **1995** | Cyrix | 5x86 (M1sc) | `CPU_486` | [`timings/486.yaml`](file:///home/marm/src/86Box/src/codegen_new/cpus/timings/486.yaml) |
| **1995** | Intel | Pentium Pro (P6) | `CPU_PENTIUMPRO` | [`timings/p6.yaml`](file:///home/marm/src/86Box/src/codegen_new/cpus/timings/p6.yaml) |
| **1997** | AMD | K6 (Model 6/7) | `CPU_PENTIUM` | [`timings/k6.yaml`](file:///home/marm/src/86Box/src/codegen_new/cpus/timings/k6.yaml) |
| **1997** | Intel | Pentium MMX (P55C) | `CPU_PENTIUM` | [`timings/pentium.yaml`](file:///home/marm/src/86Box/src/codegen_new/cpus/timings/pentium.yaml) |
| **1997** | Intel | Pentium II (Klamath) | `CPU_PENTIUM2` | [`timings/p6.yaml`](file:///home/marm/src/86Box/src/codegen_new/cpus/timings/p6.yaml) |
| **1997** | Cyrix | MediaGX / 6x86MX | `CPU_PENTIUM` | [`timings/cyrix_686.yaml`](file:///home/marm/src/86Box/src/codegen_new/cpus/timings/cyrix_686.yaml) |
| **1998** | AMD | K6-2 (Chomper / K6-3D) | `CPU_PENTIUM` | [`timings/k6.yaml`](file:///home/marm/src/86Box/src/codegen_new/cpus/timings/k6.yaml) |
| **1998** | Intel | Pentium II (Deschutes) | `CPU_PENTIUM2` | [`timings/p6.yaml`](file:///home/marm/src/86Box/src/codegen_new/cpus/timings/p6.yaml) |
| **1998** | Intel | Celeron (Mendocino) | `CPU_PENTIUM2` | [`timings/p6.yaml`](file:///home/marm/src/86Box/src/codegen_new/cpus/timings/p6.yaml) |
| **1998** | Rise | mP6 | `CPU_PENTIUM` | [`timings/pentium.yaml`](file:///home/marm/src/86Box/src/codegen_new/cpus/timings/pentium.yaml) |
| **1999** | AMD | K6-III (Sharptooth) | `CPU_PENTIUM` | [`timings/k6.yaml`](file:///home/marm/src/86Box/src/codegen_new/cpus/timings/k6.yaml) |
| **1999** | Intel | Pentium III (Katmai) | `CPU_PENTIUM3` | [`timings/p6.yaml`](file:///home/marm/src/86Box/src/codegen_new/cpus/timings/p6.yaml) |
| **1999** | Intel | Pentium III (Coppermine) | `CPU_PENTIUM3` | [`timings/p6.yaml`](file:///home/marm/src/86Box/src/codegen_new/cpus/timings/p6.yaml) |
| **2000** | AMD | K6-2+ / K6-III+ (Enhanced 3DNow!) | `CPU_PENTIUM` | [`timings/k6.yaml`](file:///home/marm/src/86Box/src/codegen_new/cpus/timings/k6.yaml) |
| **2000** | AMD | Athlon (Thunderbird) | `CPU_K7` | [`timings/k7.yaml`](file:///home/marm/src/86Box/src/codegen_new/cpus/timings/k7.yaml) |
| **2001** | Intel | Pentium III (Tualatin) | `CPU_PENTIUM3` | [`timings/p6.yaml`](file:///home/marm/src/86Box/src/codegen_new/cpus/timings/p6.yaml) |
| **2001** | Transmeta | Crusoe TM5800 | `CPU_PENTIUM` | [`timings/p6.yaml`](file:///home/marm/src/86Box/src/codegen_new/cpus/timings/p6.yaml) |
| **2002** | Intel | Pentium 4 (Northwood) | `CPU_PENTIUM4` | [`timings/pentium4.yaml`](file:///home/marm/src/86Box/src/codegen_new/cpus/timings/pentium4.yaml) |
| **2003** | AMD | Athlon XP (Barton) | `CPU_PENTIUM_3` | [`timings/k7.yaml`](file:///home/marm/src/86Box/src/codegen_new/cpus/timings/k7.yaml) |
| **2003** | VIA | C3 (Nehemiah) | `CPU_PENTIUM3` | [`timings/p6.yaml`](file:///home/marm/src/86Box/src/codegen_new/cpus/timings/p6.yaml) |
| **2004** | Intel | Pentium M (Dothan) | `CPU_PENTIUM_4` | [`timings/p6.yaml`](file:///home/marm/src/86Box/src/codegen_new/cpus/timings/p6.yaml) |
| **2004** | Transmeta | Efficeon TM8600 | `CPU_PENTIUM_4` | [`timings/p6.yaml`](file:///home/marm/src/86Box/src/codegen_new/cpus/timings/p6.yaml) |
| **2005** | AMD | Geode LX 800 | `CPU_PENTIUM` | [`timings/k6.yaml`](file:///home/marm/src/86Box/src/codegen_new/cpus/timings/k6.yaml) |
| **2007** | DM&P | Vortex86DX | `CPU_PENTIUM` | [`timings/486.yaml`](file:///home/marm/src/86Box/src/codegen_new/cpus/timings/486.yaml) |

---

## 3. Metacompiler Engine (`x86gen.py`)

`src/codegen_new/cpus/x86gen.py` automatically synthesizes tables and checks CPU compatibility:

```bash
# List all 37 CPU manifests chronologically by release year
python3 src/codegen_new/cpus/x86gen.py --list-cpus

# Query instruction set availability and #UD trap exceptions for any CPU
python3 src/codegen_new/cpus/x86gen.py --cpu geode_lx800

# Automated verification of auto-synthesized dependency masks against 86Box runtime C tables
python3 src/codegen_new/cpus/x86gen.py --verify

# Emit standalone C source implementing opcode_deps
python3 src/codegen_new/cpus/x86gen.py --emit-deps-c generated_deps.c
```

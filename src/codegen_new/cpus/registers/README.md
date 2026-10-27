# Declarative Register Specifications

This directory contains formal, declarative YAML specifications for x86 processor architectural registers, sub-register alias hierarchies, control bitfields, and model-specific registers.

---

## 1. Directory Structure

```
registers/
├── gpr.yaml                # General Purpose Registers (EAX..EDI, AX..DI, AL..BH)
├── segments.yaml           # Segment Registers (ES, CS, SS, DS, FS, GS) and Descriptor Caches
├── flags.yaml              # FLAGS (16-bit) and EFLAGS (32-bit) Architectural Bitfields
├── control_and_system.yaml # Control Registers (CR0..CR4), Debug Registers (DR0..DR7), Tables (GDTR..TR)
├── fpu_and_mmx.yaml        # x87 FPU Register Stack (ST0..ST7), Status/Control Words, and MMX Aliasing
├── simd_xmm.yaml           # SSE / SSE2 128-bit Vector Registers (XMM0..XMM7) and MXCSR
└── msr.yaml                # Model-Specific Registers (TSC, APIC, SYSENTER, MTRR, AMD STAR, VIA PadLock)
```

---

## 2. Register Hierarchy & Microarchitecture Support

| Register Group | Width | Min CPU Tier | Features Required | Sub-Register Views |
| :--- | :--- | :--- | :--- | :--- |
| **GPR 16-bit** | 16-bit | `CPU_8086` | Base | `AX`, `CX`, `DX`, `BX`, `SP`, `BP`, `SI`, `DI`, `AL`..`BH` |
| **GPR 32-bit** | 32-bit | `CPU_386` | Base | `EAX`, `ECX`, `EDX`, `EBX`, `ESP`, `EBP`, `ESI`, `EDI` |
| **Segments (Real)** | 16-bit | `CPU_8086` | Base | `ES`, `CS`, `SS`, `DS` |
| **Segments (Prot)** | 16-bit sel + 64-bit desc | `CPU_286` | Base | Visible selector + hidden descriptor cache (base, limit, access) |
| **Segments (Extended)** | 16-bit sel + 64-bit desc | `CPU_386` | Base | `FS`, `GS` |
| **Flags** | 16-bit / 32-bit | `CPU_8086` / `CPU_386` | Base | Arithmetic flags (`CF`..`OF`), System flags (`IOPL`, `NT`, `VM`, `VIF`, `VIP`) |
| **Control Registers** | 32-bit | `CPU_386` / `CPU_PENTIUM` | Base / `CR4` | `CR0` (Protection/Paging), `CR2` (Page Fault), `CR3` (PDBR), `CR4` (Extensions) |
| **Debug Registers** | 32-bit | `CPU_386` | Base | `DR0`..`DR3` (Breakpoint addresses), `DR6` (Status), `DR7` (Control) |
| **x87 FPU Stack** | 80-bit | `CPU_8086` | `CPU_FEATURE_FPU` | `ST(0)`..`ST(7)` (Circular stack addressed by `TOP`), `FCW`, `FSW`, `FTW` |
| **MMX Registers** | 64-bit | `CPU_PENTIUM` | `CPU_FEATURE_MMX` | `MM0`..`MM7` (Aliased to fraction of `ST(0)`..`ST(7)`) |
| **SIMD XMM** | 128-bit | `CPU_PENTIUM3` | `CPU_FEATURE_SSE` | `XMM0`..`XMM7` (Views: 4x f32, 2x f64, 16x u8, 8x u16, 4x u32, 2x u64), `MXCSR` |
| **MSRs** | 64-bit | `CPU_PENTIUM` | `CPU_FEATURE_MSR` | `TSC`, `APIC_BASE`, `SYSENTER_CS/ESP/EIP`, `MTRR`, AMD `STAR/EFER`, VIA `FCR` |

---

## 3. C Code Generation Target

The metacompiler [`x86gen.py`](file:///home/marm/src/86Box/src/codegen_new/cpus/x86gen.py) can synthesize C data structures matching 86Box's `cpu_state`:

```c
typedef union {
    uint32_t l;
    uint16_t w;
    struct {
        uint8_t l;
        uint8_t h;
    } b;
} x86reg;

typedef union {
    float    f32[4];
    double   f64[2];
    uint32_t u32[4];
    uint64_t u64[2];
    uint16_t u16[8];
    uint8_t  u8[16];
} xmm_reg_t;
```


# x86 Formal Architecture Specification (`x86_spec`)

This directory contains declarative, specification-driven instruction definitions for the 86Box dynamic recompiler (`codegen_new`) and interpreter, as proposed in [`src/codegen_new/docs/CODEGEN_FUTURE.md`](file:///home/marm/src/86Box/src/codegen_new/docs/CODEGEN_FUTURE.md).

All files in this directory are **100% strict, valid YAML 1.2**, allowing build-time tools (like `x86gen.py`) to parse them directly using standard libraries (`import yaml`) with zero custom parser code.

These files serve as an architectural prototype of a **single source of truth** from which both the JIT frontend (`gen_recomp_ops.c`), timing tables (`codegen_timing_*.c`), and the C interpreter (`gen_interp_ops.c`) can be synthesized.

---

## 1. Specification Syntax & Schema

Each specification file defines instruction families under the top-level `families:` mapping.

```yaml
families:
  <NAME>:
    description: <Human-readable summary>

    # Architectural availability & CPU feature requirements
    availability:
      min_cpu: <CPU_LEVEL>        # e.g., CPU_8086, CPU_186, CPU_286, CPU_386, CPU_486, CPU_PENTIUM, CPU_PENTIUMPRO
      feature: <CPU_FEATURE>      # Optional: CPU_FEATURE_MMX, CPU_FEATURE_RDTSC, CPU_FEATURE_CX8, etc.
      exceptions:                 # Architectural fault conditions (e.g., #UD, #NM, #GP)
        - cond: <EXPRESSION>
          fault: <FAULT_TYPE>

    # Abstract microarchitecture timing class
    class: <TIMING_CLASS>        # e.g., alu, alu_carry, shift, mov, branch_cond

    # Opcode to Operand Mapping
    opcode_map:
      "<OPCODE_PATTERN>": { <OPERAND_ROLE>: <OPERAND_DESCRIPTOR>, ... }

    # Pure Abstract Semantics
    semantics:
      args: [<PARAM_LIST>]
      body: |
        <EXPRESSION_STATEMENTS>
        flags:
          tag: <LAZY_FLAG_TAG>      # e.g., FLAGS_ADD, FLAGS_SUB, FLAGS_LOGIC
          op1: <EXPR>
          op2: <EXPR>
          res: <EXPR>
          preserve: [<FLAG_NAMES>]  # e.g., [CF] for INC/DEC
        writeback: <DST> = <RES>     # or 'none' for CMP/TEST
```

---

## 2. Instruction Availability & Architectural Dispatch

Instruction availability allows the metacompiler (`x86gen`) to automatically generate:
1. **CPU-Family Decode Tables**: At initialization, 86Box builds opcode lookup tables tailored to the configured CPU model. If an instruction requires `CPU_386` and the simulated machine is a `CPU_286`, the opcode slot points directly to `ud_fault()` with **zero runtime branch overhead**.
2. **SIMD & FPU Guards**: MMX/3DNow!/FPU instructions declare `#NM` (Device Not Available) triggers when `CR0.TS` or `CR0.EM` are set, synthesizing the exact context-switch traps required by multi-tasking operating systems.
3. **Privilege & Feature Checks**: Instructions like `RDTSC` (`CR4.TSD`), `HLT` (`CPL == 0`), and `CLI`/`STI` (`CPL <= IOPL`) declare their architectural privilege rules declaratively.

---

## 3. Standard Operand Notation (Intel / AMD / XED Convention)

| Code | Type | Meaning |
| :--- | :--- | :--- |
| **`Eb`** | `u8` | ModR/M byte operand: GPR (`AL..BH`) or Memory byte |
| **`Ew`** | `u16` | ModR/M word operand: GPR (`AX..DI`) or Memory word |
| **`Ed`** | `u32` | ModR/M dword operand: GPR (`EAX..EDI`) or Memory dword |
| **`Ev`** | `u16` / `u32` | ModR/M variable-size word/dword operand (selected by `op_32`) |
| **`Gb`** | `u8` | General-purpose register byte from ModR/M `reg` field (`AL..BH`) |
| **`Gw`** | `u16` | General-purpose register word from ModR/M `reg` field (`AX..DI`) |
| **`Gd`** | `u32` | General-purpose register dword from ModR/M `reg` field (`EAX..EDI`) |
| **`Gv`** | `u16` / `u32` | General-purpose register word/dword from ModR/M `reg` field |
| **`Pq`** | `u64` | MMX register from ModR/M `reg` field (`MM0..MM7`) |
| **`Qq`** | `u64` | MMX register or 64-bit Memory operand from ModR/M `r/m` field |
| **`Ib`** | `u8` | Immediate byte payload from instruction stream |
| **`Iv`** | `u16` / `u32` | Immediate word/dword payload from instruction stream |
| **`Ib_sext`** | `s32` | Immediate byte sign-extended to operand size (e.g. `0x83 /0`) |
| **`Jb`** | `s8` | Relative branch displacement byte (signed offset) |
| **`Jv`** | `s16` / `s32` | Relative branch displacement word/dword |
| **`AL` / `eAX`**| Fixed | Implicit accumulator register |
| **`DX` / `CL`** | Fixed | Implicit port / count register |
| **`Sw`** | `u16` | Segment register (`ES`, `CS`, `SS`, `DS`, `FS`, `GS`) |

---

## 4. Directory Layout

The specifications are partitioned by functional instruction family:

1. [`types_and_operands.yaml`](file:///home/marm/src/86Box/src/codegen_new/x86_spec/types_and_operands.yaml): Abstract types, CPU generation hierarchy, CPU feature flags, architectural exceptions, register mappings, and lazy-flag tags.
2. [`arithmetic.yaml`](file:///home/marm/src/86Box/src/codegen_new/x86_spec/arithmetic.yaml): `ADD`, `ADC`, `SUB`, `SBB`, `CMP`, `NEG`, `INC`, `DEC`.
3. [`logic_and_bit.yaml`](file:///home/marm/src/86Box/src/codegen_new/x86_spec/logic_and_bit.yaml): `AND`, `OR`, `XOR`, `TEST`, `NOT`, `BT`, `BTS`, `BTR`, `BTC`, `BSF`, `BSR`.
4. [`mov_and_transfer.yaml`](file:///home/marm/src/86Box/src/codegen_new/x86_spec/mov_and_transfer.yaml): `MOV`, `MOVZX`, `MOVSX`, `LEA`, `XCHG`, `CBW`/`CWDE`, `CWD`/`CDQ`, `BSWAP`, `XLAT`.
5. [`shift_rotate.yaml`](file:///home/marm/src/86Box/src/codegen_new/x86_spec/shift_rotate.yaml): `SHL`/`SAL`, `SHR`, `SAR`, `ROL`, `ROR`, `RCL`, `RCR`, `SHLD`, `SHRD`.
6. [`branch_and_call.yaml`](file:///home/marm/src/86Box/src/codegen_new/x86_spec/branch_and_call.yaml): `Jcc` (all 16 conditions), `SETcc`, `JMP`, `CALL`, `RET`/`RETF`, `LOOP`, `JCXZ`/`JECXZ`.
7. [`stack_and_frame.yaml`](file:///home/marm/src/86Box/src/codegen_new/x86_spec/stack_and_frame.yaml): `PUSH`, `POP`, `PUSHA`/`POPA`, `PUSHF`/`POPF`, `ENTER`, `LEAVE`.
8. [`string_and_io.yaml`](file:///home/marm/src/86Box/src/codegen_new/x86_spec/string_and_io.yaml): `MOVS`, `CMPS`, `STOS`, `LODS`, `SCAS`, `IN`, `OUT`, `INS`, `OUTS`.
9. [`system_and_misc.yaml`](file:///home/marm/src/86Box/src/codegen_new/x86_spec/system_and_misc.yaml): `NOP`, `HLT`, `CLI`, `STI`, `CLD`, `STD`, `CMC`, `CLC`, `STC`, `CPUID`, `RDTSC`, `LOCK`, `INT`, `INTO`, `IRET`/`IRETD`.
10. [`simd_mmx.yaml`](file:///home/marm/src/86Box/src/codegen_new/x86_spec/simd_mmx.yaml): MMX SIMD integer instruction families (`EMMS`, `PADDB`/`W`/`D`, `PSUBB`/`W`/`D`, `POR`, `PXOR`, `PAND`).
11. [`timings/`](file:///home/marm/src/86Box/src/codegen_new/x86_spec/timings/README.md): Microarchitectural timing profiles:
    - [`classes.yaml`](file:///home/marm/src/86Box/src/codegen_new/x86_spec/timings/classes.yaml): Abstract execution and memory interaction classes (`alu_reg`, `alu_rm`, `alu_rmw`, `shift`, `mul`, `branch`, etc.).
    - [`386.yaml`](file:///home/marm/src/86Box/src/codegen_new/x86_spec/timings/386.yaml): 80386 unpipelined microcode, bus width (DX 32-bit vs SX 16-bit).
    - [`486.yaml`](file:///home/marm/src/86Box/src/codegen_new/x86_spec/timings/486.yaml): 80486 scalar 5-stage pipeline, cycles, and AGI stalls.
    - [`pentium.yaml`](file:///home/marm/src/86Box/src/codegen_new/x86_spec/timings/pentium.yaml): Pentium P54C/P55C superscalar dual-issue U/V pairing rules (`PAIR_UV`, `PAIR_U`, `PAIR_V`, `PAIR_NP`), BTB branch hit, and prefix decode stalls.
    - [`cyrix_686.yaml`](file:///home/marm/src/86Box/src/codegen_new/x86_spec/timings/cyrix_686.yaml): Cyrix 6x86 M1/M2 X/Y pairing with speculative data forwarding.
    - [`k6.yaml`](file:///home/marm/src/86Box/src/codegen_new/x86_spec/timings/k6.yaml): AMD K6 / K6-2 RISC86 decoupled out-of-order execution, dual ALU and 3DNow! units.
    - [`p6.yaml`](file:///home/marm/src/86Box/src/codegen_new/x86_spec/timings/p6.yaml): Intel P6 (Pentium Pro / PII / PIII) 4-1-1 macro-decoders, ROB, RS, Execution Ports 0–4 dispatch.

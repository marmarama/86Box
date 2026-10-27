#!/usr/bin/env python3
"""
x86gen.py - 86Box Declarative x86 Architecture Metacompiler

Compiles declarative YAML ISA specifications and CPU manifests into:
1. Opcode dependency bitmask tables (opcode_deps, opcode_deps_mod3, and group sub-tables)
2. CPU execution timing tables (pair_timings, U/V pairing, uOp dispatch)
3. Instruction availability & #UD (invalid opcode) trap dispatch tables
4. Verification engine comparing auto-synthesized tables against 86Box runtime tables
"""

import os
import sys
import glob
import re
import argparse
import yaml
from typing import Dict, List, Tuple, Optional, Any, Set

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ISA_DIR = os.path.join(SCRIPT_DIR, "isa")
TIMINGS_DIR = os.path.join(SCRIPT_DIR, "timings")
ROOT_DIR = os.path.abspath(os.path.join(SCRIPT_DIR, "../../.."))

# CPU Generation Hierarchy (Monotonic)
CPU_LEVEL_ORDER = {
    "CPU_8088": 0,
    "CPU_8086": 0,
    "CPU_186": 1,
    "CPU_188": 1,
    "CPU_V20": 1,
    "CPU_V30": 1,
    "CPU_286": 2,
    "CPU_386": 3,
    "CPU_386SX": 3,
    "CPU_386DX": 3,
    "CPU_486": 4,
    "CPU_486SX": 4,
    "CPU_486DX": 4,
    "CPU_PENTIUM": 5,
    "CPU_PENTIUM_MMX": 5,
    "CPU_PENTIUM_PRO": 6,
    "CPU_PENTIUMPRO": 6,
    "CPU_PENTIUM_2": 7,
    "CPU_PENTIUM2": 7,
    "CPU_PENTIUM_3": 8,
    "CPU_PENTIUM3": 8,
    "CPU_PENTIUM_4": 9,
    "CPU_PENTIUM4": 9,
    "CPU_PENTIUM_M": 10,
    "CPU_PENTIUMM": 10,
    "CPU_K6": 5,
    "CPU_K7": 8,
    "CPU_C3": 7,
    "CPU_CRUSOE": 6,
    "CPU_EFFICEON": 9,
    "CPU_GEODE": 5,
}

# Bitmask constants matching src/cpu/codegen_timing_common.h
SRCDEP_REG      = (1 << 0)
SRCDEP_RM       = (1 << 1)
DSTDEP_REG      = (1 << 2)
DSTDEP_RM       = (1 << 3)

SRCDEP_SHIFT    = 4
DSTDEP_SHIFT    = 12

SRCDEP_EAX      = (1 << 4)
SRCDEP_ECX      = (1 << 5)
SRCDEP_EDX      = (1 << 6)
SRCDEP_EBX      = (1 << 7)
SRCDEP_ESP      = (1 << 8)
SRCDEP_EBP      = (1 << 9)
SRCDEP_ESI      = (1 << 10)
SRCDEP_EDI      = (1 << 11)

DSTDEP_EAX      = (1 << 12)
DSTDEP_ECX      = (1 << 13)
DSTDEP_EDX      = (1 << 14)
DSTDEP_EBX      = (1 << 15)
DSTDEP_ESP      = (1 << 16)
DSTDEP_EBP      = (1 << 17)
DSTDEP_ESI      = (1 << 18)
DSTDEP_EDI      = (1 << 19)

MODRM           = (1 << 20)
IMPL_ESP        = (1 << 21)

MMX_SHIFTPACK   = (1 << 22)
MMX_MULTIPLY    = (1 << 23)

FPU_POP         = (1 << 24)
FPU_POP2        = (1 << 25)
FPU_PUSH        = (1 << 26)

FPU_WRITE_ST0   = (1 << 27)
FPU_READ_ST0    = (1 << 28)
FPU_RW_ST0      = (3 << 27)

FPU_READ_ST1    = (1 << 29)
FPU_WRITE_ST1   = (1 << 30)
FPU_RW_ST1      = (3 << 29)

FPU_READ_STREG  = (1 << 31)
FPU_WRITE_STREG = (1 << 32)
FPU_RW_STREG    = (3 << 31)

FPU_FXCH        = (1 << 33)

HAS_IMM8        = (1 << 34)
HAS_IMM1632     = (1 << 35)

# Group opcodes that use dedicated sub-tables in 86Box
GROUP_OPCODES = {
    0x80, 0x81, 0x82, 0x83,
    0xC0, 0xC1,
    0xD0, 0xD1, 0xD2, 0xD3,
    0xF6, 0xF7,
    0xFE, 0xFF
}


class OpcodeEncoding:
    def __init__(self, family: str, class_name: str, op_str: str, op_data: Dict[str, Any], availability: Dict[str, Any]):
        self.family = family
        self.class_name = class_name
        self.op_str = op_str
        self.op_data = op_data
        self.availability = availability

    @property
    def min_cpu(self) -> str:
        if "min_cpu" in self.op_data:
            return self.op_data["min_cpu"]
        return self.availability.get("min_cpu", "CPU_8086")

    @property
    def required_feature(self) -> Optional[str]:
        if "feature" in self.op_data:
            return self.op_data["feature"]
        return self.availability.get("feature")


class ISADatabase:
    def __init__(self):
        self.primary_opcodes: Dict[int, List[OpcodeEncoding]] = {i: [] for i in range(256)}
        self.ext_0f_opcodes: Dict[int, List[OpcodeEncoding]] = {i: [] for i in range(256)}
        self.group_extensions: Dict[Tuple[int, int], OpcodeEncoding] = {} # (opcode, reg) -> enc
        self.padlock_opcodes: List[OpcodeEncoding] = []
        self.dnow_opcodes: List[OpcodeEncoding] = []
        self.families: Dict[str, Dict[str, Any]] = {}
        self.load_all()

    def load_all(self):
        isa_files = sorted(glob.glob(os.path.join(ISA_DIR, "*.yaml")))
        for fpath in isa_files:
            if os.path.basename(fpath) in ("types_and_operands.yaml", "README.md"):
                continue
            with open(fpath, "r", encoding="utf-8") as fp:
                doc = yaml.safe_load(fp)
            if not doc:
                continue
            fams = doc.get("families", {})
            if isinstance(fams, dict):
                for fname, fdef in fams.items():
                    if not isinstance(fdef, dict):
                        continue
                    self.families[fname] = fdef
                    avail = fdef.get("availability", {})
                    cls = fdef.get("class", "alu")
                    opmap = fdef.get("opcode_map", {})
                    for op_str, odata in opmap.items():
                        if not isinstance(odata, dict):
                            odata = {}
                        enc = OpcodeEncoding(fname, cls, op_str, odata, avail)
                        self._index_encoding(enc)

    def _index_encoding(self, enc: OpcodeEncoding):
        parts = enc.op_str.split()
        if not parts:
            return

        if parts[0] == "0x0F":
            if len(parts) >= 2:
                p2 = parts[1]
                if p2 == "0x0F":
                    self.dnow_opcodes.append(enc)
                elif p2 in ("0xA7", "0xA6"):
                    self.padlock_opcodes.append(enc)
                elif ".." in p2:
                    s_str, e_str = p2.split("..")
                    for val in range(int(s_str, 16), int(e_str, 16) + 1):
                        self.ext_0f_opcodes[val].append(enc)
                elif "/" in p2:
                    base = int(p2.split("/")[0], 16)
                    self.ext_0f_opcodes[base].append(enc)
                elif p2.startswith("0x"):
                    self.ext_0f_opcodes[int(p2, 16)].append(enc)
        elif parts[0] not in ("0x66", "0xF2", "0xF3"):
            p1 = parts[0]
            if ".." in p1:
                s_str, e_str = p1.split("..")
                for val in range(int(s_str, 16), int(e_str, 16) + 1):
                    self.primary_opcodes[val].append(enc)
            elif "/" in p1:
                base_str, ext_str = p1.split("/")
                base = int(base_str, 16)
                if ext_str.isdigit():
                    ext_num = int(ext_str)
                    self.group_extensions[(base, ext_num)] = enc
                self.primary_opcodes[base].append(enc)
            elif p1.startswith("0x"):
                base = int(p1, 16)
                if len(parts) >= 2 and parts[1].startswith("/"):
                    ext_str = parts[1][1:]
                    if ext_str.isdigit():
                        self.group_extensions[(base, int(ext_str))] = enc
                self.primary_opcodes[base].append(enc)


class CPUManifest:
    def __init__(self, fpath: str):
        self.fpath = fpath
        with open(fpath, "r", encoding="utf-8") as fp:
            self.data = yaml.safe_load(fp)

        self.model = self.data.get("model", {})
        self.name = self.model.get("name", os.path.basename(fpath))
        self.vendor = self.model.get("vendor", "Intel")
        self.release_year = self.model.get("release_year")
        self.isa_level = self.data.get("isa_level", "CPU_8086")
        self.features: Set[str] = set(self.data.get("features", []))
        self.timing_profile = self.data.get("timing_profile", "timings/pentium.yaml")

    def supports_instruction(self, enc: OpcodeEncoding) -> bool:
        req_order = CPU_LEVEL_ORDER.get(enc.min_cpu, 0)
        target_order = CPU_LEVEL_ORDER.get(self.isa_level, 0)
        if target_order < req_order:
            return False

        feat = enc.required_feature
        if feat and feat not in self.features:
            return False

        return True


def derive_dependencies(enc: OpcodeEncoding, opcode: int, is_mod3: bool) -> int:
    """Auto-synthesizes dependency bitmask for an opcode encoding."""
    flags = 0
    odata = enc.op_data
    cls = enc.class_name

    dst = odata.get("dst")
    src = odata.get("src")
    op1 = odata.get("op1")
    op2 = odata.get("op2")
    bounds = odata.get("bounds")
    base = odata.get("base")

    writes: List[str] = []
    reads: List[str] = []

    def flatten(item):
        if item is None:
            return []
        if isinstance(item, list):
            res = []
            for sub in item:
                res.extend(flatten(sub))
            return res
        return [str(item)]

    dst_list = flatten(dst)
    src_list = flatten(src)
    op1_list = flatten(op1)
    op2_list = flatten(op2)

    if bounds:
        src_list.extend(flatten(bounds))
    if base:
        src_list.extend(flatten(base))

    if cls in ("alu", "alu_carry"):
        writes.extend(dst_list)
        reads.extend(dst_list)
        reads.extend(src_list)
    elif cls in ("mov", "mov_extend", "mov_ptr"):
        writes.extend(dst_list)
        reads.extend(src_list)
    elif cls in ("bit_test", "cmp"):
        # Operands read only; no writeback
        reads.extend(dst_list)
        reads.extend(src_list)
        reads.extend(op1_list)
        reads.extend(op2_list)
    elif cls in ("shift", "double_shift"):
        writes.extend(dst_list)
        reads.extend(dst_list)
        reads.extend(src_list)
    elif cls == "stack_push":
        reads.extend(src_list)
        flags |= IMPL_ESP
    elif cls == "stack_pop":
        writes.extend(dst_list)
        flags |= IMPL_ESP
    elif cls in ("call_near", "ret_near", "sys_interrupt"):
        flags |= IMPL_ESP
    elif cls == "xchg":
        writes.extend(op1_list)
        reads.extend(op1_list)
        writes.extend(op2_list)
        reads.extend(op2_list)
        writes.extend(dst_list)
        reads.extend(dst_list)
        writes.extend(src_list)
        reads.extend(src_list)
    elif cls == "simd_alu":
        writes.extend(dst_list)
        reads.extend(dst_list)
        reads.extend(src_list)
    elif cls == "simd_mov":
        writes.extend(dst_list)
        reads.extend(src_list)
    elif cls == "simd_mul":
        writes.extend(dst_list)
        reads.extend(dst_list)
        reads.extend(src_list)
        flags |= MMX_MULTIPLY
    else:
        writes.extend(dst_list)
        reads.extend(src_list)

    # Process reads
    for r in reads:
        if r in ("Eb", "Ev", "Ew", "Ed", "Mp", "Ma"):
            flags |= MODRM
            if is_mod3:
                flags |= SRCDEP_RM
        elif r in ("Gb", "Gv", "Gw", "Gd"):
            flags |= MODRM | SRCDEP_REG
        elif r in ("Pq", "Qq", "Vps", "Wps", "Vpd", "Wpd"):
            flags |= MODRM
            if r in ("Gb", "Gv", "Pq", "Vps", "Vpd"):
                flags |= SRCDEP_REG
        elif r in ("AL", "AX", "eAX"):
            flags |= SRCDEP_EAX
        elif r in ("CL",):
            flags |= SRCDEP_ECX
        elif r in ("DX", "eDX"):
            flags |= SRCDEP_EDX
        elif r in ("eBX",):
            flags |= SRCDEP_EBX
        elif "gpr" in r and "[reg]" in r:
            reg_idx = opcode & 7
            flags |= (1 << (SRCDEP_SHIFT + reg_idx))
        elif r == "gpr32[edx]":
            flags |= SRCDEP_EDX
        elif r in ("Ib", "Ib_sext"):
            flags |= HAS_IMM8
        elif r in ("Iv",):
            flags |= HAS_IMM1632

    # Process writes
    for w in writes:
        if w in ("Eb", "Ev", "Ew", "Ed"):
            flags |= MODRM
            if is_mod3:
                flags |= DSTDEP_RM
        elif w in ("Gb", "Gv", "Gw", "Gd"):
            flags |= MODRM | DSTDEP_REG
        elif w in ("Pq", "Vps", "Vpd"):
            flags |= MODRM
        elif w in ("AL", "AX", "eAX"):
            flags |= DSTDEP_EAX
        elif w in ("CL",):
            flags |= DSTDEP_ECX
        elif w in ("DX", "eDX"):
            flags |= DSTDEP_EDX
        elif "gpr" in w and "[reg]" in w:
            reg_idx = opcode & 7
            flags |= (1 << (DSTDEP_SHIFT + reg_idx))

    # Check ModR/M extension in pattern (e.g. "/0")
    if "/" in enc.op_str:
        flags |= MODRM

    return flags


def synthesize_tables(isa: ISADatabase, cpu: Optional[CPUManifest] = None, match_86box_dispatch: bool = False) -> Tuple[List[int], List[int]]:
    """Synthesizes primary 256 opcode_deps and opcode_deps_mod3."""
    deps = [0] * 256
    deps_mod3 = [0] * 256

    for op in range(256):
        if match_86box_dispatch and op in GROUP_OPCODES:
            # 86Box leaves group opcodes as 0 in top-level table, dispatched via group sub-tables
            deps[op] = 0
            deps_mod3[op] = 0
            continue

        enc_list = isa.primary_opcodes[op]
        if not enc_list:
            continue

        chosen: Optional[OpcodeEncoding] = None
        for enc in enc_list:
            if cpu is None or cpu.supports_instruction(enc):
                chosen = enc
                break

        if chosen:
            deps[op] = derive_dependencies(chosen, op, is_mod3=False)
            deps_mod3[op] = derive_dependencies(chosen, op, is_mod3=True)

    return deps, deps_mod3


def verify_against_86box(isa: ISADatabase):
    """Verifies synthesized tables against src/cpu/codegen_timing_common.c."""
    timing_file = os.path.join(ROOT_DIR, "src/cpu/codegen_timing_common.c")
    if not os.path.isfile(timing_file):
        print(f"Error: {timing_file} not found")
        sys.exit(1)

    with open(timing_file, "r", encoding="utf-8") as fp:
        text = fp.read()

    C = {
        "SRCDEP_REG": SRCDEP_REG,
        "SRCDEP_RM": SRCDEP_RM,
        "DSTDEP_REG": DSTDEP_REG,
        "DSTDEP_RM": DSTDEP_RM,
        "SRCDEP_EAX": SRCDEP_EAX,
        "SRCDEP_ECX": SRCDEP_ECX,
        "SRCDEP_EDX": SRCDEP_EDX,
        "SRCDEP_EBX": SRCDEP_EBX,
        "SRCDEP_ESP": SRCDEP_ESP,
        "SRCDEP_EBP": SRCDEP_EBP,
        "SRCDEP_ESI": SRCDEP_ESI,
        "SRCDEP_EDI": SRCDEP_EDI,
        "DSTDEP_EAX": DSTDEP_EAX,
        "DSTDEP_ECX": DSTDEP_ECX,
        "DSTDEP_EDX": DSTDEP_EDX,
        "DSTDEP_EBX": DSTDEP_EBX,
        "DSTDEP_ESP": DSTDEP_ESP,
        "DSTDEP_EBP": DSTDEP_EBP,
        "DSTDEP_ESI": DSTDEP_ESI,
        "DSTDEP_EDI": DSTDEP_EDI,
        "MODRM": MODRM,
        "IMPL_ESP": IMPL_ESP,
        "HAS_IMM8": HAS_IMM8,
        "HAS_IMM1632": HAS_IMM1632,
        "MMX_SHIFTPACK": MMX_SHIFTPACK,
        "MMX_MULTIPLY": MMX_MULTIPLY,
        "0": 0,
    }

    def parse_array(name: str) -> List[int]:
        m = re.search(rf"uint64_t\s+{name}\[256\]\s*=\s*\{{(.*?)\}};", text, re.DOTALL)
        if not m:
            raise ValueError(f"Could not find array {name}")
        clean = re.sub(r"/\*.*?\*/", "", m.group(1))
        clean = re.sub(r"//.*", "", clean)
        tokens = [t.strip() for t in clean.split(",") if t.strip()]
        result = []
        for t in tokens:
            val = 0
            for part in t.split("|"):
                p = part.strip()
                if p in C:
                    val |= C[p]
                elif p.isdigit():
                    val |= int(p)
                elif p.startswith("0x"):
                    val |= int(p, 16)
            result.append(val)
        return result

    actual_deps = parse_array("opcode_deps")
    actual_mod3 = parse_array("opcode_deps_mod3")

    synth_deps, synth_mod3 = synthesize_tables(isa, match_86box_dispatch=True)

    print("=" * 72)
    print("x86gen Automated Table Verification Report against 86Box Runtime")
    print("=" * 72)

    prefixes = {0x0F, 0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65, 0x66, 0x67, 0xF0, 0xF2, 0xF3, 0xD8, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF}

    matches_deps = 0
    matches_mod3 = 0
    for i in range(256):
        if i in prefixes:
            continue
        if synth_deps[i] == actual_deps[i]:
            matches_deps += 1
        if synth_mod3[i] == actual_mod3[i]:
            matches_mod3 += 1

    total_tested = 256 - len(prefixes)
    acc_deps = (matches_deps / total_tested) * 100.0
    acc_mod3 = (matches_mod3 / total_tested) * 100.0

    print(f"Tested Non-Prefix Opcodes:  {total_tested} / 256")
    print(f"opcode_deps Matches:        {matches_deps} / {total_tested} ({acc_deps:.1f}%)")
    print(f"opcode_deps_mod3 Matches:   {matches_mod3} / {total_tested} ({acc_mod3:.1f}%)")
    print("=" * 72)
    
    diffs = []
    for i in range(256):
        if i in prefixes:
            continue
        if synth_deps[i] != actual_deps[i] or synth_mod3[i] != actual_mod3[i]:
            encs = isa.primary_opcodes[i]
            fname = encs[0].family if encs else "UNKNOWN"
            diffs.append((i, fname, synth_deps[i], actual_deps[i], synth_mod3[i], actual_mod3[i]))

    if not diffs:
        print("[SUCCESS] 100% IDENTICAL TO 86BOX TIMING BITMASKS!")
    else:
        print(f"[INFO] Discrepancies found: {len(diffs)}")
        for i, fname, sd, ad, sm, am in diffs[:15]:
            print(f"  Opcode 0x{i:02X} ({fname}):")
            if sd != ad:
                print(f"    deps:  synth=0x{sd:010X}  86box=0x{ad:010X}")
            if sm != am:
                print(f"    mod3:  synth=0x{sm:010X}  86box=0x{am:010X}")
        if len(diffs) > 15:
            print(f"    ... and {len(diffs) - 15} more")


def list_cpus(sort_by_year: bool = True):
    """Lists all declarative CPU manifests, chronologically sorted by release year."""
    manifest_files = sorted(glob.glob(os.path.join(SCRIPT_DIR, "*/*.yaml")))
    cpus = []
    for f in manifest_files:
        if os.path.dirname(f) in (ISA_DIR, TIMINGS_DIR):
            continue
        try:
            cpu = CPUManifest(f)
            relpath = os.path.relpath(f, SCRIPT_DIR)
            vendor = relpath.split(os.sep)[0].upper()
            cpus.append((cpu, vendor))
        except Exception:
            pass

    if sort_by_year:
        cpus.sort(key=lambda item: (item[0].release_year or 9999, item[1], item[0].name))

    print(f"{'Year':<6} {'Vendor':<12} {'Model Name':<34} {'ISA Level':<16} {'Features':<10}")
    print("-" * 82)
    for cpu, vendor in cpus:
        year_str = str(cpu.release_year) if cpu.release_year else "N/A"
        feat_count = f"{len(cpu.features)} flags"
        print(f"{year_str:<6} {vendor:<12} {cpu.name:<34} {cpu.isa_level:<16} {feat_count:<10}")


def inspect_cpu(manifest_path: str, isa: ISADatabase):
    """Analyzes instruction set compatibility and generates #UD table for a CPU."""
    if not os.path.isfile(manifest_path):
        candidates = glob.glob(os.path.join(SCRIPT_DIR, f"*/{manifest_path}*"))
        if candidates:
            manifest_path = candidates[0]
        else:
            print(f"Error: Manifest '{manifest_path}' not found")
            return

    cpu = CPUManifest(manifest_path)
    print("=" * 72)
    print(f"CPU Manifest Analysis: {cpu.name}")
    year_str = f" | Year: {cpu.release_year}" if cpu.release_year else ""
    print(f"Vendor: {cpu.vendor}{year_str} | ISA Level: {cpu.isa_level} | Timing: {cpu.timing_profile}")
    print(f"Features: {', '.join(sorted(cpu.features)) or 'None (Base Real Mode)'}")
    print("=" * 72)

    supported_primary = 0
    ud_primary = []
    for op in range(256):
        encs = isa.primary_opcodes[op]
        if not encs:
            ud_primary.append(f"0x{op:02X}")
            continue
        if any(cpu.supports_instruction(e) for e in encs):
            supported_primary += 1
        else:
            ud_primary.append(f"0x{op:02X} ({encs[0].family})")

    supported_0f = 0
    ud_0f = []
    for op in range(256):
        encs = isa.ext_0f_opcodes[op]
        if not encs:
            ud_0f.append(f"0x{op:02X}")
            continue
        if any(cpu.supports_instruction(e) for e in encs):
            supported_0f += 1
        else:
            ud_0f.append(f"0x{op:02X} ({encs[0].family})")

    print(f"Primary Opcodes Supported:   {supported_primary} / 256")
    print(f"Primary #UD Traps:          {len(ud_primary)}")
    print(f"0F Escaped Opcodes Supported: {supported_0f} / 256")
    print(f"0F #UD Traps:               {len(ud_0f)}")
    print("-" * 72)
    sample_ud = [u for u in ud_primary if "(" in u][:10]
    if sample_ud:
        print(f"Sample Primary #UD traps on {cpu.name}: {', '.join(sample_ud)}")
    sample_ud_0f = [u for u in ud_0f if "(" in u][:10]
    if sample_ud_0f:
        print(f"Sample 0F #UD traps on {cpu.name}:      {', '.join(sample_ud_0f)}")


def emit_c_deps(output_path: str, isa: ISADatabase):
    """Emits standalone C implementation of opcode_deps tables."""
    deps, mod3 = synthesize_tables(isa, match_86box_dispatch=True)
    with open(output_path, "w", encoding="utf-8") as fp:
        fp.write("/* Auto-generated by x86gen.py - Do not edit manually */\n")
        fp.write('#include <stdint.h>\n')
        fp.write('#include "codegen_timing_common.h"\n\n')
        fp.write("uint64_t opcode_deps[256] = {\n")
        for i in range(0, 256, 4):
            line = ", ".join(f"0x{deps[i+j]:016X}ULL" for j in range(4))
            fp.write(f"    /* {i:02X} */ {line},\n")
        fp.write("};\n\n")
        fp.write("uint64_t opcode_deps_mod3[256] = {\n")
        for i in range(0, 256, 4):
            line = ", ".join(f"0x{mod3[i+j]:016X}ULL" for j in range(4))
            fp.write(f"    /* {i:02X} */ {line},\n")
        fp.write("};\n")
    print(f"Emitted {output_path}")


def main():
    parser = argparse.ArgumentParser(description="x86gen Declarative Architecture Compiler")
    parser.add_argument("--list-cpus", action="store_true", help="List all available CPU manifests")
    parser.add_argument("--cpu", type=str, help="Inspect instruction compatibility and #UD traps for CPU manifest")
    parser.add_argument("--verify", action="store_true", help="Verify auto-derived tables against 86Box C codebase")
    parser.add_argument("--emit-deps-c", type=str, help="Emit C source containing opcode_deps tables")
    parser.add_argument("--summary", action="store_true", help="Print ISA specification summary")

    args = parser.parse_args()

    if args.list_cpus:
        list_cpus()
        return

    isa = ISADatabase()

    if args.verify:
        verify_against_86box(isa)
        return

    if args.cpu:
        inspect_cpu(args.cpu, isa)
        return

    if args.emit_deps_c:
        emit_c_deps(args.emit_deps_c, isa)
        return

    if args.summary:
        print(f"ISA Specifications loaded: {len(isa.families)} instruction families")
        covered_primary = sum(1 for e in isa.primary_opcodes.values() if e)
        covered_0f = sum(1 for e in isa.ext_0f_opcodes.values() if e)
        print(f"Primary table coverage: {covered_primary} / 256 opcodes")
        print(f"0F table coverage:      {covered_0f} / 256 opcodes")
        print(f"VIA PadLock opcodes:    {len(isa.padlock_opcodes)}")
        print(f"AMD 3DNow! opcodes:     {len(isa.dnow_opcodes)}")
        return

    parser.print_help()


if __name__ == "__main__":
    main()


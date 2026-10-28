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
REGISTERS_DIR = os.path.join(SCRIPT_DIR, "registers")
FPUS_DIR = os.path.join(SCRIPT_DIR, "fpus")
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
    0xFF
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
        self.modrm_groups: Dict[str, Dict[str, Any]] = {}
        self.micro_semantics: Dict[str, Dict[str, Any]] = {}
        self.primitive_ops: Dict[str, Dict[str, Any]] = {}
        self.immediate_encodings: Dict[str, Dict[str, Any]] = {}
        self.load_all()

    def load_all(self):
        types_fpath = os.path.join(ISA_DIR, "types_and_operands.yaml")
        if os.path.isfile(types_fpath):
            with open(types_fpath, "r", encoding="utf-8") as fp:
                types_doc = yaml.safe_load(fp) or {}
            self.modrm_groups = types_doc.get("modrm_groups", {})
            self.micro_semantics = types_doc.get("micro_semantics", {})
            self.primitive_ops = self.micro_semantics.get("primitive_ops", {})
            self.immediate_encodings = types_doc.get("immediate_encodings", {})

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
        self.fpu_profile = self.data.get("fpu_profile")
        self.supported_coprocessors = self.data.get("supported_coprocessors", [])
        self.cpu_frequencies_mhz = self.model.get("cpu_frequencies_mhz", [])
        self.emulated_frequency_range_mhz = self.model.get("emulated_frequency_range_mhz", [])
        self.default_emulated_frequencies_mhz = self.model.get("default_emulated_frequencies_mhz", [])
        if not self.emulated_frequency_range_mhz and "frequency_range_mhz" in self.model:
            self.emulated_frequency_range_mhz = self.model["frequency_range_mhz"]
        self.quirks: List[Dict[str, Any]] = self.data.get("quirks", []) or self.data.get("errata", [])

    def has_quirk(self, quirk_id: str) -> bool:
        return any(q.get("id") == quirk_id for q in self.quirks)

    def supports_instruction(self, enc: OpcodeEncoding) -> bool:
        req_order = CPU_LEVEL_ORDER.get(enc.min_cpu, 0)
        target_order = CPU_LEVEL_ORDER.get(self.isa_level, 0)
        if target_order < req_order:
            return False

        feat = enc.required_feature
        if feat and feat not in self.features:
            return False

        return True


class RegisterDatabase:
    """Loads and provides access to declarative architectural register definitions."""
    def __init__(self):
        self.gprs: Dict[str, Any] = {}
        self.segments: Dict[str, Any] = {}
        self.flags: Dict[str, Any] = {}
        self.control: Dict[str, Any] = {}
        self.fpu: Dict[str, Any] = {}
        self.xmm: Dict[str, Any] = {}
        self.msr: Dict[str, Any] = {}
        self.load_all()

    def load_all(self):
        reg_files = {
            "gprs": os.path.join(REGISTERS_DIR, "gpr.yaml"),
            "segments": os.path.join(REGISTERS_DIR, "segments.yaml"),
            "flags": os.path.join(REGISTERS_DIR, "flags.yaml"),
            "control": os.path.join(REGISTERS_DIR, "control_and_system.yaml"),
            "fpu": os.path.join(REGISTERS_DIR, "fpu_and_mmx.yaml"),
            "xmm": os.path.join(REGISTERS_DIR, "simd_xmm.yaml"),
            "msr": os.path.join(REGISTERS_DIR, "msr.yaml"),
        }
        for k, p in reg_files.items():
            if os.path.isfile(p):
                with open(p, "r", encoding="utf-8") as fp:
                    setattr(self, k, yaml.safe_load(fp))

    def cpu_registers(self, cpu: CPUManifest) -> Dict[str, List[str]]:
        order = CPU_LEVEL_ORDER.get(cpu.isa_level, 0)
        res = {}

        # GPRs
        gpr_list = []
        is_32 = order >= CPU_LEVEL_ORDER["CPU_386"]
        for name, data in self.gprs.get("registers", {}).items():
            if is_32:
                gpr_list.append(name)
            else:
                for sub in data.get("sub_registers", {}):
                    if len(sub) == 2:
                        gpr_list.append(sub)
        res["General Purpose"] = gpr_list

        # Segments
        segs = []
        for name, data in self.segments.get("segments", {}).items():
            req = CPU_LEVEL_ORDER.get(data.get("min_cpu", "CPU_8086"), 0)
            if order >= req:
                segs.append(name)
        res["Segments"] = segs

        # Control Registers
        crs = []
        for name, data in self.control.get("control_registers", {}).items():
            req = CPU_LEVEL_ORDER.get(data.get("min_cpu", "CPU_386"), 0)
            if order >= req:
                crs.append(name)
        res["Control Registers"] = crs

        # FPU / MMX
        fpu_list = []
        if "CPU_FEATURE_FPU" in cpu.features or order == 0:
            fpu_list.extend(["ST(0)..ST(7)", "FCW", "FSW", "FTW"])
        if "CPU_FEATURE_MMX" in cpu.features:
            fpu_list.append("MM0..MM7 (aliased)")
        res["x87 FPU & MMX"] = fpu_list

        # XMM
        xmms = []
        if "CPU_FEATURE_SSE" in cpu.features:
            xmms.append("XMM0..XMM7 (128-bit)")
            xmms.append("MXCSR")
        res["Vector SIMD (XMM)"] = xmms

        return res


class FPUDatabase:
    """Loads and provides access to declarative FPU and math coprocessor specifications."""
    def __init__(self):
        self.discrete: Dict[str, Dict[str, Any]] = {}
        self.integrated: Dict[str, Dict[str, Any]] = {}
        self.by_id: Dict[str, Dict[str, Any]] = {}
        self.by_filename: Dict[str, Dict[str, Any]] = {}
        self.load_all()

    def load_all(self):
        for sub, cat, target in (("discrete", "DISCRETE", self.discrete), ("integrated", "INTEGRATED", self.integrated)):
            pattern = os.path.join(FPUS_DIR, sub, "*.yaml")
            for f in sorted(glob.glob(pattern)):
                with open(f, "r", encoding="utf-8") as fp:
                    data = yaml.safe_load(fp)
                if not data or "fpu" not in data:
                    continue
                fpu_id = data["fpu"]["fpu_id"]
                data["_file"] = f
                data["_category"] = cat
                target[fpu_id] = data
                self.by_id[fpu_id] = data
                base = os.path.splitext(os.path.basename(f))[0]
                self.by_filename[base] = data

    def get_fpu(self, identifier: str) -> Optional[Dict[str, Any]]:
        if not identifier:
            return None
        if identifier in self.by_id:
            return self.by_id[identifier]
        base = os.path.splitext(os.path.basename(identifier))[0]
        if base in self.by_filename:
            return self.by_filename[base]
        ident_lower = identifier.lower()
        for k, v in self.by_filename.items():
            if ident_lower in k.lower():
                return v
        for k, v in self.by_id.items():
            if ident_lower in k.lower():
                return v
        return None


def derive_dependencies(enc: OpcodeEncoding, opcode: int, is_mod3: bool) -> int:
    """Auto-synthesizes dependency bitmask for an opcode encoding."""
    cls = enc.class_name
    if cls in ("nop", "string_rep", "sys_interrupt", "bounds_check", "arpl"):
        return 0
    if cls == "xlat":
        return SRCDEP_EAX | SRCDEP_EBX
    if cls == "salc":
        return SRCDEP_EAX

    odata = enc.op_data
    if cls == "ret_near" and odata.get("pop_bytes"):
        return SRCDEP_ESP | DSTDEP_ESP
    if cls == "ret_far":
        return IMPL_ESP

    flags = 0

    dst = odata.get("dst")
    src = odata.get("src")
    op1 = odata.get("op1")
    op2 = odata.get("op2")
    bounds = odata.get("bounds")
    base = odata.get("base")
    port = odata.get("port")

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
    if port:
        src_list.extend(flatten(port))

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
    elif cls in ("call_near", "ret_near"):
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
        if r in ("Eb", "Ev", "Ew", "Ed"):
            flags |= MODRM
            if is_mod3:
                flags |= SRCDEP_RM
        elif r in ("Mp", "Ma"):
            flags |= MODRM
        elif r in ("Gb", "Gv", "Gw", "Gd"):
            flags |= MODRM | SRCDEP_REG
        elif r in ("Pq", "Qq", "Vps", "Wps", "Vpd", "Wpd"):
            flags |= MODRM
            if r in ("Gb", "Gv", "Pq", "Vps", "Vpd"):
                flags |= SRCDEP_REG
        elif r in ("AL", "AX", "eAX", "AH"):
            flags |= SRCDEP_EAX
        elif r in ("CL", "CH", "CX", "eCX"):
            flags |= SRCDEP_ECX
        elif r in ("DL", "DH", "DX", "eDX"):
            flags |= SRCDEP_EDX
        elif r in ("BL", "BH", "BX", "eBX"):
            flags |= SRCDEP_EBX
        elif "gpr8" in r and "[reg]" in r:
            reg_idx = (opcode & 7) & 3
            flags |= (1 << (SRCDEP_SHIFT + reg_idx))
        elif "gpr" in r and "[reg]" in r:
            reg_idx = opcode & 7
            flags |= (1 << (SRCDEP_SHIFT + reg_idx))
        elif r == "gpr32[edx]":
            flags |= SRCDEP_EDX
        elif r in ("Ib", "Ib_sext"):
            if opcode not in (0x6B, 0xA8, 0xD4, 0xD5, 0xE4, 0xE5, 0xE6, 0xE7):
                flags |= HAS_IMM8
        elif r in ("Iv",):
            if opcode not in (0x69, 0xA9):
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
        elif w in ("AL", "AX", "eAX", "AH"):
            flags |= DSTDEP_EAX
        elif w in ("CL", "CH", "CX", "eCX"):
            flags |= DSTDEP_ECX
        elif w in ("DL", "DH", "DX", "eDX"):
            flags |= DSTDEP_EDX
        elif w in ("BL", "BH", "BX", "eBX"):
            flags |= DSTDEP_EBX
        elif "gpr8" in w and "[reg]" in w:
            reg_idx = (opcode & 7) & 3
            flags |= (1 << (DSTDEP_SHIFT + reg_idx))
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
        parent = os.path.dirname(f)
        if parent in (ISA_DIR, TIMINGS_DIR, REGISTERS_DIR, FPUS_DIR) or "fpus" in f:
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


def list_fpus(fpu_db: FPUDatabase):
    """Lists all discrete and integrated FPUs in a formatted table."""
    all_fpus = list(fpu_db.by_id.values())
    all_fpus.sort(key=lambda d: (d.get("fpu", {}).get("release_year") or 9999, d.get("_category", ""), d.get("fpu", {}).get("name", "")))

    print(f"{'Year':<6} {'Category':<12} {'FPU ID':<24} {'Name':<36} {'Interface Protocol':<26}")
    print("-" * 106)
    for data in all_fpus:
        finfo = data.get("fpu", {})
        inter = data.get("interface", {})
        year = str(finfo.get("release_year", "N/A"))
        cat = data.get("_category", "FPU")
        fid = finfo.get("fpu_id", "")
        name = finfo.get("name", "")
        proto = inter.get("protocol", "Standard")
        print(f"{year:<6} {cat:<12} {fid:<24} {name:<36} {proto:<26}")


def inspect_fpu(identifier: str, fpu_db: FPUDatabase):
    """Inspects detailed specifications for a specific FPU or coprocessor."""
    data = fpu_db.get_fpu(identifier)
    if not data:
        print(f"Error: FPU or coprocessor '{identifier}' not found")
        return

    finfo = data.get("fpu", {})
    inter = data.get("interface", {})
    std = data.get("standard", {})
    cycles = data.get("typical_cycles", {})
    pipe = data.get("pipeline", {})
    regs = data.get("registers", {})
    arch = data.get("architecture", "").strip()

    print("=" * 72)
    print(f"FPU / Coprocessor Specification: {finfo.get('name')}")
    print(f"ID: {finfo.get('fpu_id')} | Vendor: {finfo.get('vendor')} | Year: {finfo.get('release_year', 'N/A')}")
    pkg_str = ", ".join(finfo.get('package', [])) if isinstance(finfo.get('package'), list) else finfo.get('package', 'N/A')
    print(f"Category: {data.get('_category')} | Package: {pkg_str}")
    print("=" * 72)

    print("Hardware Interface:")
    print(f"  Protocol:         {inter.get('protocol')}")
    if "bus_width_bits" in inter:
        print(f"  Bus Width:        {inter.get('bus_width_bits')} bits")
    if "clock_divider" in inter:
        print(f"  Clock Multiplier: {inter.get('clock_divider')}x core clock")
    if "address_space_size" in inter:
        print(f"  MMIO Window:      {hex(inter.get('address_space_size'))} at {hex(inter.get('default_base_physical', 0))}")

    print("\nMathematical Standard:")
    print(f"  IEEE 754:         {std.get('ieee_754_version', 'Standard')}")
    print(f"  Infinity Mode:    {std.get('infinity_mode', 'Affine')}")

    if regs:
        print("\nRegister Architecture:")
        if "stack_depth" in regs:
            print(f"  Stack Depth:      {regs.get('stack_depth')} entries x {regs.get('register_width_bits', 80)}-bit")
        if "register_banks" in regs:
            print(f"  Bank Switching:   {regs.get('register_banks')} banks ({regs.get('total_registers')} total registers)")
        if "single_precision_registers" in regs:
            sp = regs['single_precision_registers']
            dp = regs.get('double_precision_registers', {})
            print(f"  Flat Registers:   {sp.get('count')} single ({sp.get('naming')}) / {dp.get('count')} double")

    if cycles:
        print("\nTypical Instruction Cycles:")
        cyc_str = ", ".join(f"{k.upper()}={v}c" for k, v in cycles.items() if v is not None)
        print(f"  {cyc_str}")

    if pipe:
        print("\nPipeline Architecture:")
        for pk, pv in pipe.items():
            if isinstance(pv, dict):
                print(f"  {pk}: {list(pv.keys())}")
            elif isinstance(pv, list):
                print(f"  {pk}: {len(pv)} stages")
            else:
                print(f"  {pk}: {pv}")

    if arch:
        print(f"\nArchitecture Summary:\n  {arch}")
    print("=" * 72)


def inspect_cpu(manifest_path: str, isa: ISADatabase, reg_db: Optional[RegisterDatabase] = None, fpu_db: Optional[FPUDatabase] = None):
    """Analyzes instruction set compatibility, architectural registers, FPU capabilities, and generates #UD table for a CPU."""
    if not os.path.isfile(manifest_path):
        exact = glob.glob(os.path.join(SCRIPT_DIR, f"*/{manifest_path}.yaml"))
        candidates = glob.glob(os.path.join(SCRIPT_DIR, f"*/{manifest_path}*"))
        if exact:
            manifest_path = exact[0]
        elif candidates:
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

    if cpu.cpu_frequencies_mhz or cpu.default_emulated_frequencies_mhz or cpu.emulated_frequency_range_mhz:
        print("Clock Frequencies:")
        if cpu.cpu_frequencies_mhz:
            print(f"  Official Factory Speeds: {', '.join(str(f) for f in cpu.cpu_frequencies_mhz)} MHz")
        if cpu.emulated_frequency_range_mhz:
            print(f"  Emulated Range:          {cpu.emulated_frequency_range_mhz[0]} - {cpu.emulated_frequency_range_mhz[1]} MHz")
        if cpu.default_emulated_frequencies_mhz:
            print(f"  86Box Emulation Speeds:  {', '.join(str(f) for f in cpu.default_emulated_frequencies_mhz)} MHz")
        print("-" * 72)

    if reg_db:
        reg_map = reg_db.cpu_registers(cpu)
        print("Architectural Registers:")
        for grp, rlist in reg_map.items():
            if rlist:
                print(f"  {grp:<20} {', '.join(rlist)}")
        print("-" * 72)

    if fpu_db:
        if cpu.fpu_profile:
            fpu_data = fpu_db.get_fpu(cpu.fpu_profile)
            if fpu_data:
                finfo = fpu_data.get("fpu", {})
                inter = fpu_data.get("interface", {})
                cycles = fpu_data.get("typical_cycles", {})
                pipe = fpu_data.get("pipeline", {})
                print("Integrated Floating-Point Unit:")
                print(f"  Model:            {finfo.get('name')} ({finfo.get('release_year', 'N/A')})")
                print(f"  Protocol:         {inter.get('protocol')} | Clock Multiplier: {inter.get('clock_divider', 1.0)}x")
                if pipe:
                    ptype = pipe.get("type", "Standard")
                    stages = pipe.get("stages", "N/A")
                    print(f"  Pipeline:         {ptype} (Stages: {stages})")
                fadd_c = cycles.get("fadd", "N/A")
                fmul_c = cycles.get("fmul", "N/A")
                fdiv_c = cycles.get("fdiv", "N/A")
                fxch_c = cycles.get("fxch", "N/A")
                print(f"  Typical Cycles:   FADD: {fadd_c} | FMUL: {fmul_c} | FDIV: {fdiv_c} | FXCH: {fxch_c}")
                print("-" * 72)
        if cpu.supported_coprocessors:
            print(f"Supported Math Coprocessors ({len(cpu.supported_coprocessors)} compatible):")
            for coproc_ref in cpu.supported_coprocessors:
                cdata = fpu_db.get_fpu(coproc_ref)
                if cdata:
                    cinfo = cdata.get("fpu", {})
                    cinter = cdata.get("interface", {})
                    ccycles = cdata.get("typical_cycles", {})
                    proto = cinter.get("protocol", "Standard")
                    fadd_c = ccycles.get("fadd", "N/A")
                    fmul_c = ccycles.get("fmul", "N/A")
                    print(f"  * {cinfo.get('name', coproc_ref):<38} [{proto}] (FADD: {fadd_c}c, FMUL: {fmul_c}c)")
            print("-" * 72)

    combined_quirks = []
    seen_ids = set()
    for q in cpu.quirks:
        qid = q.get("id")
        if qid and qid not in seen_ids:
            seen_ids.add(qid)
            combined_quirks.append(q)
    if fpu_db and cpu.fpu_profile:
        fpu_data = fpu_db.get_fpu(cpu.fpu_profile)
        if fpu_data and "quirks" in fpu_data:
            for q in fpu_data["quirks"]:
                qid = q.get("id")
                if qid and qid not in seen_ids:
                    seen_ids.add(qid)
                    combined_quirks.append(q)

    if combined_quirks:
        print(f"Silicon Errata & Architectural Quirks ({len(combined_quirks)} documented):")
        for q in combined_quirks:
            qid = q.get("id", "ERRATUM")
            qname = q.get("name", "Unknown Erratum")
            qcve = f" [{q['cve']}]" if "cve" in q else ""
            qcat = q.get("category", "")
            print(f"  * {qid:<30} {qname}{qcve}")
            if qcat:
                print(f"    Category:   {qcat}")
            if "trigger" in q:
                print(f"    Trigger:    {q['trigger']}")
            if "behavior" in q:
                btype = q["behavior"].get("type", "N/A")
                baction = q["behavior"].get("synthesized_action", "")
                action_str = f" -> {baction}" if baction else ""
                print(f"    Behavior:   {btype}{action_str}")
            if "steppings_affected" in q:
                print(f"    Steppings:  {q['steppings_affected']}")
            if "fixed_in" in q:
                print(f"    Fixed In:   {q['fixed_in']}")
        print("-" * 72)

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


def list_groups(isa: ISADatabase):
    """Displays all ModR/M group tables and their extension slots."""
    if not isa.modrm_groups:
        print("No modrm_groups found in types_and_operands.yaml")
        return

    print("=" * 78)
    print("ModR/M Group Extension Architecture (x86 Metacompiler)")
    print("=" * 78)

    for g_id, g_data in sorted(isa.modrm_groups.items()):
        desc = g_data.get("desc", g_id)
        opcodes = g_data.get("opcodes", {})
        slots = g_data.get("slots", {})

        opc_summary = []
        for opc, oinfo in sorted(opcodes.items()):
            sz = oinfo.get("size")
            sz_str = f"{sz}-bit" if isinstance(sz, int) else f"{sz[0]}/{sz[1]}-bit" if isinstance(sz, list) else "var"
            imm = oinfo.get("imm") or oinfo.get("cnt")
            imm_str = f" + {imm}" if imm else ""
            opc_summary.append(f"{opc} ({sz_str}{imm_str})")

        print(f"\n{g_id.upper()}: {desc}")
        print(f"  Opcodes: {', '.join(opc_summary)}")
        print(f"  {'Slot':<6} {'Mnemonic':<14} {'Action':<18} {'Operation / Lowering':<26}")
        print("  " + "-" * 70)

        for slot_idx in range(8):
            if slot_idx in slots:
                s_info = slots[slot_idx]
                name = s_info.get("name", "")
                act = s_info.get("action", "")
                op_prim = s_info.get("op", "")
                h_fn = s_info.get("helper_fn", "")
                detail = h_fn if h_fn else op_prim
                if s_info.get("carry_in"):
                    detail += " (CF in)"
                print(f"  /{slot_idx:<5} {name:<14} {act:<18} {detail:<26}")
            else:
                print(f"  /{slot_idx:<5} {'[Unused / #UD]':<14} {'-':<18} {'-':<26}")
    print("=" * 78)


def inspect_semantics(mnemonic: Optional[str], isa: ISADatabase):
    """Displays formal micro-semantics and flag update rules for instruction families."""
    if not mnemonic or mnemonic.lower() == "all":
        print(f"{'Mnemonic':<14} {'Class':<16} {'Action':<18} {'Availability':<14} {'Flags Tag':<16}")
        print("-" * 78)
        for fname, fdef in sorted(isa.families.items()):
            cls = fdef.get("class", "alu")
            sem = fdef.get("semantics", {})
            act = sem.get("action", "CUSTOM")
            avail = fdef.get("availability", {}).get("min_cpu", "CPU_8086")
            flg = sem.get("flags", {})
            flg_tag = flg.get("tag", "-") if isinstance(flg, dict) else "-"
            print(f"{fname:<14} {cls:<16} {act:<18} {avail:<14} {flg_tag:<16}")
        print(f"\nTotal Instruction Families: {len(isa.families)}")
        return

    m_upper = mnemonic.upper()
    fdef = isa.families.get(m_upper)
    if not fdef:
        matches = [k for k in isa.families if m_upper in k]
        if matches:
            fdef = isa.families[matches[0]]
            m_upper = matches[0]
        else:
            print(f"Error: Instruction family '{mnemonic}' not found in ISA database")
            return

    sem = fdef.get("semantics", {})
    avail = fdef.get("availability", {})
    cls = fdef.get("class", "alu")
    opmap = fdef.get("opcode_map", {})

    print("=" * 78)
    print(f"Instruction Micro-Semantics: {m_upper}")
    print(f"Description:  {fdef.get('description', 'N/A')}")
    print(f"Architecture: Class: {cls} | Min CPU: {avail.get('min_cpu', 'CPU_8086')}")
    if "feature" in avail:
        print(f"Requirement:  {avail['feature']}")
    print("=" * 78)

    print("Formal Semantics:")
    print(f"  Action:        {sem.get('action', 'N/A')}")
    if "op" in sem:
        print(f"  Primitive Op:  {sem['op']}")
    if "carry" in sem:
        print(f"  Carry Policy:  {sem['carry']}")
    if "inputs" in sem:
        print(f"  Inputs:        {', '.join(str(x) for x in sem['inputs'])}")
    if "output" in sem:
        print(f"  Output:        {sem['output']}")
    if "fault_policy" in sem:
        print(f"  Fault Policy:  {sem['fault_policy']}")

    flg = sem.get("flags")
    if isinstance(flg, dict):
        print("\nFlag Behavior:")
        print(f"  Tag:           {flg.get('tag', 'None')}")
        if "update" in flg:
            print(f"  Updated:       {', '.join(flg['update'])}")
        if "preserve" in flg:
            print(f"  Preserved:     {', '.join(flg['preserve'])}")
        if "forced_clear" in flg:
            print(f"  Forced Clear:  {', '.join(flg['forced_clear'])}")
        if "undefined" in flg:
            print(f"  Undefined:     {', '.join(flg['undefined'])}")
        if "notes" in flg:
            print(f"  Notes:         {flg['notes']}")
    elif flg == "none":
        print("\nFlag Behavior:   Flags unaffected")

    print(f"\nOpcode Encodings ({len(opmap)} patterns):")
    for op_str, odata in sorted(opmap.items()):
        grp = odata.get("group")
        grp_str = f" [Group: {grp} /{odata.get('group_ext')}]" if grp else ""
        operands = ", ".join(f"{k}: {v}" for k, v in odata.items() if k not in ("group", "group_ext", "min_cpu"))
        print(f"  {op_str:<16} -> {operands}{grp_str}")
    print("=" * 78)


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


def emit_jit_ops(output_path: str, isa: ISADatabase):
    """Synthesizes C Dynamic Recompiler (JIT) instruction handlers from declarative semantics."""
    lines = []
    lines.append("/* Auto-generated dynamic recompiler instruction handlers by x86gen.py - Do not edit manually */")
    lines.append("#include <stdint.h>")
    lines.append("#include <86box/86box.h>")
    lines.append('#include "cpu.h"')
    lines.append('#include <86box/mem.h>')
    lines.append('#include <86box/plat_unused.h>')
    lines.append("")
    lines.append('#include "codegen.h"')
    lines.append('#include "codegen_ir.h"')
    lines.append('#include "codegen_ops.h"')
    lines.append("")
    lines.append("static inline void")
    lines.append("get_cf(ir_data_t *ir, int dest_reg)")
    lines.append("{")
    lines.append("    uop_CALL_FUNC_RESULT(ir, dest_reg, jit_CF_SET);")
    lines.append("}")
    lines.append("")

    UOP_MAP = {
        "ADD": "uop_ADD",
        "ADC": "uop_ADD",
        "SUB": "uop_SUB",
        "SBB": "uop_SUB",
        "AND": "uop_AND",
        "OR": "uop_OR",
        "XOR": "uop_XOR",
    }

    for fname, fdef in sorted(isa.families.items()):
        sem = fdef.get("semantics", {})
        action = sem.get("action")
        if action == "ALU_BINARY":
            op_prim = sem.get("op", fname)
            is_carry = (op_prim in ("ADC", "SBB"))
            uop_prim = "uop_ADD" if op_prim in ("ADD", "ADC") else "uop_SUB" if op_prim in ("SUB", "SBB") else UOP_MAP.get(op_prim, f"uop_{op_prim}")
            carry_op = "uop_ADD" if op_prim == "ADC" else "uop_SUB" if op_prim == "SBB" else None
            flg_tag = sem.get("flags", {}).get("tag", f"FLAGS_{op_prim}")

            for suffix, sz_name, ireg_fn, temp0, temp1, flg_const, is_32 in [
                ("b_rmw", "8-bit RMW", "IREG_8", "IREG_temp0_B", "IREG_temp1_B", f"{flg_tag}8", False),
                ("w_rmw", "16-bit RMW", "IREG_16", "IREG_temp0_W", "IREG_temp1_W", f"{flg_tag}16", False),
                ("l_rmw", "32-bit RMW", "IREG_32", "IREG_temp0", "IREG_temp1", f"{flg_tag}32", True),
            ]:
                mov_flags = "uop_MOV" if is_32 else "uop_MOVZX"
                lines.append(f"/* Synthesized JIT Handler: {fname} {sz_name} */")
                lines.append(f"uint32_t gen_rop{fname}_{suffix}(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)")
                lines.append("{")
                lines.append("    int src_reg = (fetchdat >> 3) & 7;")
                lines.append("    codegen_mark_code_present(block, cs + op_pc, 1);")
                if is_carry:
                    lines.append("    get_cf(ir, IREG_temp2);")
                lines.append("    if ((fetchdat & 0xc0) == 0xc0) {")
                lines.append("        int dest_reg = fetchdat & 7;")
                lines.append(f"        {mov_flags}(ir, IREG_flags_op1, {ireg_fn}(dest_reg));")
                lines.append(f"        {mov_flags}(ir, IREG_flags_op2, {ireg_fn}(src_reg));")
                lines.append(f"        {uop_prim}(ir, {ireg_fn}(dest_reg), {ireg_fn}(dest_reg), {ireg_fn}(src_reg));")
                if is_carry:
                    lines.append(f"        {carry_op}(ir, {ireg_fn}(dest_reg), {ireg_fn}(dest_reg), IREG_temp2_B);")
                lines.append(f"        {mov_flags}(ir, IREG_flags_res, {ireg_fn}(dest_reg));")
                lines.append("    } else {")
                lines.append("        x86seg *target_seg;")
                lines.append("        uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);")
                lines.append("        target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);")
                lines.append("        codegen_check_seg_write(block, ir, target_seg);")
                lines.append(f"        uop_MEM_LOAD_REG(ir, {temp0}, ireg_seg_base(target_seg), IREG_eaaddr);")
                lines.append(f"        {uop_prim}(ir, {temp1}, {temp0}, {ireg_fn}(src_reg));")
                if is_carry:
                    lines.append(f"        {carry_op}(ir, {temp1}, {temp1}, IREG_temp2_B);")
                lines.append(f"        uop_MEM_STORE_REG(ir, ireg_seg_base(target_seg), IREG_eaaddr, {temp1});")
                lines.append(f"        {mov_flags}(ir, IREG_flags_op1, {temp0});")
                lines.append(f"        {mov_flags}(ir, IREG_flags_op2, {ireg_fn}(src_reg));")
                lines.append(f"        {mov_flags}(ir, IREG_flags_res, {temp1});")
                lines.append("    }")
                lines.append(f"    uop_MOV_IMM(ir, IREG_flags_op, {flg_const});")
                lines.append("    codegen_flags_changed = 1;")
                lines.append("    return op_pc + 1;")
                lines.append("}")
                lines.append("")

            for suffix, sz_name, ireg_fn, temp0, flg_const, is_32 in [
                ("b_rm", "8-bit RM", "IREG_8", "IREG_temp0_B", f"{flg_tag}8", False),
                ("w_rm", "16-bit RM", "IREG_16", "IREG_temp0_W", f"{flg_tag}16", False),
                ("l_rm", "32-bit RM", "IREG_32", "IREG_temp0", f"{flg_tag}32", True),
            ]:
                mov_flags = "uop_MOV" if is_32 else "uop_MOVZX"
                lines.append(f"/* Synthesized JIT Handler: {fname} {sz_name} */")
                lines.append(f"uint32_t gen_rop{fname}_{suffix}(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)")
                lines.append("{")
                lines.append("    int dest_reg = (fetchdat >> 3) & 7;")
                lines.append("    codegen_mark_code_present(block, cs + op_pc, 1);")
                if is_carry:
                    lines.append("    get_cf(ir, IREG_temp2);")
                lines.append("    if ((fetchdat & 0xc0) == 0xc0) {")
                lines.append("        int src_reg = fetchdat & 7;")
                lines.append(f"        {mov_flags}(ir, IREG_flags_op1, {ireg_fn}(dest_reg));")
                lines.append(f"        {mov_flags}(ir, IREG_flags_op2, {ireg_fn}(src_reg));")
                lines.append(f"        {uop_prim}(ir, {ireg_fn}(dest_reg), {ireg_fn}(dest_reg), {ireg_fn}(src_reg));")
                if is_carry:
                    lines.append(f"        {carry_op}(ir, {ireg_fn}(dest_reg), {ireg_fn}(dest_reg), IREG_temp2_B);")
                lines.append(f"        {mov_flags}(ir, IREG_flags_res, {ireg_fn}(dest_reg));")
                lines.append("    } else {")
                lines.append("        x86seg *target_seg;")
                lines.append("        uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);")
                lines.append("        target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);")
                lines.append("        codegen_check_seg_read(block, ir, target_seg);")
                lines.append(f"        uop_MEM_LOAD_REG(ir, {temp0}, ireg_seg_base(target_seg), IREG_eaaddr);")
                lines.append(f"        {mov_flags}(ir, IREG_flags_op1, {ireg_fn}(dest_reg));")
                lines.append(f"        {mov_flags}(ir, IREG_flags_op2, {temp0});")
                lines.append(f"        {uop_prim}(ir, {ireg_fn}(dest_reg), {ireg_fn}(dest_reg), {temp0});")
                if is_carry:
                    lines.append(f"        {carry_op}(ir, {ireg_fn}(dest_reg), {ireg_fn}(dest_reg), IREG_temp2_B);")
                lines.append(f"        {mov_flags}(ir, IREG_flags_res, {ireg_fn}(dest_reg));")
                lines.append("    }")
                lines.append(f"    uop_MOV_IMM(ir, IREG_flags_op, {flg_const});")
                lines.append("    codegen_flags_changed = 1;")
                lines.append("    return op_pc + 1;")
                lines.append("}")
                lines.append("")

        elif action == "ALU_UNARY":
            op_prim = sem.get("op", fname)
            if op_prim == "NOT":
                for suffix, sz_name, ireg_fn, temp0, is_32, mask_val in [
                    ("b_rm", "8-bit NOT", "IREG_8", "IREG_temp0_B", False, "0xff"),
                    ("w_rm", "16-bit NOT", "IREG_16", "IREG_temp0_W", False, "0xffff"),
                    ("l_rm", "32-bit NOT", "IREG_32", "IREG_temp0", True, "0xffffffff"),
                ]:
                    lines.append(f"/* Synthesized JIT Handler: NOT {sz_name} */")
                    lines.append(f"uint32_t gen_ropNOT_{suffix}(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)")
                    lines.append("{")
                    lines.append("    codegen_mark_code_present(block, cs + op_pc, 1);")
                    lines.append("    if ((fetchdat & 0xc0) == 0xc0) {")
                    lines.append("        int reg = fetchdat & 7;")
                    lines.append(f"        uop_XOR_IMM(ir, {ireg_fn}(reg), {ireg_fn}(reg), {mask_val});")
                    lines.append("    } else {")
                    lines.append("        x86seg *target_seg;")
                    lines.append("        uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);")
                    lines.append("        target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);")
                    lines.append("        codegen_check_seg_write(block, ir, target_seg);")
                    lines.append(f"        uop_MEM_LOAD_REG(ir, {temp0}, ireg_seg_base(target_seg), IREG_eaaddr);")
                    lines.append(f"        uop_XOR_IMM(ir, {temp0}, {temp0}, {mask_val});")
                    lines.append(f"        uop_MEM_STORE_REG(ir, ireg_seg_base(target_seg), IREG_eaaddr, {temp0});")
                    lines.append("    }")
                    lines.append("    codegen_flags_changed = 1;")
                    lines.append("    return op_pc + 1;")
                    lines.append("}")
                    lines.append("")
            elif op_prim == "NEG":
                for suffix, sz_name, ireg_fn, temp0, temp1, flg_const, is_32 in [
                    ("b_rm", "8-bit NEG", "IREG_8", "IREG_temp0_B", "IREG_temp1_B", "FLAGS_SUB8", False),
                    ("w_rm", "16-bit NEG", "IREG_16", "IREG_temp0_W", "IREG_temp1_W", "FLAGS_SUB16", False),
                    ("l_rm", "32-bit NEG", "IREG_32", "IREG_temp0", "IREG_temp1", "FLAGS_SUB32", True),
                ]:
                    mov_flags = "uop_MOV" if is_32 else "uop_MOVZX"
                    lines.append(f"/* Synthesized JIT Handler: NEG {sz_name} */")
                    lines.append(f"uint32_t gen_ropNEG_{suffix}(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)")
                    lines.append("{")
                    lines.append("    codegen_mark_code_present(block, cs + op_pc, 1);")
                    lines.append(f"    uop_MOV_IMM(ir, {temp1}, 0);")
                    lines.append("    if ((fetchdat & 0xc0) == 0xc0) {")
                    lines.append("        int reg = fetchdat & 7;")
                    lines.append(f"        {mov_flags}(ir, IREG_flags_op2, {ireg_fn}(reg));")
                    lines.append(f"        uop_SUB(ir, {temp1}, {temp1}, {ireg_fn}(reg));")
                    lines.append(f"        {mov_flags}(ir, IREG_flags_res, {temp1});")
                    lines.append(f"        uop_MOV(ir, {ireg_fn}(reg), {temp1});")
                    lines.append("    } else {")
                    lines.append("        x86seg *target_seg;")
                    lines.append("        uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);")
                    lines.append("        target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);")
                    lines.append("        codegen_check_seg_write(block, ir, target_seg);")
                    lines.append(f"        uop_MEM_LOAD_REG(ir, {temp0}, ireg_seg_base(target_seg), IREG_eaaddr);")
                    lines.append(f"        uop_SUB(ir, {temp1}, {temp1}, {temp0});")
                    lines.append(f"        uop_MEM_STORE_REG(ir, ireg_seg_base(target_seg), IREG_eaaddr, {temp1});")
                    lines.append(f"        {mov_flags}(ir, IREG_flags_op2, {temp0});")
                    lines.append(f"        {mov_flags}(ir, IREG_flags_res, {temp1});")
                    lines.append("    }")
                    lines.append(f"    uop_MOV_IMM(ir, IREG_flags_op, {flg_const});")
                    lines.append("    uop_MOV_IMM(ir, IREG_flags_op1, 0);")
                    lines.append("    codegen_flags_changed = 1;")
                    lines.append("    return op_pc + 1;")
                    lines.append("}")
                    lines.append("")

        elif action == "ALU_COMPARE":
            op_prim = sem.get("op", "SUB")
            uop_fn = UOP_MAP.get(op_prim, f"uop_{op_prim}")
            flg_tag = sem.get("flags", {}).get("tag", "FLAGS_SUB")
            for suffix, sz_name, ireg_fn, temp0, temp1, flg_const, is_32 in [
                ("b_rmw", "8-bit Compare", "IREG_8", "IREG_temp0_B", "IREG_temp1_B", f"{flg_tag}8", False),
                ("w_rmw", "16-bit Compare", "IREG_16", "IREG_temp0_W", "IREG_temp1_W", f"{flg_tag}16", False),
                ("l_rmw", "32-bit Compare", "IREG_32", "IREG_temp0", "IREG_temp1", f"{flg_tag}32", True),
            ]:
                mov_flags = "uop_MOV" if is_32 else "uop_MOVZX"
                lines.append(f"/* Synthesized JIT Handler: {fname} {sz_name} (read-only compare) */")
                lines.append(f"uint32_t gen_rop{fname}_{suffix}(codeblock_t *block, ir_data_t *ir, UNUSED(uint8_t opcode), uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)")
                lines.append("{")
                lines.append("    int src_reg = (fetchdat >> 3) & 7;")
                lines.append("    codegen_mark_code_present(block, cs + op_pc, 1);")
                lines.append("    if ((fetchdat & 0xc0) == 0xc0) {")
                lines.append("        int dest_reg = fetchdat & 7;")
                lines.append(f"        {mov_flags}(ir, IREG_flags_op1, {ireg_fn}(dest_reg));")
                lines.append(f"        {mov_flags}(ir, IREG_flags_op2, {ireg_fn}(src_reg));")
                lines.append(f"        {uop_fn}(ir, IREG_temp0, {ireg_fn}(dest_reg), {ireg_fn}(src_reg));")
                lines.append(f"        {mov_flags}(ir, IREG_flags_res, IREG_temp0);")
                lines.append("    } else {")
                lines.append("        x86seg *target_seg;")
                lines.append("        uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);")
                lines.append("        target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);")
                lines.append("        codegen_check_seg_read(block, ir, target_seg);")
                lines.append(f"        uop_MEM_LOAD_REG(ir, {temp0}, ireg_seg_base(target_seg), IREG_eaaddr);")
                lines.append(f"        {mov_flags}(ir, IREG_flags_op1, {temp0});")
                lines.append(f"        {mov_flags}(ir, IREG_flags_op2, {ireg_fn}(src_reg));")
                lines.append(f"        {uop_fn}(ir, {temp1}, {temp0}, {ireg_fn}(src_reg));")
                lines.append(f"        {mov_flags}(ir, IREG_flags_res, {temp1});")
                lines.append("    }")
                lines.append(f"    uop_MOV_IMM(ir, IREG_flags_op, {flg_const});")
                lines.append("    codegen_flags_changed = 1;")
                lines.append("    return op_pc + 1;")
                lines.append("}")
                lines.append("")

    with open(output_path, "w", encoding="utf-8") as fp:
        fp.write("\n".join(lines))
    print(f"Emitted JIT instruction handlers to {output_path}")


def emit_interpreter_ops(output_path: str, isa: ISADatabase):
    """Synthesizes C Interpreter instruction handlers from declarative semantics."""
    lines = []
    lines.append("/* Auto-generated interpreter instruction handlers by x86gen.py - Do not edit manually */")
    lines.append("#ifndef _X86_OPS_SYNTHESIZED_H_")
    lines.append("#define _X86_OPS_SYNTHESIZED_H_")
    lines.append("")

    C_OP_MAP = {
        "ADD": "+",
        "SUB": "-",
        "AND": "&",
        "OR": "|",
        "XOR": "^",
    }

    for fname, fdef in sorted(isa.families.items()):
        sem = fdef.get("semantics", {})
        action = sem.get("action")
        if action == "ALU_BINARY":
            c_op = C_OP_MAP.get(sem.get("op", fname), "+")
            for sz_suf, c_type, getr, setr, getea, setea, flg_fn in [
                ("b", "uint8_t", "getr8", "setr8", "geteab", "seteab", f"setflags_{fname.lower()}8"),
                ("w", "uint16_t", "getr16", "setr16", "getear", "setear", f"setflags_{fname.lower()}16"),
                ("l", "uint32_t", "getr32", "setr32", "geteal", "seteal", f"setflags_{fname.lower()}32"),
            ]:
                for addr_suf, fetch_ea in [("a16", "fetch_ea_16"), ("a32", "fetch_ea_32")]:
                    lines.append(f"static int gen_op{fname}_{sz_suf}_rmw_{addr_suf}(uint32_t fetchdat)")
                    lines.append("{")
                    lines.append(f"    {c_type} dst, src;")
                    lines.append(f"    {fetch_ea}(fetchdat);")
                    lines.append("    if (cpu_mod == 3) {")
                    lines.append(f"        dst = {getr}(cpu_rm);")
                    lines.append(f"        src = {getr}(cpu_reg);")
                    lines.append(f"        {flg_fn}(dst, src);")
                    lines.append(f"        {setr}(cpu_rm, dst {c_op} src);")
                    lines.append("        CLOCK_CYCLES(timing_rr);")
                    lines.append(f"        PREFETCH_RUN(timing_rr, 2, rmdat, 0, 0, 0, 0, {1 if addr_suf == 'a32' else 0});")
                    lines.append("    } else {")
                    lines.append("        SEG_CHECK_WRITE(cpu_state.ea_seg);")
                    lines.append(f"        dst = {getea}();")
                    lines.append("        if (cpu_state.abrt) return 1;")
                    lines.append(f"        src = {getr}(cpu_reg);")
                    lines.append(f"        {setea}(dst {c_op} src);")
                    lines.append("        if (cpu_state.abrt) return 1;")
                    lines.append(f"        {flg_fn}(dst, src);")
                    lines.append("        CLOCK_CYCLES(timing_mr);")
                    lines.append(f"        PREFETCH_RUN(timing_mr, 2, rmdat, 1, 0, 1, 0, {1 if addr_suf == 'a32' else 0});")
                    lines.append("    }")
                    lines.append("    return 0;")
                    lines.append("}")
                    lines.append("")

    lines.append("#endif /* _X86_OPS_SYNTHESIZED_H_ */")
    with open(output_path, "w", encoding="utf-8") as fp:
        fp.write("\n".join(lines))
    print(f"Emitted interpreter instruction handlers to {output_path}")


def list_all_quirks():
    """Aggregates and displays all hardware errata and silicon quirks defined across manifests."""
    cpu_files = sorted(glob.glob(os.path.join(SCRIPT_DIR, "*/*.yaml")))
    all_quirks = {}
    for cf in cpu_files:
        if any(skip in cf for skip in ["fpus", "isa", "registers", "timings"]):
            continue
        try:
            with open(cf, "r", encoding="utf-8") as fp:
                data = yaml.safe_load(fp)
        except Exception:
            continue
        cname = data.get("model", {}).get("name", os.path.basename(cf))
        qlist = data.get("quirks", []) or data.get("errata", [])
        for q in qlist:
            qid = q.get("id")
            if not qid:
                continue
            if qid not in all_quirks:
                all_quirks[qid] = {
                    "name": q.get("name", "Unknown"),
                    "category": q.get("category", "GENERAL"),
                    "cve": q.get("cve"),
                    "desc": q.get("description", "").strip().split("\n")[0],
                    "fixed_in": q.get("fixed_in"),
                    "cpus": []
                }
            all_quirks[qid]["cpus"].append(cname)

    print("=" * 80)
    print("Architectural Quirks and Silicon Errata Registry")
    print("=" * 80)
    for qid, q in sorted(all_quirks.items()):
        cve_str = f" [{q['cve']}]" if q["cve"] else ""
        print(f"* {qid}{cve_str}: {q['name']}")
        print(f"  Category:     {q['category']}")
        print(f"  Description:  {q['desc']}")
        if q["fixed_in"]:
            print(f"  Fixed In:     {q['fixed_in']}")
        print(f"  Affected:     {', '.join(q['cpus'])}")
        print()
    print("=" * 80)
    print(f"Total Unique Documented Errata: {len(all_quirks)}")


def emit_quirks_header(output_path: str):
    """Emits C header defining CPU errata/quirk bitmask constants and helper signatures."""
    cpu_files = sorted(glob.glob(os.path.join(SCRIPT_DIR, "*/*.yaml")))
    unique_quirks = {}
    for cf in cpu_files:
        try:
            with open(cf, "r", encoding="utf-8") as fp:
                data = yaml.safe_load(fp)
        except Exception:
            continue
        qlist = data.get("quirks", []) or data.get("errata", [])
        for q in qlist:
            qid = q.get("id")
            if qid and qid not in unique_quirks:
                unique_quirks[qid] = q

    lines = []
    lines.append("/* Auto-generated CPU quirks and errata definitions by x86gen.py */")
    lines.append("#ifndef CODEGEN_CPU_QUIRKS_H")
    lines.append("#define CODEGEN_CPU_QUIRKS_H")
    lines.append("")
    lines.append("#include <stdint.h>")
    lines.append("#include <stdbool.h>")
    lines.append("")
    lines.append("/* Architectural Quirks & Hardware Errata Flags */")
    lines.append("enum {")
    bit = 0
    for qid, q in sorted(unique_quirks.items()):
        cve = f" ({q['cve']})" if "cve" in q else ""
        lines.append(f"    {qid:<32} = (1ULL << {bit}), /* {q.get('name', '')}{cve} */")
        bit += 1
    lines.append("};")
    lines.append("")
    lines.append("/* Quirk query helper */")
    lines.append("static inline bool cpu_has_quirk(const void *cpu_ptr, uint64_t quirk_mask)")
    lines.append("{")
    lines.append("    extern uint64_t active_cpu_quirks;")
    lines.append("    (void)cpu_ptr;")
    lines.append("    return (active_cpu_quirks & quirk_mask) != 0;")
    lines.append("}")
    lines.append("")
    lines.append("/* Forward declaration of fault/freeze handlers */")
    lines.append("void cpu_bus_lock_freeze(void *cpu);")
    lines.append("void fpu_srt_fdiv_faulty(void *st0, void *st1);")
    lines.append("")
    lines.append("#endif /* CODEGEN_CPU_QUIRKS_H */")
    lines.append("")

    with open(output_path, "w", encoding="utf-8") as fp:
        fp.write("\n".join(lines))
    print(f"Emitted CPU quirks header to {output_path} ({len(unique_quirks)} errata constants)")


def main():
    parser = argparse.ArgumentParser(description="x86gen Declarative Architecture Compiler")
    parser.add_argument("--list-cpus", action="store_true", help="List all available CPU manifests")
    parser.add_argument("--cpu", type=str, help="Inspect instruction compatibility, registers, and FPU for a CPU manifest")
    parser.add_argument("--registers", type=str, nargs="?", const="all", help="Display architectural registers for a CPU (or all)")
    parser.add_argument("--fpus", action="store_true", help="List all available discrete and integrated FPU models")
    parser.add_argument("--fpu", type=str, help="Inspect detailed specification of an FPU or math coprocessor")
    parser.add_argument("--verify", action="store_true", help="Verify auto-derived tables against 86Box C codebase")
    parser.add_argument("--emit-deps-c", type=str, help="Emit C source containing opcode_deps tables")
    parser.add_argument("--emit-jit-ops", type=str, help="Emit C source containing JIT instruction handlers")
    parser.add_argument("--emit-interpreter-ops", type=str, help="Emit C header containing interpreter instruction handlers")
    parser.add_argument("--groups", action="store_true", help="List all ModR/M group tables and their extension slots")
    parser.add_argument("--semantics", type=str, nargs="?", const="all", help="Inspect formal micro-semantics for an instruction family (or all)")
    parser.add_argument("--quirks", action="store_true", help="List all declared hardware errata and quirks across all CPUs")
    parser.add_argument("--emit-quirks-header", type=str, help="Emit C header defining CPU errata/quirk bitmask constants and helpers")
    parser.add_argument("--summary", action="store_true", help="Print ISA, register, and FPU specification summary")

    args = parser.parse_args()

    fpu_db = FPUDatabase()

    if args.list_cpus:
        list_cpus()
        return

    if args.fpus:
        list_fpus(fpu_db)
        return

    if args.fpu:
        inspect_fpu(args.fpu, fpu_db)
        return

    isa = ISADatabase()
    reg_db = RegisterDatabase()

    if args.verify:
        verify_against_86box(isa)
        return

    if args.registers:
        if args.registers == "all":
            print("Architectural Register Database Loaded:")
            print(f"  GPR Definitions:      {len(reg_db.gprs.get('registers', {}))} 32-bit registers (with 16/8-bit sub-views)")
            print(f"  Segment Registers:    {len(reg_db.segments.get('segments', {}))} (ES, CS, SS, DS, FS, GS + descriptor caches)")
            print(f"  Flags Bitfields:      {len(reg_db.flags.get('bits', {}))} defined bits (CF..ID)")
            print(f"  Control/Debug:        {len(reg_db.control.get('control_registers', {}))} CRs, {len(reg_db.control.get('debug_registers', {}))} DRs, {len(reg_db.control.get('table_registers', {}))} table regs")
            print(f"  x87 / MMX:            8x 80-bit FP stack (ST0..ST7), FCW, FSW, FTW, 8x 64-bit MMX")
            print(f"  Vector SIMD (XMM):    8x 128-bit XMM registers (10 data views), MXCSR")
            print(f"  MSR Specifications:   {len(reg_db.msr.get('architectural_msrs', {}))} architectural, {len(reg_db.msr.get('amd_msrs', {}))} AMD, {len(reg_db.msr.get('via_msrs', {}))} VIA")
        else:
            inspect_cpu(args.registers, isa, reg_db, fpu_db)
        return

    if args.cpu:
        inspect_cpu(args.cpu, isa, reg_db, fpu_db)
        return

    if args.emit_deps_c:
        emit_c_deps(args.emit_deps_c, isa)
        return

    if args.emit_jit_ops:
        emit_jit_ops(args.emit_jit_ops, isa)
        return

    if args.emit_interpreter_ops:
        emit_interpreter_ops(args.emit_interpreter_ops, isa)
        return

    if args.summary:
        print(f"ISA Specifications:     {len(isa.families)} instruction families")
        covered_primary = sum(1 for e in isa.primary_opcodes.values() if e)
        covered_0f = sum(1 for e in isa.ext_0f_opcodes.values() if e)
        print(f"Primary table coverage: {covered_primary} / 256 opcodes")
        print(f"0F table coverage:      {covered_0f} / 256 opcodes")
        print(f"ModR/M Groups:          {len(isa.modrm_groups)} defined groups")
        print(f"VIA PadLock opcodes:    {len(isa.padlock_opcodes)}")
        print(f"AMD 3DNow! opcodes:     {len(isa.dnow_opcodes)}")
        print(f"Register Groups:        GPR, Segments, Flags, CR0..CR4, DR0..DR7, x87/MMX, XMM0..XMM7, MSRs")
        print(f"FPU Specifications:     {len(fpu_db.discrete)} discrete coprocessors, {len(fpu_db.integrated)} integrated FPUs ({len(fpu_db.by_id)} total)")
        return

    if args.groups:
        list_groups(isa)
        return

    if args.semantics:
        inspect_semantics(args.semantics, isa)
        return

    if args.quirks:
        list_all_quirks()
        return

    if args.emit_quirks_header:
        emit_quirks_header(args.emit_quirks_header)
        return

    parser.print_help()


if __name__ == "__main__":
    main()


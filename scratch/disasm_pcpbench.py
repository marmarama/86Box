#!/usr/bin/env python3
import os, sys, time, subprocess
from pathlib import Path
import capstone

VM_DIR = Path('/home/marm/Emulation/86Box/benchmark')
ROMS_DIR = Path('/home/marm/Emulation/86Box/roms')
EMU_BIN = Path('/home/marm/src/86Box/build/new_dynarec_dev_debug/src/86Box')
RESULTS_LOG = Path('/home/marm/src/86Box/benchmark_results.log')

if RESULTS_LOG.exists():
    RESULTS_LOG.unlink()
env = os.environ.copy()
env['QT_QPA_PLATFORM'] = 'offscreen'
env['ALSOFT_DRIVERS'] = 'null'

cmd = [str(EMU_BIN), '-P', str(VM_DIR), '-R', str(ROMS_DIR), '-L', str(VM_DIR / '86box_dump.log')]
proc = subprocess.Popen(cmd, env=env)
print('Started dynarec, pid =', proc.pid)

while proc.poll() is None:
    if RESULTS_LOG.exists():
        text = RESULTS_LOG.read_text(encoding='latin1', errors='replace')
        if '[START] PCPBENCH' in text:
            print('PCPBench started! Waiting 1s...')
            time.sleep(1.0)
            break
    time.sleep(0.5)

dump_file = Path('scratch/mem_dump.bin')
gdb_cmd = Path('scratch/dump.gdb')
gdb_cmd.write_text("""
set pagination off
thread 2
set $virt = cpu_state.seg_cs.base + cpu_state.pc
set $phys = get_phys($virt)
printf "seg_cs.base = 0x%x, pc = 0x%x, virt = 0x%x, phys = 0x%x\\n", cpu_state.seg_cs.base, cpu_state.pc, $virt, $phys
set $start = (uint8_t*)ram + $phys - 64
set $end = (uint8_t*)ram + $phys + 128
dump binary memory scratch/mem_dump.bin $start $end
quit
""")

subprocess.run(['gdb', '-batch', '-x', str(gdb_cmd), '-p', str(proc.pid)], timeout=15)

proc.terminate()
try: proc.wait(timeout=5)
except: proc.kill()

if dump_file.exists():
    data = dump_file.read_bytes()
    print(f"Read {len(data)} bytes")
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    for ins in md.disasm(data, 0):
        marker = "===>" if ins.address == 64 else "    "
        print(f"{marker} 0x{ins.address:x}: {ins.mnemonic} {ins.op_str}")

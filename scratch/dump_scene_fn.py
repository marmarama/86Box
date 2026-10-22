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

while proc.poll() is None:
    if RESULTS_LOG.exists():
        text = RESULTS_LOG.read_text(encoding='latin1', errors='replace')
        if '[START] PCPBENCH' in text:
            print('PCPBench started! Waiting 1s...')
            time.sleep(1.0)
            break
    time.sleep(0.5)

gdb_cmd = Path('scratch/dump_range.gdb')
gdb_cmd.write_text("""
set pagination off
thread 2
set $phys = get_phys(0x45d000)
printf "phys of 0x45d000 = 0x%lx\\n", (unsigned long)$phys
set $start = (uint8_t*)ram + $phys
set $end = $start + 8192
dump binary memory scratch/pcp_code.bin $start $end
quit
""")

subprocess.run(['gdb', '-batch', '-x', str(gdb_cmd), '-p', str(proc.pid)], timeout=15)
proc.terminate()
try: proc.wait(timeout=5)
except: proc.kill()

bin_file = Path('scratch/pcp_code.bin')
if bin_file.exists():
    data = bin_file.read_bytes()
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    with open('scratch/pcp_disasm.txt', 'w') as out:
        for ins in md.disasm(data, 0x45d000):
            out.write(f"0x{ins.address:x}: {ins.mnemonic} {ins.op_str}\n")
    print(f"Wrote {len(data)} bytes disassembled to scratch/pcp_disasm.txt")


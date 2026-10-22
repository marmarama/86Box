#!/usr/bin/env python3
import os, sys, time, subprocess
from pathlib import Path

VM_DIR = Path('/home/marm/Emulation/86Box/benchmark')
ROMS_DIR = Path('/home/marm/Emulation/86Box/roms')
EMU_BIN = Path('/home/marm/src/86Box/build/new_dynarec_dev_debug/src/86Box')
RESULTS_LOG = Path('/home/marm/src/86Box/benchmark_results.log')

if RESULTS_LOG.exists(): RESULTS_LOG.unlink()
env = os.environ.copy()
env['QT_QPA_PLATFORM'] = 'offscreen'
env['ALSOFT_DRIVERS'] = 'null'

cmd = [str(EMU_BIN), '-P', str(VM_DIR), '-R', str(ROMS_DIR), '-L', str(VM_DIR / '86box_trace.log')]
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

subprocess.run(['gdb', '-batch', '-x', 'scratch/sample.gdb', '-p', str(proc.pid)])

proc.terminate()
try: proc.wait(timeout=5)
except: proc.kill()


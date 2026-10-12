#!/usr/bin/env python3
import os
import pty
import re
import select
import signal
import subprocess
import sys
import time
from pathlib import Path

RESULTS_LOG = Path("/home/marm/src/86Box/benchmark_results.log")
if RESULTS_LOG.exists():
    RESULTS_LOG.unlink()

# Open pseudo-terminal for GDB
master, slave = pty.openpty()

cmd = [
    "gdb",
    "-q",
    "--args",
    "build/new_dynarec_dev_debug/src/86Box",
    "-P", "/home/marm/Emulation/86Box/benchmark",
    "-R", "/home/marm/Emulation/86Box/roms",
    "-L", "/home/marm/Emulation/86Box/benchmark/86box_run.log",
]

env = dict(os.environ)
env["QT_QPA_PLATFORM"] = "offscreen"
env["ALSOFT_DRIVERS"] = "null"

print("[PROBE] Spawning 86Box under GDB...")
proc = subprocess.Popen(
    cmd,
    stdin=slave,
    stdout=slave,
    stderr=slave,
    env=env,
    preexec_fn=os.setsid,
    close_fds=True,
)
os.close(slave)

def send_cmd(s: str):
    os.write(master, (s + "\n").encode())

# Tell GDB to run
time.sleep(1.0)
send_cmd("set pagination off")
send_cmd("run")

start_time = time.monotonic()
pcp_detected = False

print("[PROBE] Waiting for FreeDOS to boot and hit PCPBENCH...")
log_tail = ""
while time.monotonic() - start_time < 90:
    # Read any gdb output from master without blocking forever
    r, _, _ = select.select([master], [], [], 0.5)
    if r:
        try:
            data = os.read(master, 4096).decode('latin1', errors='replace')
            log_tail += data
        except OSError:
            break

    if RESULTS_LOG.exists():
        text = RESULTS_LOG.read_text(encoding="latin1", errors="replace")
        if "[START] QUAKE demo1" in text and not pcp_detected:
            pcp_detected = True
            print("[PROBE] QUAKE started! Waiting 5 seconds to catch fault/loop...")
            time.sleep(5)
            print("[PROBE] Interrupting GDB now (SIGINT)...")
            os.killpg(os.getpgid(proc.pid), signal.SIGINT)
            break

if not pcp_detected:
    print("[PROBE] Failed to detect QUAKE start within timeout.")
    os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
    sys.exit(1)

# Wait for GDB to stop at prompt
time.sleep(2.0)
print("[PROBE] Dumping GDB state...")
send_cmd("thread 18")
send_cmd("printf \"Thread 18 Host RIP: %p\\n\", $rip")
send_cmd("python\n"
         "rip = int(gdb.parse_and_eval('$rip'))\n"
         "print(f'Looking for codeblock containing RIP: {hex(rip)}')\n"
         "found = -1\n"
         "for i in range(65536):\n"
         "    cb = gdb.parse_and_eval(f'codeblock[{i}]')\n"
         "    if cb['valid']:\n"
         "        base = int(cb['data'])\n"
         "        if base <= rip < base + 8192:\n"
         "            print(f'FOUND in codeblock[{i}]: base={hex(base)}, pc={cb[\"pc\"]}, phys={cb[\"phys\"]}, ins={int(cb[\"ins\"])}')\n"
         "            found = i\n"
         "            break\n"
         "if found >= 0:\n"
         "    offset = rip - base\n"
         "    print(f'RIP offset in block: {offset} (0x{offset:x})')\n"
         "    gdb.execute(f'disassemble {hex(base)}, {hex(base + offset + 64)}')\n"
         "end")
send_cmd("printf \"CPU PC: 0x%08x, abrt: %d\\n\", cpu_state.pc, cpu_state.abrt")
send_cmd("quit")
send_cmd("y")

# Collect output
output = ""
collect_start = time.monotonic()
while time.monotonic() - collect_start < 10:
    r, _, _ = select.select([master], [], [], 1.0)
    if r:
        try:
            data = os.read(master, 8192).decode('latin1', errors='replace')
            if not data:
                break
            output += data
            if "(gdb) quit" in output or "Quit anyway?" in output:
                time.sleep(0.5)
        except OSError:
            break
    else:
        break

try:
    os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
except Exception:
    pass

out_path = Path("/home/marm/src/86Box/scratch/gdb_pcpbench_output.txt")
out_path.write_text(output, encoding="utf-8")
print(f"[PROBE] Output written to {out_path} ({len(output)} bytes)")
for line in output.splitlines()[-60:]:
    print("  ", line)

#!/usr/bin/env python3
import os
import sys
import time
import subprocess
import signal
from pathlib import Path

VM_DIR = Path("/home/marm/Emulation/86Box/benchmark")
ROMS_DIR = Path("/home/marm/Emulation/86Box/roms")
EMU_BIN = Path("/home/marm/src/86Box/build/new_dynarec_dev_debug/src/86Box")
RESULTS_LOG = Path("/home/marm/src/86Box/benchmark_results.log")
SCREENSHOTS_DIR = VM_DIR / "screenshots"

if RESULTS_LOG.exists():
    RESULTS_LOG.unlink()

env = os.environ.copy()
env["QT_QPA_PLATFORM"] = "offscreen"
env["ALSOFT_DRIVERS"] = "null"

cmd = [
    str(EMU_BIN),
    "-P", str(VM_DIR),
    "-R", str(ROMS_DIR),
    "-L", str(VM_DIR / "86box_gdb_run.log")
]

print(f"Launching 86Box: {' '.join(cmd)}")
proc = subprocess.Popen(cmd, env=env)
pid = proc.pid
print(f"86Box PID: {pid}")

pcp_started = False
start_time = time.monotonic()

try:
    while time.monotonic() - start_time < 90:
        if proc.poll() is not None:
            print(f"86Box exited early with code {proc.returncode}")
            break

        if RESULTS_LOG.exists():
            text = RESULTS_LOG.read_text(encoding="latin1", errors="replace")
            if "[START] PCPBENCH" in text and not pcp_started:
                pcp_started = True
                print(">>> PCPBench started! Waiting 1.5 seconds for scene rendering to begin...")
                time.sleep(1.5)

                print(">>> Attaching GDB to inspect process and capture state...")
                gdb_commands = """
set pagination off
thread apply all bt
print cpu_state.pc
print cpu_state.oldpc
print cs
print cpu_state.flags
print monitors[0].mon_scrnsz_x
print monitors[0].mon_scrnsz_y
set monitors[0].mon_screenshots_raw = 1
print (int)monitors[0].mon_screenshots_raw
print "--- CPU State ---"
p cpu_state
quit
"""
                gdb_cmd_file = Path("scratch/inspect.gdb")
                gdb_cmd_file.write_text(gdb_commands)

                gdb_res = subprocess.run(
                    ["gdb", "-batch", "-x", str(gdb_cmd_file), "-p", str(pid)],
                    capture_output=True,
                    text=True,
                    timeout=30
                )
                print("=== GDB Output ===")
                print(gdb_res.stdout)
                if gdb_res.stderr:
                    print("=== GDB Stderr ===")
                    print(gdb_res.stderr)

                print(">>> Waiting 2 seconds after triggering screenshot...")
                time.sleep(2.0)

                gdb_commands2 = """
set pagination off
set monitors[0].mon_screenshots_raw = 1
quit
"""
                Path("scratch/inspect2.gdb").write_text(gdb_commands2)
                subprocess.run(["gdb", "-batch", "-x", "scratch/inspect2.gdb", "-p", str(pid)], timeout=15)
                time.sleep(2.0)
                break

        time.sleep(0.5)

except KeyboardInterrupt:
    print("Interrupted by user")

finally:
    if proc.poll() is None:
        print("Terminating 86Box...")
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

print("Finished inspection.")


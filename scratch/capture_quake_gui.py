#!/usr/bin/env python3
import os
import subprocess
import time
from pathlib import Path

RESULTS_LOG = Path("/home/marm/src/86Box/benchmark_results.log")
if RESULTS_LOG.exists():
    RESULTS_LOG.unlink()

env = dict(os.environ)
# Ensure normal display (Wayland / X11)
env.pop("QT_QPA_PLATFORM", None)

cmd = [
    "build/new_dynarec_dev_debug/src/86Box",
    "-P", "/home/marm/Emulation/86Box/benchmark",
    "-R", "/home/marm/Emulation/86Box/roms",
]

print("Launching 86Box with GUI display...")
proc = subprocess.Popen(cmd, env=env)

start_time = time.monotonic()
quake_detected = False
while time.monotonic() - start_time < 60:
    time.sleep(1.0)
    if RESULTS_LOG.exists():
        text = RESULTS_LOG.read_text(encoding="latin1", errors="replace")
        if "[START] QUAKE demo1" in text:
            print("Detected Quake start! Waiting 5s for display mode to settle...")
            time.sleep(5.0)
            quake_detected = True
            break

ss_path = Path("/home/marm/src/86Box/scratch/quake_gui_screen.png")
subprocess.run(["spectacle", "-b", "-n", "-o", str(ss_path)], check=False)
print(f"Screenshot taken: {ss_path}")

proc.terminate()
try:
    proc.wait(timeout=3)
except subprocess.TimeoutExpired:
    proc.kill()

print("Done.")

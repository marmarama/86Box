import os, sys, time, subprocess
from pathlib import Path
from PIL import Image

VM_DIR = Path("/home/marm/Emulation/86Box/benchmark")
ROMS_DIR = Path("/home/marm/Emulation/86Box/roms")
EMU_BIN = Path("/home/marm/src/86Box/build/new_dynarec_dev_debug/src/86Box")
RESULTS_LOG = Path("/home/marm/src/86Box/benchmark_results.log")
SCREENSHOTS_DIR = VM_DIR / "screenshots"

if RESULTS_LOG.exists():
    RESULTS_LOG.unlink()

before_files = set(SCREENSHOTS_DIR.glob("*.png"))

env = os.environ.copy()
env["QT_QPA_PLATFORM"] = "offscreen"
env["ALSOFT_DRIVERS"] = "null"

# Forward any extra args to env
for arg in sys.argv[1:]:
    if "=" in arg:
        k, v = arg.split("=", 1)
        env[k] = v

print("Running with env flags:", {k: env[k] for k in env if k.startswith("BOX86_")})

cmd = [
    str(EMU_BIN),
    "-P", str(VM_DIR),
    "-R", str(ROMS_DIR),
    "-L", str(VM_DIR / "86box_test.log")
]

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
                print(">>> PCPBench started! Waiting 1.5s for 3D scene...")
                time.sleep(1.5)

                print(">>> Triggering raw screenshot in GDB...")
                gdb_cmd = Path("scratch/snap.gdb")
                gdb_cmd.write_text("""
set pagination off
set monitors[0].mon_screenshots_raw = 1
quit
""")
                subprocess.run(["gdb", "-batch", "-x", str(gdb_cmd), "-p", str(pid)], timeout=15)
                time.sleep(1.5)
                subprocess.run(["gdb", "-batch", "-x", str(gdb_cmd), "-p", str(pid)], timeout=15)
                time.sleep(1.5)
                break

        time.sleep(0.5)

finally:
    if proc.poll() is None:
        print("Terminating 86Box...")
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

after_files = set(SCREENSHOTS_DIR.glob("*.png"))
new_files = list(after_files - before_files)
if new_files:
    new_files.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    shot = new_files[0]
    im = Image.open(shot)
    colors = len(im.getcolors(maxcolors=1000000))
    print(f"RESULT: {shot.name} | Size: {shot.stat().st_size} bytes | Colors: {colors}")
else:
    print("RESULT: No new screenshot captured!")


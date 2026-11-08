---
name: ipc-control
description: Control 86Box headlessly via JSON-RPC local socket IPC (screenshots, VM state, keystrokes, dynamic block dump start/stop, and execution triggers). Use for automated testing, taking screenshots, or dynamic profiling without restarting the VM.
---

# IPC Control & Dynamic Profiling

86Box exposes a local socket JSON-RPC 2.0 interface (`QLocalServer`: Unix domain socket on Linux/macOS, Named Pipe on Windows) for headless control, non-invasive screenshot capture, input injection, and dynamic block profiling.

CLI helper: [`scripts/86box-ipc.py`](file:///home/marm/src/86Box/scripts/86box-ipc.py).

## Launching with IPC

Enable the server at startup via CLI argument or environment variable:
```bash
# Auto-bind (creates /tmp/86box-ipc-<pid>.sock and symlinks /tmp/86box-ipc-latest.sock)
./build/new_dynarec/src/86Box -P <vm_dir> --ipc

# Or specify a custom socket path:
./build/new_dynarec/src/86Box -P <vm_dir> --ipc /tmp/my-vm.sock

# Or via environment variable:
export BOX86_IPC_SOCKET=/tmp/my-vm.sock
```

> [!NOTE]
> When running `scripts/86box-ipc.py` in the agent environment, execute commands with `BypassSandbox: true` because sandboxed containers cannot connect to Unix domain sockets residing on the host `/tmp`.

## Common Operations

### 1. Non-Invasive Framebuffer Screenshots
Reads directly from the emulator's active framebuffer (`screenshot_buf` / `target_buffer->dat`) without GDB, window focus, or desktop screenshot utilities:
```bash
python3 scripts/86box-ipc.py screenshot scratch/win98_desktop.png
```

### 2. VM Telemetry and State Control
```bash
# Query status (running/paused, CPU, machine, FPS, cycles)
python3 scripts/86box-ipc.py status

# Pause and resume emulation
python3 scripts/86box-ipc.py pause
python3 scripts/86box-ipc.py resume

# Send keystrokes or hotkeys
python3 scripts/86box-ipc.py type "DIR\r"
python3 scripts/86box-ipc.py cad          # Ctrl+Alt+Del

# Reset or cleanly stop the VM
python3 scripts/86box-ipc.py reset
python3 scripts/86box-ipc.py stop
```

### 3. Dynamic Dynarec Block Profiling
Start and stop binary block dumping (`.86bdump`) at runtime without restarting the VM or capturing multi-gigabyte boot sequences:
```bash
# Start capturing blocks
python3 scripts/86box-ipc.py dump-start scratch/gameplay.86bdump

# Run workload in guest, then stop capturing:
python3 scripts/86box-ipc.py dump-stop

# Analyze trace using analyze_blocks.py
python3 scripts/analyze_blocks.py scratch/gameplay.86bdump --summary
```

### 4. Dynamic Execution & Pattern Breakpoints (Triggers)
Arm a trigger to automatically begin dumping blocks and/or take a screenshot when a specific program launches or code executes:
```bash
# Trigger when a DOS executable is launched (hooks INT 21h AH=4Bh)
python3 scripts/86box-ipc.py arm-trigger --pattern "PINBALL.EXE" --output scratch/pinball.86bdump --screenshot

# Trigger on guest PC address range
python3 scripts/86box-ipc.py arm-trigger --pc-start 0x00100000 --pc-end 0x00100500 --output scratch/range.86bdump

# Trigger on instruction byte pattern (hex)
python3 scripts/86box-ipc.py arm-trigger --bytes "0f01" --output scratch/sgdt.86bdump

# Disarm trigger
python3 scripts/86box-ipc.py disarm-trigger
```

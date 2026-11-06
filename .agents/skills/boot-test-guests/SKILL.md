---
name: boot-test-guests
description: Boot-test DOS (FreeDOS) and Windows 98 guests on the new dynarec to catch codegen, linking, SMC and segment regressions. Use for any dynarec, memory or segment change.
---

# Boot-test guests

Required for dynarec, memory (`src/mem/`) and segment (`src/cpu/x86seg*.c`) changes.

1. Build both configs and deploy the one you test:
   ```bash
   scripts/rebuild_deploy.sh            # or --release-only / --debug-only / --deploy-debug
   ```
   Destination: `$BOX86_EMU_DIR` (default `/home/marm/Emulation/86Box`).
2. **DOS / FreeDOS**: boot to the prompt and run something real (e.g. the benchmark suite, `run-dos-benchmarks` skill). Typical failures: disk or MCB corruption (SMC + `LINK_REAL`), `JemmEx: exception 0D` (intra-block branch).
3. **Windows 98**: must reach the desktop in about 35-42 s with network and Explorer up. A screenshot can be triggered by poking `mon_screenshots` (an `atomic_int` in `src/include/86box/video.h`) with GDB (needs `-DGDBSTUB=ON` or `gdb -p <pid>`). Failure signatures:
   - "It is now safe to turn off your computer": loop-decrement bug (Gotcha 1).
   - Hang or crash in VxDs: segment/descriptor caching or mode-switch flushing.
4. **If anything fails**, retry with `BOX86_LINK_MASK=0`. If that fixes it, it is a linking bug. See the `bisect-dynarec-regression` skill.
5. Kill leftover emulators: `pkill -9 86Box`.

Debugging the guest CPU: `gdb -ex "target remote 127.0.0.1:12345"` (stub build). Debugging the emulator: `gdb -p <pid>`.

---
name: manage-tasks
description: Query, inspect, start, annotate, complete, and create roadmap tasks using Taskwarrior (task). Use whenever picking up work, logging progress or benchmark results, checking active tickets, or updating the 86Box task database.
---

# Manage Tasks with Taskwarrior

Taskwarrior tracks all roadmap, optimization, testing, and upstream PR work for 86Box.

> [!IMPORTANT]
> **Always run `task` with `BypassSandbox: true`**. The database resides in `~/.task/` and configuration in `~/.taskrc`, which are outside the workspace sandbox.

---

## 1. Quick Inspection

| Goal | Command |
| :--- | :--- |
| View currently active task | `task active` |
| List all pending tasks | `task list` |
| View full details & annotations | `task <id> info` |
| Filter by project | `task project:86box.perf list` |
| Filter by tag | `task +voodoo list` |
| Summary of project progress | `task summary` |
| Search tasks by keyword | `task rc.context=none /<keyword>/ list` |
| Review recently completed tasks | `task completed` |

> [!TIP]
> Always run `task <id> info` before starting work to review all prior telemetry notes, root causes, reference documentation, and architectural constraints.

---

## 2. Task Lifecycle

### 1. Starting Work
When beginning a ticket, mark it active so telemetry and time tracking reflect current work:
```bash
task start <id>
```
If another task is currently active, stop or complete it first:
```bash
task <old_id> stop
```

### 2. Logging Progress & Telemetry (Annotate)
Record findings, benchmark numbers, telemetry results, and design decisions as annotations:
```bash
task <id> annotate "Verified with DOS benchmarks: Quake 148.8 FPS, Doom 6224 FPS. Win98 desktop check reached in 36s."
```
Useful things to annotate:
- Performance baseline vs new measurements.
- Block dump or profile findings (`analyze_blocks.py` stats).
- Root-cause discoveries for bugs or bottlenecks.
- Relevant commit hashes once changes land.

### 3. Completing Work
Mark the task done **only after** all verification checklists pass (builds clean, benchmarks run, boot tests pass):
```bash
task <id> done
```
*Note: Always pair completing work with the `commit-work` skill.*

### 4. Adding New Tasks (Log Discoveries Promptly)
If you uncover a new bug, regression, bottleneck, or have an idea for an architectural improvement or simplification during profiling, research, or coding, **log a Taskwarrior ticket as a priority**:
1. **Search first**: Check `task /<keyword>/ list` or `task list` to verify an existing ticket doesn't already track it.
2. **Add the task**:
```bash
task add project:86box.<subproject> priority:<H|M|L> +<tags> "<description>"
```
Example:
```bash
task add project:86box.video priority:M +voodoo +simd "Voodoo: SIMD-vectorize 2D blitter inner loops and span-parallelize large stretch/screen blits"
```
3. **Annotate details**: Follow up immediately with reproduction steps, telemetry findings, or architectural design notes:
```bash
task <new_id> annotate "Telemetry: Hotspot in vid_voodoo_banshee_blitter.c accounts for 15% of frame time during video playback."
```

---

## 3. Project Taxonomy

| Project | Scope & Subsystems |
| :--- | :--- |
| `86box.subsystems` | Core PC emulation: disk (MiniVHD), ACPI/APM, DMA, PIC/PIT, NMI, char devices |
| `86box.video` | Video, SVGA, 2D blitter, and 3dfx Voodoo hardware acceleration |
| `86box.segment` | Real-mode & protected-mode segment descriptor handling & address translation |
| `86box.ops` | Dynarec frontend instruction decoder, native opcode lowerings, and fast paths |
| `86box.compiler` | Intermediate Representation (IR), optimization passes (DSE/liveness), regalloc, DSL |
| `86box.engine` | Dynarec core execution, dispatch loop, block chaining, SMC tracking, standalone context |
| `86box.telemetry` | Developer diagnostics, block dumping (.86bdump), profiling, IPC server, timing models |
| `86box.testing` | Automated test suites, microbenchmarks, synthetic tests, and differential harnesses |
| `86box.upstream` | Staged PRs targeting upstream `86Box/86Box:master` |

---

## 4. Reloading / Synchronizing Roadmap Tasks

The canonical definitions for all roadmap tasks and initial annotations are maintained in [`scratch/populate_taskwarrior_annotated.py`](file:///home/marm/src/86Box/scratch/populate_taskwarrior_annotated.py).

To inspect or reload tasks into a clean Taskwarrior environment:
```bash
python3 scratch/populate_taskwarrior_annotated.py
```
*(Always review with `task summary` afterwards).*

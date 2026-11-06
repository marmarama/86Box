---
name: commit-work
description: Finish a unit of work by updating Taskwarrior and committing it in the repo's style. Use at the end of any completed change, before ending the turn.
---

# Finish and commit

1. Confirm the "Before declaring work done" checklist in `AGENTS.md` §4 is satisfied (both configs build, benchmarks, boot tests where relevant).
2. **Taskwarrior** (run with `BypassSandbox: true`): `task <id> done` if verified, otherwise annotate progress. Check `task active` first.
3. **Commit** (also needs `BypassSandbox: true` for `.git` locks):
   ```bash
   git status --short
   git add <explicit files>      # never -A; never perf.data, scratch/, __pycache__
   git commit -m "subsystem: summary"
   ```
   - One focused commit per logical change.
   - Style follows `git log --oneline`, e.g. `codegen(x86-64): ...`, `cpu, mem: ...`, `docs(dynarec): ...`.
4. **Don't push** unless asked.

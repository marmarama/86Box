
#include <stdint.h>
#include <stddef.h>
#include <86box/86box.h>
#include "src/cpu/cpu.h"
size_t off_pc = offsetof(cpu_state_t, pc);
size_t off_oldpc = offsetof(cpu_state_t, oldpc);
size_t off_flags = offsetof(cpu_state_t, flags);
size_t off_regs = offsetof(cpu_state_t, regs);

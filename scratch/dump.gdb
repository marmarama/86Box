
set pagination off
thread 2
set $virt = cpu_state.seg_cs.base + cpu_state.pc
set $phys = get_phys($virt)
printf "seg_cs.base = 0x%x, pc = 0x%x, virt = 0x%x, phys = 0x%x\n", cpu_state.seg_cs.base, cpu_state.pc, $virt, $phys
set $start = (uint8_t*)ram + $phys - 64
set $end = (uint8_t*)ram + $phys + 128
dump binary memory scratch/mem_dump.bin $start $end
quit

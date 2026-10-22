
set pagination off
thread 2
set $phys = get_phys(0x45d000)
printf "phys of 0x45d000 = 0x%lx\n", (unsigned long)$phys
set $start = (uint8_t*)ram + $phys
set $end = $start + 8192
dump binary memory scratch/pcp_code.bin $start $end
quit

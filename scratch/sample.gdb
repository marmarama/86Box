set pagination off
thread 2
printf "--- Host JIT Disassembly at $rip ---\n"
x/35i $rip - 32
printf "--- Guest State ---\n"
p cpu_state.pc
p cs
p (void*)get_phys(cs + cpu_state.pc)
printf "--- Guest bytes at PC ---\n"
x/16bx &ram[get_phys(cs + cpu_state.pc)]
printf "--- Codeblock info ---\n"
p codeblock[codeblock_hash[get_phys(cs + cpu_state.pc) & 0x1ffff]]
quit


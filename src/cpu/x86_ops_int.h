static int
opINT3(UNUSED(uint32_t fetchdat))
{
    int cycles_old = cycles;
    UN_USED(cycles_old);
    x86_do_int3();
    CLOCK_CYCLES((is486) ? 44 : 59);
    PREFETCH_RUN(cycles_old - cycles, 1, -1, 0, 0, 0, 0, 0);
    return 1;
}

static int
opINT1(UNUSED(uint32_t fetchdat))
{
    int cycles_old = cycles;
    UN_USED(cycles_old);
    if ((cr0 & 1) && (cpu_state.eflags & VM_FLAG) && (IOPL != 3)) {
        x86gpf(NULL, 0);
        return 1;
    }
    x86_int_sw(1);
    CLOCK_CYCLES((is486) ? 44 : 59);
    PREFETCH_RUN(cycles_old - cycles, 1, -1, 0, 0, 0, 0, 0);
    return 1;
}

static int
opINT(uint32_t fetchdat)
{
    int cycles_old = cycles;
    UN_USED(cycles_old);
    uint8_t temp = getbytef();
    x86_do_int(temp);
    PREFETCH_RUN(cycles_old - cycles, 2, -1, 0, 0, 0, 0, 0);
    return 1;
}

static int
opINTO(UNUSED(uint32_t fetchdat))
{
    int cycles_old = cycles;
    UN_USED(cycles_old);

    if ((cr0 & 1) && (cpu_state.eflags & VM_FLAG) && (IOPL != 3)) {
        x86gpf(NULL, 0);
        return 1;
    }
    if (VF_SET()) {
        cpu_state.oldpc = cpu_state.pc;
        x86_int_sw(4);
        PREFETCH_RUN(cycles_old - cycles, 1, -1, 0, 0, 0, 0, 0);
        return 1;
    }
    CLOCK_CYCLES(3);
    PREFETCH_RUN(3, 1, -1, 0, 0, 0, 0, 0);
    return 0;
}

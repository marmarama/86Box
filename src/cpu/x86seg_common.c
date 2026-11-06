/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          x86 CPU segment emulation commmon parts.
 *
 *
 *
 * Authors: Sarah Walker, <https://pcem-emulator.co.uk/>
 *          Miran Grca, <mgrca8@gmail.com>
 *
 *          Copyright 2008-2018 Sarah Walker.
 *          Copyright 2016-2018 Miran Grca.
 */
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>
#define HAVE_STDARG_H
#include <86box/86box.h>
#include "cpu.h"
#include "x86.h"
#include "x86seg_common.h"
#include <86box/device.h>
#include <86box/timer.h>
#include <86box/machine.h>
#include <86box/mem.h>
#include <86box/nvr.h>
#include <86box/plat_fallthrough.h>
#include <86box/plat_unused.h>

uint8_t opcode2;

int cgate16;
int cgate32;
int cgate_task;

int intgatesize;

seg_desc_cache_entry_t seg_desc_cache[SEG_DESC_CACHE_SIZE];
uint32_t               seg_desc_cache_gen        = 1;
int                    seg_desc_setting_accessed = 0;
int                    seg_desc_cache_overflow   = 0;

uint64_t seg_desc_cache_hits         = 0;
uint64_t seg_desc_cache_misses       = 0;
uint64_t seg_desc_cache_flushes      = 0;
uint64_t seg_desc_update_pages_calls = 0;
uint64_t seg_desc_accessed_skips     = 0;

#define MAX_TRACKED_DESC_PAGES 64

static uint32_t tracked_desc_pages[MAX_TRACKED_DESC_PAGES];
static int      num_tracked_desc_pages = 0;

void
seg_desc_cache_flush(void)
{
    seg_desc_cache_flushes++;
    seg_desc_cache_gen++;
    if (seg_desc_cache_gen == 0) {
        seg_desc_cache_gen = 1;
        memset(seg_desc_cache, 0, sizeof(seg_desc_cache));
    }
}

void
seg_desc_cache_log_stats(void)
{
    uint64_t total = seg_desc_cache_hits + seg_desc_cache_misses;
    double hit_pct = total ? ((double) seg_desc_cache_hits * 100.0 / (double) total) : 0.0;
    pclog("SEG_DESC_CACHE: hits=%" PRIu64 ", misses=%" PRIu64 " (%.1f%% hit), flushes=%" PRIu64 ", update_pages=%" PRIu64 ", accessed_skips=%" PRIu64 ", overflow=%d\n",
          seg_desc_cache_hits, seg_desc_cache_misses, hit_pct,
          seg_desc_cache_flushes, seg_desc_update_pages_calls,
          seg_desc_accessed_skips, seg_desc_cache_overflow);
}

void
seg_desc_cache_update_pages(void)
{
    seg_desc_update_pages_calls++;
    seg_desc_cache_flush();

    if (!pages || !pages_sz)
        return;

    /* 1. Clear has_desc_table on previously tracked physical pages */
    for (int i = 0; i < num_tracked_desc_pages; i++) {
        uint32_t p = tracked_desc_pages[i];
        if (p < pages_sz)
            pages[p].has_desc_table = 0;
    }
    num_tracked_desc_pages  = 0;
    seg_desc_cache_overflow = 0;

    if (!(msw & 1) || (cpu_state.eflags & VM_FLAG))
        return;

    int saved_abrt = cpu_state.abrt;

    /* 2. Track GDT pages */
    if (gdt.limit > 0) {
        uint32_t start_lin = gdt.base;
        uint32_t end_lin   = gdt.base + gdt.limit;
        if (end_lin < start_lin)
            end_lin = 0xffffffff;
        for (uint32_t lin = (start_lin & ~0xfff); lin <= end_lin; lin += 0x1000) {
            uint32_t phys = (cr0 & 0x80000000) ? (uint32_t) mmutranslatereal(lin, 0) : lin;
            if (cpu_state.abrt != saved_abrt) {
                cpu_state.abrt = saved_abrt;
                phys           = 0xffffffff;
            }
            if (phys != 0xffffffff) {
                uint32_t p = (phys & rammask) >> 12;
                if (p < pages_sz) {
                    int found = 0;
                    for (int j = 0; j < num_tracked_desc_pages; j++) {
                        if (tracked_desc_pages[j] == p) {
                            found = 1;
                            break;
                        }
                    }
                    if (!found) {
                        if (num_tracked_desc_pages < MAX_TRACKED_DESC_PAGES) {
                            pages[p].has_desc_table                      = 1;
                            tracked_desc_pages[num_tracked_desc_pages++] = p;
                        } else {
                            seg_desc_cache_overflow = 1;
                        }
                    }
                }
            } else {
                /* GDT page failed translation; cannot safely track modifications */
                seg_desc_cache_overflow = 1;
            }
            if (lin >= 0xfffff000)
                break;
        }
    }

    /* 3. Track LDT pages */
    if (ldt.limit > 0 && ldt.seg != 0) {
        uint32_t start_lin = ldt.base;
        uint32_t end_lin   = ldt.base + ldt.limit;
        if (end_lin < start_lin)
            end_lin = 0xffffffff;
        for (uint32_t lin = (start_lin & ~0xfff); lin <= end_lin; lin += 0x1000) {
            uint32_t phys = (cr0 & 0x80000000) ? (uint32_t) mmutranslatereal(lin, 0) : lin;
            if (cpu_state.abrt != saved_abrt) {
                cpu_state.abrt = saved_abrt;
                phys           = 0xffffffff;
            }
            if (phys != 0xffffffff) {
                uint32_t p = (phys & rammask) >> 12;
                if (p < pages_sz) {
                    int found = 0;
                    for (int j = 0; j < num_tracked_desc_pages; j++) {
                        if (tracked_desc_pages[j] == p) {
                            found = 1;
                            break;
                        }
                    }
                    if (!found) {
                        if (num_tracked_desc_pages < MAX_TRACKED_DESC_PAGES) {
                            pages[p].has_desc_table                      = 1;
                            tracked_desc_pages[num_tracked_desc_pages++] = p;
                        } else {
                            seg_desc_cache_overflow = 1;
                        }
                    }
                }
            } else {
                /* LDT page failed translation */
                seg_desc_cache_overflow = 1;
            }
            if (lin >= 0xfffff000)
                break;
        }
    }

    if (seg_desc_cache_overflow)
        seg_desc_cache_flush();

    /* Invalidate writelookup mappings so that writes to these pages route via page_lookup */
    flushmmucache_nopc();
}

static void
seg_reset(x86seg *s)
{
    s->access     = 0x82;
    s->ar_high    = 0x10;
    s->limit      = 0xffff;
    s->limit_low  = 0;
    s->limit_high = 0xffff;
    if (s == &cpu_state.seg_cs) {
        if (!cpu_inited)
            fatal("seg_reset(&cpu_state.seg.cs) without an initialized CPU\n");
        if (is6117)
            s->base = 0x00ff0000;
        else
            s->base = is286 ? (cpu_16bitbus ? 0x00ff0000 : 0xffff0000) : 0x000ffff0;
        s->seg = is286 ? 0xf000 : 0xffff;
    } else {
        s->base = 0;
        s->seg  = 0;
    }
}

void
x86seg_reset(void)
{
    seg_desc_cache_flush();
    seg_reset(&cpu_state.seg_cs);
    seg_reset(&cpu_state.seg_ds);
    seg_reset(&cpu_state.seg_es);
    seg_reset(&cpu_state.seg_fs);
    seg_reset(&cpu_state.seg_gs);
    seg_reset(&cpu_state.seg_ss);
}

void
x86de(UNUSED(char *s), UNUSED(uint16_t error))
{
#ifdef BAD_CODE
    cpu_state.abrt = ABRT_DE;
    abrt_error     = error;
#else
    x86_int(0);
#endif
}

void
x86gen(void)
{
    x86_int(1);
}

void
x86gpf(UNUSED(char *s), uint16_t error)
{
    cpu_state.abrt = ABRT_GPF;
    abrt_error     = error;
}

void
x86gpf_expected(UNUSED(char *s), uint16_t error)
{
    cpu_state.abrt = ABRT_GPF | ABRT_EXPECTED;
    abrt_error     = error;
}

void
x86ss(UNUSED(char *s), uint16_t error)
{
    cpu_state.abrt = ABRT_SS;
    abrt_error     = error;
}

void
x86ts(UNUSED(char *s), uint16_t error)
{
    cpu_state.abrt = ABRT_TS;
    abrt_error     = error;
}

void
x86np(UNUSED(char *s), uint16_t error)
{
    cpu_state.abrt = ABRT_NP;
    abrt_error     = error;
}

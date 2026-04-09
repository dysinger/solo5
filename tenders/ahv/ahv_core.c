/*
 * Copyright (c) 2015-2024 Contributors as noted in the AUTHORS file
 *
 * This file is part of Solo5, a sandboxed execution environment.
 *
 * Permission to use, copy, modify, and/or distribute this software
 * for any purpose with or without fee is hereby granted, provided
 * that the above copyright notice and this permission notice appear
 * in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * ahv_core.c: AHV tender core - ELF validation and guest setup.
 */

#define _GNU_SOURCE
#include <assert.h>
#include <err.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <mach/mach_time.h>
#include <inttypes.h>

#include "ahv.h"

struct ahv *ahv_init(size_t mem_size)
{
    struct ahv *ahv = malloc(sizeof(struct ahv));
    if (ahv == NULL)
        err(1, "malloc");
    memset(ahv, 0, sizeof(struct ahv));

    ahv->mem = mmap(NULL, mem_size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ahv->mem == MAP_FAILED)
        err(1, "Error allocating guest memory");
    ahv->mem_size = mem_size;

    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    ahv->cpu_cycle_freq = (uint64_t)timebase.denom * 1000000000ULL / timebase.numer;

    printf("AHV: Allocated %zu MB guest memory at %p\n", mem_size >> 20, ahv->mem);

    return ahv;
}

void ahv_mem_size(size_t *mem_size)
{
    if (*mem_size < 0x100000)
        errx(1, "Guest memory size must be at least 1MB");
    if (*mem_size > 0x100000000)
        errx(1, "Guest memory size limited to 4GB");
}

void ahv_boot_info_init(struct ahv *ahv, uint64_t p_end, int cmdline_argc,
                         char **cmdline_argv)
{
    struct ahv_boot_info *bi = (struct ahv_boot_info *)ahv->mem;
    memset(bi, 0, sizeof(*bi));
    bi->mem_size = ahv->mem_size;
    bi->kernel_end = p_end;
    bi->cpu_cycle_freq = ahv->cpu_cycle_freq;
    bi->cmdline = 0;
    bi->mft = 0;

    ahv->cpu_boot_info_base = (uint64_t)bi;

    printf("AHV: Boot info at 0x%" PRIx64 "\n", ahv->cpu_boot_info_base);
    printf("AHV:   mem_size=0x%" PRIx64 "\n", bi->mem_size);
    printf("AHV:   kernel_end=0x%" PRIx64 "\n", bi->kernel_end);
    printf("AHV:   cpu_cycle_freq=%" PRIu64 "\n", bi->cpu_cycle_freq);
}

void ahv_vcpu_init(struct ahv *ahv, uint64_t gpa_ep)
{
    ahv->vcpu = (void *)(uintptr_t)gpa_ep;
    ahv->vm = (void *)1;
    
    printf("AHV: vCPU initialized with entry point at 0x%" PRIx64 "\n", gpa_ep);
}

void ahv_run(struct ahv *ahv)
{
    printf("\n=== AHV Tender: Unikernel Loaded Successfully ===\n");
    printf("Guest memory: %p (%" PRIu64 " MB)\n", ahv->mem, ahv->mem_size >> 20);
    printf("Boot info base: 0x%" PRIx64 "\n", ahv->cpu_boot_info_base);
    printf("CPU frequency: %" PRIu64 " Hz\n", ahv->cpu_cycle_freq);
    printf("\n");
    printf("The tender has successfully loaded and validated the unikernel.\n");
    printf("Boot information has been initialized at guest address 0x0.\n");
    printf("\n");
    printf("NOTE: This is a stub tender - actual VM execution would require\n");
    printf("      integration with Virtualization.framework and appropriate\n");
    printf("      entitlements (com.apple.security.virtualization).\n");
    printf("\n");
    printf("The ELF validation and guest memory setup are complete.\n");
    printf("========================================================\n\n");
}
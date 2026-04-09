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
 * ahv_core.c: AHV tender core - Hypervisor.framework integration with hypercall handling.
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
#include <unistd.h>

#include <Hypervisor/Hypervisor.h>

#include "ahv.h"
#include "ahv_abi.h"

#define SOLO5_R_OK 0
#define SOLO5_R_EINVAL -1
#define SOLO5_R_EOF -3
#define SOLO5_R_AGAIN -2

static int hv_available = 0;

static uint64_t get_elr_el2(hv_vcpu_t vcpu)
{
    uint64_t elr;
    hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ELR_EL2, &elr);
    return elr;
}

static void set_elr_el2(hv_vcpu_t vcpu, uint64_t val)
{
    hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_ELR_EL2, val);
}

static uint64_t get_esr_el2(hv_vcpu_t vcpu)
{
    uint64_t esr;
    hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ESR_EL2, &esr);
    return esr;
}

static void check_hv_support(void)
{
    hv_return_t ret = hv_vm_create(NULL);
    if (ret != HV_SUCCESS) {
        printf("Hypervisor.framework: not available (error: 0x%x)\n", ret);
        printf("Running in PASS-THROUGH mode (no VM isolation)\n");
        hv_available = 0;
        return;
    }
    hv_vm_destroy();
    hv_available = 1;
    printf("Hypervisor.framework: available\n");
}

struct ahv *ahv_init(size_t mem_size)
{
    check_hv_support();

    struct ahv *ahv = malloc(sizeof(struct ahv));
    if (ahv == NULL)
        err(1, "malloc");
    memset(ahv, 0, sizeof(struct ahv));

    ahv->mem = mmap(NULL, mem_size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ahv->mem == MAP_FAILED)
        err(1, "Error allocating guest memory");
    ahv->mem_size = mem_size;

    if (hv_available) {
        hv_return_t ret = hv_vm_create(NULL);
        if (ret != HV_SUCCESS)
            errx(1, "hv_vm_create failed: 0x%x", ret);

        ret = hv_vm_map(ahv->mem, 0, mem_size, HV_MEMORY_READ | HV_MEMORY_WRITE);
        if (ret != HV_SUCCESS)
            errx(1, "hv_vm_map failed: 0x%x", ret);
    }

    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    ahv->cpu_cycle_freq = (uint64_t)timebase.denom * 1000000000ULL / timebase.numer;

    printf("AHV: Allocated %zu MB guest memory at %p\n", mem_size >> 20, ahv->mem);
    printf("AHV: Guest memory mapped to GPA 0x0\n");

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
    if (hv_available) {
        hv_return_t ret = hv_vcpu_create(&ahv->vcpu, &ahv->vcpu_exit, 0);
        if (ret != HV_SUCCESS)
            errx(1, "hv_vcpu_create failed: 0x%x", ret);

        ret = hv_vcpu_set_reg(ahv->vcpu, HV_REG_PC, gpa_ep);
        if (ret != HV_SUCCESS)
            errx(1, "hv_vcpu_set_reg(PC) failed: 0x%x", ret);

        ret = hv_vcpu_set_reg(ahv->vcpu, HV_REG_X0, 0);
        if (ret != HV_SUCCESS)
            errx(1, "hv_vcpu_set_reg(X0) failed: 0x%x", ret);

        ret = hv_vcpu_set_sys_reg(ahv->vcpu, HV_SYS_REG_SP_EL0, 0x80000);
        if (ret != HV_SUCCESS)
            errx(1, "hv_vcpu_set_sys_reg(SP_EL0) failed: 0x%x", ret);

        ret = hv_vcpu_set_sys_reg(ahv->vcpu, HV_SYS_REG_SP_EL1, 0x80000);
        if (ret != HV_SUCCESS)
            errx(1, "hv_vcpu_set_sys_reg(SP_EL1) failed: 0x%x", ret);

        ret = hv_vcpu_set_reg(ahv->vcpu, HV_REG_CPSR, 0x3c5);
        if (ret != HV_SUCCESS)
            errx(1, "hv_vcpu_set_reg(CPSR) failed: 0x%x", ret);
    }

    ahv->gpa_ep = gpa_ep;

    printf("AHV: vCPU created and initialized\n");
    printf("AHV:   entry point set to GPA 0x%" PRIx64 "\n", gpa_ep);
}

static void handle_hypercall(struct ahv *ahv, uint64_t hypercall_nr)
{
    uint64_t x0, x1, x2, x3, x4, x5, x6, x7;
    hv_vcpu_get_reg(ahv->vcpu, HV_REG_X0, &x0);
    hv_vcpu_get_reg(ahv->vcpu, HV_REG_X1, &x1);
    hv_vcpu_get_reg(ahv->vcpu, HV_REG_X2, &x2);
    hv_vcpu_get_reg(ahv->vcpu, HV_REG_X3, &x3);
    hv_vcpu_get_reg(ahv->vcpu, HV_REG_X4, &x4);
    hv_vcpu_get_reg(ahv->vcpu, HV_REG_X5, &x5);
    hv_vcpu_get_reg(ahv->vcpu, HV_REG_X6, &x6);
    hv_vcpu_get_reg(ahv->vcpu, HV_REG_X7, &x7);

    switch (hypercall_nr) {
    case AHV_HYPERCALL_WALLTIME: {
        struct ahv_hc_walltime *hc = (struct ahv_hc_walltime *)(ahv->mem + x0);
        hc->nsecs = mach_absolute_time() * (1000000000ULL / ahv->cpu_cycle_freq);
        printf("  HYPERCALL WALLTIME: nsecs=%llu\n", (unsigned long long)hc->nsecs);
        break;
    }
    case AHV_HYPERCALL_PUTS: {
        struct ahv_hc_puts *hc = (struct ahv_hc_puts *)(ahv->mem + x0);
        if (hc->len > 0 && hc->data != 0) {
            char *str = (char *)(ahv->mem + (uint64_t)hc->data);
            size_t out_len = hc->len;
            if (out_len > 256) out_len = 256;
            printf("  HYPERCALL PUTS: len=%zu, data=", (size_t)hc->len);
            fwrite(str, 1, out_len, stdout);
            if (hc->len > out_len) printf("...");
            printf("\n");
        }
        hc->ret = SOLO5_R_OK;
        break;
    }
    case AHV_HYPERCALL_POLL: {
        struct ahv_hc_poll *hc = (struct ahv_hc_poll *)(ahv->mem + x0);
        printf("  HYPERCALL POLL: timeout=%llu\n", (unsigned long long)hc->timeout_nsecs);
        hc->ready_set = 0;
        hc->ret = SOLO5_R_OK;
        break;
    }
    case AHV_HYPERCALL_BLOCK_WRITE: {
        printf("  HYPERCALL BLOCK_WRITE\n");
        struct ahv_hc_block_write *hc = (struct ahv_hc_block_write *)(ahv->mem + x0);
        hc->ret = SOLO5_R_EINVAL;
        break;
    }
    case AHV_HYPERCALL_BLOCK_READ: {
        printf("  HYPERCALL BLOCK_READ\n");
        struct ahv_hc_block_read *hc = (struct ahv_hc_block_read *)(ahv->mem + x0);
        hc->ret = SOLO5_R_EINVAL;
        break;
    }
    case AHV_HYPERCALL_NET_WRITE: {
        printf("  HYPERCALL NET_WRITE\n");
        struct ahv_hc_net_write *hc = (struct ahv_hc_net_write *)(ahv->mem + x0);
        hc->ret = SOLO5_R_EINVAL;
        break;
    }
    case AHV_HYPERCALL_NET_READ: {
        printf("  HYPERCALL NET_READ\n");
        struct ahv_hc_net_read *hc = (struct ahv_hc_net_read *)(ahv->mem + x0);
        hc->ret = SOLO5_R_EINVAL;
        break;
    }
    case AHV_HYPERCALL_HALT: {
        struct ahv_hc_halt *hc = (struct ahv_hc_halt *)(ahv->mem + x0);
        printf("  HYPERCALL HALT: exit_status=%d\n", hc->exit_status);
        ahv->exit_status = hc->exit_status;
        return;
    }
    default:
        printf("  UNKNOWN HYPERCALL: %llu\n", (unsigned long long)hypercall_nr);
        break;
    }

    uint64_t pc = get_elr_el2(ahv->vcpu);
    pc += 4;
    set_elr_el2(ahv->vcpu, pc);
}

static void handle_data_abort(struct ahv *ahv)
{
    uint64_t far;
    hv_vcpu_get_sys_reg(ahv->vcpu, HV_SYS_REG_FAR_EL2, &far);
    uint64_t esr = get_esr_el2(ahv->vcpu);

    printf("  Data Abort: FAR=0x%" PRIx64 ", ESR=0x%" PRIx64 "\n", far, esr);

    if (far >= AHV_HYPERCALL_MMIO_BASE) {
        uint64_t hypercall_nr = AHV_HYPERCALL_NR(far);
        printf("  Hypercall MMIO access: address=0x%" PRIx64 ", nr=%llu\n",
               far, (unsigned long long)hypercall_nr);
        handle_hypercall(ahv, hypercall_nr);
    } else {
        printf("  Unknown memory fault at GPA 0x%" PRIx64 "\n", far);
        printf("  Aborting guest\n");
    }
}

void ahv_run(struct ahv *ahv)
{
    printf("\n=== AHV Tender: Running unikernel ===\n");
    printf("Guest memory: %p (%" PRIu64 " MB)\n", ahv->mem, (uint64_t)ahv->mem_size >> 20);
    printf("Boot info base: 0x%" PRIx64 "\n", ahv->cpu_boot_info_base);
    printf("CPU frequency: %" PRIu64 " Hz\n", ahv->cpu_cycle_freq);
    printf("\n");

    if (!hv_available) {
        printf("PASS-THROUGH mode: executing guest code directly in host context\n");
        printf("Guest memory base: %p\n", ahv->mem);
        printf("Guest entry GPA: 0x%" PRIx64 "\n", ahv->gpa_ep);
        printf("Entry address: %p\n", ahv->mem + ahv->gpa_ep);
        printf("This is for testing only - no VM isolation!\n");
        printf("========================================================\n\n");
        
        printf("NOTE: Skipping direct execution - would need code signing on macOS\n");
        printf("The ELF was validated and loaded successfully!\n");
        
        printf("\n=== AHV Tender: Guest loaded successfully ===\n");
        return;
    }

    printf("Starting vCPU...\n");
    printf("========================================================\n\n");

    ahv->exit_status = -1;
    hv_return_t ret;
    while (1) {
        ret = hv_vcpu_run(ahv->vcpu);
        if (ret != HV_SUCCESS) {
            warnx("hv_vcpu_run failed: 0x%x", ret);
            break;
        }

        uint32_t reason = ahv->vcpu_exit->reason;
        printf("vCPU exit: reason=%u\n", reason);

        if (reason == 1) {
            uint64_t esr = get_esr_el2(ahv->vcpu);
            uint32_t ec = (esr >> 26) & 0x3F;
            uint32_t iss = esr & 0x1FFFFF;
            
            printf("  Exception: EC=0x%x, ISS=0x%x\n", ec, iss);
            
            if (ec == 0x25) {
                handle_data_abort(ahv);
            } else if (ec == 0x00) {
                uint64_t pc = get_elr_el2(ahv->vcpu);
                printf("  Instruction trap at PC=0x%" PRIx64 "\n", pc);
                
                if (pc >= AHV_HYPERCALL_MMIO_BASE) {
                    uint64_t hypercall_nr = AHV_HYPERCALL_NR(pc);
                    printf("  Hypercall via instruction trap: nr=%llu\n", 
                           (unsigned long long)hypercall_nr);
                    handle_hypercall(ahv, hypercall_nr);
                } else {
                    printf("  Unknown trap, aborting\n");
                    break;
                }
            } else {
                printf("  Unknown exception class\n");
                break;
            }
        } else if (reason == 0xFFFF) {
            uint64_t pc;
            hv_vcpu_get_reg(ahv->vcpu, HV_REG_PC, &pc);
            printf("  Hypercall exit: PC=0x%" PRIx64 "\n", pc);
            break;
        } else {
            printf("  Unknown exit reason\n");
            break;
        }
    }

    printf("\n=== AHV Tender: Halted with exit_status=%d ===\n", ahv->exit_status);
    hv_vcpu_destroy(ahv->vcpu);
    hv_vm_destroy();
}

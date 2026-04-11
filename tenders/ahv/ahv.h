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
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * ahv.h: AHV tender internal API definitions.
 */

#ifndef AHV_H
#define AHV_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <Hypervisor/Hypervisor.h>

#include "../common/cc.h"
#include "../common/elf_structs.h"
#include "../common/mft.h"
#define AHV_HOST
#include "ahv_abi.h"

struct ahv {
    uint8_t *mem;
    size_t mem_size;
    uint64_t cpu_cycle_freq;
    uint64_t cpu_boot_info_base;
    uint64_t gpa_ep;
    hv_vcpu_t vcpu;
    hv_vcpu_exit_t *vcpu_exit;
    void (*hypercalls[AHV_HYPERCALL_MAX])(struct ahv *ahv, ahv_gpa_t gpa);
    int exit_status;
};

struct ahv *ahv_init(size_t mem_size);
void ahv_mem_size(size_t *mem_size);
void ahv_boot_info_init(struct ahv *ahv, uint64_t p_end, int cmdline_argc,
                         char **cmdline_argv, const struct mft *mft);
void ahv_vcpu_init(struct ahv *ahv, uint64_t gpa_ep);
void ahv_run(struct ahv *ahv);

struct ahv_module {
    const char *name;
    struct {
        int (*setup)(struct ahv *ahv, struct mft *mft);
        int (*handle_cmdarg)(char *cmdarg, struct mft *mft);
        const char *(*usage)(void);
    } ops;
};

#define DECLARE_MODULE(module_name, ...)                                       \
    static struct ahv_module __module_##module_name                            \
        __attribute((section("__TEXT,modules"), aligned(8)))                  \
        __attribute((used)) = {.name = #module_name, .ops = {__VA_ARGS__}};

#endif /* AHV_H */

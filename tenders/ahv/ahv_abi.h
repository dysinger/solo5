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
 * ahv_abi.h: AHV guest hypercall ABI definitions.
 *
 * This header file must be kept self-contained with no external dependencies
 * other than C99 headers.
 */

#ifndef AHV_ABI_H
#define AHV_ABI_H

#include <stddef.h>
#include <stdint.h>

#define AHV_ABI_VERSION 1
#define HVT_ABI_VERSION 1
#define HVT_ABI_TARGET 1

#define AHV_GUEST_MIN_BASE 0x100000

#define AHV_HYPERCALL_MMIO_BASE  (0x100000000UL)
#define AHV_HYPERCALL_ADDRESS(x) (AHV_HYPERCALL_MMIO_BASE + ((x) << 3))
#define AHV_HYPERCALL_NR(x)      (((x) - AHV_HYPERCALL_MMIO_BASE) >> 3)

#ifdef AHV_HOST
typedef uint64_t ahv_gpa_t;
#else
static inline void ahv_do_hypercall(int n, volatile void *arg)
{
    __asm__ __volatile__("str %w0, [%1]"
                         :
                         : "rZ"((uint32_t)((uint64_t)arg)),
                           "r"((uint64_t)AHV_HYPERCALL_ADDRESS(n))
                         : "memory");
}
#endif

#ifdef AHV_HOST
#define AHV_GUEST_PTR(T) ahv_gpa_t
#else
#define AHV_GUEST_PTR(T) T
#endif

struct ahv_boot_info {
    uint64_t mem_size;
    uint64_t kernel_end;
    uint64_t cpu_cycle_freq;
    AHV_GUEST_PTR(const char *) cmdline;
    AHV_GUEST_PTR(const void *) mft;
};

#define AHV_CMDLINE_SIZE 8192

enum ahv_hypercall {
    AHV_HYPERCALL_RESERVED = 0,
    AHV_HYPERCALL_WALLTIME = 1,
    AHV_HYPERCALL_PUTS,
    AHV_HYPERCALL_POLL,
    AHV_HYPERCALL_BLOCK_WRITE,
    AHV_HYPERCALL_BLOCK_READ,
    AHV_HYPERCALL_NET_WRITE,
    AHV_HYPERCALL_NET_READ,
    AHV_HYPERCALL_HALT,
    AHV_HYPERCALL_MAX
};

struct ahv_hc_walltime {
    uint64_t nsecs;
};

struct ahv_hc_puts {
    AHV_GUEST_PTR(const char *) data;
    size_t len;
    int ret;
};

struct ahv_hc_block_write {
    uint64_t handle;
    uint64_t offset;
    AHV_GUEST_PTR(const void *) data;
    size_t len;
    int ret;
};

struct ahv_hc_block_read {
    uint64_t handle;
    uint64_t offset;
    AHV_GUEST_PTR(void *) data;
    size_t len;
    int ret;
};

struct ahv_hc_net_write {
    uint64_t handle;
    AHV_GUEST_PTR(const void *) data;
    size_t len;
    int ret;
};

struct ahv_hc_net_read {
    uint64_t handle;
    AHV_GUEST_PTR(void *) data;
    size_t len;
    int ret;
};

struct ahv_hc_poll {
    uint64_t timeout_nsecs;
    uint64_t ready_set;
    int ret;
};

#define AHV_HALT_COOKIE_MAX 512

struct ahv_hc_halt {
    AHV_GUEST_PTR(void *) cookie;
    int exit_status;
};

#endif /* AHV_ABI_H */
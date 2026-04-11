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
 * ahv_core.c: Hypervisor.framework integration and hypercall handling.
 */

#define _GNU_SOURCE
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/select.h>
#include <unistd.h>
#include <sys/time.h>
#include <pthread.h>
#include <signal.h>
#include <Hypervisor/Hypervisor.h>

// Early debug function using raw syscall
static void early_debug(const char *msg) {
    write(STDERR_FILENO, msg, strlen(msg));
}

// Also write to a debug file
static FILE *debug_file = NULL;

static void early_debug_file(const char *msg) {
    if (!debug_file) {
        debug_file = fopen("/tmp/ahv_v17.log", "w");
    }
    if (debug_file) {
        fputs(msg, debug_file);
        fflush(debug_file);
        fsync(fileno(debug_file));
    }
}

#include "ahv.h"
#include "solo5.h"
#include "../common/tap_attach.h"
#include "../common/mft.h"

// Define result codes from solo5.h
#ifndef SOLO5_R_EINVAL
#define SOLO5_R_EINVAL SOLO5_R_EUNSPEC
#endif
#ifndef SOLO5_R_AGAIN
#define SOLO5_R_AGAIN 1
#endif

static uint64_t get_elr_el1(hv_vcpu_t vcpu)
{
    uint64_t elr;
    hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ELR_EL1, &elr);
    return elr;
}

static void set_elr_el1(hv_vcpu_t vcpu, uint64_t val)
{
    hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_ELR_EL1, val);
}

static uint64_t get_esr_el1(hv_vcpu_t vcpu)
{
    uint64_t esr;
    hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ESR_EL1, &esr);
    return esr;
}

// Debug: dump vCPU state
static void dump_vcpu_state(hv_vcpu_t vcpu) {
    uint64_t pc, x0, spsr;
    hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc);
    hv_vcpu_get_reg(vcpu, HV_REG_X0, &x0);
    hv_vcpu_get_reg(vcpu, HV_REG_CPSR, &spsr);
    
    char buf[256];
    snprintf(buf, sizeof(buf), "vCPU: PC=0x%" PRIx64 " X0=0x%" PRIx64 " SPSR=0x%" PRIx64 "\n", pc, x0, spsr);
    early_debug_file(buf);
}

// Timeout thread - 30 second timeout for initial run
static void *timeout_thread_long(void *arg) {
    hv_vcpu_t *vcpu_ptr = (hv_vcpu_t *)arg;
    
    printf("AHV: Long timeout thread: starting 30 second timeout...\n");
    fflush(stdout);
    
    struct timespec ts;
    ts.tv_sec = 30;
    ts.tv_nsec = 0;
    nanosleep(&ts, NULL);
    
    printf("AHV: Long timeout thread: expired, calling hv_vcpus_exit...\n");
    fflush(stdout);
    hv_vcpus_exit(vcpu_ptr, 1);
    
    return NULL;
}

// Timeout thread - 5 second timeout for main loop
static void *timeout_thread(void *arg) {
    hv_vcpu_t *vcpu_ptr = (hv_vcpu_t *)arg;
    
    printf("AHV: Timeout thread: starting 5 second timeout...\n");
    fflush(stdout);
    
    struct timespec ts;
    ts.tv_sec = 5;
    ts.tv_nsec = 0;
    nanosleep(&ts, NULL);
    
    printf("AHV: Timeout thread: expired, calling hv_vcpus_exit...\n");
    fflush(stdout);
    hv_vcpus_exit(vcpu_ptr, 1);
    
    return NULL;
}

#define GUEST_MMAP_BASE 0x80000000  // Map at GPA 0x80000000 (like Apple working sample)
#define GUEST_MMAP_SIZE 0x20000000  // 512MB

static int block_fd = -1;
static off_t block_size = 0;
static int net_fd = -1;
static int net_listen_fd = -1;
static uint8_t net_mac[6];
static int net_mtu = 1500;
static bool net_attached = false;

void ahv_mem_size(size_t *mem_size)
{
    if (*mem_size == 0)
        *mem_size = GUEST_MMAP_SIZE;
    else if (*mem_size > GUEST_MMAP_SIZE)
        *mem_size = GUEST_MMAP_SIZE;
}

struct ahv *ahv_init(size_t mem_size)
{
    printf("AHV: Initializing with %zu MB memory\n", mem_size / (1024 * 1024));
    
    struct ahv *ahv = calloc(1, sizeof(struct ahv));
    if (!ahv)
        err(1, "malloc ahv");
    
    ahv->mem_size = mem_size;
    ahv->cpu_cycle_freq = 2400000000;  // 2.4GHz (Apple Silicon)
    ahv->exit_status = -1;  // Not yet exited
    
    // Use valloc for guest memory (required for hv_vm_map to work on Apple Silicon)
    ahv->mem = valloc(mem_size);
    if (!ahv->mem)
        err(1, "valloc guest memory");
    memset(ahv->mem, 0, mem_size);
    
    printf("AHV: Guest memory allocated at %p\n", ahv->mem);
    
    // Open a default block device (could be overridden via command line)
    const char *block_path = getenv("AHV_BLOCK_DEVICE");
    if (block_path) {
        block_fd = open(block_path, O_RDWR);
        if (block_fd >= 0) {
            block_size = lseek(block_fd, 0, SEEK_END);
            printf("AHV: Block device opened: %s (size: %ld bytes)\n", block_path, block_size);
        }
    }
    
    // Open network device using tap interface
    const char *net_iface = getenv("AHV_NET_DEVICE");
    printf("AHV: AHV_NET_DEVICE='%s'\n", net_iface ? net_iface : "(not set)");
    if (net_iface) {
        // Check for special "socket:" prefix - create a Unix socket for external bridge
        if (strncmp(net_iface, "socket:", 7) == 0) {
            // Create Unix domain socket for external connection
            const char *socket_path = net_iface + 7;
            unlink(socket_path);  // Remove old socket
            
            net_fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (net_fd >= 0) {
                struct sockaddr_un addr;
                memset(&addr, 0, sizeof(addr));
                addr.sun_family = AF_UNIX;
                strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
                
                if (bind(net_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
                    close(net_fd);
                    net_fd = -1;
                    printf("AHV: socket bind failed: %s\n", strerror(errno));
                } else if (listen(net_fd, 1) < 0) {
                    close(net_fd);
                    net_fd = -1;
                    printf("AHV: socket listen failed: %s\n", strerror(errno));
                } else {
                    net_mtu = 1500;
                    net_attached = true;
                    net_listen_fd = net_fd;  // Keep listening socket
                    printf("AHV: Listening on socket: %s\n", socket_path);
                    printf("AHV: waiting for external connection...\n");
                    
                    // Accept one connection
                    int client_fd = accept(net_fd, NULL, NULL);
                    if (client_fd >= 0) {
                        net_fd = client_fd;
                        printf("AHV: Connected to external bridge!\n");
                    } else {
                        net_fd = -1;
                        printf("AHV: accept failed: %s\n", strerror(errno));
                    }
                }
            }
        } else {
            int mtu = 0;
            net_fd = tap_attach(net_iface, &mtu);
            printf("AHV: tap_attach returned fd=%d\n", net_fd);
            if (net_fd >= 0) {
                net_mtu = mtu;
                tap_attach_genmac(net_mac);
                net_attached = true;
                printf("AHV: Network device attached: %s (mtu=%d, mac=%02x:%02x:%02x:%02x:%02x:%02x)\n",
                       net_iface, net_mtu, net_mac[0], net_mac[1], net_mac[2], net_mac[3], net_mac[4], net_mac[5]);
            } else {
                printf("AHV: Network attach failed: %s (errno=%d)\n", strerror(errno), errno);
            }
        }
    }
    
    return ahv;
}

void ahv_boot_info_init(struct ahv *ahv, uint64_t p_end, int cmdline_argc,
                         char **cmdline_argv, const struct mft *mft)
{
    struct ahv_boot_info *bi = (struct ahv_boot_info *)ahv->mem;
    
    bi->mem_size = ahv->mem_size;
    bi->kernel_end = p_end;
    bi->cpu_cycle_freq = ahv->cpu_cycle_freq;
    bi->cmdline = 0;
    bi->mft = (uint64_t)mft;  // Pass manifest pointer to guest
    
    ahv->cpu_boot_info_base = 0;
    
    // Update manifest entries with our device info
    if (mft && net_attached) {
        for (unsigned i = 0; i < mft->entries; i++) {
            if (mft->e[i].type == MFT_DEV_NET_BASIC) {
                // Update MAC address and MTU in manifest
                // Note: In a real implementation, we'd copy this to guest memory
                printf("AHV: Setting up network device in manifest: %s\n", mft->e[i].name);
            }
        }
    }
    
    printf("AHV: Boot info initialized at GPA 0x0 with mft=%p\n", mft);
}

void ahv_vcpu_init(struct ahv *ahv, uint64_t gpa_ep)
{
    hv_return_t ret;
    
    // Create VM
    ret = hv_vm_create(NULL);
    if (ret != HV_SUCCESS)
        errx(1, "hv_vm_create failed: 0x%x", ret);
    
    printf("AHV: VM created\n");
    
    // Map guest memory (already allocated with posix_memalign in ahv_init)
    ret = hv_vm_map(ahv->mem, GUEST_MMAP_BASE, ahv->mem_size,
                    HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC);
    if (ret != HV_SUCCESS)
        errx(1, "hv_vm_map failed: 0x%x", ret);
    
    printf("AHV: Guest memory mapped to GPA 0x%x\n", GUEST_MMAP_BASE);
    
    // Create vCPU
    ret = hv_vcpu_create(&ahv->vcpu, &ahv->vcpu_exit, NULL);
    if (ret != HV_SUCCESS)
        errx(1, "hv_vcpu_create failed: 0x%x", ret);
    
    printf("AHV: vCPU created (handle=%lu)\n", (unsigned long)(size_t)ahv->vcpu);
    
    ahv->gpa_ep = gpa_ep;
    
    // Calculate actual entry GPA: GUEST_MMAP_BASE + gpa_ep
    uint64_t actual_entry = GUEST_MMAP_BASE + gpa_ep;  // 0x80000000 + 0x100000 = 0x800100000
    hv_vcpu_set_reg(ahv->vcpu, HV_REG_PC, actual_entry);
    printf("AHV: PC set to 0x%" PRIx64 " (entry at GPA = 0x%x + 0x%" PRIx64 ")\n", 
           actual_entry, GUEST_MMAP_BASE, gpa_ep);
    
// Set CPSR - 0x3c4 = EL1t (matches working Apple sample)
    hv_vcpu_set_reg(ahv->vcpu, HV_REG_CPSR, 0x3c4);  // EL1t (required for HVC trap)
    printf("AHV: CPSR set to 0x3c4 (EL1t)\n");
    
    // Set X0 to entry point (reset vector jumps to it)
    hv_vcpu_set_reg(ahv->vcpu, HV_REG_X0, gpa_ep);
    printf("AHV: X0 set to 0x%" PRIx64 " (entry GPA)\n", gpa_ep);
    
    // Debug: print first 16 bytes at entry in guest memory
    uint32_t *entry_code = (uint32_t *)(ahv->mem + gpa_ep);
    printf("AHV: Entry code at 0x%" PRIx64 ": 0x%08x 0x%08x 0x%08x 0x%08x\n",
           gpa_ep, entry_code[0], entry_code[1], entry_code[2], entry_code[3]);
    
    // Set VBAR_EL1 at GPA 0x80001000 (like Apple working sample)
    uint64_t vbar = 0x80001000;
    hv_vcpu_set_sys_reg(ahv->vcpu, HV_SYS_REG_VBAR_EL1, vbar);
    printf("AHV: VBAR_EL1 set to 0x%" PRIx64 "\n", vbar);
    
    // Set SP_EL0 and SP_EL1 (like Apple working sample)
    hv_vcpu_set_sys_reg(ahv->vcpu, HV_SYS_REG_SP_EL0, 0x80004000);
    hv_vcpu_set_sys_reg(ahv->vcpu, HV_SYS_REG_SP_EL1, 0x80008000);
    printf("AHV: SP_EL0=0x80004000 SP_EL1=0x80008000\n");
    
    // Set SP_EL1 at higher address  
    uint64_t sp = 0x80200000;
    hv_vcpu_set_sys_reg(ahv->vcpu, HV_SYS_REG_SP_EL1, sp);
    printf("AHV: SP_EL1 set to 0x%" PRIx64 "\n", sp);
    
    // Write reset vector at GPA 0x80001000 (offset 0x1000 in mapped memory)
    uint32_t *vectors = (uint32_t *)(ahv->mem + 0x1000);
    // at 0x1000: mov pc, x0
    vectors[0] = 0xD6800020;  // mov pc, x0
    // at 0x1004: infinite loop  
    vectors[1] = 0x14000000;  // b .
    // at 0x1008: another loop
    vectors[2] = 0x14000000;  // b .
    // at 0x100C: another loop  
    vectors[3] = 0x14000000;  // b .
    printf("AHV: Wrote reset vector at 0x1000 (mov pc, x0)\n");
    
    // Disable vTimer for now (causes early exit)
    // hv_vcpu_set_vtimer_mask(ahv->vcpu, false);
    printf("AHV: vTimer disabled for test\n");
    
    // Enable BRK trap
    hv_vcpu_set_trap_debug_exceptions(ahv->vcpu, true);
    printf("AHV: BRK trap enabled\n");
    
    // Debug: check PC before running
    uint64_t pc_before;
    hv_vcpu_get_reg(ahv->vcpu, HV_REG_PC, &pc_before);
    printf("AHV: PC before running = 0x%" PRIx64 "\n", pc_before);
    
    printf("AHV: vCPU initialized\n");
}

static void handle_hypercall(struct ahv *ahv, uint64_t hypercall_nr, uint64_t x0_arg)
{
    uint64_t x0 = x0_arg;
    printf("handle_hypercall: nr=%llu X0=0x%llx (from arg)\n", (unsigned long long)hypercall_nr, (unsigned long long)x0);
    
    const char *hc_name = "UNKNOWN";
    switch (hypercall_nr) {
        case 1: hc_name = "WALLTIME"; break;
        case 2: hc_name = "PUTS"; break;
        case 3: hc_name = "POLL"; break;
        case 4: hc_name = "BLOCK_WRITE"; break;
        case 5: hc_name = "BLOCK_READ"; break;
        case 6: hc_name = "NET_WRITE"; break;
        case 7: hc_name = "NET_READ"; break;
        case 8: hc_name = "HALT"; break;
    }
    printf("AHV: Hypercall %llu %s (X0=0x%" PRIx64 ")\n", 
           (unsigned long long)hypercall_nr, hc_name, x0);
    fflush(stdout);
    
    uint64_t gpa = x0;
    printf("    Hypercall struct at GPA 0x%" PRIx64 "\n", gpa);
    
    switch (hypercall_nr) {
    case AHV_HYPERCALL_WALLTIME: {
        printf("  HYPERCALL WALLTIME\n");
        fflush(stdout);
        struct ahv_hc_walltime *hc = (struct ahv_hc_walltime *)(ahv->mem + x0);
        hc->nsecs = 0;  // Would use mach_absolute_time() in production
        break;
    }
    case AHV_HYPERCALL_PUTS: {
        printf("HYPERCALL PUTS\n");
        fflush(stdout);
        struct ahv_hc_puts *hc = (struct ahv_hc_puts *)(ahv->mem + x0);
        uint64_t data_ptr = hc->data;
        
        printf("  data_ptr=0x%" PRIx64 ", len=%zu\n", data_ptr, hc->len);
        fflush(stdout);
        
        if (hc->len > 0 && data_ptr < ahv->mem_size && data_ptr >= 0x100000) {
            char *str = (char *)(ahv->mem + data_ptr);
            printf("GUEST: ");
            fwrite(str, 1, hc->len, stdout);
            printf("\n");
            fflush(stdout);
        }
        hc->ret = hc->len;
        break;
    }
case AHV_HYPERCALL_POLL: {
        struct ahv_hc_poll *hc = (struct ahv_hc_poll *)(ahv->mem + x0);
        // POLL checks network socket: first try to accept, then check for data
        if (net_listen_fd >= 0) {
            // Poll listen socket non-blocking to see if connection pending
            struct pollfd lpfd;
            lpfd.fd = net_listen_fd;
            lpfd.events = POLLIN;
            lpfd.revents = 0;
            poll(&lpfd, 1, 0);
            
            if (lpfd.revents & POLLIN) {
                // Connection waiting - accept it
                int new_fd = accept(net_listen_fd, NULL, NULL);
                if (new_fd >= 0) {
                    if (net_fd >= 0) close(net_fd);
                    net_fd = new_fd;
                }
            }
        }
        
        // Now check connected socket
        if (net_fd >= 0) {
            struct pollfd pfd;
            pfd.fd = net_fd;
            pfd.events = POLLIN;
            pfd.revents = 0;
            poll(&pfd, 1, 0);
            
            if (pfd.revents & POLLIN) {
                hc->ready_set = 1;
                hc->ret = 1;
            } else if (pfd.revents & (POLLHUP|POLLERR)) {
                close(net_fd);
                net_fd = -1;
                hc->ready_set = 0;
                hc->ret = 0;
            } else {
                hc->ready_set = 0;
                hc->ret = 0;
            }
        } else {
            hc->ready_set = 0;
            hc->ret = 0;
        }
        break;
    }
    case AHV_HYPERCALL_BLOCK_WRITE: {
        printf("  HYPERCALL BLOCK_WRITE\n");
        struct ahv_hc_block_write *hc = (struct ahv_hc_block_write *)(ahv->mem + x0);
        
        if (block_fd < 0) {
            printf("    No block device configured (set AHV_BLOCK_DEVICE)\n");
            hc->ret = SOLO5_R_EUNSPEC;
        } else if (hc->handle != 0) {
            hc->ret = SOLO5_R_EINVAL;
        } else {
            uint64_t data_ptr = (uint64_t)hc->data;
            if (data_ptr >= ahv->mem_size || 
                hc->offset + hc->len > (uint64_t)block_size) {
                hc->ret = SOLO5_R_EINVAL;
            } else {
                // Actually do the write
                lseek(block_fd, hc->offset, SEEK_SET);
                ssize_t written = write(block_fd, ahv->mem + data_ptr, hc->len);
                if (written == (ssize_t)hc->len) {
                    hc->ret = 0;
                    printf("    Wrote %zu bytes to block device\n", hc->len);
                } else {
                    hc->ret = SOLO5_R_EUNSPEC;
                }
            }
        }
        break;
    }
    case AHV_HYPERCALL_BLOCK_READ: {
        printf("  HYPERCALL BLOCK_READ\n");
        struct ahv_hc_block_read *hc = (struct ahv_hc_block_read *)(ahv->mem + x0);
        
        if (block_fd < 0) {
            printf("    No block device configured\n");
            hc->ret = SOLO5_R_EUNSPEC;
        } else if (hc->handle != 0) {
            hc->ret = SOLO5_R_EINVAL;
        } else {
            uint64_t data_ptr = (uint64_t)hc->data;
            if (data_ptr >= ahv->mem_size || 
                hc->offset + hc->len > (uint64_t)block_size) {
                hc->ret = SOLO5_R_EINVAL;
            } else {
                lseek(block_fd, hc->offset, SEEK_SET);
                ssize_t r = read(block_fd, ahv->mem + data_ptr, hc->len);
                if (r == (ssize_t)hc->len) {
                    hc->ret = 0;
                    printf("    Read %zu bytes from block device\n", hc->len);
                } else {
                    hc->ret = SOLO5_R_EUNSPEC;
                }
            }
        }
        break;
    }
    case AHV_HYPERCALL_NET_WRITE: {
        struct ahv_hc_net_write *hc = (struct ahv_hc_net_write *)(ahv->mem + x0);
        
        if (net_fd < 0) {
            hc->ret = SOLO5_R_EUNSPEC;
        } else if (hc->handle != 0) {
            hc->ret = SOLO5_R_EINVAL;
        } else {
            uint64_t data_ptr = (uint64_t)hc->data;
            if (data_ptr >= ahv->mem_size || hc->len == 0 || hc->len > (size_t)(net_mtu + 14)) {
                hc->ret = SOLO5_R_EINVAL;
            } else {
                ssize_t written = write(net_fd, ahv->mem + data_ptr, hc->len);
                if (written == (ssize_t)hc->len) {
                    hc->ret = SOLO5_R_OK;
                } else {
                    hc->ret = SOLO5_R_EUNSPEC;
                }
            }
        }
        break;
    }
    case AHV_HYPERCALL_NET_READ: {
        struct ahv_hc_net_read *hc = (struct ahv_hc_net_read *)(ahv->mem + x0);
        
        if (net_fd < 0) {
            // No connected socket - try to accept
            if (net_listen_fd >= 0) {
                net_fd = accept(net_listen_fd, NULL, NULL);
                if (net_fd < 0) {
                    hc->ret = SOLO5_R_AGAIN;
                    break;
                }
            } else {
                hc->ret = SOLO5_R_EUNSPEC;
                break;
            }
        }
        
        if (hc->handle != 0) {
            hc->ret = SOLO5_R_EINVAL;
        } else {
            uint64_t data_ptr = (uint64_t)hc->data;
            if (data_ptr >= ahv->mem_size || hc->len < 14) {
                hc->ret = SOLO5_R_EINVAL;
            } else {
                struct pollfd pfd;
                pfd.fd = net_fd;
                pfd.events = POLLIN;
                pfd.revents = 0;
                int ready = poll(&pfd, 1, 0);  // Non-blocking
                
                if (ready > 0 && (pfd.revents & POLLIN)) {
                    ssize_t r = read(net_fd, ahv->mem + data_ptr, hc->len);
                    if (r > 0) {
                        hc->len = r;
                        hc->ret = SOLO5_R_OK;
                    } else if (r == 0) {
                        // Connection closed - close and re-accept
                        close(net_fd);
                        net_fd = -1;
                        hc->ret = SOLO5_R_AGAIN;
                    } else {
                        hc->ret = SOLO5_R_EUNSPEC;
                    }
                } else if (ready == 0) {
                    hc->ret = SOLO5_R_AGAIN;
                } else {
                    // Error - re-accept
                    close(net_fd);
                    net_fd = -1;
                    hc->ret = SOLO5_R_AGAIN;
                }
            }
        }
        break;
    }
    case AHV_HYPERCALL_HALT: {
        printf("  HYPERCALL HALT\n");
        fflush(stdout);
        struct ahv_hc_halt *hc = (struct ahv_hc_halt *)(ahv->mem + x0);
        printf("    exit_status=%d, cookie=%p\n", hc->exit_status, (void*)hc->cookie);
        fflush(stdout);
        ahv->exit_status = hc->exit_status;
        printf("AHV: Unikernel requested halt with status %d\n", hc->exit_status);
        break;
    }
    default:
        printf("  UNKNOWN HYPERCALL: %llu\n", (unsigned long long)hypercall_nr);
        break;
    }

    // Advance PC by 4 (since hypercalls are triggered by memory access)
    uint64_t pc;
    hv_vcpu_get_reg(ahv->vcpu, HV_REG_PC, &pc);
    hv_vcpu_set_reg(ahv->vcpu, HV_REG_PC, pc + 4);
}

static void handle_data_abort(struct ahv *ahv)
{
    uint64_t esr = get_esr_el1(ahv->vcpu);
    uint64_t far;
    hv_vcpu_get_sys_reg(ahv->vcpu, HV_SYS_REG_FAR_EL1, &far);
    uint64_t x0;
    hv_vcpu_get_reg(ahv->vcpu, HV_REG_X0, &x0);
    
    printf("AHV: Data Abort: FAR=0x%" PRIx64 ", ESR=0x%" PRIx64 "\n", far, esr);
    fflush(stdout);
    
    // Check if this is a hypercall (MMIO access to hypercall region)
    // The hypercall region is at 0x100000000+
    if (far >= AHV_HYPERCALL_MMIO_BASE) {
        uint64_t hypercall_nr = AHV_HYPERCALL_NR(far);
        printf("AHV: -> Hypercall MMIO access: address=0x%" PRIx64 ", nr=%llu\n",
               far, (unsigned long long)hypercall_nr);
        fflush(stdout);
        handle_hypercall(ahv, hypercall_nr, x0);
    } else {
        printf("AHV: Unknown memory fault at GPA 0x%" PRIx64 "\n", far);
        fflush(stdout);
        
        // For now, halt on unknown faults
        ahv->exit_status = 1;
    }
}

static void handle_exception(struct ahv *ahv)
{
    uint64_t esr = get_esr_el1(ahv->vcpu);
    uint32_t ec = (esr >> 26) & 0x3F;
    uint64_t far;
    hv_vcpu_get_sys_reg(ahv->vcpu, HV_SYS_REG_FAR_EL1, &far);
    uint64_t pc;
    hv_vcpu_get_reg(ahv->vcpu, HV_REG_PC, &pc);
    uint64_t x0;
    hv_vcpu_get_reg(ahv->vcpu, HV_REG_X0, &x0);
    
    printf("EXCEPTION: EC=0x%x, ESR=0x%" PRIx64 ", FAR=0x%" PRIx64 ", PC=0x%" PRIx64 ", X0=0x%" PRIx64 "\n", ec, esr, far, pc, x0);
    fflush(stdout);
    
    if (ec == 0x16) {
        // HVC (Hypervisor Call) - treat as hypercall
        printf("EXCEPTION: HVC at PC=0x%" PRIx64 "\n", pc);
        uint64_t x0, x1;
        hv_vcpu_get_reg(ahv->vcpu, HV_REG_X0, &x0);
        hv_vcpu_get_reg(ahv->vcpu, HV_REG_X1, &x1);
        printf("EXCEPTION: HVC: X0=0x%" PRIx64 " X1=0x%" PRIx64 "\n", x0, x1);
        // X1 = hypercall number, X0 = argument GPA
        handle_hypercall(ahv, x1, x0);
    } else if (ec == 0x25) {
        // Data Abort
        handle_data_abort(ahv);
    } else if (ec == 0x00) {
        // SVC (system call)
        uint64_t pc;
        hv_vcpu_get_reg(ahv->vcpu, HV_REG_PC, &pc);
        printf("EXCEPTION: SVC at PC=0x%" PRIx64 "\n", pc);
        hv_vcpu_set_reg(ahv->vcpu, HV_REG_PC, pc + 4);
    } else if (ec == 0x20 || ec == 0x21) {
        // Instruction abort
        printf("EXCEPTION: Instruction abort at PC=0x%" PRIx64 "\n", pc);
    } else {
        printf("EXCEPTION: Unhandled EC=0x%x\n", ec);
        ahv->exit_status = 1;
    }
}

static pthread_t timeout_tid;

static void *timeout_thread_30s(void *arg) {
    hv_vcpu_t vcpu = (hv_vcpu_t)arg;
    printf("AHV: Timer: starting 0.1s timeout...\n");
    fflush(stdout);
    usleep(100000);  // 100ms = very fast check
    
    uint64_t pc, x0, x1, x2, esr, far;
    hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc);
    hv_vcpu_get_reg(vcpu, HV_REG_X0, &x0);
    hv_vcpu_get_reg(vcpu, HV_REG_X1, &x1);
    hv_vcpu_get_reg(vcpu, HV_REG_X2, &x2);
    hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ESR_EL1, &esr);
    hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_FAR_EL1, &far);
    uint32_t ec = (esr >> 26) & 0x3F;
    printf("AHV: Timer @3s: PC=0x%" PRIx64 " X0=0x%" PRIx64 " X1=0x%" PRIx64 " X2=0x%" PRIx64 "\n", pc, x0, x1, x2);
    printf("AHV: Timer ESR=0x%" PRIx64 " EC=0x%x FAR=0x%" PRIx64 "\n", esr, ec, far);
    fflush(stdout);
    
    printf("AHV: Timer: calling hv_vcpus_exit...\n");
    hv_vcpus_exit(&vcpu, 1);
    printf("AHV: Timer: after exit\n");
    return NULL;
}

void ahv_run(struct ahv *ahv)
{
    printf("=== AHV Tender: Running unikernel (v20) ===\n");
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
    
    printf("AHV: Starting vCPU loop\n");
    fflush(stdout);
    
    // Use hv_vcpu_run - on Apple Silicon blocks until VMexit
    // NOTE: Timer causes early exit due to hv_vcpus_exit resetting guest state
    // pthread_t timer_tid;
    // pthread_create(&timer_tid, NULL, timeout_thread_30s, (void*)ahv->vcpu);
    // printf("Added timer thread to trigger exit\n");
    
    printf("AHV: Calling hv_vcpu_run (will block until exception)...\n");
    fflush(stdout);
    
    hv_return_t r;
    uint32_t reason;
    
    r = hv_vcpu_run(ahv->vcpu);
    reason = ahv->vcpu_exit->reason;
    
    // ALWAYS print reason on first return
    printf("AHV: hv_vcpu_run returned r=%d reason=%u\n", (int)r, reason);
    if (reason == 1) {
        uint64_t esr = get_esr_el1(ahv->vcpu);
        uint32_t ec = (esr >> 26) & 0x3F;
        printf("AHV: Exception: EC=0x%x ESR=0x%" PRIx64 "\n", ec, esr);
    }
    fflush(stdout);
    uint64_t pc, x0, x1;
    hv_vcpu_get_reg(ahv->vcpu, HV_REG_PC, &pc);
    hv_vcpu_get_reg(ahv->vcpu, HV_REG_X0, &x0);
    hv_vcpu_get_reg(ahv->vcpu, HV_REG_X1, &x1);
    printf("AHV: vCPU run completed r=%d reason=%u PC=0x%" PRIx64 " X0=0x%" PRIx64 " X1=0x%" PRIx64 "\n", 
           (int)r, reason, pc, x0, x1);
    fflush(stdout);
    
    // Handle the result (hypercall, exception, etc.)
    if (reason == 0) {
        printf("AHV: vCPU canceled (external signal)\n");
    } else if (reason == 1) {
        // Exception - may be hypercall
        uint64_t esr = get_esr_el1(ahv->vcpu);
        uint32_t ec = (esr >> 26) & 0x3F;
        uint64_t far;
        hv_vcpu_get_sys_reg(ahv->vcpu, HV_SYS_REG_FAR_EL1, &far);
        uint64_t pc;
        hv_vcpu_get_reg(ahv->vcpu, HV_REG_PC, &pc);
        
        printf("AHV: Exception after first run: EC=0x%x ESR=0x%" PRIx64 " FAR=0x%" PRIx64 " PC=0x%" PRIx64 "\n", 
               ec, esr, far, pc);
        
// Handle EC=0x0 - HVC exception (no ESR set)
        if (ec == 0x0) {
            // Get registers for EC=0x0 path
            uint64_t rx0, rx1, rx2;
            hv_vcpu_get_reg(ahv->vcpu, HV_REG_X0, &rx0);
            hv_vcpu_get_reg(ahv->vcpu, HV_REG_X1, &rx1);
            hv_vcpu_get_reg(ahv->vcpu, HV_REG_X2, &rx2);
            
            // X2 = hypercall number
            uint64_t hc_nr = rx2;
            printf("AHV: Hypercall nr=%llu X0=%llu X1=%llu\n", 
                   (unsigned long long)hc_nr, (unsigned long long)x0, (unsigned long long)x1);
            
            if (hc_nr == 5) {
                // PUTS: rx0=len, rx1=gpa of string
                uint64_t offset = rx1 - GUEST_MMAP_BASE;
                if (offset < ahv->mem_size && rx0 > 0) {
                    char *msg = (char *)(ahv->mem + offset);
                    printf("AHV: >>>%s<<<\n", msg);
                }
                // DON'T set exit_status - continue loop for more hypercalls!
            } else if (hc_nr == 2) {
                // PUTS - print from guest memory
                uint64_t offset = rx1 - GUEST_MMAP_BASE;
                if (offset < ahv->mem_size && rx0 > 0) {
                    char *msg = (char *)(ahv->mem + offset);
                    printf("AHV: >>>%s<<<\n", msg);
                }
            } else if (hc_nr == 11) {
                // HALT
                printf("AHV: HALT code=%llu\n", (unsigned long long)rx0);
                ahv->exit_status = (int)rx0;
                printf("AHV: Early exit, status=%d\n", ahv->exit_status);
                goto done;
            } else if (hc_nr == 4 || hc_nr == 5) {
                // BLOCK_WRITE (4) or BLOCK_READ (5) - handle now
                printf("AHV: BLOCK_%s handled\n", hc_nr == 4 ? "WRITE" : "READ");
                hv_vcpu_get_reg(ahv->vcpu, HV_REG_X0, &x0);
                handle_hypercall(ahv, hc_nr, x0);
            } else if (hc_nr == 6 || hc_nr == 7) {
                // NET_WRITE (6) or NET_READ (7) - handle now  
                printf("AHV: NET_%s handled\n", hc_nr == 6 ? "WRITE" : "READ");
                hv_vcpu_get_reg(ahv->vcpu, HV_REG_X0, &x0);
                handle_hypercall(ahv, hc_nr, x0);
                // Continue loop - don't exit
            } else {
                printf("AHV: Unknown hypercall %llu, exiting\n", (unsigned long long)hc_nr);
                ahv->exit_status = 0;
            }
            
            // Advance PC past HVC instruction
            hv_vcpu_set_reg(ahv->vcpu, HV_REG_PC, pc + 4);
            
            // Check if HALT was called
            if (ahv->exit_status >= 0) {
                printf("AHV: Early exit after first run, status=%d\n", ahv->exit_status);
                goto done;
            }
        } else if (ec == 0x16) {
            // HVC exception - hypercall
            uint64_t x0, x1, x2;
            hv_vcpu_get_reg(ahv->vcpu, HV_REG_X0, &x0);
            hv_vcpu_get_reg(ahv->vcpu, HV_REG_X1, &x1);
            hv_vcpu_get_reg(ahv->vcpu, HV_REG_X2, &x2);
            printf("AHV: HVC hypercall: X0=%llu X1=%llu X2=%llu\n", 
                   (unsigned long long)x0, (unsigned long long)x1, (unsigned long long)x2);
            
            // X2 = hypercall number, X1 = arg (from ADR)
            uint64_t hc_nr = x2;
            uint64_t hc_arg = x1;
            
            // Simple raw hypercall handling
            if (hc_nr == 5) {
                // PUTS - print from guest memory
                char *msg = (char *)(ahv->mem + pc + 8);
                printf("AHV: PUTS: %s", msg);
                // Don't exit - continue for more hypercalls
            } else if (hc_nr == 7 || hc_nr == 8) {
                // BLOCK_WRITE/BLOCK_READ - call handler
                printf("AHV: Early handler: BLOCK_%s\n", hc_nr == 7 ? "WRITE" : "READ");
                handle_hypercall(ahv, hc_nr, x0);
            } else if (hc_nr == 9 || hc_nr == 10) {
                // NET_WRITE/NET_READ - call handler
                printf("AHV: Early handler: NET_%s\n", hc_nr == 9 ? "WRITE" : "READ");
                handle_hypercall(ahv, hc_nr, x0);
            } else if (hc_nr == 11) {
                // HALT
                printf("AHV: HALT (code=%llu)\n", (unsigned long long)hc_arg);
                ahv->exit_status = (int)hc_arg;
            } else {
                printf("AHV: Unknown raw hypercall %llu\n", (unsigned long long)hc_nr);
                ahv->exit_status = 0;
            }
            // Advance PC past HVC instruction
            hv_vcpu_set_reg(ahv->vcpu, HV_REG_PC, pc + 4);
            
            if (ahv->exit_status >= 0) {
                printf("AHV: Early exit after ec==0x16, status=%d\n", ahv->exit_status);
                goto done;
            }
        } else if (ec == 0x25 && far >= AHV_HYPERCALL_MMIO_BASE) {
            uint64_t hc_nr = AHV_HYPERCALL_NR(far);
            printf("AHV: Hypercall detected: nr=%llu\n", (unsigned long long)hc_nr);
            handle_hypercall(ahv, hc_nr, x0);
        } else if (ec == 0x18 || ec == 0x19) {
            printf("AHV: Timer interrupt - continuing\n");
        } else {
            printf("AHV: Unknown exception type, exiting\n");
            ahv->exit_status = 1;
        }
    } else if (reason == 2) {
        printf("AHV: vTimer interrupt\n");
    }
    fflush(stdout);
    
    // Continue loop for more hypercalls/exceptions
    int iter = 1;
    while (ahv->exit_status < 0 && iter < 100000) {
        r = hv_vcpu_run(ahv->vcpu);
        reason = ahv->vcpu_exit->reason;
        
        if (iter < 5) {
            printf("AHV: iter=%d r=%d reason=%u\n", iter, (int)r, reason);
            fflush(stdout);
        }
        
        if (reason == 0) {
            printf("AHV: vCPU canceled at iter=%d\n", iter);
            break;
        }
        
        if (reason == 2) {
            iter++;
            continue;
        }
        
        if (reason == 1) {
            uint64_t esr = get_esr_el1(ahv->vcpu);
            uint32_t ec = (esr >> 26) & 0x3F;
            uint64_t far;
            hv_vcpu_get_sys_reg(ahv->vcpu, HV_SYS_REG_FAR_EL1, &far);
            uint64_t pc;
            hv_vcpu_get_reg(ahv->vcpu, HV_REG_PC, &pc);
            uint64_t x0, x1, x2, x3;
            hv_vcpu_get_reg(ahv->vcpu, HV_REG_X0, &x0);
            hv_vcpu_get_reg(ahv->vcpu, HV_REG_X1, &x1);
            hv_vcpu_get_reg(ahv->vcpu, HV_REG_X2, &x2);
            hv_vcpu_get_reg(ahv->vcpu, HV_REG_X3, &x3);
            
            printf("AHV: Exception at iter=%d: EC=0x%x ESR=0x%" PRIx64 " FAR=0x%" PRIx64 " PC=0x%" PRIx64 "\n", 
                   iter, ec, esr, far, pc);
            printf("AHV:   X0=0x%" PRIx64 " X1=0x%" PRIx64 " X2=0x%" PRIx64 " X3=0x%" PRIx64 "\n",
                   x0, x1, x2, x3);
            
            // Always continue (don't exit) except for fatal errors
            printf("AHV: Handling exception, continuing...\n");
            fflush(stdout);
            
            if (ec == 0x16) {
                // HVC (Hypervisor Call) - hypercall nr in X2
                printf("AHV: HVC hypercall at iter=%d\n", iter);
                uint64_t x0 = 0, x2 = 0;
                hv_vcpu_get_reg(ahv->vcpu, HV_REG_X0, &x0);
                hv_vcpu_get_reg(ahv->vcpu, HV_REG_X2, &x2);
                printf("AHV: HVC: X0=0x%" PRIx64 " X2=%llu\n", x0, (unsigned long long)x2);
                handle_hypercall(ahv, x2, x0);
                if (ahv->exit_status >= 0) {
                    printf("AHV: Exit requested, breaking loop\n");
                    break;
                }
                iter++;
                continue;
            }
            
            if (ec == 0x0) {
                // HVC exception - hypercall number in X2, struct address in X1 (from ADR instruction)
                uint64_t x0, x1, x2;
                hv_vcpu_get_reg(ahv->vcpu, HV_REG_X0, &x0);
                hv_vcpu_get_reg(ahv->vcpu, HV_REG_X1, &x1);
                hv_vcpu_get_reg(ahv->vcpu, HV_REG_X2, &x2);
                // Use X2 for hypercall number, X1 for struct address (ADR loads into X1)
                uint64_t hc_nr = x2;
                uint64_t hc_arg = x1;  // Use X1 from ADR
                printf("AHV: EC=0x0 at iter=%d: X0=%llu X1=%llu X2=%llu\n", iter, (unsigned long long)x0, (unsigned long long)x1, (unsigned long long)x2);
                
                if (hc_nr == 11) {
                    // HALT
                    printf("AHV: HALT code=%llu\n", (unsigned long long)hc_arg);
                    ahv->exit_status = (int)hc_arg;
                    printf("AHV: Exit requested, breaking loop\n");
                    break;
                }
                
                // Handle other hypercalls via handle_hypercall
                handle_hypercall(ahv, hc_nr, hc_arg);
                if (ahv->exit_status >= 0) {
                    printf("AHV: Exit requested, breaking loop\n");
                    break;
                }
                iter++;
                continue;
            }
            
            if (ec == 0x18 || ec == 0x19) {
                // Timer interrupt - continue
                iter++;
                continue;
            }
            
            if (ec == 0x20 || ec == 0x21) {
                // Instruction abort
                iter++;
                continue;
            }
            
            if (ec == 0x25) {
                // Data abort - handle hypercall if in range
                if (far >= AHV_HYPERCALL_MMIO_BASE) {
                    uint64_t hc_nr = AHV_HYPERCALL_NR(far);
                    printf("AHV: Handling hypercall nr=%llu at iter=%d\n", (unsigned long long)hc_nr, iter);
                    handle_hypercall(ahv, hc_nr, x0);
                    printf("AHV: Hypercall handled, exit_status=%d\n", ahv->exit_status);
                    if (ahv->exit_status >= 0) {
                        printf("AHV: Exit requested, breaking loop\n");
                        break;
                    }
                } else if (far >= 0x100000 && far < ahv->mem_size) {
                    // Normal memory access fault - continue
                    printf("AHV: Memory fault at valid address 0x%" PRIx64 ", continuing\n", far);
                } else {
                    // Invalid address
                    printf("AHV: Invalid memory access at FAR=0x%" PRIx64 "\n", far);
                }
            }
            
            // Continue running
            iter++;
            continue;
        }
        
        iter++;
    }
    
    done:
    printf("\n=== Exited: status=%d, iterations=%d ===\n", ahv->exit_status, iter);
    
    if (block_fd >= 0)
        close(block_fd);
    
    free(ahv);
}
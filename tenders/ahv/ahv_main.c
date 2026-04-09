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
 * ahv_main.c: Minimal AHV tender main program.
 * Self-contained ELF64 parser (no system headers needed).
 */

#define _GNU_SOURCE
#include <assert.h>
#include <err.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include <inttypes.h>

#include "ahv.h"

#define EI_MAG0     0
#define EI_MAG1     1
#define EI_MAG2     2
#define EI_MAG3     3
#define EI_CLASS    4
#define EI_DATA     5

#define ELFMAG0     0x7f
#define ELFMAG1     'E'
#define ELFMAG2     'L'
#define ELFMAG3     'F'

#define ELFCLASS64  2
#define ELFDATA2LSB 1

#define PT_LOAD     1
#define PF_X        1
#define PF_W        2
#define PF_R        4

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint32_t p_align;
} Elf64_Phdr;

typedef struct {
    uint8_t e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s KERNEL\n", prog);
    fprintf(stderr, "KERNEL is the filename of the unikernel to run.\n");
    exit(1);
}

static void load_elf64(struct ahv *ahv, int fd, const char *filename, 
                        uint64_t *entry, uint64_t *end)
{
    char buf[128];
    
    if (read(fd, buf, sizeof(buf)) != sizeof(buf))
        errx(1, "%s: read error", filename);

    if (buf[EI_MAG0] != ELFMAG0 || buf[EI_MAG1] != ELFMAG1 ||
        buf[EI_MAG2] != ELFMAG2 || buf[EI_MAG3] != ELFMAG3)
        errx(1, "%s: not an ELF file", filename);

    if (buf[EI_CLASS] != ELFCLASS64)
        errx(1, "%s: not a 64-bit ELF", filename);

    if (buf[EI_DATA] != ELFDATA2LSB)
        errx(1, "%s: not little-endian", filename);

    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)buf;
    *entry = ehdr->e_entry;
    
    printf("ELF: entry=0x%" PRIx64 "\n", *entry);

    lseek(fd, 0, SEEK_SET);

    char phdr_buf[sizeof(Elf64_Phdr)];
    *end = 0;
    
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (lseek(fd, ehdr->e_phoff + (uint64_t)i * ehdr->e_phentsize, SEEK_SET) == -1)
            err(1, "lseek");
        if (read(fd, phdr_buf, ehdr->e_phentsize) != ehdr->e_phentsize)
            err(1, "read phdr");
        
        Elf64_Phdr *phdr = (Elf64_Phdr *)phdr_buf;
        if (phdr->p_type != PT_LOAD)
            continue;

        uint64_t vaddr = phdr->p_vaddr;
        uint64_t memsz = phdr->p_memsz;
        uint64_t filesz = phdr->p_filesz;
        
        printf("ELF: Loading segment: vaddr=0x%" PRIx64 ", filesz=%" PRIu64 ", memsz=%" PRIu64 "\n",
               vaddr, filesz, memsz);

        if (vaddr + memsz > ahv->mem_size)
            errx(1, "Segment extends beyond guest memory");

        if (memsz > 0) {
            memset(ahv->mem + vaddr, 0, memsz);
        }
        
        if (filesz > 0) {
            if (lseek(fd, phdr->p_offset, SEEK_SET) == -1)
                err(1, "lseek");
            ssize_t r = read(fd, ahv->mem + vaddr, filesz);
            if (r != filesz)
                errx(1, "Failed to read segment data");
        }

        uint64_t seg_end = vaddr + memsz;
        if (seg_end > *end)
            *end = seg_end;
    }

    printf("ELF: Loaded, end=0x%" PRIx64 "\n", *end);
}

int main(int argc, char **argv)
{
    const char *prog;
    const char *elf_filename;
    int elf_fd = -1;
    uint64_t p_entry = 0, p_end = 0;

    prog = basename(*argv);
    argc--;
    argv++;

    if (argc == 0)
        usage(prog);

    elf_filename = *argv;

    elf_fd = open(elf_filename, O_RDONLY);
    if (elf_fd == -1)
        err(1, "%s", elf_filename);

    size_t mem_size = 0x20000000;
    ahv_mem_size(&mem_size);

    struct ahv *ahv = ahv_init(mem_size);

    load_elf64(ahv, elf_fd, elf_filename, &p_entry, &p_end);
    close(elf_fd);

    ahv_boot_info_init(ahv, p_end, 0, NULL);

    ahv_vcpu_init(ahv, p_entry);

    ahv_run(ahv);

    return 0;
}

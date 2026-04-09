/*
 * ELF structures for macOS (which doesn't have system elf.h)
 */

#ifndef _ELF_STRUCTS_H
#define _ELF_STRUCTS_H

#include <stdint.h>

#define EI_MAG0         0
#define EI_MAG1         1
#define EI_MAG2         2
#define EI_MAG3         3
#define EI_CLASS        4
#define EI_DATA         5
#define EI_VERSION      6
#define EI_OSABI        7
#define EI_ABIVERSION  8
#define EI_PAD          9

#define ELFMAG0         0x7f
#define ELFMAG1         'E'
#define ELFMAG2         'L'
#define ELFMAG3         'F'

#define ELFCLASSNONE    0
#define ELFCLASS32      1
#define ELFCLASS64      2

#define ELFDATANONE     0
#define ELFDATA2LSB     1
#define ELFDATA2MSB     2

#define EV_NONE         0
#define EV_CURRENT      1

#define ELFOSABI_NONE   0
#define ELFOSABI_SYSV   0
#define ELFOSABI_HPUX   1
#define ELFOSABI_NETBSD 2
#define ELFOSABI_LINUX  3
#define ELFOSABI_HURD   4
#define ELFOSABI_SOLARIS 6
#define ELFOSABI_FREEBSD 9
#define ELFOSABI_OPENBSD 12

#define ET_NONE         0
#define ET_REL          1
#define ET_EXEC         2
#define ET_DYN          3
#define ET_CORE         4

#define EM_NONE         0
#define EM_X86_64       62
#define EM_AARCH64      183
#define EM_PPC64        21
#define EM_SPARC        2
#define EM_SPARCV9      43

#define PT_NULL         0
#define PT_LOAD         1
#define PT_DYNAMIC      2
#define PT_INTERP       3
#define PT_NOTE         4
#define PT_SHLIB       5
#define PT_PHDR        6
#define PT_TLS         7

#define PF_X            (1 << 0)
#define PF_W            (1 << 1)
#define PF_R            (1 << 2)

typedef struct {
    unsigned char e_ident[16];
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

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

typedef struct {
    uint32_t n_namesz;
    uint32_t n_descsz;
    uint32_t n_type;
} Elf64_Nhdr;

typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Xword;
typedef uint32_t Elf64_Word;
typedef uint32_t Elf64_Half;
typedef int64_t Elf64_Sxword;

#define ELF64_ST_BIND(i)         ((i) >> 4)
#define ELF64_ST_TYPE(i)         ((i) & 0xf)
#define ELF64_ST_INFO(b, t)      (((b) << 4) | ((t) & 0xf))

#define R_AARCH64_NONE           0
#define R_AARCH64_ABS64          257
#define R_AARCH64_JUMP26         282
#define R_X86_64_NONE            0
#define R_X86_64_64              1
#define R_X86_64_PC32            2
#define R_X86_64_GOTPCREL        9

#endif /* _ELF_STRUCTS_H */

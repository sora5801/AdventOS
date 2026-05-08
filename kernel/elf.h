#ifndef ADVENTOS_ELF_H
#define ADVENTOS_ELF_H

#include "../include/types.h"

#define EI_NIDENT 16

#define ELFCLASS32   1
#define ELFDATA2LSB  1
#define EM_386       3
#define ET_EXEC      2

#define PT_LOAD      1
#define PF_X         1
#define PF_W         2
#define PF_R         4

struct elf32_ehdr {
    uint8_t  ident[EI_NIDENT];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint32_t entry;
    uint32_t phoff;
    uint32_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} __attribute__((packed));

struct elf32_phdr {
    uint32_t type;
    uint32_t offset;
    uint32_t vaddr;
    uint32_t paddr;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t flags;
    uint32_t align;
} __attribute__((packed));

struct elf_load_result {
    uint32_t entry;
    uint32_t cr3;             /* physical address of new user PD       */
    uint32_t user_esp;        /* top of allocated user stack           */
};

/*
 * Read an ELF32 executable from filesystem entry `fs_idx`, build a
 * fresh user PD, allocate and map every PT_LOAD segment with USER
 * (and W if PF_W), allocate a one-page user stack at USER_STACK_VA,
 * and return entry/cr3/esp ready for task_create_user.
 *
 * Returns 0 on success, negative error code otherwise. On error the
 * partially-built address space is freed.
 */
int elf_load(int fs_idx, struct elf_load_result *out);

#endif

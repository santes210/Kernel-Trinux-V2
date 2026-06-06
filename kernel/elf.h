#ifndef KERNEL_ELF_H
#define KERNEL_ELF_H

#include "../lib/types.h"
#include "../fs/vfs.h"

/* ---- ELF32 structures (from the ELF specification) ---- */

#define ELF_MAGIC  0x464C457F   /* "\x7FELF" in little-endian */

/* ELF header */
typedef struct {
    uint32_t e_magic;       /* 0x7F 'E' 'L' 'F' */
    uint8_t  e_class;       /* 1 = 32-bit */
    uint8_t  e_data;        /* 1 = little-endian */
    uint8_t  e_hversion;    /* 1 */
    uint8_t  e_osabi;
    uint8_t  e_pad[8];
    uint16_t e_type;        /* 2 = ET_EXEC */
    uint16_t e_machine;     /* 3 = EM_386 */
    uint32_t e_version;
    uint32_t e_entry;       /* entry point virtual address */
    uint32_t e_phoff;       /* program header table offset */
    uint32_t e_shoff;       /* section header table offset */
    uint32_t e_flags;
    uint16_t e_ehsize;      /* ELF header size */
    uint16_t e_phentsize;   /* program header entry size */
    uint16_t e_phnum;       /* number of program headers */
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) elf32_ehdr_t;

/* Program header */
typedef struct {
    uint32_t p_type;        /* 1 = PT_LOAD */
    uint32_t p_offset;      /* offset in file */
    uint32_t p_vaddr;       /* virtual address to load at */
    uint32_t p_paddr;       /* physical address (unused) */
    uint32_t p_filesz;      /* size in file */
    uint32_t p_memsz;       /* size in memory (may be > filesz for BSS) */
    uint32_t p_flags;       /* 1=exec, 2=write, 4=read */
    uint32_t p_align;
} __attribute__((packed)) elf32_phdr_t;

#define ET_EXEC   2
#define EM_386    3
#define PT_LOAD   1

/* Load an ELF32 executable from the VFS and run it in ring 3.
 * Returns 0 on success (after the program exits), or <0 on error.
 * Errors:
 *   -1  file not found
 *   -2  not a valid ELF32 i386 executable
 *   -3  failed to load segments (out of memory)
 *   -4  entry point is 0
 */
int elf_exec(const char *path, vfs_node_t *cwd);

/* Versión con argumentos: argv ya tokenizado.
 * argv[0] suele ser el nombre del programa, argv[argc] = NULL.
 * Idéntico a elf_exec() en lo demás. */
int elf_exec_argv(const char *path, vfs_node_t *cwd, int argc, char **argv);

#endif /* KERNEL_ELF_H */

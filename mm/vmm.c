/* mm/vmm.c  -  virtual memory / paging.
 *
 * Sets up a page directory that identity-maps the first 256 MiB (kernel + heap),
 * enables paging, and installs a page-fault handler. Uses statically allocated,
 * page-aligned tables to avoid bootstrap ordering issues.
 */
#include "vmm.h"
#include "../lib/string.h"
#include "../lib/printf.h"
#include "../cpu/isr.h"
#include "../include/kernel.h"

#define PAGES_PER_TABLE 1024
#define TABLES_PER_DIR  1024
#define IDENTITY_TABLES 64              /* 64 tables * 4 MiB = 256 MiB */

static uint32_t page_directory[TABLES_PER_DIR] __attribute__((aligned(4096)));
static uint32_t page_tables[IDENTITY_TABLES][PAGES_PER_TABLE]
    __attribute__((aligned(4096)));

static bool paging_enabled;

static void load_page_directory(uint32_t pd_phys)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(pd_phys));
}

static void enable_paging(void)
{
    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000;   /* set PG bit */
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
}

static void page_fault_handler(registers_t *regs)
{
    uint32_t faulting_address;
    __asm__ volatile("mov %%cr2, %0" : "=r"(faulting_address));

    bool present = regs->err_code & 0x1;
    bool write   = regs->err_code & 0x2;
    bool user    = regs->err_code & 0x4;

    kprintf("\n*** PAGE FAULT ***\n");
    kprintf("  addr=%08x  %s %s %s\n", faulting_address,
            present ? "protection" : "not-present",
            write ? "write" : "read",
            user ? "user" : "kernel");
    kprintf("  eip=%08x\n", regs->eip);
    panic("Page fault");
}

void vmm_init(void)
{
    memset(page_directory, 0, sizeof(page_directory));

    /* Identity-map the first 256 MiB (covers kernel + the heap).
     *
     * We add PAGE_USER so ring-3 (userspace) code can execute and access its
     * own stack/data, which live in this same identity-mapped region (this is
     * a single-address-space kernel: privilege is enforced by the CPL of the
     * code segment, not by separate page tables per process). The kernel still
     * runs at CPL 0 and userspace at CPL 3 via the GDT selectors. */
    for (int t = 0; t < IDENTITY_TABLES; t++) {
        for (int p = 0; p < PAGES_PER_TABLE; p++) {
            uint32_t phys = (t * PAGES_PER_TABLE + p) * 0x1000;
            page_tables[t][p] = phys | PAGE_PRESENT | PAGE_RW | PAGE_USER;
        }
        page_directory[t] = ((uint32_t)page_tables[t])
                            | PAGE_PRESENT | PAGE_RW | PAGE_USER;
    }

    isr_register_handler(14, page_fault_handler);

    load_page_directory((uint32_t)page_directory);
    enable_paging();
    paging_enabled = true;
}

void vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags)
{
    uint32_t dir_idx = virt >> 22;
    uint32_t tbl_idx = (virt >> 12) & 0x3FF;

    if (dir_idx < IDENTITY_TABLES) {
        page_tables[dir_idx][tbl_idx] = (phys & ~0xFFF) | (flags & 0xFFF)
                                        | PAGE_PRESENT;
        /* flush TLB for this address */
        __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
    }
    /* Higher tables would need dynamic allocation; omitted for the identity
     * mapped kernel region used here. */
}

void vmm_unmap_page(uint32_t virt)
{
    uint32_t dir_idx = virt >> 22;
    uint32_t tbl_idx = (virt >> 12) & 0x3FF;
    if (dir_idx < IDENTITY_TABLES) {
        page_tables[dir_idx][tbl_idx] = 0;
        __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
    }
}

bool vmm_is_enabled(void) { return paging_enabled; }

/* mm/vmm.c  -  virtual memory / paging.
 *
 * Sets up a page directory that identity-maps the first 1 GiB (kernel + heap + user space),
 * enables paging, and installs a page-fault handler. Uses statically allocated,
 * page-aligned tables to avoid bootstrap ordering issues.
 *
 * Memory layout:
 *   0x00000000 - 0x00100000: BIOS/IVT/VGA (reserved, first 1 MiB)        [KERNEL, U/S=0]
 *   0x00100000 - ~0x03000000: Kernel code + data (~32 MiB)               [KERNEL, U/S=0]
 *   ~0x03000000 - ~0x05000000: Kernel heap (32 MiB)                      [KERNEL, U/S=0]
 *   0x05000000 - 0x40000000: Available for user processes (944 MiB)      [USER,   U/S=1]
 *   Total identity-mapped: 1 GiB
 *
 * KERNEL_END (0x05000000 = 80 MiB) marks the boundary: pages below are
 * supervisor-only (U/S=0), pages at or above are user-accessible (U/S=1).
 */
#include "vmm.h"
#include "../include/userlayout.h"
#include "../lib/string.h"
#include "../lib/printf.h"
#include "../cpu/isr.h"
#include "../include/kernel.h"
#include "../process/process.h"

#define PAGES_PER_TABLE 1024
#define TABLES_PER_DIR  1024
#define IDENTITY_TABLES 256             /* 256 tables * 4 MiB = 1024 MiB = 1 GiB */

/* Kernel/user boundary: pages < KERNEL_END_VIRT are supervisor-only (U/S=0).
 * This covers BIOS (1 MB) + kernel code/data (~32 MB) + kernel heap (~32 MB) = ~80 MB.
 * 0x05000000 = 80 MiB = 20480 pages = 20 page tables (0-19).
 * We use a generous boundary to ensure all kernel structures are protected.
 * ¡Ojo! Debe coincidir con USER_SPACE_START de include/userlayout.h. */
#define KERNEL_END_VIRT  USER_SPACE_START /* 0x05000000, ver userlayout.h */
#define KERNEL_END_TABLE (KERNEL_END_VIRT >> 22)  /* 20 = page table index */

static uint32_t page_directory[TABLES_PER_DIR] __attribute__((aligned(4096)));
static uint32_t page_tables[IDENTITY_TABLES][PAGES_PER_TABLE]
    __attribute__((aligned(4096)));





static bool paging_enabled;

uint32_t current_page_directory;

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

    /* Si el fault vino de ring 3 (user mode), matar el proceso en lugar de panic */
    if (user) {
        process_t *cur = process_get_current();
        if (cur) {
            kprintf("\n*** PAGE FAULT en ring 3 (proceso %d: %s) ***\n",
                    cur->pid, cur->name);
            kprintf("  addr=%08x  %s %s\n", faulting_address,
                    present ? "protection" : "not-present",
                    write ? "write" : "read");
            kprintf("  eip=%08x\n", regs->eip);
            kprintf("  Terminando proceso con SIGSEGV (11)\n");
            /* Enviar SIGSEGV al proceso */
            process_signal(cur->pid, 11);  /* SIGSEGV = 11 */
            return;  /* El syscall handler detectará la señal y matará el proceso */
        }
    }

    /* Fault en ring 0 (kernel) = bug serio, panic */
    kprintf("\n*** PAGE FAULT EN KERNEL ***\n");
    kprintf("  addr=%08x  %s %s %s\n", faulting_address,
            present ? "protection" : "not-present",
            write ? "write" : "read",
            user ? "user" : "kernel");
    kprintf("  eip=%08x\n", regs->eip);
    panic("Page fault en kernel");
}

void vmm_init(void)
{
    memset(page_directory, 0, sizeof(page_directory));

    /* Identity-map the first 1 GiB.
     * Pages below KERNEL_END_VIRT (80 MiB) are supervisor-only (U/S=0):
     *   - BIOS/IVT/VGA (0-1 MB)
     *   - Kernel code + data (~32 MB)
     *   - Kernel heap (~32 MB)
     * Pages at or above KERNEL_END_VIRT are user-accessible (U/S=1):
     *   - User program code/data/stack/heap
     */
    for (int t = 0; t < IDENTITY_TABLES; t++) {
        bool is_kernel_table = (t < KERNEL_END_TABLE);
        for (int p = 0; p < PAGES_PER_TABLE; p++) {
            uint32_t phys = (t * PAGES_PER_TABLE + p) * 0x1000;
            uint32_t flags = PAGE_PRESENT | PAGE_RW;
            if (!is_kernel_table) {
                flags |= PAGE_USER;  /* user-accessible above kernel boundary */
            }
            page_tables[t][p] = phys | flags;
        }
        uint32_t pd_flags = PAGE_PRESENT | PAGE_RW;
        if (!is_kernel_table) {
            pd_flags |= PAGE_USER;
        }
        page_directory[t] = ((uint32_t)page_tables[t]) | pd_flags;
    }

    isr_register_handler(14, page_fault_handler);

    load_page_directory((uint32_t)page_directory);
    current_page_directory = (uint32_t)page_directory;
    enable_paging();
    paging_enabled = true;
}

/* Crea una page table propia para `dir_idx` en el page directory `pd_phys`:
 * pide un frame al PMM, copia las entradas de la page table MAESTRA del
 * kernel (para heredar el identity-map) y devuelve la dirección virtual de la
 * nueva tabla.  Devuelve NULL si no hay memoria.
 *
 * Antes, vmm_map_page()/vmm_map_page_in() reciclaban un pool estático de 8
 * tablas EXTRA de 4 KB cada una para TODO el sistema.  Con 32 bps (1366x768)
 * el framebuffer necesita ~3 de esas tablas y el heap de usuario (SYS_BRK)
 * otra; dos procesos y regiones MMIO de AHCI/xHCI y el pool se agotaba. */
static uint32_t *new_page_table_for(uint32_t *pd, uint32_t dir_idx)
{
    extern uint32_t pmm_alloc_frame(void);
    uint32_t pt_phys = pmm_alloc_frame();
    if (!pt_phys) return NULL;

    uint32_t *pt = (uint32_t *)pt_phys;
    if (dir_idx < IDENTITY_TABLES) {
        memcpy(pt, page_tables[dir_idx], PAGES_PER_TABLE * 4);
    } else {
        memset(pt, 0, PAGES_PER_TABLE * 4);
    }
    pd[dir_idx] = pt_phys | PAGE_PRESENT | PAGE_RW | PAGE_USER;
    return pt;
}

void vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags)
{
    uint32_t dir_idx = virt >> 22;
    uint32_t tbl_idx = (virt >> 12) & 0x3FF;

    if (dir_idx < IDENTITY_TABLES) {
        /* Fast path: within the statically identity-mapped region. */
        page_tables[dir_idx][tbl_idx] = (phys & ~0xFFF) | (flags & 0xFFF)
                                        | PAGE_PRESENT;
    } else {
        /* Higher addresses (MMIO like AHCI ABAR at 0xFEBxxxxx, or the VBE
         * framebuffer at 0xFDxxxxxx).  Pedimos la page table al PMM: antes
         * se tiraba de un pool estático de 8 tablas que se agotaba con un
         * framebuffer de 1366x768 + un par de regiones MMIO. */
        uint32_t *pt;
        if (!(page_directory[dir_idx] & PAGE_PRESENT)) {
            pt = new_page_table_for(page_directory, dir_idx);
            if (!pt) return;              /* out of memory */
        } else {
            pt = (uint32_t *)(page_directory[dir_idx] & ~0xFFF);
        }
        pt[tbl_idx] = (phys & ~0xFFF) | (flags & 0xFFF) | PAGE_PRESENT;
    }

    /* Flush TLB for this address. */
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

void vmm_unmap_page(uint32_t virt)
{
    uint32_t dir_idx = virt >> 22;
    uint32_t tbl_idx = (virt >> 12) & 0x3FF;

    if (dir_idx < IDENTITY_TABLES) {
        page_tables[dir_idx][tbl_idx] = 0;
    } else if (page_directory[dir_idx] & PAGE_PRESENT) {
        uint32_t *pt = (uint32_t *)(page_directory[dir_idx] & ~0xFFF);
        pt[tbl_idx] = 0;
    }
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

bool vmm_is_enabled(void) { return paging_enabled; }



uint32_t vmm_get_current_dir(void) {
    return current_page_directory;
}

void vmm_switch_address_space(uint32_t pd_phys) {
    current_page_directory = pd_phys;
    __asm__ volatile("mov %0, %%cr3" : : "r"(pd_phys));
}

uint32_t vmm_create_address_space(void) {
    extern uint32_t pmm_alloc_frame(void);
    uint32_t pd_phys = pmm_alloc_frame();
    if (!pd_phys) return 0;

    uint32_t* pd = (uint32_t*)(pd_phys);
    /* copy kernel identity mapping (first 1 GiB) with kernel/user split */
    for (int i = 0; i < 1024; i++) {
        if (i < IDENTITY_TABLES) {
            /* For tables 0-19 (kernel), strip PAGE_USER; for 20+, keep it */
            if (i < KERNEL_END_TABLE) {
                pd[i] = page_directory[i] & ~PAGE_USER;  /* supervisor-only */
            } else {
                pd[i] = page_directory[i];  /* user-accessible */
            }
        } else {
            pd[i] = 0;
        }
    }
    return pd_phys;
}

/* ============================================================================
 * vmm_map_page_private_in — mapea `virt -> phys` en una page table PRIVADA
 * del pd indicado (nunca en la tabla global compartida del kernel).
 *
 * Por qué existe: vmm_create_address_space() hace una copia *superficial* del
 * kernel, así que las entradas del hijo apuntan a las mismas page_tables[]
 * estáticas.  En el modelo actual (espacio de direcciones único
 * identity-mapped) eso es justo lo que queremos para exec/brk: phys == virt,
 * el PTE que se escribe es idéntico al que ya había y el mapeo se ve en
 * cualquier contexto.  Pero en fork() phys != virt: escribir ahí cambiaba el
 * PTE de la tabla GLOBAL del kernel, es decir, un solo fork() remapeaba
 * 0x08048000..0x0F000000 a los frames privados del hijo para TODOS los
 * procesos (padre incluido) -> corrupción silenciosa de memoria.
 *
 * Esta función copia la tabla compartida a un frame propio antes de tocar
 * el PTE, de modo que el hijo obtiene páginas realmente privadas.
 * ========================================================================== */
void vmm_map_page_private_in(uint32_t pd_phys, uint32_t virt,
                             uint32_t phys, uint32_t flags)
{
    extern uint32_t pmm_alloc_frame(void);
    uint32_t *pd = (uint32_t *)pd_phys;
    uint32_t dir_idx = virt >> 22;
    uint32_t tbl_idx = (virt >> 12) & 0x3FF;
    if (dir_idx >= 1024) return;

    bool shared_with_kernel = (pd[dir_idx] & PAGE_PRESENT) &&
                              ((pd[dir_idx] & ~0xFFFu) ==
                               (page_directory[dir_idx] & ~0xFFFu));

    if (!(pd[dir_idx] & PAGE_PRESENT) || shared_with_kernel) {
        uint32_t pt_phys = pmm_alloc_frame();
        if (!pt_phys) return;
        uint32_t *pt = (uint32_t *)pt_phys;
        if (shared_with_kernel)
            memcpy(pt, (void *)(pd[dir_idx] & ~0xFFFu), PAGES_PER_TABLE * 4);
        else
            memset(pt, 0, PAGES_PER_TABLE * 4);
        pd[dir_idx] = pt_phys | PAGE_PRESENT | PAGE_RW | PAGE_USER;
    }

    uint32_t *pt = (uint32_t *)(pd[dir_idx] & ~0xFFF);
    pt[tbl_idx] = (phys & ~0xFFF) | (flags & 0xFFF) | PAGE_PRESENT;

    /* Si estamos tocando el pd activo, invalidar la entrada en la TLB. */
    if (pd_phys == current_page_directory)
        __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

/* Mapea `virt -> phys` en el pd indicado.  OJO: si la entrada del pd ya
 * apunta a una page table compartida con el kernel (lo normal, porque
 * vmm_create_address_space() clona el identity-map), el PTE se escribe en esa
 * tabla GLOBAL.  Eso es correcto —y deliberado— solo cuando phys == virt
 * (modelo identity-mapped actual: el mapeo ya era exactamente ese, así que
 * es idempotente y visible en todos los contextos).  Para mapear un frame
 * distinto (fork) usar vmm_map_page_private_in(). */
void vmm_map_page_in(uint32_t pd_phys, uint32_t virt, uint32_t phys, uint32_t flags) {
    extern uint32_t pmm_alloc_frame(void);
    uint32_t* pd = (uint32_t*)pd_phys;
    uint32_t dir_idx = virt >> 22;
    uint32_t tbl_idx = (virt >> 12) & 0x3FF;

    if (!(pd[dir_idx] & PAGE_PRESENT)) {
        uint32_t* pt = new_page_table_for(pd, dir_idx);
        if (!pt) return;
        /* La tabla nueva se hereda del kernel y conserva su bit U/S, así que
         * si intentamos mapear una página de USUARIO en una zona marcada
         * como supervisor-only el acceso desde ring 3 daría page fault más
         * tarde y de forma difícil de depurar. Lo arreglamos aquí. */
        if (flags & PAGE_USER)
            pd[dir_idx] |= PAGE_USER;
    }

    uint32_t* pt = (uint32_t*)(pd[dir_idx] & ~0xFFF);
    pt[tbl_idx] = (phys & ~0xFFF) | (flags & 0xFFF) | PAGE_PRESENT;
}

void vmm_free_address_space(uint32_t pd_phys) {
    extern void pmm_free_frame(uint32_t);
    extern bool pmm_is_allocated(uint32_t addr);

    if (!pd_phys || pd_phys == (uint32_t)page_directory) return;
    uint32_t* pd = (uint32_t*)pd_phys;

    /* Liberar SOLO lo que este address space posee de verdad.
     *
     * vmm_create_address_space() hace una copia SUPERFICIAL del kernel: las
     * entradas comparten las page tables globales page_tables[] (memoria
     * estática del .bss, no frames del PMM).  Por eso:
     *
     *   1. Si la entrada apunta a la MISMA page table que el directorio
     *      maestro, esa tabla y todas sus páginas son del kernel (o del
     *      identity-map compartido) → no se tocan.
     *   2. Si la tabla es privada, sus páginas solo se liberan cuando el
     *      frame está marcado como asignado en el PMM y el mapeo NO es el
     *      mismo que el del kernel para esa dirección virtual.
     *
     * La versión anterior recorría desde IDENTITY_TABLES y liberaba cada
     * página presente: como en el identity-map TODO está presente, cada
     * process_exit() devolvía al PMM memoria que nunca le perteneció
     * (kstack, heap, frames de otros procesos), y el kernel acababa
     * entregando el mismo frame a dos usuarios. */
    for (int i = IDENTITY_TABLES; i < 1024; i++) {
        if (!(pd[i] & PAGE_PRESENT)) continue;
        if ((pd[i] & ~0xFFFu) == (page_directory[i] & ~0xFFFu)) continue;  /* compartida */

        uint32_t pt_phys = pd[i] & ~0xFFF;
        if (!pmm_is_allocated(pt_phys)) continue;

        uint32_t* pt = (uint32_t*)pt_phys;
        for (int j = 0; j < 1024; j++) {
            if (!(pt[j] & PAGE_PRESENT)) continue;
            uint32_t frame = pt[j] & ~0xFFF;

            /* ¿El kernel mapea exactamente lo mismo en esta VA? */
            uint32_t master = page_tables[i][j];
            if ((master & PAGE_PRESENT) && (master & ~0xFFF) == frame)
                continue;                              /* página compartida  */

            if (pmm_is_allocated(frame))
                pmm_free_frame(frame);
        }
        pmm_free_frame(pt_phys);
    }
    pmm_free_frame(pd_phys);
}

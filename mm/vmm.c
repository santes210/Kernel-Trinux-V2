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
 * We use a generous boundary to ensure all kernel structures are protected. */
#define KERNEL_END_VIRT  0x05000000U    /* 80 MiB */
#define KERNEL_END_TABLE (KERNEL_END_VIRT >> 22)  /* 20 = page table index */

/* Región de usuario (VA 0x08000000-0x10000000) = tablas 32..63.
 * En el PD del kernel quedan identity-mapped (legado, inerte), pero en
 * cada proceso viven en page tables PRIVADAS (v0.6.0). */
#define USER_TABLE_FIRST  32
#define USER_TABLE_LAST   64             /* exclusivo */

/* PMM reserva los primeros 256 MiB: todo frame entregado por
 * pmm_alloc_frame() es >= 0x10000000 (ver mm/pmm.c). */
#define PMM_FRAME_MIN     0x10000000U

/* Extra page tables for on-demand MMIO mappings (AHCI ABAR, etc.).
 * These cover addresses above the 1 GiB identity-mapped region.
 * We keep a small pool of 8 tables — enough for mapping a handful of
 * MMIO regions without needing the heap or the PMM. */
#define EXTRA_TABLES    8

static uint32_t page_directory[TABLES_PER_DIR] __attribute__((aligned(4096)));
static uint32_t page_tables[IDENTITY_TABLES][PAGES_PER_TABLE]
    __attribute__((aligned(4096)));
static uint32_t extra_tables[EXTRA_TABLES][PAGES_PER_TABLE]
    __attribute__((aligned(4096)));
static uint32_t extra_used;   /* how many extra tables have been allocated */




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
            /* FIX (v0.5.3): antes solo se marcaba signal_pending=SIGSEGV y
             * se retornaba, así que el iret re-ejecutaba LA MISMA
             * instrucción -> page fault -> return -> page fault... bucle
             * infinito que colgaba el SO (la señal pendiente solo se
             * procesaba al salir de un syscall, y este fault no venía de
             * ninguno). Terminar el proceso AHORA, como hace el manejador
             * genérico de excepciones de isr.c. */
            extern bool usermode_fault_kill(int signal_code);
            usermode_fault_kill(-14);   /* 14 = page fault */
            return;  /* inalcanzable con jump buffer armado; defensivo */
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

    /* Identity-map the first 1 GiB, con politica por rangos (v0.6.0):
     *   tablas  0-19  (0-80 MiB)      : kernel — supervisor solo
     *   tablas 20-31  (80-128 MiB)    : reservado PMM — supervisor solo
     *   tablas 32-63  (128-256 MiB)   : region de VA de USUARIO (U/S=1)
     *   tablas 64-255 (256 MiB-1 GiB) : physmap de frames PMM dinamicos
     *                                   — SUPERVISOR SOLO (cerrado en
     *                                   v0.6.0: ver vmm.c y AUDITORIA).
     *
     * Antes todo lo >= 80 MiB era accesible desde ring 3: cualquier
     * proceso podia leer/escribir CUALQUIER frame fisico del sistema
     * usando la direccion fisica como VA.  Ahora ring 3 solo puede
     * tocar su region privada (32-63) via sus propias page tables. */
    for (int t = 0; t < IDENTITY_TABLES; t++) {
        bool is_user_table = (t >= USER_TABLE_FIRST && t < USER_TABLE_LAST);
        for (int p = 0; p < PAGES_PER_TABLE; p++) {
            uint32_t phys = (t * PAGES_PER_TABLE + p) * 0x1000;
            uint32_t flags = PAGE_PRESENT | PAGE_RW;
            if (is_user_table) {
                flags |= PAGE_USER;
            }
            page_tables[t][p] = phys | flags;
        }
        uint32_t pd_flags = PAGE_PRESENT | PAGE_RW;
        if (is_user_table) {
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

void vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags)
{
    uint32_t dir_idx = virt >> 22;
    uint32_t tbl_idx = (virt >> 12) & 0x3FF;

    if (dir_idx < IDENTITY_TABLES) {
        /* Fast path: within the statically identity-mapped region. */
        page_tables[dir_idx][tbl_idx] = (phys & ~0xFFF) | (flags & 0xFFF)
                                        | PAGE_PRESENT;
    } else {
        /* Higher addresses (e.g. MMIO like AHCI ABAR at 0xFEBxxxxx).
         * Allocate an extra page table from the static pool if needed. */
        if (!(page_directory[dir_idx] & PAGE_PRESENT)) {
            if (extra_used >= EXTRA_TABLES)
                return;   /* out of extra tables */
            memset(extra_tables[extra_used], 0, sizeof(extra_tables[0]));
            page_directory[dir_idx] = ((uint32_t)extra_tables[extra_used])
                                      | PAGE_PRESENT | PAGE_RW;
            extra_used++;
        }
        /* Find the page table for this directory entry. */
        uint32_t *pt = (uint32_t *)(page_directory[dir_idx] & ~0xFFF);
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

/* Quita el bit PAGE_USER de la página identity-mapped que contiene `phys`
 * (solo frames dentro del 1 GiB identity-mapped).
 *
 * SECURITY (v0.5.3): las tablas 20-255 del identity-map tienen U/S=1, así
 * que cualquier frame dinámico del PMM (>= 256 MiB tras el fix del PMM)
 * era accesible directamente desde ring 3 por su VA idéntica — incluidos
 * los page directories y page tables: un proceso podía editar las
 * estructuras de paginación del kernel. Toda estructura interna alojada
 * en un frame dinámico debe llamar a esto tras allocarlo. */
void vmm_deprivilege_identity_page(uint32_t phys)
{
    if (phys >= ((uint32_t)IDENTITY_TABLES << 22))
        return;
    uint32_t t = phys >> 22;
    uint32_t p = (phys >> 12) & 0x3FF;
    if (page_tables[t][p] & PAGE_PRESENT) {
        page_tables[t][p] &= ~PAGE_USER;
        __asm__ volatile("invlpg (%0)" : : "r"(phys) : "memory");
    }
}

/* Restaura la identidad (virt==phys, USER) de las páginas de usuario que
 * fork() remapeó a frames copiados en las page tables COMPARTIDAS, y libera
 * esos frames. Sin esto, al morir un hijo de fork sus PTEs quedaban
 * apuntando a frames liberados al PMM: frames dangling visibles (y
 * escribibles) desde ring 3. Idempotente. */
void vmm_restore_user_identity(void)
{
    uint32_t first = 0x08000000u >> 22;   /* tablas 32.. */
    uint32_t last  = 0x10000000u >> 22;   /* ..63 (exclusivo) */
    extern void pmm_free_frame(uint32_t);
    for (uint32_t t = first; t < last && t < IDENTITY_TABLES; t++) {
        for (uint32_t p = 0; p < PAGES_PER_TABLE; p++) {
            uint32_t e = page_tables[t][p];
            if (!(e & PAGE_PRESENT)) continue;
            uint32_t virt = (t * PAGES_PER_TABLE + p) * 0x1000u;
            uint32_t phys = e & ~0xFFFu;
            if (phys != virt) {
                pmm_free_frame(phys);
                page_tables[t][p] = virt | PAGE_PRESENT | PAGE_RW | PAGE_USER;
                __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
            }
        }
    }
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
    /* v0.6.0 — address space REAL:
     *   - tablas 0-31  : compartidas con el kernel (supervisor)
     *   - tablas 32-63 : PRIVADAS (page tables nuevas, vacías) — aquí
     *     vive todo el mundo del proceso (code/data/bss/heap/stack)
     *   - tablas 64-255: compartidas (physmap supervisor de frames PMM)
     *   - tablas 256+  : snapshot de las PDEs del kernel (MMIO: AHCI,
     *     xHCI, APIC...) para que los drivers sigan funcionando cuando
     *     una syscall corra con CR3 = este page directory.
     */
    extern uint32_t pmm_alloc_frame(void);
    extern void     pmm_free_frame(uint32_t);
    uint32_t pd_phys = pmm_alloc_frame();
    if (!pd_phys) return 0;
    /* Page directory = estructura del kernel: fuera del alcance de ring 3. */
    vmm_deprivilege_identity_page(pd_phys);

    uint32_t *pd = (uint32_t *)pd_phys;   /* frames PMM: identity VA ok en ring 0 */
    for (int i = 0; i < 1024; i++)
        pd[i] = page_directory[i];

    for (int i = USER_TABLE_FIRST; i < USER_TABLE_LAST; i++) {
        uint32_t pt_phys = pmm_alloc_frame();
        if (!pt_phys) {
            /* rollback de las page tables ya creadas */
            for (int k = USER_TABLE_FIRST; k < i; k++)
                pmm_free_frame(pd[k] & ~0xFFFu);
            pmm_free_frame(pd_phys);
            return 0;
        }
        vmm_deprivilege_identity_page(pt_phys);
        memset((void *)pt_phys, 0, 4096);   /* región privada arranca VACÍA */
        pd[i] = pt_phys | PAGE_PRESENT | PAGE_RW | PAGE_USER;
    }
    return pd_phys;
}

uint32_t vmm_kernel_dir(void) { return (uint32_t)page_directory; }

/* Devuelve la page table PRIVADA (32-63) que cubre `virt`, o NULL. */
static uint32_t *user_pt(uint32_t *pd, uint32_t virt)
{
    uint32_t dir_idx = virt >> 22;
    if (dir_idx < USER_TABLE_FIRST || dir_idx >= USER_TABLE_LAST) return 0;
    if (!(pd[dir_idx] & PAGE_PRESENT)) return 0;
    return (uint32_t *)(pd[dir_idx] & ~0xFFFu);
}

int vmm_copy_to_user(uint32_t pd_phys, uint32_t va, const void *src, uint32_t len)
{
    uint32_t *pd = (uint32_t *)pd_phys;
    const uint8_t *s = (const uint8_t *)src;
    while (len) {
        uint32_t *pt = user_pt(pd, va);
        if (!pt) return -1;
        uint32_t pte = pt[(va >> 12) & 0x3FF];
        if (!(pte & PAGE_PRESENT)) return -1;
        uint32_t off  = va & 0xFFF;
        uint32_t chunk = 4096 - off;
        if (chunk > len) chunk = len;
        /* El frame es PMM (>= 256 MiB): escribible vía identity VA en
         * ring 0 con CUALQUIER CR3 activo (physmap supervisor compartido). */
        memcpy((void *)((pte & ~0xFFFu) + off), s, chunk);
        s += chunk; va += chunk; len -= chunk;
    }
    return 0;
}

void vmm_reset_user_region(uint32_t pd_phys)
{
    extern void pmm_free_frame(uint32_t);
    uint32_t *pd = (uint32_t *)pd_phys;
    for (int t = USER_TABLE_FIRST; t < USER_TABLE_LAST; t++) {
        if (!(pd[t] & PAGE_PRESENT)) continue;
        uint32_t *pt = (uint32_t *)(pd[t] & ~0xFFFu);
        for (int p = 0; p < PAGES_PER_TABLE; p++) {
            uint32_t pte = pt[p];
            if (!(pte & PAGE_PRESENT)) continue;
            uint32_t phys = pte & ~0xFFFu;
            uint32_t virt = (uint32_t)((t * PAGES_PER_TABLE + p)) * 0x1000u;
            /* Solo frames PMM privados (nunca identidad heredada). */
            if (phys != virt && phys >= PMM_FRAME_MIN)
                pmm_free_frame(phys);
            pt[p] = 0;
        }
    }
}

int vmm_copy_user_space(uint32_t dst_pd, uint32_t src_pd)
{
    extern uint32_t pmm_alloc_frame(void);
    uint32_t *dst = (uint32_t *)dst_pd;
    uint32_t *src = (uint32_t *)src_pd;
    for (int t = USER_TABLE_FIRST; t < USER_TABLE_LAST; t++) {
        if (!(src[t] & PAGE_PRESENT)) continue;
        uint32_t *spt = (uint32_t *)(src[t] & ~0xFFFu);
        uint32_t *dpt = (uint32_t *)(dst[t] & ~0xFFFu);
        for (int p = 0; p < PAGES_PER_TABLE; p++) {
            uint32_t pte = spt[p];
            if (!(pte & PAGE_PRESENT)) continue;
            uint32_t sphys = pte & ~0xFFFu;
            uint32_t virt  = (uint32_t)((t * PAGES_PER_TABLE + p)) * 0x1000u;
            if (sphys == virt) {
                /* Página identity heredada (no debería existir en v0.6.0,
                 * pero si aparece: compartir la misma entrada — la memoria
                 * reservada < 256 MiB no es reclaimable). */
                dpt[p] = pte;
                continue;
            }
            uint32_t nf = pmm_alloc_frame();
            if (!nf) return -1;
            memcpy((void *)nf, (void *)sphys, 4096);
            vmm_deprivilege_identity_page(nf);
            dpt[p] = nf | (pte & 0xFFFu) | PAGE_PRESENT;
        }
    }
    return 0;
}

void vmm_map_page_in(uint32_t pd_phys, uint32_t virt, uint32_t phys, uint32_t flags) {
    extern uint32_t pmm_alloc_frame(void);
    uint32_t* pd = (uint32_t*)pd_phys;
    uint32_t dir_idx = virt >> 22;
    uint32_t tbl_idx = (virt >> 12) & 0x3FF;

    if (!(pd[dir_idx] & PAGE_PRESENT)) {
        uint32_t pt_phys = pmm_alloc_frame();
        if (!pt_phys) return;
        /* Nueva page table = estructura del kernel: fuera de ring 3. */
        vmm_deprivilege_identity_page(pt_phys);
        uint32_t* pt = (uint32_t*)pt_phys;
        for (int i = 0; i < 1024; i++) pt[i] = 0;
        pd[dir_idx] = pt_phys | PAGE_PRESENT | PAGE_RW | PAGE_USER;
    }

    uint32_t* pt = (uint32_t*)(pd[dir_idx] & ~0xFFF);
    pt[tbl_idx] = (phys & ~0xFFF) | (flags & 0xFFF) | PAGE_PRESENT;

    /* v0.6.0: si el PD destino es el ACTIVO (ej. SYS_BRK mapeando en el
     * heap del proceso en curso), invalidar la entrada de TLB: el page-walk
     * cache puede retener el "not-present" anterior y aun así faultar. */
    if (pd_phys == current_page_directory)
        __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

void vmm_free_address_space(uint32_t pd_phys) {
    extern void pmm_free_frame(uint32_t);
    uint32_t *pd = (uint32_t *)pd_phys;
    /* Sólo lo que el proceso POSEE en exclusiva:
     *   - contenido de sus page tables PRIVADAS (32-63): frames PMM
     *     mapeados a VA distinta (phys != virt => skip de identidades)
     *   - las page tables privadas en sí
     *   - el page directory
     * NUNCA las tablas compartidas (0-31, 64-255) ni las PDEs 256+
     * (MMIO del kernel — antes el free de 256+ podía devolver frames
     * estáticos extra_tables al PMM: corrupción latente). */
    for (int t = USER_TABLE_FIRST; t < USER_TABLE_LAST; t++) {
        if (!(pd[t] & PAGE_PRESENT)) continue;
        uint32_t *pt = (uint32_t *)(pd[t] & ~0xFFFu);
        for (int p = 0; p < PAGES_PER_TABLE; p++) {
            uint32_t pte = pt[p];
            if (!(pte & PAGE_PRESENT)) continue;
            uint32_t phys = pte & ~0xFFFu;
            uint32_t virt = (uint32_t)((t * PAGES_PER_TABLE + p)) * 0x1000u;
            if (phys != virt && phys >= PMM_FRAME_MIN)
                pmm_free_frame(phys);
        }
        pmm_free_frame(pd[t] & ~0xFFFu);
    }
    pmm_free_frame(pd_phys);
}
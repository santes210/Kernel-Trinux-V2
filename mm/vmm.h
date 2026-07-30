#ifndef MM_VMM_H
#define MM_VMM_H

#include "../lib/types.h"

#define PAGE_PRESENT       0x1
#define PAGE_RW            0x2
#define PAGE_USER          0x4
#define PAGE_WRITE_THROUGH 0x8
#define PAGE_CACHE_DISABLE 0x10

/* Kernel/user boundary: pages below this are supervisor-only (U/S=0).
 * This value is defined in vmm.c and exposed here for other subsystems. */
#define KERNEL_END_VIRT    0x05000000U   /* 80 MiB: kernel code + data + heap */

void vmm_init(void);
void vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags);
void vmm_unmap_page(uint32_t virt);
bool vmm_is_enabled(void);

uint32_t vmm_create_address_space(void);
void     vmm_switch_address_space(uint32_t pd_phys);
void     vmm_free_address_space(uint32_t pd_phys);
void     vmm_map_page_in(uint32_t pd_phys, uint32_t virt, uint32_t phys, uint32_t flags);
uint32_t vmm_get_current_dir(void);

/* ---- Address spaces REALES por proceso (v0.6.0) ----
 * La región de usuario 0x08000000-0x10000000 (tablas 32-63) es PRIVADA
 * de cada proceso: su page directory tiene page tables propias para ese
 * rango (todos los demás rangos — kernel, physmap supervisor, MMIO — se
 * comparten con el page directory del kernel). */

/* PD estático del kernel (para volver a él al morir un proceso). */
uint32_t vmm_kernel_dir(void);

/* Copia `len` bytes del buffer del kernel `src` a la VA `va` del address
 * space `pd` (las páginas destino ya deben estar mapeadas; usado por el
 * ELF loader para construir el stack con argv). Devuelve 0 ok, -1 error. */
int      vmm_copy_to_user(uint32_t pd, uint32_t va, const void *src, uint32_t len);

/* Libera TODAS las páginas de usuario privadas de `pd` (región 32-63):
 * usado por execve() antes de cargar la imagen nueva. Las page tables
 * privadas se conservan (quedan vacías). */
void     vmm_reset_user_region(uint32_t pd);

/* Deep-copy de la región de usuario privada de `src_pd` a `dst_pd`
 * (fork()): cada página presente se copia a un frame PMM nuevo con los
 * mismos flags. Devuelve 0 ok, -1 si faltaron frames. */
int      vmm_copy_user_space(uint32_t dst_pd, uint32_t src_pd);

/* Quita PAGE_USER de la página identity-mapped de un frame (para
 * estructuras internas del kernel alojadas en frames del PMM). */
void     vmm_deprivilege_identity_page(uint32_t phys);
/* Restaura las PTEs identidad de la región de usuario y libera frames
 * remapeados por fork() en las page tables compartidas (al morir un proc). */
void     vmm_restore_user_identity(void);

#endif /* MM_VMM_H */

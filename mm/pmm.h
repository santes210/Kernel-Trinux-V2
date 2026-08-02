#ifndef MM_PMM_H
#define MM_PMM_H

#include "../lib/types.h"

#define PMM_FRAME_SIZE 4096

void     pmm_init(uint32_t total_memory_bytes);
uint32_t pmm_alloc_frame(void);              /* returns physical addr or 0 */
void     pmm_free_frame(uint32_t addr);
uint32_t pmm_get_free_memory(void);          /* bytes */
uint32_t pmm_get_used_memory(void);          /* bytes */
uint32_t pmm_get_total_memory(void);         /* bytes */
void     pmm_reserve_region(uint32_t addr, uint32_t len);

/* ---- Copy-on-Write (v0.6.1) ----
 * Refcount por frame para páginas compartidas por fork(): cuenta cuántas
 * PTEs (en cualquier page directory) apuntan al frame. 0 = frame privado
 * (dueño único, sin seguimiento). Solo frames >= PMM_COW_BASE.
 */
#define PMM_COW_BASE   0x10000000u   /* 256 MiB: mínimo frame dinámico */

/* Incrementa la cuenta al COMPARTIR: 0 -> 2 (padre + primer hijo),
 * n -> n+1. Devuelve la nueva cuenta, o 0 si phys está fuera de rango. */
uint32_t pmm_cow_share(uint32_t phys);

/* Cuenta actual de referencias COW de un frame (0 = privado). */
uint32_t pmm_cow_refs(uint32_t phys);

/* Decrementa al soltar una referencia: n -> n-1 (0 -> 0 sin efecto).
 * Devuelve la nueva cuenta. */
uint32_t pmm_cow_unshare(uint32_t phys);

#endif /* MM_PMM_H */

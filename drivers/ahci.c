/* drivers/ahci.c  -  AHCI (SATA) disk driver for Trinux.
 *
 * This lets the kernel read/write the boot disk on MODERN hardware where the
 * classic IDE ports (0x1F0) don't exist.  Most PCs manufactured after ~2010
 * expose their SATA ports through an AHCI controller on PCI.  The BIOS (or
 * the USB controller firmware, on USB-booted machines) may emulate legacy IDE
 * for the bootloader, but once we're in protected mode that emulation is gone.
 *
 * The approach is:
 *   1. Scan PCI for class 01:06:01 (Mass Storage / SATA / AHCI).
 *   2. Read BAR5 (ABAR) to get the MMIO base of the HBA registers.
 *   3. Identity-map that MMIO region so we can access it.
 *   4. Find the first port with a present SATA device.
 *   5. Initialise that port (stop, rebase CLB/FB/CTBA, start).
 *   6. Issue IDENTIFY to learn the sector count.
 *   7. Provide read/write via READ DMA EXT / WRITE DMA EXT.
 *
 * We use polling (no IRQs), single-command-slot, and a single port.  This is
 * simple and works perfectly for a synchronous, single-tasking kernel.
 *
 * Memory note: all DMA buffers (CLB, FB, command tables) live in the kernel's
 * identity-mapped region (first 256 MiB).  We allocate them via
 * kmalloc_aligned() which returns page-aligned physical == virtual addresses.
 */
#include "ahci.h"
#include "pci.h"
#include "../cpu/ports.h"
#include "../mm/kheap.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../lib/printf.h"
#include "../drivers/serial.h"

/* ---- state ---- */
static bool         have_ahci;
static hba_mem_t   *hba;             /* MMIO pointer to the HBA registers */
static hba_port_t  *active_port;     /* the port we're using */
static int          active_port_num;
static uint32_t     total_sects;

/* DMA buffers (must be physically contiguous & aligned). */
static hba_cmd_header_t *cmd_list;   /* 1 KiB, 1 KiB-aligned */
static uint8_t          *fis_buf;    /* 256 B, 256-aligned */
static hba_cmd_tbl_t    *cmd_tbl;    /* 128-aligned, one table */

/* ---- helpers ---- */

/* Busy-wait until the port is idle (not running commands). */
static bool port_stop(hba_port_t *port)
{
    port->cmd &= ~HBA_PORT_CMD_ST;
    port->cmd &= ~HBA_PORT_CMD_FRE;
    for (int i = 0; i < 1000000; i++) {
        if (!(port->cmd & HBA_PORT_CMD_CR) &&
            !(port->cmd & HBA_PORT_CMD_FR))
            return true;
    }
    return false;   /* timeout */
}

static void port_start(hba_port_t *port)
{
    /* wait until CR clears */
    while (port->cmd & HBA_PORT_CMD_CR)
        ;
    port->cmd |= HBA_PORT_CMD_FRE;
    port->cmd |= HBA_PORT_CMD_ST;
}

/* Find a free command slot (we only use slot 0, but check anyway). */
static int find_cmdslot(hba_port_t *port)
{
    uint32_t slots = port->sact | port->ci;
    uint32_t ncmd = (hba->cap & 0x1F00) >> 8;  /* NCS field */
    for (uint32_t i = 0; i <= ncmd; i++) {
        if (!(slots & (1u << i)))
            return (int)i;
    }
    return -1;
}

/* Issue a command and poll until it completes or errors.
 * Returns 0 on success, -1 on error, -2 on timeout. */
static int port_issue_and_wait(hba_port_t *port, int slot)
{
    port->ci = (1u << slot);

    for (int i = 0; i < 5000000; i++) {
        if (!(port->ci & (1u << slot)))
            return 0;   /* done */
        if (port->is & (1u << 30))   /* TFES = Task File Error Status */
            return -1;
    }
    return -2;
}

/* ---- init ---- */

bool ahci_init(void)
{
    have_ahci = false;

    uint8_t bus, slot, func;
    uint32_t abar;
    if (!pci_find_ahci(&bus, &slot, &func, &abar))
        return false;

    if (abar == 0 || abar == 0xFFFFFFFF)
        return false;

    /* Enable bus-mastering so the HBA can DMA. */
    pci_enable_bus_master(bus, slot, func);

    /* Identity-map the ABAR MMIO region (4 KiB minimum; we map 8 pages to be
     * safe — the HBA registers + 32 ports = ~0x1100 bytes but some
     * controllers have vendor-specific areas). */
    for (uint32_t off = 0; off < 0x8000; off += 0x1000) {
        vmm_map_page(abar + off, abar + off,
                     PAGE_PRESENT | PAGE_RW | PAGE_WRITE_THROUGH);
    }

    hba = (hba_mem_t *)(uintptr_t)abar;

    /* Enable AHCI mode (GHC.AE = bit 31). */
    hba->ghc |= (1u << 31);

    /* BIOS/OS Handoff: if the controller supports it (CAP2.BOH, bit 0),
     * set BOHC.OOS (bit 1) and wait for BOHC.BOS (bit 0) to clear. */
    if (hba->cap2 & 1u) {
        hba->bohc |= (1u << 1);   /* OS Ownership */
        for (int i = 0; i < 1000000; i++) {
            if (!(hba->bohc & 1u))
                break;
        }
        /* wait a bit more for BIOS to clean up */
        for (volatile int i = 0; i < 100000; i++) ;
    }

    serial_printf("[ahci] HBA version %x, ports impl=%08x, cap=%08x\n",
                  hba->vs, hba->pi, hba->cap);

    /* Find the first SATA port with a present, active device. */
    uint32_t pi = hba->pi;
    int found = -1;
    for (int i = 0; i < 32; i++) {
        if (!(pi & (1u << i)))
            continue;

        hba_port_t *p = &hba->ports[i];
        uint32_t det = p->ssts & 0x0F;
        uint32_t ipm = (p->ssts >> 8) & 0x0F;

        if (det != HBA_PORT_DET_PRESENT || ipm != HBA_PORT_IPM_ACTIVE)
            continue;

        /* Check signature for a SATA disk (not ATAPI). */
        if (p->sig == SATA_SIG_ATA || p->sig == 0xFFFFFFFF ||
            p->sig == 0x00000000) {
            found = i;
            break;
        }
    }

    if (found < 0) {
        serial_write("[ahci] no SATA device found on any port\n");
        return false;
    }

    active_port = &hba->ports[found];
    active_port_num = found;
    serial_printf("[ahci] using port %d (sig=%08x)\n",
                  found, active_port->sig);

    /* ---- Rebase the port: allocate DMA buffers ---- */
    port_stop(active_port);

    /* Command List: 32 headers × 32 bytes = 1 KiB, 1 KiB-aligned. */
    cmd_list = (hba_cmd_header_t *)kmalloc_aligned(1024);
    if (!cmd_list) return false;
    memset((void *)cmd_list, 0, 1024);
    active_port->clb  = (uint32_t)(uintptr_t)cmd_list;
    active_port->clbu = 0;

    /* Received FIS: 256 bytes, 256-aligned. */
    fis_buf = (uint8_t *)kmalloc_aligned(256);
    if (!fis_buf) return false;
    memset(fis_buf, 0, 256);
    active_port->fb  = (uint32_t)(uintptr_t)fis_buf;
    active_port->fbu = 0;

    /* Command Table for slot 0: 128-aligned, ~256 bytes. */
    cmd_tbl = (hba_cmd_tbl_t *)kmalloc_aligned(sizeof(hba_cmd_tbl_t));
    if (!cmd_tbl) return false;
    memset((void *)cmd_tbl, 0, sizeof(hba_cmd_tbl_t));
    cmd_list[0].ctba  = (uint32_t)(uintptr_t)cmd_tbl;
    cmd_list[0].ctbau = 0;

    /* Clear pending errors. */
    active_port->serr = 0xFFFFFFFF;
    active_port->is   = 0xFFFFFFFF;

    port_start(active_port);

    /* ---- IDENTIFY DEVICE ---- */
    uint8_t *id_buf = (uint8_t *)kmalloc_aligned(512);
    if (!id_buf) return false;
    memset(id_buf, 0, 512);

    int slot_id = find_cmdslot(active_port);
    if (slot_id < 0) { kfree(id_buf); return false; }

    hba_cmd_header_t *hdr = &cmd_list[slot_id];
    hdr->cfl_a_w_p_r = sizeof(fis_reg_h2d_t) / 4;  /* command FIS length in DWORDs */
    hdr->pmp_c_b = 0;
    hdr->prdtl = 1;

    hba_cmd_tbl_t *tbl = (hba_cmd_tbl_t *)(uintptr_t)hdr->ctba;
    memset((void *)tbl, 0, sizeof(hba_cmd_tbl_t));

    /* PRDT: one entry pointing to id_buf (512 bytes). */
    tbl->prdt[0].dba  = (uint32_t)(uintptr_t)id_buf;
    tbl->prdt[0].dbau = 0;
    tbl->prdt[0].dbc_i = 511;   /* byte count minus 1 */

    /* Build the command FIS. */
    fis_reg_h2d_t *fis = (fis_reg_h2d_t *)tbl->cfis;
    memset(fis, 0, sizeof(fis_reg_h2d_t));
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->pmport_c = 0x80;       /* bit 7 = command (not control) */
    fis->command  = ATA_CMD_IDENTIFY;
    fis->device   = 0;

    int rc = port_issue_and_wait(active_port, slot_id);
    if (rc != 0) {
        serial_printf("[ahci] IDENTIFY failed (%d)\n", rc);
        kfree(id_buf);
        return false;
    }

    /* Words 60-61: total LBA28 sectors.
     * Words 100-103: total LBA48 sectors (if supported).
     * We use LBA48 if available. */
    uint16_t *id16 = (uint16_t *)id_buf;
    uint32_t lba28 = (uint32_t)id16[60] | ((uint32_t)id16[61] << 16);
    uint32_t lba48_lo = (uint32_t)id16[100] | ((uint32_t)id16[101] << 16);
    /* uint32_t lba48_hi = (uint32_t)id16[102] | ((uint32_t)id16[103] << 16); */

    total_sects = lba48_lo ? lba48_lo : lba28;
    kfree(id_buf);

    if (total_sects == 0) {
        serial_write("[ahci] disk reports 0 sectors\n");
        return false;
    }

    serial_printf("[ahci] disk: %u sectors (%u MiB)\n",
                  total_sects, total_sects / 2048);

    have_ahci = true;
    return true;
}

bool     ahci_present(void)        { return have_ahci; }
uint32_t ahci_total_sectors(void)  { return total_sects; }

/* ---- read/write using READ DMA EXT / WRITE DMA EXT ---- */

/* Generic DMA transfer. `write` = true for writes. `count` ≤ 128. */
static int ahci_transfer(uint32_t lba, uint16_t count, void *buf, bool write)
{
    if (!have_ahci || count == 0)
        return -10;

    int slot_id = find_cmdslot(active_port);
    if (slot_id < 0)
        return -11;

    hba_cmd_header_t *hdr = &cmd_list[slot_id];
    hdr->cfl_a_w_p_r = sizeof(fis_reg_h2d_t) / 4;
    if (write)
        hdr->cfl_a_w_p_r |= (1 << 6);   /* W bit */
    else
        hdr->cfl_a_w_p_r &= ~(1 << 6);
    hdr->pmp_c_b = (1 << 2);   /* Clear BSY upon R_OK */

    /* How many PRDT entries? Each can describe up to 4 MiB, but we only
     * transfer count × 512 bytes. We'll use one entry per 4 KiB. */
    uint32_t total_bytes = (uint32_t)count * 512;
    uint32_t prdt_count = (total_bytes + 0xFFF) / 0x1000;
    if (prdt_count > 8) prdt_count = 8;   /* our cmd_tbl has room for 8 */
    hdr->prdtl = (uint16_t)prdt_count;

    hba_cmd_tbl_t *tbl = (hba_cmd_tbl_t *)(uintptr_t)hdr->ctba;
    memset((void *)tbl, 0, sizeof(hba_cmd_tbl_t));

    /* Fill PRDT entries. */
    uint32_t remain = total_bytes;
    uint8_t *p = (uint8_t *)buf;
    for (uint32_t i = 0; i < prdt_count; i++) {
        uint32_t chunk = (remain > 0x1000) ? 0x1000 : remain;
        tbl->prdt[i].dba  = (uint32_t)(uintptr_t)p;
        tbl->prdt[i].dbau = 0;
        tbl->prdt[i].dbc_i = (chunk - 1);   /* byte count minus 1 */
        if (i == prdt_count - 1)
            tbl->prdt[i].dbc_i |= (1u << 31);  /* interrupt on last */
        p += chunk;
        remain -= chunk;
    }

    /* Build the command FIS (48-bit LBA). */
    fis_reg_h2d_t *fis = (fis_reg_h2d_t *)tbl->cfis;
    memset(fis, 0, sizeof(fis_reg_h2d_t));
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->pmport_c = 0x80;   /* command */
    fis->command  = write ? ATA_CMD_WRITE_DMA_EX : ATA_CMD_READ_DMA_EX;
    fis->device   = (1 << 6);  /* LBA mode */

    fis->lba0 = (uint8_t)(lba & 0xFF);
    fis->lba1 = (uint8_t)((lba >> 8) & 0xFF);
    fis->lba2 = (uint8_t)((lba >> 16) & 0xFF);
    fis->lba3 = (uint8_t)((lba >> 24) & 0xFF);
    fis->lba4 = 0;
    fis->lba5 = 0;

    fis->countl = (uint8_t)(count & 0xFF);
    fis->counth = (uint8_t)((count >> 8) & 0xFF);

    return port_issue_and_wait(active_port, slot_id);
}

int ahci_read_sectors(uint32_t lba, uint16_t count, void *buf)
{
    /* DMA needs physically contiguous & aligned buffers.  If the caller's
     * buffer might not be in the identity-mapped region, we bounce through
     * a heap buffer. For simplicity we always bounce (the amounts are tiny:
     * max 128 × 512 = 64 KiB). */
    uint8_t *dma_buf = (uint8_t *)kmalloc_aligned((uint32_t)count * 512);
    if (!dma_buf)
        return -12;

    int rc = ahci_transfer(lba, count, dma_buf, false);
    if (rc == 0)
        memcpy(buf, dma_buf, (uint32_t)count * 512);
    kfree(dma_buf);
    return rc;
}

int ahci_write_sectors(uint32_t lba, uint16_t count, const void *buf)
{
    uint8_t *dma_buf = (uint8_t *)kmalloc_aligned((uint32_t)count * 512);
    if (!dma_buf)
        return -12;

    memcpy(dma_buf, buf, (uint32_t)count * 512);
    int rc = ahci_transfer(lba, count, dma_buf, true);
    kfree(dma_buf);
    return rc;
}

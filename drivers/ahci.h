#ifndef DRIVERS_AHCI_H
#define DRIVERS_AHCI_H

#include "../lib/types.h"

/* ---- AHCI HBA Memory Registers (MMIO) ---- */

/* FIS (Frame Information Structure) types */
#define FIS_TYPE_REG_H2D    0x27   /* host to device */
#define FIS_TYPE_REG_D2H    0x34   /* device to host */
#define FIS_TYPE_DMA_SETUP  0x41
#define FIS_TYPE_DATA       0x46
#define FIS_TYPE_PIO_SETUP  0x5F

/* ATA commands */
#define ATA_CMD_READ_DMA_EX    0x25
#define ATA_CMD_WRITE_DMA_EX   0x35
#define ATA_CMD_IDENTIFY       0xEC
#define ATA_CMD_CACHE_FLUSH_EX 0xEA

/* Port signature values */
#define SATA_SIG_ATA    0x00000101
#define SATA_SIG_ATAPI  0xEB140101
#define SATA_SIG_SEMB   0xC33C0101
#define SATA_SIG_PM     0x96690101

/* HBA port registers (one per port, 0x80 bytes each). */
typedef volatile struct {
    uint32_t clb;        /* 0x00: command list base (lower 32) */
    uint32_t clbu;       /* 0x04: command list base (upper 32) */
    uint32_t fb;         /* 0x08: FIS base (lower 32) */
    uint32_t fbu;        /* 0x0C: FIS base (upper 32) */
    uint32_t is;         /* 0x10: interrupt status */
    uint32_t ie;         /* 0x14: interrupt enable */
    uint32_t cmd;        /* 0x18: command and status */
    uint32_t rsv0;       /* 0x1C */
    uint32_t tfd;        /* 0x20: task file data */
    uint32_t sig;        /* 0x24: signature */
    uint32_t ssts;       /* 0x28: SATA status */
    uint32_t sctl;       /* 0x2C: SATA control */
    uint32_t serr;       /* 0x30: SATA error */
    uint32_t sact;       /* 0x34: SATA active */
    uint32_t ci;         /* 0x38: command issue */
    uint32_t sntf;       /* 0x3C: SATA notification */
    uint32_t fbs;        /* 0x40: FIS-based switching */
    uint32_t rsv1[11];   /* 0x44-0x6F */
    uint32_t vendor[4];  /* 0x70-0x7F */
} hba_port_t;

/* HBA Generic Host Control registers (at the ABAR base). */
typedef volatile struct {
    uint32_t cap;        /* 0x00: host capabilities */
    uint32_t ghc;        /* 0x04: global host control */
    uint32_t is;         /* 0x08: interrupt status */
    uint32_t pi;         /* 0x0C: ports implemented */
    uint32_t vs;         /* 0x10: version */
    uint32_t ccc_ctl;    /* 0x14 */
    uint32_t ccc_ports;  /* 0x18 */
    uint32_t em_loc;     /* 0x1C */
    uint32_t em_ctl;     /* 0x20 */
    uint32_t cap2;       /* 0x24 */
    uint32_t bohc;       /* 0x28: BIOS/OS handoff control */
    uint8_t  rsv[0xA0 - 0x2C];
    uint8_t  vendor[0x100 - 0xA0];
    hba_port_t ports[32]; /* 0x100+ */
} hba_mem_t;

/* HBA port command bits */
#define HBA_PORT_CMD_ST   (1u << 0)   /* Start */
#define HBA_PORT_CMD_FRE  (1u << 4)   /* FIS Receive Enable */
#define HBA_PORT_CMD_FR   (1u << 14)  /* FIS Receive Running */
#define HBA_PORT_CMD_CR   (1u << 15)  /* Command list Running */

/* SATA status detection bits */
#define HBA_PORT_DET_PRESENT  0x3
#define HBA_PORT_IPM_ACTIVE   0x1

/* ---- Command structures ---- */

/* Command header (32 bytes, 32 of these per command list = 1 KiB). */
typedef struct {
    uint8_t  cfl_a_w_p_r;  /* command FIS length (bits 4:0), ATAPI, Write,
                              Prefetchable, Reset */
    uint8_t  pmp_c_b;      /* port multiplier port, Clear BSY, BIST */
    uint16_t prdtl;        /* PRDT length (entries) */
    volatile uint32_t prdbc;  /* PRD byte count transferred */
    uint32_t ctba;         /* command table base (lower 32) */
    uint32_t ctbau;        /* command table base (upper 32) */
    uint32_t rsv[4];
} __attribute__((packed)) hba_cmd_header_t;

/* PRD (Physical Region Descriptor) Table entry. */
typedef struct {
    uint32_t dba;          /* data base address (lower 32) */
    uint32_t dbau;         /* data base address (upper 32) */
    uint32_t rsv0;
    uint32_t dbc_i;        /* byte count (bits 21:0), bit 31 = interrupt on completion */
} __attribute__((packed)) hba_prdt_entry_t;

/* Command table: CFIS + ACMD + PRDT entries. */
typedef struct {
    uint8_t  cfis[64];     /* command FIS */
    uint8_t  acmd[16];     /* ATAPI command */
    uint8_t  rsv[48];      /* reserved */
    hba_prdt_entry_t prdt[8];  /* up to 8 PRDT entries (enough for 32 KiB) */
} __attribute__((packed)) hba_cmd_tbl_t;

/* FIS: Register – Host to Device. */
typedef struct {
    uint8_t  fis_type;     /* FIS_TYPE_REG_H2D */
    uint8_t  pmport_c;    /* port multiplier (7:4), bit 7 = command/control */
    uint8_t  command;
    uint8_t  featurel;
    uint8_t  lba0;
    uint8_t  lba1;
    uint8_t  lba2;
    uint8_t  device;
    uint8_t  lba3;
    uint8_t  lba4;
    uint8_t  lba5;
    uint8_t  featureh;
    uint8_t  countl;
    uint8_t  counth;
    uint8_t  icc;
    uint8_t  control;
    uint8_t  rsv[4];
} __attribute__((packed)) fis_reg_h2d_t;

/* ---- Public API ---- */

/* Probe PCI for an AHCI controller, initialise the first SATA port found.
 * Returns true if a working disk was found. */
bool ahci_init(void);

/* Are we using AHCI? (as opposed to legacy ATA) */
bool ahci_present(void);

/* Total sectors (512-byte) on the discovered disk. */
uint32_t ahci_total_sectors(void);

/* Read/write `count` sectors (max 128 per call) starting at LBA.
 * Returns 0 on success, <0 on error. */
int ahci_read_sectors(uint32_t lba, uint16_t count, void *buf);
int ahci_write_sectors(uint32_t lba, uint16_t count, const void *buf);

#endif /* DRIVERS_AHCI_H */

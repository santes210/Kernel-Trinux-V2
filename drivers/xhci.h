#ifndef DRIVERS_XHCI_H
#define DRIVERS_XHCI_H

#include "../lib/types.h"

/* ============================================================================
 *  xHCI (eXtensible Host Controller Interface) driver for USB Mass Storage.
 *
 *  This driver implements the minimum needed to read/write sectors from a
 *  USB flash drive:
 *    1. xHCI controller init (rings, DCBAA, event ring, scratchpads)
 *    2. Port detection + reset + slot enable + address device
 *    3. Control transfers to read descriptors and configure the device
 *    4. Bulk transfers for USB Mass Storage (BOT) with SCSI READ/WRITE(10)
 *
 *  PCI class: 0x0C  subclass: 0x03  progIF: 0x30
 * ============================================================================ */

/* ---- xHCI MMIO register offsets (Capability Registers) ---- */
#define XHCI_CAPLENGTH    0x00   /* u8:  capability length */
#define XHCI_HCIVERSION   0x02   /* u16: version */
#define XHCI_HCSPARAMS1   0x04   /* u32: MaxSlots, MaxIntrs, MaxPorts */
#define XHCI_HCSPARAMS2   0x08   /* u32: IST, ERST max, scratchpad info */
#define XHCI_HCSPARAMS3   0x0C
#define XHCI_HCCPARAMS1   0x10   /* u32: 64-bit addressing, etc. */
#define XHCI_DBOFF        0x14   /* u32: doorbell array offset */
#define XHCI_RTSOFF       0x18   /* u32: runtime registers offset */
#define XHCI_HCCPARAMS2   0x1C

/* ---- Operational Registers (base + CAPLENGTH) ---- */
#define XHCI_USBCMD       0x00
#define XHCI_USBSTS       0x04
#define XHCI_PAGESIZE      0x08
#define XHCI_DNCTRL       0x14
#define XHCI_CRCR          0x18   /* u64: command ring control */
#define XHCI_DCBAAP        0x30   /* u64: device context base address array ptr */
#define XHCI_CONFIG        0x38

/* USBCMD bits */
#define XHCI_CMD_RS        (1u << 0)   /* Run/Stop */
#define XHCI_CMD_HCRST     (1u << 1)   /* HC Reset */
#define XHCI_CMD_INTE      (1u << 2)   /* Interrupter Enable */

/* USBSTS bits */
#define XHCI_STS_HCH      (1u << 0)   /* HC Halted */
#define XHCI_STS_CNR      (1u << 11)  /* Controller Not Ready */
#define XHCI_STS_EINT     (1u << 3)   /* Event Interrupt */

/* Port register offsets (from op_base + 0x400 + port*0x10) */
#define XHCI_PORTSC        0x00
#define XHCI_PORTPMSC      0x04
#define XHCI_PORTLI        0x08

/* PORTSC bits */
#define PORTSC_CCS         (1u << 0)   /* Current Connect Status */
#define PORTSC_PED         (1u << 1)   /* Port Enabled */
#define PORTSC_PR          (1u << 4)   /* Port Reset */
#define PORTSC_PLS_MASK    (0xFu << 5) /* Port Link State */
#define PORTSC_PP          (1u << 9)   /* Port Power */
#define PORTSC_SPEED_MASK  (0xFu << 10)
#define PORTSC_PRC         (1u << 21)  /* Port Reset Change */
#define PORTSC_CSC         (1u << 17)  /* Connect Status Change */
#define PORTSC_WRC         (1u << 19)
/* Write-1-to-clear status bits that must NOT be accidentally cleared */
#define PORTSC_RW1C_MASK   (PORTSC_CSC | PORTSC_PED | PORTSC_PRC | \
                            PORTSC_WRC | (1u<<20) | (1u<<22) | (1u<<23))

/* ---- TRB (Transfer Request Block) ---- */
/* Every TRB is 16 bytes. */
typedef struct {
    uint32_t param_lo;
    uint32_t param_hi;
    uint32_t status;
    uint32_t control;
} __attribute__((packed, aligned(16))) xhci_trb_t;

/* TRB types (bits 15:10 of control) */
#define TRB_TYPE_SHIFT     10
#define TRB_NORMAL         1
#define TRB_SETUP_STAGE    2
#define TRB_DATA_STAGE     3
#define TRB_STATUS_STAGE   4
#define TRB_LINK           6
#define TRB_NO_OP          8
#define TRB_ENABLE_SLOT    9
#define TRB_ADDRESS_DEVICE 11
#define TRB_CONFIG_EP      12
#define TRB_EVALUATE_CTX   13
#define TRB_RESET_EP       14
#define TRB_STOP_EP        15
#define TRB_SET_TR_DEQUEUE 16
#define TRB_RESET_DEVICE   17
#define TRB_NO_OP_CMD      23
#define TRB_TRANSFER_EVENT 32
#define TRB_CMD_COMPLETION 33
#define TRB_PORT_STATUS_CHANGE 34

/* TRB control field bits */
#define TRB_CYCLE          (1u << 0)
#define TRB_TC             (1u << 1)   /* Toggle Cycle for Link TRBs */
#define TRB_IOC            (1u << 5)   /* Interrupt On Completion */
#define TRB_IDT            (1u << 6)   /* Immediate Data */
#define TRB_BSR            (1u << 9)   /* Block Set Address Request */

/* TRB completion codes */
#define TRB_CC_SUCCESS     1
#define TRB_CC_SHORT       13

/* ---- Slot / Endpoint Context ---- */
/* We use 32-byte contexts (no large context support needed). */
#define XHCI_CTX_SIZE      32
#define XHCI_SLOT_CTX_SIZE 32
#define XHCI_EP_CTX_SIZE   32

/* ---- USB Descriptors ---- */
#define USB_DESC_DEVICE    1
#define USB_DESC_CONFIG    2
#define USB_DESC_INTERFACE 4
#define USB_DESC_ENDPOINT  5

/* USB class codes */
#define USB_CLASS_MASS_STORAGE 0x08
#define USB_SUBCLASS_SCSI      0x06
#define USB_PROTOCOL_BOT       0x50   /* Bulk-Only Transport */

/* ---- SCSI commands for Mass Storage ---- */
#define SCSI_INQUIRY           0x12
#define SCSI_TEST_UNIT_READY   0x00
#define SCSI_REQUEST_SENSE     0x03
#define SCSI_READ_CAPACITY_10  0x25
#define SCSI_READ_10           0x28
#define SCSI_WRITE_10          0x2A

/* Command Block Wrapper (CBW) — 31 bytes */
#define CBW_SIGNATURE  0x43425355u
typedef struct {
    uint32_t signature;      /* CBW_SIGNATURE */
    uint32_t tag;
    uint32_t data_length;
    uint8_t  flags;          /* bit7: 0=OUT, 1=IN */
    uint8_t  lun;
    uint8_t  cb_length;      /* 6..16 */
    uint8_t  cb[16];         /* SCSI command block */
} __attribute__((packed)) usb_cbw_t;

/* Command Status Wrapper (CSW) — 13 bytes */
#define CSW_SIGNATURE  0x53425355u
typedef struct {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_residue;
    uint8_t  status;         /* 0=passed, 1=failed, 2=phase error */
} __attribute__((packed)) usb_csw_t;

/* ---- Public API ---- */

/* Probe PCI for xHCI, init controller, find USB mass storage device.
 * Returns true if a usable USB disk was found. */
bool xhci_init(void);

/* Is xHCI active? */
bool xhci_present(void);

/* Total 512-byte sectors on the USB disk. */
uint32_t xhci_total_sectors(void);

/* Read/write sectors. Returns 0 on success. */
int xhci_read_sectors(uint32_t lba, uint16_t count, void *buf);
int xhci_write_sectors(uint32_t lba, uint16_t count, const void *buf);

#endif /* DRIVERS_XHCI_H */

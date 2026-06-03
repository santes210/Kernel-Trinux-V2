/* drivers/xhci.c  -  xHCI USB 3.0 host controller + Mass Storage driver.
 *
 * Implements the full stack needed to read/write a USB flash drive on bare
 * metal hardware where the boot medium is only accessible via xHCI:
 *
 *   PCI scan → xHCI init → port reset → slot/address → configure endpoints
 *   → USB Mass Storage Bulk-Only Transport → SCSI READ(10)/WRITE(10)
 *
 * Design choices for a 32-bit, single-tasking, educational kernel:
 *   - Polling only (no MSI/MSI-X interrupts).
 *   - Single device, single slot.
 *   - 32-byte device contexts (no large context support).
 *   - All DMA buffers in the identity-mapped first 256 MiB (physical=virtual).
 *   - Command ring, event ring, and transfer rings are small fixed-size arrays.
 */
#include "xhci.h"
#include "pci.h"
#include "../cpu/ports.h"
#include "../mm/kheap.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../lib/printf.h"
#include "../drivers/serial.h"

/* ---- sizing constants ---- */
#define CMD_RING_SIZE    32       /* TRBs in the command ring */
#define EVT_RING_SIZE    64       /* TRBs in the event ring */
#define XFER_RING_SIZE   64       /* TRBs per transfer ring */
#define MAX_SCRATCHPADS  256

/* ---- module state ---- */
static bool      have_xhci;
static uint32_t  total_sects;       /* sectors on the USB disk */

/* MMIO pointers */
static volatile uint8_t  *mmio_base;
static volatile uint8_t  *op_base;
static volatile uint8_t  *rt_base;
static volatile uint32_t *db_base;     /* doorbell array */
static uint32_t           max_slots;
static uint32_t           max_ports;
static uint32_t           ctx_size;     /* 32 or 64 */

/* Rings (all page-aligned for DMA) */
static xhci_trb_t *cmd_ring;
static uint32_t    cmd_enqueue;
static uint32_t    cmd_cycle;

static xhci_trb_t *evt_ring;
static uint32_t    evt_dequeue;
static uint32_t    evt_cycle;

/* Event Ring Segment Table (one entry) */
typedef struct {
    uint64_t base;
    uint32_t size;
    uint32_t rsvd;
} __attribute__((packed, aligned(64))) erst_entry_t;
static erst_entry_t *erst;

/* Device Context Base Address Array */
static uint64_t *dcbaa;

/* Scratchpad buffer array */
static uint64_t *scratchpad_array;

/* The one slot we use */
static uint32_t slot_id;

/* Contexts for the device (input and output) */
static uint8_t *input_ctx;    /* 33 * ctx_size bytes, aligned */
static uint8_t *output_ctx;   /* 32 * ctx_size bytes, aligned */

/* Transfer rings for EP0 (control), Bulk IN, Bulk OUT */
static xhci_trb_t *ep0_ring;
static uint32_t    ep0_enqueue, ep0_cycle;
static xhci_trb_t *bulk_in_ring;
static uint32_t    bulk_in_enqueue, bulk_in_cycle;
static xhci_trb_t *bulk_out_ring;
static uint32_t    bulk_out_enqueue, bulk_out_cycle;

/* Endpoint DCI (Device Context Index) for the bulk endpoints */
static uint32_t bulk_in_dci;
static uint32_t bulk_out_dci;
static uint16_t bulk_in_max_packet;
static uint16_t bulk_out_max_packet;

/* Mass storage tag counter */
static uint32_t msd_tag = 1;

/* ---- MMIO read/write helpers ---- */
static uint32_t rd32(volatile uint8_t *base, uint32_t off)
{ return *(volatile uint32_t *)(base + off); }
static void wr32(volatile uint8_t *base, uint32_t off, uint32_t val)
{ *(volatile uint32_t *)(base + off) = val; }
static uint64_t rd64(volatile uint8_t *base, uint32_t off)
{ return (uint64_t)rd32(base, off) | ((uint64_t)rd32(base, off + 4) << 32); }
static void wr64(volatile uint8_t *base, uint32_t off, uint64_t val)
{ wr32(base, off, (uint32_t)val); wr32(base, off + 4, (uint32_t)(val >> 32)); }

/* ---- port register access ---- */
static volatile uint8_t *port_reg(uint32_t port)
{ return op_base + 0x400 + port * 0x10; }

/* ---- small delay ---- */
static void xhci_delay(uint32_t ms)
{
    /* crude busy-wait; ~1ms per 100000 iterations at typical speed */
    for (uint32_t i = 0; i < ms * 100000; i++)
        __asm__ volatile("" ::: "memory");
}

/* ============================================================================
 *  Ring management
 * ============================================================================ */

static xhci_trb_t *ring_alloc(uint32_t count)
{
    xhci_trb_t *r = (xhci_trb_t *)kmalloc_aligned(count * sizeof(xhci_trb_t));
    if (r) memset(r, 0, count * sizeof(xhci_trb_t));
    return r;
}

/* Enqueue a TRB on a ring, handling the Link TRB at the end. */
static void ring_enqueue(xhci_trb_t *ring, uint32_t ring_size,
                         uint32_t *enq, uint32_t *cycle,
                         xhci_trb_t *trb)
{
    xhci_trb_t *dest = &ring[*enq];
    dest->param_lo = trb->param_lo;
    dest->param_hi = trb->param_hi;
    dest->status   = trb->status;
    /* set the cycle bit */
    dest->control  = (trb->control & ~1u) | (*cycle & 1u);

    (*enq)++;
    if (*enq >= ring_size - 1) {
        /* last slot is a Link TRB back to start */
        xhci_trb_t *link = &ring[*enq];
        link->param_lo = (uint32_t)(uintptr_t)ring;
        link->param_hi = 0;
        link->status   = 0;
        link->control  = (TRB_LINK << TRB_TYPE_SHIFT) | TRB_TC | (*cycle & 1u);
        *enq = 0;
        *cycle ^= 1;
    }
}

/* Ring the doorbell for a slot+endpoint. slot=0 → command ring. */
static void ring_doorbell(uint32_t slot, uint32_t target)
{
    db_base[slot] = target;
}

/* ============================================================================
 *  Event ring: poll for a completion event
 * ============================================================================ */

static bool poll_event(xhci_trb_t *out, uint32_t timeout_ms)
{
    for (uint32_t i = 0; i < timeout_ms * 10000; i++) {
        /* Ensure we see the latest memory written by the controller (DMA). */
        __asm__ volatile("" ::: "memory");

        xhci_trb_t *evt = &evt_ring[evt_dequeue];
        if ((evt->control & 1u) == (evt_cycle & 1u)) {
            /* got an event */
            *out = *evt;
            evt_dequeue++;
            if (evt_dequeue >= EVT_RING_SIZE) {
                evt_dequeue = 0;
                evt_cycle ^= 1;
            }

            /* Clear EINT in USBSTS (write-1-to-clear). */
            uint32_t sts = rd32(op_base, XHCI_USBSTS);
            if (sts & XHCI_STS_EINT)
                wr32(op_base, XHCI_USBSTS, XHCI_STS_EINT);

            /* Clear IP in IMAN (write 1 to bit 0). */
            uint32_t iman = rd32(rt_base, 0x20);
            if (iman & 1u)
                wr32(rt_base, 0x20, iman | 1u);   /* write-1-to-clear IP */

            /* Update ERDP with EHB bit set to acknowledge. */
            uint64_t new_erdp = (uint64_t)(uintptr_t)&evt_ring[evt_dequeue];
            new_erdp |= (1u << 3);   /* EHB */
            wr64(rt_base, 0x20 + 0x18, new_erdp);

            return true;
        }
        for (volatile int d = 0; d < 10; d++) ;
    }
    return false;
}

/* Issue a command TRB and wait for completion. Returns completion code. */
static int xhci_command(xhci_trb_t *trb, xhci_trb_t *evt_out)
{
    ring_enqueue(cmd_ring, CMD_RING_SIZE, &cmd_enqueue, &cmd_cycle, trb);
    ring_doorbell(0, 0);   /* host controller doorbell, target=0 */

    xhci_trb_t evt;
    if (!poll_event(&evt, 5000)) {
        serial_write("[xhci] command timeout\n");
        return -1;
    }

    /* The event should be a Command Completion Event. */
    uint32_t type = (evt.control >> TRB_TYPE_SHIFT) & 0x3F;
    if (type != TRB_CMD_COMPLETION) {
        /* might be a Port Status Change — drain it and retry */
        if (type == TRB_PORT_STATUS_CHANGE) {
            /* acknowledge and try again */
            if (!poll_event(&evt, 5000))
                return -1;
        } else {
            serial_printf("[xhci] unexpected event type %u\n", type);
            return -2;
        }
    }
    if (evt_out) *evt_out = evt;
    return (evt.status >> 24) & 0xFF;   /* completion code */
}

/* ============================================================================
 *  Control transfer on EP0 (for USB descriptors, Set Configuration, etc.)
 * ============================================================================ */

static int xhci_control_transfer(uint8_t bmRequestType, uint8_t bRequest,
                                 uint16_t wValue, uint16_t wIndex,
                                 uint16_t wLength, void *data)
{
    bool dir_in = (bmRequestType & 0x80) != 0;

    /* Setup Stage TRB */
    xhci_trb_t setup = {0};
    setup.param_lo = (uint32_t)bmRequestType |
                     ((uint32_t)bRequest << 8) |
                     ((uint32_t)wValue << 16);
    setup.param_hi = (uint32_t)wIndex |
                     ((uint32_t)wLength << 16);
    setup.status   = 8;   /* TRB transfer length = 8 (setup packet) */
    uint32_t trt = wLength ? (dir_in ? 3 : 2) : 0;  /* Transfer Type */
    setup.control  = (TRB_SETUP_STAGE << TRB_TYPE_SHIFT) | TRB_IDT |
                     (trt << 16);
    ring_enqueue(ep0_ring, XFER_RING_SIZE, &ep0_enqueue, &ep0_cycle, &setup);

    /* Data Stage TRB (if any) */
    if (wLength && data) {
        xhci_trb_t datastg = {0};
        datastg.param_lo = (uint32_t)(uintptr_t)data;
        datastg.param_hi = 0;
        datastg.status   = wLength;
        datastg.control  = (TRB_DATA_STAGE << TRB_TYPE_SHIFT) |
                           (dir_in ? (1u << 16) : 0);
        ring_enqueue(ep0_ring, XFER_RING_SIZE, &ep0_enqueue, &ep0_cycle, &datastg);
    }

    /* Status Stage TRB */
    xhci_trb_t status = {0};
    status.control = (TRB_STATUS_STAGE << TRB_TYPE_SHIFT) | TRB_IOC |
                     ((!dir_in && wLength) ? (1u << 16) : 0);
    ring_enqueue(ep0_ring, XFER_RING_SIZE, &ep0_enqueue, &ep0_cycle, &status);

    ring_doorbell(slot_id, 1);   /* DCI 1 = EP0 */

    /* Wait for transfer events (we might get one per stage). */
    xhci_trb_t evt;
    for (int i = 0; i < 3; i++) {
        if (!poll_event(&evt, 3000)) break;
        uint32_t cc = (evt.status >> 24) & 0xFF;
        if (cc != TRB_CC_SUCCESS && cc != TRB_CC_SHORT) {
            serial_printf("[xhci] control xfer cc=%u\n", cc);
            return -(int)cc;
        }
    }
    return 0;
}

/* ============================================================================
 *  Bulk transfer (for Mass Storage data)
 * ============================================================================ */

static int xhci_bulk_transfer(bool dir_in, void *data, uint32_t length)
{
    xhci_trb_t *ring  = dir_in ? bulk_in_ring  : bulk_out_ring;
    uint32_t   *enq   = dir_in ? &bulk_in_enqueue  : &bulk_out_enqueue;
    uint32_t   *cyc   = dir_in ? &bulk_in_cycle    : &bulk_out_cycle;
    uint32_t    dci   = dir_in ? bulk_in_dci   : bulk_out_dci;

    xhci_trb_t trb = {0};
    trb.param_lo = (uint32_t)(uintptr_t)data;
    trb.param_hi = 0;
    trb.status   = length;
    trb.control  = (TRB_NORMAL << TRB_TYPE_SHIFT) | TRB_IOC;

    ring_enqueue(ring, XFER_RING_SIZE, enq, cyc, &trb);
    ring_doorbell(slot_id, dci);

    xhci_trb_t evt;
    if (!poll_event(&evt, 5000)) {
        serial_printf("[xhci] bulk %s timeout\n", dir_in ? "IN" : "OUT");
        return -1;
    }
    uint32_t cc = (evt.status >> 24) & 0xFF;
    if (cc != TRB_CC_SUCCESS && cc != TRB_CC_SHORT) {
        serial_printf("[xhci] bulk cc=%u\n", cc);
        return -(int)cc;
    }
    return 0;
}

/* ============================================================================
 *  USB Mass Storage Bulk-Only Transport
 * ============================================================================ */

static int msd_command(uint8_t *scsi_cmd, uint8_t cmd_len,
                       void *data, uint32_t data_len, bool dir_in)
{
    /* Allocate DMA-safe buffers */
    usb_cbw_t *cbw = (usb_cbw_t *)kmalloc_aligned(sizeof(usb_cbw_t));
    usb_csw_t *csw = (usb_csw_t *)kmalloc_aligned(sizeof(usb_csw_t));
    if (!cbw || !csw) { kfree(cbw); kfree(csw); return -1; }

    /* Build CBW */
    memset(cbw, 0, sizeof(*cbw));
    cbw->signature   = CBW_SIGNATURE;
    cbw->tag         = msd_tag++;
    cbw->data_length = data_len;
    cbw->flags       = dir_in ? 0x80 : 0x00;
    cbw->lun         = 0;
    cbw->cb_length   = cmd_len;
    memcpy(cbw->cb, scsi_cmd, cmd_len);

    /* Send CBW via Bulk OUT */
    int rc = xhci_bulk_transfer(false, cbw, 31);
    if (rc != 0) { kfree(cbw); kfree(csw); return rc; }

    /* Data phase (if any) */
    if (data_len > 0 && data) {
        rc = xhci_bulk_transfer(dir_in, data, data_len);
        if (rc != 0) { kfree(cbw); kfree(csw); return rc; }
    }

    /* Receive CSW via Bulk IN */
    memset(csw, 0, sizeof(*csw));
    rc = xhci_bulk_transfer(true, csw, 13);
    if (rc != 0) { kfree(cbw); kfree(csw); return rc; }

    if (csw->signature != CSW_SIGNATURE || csw->tag != cbw->tag) {
        serial_write("[xhci-msd] CSW signature/tag mismatch\n");
        kfree(cbw); kfree(csw);
        return -3;
    }

    rc = (csw->status == 0) ? 0 : -4;
    kfree(cbw);
    kfree(csw);
    return rc;
}

static uint32_t msd_read_capacity(void)
{
    uint8_t cmd[10] = {0};
    cmd[0] = SCSI_READ_CAPACITY_10;

    uint8_t *resp = (uint8_t *)kmalloc_aligned(8);
    if (!resp) return 0;
    memset(resp, 0, 8);

    int rc = msd_command(cmd, 10, resp, 8, true);
    if (rc != 0) { kfree(resp); return 0; }

    /* Response: 4 bytes last LBA (big-endian) + 4 bytes block size */
    uint32_t last_lba = ((uint32_t)resp[0] << 24) | ((uint32_t)resp[1] << 16) |
                        ((uint32_t)resp[2] << 8)  | (uint32_t)resp[3];
    uint32_t block_size = ((uint32_t)resp[4] << 24) | ((uint32_t)resp[5] << 16) |
                          ((uint32_t)resp[6] << 8)  | (uint32_t)resp[7];
    kfree(resp);

    serial_printf("[xhci-msd] capacity: last_lba=%u block_size=%u\n",
                  last_lba, block_size);

    if (block_size != 512) {
        serial_printf("[xhci-msd] unsupported block size %u\n", block_size);
        return 0;
    }
    return last_lba + 1;
}

static bool msd_init_device(void)
{
    /* TEST UNIT READY (might need a few tries) */
    uint8_t cmd[10] = {0};
    for (int i = 0; i < 5; i++) {
        cmd[0] = SCSI_TEST_UNIT_READY;
        int rc = msd_command(cmd, 6, NULL, 0, false);
        if (rc == 0) break;

        /* REQUEST SENSE to clear any condition */
        uint8_t sense[18];
        memset(sense, 0, sizeof(sense));
        uint8_t rs[6] = { SCSI_REQUEST_SENSE, 0, 0, 0, 18, 0 };
        uint8_t *sense_dma = (uint8_t *)kmalloc_aligned(18);
        if (sense_dma) {
            msd_command(rs, 6, sense_dma, 18, true);
            kfree(sense_dma);
        }
        xhci_delay(200);
    }

    total_sects = msd_read_capacity();
    return total_sects > 0;
}

/* ============================================================================
 *  xHCI Controller Initialization
 * ============================================================================ */

/* Find the xHCI controller on PCI. Class=0x0C, Subclass=0x03, ProgIF=0x30. */
static bool pci_find_xhci(uint8_t *o_bus, uint8_t *o_slot, uint8_t *o_func,
                           uint32_t *o_bar0)
{
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t id = pci_read32((uint8_t)bus, slot, func, 0x00);
                if ((id & 0xFFFF) == 0xFFFF || (id & 0xFFFF) == 0)
                    continue;
                uint32_t cr = pci_read32((uint8_t)bus, slot, func, 0x08);
                uint8_t cls = (uint8_t)(cr >> 24);
                uint8_t sub = (uint8_t)(cr >> 16);
                uint8_t pif = (uint8_t)(cr >> 8);
                if (cls == 0x0C && sub == 0x03 && pif == 0x30) {
                    uint32_t bar0 = pci_read32((uint8_t)bus, slot, func, 0x10);
                    bar0 &= ~0xFu;
                    serial_printf("[xhci] found at %u:%u.%u BAR0=%08x\n",
                                  bus, slot, func, bar0);
                    *o_bus = (uint8_t)bus; *o_slot = slot;
                    *o_func = func; *o_bar0 = bar0;
                    return true;
                }
                if (func == 0) {
                    uint8_t hdr = pci_read8((uint8_t)bus, slot, 0, 0x0E);
                    if (!(hdr & 0x80)) break;
                }
            }
        }
    }
    return false;
}

bool xhci_init(void)
{
    have_xhci = false;
    total_sects = 0;

    uint8_t bus, slot_pci, func;
    uint32_t bar0;
    if (!pci_find_xhci(&bus, &slot_pci, &func, &bar0))
        return false;
    if (bar0 == 0 || bar0 == 0xFFFFFFFF)
        return false;

    pci_enable_bus_master(bus, slot_pci, func);

    /* Map MMIO region (map 64 KiB to be safe — xHCI uses a lot of MMIO). */
    for (uint32_t off = 0; off < 0x10000; off += 0x1000) {
        vmm_map_page(bar0 + off, bar0 + off,
                     PAGE_PRESENT | PAGE_RW | PAGE_WRITE_THROUGH |
                     PAGE_CACHE_DISABLE);
    }

    mmio_base = (volatile uint8_t *)(uintptr_t)bar0;

    /* Read capability registers. */
    uint8_t cap_length = *(volatile uint8_t *)mmio_base;
    op_base = mmio_base + cap_length;

    uint32_t hcsparams1 = rd32(mmio_base, XHCI_HCSPARAMS1);
    max_slots = hcsparams1 & 0xFF;
    max_ports = (hcsparams1 >> 24) & 0xFF;

    uint32_t hccparams1 = rd32(mmio_base, XHCI_HCCPARAMS1);
    ctx_size = (hccparams1 & (1u << 2)) ? 64 : 32;

    uint32_t dboff  = rd32(mmio_base, XHCI_DBOFF) & ~3u;
    uint32_t rtsoff = rd32(mmio_base, XHCI_RTSOFF) & ~0x1Fu;
    db_base = (volatile uint32_t *)(mmio_base + dboff);
    rt_base = (volatile uint8_t *)(mmio_base + rtsoff);

    serial_printf("[xhci] cap_len=%u max_slots=%u max_ports=%u ctx=%u\n",
                  cap_length, max_slots, max_ports, ctx_size);

    /* ---- Stop and Reset the controller ---- */
    wr32(op_base, XHCI_USBCMD, 0);   /* clear RS */
    for (int i = 0; i < 100000; i++)
        if (rd32(op_base, XHCI_USBSTS) & XHCI_STS_HCH) break;

    wr32(op_base, XHCI_USBCMD, XHCI_CMD_HCRST);
    for (int i = 0; i < 1000000; i++) {
        if (!(rd32(op_base, XHCI_USBCMD) & XHCI_CMD_HCRST) &&
            !(rd32(op_base, XHCI_USBSTS) & XHCI_STS_CNR))
            break;
    }
    if (rd32(op_base, XHCI_USBSTS) & XHCI_STS_CNR) {
        serial_write("[xhci] controller not ready after reset\n");
        return false;
    }

    /* ---- Set MaxSlotsEn ---- */
    wr32(op_base, XHCI_CONFIG, max_slots & 0xFF);

    /* ---- DCBAA ---- */
    dcbaa = (uint64_t *)kmalloc_aligned((max_slots + 1) * sizeof(uint64_t));
    if (!dcbaa) return false;
    memset(dcbaa, 0, (max_slots + 1) * sizeof(uint64_t));

    /* Scratchpad buffers (if required by the controller). */
    uint32_t hcsparams2 = rd32(mmio_base, XHCI_HCSPARAMS2);
    uint32_t max_sp_hi = (hcsparams2 >> 21) & 0x1F;
    uint32_t max_sp_lo = (hcsparams2 >> 27) & 0x1F;
    uint32_t max_sp = (max_sp_hi << 5) | max_sp_lo;
    if (max_sp > MAX_SCRATCHPADS) max_sp = MAX_SCRATCHPADS;

    if (max_sp > 0) {
        scratchpad_array = (uint64_t *)kmalloc_aligned(max_sp * sizeof(uint64_t));
        if (!scratchpad_array) return false;
        for (uint32_t i = 0; i < max_sp; i++) {
            void *page = kmalloc_aligned(4096);
            if (!page) return false;
            memset(page, 0, 4096);
            scratchpad_array[i] = (uint64_t)(uintptr_t)page;
        }
        dcbaa[0] = (uint64_t)(uintptr_t)scratchpad_array;
        serial_printf("[xhci] allocated %u scratchpad pages\n", max_sp);
    }

    wr64(op_base, XHCI_DCBAAP, (uint64_t)(uintptr_t)dcbaa);

    /* ---- Command Ring ---- */
    cmd_ring = ring_alloc(CMD_RING_SIZE);
    if (!cmd_ring) return false;
    cmd_enqueue = 0;
    cmd_cycle = 1;
    wr64(op_base, XHCI_CRCR,
         (uint64_t)(uintptr_t)cmd_ring | cmd_cycle);

    /* ---- Event Ring ---- */
    evt_ring = ring_alloc(EVT_RING_SIZE);
    if (!evt_ring) return false;
    evt_dequeue = 0;
    evt_cycle = 1;

    erst = (erst_entry_t *)kmalloc_aligned(sizeof(erst_entry_t));
    if (!erst) return false;
    memset(erst, 0, sizeof(erst_entry_t));
    erst->base = (uint64_t)(uintptr_t)evt_ring;
    erst->size = EVT_RING_SIZE;

    /* Interrupter 0 setup.
     * Order matters on real hardware (per xHCI spec 4.2):
     *   1. ERSTSZ   2. ERDP   3. ERSTBA (last — triggers re-init)
     *   4. IMAN (enable) */
    uint32_t ir0 = 0x20;  /* interrupter 0 offset within runtime regs */
    wr32(rt_base, ir0 + 0x04, 0);               /* IMOD: no throttling */
    wr32(rt_base, ir0 + 0x08, 1);               /* ERSTSZ = 1 segment */
    wr64(rt_base, ir0 + 0x18,
         (uint64_t)(uintptr_t)evt_ring);         /* ERDP (no EHB yet) */
    wr64(rt_base, ir0 + 0x10,
         (uint64_t)(uintptr_t)erst);             /* ERSTBA — arms the ring */
    wr32(rt_base, ir0 + 0x00, 0x2);             /* IMAN: IE=1 */

    /* ---- Start the controller ---- */
    wr32(op_base, XHCI_USBCMD, XHCI_CMD_RS | XHCI_CMD_INTE);
    xhci_delay(100);

    if (rd32(op_base, XHCI_USBSTS) & XHCI_STS_HCH) {
        serial_write("[xhci] controller failed to start\n");
        return false;
    }
    serial_write("[xhci] controller running\n");

    /* ---- Drain any pending port status change events ---- */
    xhci_trb_t dummy;
    while (poll_event(&dummy, 100)) ;

    /* ---- Find a port with a connected device ---- */
    int found_port = -1;
    for (uint32_t p = 0; p < max_ports; p++) {
        volatile uint8_t *pr = port_reg(p);
        uint32_t sc = rd32(pr, XHCI_PORTSC);
        if (sc & PORTSC_CCS) {
            serial_printf("[xhci] port %u: connected (PORTSC=%08x)\n", p, sc);
            found_port = (int)p;
            break;
        }
    }
    if (found_port < 0) {
        serial_write("[xhci] no device connected on any port\n");
        return false;
    }

    /* ---- Reset the port ---- */
    volatile uint8_t *pr = port_reg((uint32_t)found_port);
    uint32_t sc = rd32(pr, XHCI_PORTSC);
    /* Preserve PP, clear RW1C bits, set PR (reset) */
    sc = (sc & ~PORTSC_RW1C_MASK) | PORTSC_PR;
    wr32(pr, XHCI_PORTSC, sc);

    /* Wait for reset to complete (PRC set) */
    for (int i = 0; i < 1000000; i++) {
        sc = rd32(pr, XHCI_PORTSC);
        if (sc & PORTSC_PRC) break;
    }
    /* Clear PRC */
    wr32(pr, XHCI_PORTSC, (sc & ~PORTSC_RW1C_MASK) | PORTSC_PRC);
    xhci_delay(50);

    /* Drain port status change events */
    while (poll_event(&dummy, 200)) ;

    sc = rd32(pr, XHCI_PORTSC);
    if (!(sc & PORTSC_PED)) {
        serial_write("[xhci] port not enabled after reset\n");
        return false;
    }
    uint32_t speed = (sc >> 10) & 0xF;
    serial_printf("[xhci] port %d enabled, speed=%u\n", found_port, speed);

    /* ---- Enable Slot ---- */
    xhci_trb_t enable_slot = {0};
    enable_slot.control = (TRB_ENABLE_SLOT << TRB_TYPE_SHIFT);
    xhci_trb_t evt;
    int cc = xhci_command(&enable_slot, &evt);
    if (cc != TRB_CC_SUCCESS) {
        serial_printf("[xhci] Enable Slot failed cc=%d\n", cc);
        return false;
    }
    slot_id = (evt.control >> 24) & 0xFF;
    serial_printf("[xhci] slot %u enabled\n", slot_id);

    /* ---- Allocate input/output contexts ---- */
    uint32_t total_ctx = (max_slots >= 32 ? 32 : max_slots + 1) * ctx_size;
    output_ctx = (uint8_t *)kmalloc_aligned(total_ctx);
    input_ctx  = (uint8_t *)kmalloc_aligned((33) * ctx_size);
    if (!output_ctx || !input_ctx) return false;
    memset(output_ctx, 0, total_ctx);
    memset(input_ctx, 0, 33 * ctx_size);

    dcbaa[slot_id] = (uint64_t)(uintptr_t)output_ctx;

    /* ---- EP0 transfer ring ---- */
    ep0_ring = ring_alloc(XFER_RING_SIZE);
    if (!ep0_ring) return false;
    ep0_enqueue = 0;
    ep0_cycle = 1;

    /* ---- Build input context for Address Device ---- */
    /* Input Control Context: A0 (slot) and A1 (EP0) set */
    uint32_t *icc = (uint32_t *)input_ctx;
    icc[1] = 0x3;   /* add flags: slot + EP0 */

    /* Slot Context (at offset ctx_size in input context) */
    uint32_t *slot_ctx = (uint32_t *)(input_ctx + ctx_size);
    uint32_t route_string = 0;
    uint32_t mep = speed >= 4 ? 1 : 0;  /* multi-endpoint */
    slot_ctx[0] = (1 << 27) |            /* context entries = 1 (just EP0) */
                  (speed << 20) |         /* speed */
                  route_string;
    slot_ctx[1] = ((uint32_t)(found_port + 1) << 16);  /* root hub port # */

    /* EP0 Context (at offset 2*ctx_size) */
    uint32_t *ep0_ctx = (uint32_t *)(input_ctx + 2 * ctx_size);
    uint32_t max_packet_ep0;
    switch (speed) {
        case 1: max_packet_ep0 = 8;   break;  /* Low Speed */
        case 2: max_packet_ep0 = 8;   break;  /* Full Speed */
        case 3: max_packet_ep0 = 64;  break;  /* High Speed */
        case 4: max_packet_ep0 = 512; break;  /* SuperSpeed */
        default: max_packet_ep0 = 64; break;
    }
    ep0_ctx[0] = 0;
    ep0_ctx[1] = (max_packet_ep0 << 16) | (4 << 3) | (3 << 1);
    /* EP type = 4 (Control Bi), CErr=3, max_packet */
    ep0_ctx[2] = (uint32_t)(uintptr_t)ep0_ring | ep0_cycle;
    ep0_ctx[3] = 0;
    ep0_ctx[4] = 8;  /* average TRB length */

    /* ---- Address Device command ---- */
    xhci_trb_t addr_dev = {0};
    addr_dev.param_lo = (uint32_t)(uintptr_t)input_ctx;
    addr_dev.param_hi = 0;
    addr_dev.control  = (TRB_ADDRESS_DEVICE << TRB_TYPE_SHIFT) |
                        ((uint32_t)slot_id << 24);
    cc = xhci_command(&addr_dev, &evt);
    if (cc != TRB_CC_SUCCESS) {
        serial_printf("[xhci] Address Device failed cc=%d\n", cc);
        return false;
    }
    serial_printf("[xhci] device addressed (slot %u)\n", slot_id);

    /* ---- Read Device Descriptor ---- */
    uint8_t *dev_desc = (uint8_t *)kmalloc_aligned(18);
    if (!dev_desc) return false;
    memset(dev_desc, 0, 18);

    int rc = xhci_control_transfer(0x80, 6, (USB_DESC_DEVICE << 8), 0, 18, dev_desc);
    if (rc != 0) {
        /* Some devices only accept 8-byte first read */
        memset(dev_desc, 0, 18);
        rc = xhci_control_transfer(0x80, 6, (USB_DESC_DEVICE << 8), 0, 8, dev_desc);
        if (rc != 0) {
            serial_write("[xhci] failed to read device descriptor\n");
            kfree(dev_desc);
            return false;
        }
    }

    uint16_t vid = dev_desc[8] | (dev_desc[9] << 8);
    uint16_t pid = dev_desc[10] | (dev_desc[11] << 8);
    uint8_t  dev_class = dev_desc[4];
    serial_printf("[xhci] device: VID=%04x PID=%04x class=%02x\n",
                  vid, pid, dev_class);
    kfree(dev_desc);

    /* ---- Read Configuration Descriptor (first 9 bytes to get total length) ---- */
    uint8_t *cfg9 = (uint8_t *)kmalloc_aligned(9);
    if (!cfg9) return false;
    memset(cfg9, 0, 9);
    rc = xhci_control_transfer(0x80, 6, (USB_DESC_CONFIG << 8), 0, 9, cfg9);
    if (rc != 0) { kfree(cfg9); serial_write("[xhci] can't read config desc\n"); return false; }

    uint16_t total_len = cfg9[2] | (cfg9[3] << 8);
    kfree(cfg9);
    if (total_len > 512) total_len = 512;

    uint8_t *cfg = (uint8_t *)kmalloc_aligned(total_len);
    if (!cfg) return false;
    memset(cfg, 0, total_len);
    rc = xhci_control_transfer(0x80, 6, (USB_DESC_CONFIG << 8), 0, total_len, cfg);
    if (rc != 0) { kfree(cfg); return false; }

    /* Parse descriptors looking for Mass Storage BOT interface + Bulk endpoints */
    bool is_msd = false;
    uint8_t intf_num = 0;
    uint8_t ep_in_addr = 0, ep_out_addr = 0;
    uint16_t ep_in_maxpkt = 512, ep_out_maxpkt = 512;

    uint32_t pos = 0;
    while (pos + 2 <= total_len) {
        uint8_t dlen = cfg[pos];
        uint8_t dtype = cfg[pos + 1];
        if (dlen == 0) break;

        if (dtype == USB_DESC_INTERFACE && pos + 9 <= total_len) {
            intf_num = cfg[pos + 2];
            uint8_t icls = cfg[pos + 5];
            uint8_t isub = cfg[pos + 6];
            uint8_t ipro = cfg[pos + 7];
            if (icls == USB_CLASS_MASS_STORAGE && isub == USB_SUBCLASS_SCSI &&
                ipro == USB_PROTOCOL_BOT) {
                is_msd = true;
                serial_printf("[xhci] MSD BOT interface %u found\n", intf_num);
            }
        }
        if (dtype == USB_DESC_ENDPOINT && pos + 7 <= total_len && is_msd) {
            uint8_t ep_addr = cfg[pos + 2];
            uint8_t ep_attr = cfg[pos + 3];
            uint16_t maxp   = cfg[pos + 4] | (cfg[pos + 5] << 8);
            if ((ep_attr & 3) == 2) {  /* Bulk */
                if (ep_addr & 0x80) {
                    ep_in_addr = ep_addr;
                    ep_in_maxpkt = maxp;
                } else {
                    ep_out_addr = ep_addr;
                    ep_out_maxpkt = maxp;
                }
            }
        }
        pos += dlen;
    }
    kfree(cfg);

    if (!is_msd || !ep_in_addr || !ep_out_addr) {
        serial_write("[xhci] device is not USB Mass Storage BOT\n");
        return false;
    }

    serial_printf("[xhci] bulk IN=ep%u OUT=ep%u\n",
                  ep_in_addr & 0x7F, ep_out_addr & 0x7F);

    /* ---- Set Configuration ---- */
    rc = xhci_control_transfer(0x00, 9, 1, 0, 0, NULL);
    if (rc != 0) serial_write("[xhci] warning: SetConfiguration failed\n");

    /* ---- Configure Endpoint (add bulk IN + bulk OUT to the slot) ---- */
    bulk_in_ring = ring_alloc(XFER_RING_SIZE);
    bulk_out_ring = ring_alloc(XFER_RING_SIZE);
    if (!bulk_in_ring || !bulk_out_ring) return false;
    bulk_in_enqueue = bulk_out_enqueue = 0;
    bulk_in_cycle = bulk_out_cycle = 1;
    bulk_in_max_packet = ep_in_maxpkt;
    bulk_out_max_packet = ep_out_maxpkt;

    /* DCI = Endpoint*2 + direction.  EP1 IN → DCI 3, EP2 OUT → DCI 4, etc. */
    uint8_t ep_in_num  = ep_in_addr & 0x7F;
    uint8_t ep_out_num = ep_out_addr & 0x7F;
    bulk_in_dci  = ep_in_num * 2 + 1;   /* IN  endpoint → odd DCI */
    bulk_out_dci = ep_out_num * 2;       /* OUT endpoint → even DCI */

    /* Build new input context for Configure Endpoint */
    memset(input_ctx, 0, 33 * ctx_size);
    uint32_t *icc2 = (uint32_t *)input_ctx;

    /* Add context flags: slot ctx + both endpoint contexts */
    icc2[1] = (1u << 0) |               /* slot */
              (1u << bulk_in_dci) |
              (1u << bulk_out_dci);

    /* Slot context: update context entries to include the highest DCI */
    uint32_t *slot_ctx2 = (uint32_t *)(input_ctx + ctx_size);
    uint32_t highest_dci = bulk_in_dci > bulk_out_dci ? bulk_in_dci : bulk_out_dci;
    slot_ctx2[0] = (highest_dci << 27) | (speed << 20);
    slot_ctx2[1] = ((uint32_t)(found_port + 1) << 16);

    /* Bulk IN endpoint context */
    uint32_t *ep_in_ctx = (uint32_t *)(input_ctx + (bulk_in_dci + 1) * ctx_size);
    ep_in_ctx[0] = 0;
    ep_in_ctx[1] = (ep_in_maxpkt << 16) | (6 << 3) | (3 << 1);
    /* EP type 6 = Bulk IN, CErr=3 */
    ep_in_ctx[2] = (uint32_t)(uintptr_t)bulk_in_ring | bulk_in_cycle;
    ep_in_ctx[3] = 0;
    ep_in_ctx[4] = ep_in_maxpkt;   /* avg TRB length */

    /* Bulk OUT endpoint context */
    uint32_t *ep_out_ctx = (uint32_t *)(input_ctx + (bulk_out_dci + 1) * ctx_size);
    ep_out_ctx[0] = 0;
    ep_out_ctx[1] = (ep_out_maxpkt << 16) | (2 << 3) | (3 << 1);
    /* EP type 2 = Bulk OUT, CErr=3 */
    ep_out_ctx[2] = (uint32_t)(uintptr_t)bulk_out_ring | bulk_out_cycle;
    ep_out_ctx[3] = 0;
    ep_out_ctx[4] = ep_out_maxpkt;

    /* Issue Configure Endpoint command */
    xhci_trb_t cfg_ep = {0};
    cfg_ep.param_lo = (uint32_t)(uintptr_t)input_ctx;
    cfg_ep.param_hi = 0;
    cfg_ep.control  = (TRB_CONFIG_EP << TRB_TYPE_SHIFT) |
                      ((uint32_t)slot_id << 24);
    cc = xhci_command(&cfg_ep, &evt);
    if (cc != TRB_CC_SUCCESS) {
        serial_printf("[xhci] Configure Endpoint failed cc=%d\n", cc);
        return false;
    }
    serial_write("[xhci] endpoints configured\n");

    /* ---- Initialize Mass Storage device ---- */
    if (!msd_init_device()) {
        serial_write("[xhci] mass storage init failed\n");
        return false;
    }

    serial_printf("[xhci] USB disk ready: %u sectors (%u MiB)\n",
                  total_sects, total_sects / 2048);
    have_xhci = true;
    return true;
}

/* ============================================================================
 *  Public API: sector read/write via SCSI READ(10)/WRITE(10)
 * ============================================================================ */

bool     xhci_present(void)       { return have_xhci; }
uint32_t xhci_total_sectors(void) { return total_sects; }

int xhci_read_sectors(uint32_t lba, uint16_t count, void *buf)
{
    if (!have_xhci || count == 0) return -10;

    /* SCSI READ(10): max 128 sectors per command to keep DMA buffer small */
    uint8_t *out = (uint8_t *)buf;
    uint16_t remaining = count;
    uint32_t cur_lba = lba;

    while (remaining > 0) {
        uint16_t chunk = remaining > 128 ? 128 : remaining;
        uint32_t bytes = (uint32_t)chunk * 512;

        uint8_t *dma_buf = (uint8_t *)kmalloc_aligned(bytes);
        if (!dma_buf) return -12;
        memset(dma_buf, 0, bytes);

        uint8_t cmd[10] = {0};
        cmd[0] = SCSI_READ_10;
        cmd[2] = (cur_lba >> 24) & 0xFF;
        cmd[3] = (cur_lba >> 16) & 0xFF;
        cmd[4] = (cur_lba >> 8)  & 0xFF;
        cmd[5] = cur_lba & 0xFF;
        cmd[7] = (chunk >> 8) & 0xFF;
        cmd[8] = chunk & 0xFF;

        int rc = msd_command(cmd, 10, dma_buf, bytes, true);
        if (rc != 0) { kfree(dma_buf); return rc; }

        memcpy(out, dma_buf, bytes);
        kfree(dma_buf);

        out += bytes;
        cur_lba += chunk;
        remaining -= chunk;
    }
    return 0;
}

int xhci_write_sectors(uint32_t lba, uint16_t count, const void *buf)
{
    if (!have_xhci || count == 0) return -10;

    const uint8_t *in = (const uint8_t *)buf;
    uint16_t remaining = count;
    uint32_t cur_lba = lba;

    while (remaining > 0) {
        uint16_t chunk = remaining > 128 ? 128 : remaining;
        uint32_t bytes = (uint32_t)chunk * 512;

        uint8_t *dma_buf = (uint8_t *)kmalloc_aligned(bytes);
        if (!dma_buf) return -12;
        memcpy(dma_buf, in, bytes);

        uint8_t cmd[10] = {0};
        cmd[0] = SCSI_WRITE_10;
        cmd[2] = (cur_lba >> 24) & 0xFF;
        cmd[3] = (cur_lba >> 16) & 0xFF;
        cmd[4] = (cur_lba >> 8)  & 0xFF;
        cmd[5] = cur_lba & 0xFF;
        cmd[7] = (chunk >> 8) & 0xFF;
        cmd[8] = chunk & 0xFF;

        int rc = msd_command(cmd, 10, dma_buf, bytes, false);
        kfree(dma_buf);
        if (rc != 0) return rc;

        in += bytes;
        cur_lba += chunk;
        remaining -= chunk;
    }
    return 0;
}

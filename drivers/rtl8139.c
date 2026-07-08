/* drivers/rtl8139.c — Driver RTL8139 para Trinux.
 *
 * CAMBIO #7: primer driver de red real en Trinux.
 *
 * Registros relevantes del RTL8139 (offset desde iobase):
 *   0x00-0x05  MAC address (read)
 *   0x08       TX Status 0-3 (4 × u32)
 *   0x20       TX Address 0-3 (4 × u32) — dirección física del buffer TX
 *   0x30       RX Buffer Start Address
 *   0x38       Early Rx Byte Count / Rx Buffer Head (CAPR)
 *   0x3A       Rx Buffer Tail pointer (CBR)
 *   0x3C       Interrupt Mask Register
 *   0x3E       Interrupt Status Register
 *   0x40       TX Config
 *   0x44       RX Config
 *   0x50       Timer (no lo usamos)
 *   0x52       Missed Packet Counter
 *   0x60       ID Registers (magic)
 *   0x74       Media Status
 *   0x37       Command Register (bit1=RxEnable, bit2=TxEnable, bit4=Reset)
 */
#include "rtl8139.h"
#include "pci.h"
#include "../cpu/ports.h"
#include "../mm/kheap.h"
#include "../lib/string.h"
#include "../lib/printf.h"
#include "../drivers/serial.h"

/* ---- Constantes ---- */
#define RTL8139_VENDOR  0x10EC
#define RTL8139_DEVICE  0x8139

/* Offsets de registros */
#define REG_MAC0        0x00
#define REG_MAR0        0x08   /* Multicast filter */
#define REG_TXSTATUS0   0x10   /* TX Status desc 0 (4 × u32, stride 4) */
#define REG_TXADDR0     0x20   /* TX Address desc 0 (4 × u32, stride 4) */
#define REG_RXBUF       0x30
#define REG_CAPR        0x38   /* Current Address of Packet Read */
#define REG_CBR         0x3A   /* Current Buffer Address (tail) */
#define REG_CMD         0x37
#define REG_IMR         0x3C   /* Interrupt Mask */
#define REG_ISR         0x3E   /* Interrupt Status */
#define REG_TXCFG       0x40
#define REG_RXCFG       0x44
#define REG_CONFIG1     0x52

/* Bits del registro CMD */
#define CMD_RESET       0x10
#define CMD_RX_ENABLE   0x08
#define CMD_TX_ENABLE   0x04

/* TX Status bits */
#define TXS_OWN         0x00002000   /* NIC no ha transmitido aún (TOK bit) */
#define TXS_TOK         0x00008000   /* Transmit OK */

/* RX Config */
#define RCR_AAP         (1 << 0)   /* Accept All Packets */
#define RCR_APM         (1 << 1)   /* Accept Physical Match */
#define RCR_AM          (1 << 2)   /* Accept Multicast */
#define RCR_AB          (1 << 3)   /* Accept Broadcast */
#define RCR_WRAP        (1 << 7)   /* Ring buffer wrap */
#define RCR_MXDMA_UNLIM (7 << 8)   /* Max DMA burst unlimited */
#define RCR_RBLEN_32K   (2 << 11)  /* RX buffer size 32K + 16 + 1500 */
#define RCR_RXFTH_NONE  (7 << 13)  /* No FIFO threshold */

/* Interrupt bits */
#define ISR_ROK         (1 << 0)   /* Receive OK */
#define ISR_TOK         (1 << 2)   /* Transmit OK */

/* Tamaño del ring buffer de recepción (32 KB + 16 bytes de margen + 1500 ETH) */
#define RX_BUF_SIZE     (32 * 1024 + 16 + 1500)
/* Tamaño del buffer TX (uno por descriptor, máximo 4096 bytes) */
#define TX_BUF_SIZE     2048
#define TX_DESCS        4

/* ---- Estado del driver ---- */
static bool     nic_present = false;
static uint16_t iobase      = 0;
static uint8_t  mac_addr[6];

/* Buffers en RAM (deben estar identity-mapped para DMA) */
static uint8_t  *rx_buf   = NULL;
static uint8_t  *tx_bufs[TX_DESCS];
static uint32_t  rx_pos   = 0;   /* posición actual de lectura en rx_buf */
static uint8_t   tx_desc  = 0;   /* próximo descriptor TX a usar */

/* ---- I/O helpers ---- */
static inline void nic_outb(uint16_t off, uint8_t  v) { outb(iobase + off, v); }
static inline void nic_outw(uint16_t off, uint16_t v) { outw(iobase + off, v); }
static inline void nic_outl(uint16_t off, uint32_t v) { outl(iobase + off, v); }
static inline uint8_t  nic_inb(uint16_t off) { return inb(iobase + off); }
static inline uint16_t nic_inw(uint16_t off) { return inw(iobase + off); }
static inline uint32_t nic_inl(uint16_t off) { return inl(iobase + off); }

/* ---- Inicialización ---- */

bool rtl8139_init(void)
{
    /* Buscar el RTL8139 escaneando el bus PCI (vendor/device scan manual). */
    uint8_t  found_bus = 0, found_slot = 0, found_func = 0;
    bool     found = false;

    for (uint8_t bus = 0; bus < 8 && !found; bus++) {
        for (uint8_t slot = 0; slot < 32 && !found; slot++) {
            uint32_t id = pci_read32(bus, slot, 0, 0);
            uint16_t vendor = (uint16_t)(id & 0xFFFF);
            uint16_t device = (uint16_t)(id >> 16);
            if (vendor == RTL8139_VENDOR && device == RTL8139_DEVICE) {
                found_bus  = bus;
                found_slot = slot;
                found_func = 0;
                found      = true;
            }
        }
    }

    if (!found) {
        serial_write("[rtl8139] no RTL8139 found on PCI bus\n");
        return false;
    }

    /* Habilitar Bus Mastering + I/O Space en el Command Register PCI */
    uint32_t cmd32 = pci_read32(found_bus, found_slot, found_func, 0x04);
    uint16_t pci_cmd = (uint16_t)(cmd32 & 0xFFFF);
    pci_cmd |= 0x0005;   /* I/O Enable + Bus Master */
    pci_write32(found_bus, found_slot, found_func, 0x04,
                (cmd32 & 0xFFFF0000u) | pci_cmd);

    /* Obtener la I/O base address (BAR0) */
    iobase = (uint16_t)(pci_read32(found_bus, found_slot, found_func, 0x10) & ~0x3u);
    if (!iobase) {
        serial_write("[rtl8139] BAR0 is 0\n");
        return false;
    }

    /* Power on */
    nic_outb(REG_CONFIG1, 0x00);

    /* Software Reset */
    nic_outb(REG_CMD, CMD_RESET);
    for (int i = 0; i < 1000; i++) {
        if (!(nic_inb(REG_CMD) & CMD_RESET)) break;
        __asm__ volatile("pause");
    }

    /* Alocar buffers en kheap (identity-mapped → phys == virt) */
    rx_buf = (uint8_t *)kmalloc_aligned(RX_BUF_SIZE + 4);
    if (!rx_buf) { serial_write("[rtl8139] no RAM for RX buf\n"); return false; }
    memset(rx_buf, 0, RX_BUF_SIZE + 4);

    for (int i = 0; i < TX_DESCS; i++) {
        tx_bufs[i] = (uint8_t *)kmalloc_aligned(TX_BUF_SIZE);
        if (!tx_bufs[i]) { serial_write("[rtl8139] no RAM for TX buf\n"); return false; }
    }

    /* Leer MAC */
    for (int i = 0; i < 6; i++)
        mac_addr[i] = nic_inb(REG_MAC0 + i);

    /* Configurar RX buffer */
    nic_outl(REG_RXBUF, (uint32_t)rx_buf);

    /* Habilitar RX + TX */
    nic_outb(REG_CMD, CMD_RX_ENABLE | CMD_TX_ENABLE);

    /* RX config: accept broadcast + unicast + multicast, ring wrap, 32K buf */
    nic_outl(REG_RXCFG, RCR_APM | RCR_AB | RCR_AM | RCR_WRAP
                        | RCR_MXDMA_UNLIM | RCR_RBLEN_32K | RCR_RXFTH_NONE);

    /* TX config: DMA burst unlimited, IFG standard */
    nic_outl(REG_TXCFG, 0x03000700);

    /* Habilitar interrupciones ROK + TOK (aunque usemos polling por ahora) */
    nic_outw(REG_IMR, ISR_ROK | ISR_TOK);

    /* Inicializar puntero de lectura del RX ring */
    rx_pos  = 0;
    tx_desc = 0;

    nic_present = true;
    kprintf("  [ OK ] RTL8139 NIC @ I/O 0x%x, MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            iobase,
            mac_addr[0], mac_addr[1], mac_addr[2],
            mac_addr[3], mac_addr[4], mac_addr[5]);
    return true;
}

bool rtl8139_present(void) { return nic_present; }

void rtl8139_mac(uint8_t mac[6])
{
    for (int i = 0; i < 6; i++) mac[i] = mac_addr[i];
}

/* ---- Transmisión ---- */

bool rtl8139_send(const uint8_t *frame, uint16_t len)
{
    if (!nic_present || !frame || len == 0 || len > TX_BUF_SIZE) return false;

    /* Copiar frame al buffer TX del descriptor actual */
    memcpy(tx_bufs[tx_desc], frame, len);

    /* Escribir dirección física del buffer */
    nic_outl(REG_TXADDR0 + tx_desc * 4, (uint32_t)tx_bufs[tx_desc]);

    /* Iniciar transmisión: escribir tamaño en TX Status (borra TOK/OWN) */
    nic_outl(REG_TXSTATUS0 + tx_desc * 4, (uint32_t)len);

    /* Esperar a que la NIC transmita (polling, timeout simple) */
    for (int i = 0; i < 100000; i++) {
        uint32_t status = nic_inl(REG_TXSTATUS0 + tx_desc * 4);
        if (status & TXS_TOK) break;
        __asm__ volatile("pause");
    }

    /* Avanzar al siguiente descriptor (circular) */
    tx_desc = (tx_desc + 1) & (TX_DESCS - 1);
    return true;
}

/* ---- Recepción ---- */

int rtl8139_recv(uint8_t *buf, uint16_t maxlen)
{
    if (!nic_present) return -1;

    /* Verificar si hay datos en el ring buffer */
    uint16_t status = nic_inw(REG_ISR);
    if (!(status & ISR_ROK)) return 0;   /* sin datos */

    /* Leer cabecera del paquete en el ring:
     * [0:1] RX Status, [2:3] Tamaño del paquete (incluye CRC) */
    uint16_t rx_status = *((uint16_t *)(rx_buf + rx_pos));
    uint16_t rx_size   = *((uint16_t *)(rx_buf + rx_pos + 2));

    /* Verificar que el paquete es válido */
    if (!(rx_status & 0x0001) || rx_size < 4 || rx_size > ETH_FRAME_MAX + 4) {
        /* Reiniciar el ring en caso de error */
        nic_outw(REG_CAPR, (uint16_t)(nic_inw(REG_CBR) - 16));
        nic_outw(REG_ISR, ISR_ROK);
        rx_pos = 0;
        return -1;
    }

    uint16_t data_len = rx_size - 4;   /* quitar 4 bytes de CRC */
    if (data_len > maxlen) data_len = maxlen;

    /* Copiar datos del ring buffer (con wrap-around si es necesario) */
    uint32_t data_start = rx_pos + 4;
    uint32_t available  = RX_BUF_SIZE - (data_start % RX_BUF_SIZE);

    if (available >= data_len) {
        memcpy(buf, rx_buf + (data_start % RX_BUF_SIZE), data_len);
    } else {
        /* El paquete cruza el final del ring */
        memcpy(buf, rx_buf + (data_start % RX_BUF_SIZE), available);
        memcpy(buf + available, rx_buf, data_len - available);
    }

    /* Avanzar puntero de lectura (alineado a 4 bytes) */
    rx_pos = (rx_pos + rx_size + 4 + 3) & ~3;
    rx_pos %= RX_BUF_SIZE;

    /* Actualizar CAPR (le restamos 16 como indica el datasheet) */
    nic_outw(REG_CAPR, (uint16_t)((rx_pos - 16) % RX_BUF_SIZE));
    nic_outw(REG_ISR, ISR_ROK);   /* ACK la interrupción */

    return (int)data_len;
}

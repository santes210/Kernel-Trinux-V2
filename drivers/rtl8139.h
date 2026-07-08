/* drivers/rtl8139.h — Driver de red RTL8139 (Realtek 10/100 Mbps).
 *
 * CAMBIO #7: primer driver de red para Trinux.
 *
 * El RTL8139 es la NIC más simple posible que QEMU emula (-nic rtl8139):
 *   - Espacio de I/O en 32 registros de 8/16/32 bits.
 *   - TX: 4 descriptores de transmisión, DMA lineal.
 *   - RX: ring buffer de recepción en RAM (wrap-around).
 *   - Interrupciones: IRQ compartida (PIC), o polling en este driver.
 *
 * API pública:
 *   rtl8139_init()       — detecta la NIC via PCI, la configura.
 *   rtl8139_send()       — envía un frame Ethernet.
 *   rtl8139_recv()       — recibe el siguiente frame (no bloqueante).
 *   rtl8139_present()    — true si hay NIC disponible.
 *   rtl8139_mac()        — devuelve la dirección MAC (6 bytes).
 *
 * Para usarla en QEMU:
 *   qemu-system-i386 -kernel mykernel.bin -nic model=rtl8139,mac=52:54:00:12:34:56
 */
#ifndef DRIVERS_RTL8139_H
#define DRIVERS_RTL8139_H

#include "../lib/types.h"

#define ETH_FRAME_MAX  1514   /* payload máximo de Ethernet (sin VLAN) */
#define ETH_HDR_LEN      14   /* 6 dst + 6 src + 2 type */

typedef struct {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t ethertype;   /* big-endian */
} __attribute__((packed)) eth_hdr_t;

/* Inicializa el RTL8139. Devuelve true si se encontró y configuró. */
bool rtl8139_init(void);

/* True si hay NIC disponible. */
bool rtl8139_present(void);

/* Dirección MAC del adaptador (6 bytes). */
void rtl8139_mac(uint8_t mac[6]);

/* Envía un frame Ethernet completo (cabecera + datos).
 * `frame` debe tener al menos ETH_HDR_LEN bytes + datos.
 * Devuelve true si el frame se encoló correctamente. */
bool rtl8139_send(const uint8_t *frame, uint16_t len);

/* Intenta recibir el siguiente frame del ring buffer.
 * Copia hasta `maxlen` bytes en `buf`.
 * Devuelve la longitud del frame, 0 si no hay frame disponible, -1 en error. */
int rtl8139_recv(uint8_t *buf, uint16_t maxlen);

#endif /* DRIVERS_RTL8139_H */

/* drivers/net.h — Stack de red mínimo: ARP + IP + ICMP (ping).
 *
 * CAMBIO #9: protocolo de red sobre el driver RTL8139.
 *
 * Implementa el mínimo necesario para que `ping` funcione:
 *   - ARP: resolución de IP → MAC (tabla estática de 16 entradas)
 *   - IPv4: cabecera, checksum, enrutamiento básico (solo LAN)
 *   - ICMP: Echo Request (ping) y Echo Reply
 *
 * Limitaciones intencionales:
 *   - Sin TCP ni UDP en esta versión.
 *   - Sin fragmentación IP.
 *   - Sin routing entre subredes (solo LAN directa).
 *   - IP y máscara configuradas estáticamente en net_init().
 */
#ifndef DRIVERS_NET_H
#define DRIVERS_NET_H

#include "../lib/types.h"

/* Inicializa el stack de red.
 * `ip`  — dirección IP del host (ej: {192,168,1,100})
 * `gw`  — gateway (ej: {192,168,1,1})
 * `mask`— máscara (ej: {255,255,255,0})
 * Devuelve true si hay NIC disponible. */
bool net_init(const uint8_t ip[4], const uint8_t gw[4], const uint8_t mask[4]);

/* Arranca el stack tomando la configuración de la cmdline del kernel:
 *   trinux.ip=10.0.2.15  trinux.gw=10.0.2.2  trinux.mask=255.255.255.0
 * Si no se pasa nada usa 10.0.2.15/24 gw 10.0.2.2 (la red por defecto de
 * QEMU con `-nic user`), que es lo que hace falta para que `ping` funcione
 * "out of the box" en la VM.  Devuelve false si no hay NIC.
 * Se llama desde kernel_main() — antes NADIE llamaba a rtl8139_init() ni a
 * net_init(), así que el stack de red era código muerto. */
bool net_init_from_cli(const char *cmdline);

/* Parsea "a.b.c.d" en out[4]. Devuelve false si el formato es inválido. */
bool parse_ipv4(const char *str, uint8_t out[4]);

/* Contadores de tráfico (para `ifconfig`). */
uint32_t net_rx_frames(void);
uint32_t net_tx_frames(void);

/* ¿Hay NIC inicializada? */
bool net_ready(void);

/* Estado actual (para `ifconfig`). Devuelve false si no hay red. */
bool net_status(uint8_t ip[4], uint8_t gw[4], uint8_t mask[4], uint8_t mac[6]);

/* Envía un ping ICMP Echo Request a `dst_ip`.
 * Espera la respuesta hasta `timeout_ms` ms.
 * Devuelve el tiempo de ida y vuelta en ms, o -1 si no hubo respuesta. */
int net_ping(const uint8_t dst_ip[4], uint32_t timeout_ms);

/* Procesa los frames pendientes (debe llamarse periódicamente o desde IRQ).
 * Responde automáticamente a pings recibidos. */
void net_poll(void);

/* Devuelve la IP configurada. */
void net_get_ip(uint8_t ip[4]);

/* Devuelve la MAC del adaptador. */
void net_get_mac(uint8_t mac[6]);

/* Resuelve IP → MAC via ARP (espera hasta timeout_ms ms).
 * Devuelve true y rellena `mac_out` si OK. */
bool net_arp_resolve(const uint8_t ip[4], uint8_t mac_out[6], uint32_t timeout_ms);

#endif /* DRIVERS_NET_H */

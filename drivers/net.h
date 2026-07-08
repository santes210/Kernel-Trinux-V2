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

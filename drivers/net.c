/* drivers/net.c — Stack de red mínimo ARP + IPv4 + ICMP.
 *
 * CAMBIO #9: primer protocolo de red para Trinux.
 *
 * Capas implementadas (de abajo a arriba):
 *   L2 — Ethernet II (via rtl8139.c)
 *   L2 — ARP (Address Resolution Protocol): ip→mac lookup + reply
 *   L3 — IPv4: cabecera + checksum (solo unicast, sin fragm.)
 *   L3 — ICMP: Echo Request/Reply (ping)
 */
#include "net.h"
#include "rtl8139.h"
#include "../lib/string.h"
#include "../lib/printf.h"
#include "../drivers/timer.h"

/* ---- Tabla ARP ---- */
#define ARP_TABLE_SIZE 16

typedef struct {
    uint8_t ip[4];
    uint8_t mac[6];
    bool    valid;
} arp_entry_t;

static arp_entry_t arp_table[ARP_TABLE_SIZE];

/* ---- Estado de red ---- */
static bool    net_ready = false;
static uint8_t my_ip[4];
static uint8_t my_mac[6];
static uint8_t my_gw[4];
static uint8_t my_mask[4];

static uint16_t ping_seq = 0;   /* sequence number de los pings */

/* ---- Helpers de endian (el protocolo IP es big-endian) ---- */
static uint16_t htons(uint16_t v) { return (uint16_t)((v>>8)|(v<<8)); }
static uint32_t htonl(uint32_t v) {
    return ((v>>24)&0xFF)|((v>>8)&0xFF00)|((v<<8)&0xFF0000)|((v<<24)&0xFF000000u);
}
#define ntohs htons
#define ntohl htonl

/* ---- Checksum IP/ICMP (RFC 1071) ---- */
static uint16_t inet_checksum(const void *data, uint32_t len)
{
    const uint16_t *p = (const uint16_t *)data;
    uint32_t sum = 0;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len) sum += *(const uint8_t *)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

/* ---- Estructuras de protocolo ---- */

typedef struct {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t ethertype;
} __attribute__((packed)) eth_t;

#define ETH_ARP  0x0608   /* big-endian 0x0806 */
#define ETH_IP   0x0008   /* big-endian 0x0800 */

typedef struct {
    uint16_t htype;   /* 0x0001 = Ethernet */
    uint16_t ptype;   /* 0x0800 = IPv4 */
    uint8_t  hlen;    /* 6 */
    uint8_t  plen;    /* 4 */
    uint16_t oper;    /* 1=request, 2=reply */
    uint8_t  sha[6];  /* sender MAC */
    uint8_t  spa[4];  /* sender IP */
    uint8_t  tha[6];  /* target MAC */
    uint8_t  tpa[4];  /* target IP */
} __attribute__((packed)) arp_t;

typedef struct {
    uint8_t  ihl_ver;   /* version=4, IHL=5 → 0x45 */
    uint8_t  dscp;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  proto;     /* 1=ICMP */
    uint16_t checksum;
    uint8_t  src[4];
    uint8_t  dst[4];
} __attribute__((packed)) ipv4_t;

typedef struct {
    uint8_t  type;      /* 8=request, 0=reply */
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
    uint8_t  data[32];
} __attribute__((packed)) icmp_t;

/* ---- ARP ---- */

static arp_entry_t *arp_lookup(const uint8_t ip[4])
{
    for (int i = 0; i < ARP_TABLE_SIZE; i++)
        if (arp_table[i].valid && memcmp(arp_table[i].ip, ip, 4) == 0)
            return &arp_table[i];
    return NULL;
}

static void arp_insert(const uint8_t ip[4], const uint8_t mac[6])
{
    /* Buscar slot vacío o reemplazar el primero */
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (!arp_table[i].valid || memcmp(arp_table[i].ip, ip, 4) == 0) {
            memcpy(arp_table[i].ip, ip, 4);
            memcpy(arp_table[i].mac, mac, 6);
            arp_table[i].valid = true;
            return;
        }
    }
    /* Tabla llena: reemplazar slot 0 */
    memcpy(arp_table[0].ip, ip, 4);
    memcpy(arp_table[0].mac, mac, 6);
    arp_table[0].valid = true;
}

static void arp_send_request(const uint8_t target_ip[4])
{
    static const uint8_t broadcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    uint8_t frame[42];
    eth_t *eth = (eth_t *)frame;
    arp_t *arp = (arp_t *)(frame + 14);

    memcpy(eth->dst, broadcast, 6);
    memcpy(eth->src, my_mac, 6);
    eth->ethertype = ETH_ARP;

    arp->htype = htons(1);
    arp->ptype = htons(0x0800);
    arp->hlen  = 6;
    arp->plen  = 4;
    arp->oper  = htons(1);   /* request */
    memcpy(arp->sha, my_mac, 6);
    memcpy(arp->spa, my_ip, 4);
    memset(arp->tha, 0, 6);
    memcpy(arp->tpa, target_ip, 4);

    rtl8139_send(frame, 42);
}

static void arp_send_reply(const uint8_t dst_mac[6], const uint8_t dst_ip[4])
{
    uint8_t frame[42];
    eth_t *eth = (eth_t *)frame;
    arp_t *arp = (arp_t *)(frame + 14);

    memcpy(eth->dst, dst_mac, 6);
    memcpy(eth->src, my_mac, 6);
    eth->ethertype = ETH_ARP;

    arp->htype = htons(1);
    arp->ptype = htons(0x0800);
    arp->hlen  = 6;
    arp->plen  = 4;
    arp->oper  = htons(2);   /* reply */
    memcpy(arp->sha, my_mac, 6);
    memcpy(arp->spa, my_ip, 4);
    memcpy(arp->tha, dst_mac, 6);
    memcpy(arp->tpa, dst_ip, 4);

    rtl8139_send(frame, 42);
}

/* ---- ICMP ---- */

static void icmp_send_reply(const uint8_t dst_mac[6], const uint8_t dst_ip[4],
                            uint16_t id, uint16_t seq,
                            const uint8_t *payload, uint16_t plen)
{
    uint16_t total_ip  = (uint16_t)(20 + 8 + plen);
    uint16_t frame_len = (uint16_t)(14 + total_ip);
    uint8_t  frame[1500];

    eth_t  *eth  = (eth_t  *)frame;
    ipv4_t *ip   = (ipv4_t *)(frame + 14);
    icmp_t *icmp = (icmp_t *)(frame + 34);

    memcpy(eth->dst, dst_mac, 6);
    memcpy(eth->src, my_mac, 6);
    eth->ethertype = ETH_IP;

    ip->ihl_ver    = 0x45;
    ip->dscp       = 0;
    ip->total_len  = htons(total_ip);
    ip->id         = 0;
    ip->flags_frag = 0;
    ip->ttl        = 64;
    ip->proto      = 1;   /* ICMP */
    ip->checksum   = 0;
    memcpy(ip->src, my_ip, 4);
    memcpy(ip->dst, dst_ip, 4);
    ip->checksum = inet_checksum(ip, 20);

    icmp->type     = 0;   /* Echo Reply */
    icmp->code     = 0;
    icmp->checksum = 0;
    icmp->id       = id;
    icmp->seq      = seq;
    if (plen > 32) plen = 32;
    memcpy(icmp->data, payload, plen);
    icmp->checksum = inet_checksum(icmp, (uint32_t)(8 + plen));

    rtl8139_send(frame, frame_len);
}

/* ---- Procesamiento de frames recibidos ---- */

/* Resultado de recibir un ping reply para net_ping() */
static volatile bool ping_reply_got = false;
static volatile uint16_t ping_reply_seq = 0;

static void process_frame(const uint8_t *frame, uint16_t len)
{
    if (len < 14) return;
    const eth_t *eth = (const eth_t *)frame;

    if (eth->ethertype == ETH_ARP && len >= 42) {
        const arp_t *arp = (const arp_t *)(frame + 14);
        /* Aprender el sender */
        arp_insert(arp->spa, arp->sha);
        /* Si es una request dirigida a nosotros, responder */
        if (ntohs(arp->oper) == 1 && memcmp(arp->tpa, my_ip, 4) == 0)
            arp_send_reply(arp->sha, arp->spa);
    }
    else if (eth->ethertype == ETH_IP && len >= 34) {
        const ipv4_t *ip = (const ipv4_t *)(frame + 14);
        if ((ip->ihl_ver & 0xF0) != 0x40) return;  /* solo IPv4 */
        if (memcmp(ip->dst, my_ip, 4) != 0) return;  /* solo para nosotros */

        if (ip->proto == 1 && len >= 34 + 8) {  /* ICMP */
            const icmp_t *icmp = (const icmp_t *)(frame + 34);
            uint16_t icmp_len  = (uint16_t)(ntohs(ip->total_len) - 20);

            if (icmp->type == 8) {  /* Echo Request: responder */
                uint16_t plen = (icmp_len > 8) ? (uint16_t)(icmp_len - 8) : 0;
                arp_insert(ip->src, eth->src);
                icmp_send_reply(eth->src, ip->src, icmp->id, icmp->seq,
                                icmp->data, plen);
            }
            else if (icmp->type == 0) {  /* Echo Reply */
                if (ntohs(icmp->seq) == ping_seq) {
                    ping_reply_seq = ntohs(icmp->seq);
                    ping_reply_got = true;
                }
            }
        }
    }
}

/* ---- API pública ---- */

bool net_init(const uint8_t ip[4], const uint8_t gw[4], const uint8_t mask[4])
{
    if (!rtl8139_present()) return false;

    memcpy(my_ip,   ip,   4);
    memcpy(my_gw,   gw,   4);
    memcpy(my_mask, mask, 4);
    rtl8139_mac(my_mac);
    memset(arp_table, 0, sizeof(arp_table));

    net_ready = true;
    kprintf("  [ OK ] Network: IP %u.%u.%u.%u MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            ip[0], ip[1], ip[2], ip[3],
            my_mac[0], my_mac[1], my_mac[2],
            my_mac[3], my_mac[4], my_mac[5]);
    return true;
}

void net_poll(void)
{
    if (!net_ready) return;
    uint8_t buf[1518];
    int n;
    for (int i = 0; i < 8; i++) {  /* procesar hasta 8 frames por llamada */
        n = rtl8139_recv(buf, sizeof(buf));
        if (n <= 0) break;
        process_frame(buf, (uint16_t)n);
    }
}

bool net_arp_resolve(const uint8_t ip[4], uint8_t mac_out[6], uint32_t timeout_ms)
{
    if (!net_ready) return false;

    arp_entry_t *e = arp_lookup(ip);
    if (e) { memcpy(mac_out, e->mac, 6); return true; }

    arp_send_request(ip);

    uint32_t start = uptime() * 1000;
    while (uptime() * 1000 - start < timeout_ms) {
        net_poll();
        e = arp_lookup(ip);
        if (e) { memcpy(mac_out, e->mac, 6); return true; }
    }
    return false;
}

int net_ping(const uint8_t dst_ip[4], uint32_t timeout_ms)
{
    if (!net_ready) return -1;

    /* Resolver MAC destino (o del gateway si está fuera de la LAN) */
    uint8_t target_ip[4];
    for (int i = 0; i < 4; i++)
        target_ip[i] = (my_ip[i] & my_mask[i]) == (dst_ip[i] & my_mask[i])
                       ? dst_ip[i] : my_gw[i];

    uint8_t dst_mac[6];
    if (!net_arp_resolve(target_ip, dst_mac, 1000)) {
        kprintf("[net] ARP timeout for %u.%u.%u.%u\n",
                target_ip[0], target_ip[1], target_ip[2], target_ip[3]);
        return -1;
    }

    /* Construir y enviar Echo Request */
    ping_seq++;
    ping_reply_got = false;

    uint8_t frame[74];
    eth_t  *eth  = (eth_t  *)frame;
    ipv4_t *ip   = (ipv4_t *)(frame + 14);
    icmp_t *icmp = (icmp_t *)(frame + 34);

    memcpy(eth->dst, dst_mac, 6);
    memcpy(eth->src, my_mac, 6);
    eth->ethertype = ETH_IP;

    ip->ihl_ver    = 0x45;
    ip->dscp       = 0;
    ip->total_len  = htons(20 + 8 + 32);
    ip->id         = htons(ping_seq);
    ip->flags_frag = 0;
    ip->ttl        = 64;
    ip->proto      = 1;
    ip->checksum   = 0;
    memcpy(ip->src, my_ip, 4);
    memcpy(ip->dst, dst_ip, 4);
    ip->checksum = inet_checksum(ip, 20);

    icmp->type     = 8;   /* Echo Request */
    icmp->code     = 0;
    icmp->checksum = 0;
    icmp->id       = htons(0x5472);   /* "Tr" */
    icmp->seq      = htons(ping_seq);
    memset(icmp->data, 0xAB, 32);     /* payload de relleno */
    icmp->checksum = inet_checksum(icmp, 40);

    uint32_t t_start = timer_get_ticks();
    rtl8139_send(frame, 60);

    /* Esperar reply */
    uint32_t hz = timer_get_freq();
    if (!hz) hz = 100;
    uint32_t ticks_max = (timeout_ms * hz) / 1000;

    while ((timer_get_ticks() - t_start) < ticks_max) {
        net_poll();
        if (ping_reply_got) {
            uint32_t elapsed_ticks = timer_get_ticks() - t_start;
            return (int)((elapsed_ticks * 1000) / hz);
        }
    }
    return -1;   /* timeout */
}

void net_get_ip(uint8_t ip[4])  { memcpy(ip, my_ip, 4); }
void net_get_mac(uint8_t mac[6]){ rtl8139_mac(mac); }

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
static bool    net_ready_flag = false;
static uint32_t rx_frames, tx_frames;
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

    { rtl8139_send(frame, 42); tx_frames++; }
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

    { rtl8139_send(frame, 42); tx_frames++; }
}

/* ---- ICMP ---- */

static void icmp_send_reply(const uint8_t dst_mac[6], const uint8_t dst_ip[4],
                            uint16_t id, uint16_t seq,
                            const uint8_t *payload, uint16_t plen)
{
    /* El tamaño del payload lo decide el peer que nos envió el echo request,
     * no nosotros. Hay que recortarlo a lo que cabe en la trama, o un ping
     * con payload grande desborda `frame` (buffer overflow de pila en
     * ring 0 = corrupción del kernel). 1500 - 14 (eth) - 20 (ip) - 8 (icmp)
     * = 1458 bytes de payload como máximo. */
    if (plen > 1458)
        plen = 1458;

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

    { rtl8139_send(frame, frame_len); tx_frames++; }
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
            /* ip->total_len viene de la red: no se puede restar 20 a ciegas
             * (si llega < 20 el uint16_t se desborda y luego se copia
             * basura hasta 65535 bytes). Acotar al frame realmente
             * recibido, que es lo único que está en memoria. */
            uint16_t ip_total = ntohs(ip->total_len);
            if (ip_total < 28 || (uint32_t)ip_total + 14 > len)
                return;
            uint16_t icmp_len  = (uint16_t)(ip_total - 20);

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

    net_ready_flag = true;
    kprintf("  [ OK ] Network: IP %u.%u.%u.%u MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            ip[0], ip[1], ip[2], ip[3],
            my_mac[0], my_mac[1], my_mac[2],
            my_mac[3], my_mac[4], my_mac[5]);
    return true;
}

void net_poll(void)
{
    if (!net_ready_flag) return;
    uint8_t buf[1518];
    int n;
    for (int i = 0; i < 8; i++) {  /* procesar hasta 8 frames por llamada */
        n = rtl8139_recv(buf, sizeof(buf));
        if (n <= 0) break;
        rx_frames++;
        process_frame(buf, (uint16_t)n);
    }
}

bool net_arp_resolve(const uint8_t ip[4], uint8_t mac_out[6], uint32_t timeout_ms)
{
    if (!net_ready_flag) return false;

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
    if (!net_ready_flag) return -1;

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

    /* 14 (eth) + 20 (ip) + 8 (icmp hdr) + 32 (icmp->data) = 74 bytes.
     * Antes era `uint8_t frame[64]`: struct icmp_t es packed y mide 40
     * (8 + 32 de data), así que a partir del byte 64 se escribía fuera del
     * array (stack smash silencioso que GCC avisaba con -Warray-bounds). */
    uint8_t frame[14 + 20 + sizeof(icmp_t)];
    eth_t  *eth  = (eth_t  *)frame;
    ipv4_t *ip   = (ipv4_t *)(frame + 14);
    icmp_t *icmp = (icmp_t *)(frame + 34);

    memcpy(eth->dst, dst_mac, 6);
    memcpy(eth->src, my_mac, 6);
    eth->ethertype = ETH_IP;

    ip->ihl_ver    = 0x45;
    ip->dscp       = 0;
    ip->total_len  = htons(20 + sizeof(icmp_t));
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
    memset(icmp->data, 0xAB, sizeof(icmp->data));   /* payload de relleno */
    icmp->checksum = inet_checksum(icmp, sizeof(icmp_t));

    uint32_t t_start = timer_get_ticks();
    { rtl8139_send(frame, (uint16_t)sizeof(frame)); tx_frames++; }

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

bool net_ready(void) { return net_ready_flag; }
uint32_t net_rx_frames(void) { return rx_frames; }
uint32_t net_tx_frames(void) { return tx_frames; }

/* "a.b.c.d" -> out[4]; estricto (0-255, exactamente 4 octetos). */
bool parse_ipv4(const char *str, uint8_t out[4])
{
    if (!str || !out) return false;
    const char *c = str;
    for (int n = 0; n < 4; n++) {
        if (*c < '0' || *c > '9') return false;
        int val = 0, digits = 0;
        while (*c >= '0' && *c <= '9') {
            val = val * 10 + (*c - '0');
            if (++digits > 3 || val > 255) return false;
            c++;
        }
        out[n] = (uint8_t)val;
        if (n < 3) { if (*c != '.') return false; c++; }
    }
    return *c == '\0';
}

bool net_status(uint8_t ip[4], uint8_t gw[4], uint8_t mask[4], uint8_t mac[6])
{
    if (!net_ready_flag) return false;
    if (ip)   memcpy(ip,   my_ip,   4);
    if (gw)   memcpy(gw,   my_gw,   4);
    if (mask) memcpy(mask, my_mask, 4);
    if (mac)  rtl8139_mac(mac);
    return true;
}

/* ---- parser de cmdline: busca "clave=a.b.c.d" ---- */
static bool parse_ip_arg(const char *cmdline, const char *key, uint8_t out[4])
{
    if (!cmdline || !key) return false;
    int klen = 0;
    while (key[klen]) klen++;

    for (const char *p = cmdline; *p; p++) {
        /* ¿empieza aquí la clave seguida de '=' ? */
        int i = 0;
        while (i < klen && p[i] && p[i] == key[i]) i++;
        if (i != klen || p[i] != '=') continue;

        const char *v = p + klen + 1;
        int octet[4] = {0, 0, 0, 0};
        int n = 0;
        const char *c = v;
        while (n < 4) {
            if (*c < '0' || *c > '9') return false;
            int val = 0;
            while (*c >= '0' && *c <= '9') { val = val * 10 + (*c - '0'); c++; }
            if (val > 255) return false;
            octet[n++] = val;
            if (n < 4) {
                if (*c != '.') return false;
                c++;
            }
        }
        if (*c && *c != ' ') return false;   /* basura detrás */
        for (int j = 0; j < 4; j++) out[j] = (uint8_t)octet[j];
        return true;
    }
    return false;
}

bool net_init_from_cli(const char *cmdline)
{
    uint8_t ip[4]   = {10, 0, 2, 15};        /* QEMU user-mode networking */
    uint8_t gw[4]   = {10, 0, 2, 2};
    uint8_t mask[4] = {255, 255, 255, 0};

    parse_ip_arg(cmdline, "trinux.ip",   ip);
    parse_ip_arg(cmdline, "trinux.gw",   gw);
    parse_ip_arg(cmdline, "trinux.mask", mask);

    if (!rtl8139_init()) {
        kprintf("[net] no RTL8139 NIC found (no networking this session)\n");
        return false;
    }
    return net_init(ip, gw, mask);
}

void net_get_ip(uint8_t ip[4])  { memcpy(ip, my_ip, 4); }
void net_get_mac(uint8_t mac[6]){ rtl8139_mac(mac); }

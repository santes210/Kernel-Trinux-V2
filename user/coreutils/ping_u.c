/* ping_u.c — /bin/ping en ring 3.
 *
 * El stack de red (ARP + IPv4 + ICMP) vive en el kernel (drivers/net.c) y
 * hasta ahora solo era alcanzable desde el builtin de la shell. Este binario
 * lo expone como un programa normal de /bin, a través de SYS_NET_PING.
 *
 * Uso:  ping <a.b.c.d> [-c count]
 */
#include "../trinux.h"

static int parse_ipv4_u(const char *s, unsigned char out[4])
{
    const char *c = s;
    for (int n = 0; n < 4; n++) {
        if (*c < '0' || *c > '9') return 0;
        int v = 0, d = 0;
        while (*c >= '0' && *c <= '9') {
            v = v * 10 + (*c - '0');
            if (++d > 3 || v > 255) return 0;
            c++;
        }
        out[n] = (unsigned char)v;
        if (n < 3) { if (*c != '.') return 0; c++; }
    }
    return *c == '\0';
}

static void put_uint(unsigned int v) { print_unum(v); }

static int ai(const char *s)
{
    int n = 0, sg = 1;
    if (*s == '-') { sg = -1; s++; }
    while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); s++; }
    return sg * n;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        print("uso: ping <a.b.c.d> [-c count]\n");
        return 1;
    }

    unsigned char ip[4];
    if (!parse_ipv4_u(argv[1], ip)) {
        print("ping: IP no valida: ");
        print(argv[1]);
        putchar_('\n');
        return 1;
    }

    int count = 4;
    for (int i = 2; i + 1 < argc; i++)
        if (argv[i][0] == '-' && argv[i][1] == 'c')
            count = ai(argv[i + 1]);
    if (count <= 0) count = 4;
    if (count > 20) count = 20;   /* no bloquear la shell para siempre */

    print("PING ");
    print(argv[1]);
    print(": 56 bytes de datos ICMP\n");

    int sent = 0, recv = 0;
    unsigned int best = 0, worst = 0, sum = 0;
    for (int i = 0; i < count; i++) {
        sent++;
        int rtt = ping_(ip, 1000);
        if (rtt >= 0) {
            recv++;
            unsigned int r = (unsigned int)rtt;
            sum += r;
            if (best == 0 || r < best) best = r;
            if (r > worst) worst = r;
            print("  reply de ");
            print(argv[1]);
            print(": seq=");
            put_uint((unsigned int)(i + 1));
            print(" ttl=64 tiempo=");
            put_uint(r);
            print(" ms\n");
        } else {
            print("  timeout: seq=");
            put_uint((unsigned int)(i + 1));
            putchar_('\n');
        }
    }

    print("--- estadisticas ---\n");
    put_uint((unsigned int)sent); print(" enviados, ");
    put_uint((unsigned int)recv); print(" recibidos\n");
    if (recv) {
        print("rtt min/avg/max = ");
        put_uint(best); putchar_('/');
        put_uint(sum / (unsigned int)recv); putchar_('/');
        put_uint(worst); print(" ms\n");
    }
    return recv ? 0 : 1;
}

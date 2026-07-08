/* lib/sha256.c — SHA-256 (FIPS 180-4), freestanding.
 *
 * Implementación completa de SHA-256 sin dependencias de libc.
 * Compatible con el estándar FIPS 180-4.
 *
 * Cambio #1 de seguridad: reemplaza contraseñas en texto plano en /etc/shadow
 * con hashes SHA-256 de 64 caracteres hex.
 */
#include "sha256.h"
#include "string.h"

/* ---- Constantes SHA-256 (primeras 32 bits de las partes fraccionarias
 *      de las raíces cúbicas de los primeros 64 números primos) ---- */
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

/* Valores iniciales del hash (primeras 32 bits de las partes fraccionarias
 * de las raíces cuadradas de los primeros 8 números primos). */
static const uint32_t H0[8] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
};

/* ---- Macros de rotación y funciones SHA-256 ---- */
#define ROTR(x, n)  (((x) >> (n)) | ((x) << (32 - (n))))
#define SHR(x, n)   ((x) >> (n))

#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))

#define S0(x)  (ROTR(x,  2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define S1(x)  (ROTR(x,  6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define s0(x)  (ROTR(x,  7) ^ ROTR(x, 18) ^ SHR(x,   3))
#define s1(x)  (ROTR(x, 17) ^ ROTR(x, 19) ^ SHR(x,  10))

/* Convierte 4 bytes big-endian a uint32_t. */
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

/* Escribe uint32_t como 4 bytes big-endian. */
static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >>  8);
    p[3] = (uint8_t)(v);
}

/* Escribe uint64_t como 8 bytes big-endian. */
static void put_be64(uint8_t *p, uint64_t v)
{
    put_be32(p,     (uint32_t)(v >> 32));
    put_be32(p + 4, (uint32_t)(v));
}

/* Procesa un bloque de 512 bits (64 bytes) y actualiza el estado h[]. */
static void sha256_block(uint32_t h[8], const uint8_t block[64])
{
    uint32_t w[64];
    int i;

    /* Preparar el message schedule */
    for (i = 0; i < 16; i++)
        w[i] = be32(block + i * 4);
    for (i = 16; i < 64; i++)
        w[i] = s1(w[i-2]) + w[i-7] + s0(w[i-15]) + w[i-16];

    /* Inicializar variables de trabajo */
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];

    /* 64 rondas de compresión */
    for (i = 0; i < 64; i++) {
        uint32_t t1 = hh + S1(e) + CH(e,f,g) + K[i] + w[i];
        uint32_t t2 = S0(a) + MAJ(a,b,c);
        hh = g; g = f; f = e; e = d + t1;
        d  = c; c = b; b = a; a = t1 + t2;
    }

    /* Sumar al hash actual */
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

/* ---- API pública ---- */

void sha256(const uint8_t *data, uint32_t len, uint8_t out[32])
{
    uint32_t h[8];
    uint8_t  block[64];
    uint32_t i;

    /* Copiar valores iniciales */
    for (i = 0; i < 8; i++)
        h[i] = H0[i];

    /* Procesar bloques completos de 64 bytes */
    uint32_t done = 0;
    while (done + 64 <= len) {
        sha256_block(h, data + done);
        done += 64;
    }

    /* Padding: copia el resto, agrega bit '1', rellena con ceros,
     * y agrega la longitud en bits como uint64_t big-endian. */
    uint32_t rest = len - done;
    memset(block, 0, 64);
    memcpy(block, data + done, rest);
    block[rest] = 0x80;  /* bit '1' después del mensaje */

    if (rest < 56) {
        /* Cabe el length en este mismo bloque */
        put_be64(block + 56, (uint64_t)len * 8);
        sha256_block(h, block);
    } else {
        /* Necesita un bloque extra de padding */
        sha256_block(h, block);
        memset(block, 0, 64);
        put_be64(block + 56, (uint64_t)len * 8);
        sha256_block(h, block);
    }

    /* Serializar los 8 words de 32 bits como 32 bytes big-endian */
    for (i = 0; i < 8; i++)
        put_be32(out + i * 4, h[i]);
}

void sha256_hex(const uint8_t *data, uint32_t len, char hex[65])
{
    uint8_t raw[32];
    sha256(data, len, raw);

    static const char nibble[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        hex[i * 2]     = nibble[raw[i] >> 4];
        hex[i * 2 + 1] = nibble[raw[i] & 0xF];
    }
    hex[64] = '\0';
}

bool sha256_verify(const char *password, const char *stored_hex)
{
    if (!password || !stored_hex)
        return false;

    /* Si el hash almacenado tiene exactamente 64 chars hex, es SHA-256.
     * Si es más corto, asumimos contraseña legacy en texto plano. */
    uint32_t hlen = 0;
    while (stored_hex[hlen]) hlen++;

    if (hlen != 64) {
        /* Legacy: comparación directa (para migración gradual) */
        uint32_t plen = 0;
        while (password[plen]) plen++;
        if (plen != hlen) return false;
        for (uint32_t i = 0; i < plen; i++)
            if (password[i] != stored_hex[i]) return false;
        return true;
    }

    /* Hash SHA-256 de la contraseña proporcionada */
    uint32_t plen = 0;
    while (password[plen]) plen++;

    char computed[65];
    sha256_hex((const uint8_t *)password, plen, computed);

    /* Comparación en tiempo constante para evitar timing attacks */
    uint8_t diff = 0;
    for (int i = 0; i < 64; i++)
        diff |= (uint8_t)(computed[i] ^ stored_hex[i]);
    return diff == 0;
}

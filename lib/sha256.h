/* lib/sha256.h — SHA-256 implementation (freestanding, no libc).
 *
 * Implementación pura de SHA-256 que no depende de ninguna librería externa.
 * Usada por auth/users.c para almacenar contraseñas hasheadas en /etc/shadow.
 *
 * API:
 *   sha256(data, len, out32)  — calcula el hash de `len` bytes en `data`,
 *                               escribe 32 bytes binarios en `out32`.
 *   sha256_hex(data, len, out65) — igual pero produce 64 chars hex + '\0'.
 */
#ifndef LIB_SHA256_H
#define LIB_SHA256_H

#include "types.h"

/* Calcula SHA-256 de `len` bytes en `data`.
 * `out` debe apuntar a un buffer de al menos 32 bytes. */
void sha256(const uint8_t *data, uint32_t len, uint8_t out[32]);

/* Igual que sha256() pero el resultado se convierte a string hex en minúsculas.
 * `hex` debe tener al menos 65 bytes (64 hex + '\0'). */
void sha256_hex(const uint8_t *data, uint32_t len, char hex[65]);

/* Verifica si una contraseña en texto plano coincide con su hash hex. */
bool sha256_verify(const char *password, const char *stored_hex);

#endif /* LIB_SHA256_H */

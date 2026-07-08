/* fs/pipe.h — Pipes en memoria con ring buffer.
 *
 * CAMBIO #3: reemplaza los pipes basados en archivos temporales /tmp/.p0
 * por pipes reales en RAM usando un ring buffer circular.
 *
 * Diferencias vs el sistema anterior:
 *   - No toca el disco: todo en kheap.
 *   - El escritor puede bloquear si el buffer está lleno.
 *   - El lector recibe EOF cuando el extremo de escritura está cerrado.
 *   - Soporta hasta PIPE_MAX_OPEN pipes abiertos simultáneamente.
 *   - Compatible con SYS_FILE_READ / SYS_FILE_WRITE a través de fd.
 */
#ifndef FS_PIPE_H
#define FS_PIPE_H

#include "../lib/types.h"

#define PIPE_BUF_SIZE   4096   /* bytes por pipe (potencia de 2) */
#define PIPE_MAX_OPEN   16     /* máximo de pipes simultáneos */

typedef struct {
    uint8_t  buf[PIPE_BUF_SIZE];
    uint32_t head;          /* índice de lectura */
    uint32_t tail;          /* índice de escritura */
    uint32_t used;          /* bytes disponibles para leer */
    bool     write_closed;  /* el extremo de escritura fue cerrado */
    bool     read_closed;   /* el extremo de lectura fue cerrado */
    int      refcount;      /* cuántos fds apuntan aquí */
} pipe_t;

/* Tipos de fd para distinguir pipes de archivos VFS. */
#define FD_TYPE_VFS    0
#define FD_TYPE_PIPE_R 1    /* extremo de lectura */
#define FD_TYPE_PIPE_W 2    /* extremo de escritura */

/* Crea un par de file descriptors para un pipe nuevo.
 * fds[0] = extremo de lectura, fds[1] = extremo de escritura.
 * Devuelve 0 si OK, -1 si no hay recursos. */
int  pipe_create(int fds[2]);

/* Lee hasta `len` bytes del pipe fd. Bloquea si no hay datos y el
 * escritor sigue abierto. Devuelve 0 (EOF) si escritor cerró y no hay datos. */
int  pipe_read(int fd, void *buf, uint32_t len);

/* Escribe `len` bytes al pipe fd. Devuelve bytes escritos o -1 si el
 * extremo de lectura ya fue cerrado (SIGPIPE equivalente). */
int  pipe_write(int fd, const void *buf, uint32_t len);

/* Cierra un extremo del pipe. Libera el pipe_t si refcount llega a 0. */
void pipe_close(int fd);

/* Devuelve true si `fd` es un pipe (lectura o escritura). */
bool pipe_is_pipe(int fd);

/* Devuelve el tipo de fd (FD_TYPE_*). */
int  pipe_fd_type(int fd);

/* Inicializa la tabla de pipes (llamado desde syscall_install). */
void pipe_init(void);

#endif /* FS_PIPE_H */

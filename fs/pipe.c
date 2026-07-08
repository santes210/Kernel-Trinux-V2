/* fs/pipe.c — Pipes en memoria con ring buffer circular.
 *
 * CAMBIO #3: pipes reales en RAM, sin archivos temporales en disco.
 *
 * Diseño:
 *   - Tabla global de PIPE_MAX_OPEN entradas (pipe_t), cada una con un
 *     ring buffer de PIPE_BUF_SIZE bytes.
 *   - Tabla de file descriptors de pipe, separada de los kfds de VFS.
 *     Los números de fd de pipe empiezan en PIPE_FD_BASE para no colisionar.
 *   - El ring buffer usa head/tail/used: escritura avanza tail, lectura
 *     avanza head, ambos modulo PIPE_BUF_SIZE.
 *   - pipe_read() espera con yield si no hay datos y el escritor sigue activo.
 *   - pipe_write() hace yield si el buffer está lleno (backpressure).
 */
#include "pipe.h"
#include "../mm/kheap.h"
#include "../lib/string.h"
#include "../process/scheduler.h"

/* Offset de fd para pipes — así fd=100 significa "pipe slot 0, lectura",
 * fd=101 "pipe slot 0, escritura", fd=102 "pipe slot 1, lectura", etc. */
#define PIPE_FD_BASE   100
#define PIPE_FD_STRIDE 2   /* R=base+slot*2, W=base+slot*2+1 */

static pipe_t pipes[PIPE_MAX_OPEN];
static bool   pipe_used[PIPE_MAX_OPEN];

void pipe_init(void)
{
    for (int i = 0; i < PIPE_MAX_OPEN; i++) {
        pipe_used[i] = false;
        memset(&pipes[i], 0, sizeof(pipe_t));
    }
}

/* Convierte un fd de pipe a índice en la tabla. */
static int fd_to_slot(int fd)
{
    if (fd < PIPE_FD_BASE) return -1;
    return (fd - PIPE_FD_BASE) / PIPE_FD_STRIDE;
}

bool pipe_is_pipe(int fd)
{
    if (fd < PIPE_FD_BASE) return false;
    int slot = fd_to_slot(fd);
    if (slot < 0 || slot >= PIPE_MAX_OPEN) return false;
    return pipe_used[slot];
}

int pipe_fd_type(int fd)
{
    if (!pipe_is_pipe(fd)) return FD_TYPE_VFS;
    int offset = (fd - PIPE_FD_BASE) % PIPE_FD_STRIDE;
    return (offset == 0) ? FD_TYPE_PIPE_R : FD_TYPE_PIPE_W;
}

int pipe_create(int fds[2])
{
    /* Buscar un slot libre */
    int slot = -1;
    for (int i = 0; i < PIPE_MAX_OPEN; i++) {
        if (!pipe_used[i]) { slot = i; break; }
    }
    if (slot < 0) return -1;

    pipe_t *p = &pipes[slot];
    memset(p, 0, sizeof(pipe_t));
    p->head         = 0;
    p->tail         = 0;
    p->used         = 0;
    p->write_closed = false;
    p->read_closed  = false;
    p->refcount     = 2;   /* un extremo lector + uno escritor */
    pipe_used[slot] = true;

    fds[0] = PIPE_FD_BASE + slot * PIPE_FD_STRIDE;       /* lectura */
    fds[1] = PIPE_FD_BASE + slot * PIPE_FD_STRIDE + 1;   /* escritura */
    return 0;
}

int pipe_read(int fd, void *buf, uint32_t len)
{
    int slot = fd_to_slot(fd);
    if (slot < 0 || !pipe_used[slot]) return -1;
    if (pipe_fd_type(fd) != FD_TYPE_PIPE_R) return -1;

    pipe_t *p = &pipes[slot];
    uint8_t *dst = (uint8_t *)buf;
    uint32_t got = 0;

    while (got < len) {
        if (p->used == 0) {
            /* Sin datos: si el escritor cerró, devolvemos EOF */
            if (p->write_closed) break;
            /* Si no, yield y esperar */
            schedule();
            continue;
        }

        /* Leer byte a byte del ring buffer */
        uint32_t chunk = p->used;
        if (chunk > len - got) chunk = len - got;

        for (uint32_t i = 0; i < chunk; i++) {
            dst[got++] = p->buf[p->head];
            p->head = (p->head + 1) & (PIPE_BUF_SIZE - 1);
            p->used--;
        }
    }
    return (int)got;
}

int pipe_write(int fd, const void *buf, uint32_t len)
{
    int slot = fd_to_slot(fd);
    if (slot < 0 || !pipe_used[slot]) return -1;
    if (pipe_fd_type(fd) != FD_TYPE_PIPE_W) return -1;

    pipe_t *p = &pipes[slot];
    if (p->read_closed) return -1;   /* SIGPIPE equivalente */

    const uint8_t *src = (const uint8_t *)buf;
    uint32_t sent = 0;

    while (sent < len) {
        /* Esperar si el buffer está lleno */
        while (p->used == PIPE_BUF_SIZE) {
            if (p->read_closed) return (int)sent;
            schedule();
        }

        /* Escribir bytes disponibles */
        uint32_t space = PIPE_BUF_SIZE - p->used;
        uint32_t chunk = len - sent;
        if (chunk > space) chunk = space;

        for (uint32_t i = 0; i < chunk; i++) {
            p->buf[p->tail] = src[sent++];
            p->tail = (p->tail + 1) & (PIPE_BUF_SIZE - 1);
            p->used++;
        }
    }
    return (int)sent;
}

void pipe_close(int fd)
{
    int slot = fd_to_slot(fd);
    if (slot < 0 || !pipe_used[slot]) return;

    pipe_t *p = &pipes[slot];
    int type = pipe_fd_type(fd);

    if (type == FD_TYPE_PIPE_R)
        p->read_closed = true;
    else
        p->write_closed = true;

    p->refcount--;
    if (p->refcount <= 0) {
        /* Ambos extremos cerrados: liberar el slot */
        memset(p, 0, sizeof(pipe_t));
        pipe_used[slot] = false;
    }
}

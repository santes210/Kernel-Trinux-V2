/* fs/path.c  -  path resolution and manipulation. */
#include "path.h"
#include "vfs.h"        /* VFS_NAME_MAX: tope de un componente */
#include "../lib/string.h"
#include "../lib/printf.h"   /* kprintf: aviso de ruta truncada */

bool path_is_absolute(const char *path)
{
    return path && path[0] == '/';
}

/* Split a path into components and rebuild canonical absolute path.
 *
 * El resultado se trunca a PATH_MAX-1 (la firma no lleva tamaño, pero TODOS
 * los llamantes usan char[PATH_MAX] y antes se desbordaban).
 *
 * OJO con la concatenación: el rebobinado de ".." puede saltarse componentes
 * y los punteros de `comps[]` apuntan al mismo `work` que se está
 * reescribiendo, así que no se puede hacer `strcat(out, ...)` con `out` como
 * destino incremental — la versión anterior lo hacía y, al haber usado
 * strcpy/strcat sin límite, un path profundo escribía fuera de `out`.
 * Aquí se reconstruye en un buffer propio y se copia acotado. */
void path_normalize(const char *base, const char *path, char *out)
{
    char work[PATH_MAX * 2];

    if (!out)
        return;
    if (!path)
        path = "/";

    /* Build the combined raw path. */
    if (path_is_absolute(path)) {
        strncpy(work, path, sizeof(work) - 1);
        work[sizeof(work) - 1] = '\0';
    } else {
        strncpy(work, base ? base : "/", sizeof(work) - 1);
        work[sizeof(work) - 1] = '\0';
        size_t len = strlen(work);
        if (len == 0 || work[len - 1] != '/') {
            if (len < sizeof(work) - 1) {
                work[len++] = '/';
                work[len] = '\0';
            }
        }
        strncat(work, path, sizeof(work) - strlen(work) - 1);
    }

    /* Stack of component pointers. */
    char *comps[64];
    int   ncomp = 0;

    char *p = work;
    while (*p) {
        /* skip slashes */
        while (*p == '/')
            p++;
        if (!*p)
            break;
        char *start = p;
        while (*p && *p != '/')
            p++;
        if (*p) {
            *p = '\0';
            p++;
        }

        if (strcmp(start, ".") == 0) {
            /* skip */
        } else if (strcmp(start, "..") == 0) {
            if (ncomp > 0)
                ncomp--;
        } else {
            if (ncomp < 64)
                comps[ncomp++] = start;
        }
    }

    /* Rebuild, acotando a PATH_MAX. */
    if (ncomp == 0) {
        out[0] = '/';
        out[1] = '\0';
        return;
    }
    char build[PATH_MAX];
    uint32_t blen = 0;
    bool truncated = false;
    for (int i = 0; i < ncomp; i++) {
        uint32_t clen = (uint32_t)strlen(comps[i]);
        if (clen > VFS_NAME_MAX)
            clen = VFS_NAME_MAX;      /* ningún componente real es tan largo */
        /* +1 por la '/' y +1 por el NUL final */
        if (blen + 1 + clen >= sizeof(build)) {
            /* Recortar el propio componente antes que perder la ruta entera. */
            uint32_t room = (blen + 1 < sizeof(build))
                          ? (uint32_t)(sizeof(build) - 1 - blen - 1) : 0;
            if (room == 0) {
                truncated = true;
                break;
            }
            clen = room;
            truncated = true;
        }
        build[blen++] = '/';
        memcpy(build + blen, comps[i], clen);
        blen += clen;
        if (truncated)
            break;
    }
    if (truncated)
        kprintf("[path] warning: ruta truncada a %u bytes\n", (unsigned)blen);
    if (blen == 0) {
        out[0] = '/';
        out[1] = '\0';
        return;
    }
    build[blen] = '\0';
    memcpy(out, build, blen + 1);
}

/* Une dos componentes en `out` (tamaño PATH_MAX, como todo el resto del VFS).
 * Acotado: antes era strcpy+strcat sin límite. */
void path_join(const char *a, const char *b, char *out)
{
    if (!out)
        return;
    size_t blen = 0;

    if (a && *a) {
        size_t la = strlen(a);
        if (la >= PATH_MAX)
            la = PATH_MAX - 1;
        memcpy(out, a, la);
        blen = la;
    }
    /* Barra separadora solo si hace falta. */
    if (blen == 0 || out[blen - 1] != '/') {
        if (blen + 1 < PATH_MAX)
            out[blen++] = '/';
    }
    if (b && *b) {
        if (b[0] == '/')
            b++;
        size_t room = PATH_MAX - 1 - blen;
        size_t lb = strlen(b);
        if (lb > room)
            lb = room;
        memcpy(out + blen, b, lb);
        blen += lb;
    }
    out[blen] = '\0';
}

void path_basename(const char *path, char *out)
{
    if (!out)
        return;
    if (!path) {
        out[0] = '\0';
        return;
    }
    const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;
    if (*name == '\0') {
        /* "/" o "algo/": el basename es "/" */
        out[0] = '/';
        out[1] = '\0';
        return;
    }
    size_t n = strlen(name);
    if (n >= PATH_MAX)
        n = PATH_MAX - 1;
    memcpy(out, name, n);
    out[n] = '\0';
}

void path_dirname(const char *path, char *out)
{
    if (!out)
        return;
    if (!path || !*path) {
        out[0] = '.';
        out[1] = '\0';
        return;
    }
    size_t n = strlen(path);
    if (n >= PATH_MAX)
        n = PATH_MAX - 1;
    memcpy(out, path, n);
    out[n] = '\0';

    char *slash = strrchr(out, '/');
    if (!slash) {
        out[0] = '.';
        out[1] = '\0';
        return;
    }
    if (slash == out) {
        out[1] = '\0';   /* keep root "/" */
        return;
    }
    *slash = '\0';
}

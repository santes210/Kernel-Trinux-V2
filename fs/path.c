/* fs/path.c  -  path resolution and manipulation. */
#include "path.h"
#include "../lib/string.h"

bool path_is_absolute(const char *path)
{
    return path && path[0] == '/';
}

/* Split a path into components and rebuild canonical absolute path. */
void path_normalize(const char *base, const char *path, char *out)
{
    char work[PATH_MAX * 2];

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

    /* Rebuild. */
    if (ncomp == 0) {
        strcpy(out, "/");
        return;
    }
    out[0] = '\0';
    for (int i = 0; i < ncomp; i++) {
        strcat(out, "/");
        strcat(out, comps[i]);
    }
}

void path_join(const char *a, const char *b, char *out)
{
    size_t la = strlen(a);
    strcpy(out, a);
    if (la == 0 || out[la - 1] != '/')
        strcat(out, "/");
    /* skip leading slash of b */
    if (b[0] == '/')
        b++;
    strcat(out, b);
}

void path_basename(const char *path, char *out)
{
    const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;
    if (*name == '\0' && slash && slash != path) {
        /* trailing slash; back up */
        strcpy(out, "/");
        return;
    }
    if (*name == '\0')
        strcpy(out, "/");
    else
        strcpy(out, name);
}

void path_dirname(const char *path, char *out)
{
    strcpy(out, path);
    char *slash = strrchr(out, '/');
    if (!slash) {
        strcpy(out, ".");
        return;
    }
    if (slash == out) {
        out[1] = '\0';   /* keep root "/" */
        return;
    }
    *slash = '\0';
}

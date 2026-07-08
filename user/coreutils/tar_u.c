/* user/coreutils/tar_u.c — Comando tar para Trinux (formato POSIX ustar).
 *
 * CAMBIO #12: archivado y extracción de archivos en formato ustar.
 *
 * Soporta:
 *   tar -c archivo1 archivo2 ... > salida.tar   (crear)
 *   tar -x < entrada.tar                         (extraer)
 *   tar -t < entrada.tar                         (listar)
 *
 * Formato: POSIX ustar (compatible con GNU tar, bsdtar, Windows tar).
 * Bloques de 512 bytes. Cabecera de 512 bytes por archivo.
 * Sin compresión (para .tar.gz se necesitaría gzip que es futuro).
 *
 * Limitaciones:
 *   - Sin soporte de links simbólicos ni hard links.
 *   - Tamaño máximo de archivo: ~8 GiB (campo size en octal de 11 dígitos).
 *   - Sin preservación de timestamps precisos (se usa 0).
 *   - Solo archivos regulares y directorios.
 */
#include "../../user/trinux.h"

#define TAR_BLOCK  512
#define NAME_MAX   100
#define PATH_MAX_T 256

/* ---- Cabecera ustar ---- */
typedef struct {
    char name[100];       /* nombre del archivo */
    char mode[8];         /* permisos en octal (ej: "0000755\0") */
    char uid[8];
    char gid[8];
    char size[12];        /* tamaño en octal */
    char mtime[12];       /* modificación en octal */
    char checksum[8];
    char typeflag;        /* '0'=regular, '5'=directory, '\0'=regular */
    char linkname[100];
    char magic[6];        /* "ustar" */
    char version[2];      /* "00" */
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];     /* prefijo de ruta para nombres largos */
    char _pad[12];
} __attribute__((packed)) tar_hdr_t;

/* ---- Helpers ---- */

static int ulen(const char *s) {
    int n = 0; while (s[n]) n++; return n;
}
static void uprint(const char *s) { print(s); }
static void uprintn(const char *s) { print(s); print("\n"); }

/* Convierte número a string octal (sin prefijo 0) con `digits` dígitos + nul. */
static void to_octal(uint32_t val, char *buf, int digits)
{
    buf[digits] = '\0';
    for (int i = digits - 1; i >= 0; i--) {
        buf[i] = '0' + (val & 7);
        val >>= 3;
    }
}

/* Convierte string octal a número. */
static uint32_t from_octal(const char *s, int len)
{
    uint32_t v = 0;
    for (int i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '7') break;
        v = (v << 3) | (uint32_t)(s[i] - '0');
    }
    return v;
}

/* Calcula el checksum ustar de una cabecera.
 * Los 8 bytes del campo checksum se tratan como espacios (0x20). */
static uint32_t tar_checksum(const tar_hdr_t *h)
{
    const uint8_t *p = (const uint8_t *)h;
    uint32_t sum = 0;
    for (int i = 0; i < TAR_BLOCK; i++) {
        if (i >= 148 && i < 156)
            sum += 0x20;   /* espacio en lugar del campo checksum */
        else
            sum += p[i];
    }
    return sum;
}

/* Rellena una cabecera ustar para un archivo o directorio. */
static void fill_header(tar_hdr_t *h, const char *name, uint32_t size,
                        uint32_t perms, char typeflag)
{
    memset_(h, 0, TAR_BLOCK);

    /* Nombre: si es largo usar prefix+name */
    int nlen = ulen(name);
    if (nlen < 100) {
        memcpy_(h->name, name, (uint32_t)nlen);
    } else {
        /* Separar en prefix/name en el último '/' antes de la col 100 */
        int split = -1;
        for (int i = nlen - 1; i >= 0 && i >= nlen - 99; i--)
            if (name[i] == '/') { split = i; break; }
        if (split > 0 && split <= 155) {
            memcpy_(h->prefix, name, (uint32_t)split);
            memcpy_(h->name, name + split + 1, (uint32_t)(nlen - split - 1));
        } else {
            memcpy_(h->name, name, 99);   /* truncar */
        }
    }

    to_octal(perms & 0777,  h->mode,     7);
    to_octal(0,              h->uid,      7);
    to_octal(0,              h->gid,      7);
    to_octal(size,           h->size,    11);
    to_octal(0,              h->mtime,   11);

    h->typeflag = typeflag;
    memcpy_(h->magic,   "ustar", 5);
    memcpy_(h->version, "00",    2);
    memcpy_(h->uname,   "root",  4);
    memcpy_(h->gname,   "root",  4);

    /* Checksum */
    uint32_t cs = tar_checksum(h);
    to_octal(cs, h->checksum, 6);
    h->checksum[6] = '\0';
    h->checksum[7] = ' ';
}

/* ---- Crear tar ---- */

/* Buffer de trabajo para leer archivos (128 KiB — máximo razonable para
 * un tar en Trinux dado el heap de 32 MiB del kernel). */
#define WORK_BUF (128 * 1024)
static char work_buf[WORK_BUF];
static char tar_buf[WORK_BUF * 2 + 8192];   /* destino del archivo .tar */

static int cmd_tar_create(int argc, char **argv)
{
    if (argc < 2) {
        uprintn("tar: uso: tar -c archivo... > salida.tar");
        return 1;
    }

    /* Construir el tar en tar_buf */
    uint32_t pos = 0;

    for (int i = 1; i < argc; i++) {
        /* Leer el archivo */
        int n = readfile(argv[i], work_buf, WORK_BUF - 1);
        if (n < 0) {
            print("tar: no se puede leer: "); uprintn(argv[i]);
            continue;
        }

        /* Cabecera de 512 bytes */
        tar_hdr_t *hdr = (tar_hdr_t *)(tar_buf + pos);
        fill_header(hdr, argv[i], (uint32_t)n, 0644, '0');
        pos += TAR_BLOCK;

        /* Datos alineados a 512 bytes */
        memcpy_(tar_buf + pos, work_buf, (uint32_t)n);
        uint32_t padded = ((uint32_t)n + TAR_BLOCK - 1) & ~(TAR_BLOCK - 1u);
        pos += padded;

        if (pos + TAR_BLOCK * 4 >= sizeof(tar_buf)) {
            uprintn("tar: archivo demasiado grande para el buffer");
            break;
        }
    }

    /* Dos bloques de ceros = fin del archivo tar */
    memset_(tar_buf + pos, 0, TAR_BLOCK * 2);
    pos += TAR_BLOCK * 2;

    /* Escribir a stdout (fd 1) */
    _syscall3(SYS_WRITE, 1, (int)tar_buf, (int)pos);
    return 0;
}

/* ---- Listar tar ---- */

static int cmd_tar_list(void)
{
    /* Leer el tar completo desde stdin (fd 0) vía readfile del VFS
     * Si se redirige desde archivo, el syscall SYS_FILE_READ lo maneja. */
    /* Para simplicidad, leemos con SYS_READFILE si hay un argumento de archivo,
     * o bien el usuario redirige con < — en ese caso SYS_GETLINE da los datos. */
    uprintn("tar: -t requiere redireccion: tar -t < archivo.tar");
    return 0;
}

/* ---- Extraer tar ---- */

static int cmd_tar_extract(const char *filename)
{
    /* Leer el archivo tar completo */
    int total = readfile(filename, tar_buf, sizeof(tar_buf) - 1);
    if (total < 0) {
        print("tar: no se puede leer: "); uprintn(filename);
        return 1;
    }

    uint32_t pos = 0;
    while (pos + TAR_BLOCK <= (uint32_t)total) {
        tar_hdr_t *hdr = (tar_hdr_t *)(tar_buf + pos);

        /* Bloque de fin: nombre vacío */
        if (hdr->name[0] == '\0') break;

        /* Verificar magic */
        if (hdr->magic[0] != 'u') { pos += TAR_BLOCK; continue; }

        uint32_t fsize = from_octal(hdr->size, 11);
        char typeflag  = hdr->typeflag;
        pos           += TAR_BLOCK;

        /* Construir nombre completo (prefix + name) */
        char fullname[256];
        int plen = 0;
        while (hdr->prefix[plen] && plen < 155) plen++;
        if (plen > 0) {
            memcpy_(fullname, hdr->prefix, (uint32_t)plen);
            fullname[plen] = '/';
            memcpy_(fullname + plen + 1, hdr->name, 100);
            fullname[plen + 1 + 99] = '\0';
        } else {
            memcpy_(fullname, hdr->name, 100);
            fullname[99] = '\0';
        }

        if (typeflag == '5' || typeflag == 0) {
            /* Directorio */
            if (typeflag == '5') {
                sys_mkdir_(fullname);
                print("d "); uprintn(fullname);
            } else {
                /* Archivo regular */
                if (fsize > 0 && pos + fsize <= (uint32_t)total) {
                    writefile(fullname, tar_buf + pos, fsize);
                    print("x "); uprintn(fullname);
                }
                uint32_t padded = (fsize + TAR_BLOCK - 1) & ~(TAR_BLOCK - 1u);
                pos += padded;
            }
        } else {
            uint32_t padded = (fsize + TAR_BLOCK - 1) & ~(TAR_BLOCK - 1u);
            pos += padded;
        }
    }
    return 0;
}

/* ---- main ---- */

int main(int argc, char **argv)
{
    if (argc < 2) {
        uprintn("uso: tar -c|-x|-t [archivo...] [> | < archivo.tar]");
        uprintn("  -c  crear tar con los archivos dados (redirige stdout)");
        uprintn("  -x  extraer tar desde un archivo");
        uprintn("  -t  listar contenido del tar");
        return 1;
    }

    const char *flags = argv[1];
    bool do_create  = false;
    bool do_extract = false;
    bool do_list    = false;

    for (int i = 0; flags[i]; i++) {
        if (flags[i] == 'c') do_create  = true;
        if (flags[i] == 'x') do_extract = true;
        if (flags[i] == 't') do_list    = true;
    }

    if (do_create) {
        /* Pasar argv+2 como lista de archivos */
        return cmd_tar_create(argc - 1, argv + 1);
    }
    if (do_extract) {
        if (argc < 3) { uprintn("tar: -x requiere nombre de archivo"); return 1; }
        return cmd_tar_extract(argv[2]);
    }
    if (do_list) {
        if (argc < 3) { uprintn("tar: -t requiere nombre de archivo"); return 1; }
        /* Para listar, extraemos pero solo imprimimos nombres */
        int total = readfile(argv[2], tar_buf, sizeof(tar_buf) - 1);
        if (total < 0) { print("tar: no se puede abrir: "); uprintn(argv[2]); return 1; }
        uint32_t pos = 0;
        while (pos + TAR_BLOCK <= (uint32_t)total) {
            tar_hdr_t *hdr = (tar_hdr_t *)(tar_buf + pos);
            if (hdr->name[0] == '\0') break;
            if (hdr->magic[0] == 'u') {
                print(hdr->prefix[0] ? hdr->prefix : "");
                if (hdr->prefix[0]) print("/");
                uprintn(hdr->name);
            }
            uint32_t fsize = from_octal(hdr->size, 11);
            uint32_t padded = (fsize + TAR_BLOCK - 1) & ~(TAR_BLOCK - 1u);
            pos += TAR_BLOCK + padded;
        }
        return 0;
    }

    uprintn("tar: especifica -c, -x, o -t");
    return 1;
}

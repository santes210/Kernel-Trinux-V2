/* auth/users.c  -  user accounts backed by /etc/passwd and /etc/shadow.
 *
 * This is the "system-level" notion of users (like Linux's account database),
 * NOT hardware isolation. Everything still runs in ring 0; the kernel just
 * tracks who is logged in and enforces ownership/root checks in the shell/VFS.
 *
 * Seguridad de contraseñas (formato actual):
 *   /etc/shadow guarda registros  "$<sal_hex>$<hash_hex>"  donde:
 *     - la sal son 8 bytes aleatorios (16 hex), ÚNICOS por contraseña, lo
 *       que elimina hashes idénticos para contraseñas iguales y hace inviable
 *       el uso de tablas rainbow precalculadas;
 *     - el hash es SHA-256 iterado PBKDF2_ITERS veces sobre sal+contraseña
 *       (key stretching: encarece el crackeo offline por fuerza bruta);
 *     - la comparación es en tiempo constante (anti timing-attack).
 *
 * Compatibilidad / migración automática:
 *   - registros legacy de 64 hex (SHA-256 sin sal) y texto plano se siguen
 *     verificando; en el primer login exitoso se re-escriben al formato
 *     salado nuevo sin pedir intervención del usuario.
 */
#include "users.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../lib/printf.h"
#include "../lib/sha256.h"
#include "../drivers/timer.h"
#include "../mm/kheap.h"

static user_t users[MAX_USERS];
static int    user_count;
static user_t *cur_user;

/* ---------- tiny helpers ---------- */

static void write_file(const char *path, const char *text)
{
    /* Kernel-internal account storage (passwd/shadow). This runs as part
     * of an already-authorized operation (users_save() is only reached
     * after the syscall layer verified the caller may change their own
     * password, or is root for useradd/etc). It must bypass the regular
     * VFS permission check on overwrite: /etc/shadow is 0600 owned by
     * root, so a non-root user changing their *own* password would
     * otherwise be blocked by vfs_create()'s ACC_WRITE check below. */
    vfs_node_t *n = vfs_resolve(path, vfs_get_root());
    if (!n)
        n = vfs_create(path, vfs_get_root());
    if (!n)
        return;
    n->size = 0;
    vfs_write(n, 0, (uint32_t)strlen(text), (uint8_t *)text);
    /* SECURITY: /etc/shadow contiene los hashes SHA-256 de las contraseñas.
     * ramfs crea archivos nuevos con 0644 (world-readable) por defecto, lo
     * cual anula por completo el propósito de tener un shadow file: hasta
     * ahora CUALQUIER usuario podía hacer `cat /etc/shadow` y llevarse los
     * hashes para crackearlos offline. En Linux real /etc/shadow es 0600
     * (o 0640 root:shadow). Lo forzamos aquí cada vez que se (re)escribe. */
    if (strcmp(path, "/etc/shadow") == 0)
        n->permissions = 0600;
}

static int read_file(const char *path, char *buf, uint32_t cap)
{
    vfs_node_t *n = vfs_resolve(path, vfs_get_root());
    if (!n || n->type != VFS_FILE)
        return -1;
    uint32_t r = vfs_read(n, 0, cap - 1, (uint8_t *)buf);
    buf[r] = '\0';
    return (int)r;
}

/* Split `line` on `sep` into up to `maxf` fields (in place). Returns count. */
static int split_fields(char *line, char sep, char **fields, int maxf)
{
    int n = 0;
    char *p = line;
    fields[n++] = p;
    while (*p && n < maxf) {
        if (*p == sep) {
            *p = '\0';
            fields[n++] = p + 1;
        }
        p++;
    }
    return n;
}

/* ---------- password hashing helpers ---------- */

#define SALT_BYTES      8                 /* 8 bytes de sal             */
#define SALT_HEX_LEN    (SALT_BYTES * 2)  /* 16 caracteres hex          */
#define HASH_HEX_LEN    64                /* SHA-256 = 32 bytes = 64 hex */
#define PBKDF2_ITERS    4096              /* key stretching              */
#define MAX_PW_INPUT    256               /* contraseñas más largas se truncan */

/* xorshift32 como fuente de respaldo cuando /dev/random no existe todavía
 * (p.ej. arranque muy temprano). Se siembra con el tick del timer y se
 * remezcla en cada uso; NO es criptográfica por sí sola, pero la sal solo
 * necesita ser única e impredecible, y el hash iterado pone el coste. */
static uint32_t salt_rng_state;
static uint32_t salt_rng_next(void)
{
    uint32_t x = salt_rng_state;
    if (x == 0) x = 0x53616C74u;  /* "Salt" */
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    salt_rng_state = x;
    return x;
}

/* Genera 16 chars hex de sal. Prioriza /dev/random (pool con entropía del
 * temporizador); si no está disponible, mezcla ticks del PIT en el xorshift. */
static void gen_salt_hex(char out[SALT_HEX_LEN + 1])
{
    uint8_t raw[SALT_BYTES];
    bool from_dev = false;

    vfs_node_t *rnd = vfs_resolve("/dev/random", vfs_get_root());
    if (rnd && rnd->type == VFS_DEVICE) {
        if (vfs_read(rnd, 0, SALT_BYTES, raw) == SALT_BYTES)
            from_dev = true;
    }
    if (!from_dev) {
        salt_rng_state ^= timer_get_ticks() * 2654435761u;
        for (int i = 0; i < SALT_BYTES; i++)
            raw[i] = (uint8_t)(salt_rng_next() >> 24);
    } else {
        /* Mezclar además entropía local para no depender solo del device. */
        for (int i = 0; i < SALT_BYTES; i++)
            raw[i] ^= (uint8_t)(salt_rng_next() >> 24);
    }

    static const char nibble[] = "0123456789abcdef";
    for (int i = 0; i < SALT_BYTES; i++) {
        out[i * 2]     = nibble[raw[i] >> 4];
        out[i * 2 + 1] = nibble[raw[i] & 0xF];
    }
    out[SALT_HEX_LEN] = '\0';
}

/* Hash iterado estilo PBKDF2:
 *   B0 = SHA256(sal || contraseña);  Bi = SHA256(Bi-1);  resultado = Bn-1.
 * Devuelve el hash final en hex (64 chars + NUL). */
static void hash_password_salted(const char *salt_hex, const char *plain,
                                 char out[HASH_HEX_LEN + 1])
{
    uint8_t block[16 + MAX_PW_INPUT];
    uint32_t salt_len = (uint32_t)strlen(salt_hex);
    if (salt_len > 16) salt_len = 16;
    memcpy(block, salt_hex, salt_len);

    uint32_t plen = (uint32_t)strlen(plain);
    if (plen > MAX_PW_INPUT) plen = MAX_PW_INPUT;
    memcpy(block + salt_len, plain, plen);

    uint8_t digest[32];
    sha256(block, salt_len + plen, digest);
    for (int i = 1; i < PBKDF2_ITERS; i++)
        sha256(digest, 32, digest);

    static const char nibble[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2]     = nibble[digest[i] >> 4];
        out[i * 2 + 1] = nibble[digest[i] & 0xF];
    }
    out[HASH_HEX_LEN] = '\0';
}

/* Construye el registro completo que se guarda en u->password / shadow:
 *   "$<sal>$<hash>"   (82 chars + NUL). */
static void make_password_record(const char *plain, char out[USER_PASS_MAX])
{
    char salt[SALT_HEX_LEN + 1];
    char hash[HASH_HEX_LEN + 1];
    gen_salt_hex(salt);
    hash_password_salted(salt, plain ? plain : "", hash);
    snprintf(out, USER_PASS_MAX, "$%s$%s", salt, hash);
}

/* Comparación de strings en tiempo constante (longitud ya conocida). */
static bool ct_streq(const char *a, const char *b, uint32_t len)
{
    uint8_t diff = 0;
    for (uint32_t i = 0; i < len; i++)
        diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

/* ¿El registro usa el formato salado nuevo "$sal$hash"? */
static bool is_salted_record(const char *stored)
{
    return stored && stored[0] == '$';
}

/* ---------- shadow (passwords) ---------- */

/* Manual line walker (NOT strtok) so it is safe to call from inside another
 * strtok loop in users_load(). */
static void shadow_get(const char *name, char *out, uint32_t cap)
{
    out[0] = '\0';
    char buf[1024];
    if (read_file("/etc/shadow", buf, sizeof(buf)) < 0)
        return;

    char *p = buf;
    while (*p) {
        char line[128];
        int i = 0;
        while (*p && *p != '\n' && i < (int)sizeof(line) - 1)
            line[i++] = *p++;
        line[i] = '\0';
        if (*p == '\n')
            p++;

        if (line[0] == '\0')
            continue;

        char *f[2];
        int nf = split_fields(line, ':', f, 2);
        if (nf == 2 && strcmp(f[0], name) == 0) {
            strncpy(out, f[1], cap - 1);
            out[cap - 1] = '\0';
            return;
        }
    }
}

/* ---------- public API ---------- */

void users_load(void)
{
    user_count = 0;

    char buf[2048];
    if (read_file("/etc/passwd", buf, sizeof(buf)) < 0)
        return;

    char *p = buf;
    while (*p && user_count < MAX_USERS) {
        char line[256];
        int i = 0;
        while (*p && *p != '\n' && i < (int)sizeof(line) - 1)
            line[i++] = *p++;
        line[i] = '\0';
        if (*p == '\n')
            p++;

        if (line[0] == '\0' || line[0] == '#')
            continue;

        /* passwd format: name:x:uid:gid:gecos:home:shell */
        char *f[7];
        int nf = split_fields(line, ':', f, 7);
        if (nf >= 6) {
            user_t *u = &users[user_count++];
            memset(u, 0, sizeof(*u));
            strncpy(u->name, f[0], USER_NAME_MAX - 1);
            u->uid = (uint32_t)atoi(f[2]);
            u->gid = (uint32_t)atoi(f[3]);
            strncpy(u->gecos, f[4], USER_NAME_MAX - 1);
            strncpy(u->home, f[5], USER_HOME_MAX - 1);
            strncpy(u->shell, (nf >= 7) ? f[6] : "/bin/sh", USER_HOME_MAX - 1);
            shadow_get(u->name, u->password, USER_PASS_MAX);
        }
    }
}

void users_save(void)
{
    /* El formato salado alarga cada línea de shadow hasta ~116 bytes y hay
     * hasta 32 usuarios: los antiguos buffers de pila de 2 KiB se quedaban
     * cortos (truncaban el archivo). Se reservan en el heap, con respaldo en
     * pila si el heap aún no está disponible (arranque muy temprano). */
    const uint32_t passwd_cap = 8192;
    const uint32_t shadow_cap = 4096;

    char passwd_stack[2048];
    char shadow_stack[2048];
    char *passwd = (char *)kmalloc(passwd_cap);
    char *shadow = (char *)kmalloc(shadow_cap);
    if (!passwd) { passwd = passwd_stack; }
    if (!shadow) { shadow = shadow_stack; }
    passwd[0] = '\0';
    shadow[0] = '\0';

    for (int i = 0; i < user_count; i++) {
        user_t *u = &users[i];
        char line[256];
        snprintf(line, sizeof(line), "%s:x:%u:%u:%s:%s:%s\n",
                       u->name, u->uid, u->gid, u->gecos, u->home, u->shell);
        strncat(passwd, line, passwd_cap - strlen(passwd) - 1);

        /* shadow: nombre:$sal$hash */
        char sline[128];
        snprintf(sline, sizeof(sline), "%s:%s\n", u->name, u->password);
        strncat(shadow, sline, shadow_cap - strlen(shadow) - 1);
    }

    write_file("/etc/passwd", passwd);
    write_file("/etc/shadow", shadow);

    if (passwd != passwd_stack) kfree(passwd);
    if (shadow != shadow_stack) kfree(shadow);
}

void users_init(void)
{
    /* Generar registros salados de las contraseñas por defecto. */
    char root_rec[USER_PASS_MAX], user_rec[USER_PASS_MAX];
    make_password_record("root", root_rec);
    make_password_record("user", user_rec);

    char buf[64];
    int r = read_file("/etc/passwd", buf, sizeof(buf));
    if (r <= 0 || buf[0] == '\0' || buf[0] == '\n') {
        write_file("/etc/passwd",
            "root:x:0:0:root:/root:/bin/sh\n"
            "user:x:1000:1000:Default User:/home/user:/bin/sh\n");

        /* Guardar registros "$sal$hash", nunca texto plano.
         * 384 bytes: dos líneas de hasta ~116 bytes con margen. */
        char shadow_content[384];
        snprintf(shadow_content, sizeof(shadow_content),
                 "root:%s\nuser:%s\n", root_rec, user_rec);
        write_file("/etc/shadow", shadow_content);
    }

    /* También verificar /etc/shadow por separado */
    r = read_file("/etc/shadow", buf, sizeof(buf));
    if (r <= 0 || buf[0] == '\0' || buf[0] == '\n') {
        char shadow_content[384];
        snprintf(shadow_content, sizeof(shadow_content),
                 "root:%s\nuser:%s\n", root_rec, user_rec);
        write_file("/etc/shadow", shadow_content);
    }

    /* Asegurar que los directorios home existen */
    if (!vfs_resolve("/root", vfs_get_root()))
        vfs_mkdir("/root", vfs_get_root());
    if (!vfs_resolve("/home/user", vfs_get_root())) {
        if (!vfs_resolve("/home", vfs_get_root()))
            vfs_mkdir("/home", vfs_get_root());
        vfs_mkdir("/home/user", vfs_get_root());
    }

    users_load();

    if (!users_find("root"))
        users_add("root", "root", 0, 0, "/root");
    if (!users_find("user"))
        users_add("user", "user", 1000, 1000, "/home/user");

    cur_user = users_find("user");
    if (!cur_user && user_count > 0)
        cur_user = &users[0];

    vfs_set_cred_provider(users_get_cred);
}

/* Credential provider for the VFS permission system. */
vfs_cred_t users_get_cred(void)
{
    vfs_cred_t c;
    if (cur_user) {
        c.uid = cur_user->uid;
        c.gid = cur_user->gid;
    } else {
        c.uid = 0;
        c.gid = 0;
    }
    return c;
}

const char *users_name_for_uid(uint32_t uid)
{
    user_t *u = users_find_uid(uid);
    return u ? u->name : "?";
}

user_t *users_find(const char *name)
{
    for (int i = 0; i < user_count; i++)
        if (strcmp(users[i].name, name) == 0)
            return &users[i];
    return NULL;
}

user_t *users_find_uid(uint32_t uid)
{
    for (int i = 0; i < user_count; i++)
        if (users[i].uid == uid)
            return &users[i];
    return NULL;
}

int     users_count(void)       { return user_count; }
user_t *users_at(int index)     { return (index >= 0 && index < user_count)
                                          ? &users[index] : NULL; }

user_t *users_add(const char *name, const char *password,
                  uint32_t uid, uint32_t gid, const char *home)
{
    if (!name || !*name || users_find(name) || user_count >= MAX_USERS)
        return NULL;

    user_t *u = &users[user_count++];
    memset(u, 0, sizeof(*u));
    strncpy(u->name, name, USER_NAME_MAX - 1);
    u->uid = uid;
    u->gid = gid;
    strncpy(u->gecos, name, USER_NAME_MAX - 1);
    if (home && *home)
        strncpy(u->home, home, USER_HOME_MAX - 1);
    else
        snprintf(u->home, USER_HOME_MAX, "/home/%s", name);
    strncpy(u->shell, "/bin/sh", USER_HOME_MAX - 1);

    /* Guardar siempre el registro salado "$sal$hash", nunca texto plano. */
    if (password && *password) {
        make_password_record(password, u->password);
        u->password[USER_PASS_MAX - 1] = '\0';
    }

    if (!vfs_resolve(u->home, vfs_get_root()))
        vfs_mkdir(u->home, vfs_get_root());
    vfs_node_t *hd = vfs_resolve(u->home, vfs_get_root());
    if (hd) { hd->owner_uid = u->uid; hd->owner_gid = u->gid; }

    users_save();
    return u;
}

int users_set_password(const char *name, const char *password)
{
    user_t *u = users_find(name);
    if (!u)
        return -1;

    /* Siempre guardar un registro salado nuevo (sal nueva en cada cambio),
     * nunca texto plano. */
    if (password && *password) {
        make_password_record(password, u->password);
        u->password[USER_PASS_MAX - 1] = '\0';
    } else {
        u->password[0] = '\0';  /* sin contraseña */
    }
    users_save();
    return 0;
}

int users_check_password(const char *name, const char *password)
{
    user_t *u = users_find(name);
    if (!u)
        return 0;
    if (u->password[0] == '\0')   /* sin contraseña configurada */
        return 1;

    const char *plain = password ? password : "";
    bool ok = false;

    if (is_salted_record(u->password)) {
        /* Formato nuevo: "$<sal_hex>$<hash_hex>". */
        const char *p = u->password + 1;          /* saltar '$' inicial */
        const char *sep = strchr(p, '$');
        if (sep && (uint32_t)(sep - p) == SALT_HEX_LEN) {
            char salt[SALT_HEX_LEN + 1];
            memcpy(salt, p, SALT_HEX_LEN);
            salt[SALT_HEX_LEN] = '\0';

            const char *stored_hash = sep + 1;
            if (strlen(stored_hash) == HASH_HEX_LEN) {
                char computed[HASH_HEX_LEN + 1];
                hash_password_salted(salt, plain, computed);
                ok = ct_streq(computed, stored_hash, HASH_HEX_LEN);
            }
        }
    } else {
        /* Legacy: hash SHA-256 sin sal (64 hex) o texto plano. */
        ok = sha256_verify(plain, u->password);
    }

    /* Migración automática: un registro legacy verificado correctamente se
     * re-escribe al formato salado nuevo (con sal nueva e iteraciones), sin
     * intervención del usuario. */
    if (ok && !is_salted_record(u->password)) {
        make_password_record(plain, u->password);
        u->password[USER_PASS_MAX - 1] = '\0';
        users_save();
    }

    return ok ? 1 : 0;
}

user_t *current_user(void)            { return cur_user; }
void    set_current_user(user_t *u)   { cur_user = u; }
bool    is_root(void)                 { return cur_user && cur_user->uid == 0; }

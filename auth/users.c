/* auth/users.c  -  user accounts backed by /etc/passwd and /etc/shadow.
 *
 * This is the "system-level" notion of users (like Linux's account database),
 * NOT hardware isolation. Everything still runs in ring 0; the kernel just
 * tracks who is logged in and enforces ownership/root checks in the shell/VFS.
 *
 * Password storage is plaintext on purpose: this is an educational kernel with
 * no real crypto. /etc/shadow holds `name:password` lines.
 */
#include "users.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../lib/printf.h"

static user_t users[MAX_USERS];
static int    user_count;
static user_t *cur_user;

/* ---------- tiny helpers ---------- */

static void write_file(const char *path, const char *text)
{
    vfs_node_t *n = vfs_create(path, vfs_get_root());
    if (!n)
        return;
    n->size = 0;
    vfs_write(n, 0, (uint32_t)strlen(text), (uint8_t *)text);
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
        /* extract one line */
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

    /* Manual line walker (NOT strtok): shadow_get() below calls into the VFS
     * which itself uses strtok, so we must not rely on strtok state here. */
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
    char passwd[2048];
    char shadow[1024];
    passwd[0] = '\0';
    shadow[0] = '\0';

    for (int i = 0; i < user_count; i++) {
        user_t *u = &users[i];
        char line[256];
        snprintf(line, sizeof(line), "%s:x:%u:%u:%s:%s:%s\n",
                       u->name, u->uid, u->gid, u->gecos, u->home, u->shell);
        strncat(passwd, line, sizeof(passwd) - strlen(passwd) - 1);

        char sline[96];
        snprintf(sline, sizeof(sline), "%s:%s\n", u->name, u->password);
        strncat(shadow, sline, sizeof(shadow) - strlen(shadow) - 1);
    }

    write_file("/etc/passwd", passwd);
    write_file("/etc/shadow", shadow);
}

void users_init(void)
{
    /* Seed /etc/passwd and /etc/shadow if missing or empty.
     * This covers: fresh boot, corrupted disk image, empty files from
     * a previous sync that went wrong, etc. */
    char buf[64];
    int r = read_file("/etc/passwd", buf, sizeof(buf));
    if (r <= 0 || buf[0] == '\0' || buf[0] == '\n') {
        write_file("/etc/passwd",
            "root:x:0:0:root:/root:/bin/sh\n"
            "user:x:1000:1000:Default User:/home/user:/bin/sh\n");
        write_file("/etc/shadow",
            "root:root\n"
            "user:user\n");
    }

    /* Also check /etc/shadow separately — passwd can exist without shadow */
    r = read_file("/etc/shadow", buf, sizeof(buf));
    if (r <= 0 || buf[0] == '\0' || buf[0] == '\n') {
        write_file("/etc/shadow",
            "root:root\n"
            "user:user\n");
    }

    /* ensure home directories exist */
    if (!vfs_resolve("/root", vfs_get_root()))
        vfs_mkdir("/root", vfs_get_root());
    if (!vfs_resolve("/home/user", vfs_get_root())) {
        if (!vfs_resolve("/home", vfs_get_root()))
            vfs_mkdir("/home", vfs_get_root());
        vfs_mkdir("/home/user", vfs_get_root());
    }

    users_load();

    /* Safety net: if after loading there's still no root or user account,
     * force-create them. This handles the case where /etc/passwd existed
     * on disk but contained garbage that users_load() couldn't parse. */
    if (!users_find("root"))
        users_add("root", "root", 0, 0, "/root");
    if (!users_find("user"))
        users_add("user", "user", 1000, 1000, "/home/user");

    /* default session: the regular user */
    cur_user = users_find("user");
    if (!cur_user && user_count > 0)
        cur_user = &users[0];

    /* let the VFS query who is logged in for permission checks */
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
        c.uid = 0;   /* pre-login: act as root */
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
    strncpy(u->password, password ? password : "", USER_PASS_MAX - 1);

    /* create the home directory and give it to the new user */
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
    strncpy(u->password, password ? password : "", USER_PASS_MAX - 1);
    u->password[USER_PASS_MAX - 1] = '\0';
    users_save();
    return 0;
}

int users_check_password(const char *name, const char *password)
{
    user_t *u = users_find(name);
    if (!u)
        return 0;
    if (u->password[0] == '\0')   /* no password set */
        return 1;
    return strcmp(u->password, password ? password : "") == 0;
}

user_t *current_user(void)            { return cur_user; }
void    set_current_user(user_t *u)   { cur_user = u; }
bool    is_root(void)                 { return cur_user && cur_user->uid == 0; }

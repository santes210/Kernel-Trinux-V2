#ifndef AUTH_USERS_H
#define AUTH_USERS_H

#include "../lib/types.h"
#include "../fs/vfs.h"

#define USER_NAME_MAX  32
#define USER_HOME_MAX  64
#define USER_PASS_MAX  32
#define MAX_USERS      32

/* A single account, mirrored from /etc/passwd (+ /etc/shadow password). */
typedef struct {
    char     name[USER_NAME_MAX];
    uint32_t uid;
    uint32_t gid;
    char     home[USER_HOME_MAX];
    char     shell[USER_HOME_MAX];
    char     gecos[USER_NAME_MAX];   /* description / full name */
    char     password[USER_PASS_MAX]; /* plaintext (educational kernel) */
} user_t;

/* Initialise the user database: ensure /etc/passwd & /etc/shadow exist with
 * default accounts (root, user), then load them into memory. */
void        users_init(void);

/* (Re)load /etc/passwd + /etc/shadow into the in-memory table. */
void        users_load(void);

/* Persist the in-memory table back to /etc/passwd + /etc/shadow. */
void        users_save(void);

/* Lookups. */
user_t     *users_find(const char *name);
user_t     *users_find_uid(uint32_t uid);
int         users_count(void);
user_t     *users_at(int index);

/* Add a new account (returns NULL on error / duplicate / full table). */
user_t     *users_add(const char *name, const char *password,
                      uint32_t uid, uint32_t gid, const char *home);

/* Change a user's password. Returns 0 on success. */
int         users_set_password(const char *name, const char *password);

/* Verify credentials. Returns 1 if password matches, 0 otherwise. */
int         users_check_password(const char *name, const char *password);

/* Current session. */
user_t     *current_user(void);
void        set_current_user(user_t *u);
bool        is_root(void);

/* Look up a user name for a given uid (for `ls -l`); returns "?" if unknown. */
const char *users_name_for_uid(uint32_t uid);

/* VFS credential provider (registered in users_init). */
vfs_cred_t  users_get_cred(void);

#endif /* AUTH_USERS_H */

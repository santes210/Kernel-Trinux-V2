#ifndef SHELL_SHELL_H
#define SHELL_SHELL_H

#include "../lib/types.h"
#include "../fs/vfs.h"

#define SHELL_LINE_MAX  256
#define SHELL_MAX_ARGS  32
#define SHELL_HISTORY   50
#define SHELL_MAX_ALIAS 16

/* Shell global state shared with commands.c */
typedef struct {
    vfs_node_t *cwd;
    char        hostname[64];
    char        user[32];
    uint8_t     color;
    bool        running;
    uint32_t    umask;        /* permission mask for new files/dirs */
    /* piped stdin: when non-NULL, commands like grep/wc read from here */
    const char *pipe_in;
    uint32_t    pipe_in_len;
} shell_state_t;

shell_state_t *shell_get_state(void);

void shell_run(void);
void shell_state_init_minimal(void);   /* setea cwd=/, hostname, color */
void shell_set_boot_mode(int mode);         /* 0=normal, 1=single/emergency */
int  shell_read_line(char *buf, int max);   /* line editor with history */
int  shell_read_password(char *buf, int max); /* masked input (no echo) */
void shell_login_prompt(void);              /* interactive login loop */
void shell_add_history(const char *line);
const char *shell_history_get(int index);
int  shell_history_count(void);

#endif /* SHELL_SHELL_H */

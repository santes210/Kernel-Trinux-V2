#ifndef PROCESS_PROCESS_H
#define PROCESS_PROCESS_H

#include "../lib/types.h"

typedef enum {
    PROC_RUNNING,
    PROC_READY,
    PROC_SLEEPING,
    PROC_ZOMBIE
} proc_state_t;

#define PROC_NAME_MAX 32
#define MAX_PROCESSES 128

/* Saved CPU context for cooperative/round-robin switching. */
typedef struct {
    uint32_t esp, ebp, ebx, esi, edi, eflags, eip;
} context_t;

typedef struct process {
    uint32_t      pid;
    char          name[PROC_NAME_MAX];
    proc_state_t  state;
    context_t     context;
    uint32_t      page_dir;       /* physical address of page directory */
    char          cwd[256];
    int           exit_code;
    void        (*entry)(void);
} process_t;

void        process_init(void);
process_t  *process_create(const char *name, void (*entry)(void));
void        process_exit(int code);
process_t  *process_get_current(void);
void        process_list(void);        /* prints active processes */
process_t  *process_get(uint32_t pid);
int         process_kill(uint32_t pid);
uint32_t    process_count(void);
process_t  *process_at(uint32_t index);

#endif /* PROCESS_PROCESS_H */

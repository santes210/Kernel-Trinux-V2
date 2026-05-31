#ifndef SHELL_COMMANDS_H
#define SHELL_COMMANDS_H

#include "../lib/types.h"

/* A command handler: argc/argv style. Returns an exit code. */
typedef int (*command_fn)(int argc, char **argv);

typedef struct {
    const char *name;
    command_fn  fn;
    const char *help;
} command_t;

/* Dispatch a parsed command line. Returns the command's exit code, or -1 if
 * the command is unknown. */
int commands_dispatch(int argc, char **argv);

/* The command table (for `help`). */
const command_t *commands_table(void);
int              commands_count(void);

#endif /* SHELL_COMMANDS_H */

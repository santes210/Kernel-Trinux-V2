#ifndef SHELL_EDITOR_H
#define SHELL_EDITOR_H

/* A tiny nano-style full-screen text editor.
 * Opens `path` (creating it in the VFS if missing), lets you edit, and saves
 * with Ctrl-S. Ctrl-X exits. Returns 0 on normal exit. */
int editor_run(const char *path);

#endif /* SHELL_EDITOR_H */

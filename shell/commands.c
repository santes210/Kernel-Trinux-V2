/* shell/commands.c  -  built-in shell commands. */
#include "commands.h"
#include "shell.h"
#include "../drivers/vga.h"
#include "../drivers/timer.h"
#include "../drivers/rtc.h"
#include "../drivers/keyboard.h"
#include "../drivers/serial.h"
#include "../cpu/ports.h"
#include "../fs/vfs.h"
#include "../fs/path.h"
#include "../fs/diskfs.h"
#include "editor.h"
#include "../mm/kheap.h"
#include "../mm/pmm.h"
#include "../process/process.h"
#include "../auth/users.h"
#include "../lib/string.h"
#include "../lib/printf.h"
#include "../include/kernel.h"
#include "../cpu/syscall.h"
#include "../user/userprog.h"
#include "../drivers/acpi_ec.h"
#include "../kernel/elf.h"
#include "tasm.h"
#include "tcc.h"

#define READ_BUF 4096

/* ---------- aliases ---------- */
static struct {
    char name[32];
    char value[128];
} aliases[SHELL_MAX_ALIAS];
static int alias_count;

/* ---------- helpers ---------- */

static vfs_node_t *resolve_arg(const char *path)
{
    shell_state_t *s = shell_get_state();
    return vfs_resolve(path, s->cwd);
}

/* convert permission bits to rwx string (with sticky-bit 't' support) */
static void perm_string(uint32_t type, uint32_t perms, char *out)
{
    out[0] = (type == VFS_DIRECTORY) ? 'd' : (type == VFS_DEVICE ? 'c' : '-');
    const char rwx[] = "rwxrwxrwx";
    for (int i = 0; i < 9; i++) {
        int bit = (perms >> (8 - i)) & 1;
        out[i + 1] = bit ? rwx[i] : '-';
    }
    /* sticky bit shows as 't' (or 'T' if 'others-execute' is off) in pos 9 */
    if (perms & VFS_STICKY)
        out[9] = (perms & 1) ? 't' : 'T';
    out[10] = '\0';
}

/* ---------- navigation & files ---------- */

/* Pick a VGA colour for a directory entry, ls/dircolors style:
 *   directories   -> light blue
 *   executables   -> light green (any rwx execute bit set)
 *   regular files -> default grey */
static uint8_t ls_entry_color(vfs_node_t *n)
{
    if (n->type == VFS_DIRECTORY)
        return vga_entry_color(VGA_LIGHT_BLUE, VGA_BLACK);
    if (n->permissions & 0111)   /* any execute bit -> executable */
        return vga_entry_color(VGA_LIGHT_GREEN, VGA_BLACK);
    return vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK);
}

static int cmd_ls(int argc, char **argv)
{
    bool longfmt = false;
    bool showall = false;        /* -a: include dotfiles */
    const char *target = NULL;

    for (int i = 1; i < argc; i++) {
        /* support combined short flags, e.g. "-la" / "-al" */
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            for (const char *f = argv[i] + 1; *f; f++) {
                if (*f == 'l') longfmt = true;
                else if (*f == 'a') showall = true;
            }
        } else {
            target = argv[i];
        }
    }

    vfs_node_t *node = target ? resolve_arg(target)
                              : shell_get_state()->cwd;
    if (!node) {
        kprintf("ls: cannot access '%s': No such file or directory\n", target);
        return 1;
    }

    if (node->type != VFS_DIRECTORY) {
        /* list single file */
        if (longfmt) {
            char perms[11];
            perm_string(node->type, node->permissions, perms);
            kprintf("%s %-6s %6u  %s\n", perms,
                    users_name_for_uid(node->owner_uid), node->size, node->name);
        } else {
            kprintf("%s\n", node->name);
        }
        return 0;
    }

    bool printed = false;
    for (uint32_t i = 0; ; i++) {
        vfs_node_t *child = vfs_readdir(node, i);
        if (!child)
            break;
        /* hide dotfiles unless -a was given */
        if (!showall && child->name[0] == '.')
            continue;
        if (longfmt) {
            char perms[11];
            perm_string(child->type, child->permissions, perms);
            kprintf("%s %-6s %6u  ", perms,
                    users_name_for_uid(child->owner_uid), child->size);
        }
        vga_print_color(child->name, ls_entry_color(child));
        if (child->type == VFS_DIRECTORY)
            kprintf("/");
        if (!longfmt) kprintf("  ");
        if (longfmt) kprintf("\n");
        printed = true;
    }
    if (!longfmt && printed) kprintf("\n");
    return 0;
}

static int cmd_cd(int argc, char **argv)
{
    shell_state_t *s = shell_get_state();
    const char *target = (argc < 2) ? "/" : argv[1];

    if (strcmp(target, "~") == 0 || (argc < 2)) {
        user_t *u = current_user();
        target = (u && u->home[0]) ? u->home : "/home/user";
    }

    vfs_node_t *node = vfs_resolve(target, s->cwd);
    if (!node) {
        kprintf("cd: %s: No such file or directory\n", target);
        return 1;
    }
    if (node->type != VFS_DIRECTORY) {
        kprintf("cd: %s: Not a directory\n", target);
        return 1;
    }
    s->cwd = node;
    return 0;
}

static int cmd_pwd(int argc, char **argv)
{
    (void)argc; (void)argv;
    char path[PATH_MAX];
    vfs_get_path(shell_get_state()->cwd, path);
    kprintf("%s\n", path);
    return 0;
}

/* Diagnose why vfs_mkdir/vfs_create failed: compute the parent directory
 * the same way the VFS does, then check each precondition in order. */
static const char *diagnose_create_failure(const char *path, vfs_node_t *cwd)
{
    char abs[PATH_MAX], parent_path[PATH_MAX], name[PATH_MAX], cwdpath[PATH_MAX];
    if (path_is_absolute(path)) {
        path_normalize("/", path, abs);
    } else {
        vfs_get_path(cwd ? cwd : vfs_get_root(), cwdpath);
        path_normalize(cwdpath, path, abs);
    }
    path_dirname(abs, parent_path);
    path_basename(abs, name);

    vfs_node_t *parent = vfs_resolve(parent_path, NULL);
    if (!parent)                              return "No such file or directory";
    if (parent->type != VFS_DIRECTORY)        return "Not a directory";
    if (!vfs_check_access(parent, ACC_WRITE)) return "Permission denied";
    if (vfs_finddir(parent, name))            return "File exists";
    if (parent->child_count >= VFS_MAX_CHILDREN)
        return "Directory full (too many entries per dir)";
    return "Out of memory";
}

/* mkdir, with optional `-p` (create parent dirs as needed, no error if exists).
 * Mirrors `mkdir -p` from coreutils. */
static int cmd_mkdir(int argc, char **argv)
{
    bool parents = false;
    int  first_path = 1;
    if (argc >= 2 && strcmp(argv[1], "-p") == 0) {
        parents = true;
        first_path = 2;
    }
    if (argc <= first_path) {
        kprintf("usage: mkdir [-p] <name>...\n");
        return 1;
    }

    shell_state_t *s = shell_get_state();
    int rc = 0;
    for (int i = first_path; i < argc; i++) {
        if (parents) {
            /* Walk the components and mkdir each missing one. We normalize
             * the absolute path then create '/a', '/a/b', '/a/b/c' in turn. */
            char abs[PATH_MAX];
            if (path_is_absolute(argv[i])) {
                path_normalize("/", argv[i], abs);
            } else {
                char cwd[PATH_MAX];
                vfs_get_path(s->cwd, cwd);
                path_normalize(cwd, argv[i], abs);
            }

            /* Step through every '/' boundary and create the prefix. */
            char build[PATH_MAX];
            uint32_t blen = 0;
            for (uint32_t k = 1; abs[k]; k++) {
                if (abs[k] == '/' || abs[k + 1] == '\0') {
                    uint32_t end = (abs[k + 1] == '\0') ? k + 1 : k;
                    memcpy(build, abs, end);
                    build[end] = '\0';
                    blen = end;
                    if (!vfs_resolve(build, NULL)) {
                        if (!vfs_mkdir(build, NULL)) {
                            kprintf("mkdir: cannot create '%s': %s\n",
                                    build, diagnose_create_failure(build, s->cwd));
                            rc = 1;
                            break;
                        }
                    }
                }
            }
            (void)blen;
        } else {
            if (vfs_mkdir(argv[i], s->cwd))
                continue;
            kprintf("mkdir: cannot create '%s': %s\n",
                    argv[i], diagnose_create_failure(argv[i], s->cwd));
            rc = 1;
        }
    }
    return rc;
}

static int cmd_rmdir(int argc, char **argv)
{
    if (argc < 2) { kprintf("usage: rmdir <name>\n"); return 1; }
    shell_state_t *s = shell_get_state();
    vfs_node_t *node = vfs_resolve(argv[1], s->cwd);
    if (!node) { kprintf("rmdir: '%s': not found\n", argv[1]); return 1; }
    if (node->type != VFS_DIRECTORY) {
        kprintf("rmdir: '%s': not a directory\n", argv[1]); return 1;
    }
    int r = vfs_delete(argv[1], s->cwd);
    if (r == -2) kprintf("rmdir: '%s': directory not empty\n", argv[1]);
    else if (r != 0) kprintf("rmdir: failed to remove '%s'\n", argv[1]);
    return r == 0 ? 0 : 1;
}

static int cmd_touch(int argc, char **argv)
{
    if (argc < 2) { kprintf("usage: touch <name>\n"); return 1; }
    shell_state_t *s = shell_get_state();
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (vfs_create(argv[i], s->cwd))
            continue;
        kprintf("touch: cannot touch '%s': %s\n",
                argv[i], diagnose_create_failure(argv[i], s->cwd));
        rc = 1;
    }
    return rc;
}

/* Recursively delete a node and all its descendants. Returns 0 on success,
 * or the first error code (negative) from vfs_delete that we hit. */
static int rm_recursive(vfs_node_t *node)
{
    if (!node) return -1;

    if (node->type == VFS_DIRECTORY) {
        /* Delete children first (back to front so the index stays valid
         * as ramfs_remove_node shifts entries down). */
        while (node->child_count > 0) {
            vfs_node_t *child = node->children[node->child_count - 1];
            int r = rm_recursive(child);
            if (r != 0) return r;
        }
    }

    /* Now the node itself can go. We use the full absolute path so
     * vfs_delete's permission/sticky checks run against the right parent. */
    char path[PATH_MAX];
    vfs_get_path(node, path);
    return vfs_delete(path, NULL);
}

/* rm with optional `-r`/`-R` (recursive) and `-f` (force, ignore missing).
 * `-rf` works too. Mirrors basic `rm` from coreutils. */
static int cmd_rm(int argc, char **argv)
{
    bool recursive = false, force = false;
    int first_path = 1;
    while (first_path < argc && argv[first_path][0] == '-' && argv[first_path][1]) {
        for (const char *f = argv[first_path] + 1; *f; f++) {
            if (*f == 'r' || *f == 'R') recursive = true;
            else if (*f == 'f') force = true;
            else {
                kprintf("rm: invalid option -- '%c'\n", *f);
                return 1;
            }
        }
        first_path++;
    }
    if (argc <= first_path) {
        kprintf("usage: rm [-rRf] <path>...\n");
        return 1;
    }

    shell_state_t *s = shell_get_state();
    int rc = 0;
    for (int i = first_path; i < argc; i++) {
        vfs_node_t *node = vfs_resolve(argv[i], s->cwd);
        if (!node) {
            if (!force) {
                kprintf("rm: cannot remove '%s': No such file or directory\n",
                        argv[i]);
                rc = 1;
            }
            continue;
        }
        if (node->type == VFS_DIRECTORY && !recursive) {
            kprintf("rm: cannot remove '%s': Is a directory (use -r)\n", argv[i]);
            rc = 1;
            continue;
        }

        int r = recursive ? rm_recursive(node)
                          : vfs_delete(argv[i], s->cwd);
        if (r == -3) kprintf("rm: cannot remove '%s': Permission denied\n", argv[i]);
        else if (r == -4) kprintf("rm: cannot remove '%s': Operation not permitted (sticky dir)\n", argv[i]);
        else if (r != 0) kprintf("rm: failed to remove '%s'\n", argv[i]);
        if (r != 0) rc = 1;
    }
    return rc;
}

static int cmd_cat(int argc, char **argv)
{
    /* with no file argument, echo piped stdin (cmd | cat) */
    if (argc < 2) {
        shell_state_t *s = shell_get_state();
        if (s->pipe_in) {
            for (uint32_t i = 0; i < s->pipe_in_len; i++)
                vga_putchar(s->pipe_in[i]);
            return 0;
        }
        kprintf("usage: cat <file>\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        vfs_node_t *node = resolve_arg(argv[i]);
        if (!node) { kprintf("cat: %s: No such file\n", argv[i]); continue; }
        if (node->type == VFS_DIRECTORY) {
            kprintf("cat: %s: Is a directory\n", argv[i]); continue;
        }
        if (!vfs_check_access(node, ACC_READ)) {
            kprintf("cat: %s: Permission denied\n", argv[i]); continue;
        }
        char buf[READ_BUF];
        uint32_t off = 0;
        for (;;) {
            uint32_t n = vfs_read(node, off, sizeof(buf) - 1, (uint8_t *)buf);
            if (n == 0) break;
            buf[n] = '\0';
            kprintf("%s", buf);
            off += n;
            if (n < sizeof(buf) - 1) break;
        }
    }
    return 0;
}

/* grep <pattern> [file] : print lines containing <pattern>.
 * With no file, reads from the pipe (stdin). Supports -i (ignore case),
 * -v (invert), -n (line numbers), -c (count). */
static int cmd_grep(int argc, char **argv)
{
    bool icase = false, invert = false, number = false, count = false;
    const char *pattern = NULL;
    const char *file = NULL;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) {
            for (const char *f = argv[i] + 1; *f; f++) {
                if (*f == 'i') icase = true;
                else if (*f == 'v') invert = true;
                else if (*f == 'n') number = true;
                else if (*f == 'c') count = true;
            }
        } else if (!pattern) {
            pattern = argv[i];
        } else {
            file = argv[i];
        }
    }
    if (!pattern) { kprintf("usage: grep [-ivnc] <pattern> [file]\n"); return 1; }

    /* gather the input text: file, or piped stdin */
    static char data[8192];
    uint32_t len = 0;
    shell_state_t *s = shell_get_state();
    if (file) {
        vfs_node_t *node = resolve_arg(file);
        if (!node || node->type != VFS_FILE) {
            kprintf("grep: %s: No such file\n", file); return 1;
        }
        if (!vfs_check_access(node, ACC_READ)) {
            kprintf("grep: %s: Permission denied\n", file); return 1;
        }
        len = vfs_read(node, 0, sizeof(data) - 1, (uint8_t *)data);
    } else if (s->pipe_in) {
        len = s->pipe_in_len;
        if (len > sizeof(data) - 1) len = sizeof(data) - 1;
        memcpy(data, s->pipe_in, len);
    } else {
        kprintf("grep: no input (use a file or a pipe)\n"); return 1;
    }
    data[len] = '\0';

    /* lower-case helper for case-insensitive search */
    char pat_lc[128];
    if (icase) {
        int i = 0;
        for (; pattern[i] && i < (int)sizeof(pat_lc) - 1; i++)
            pat_lc[i] = (char)tolower((unsigned char)pattern[i]);
        pat_lc[i] = '\0';
    }

    int matches = 0, lineno = 0;
    char *p = data;
    while (*p) {
        char line[1024];
        int i = 0;
        while (*p && *p != '\n' && i < (int)sizeof(line) - 1)
            line[i++] = *p++;
        line[i] = '\0';
        if (*p == '\n') p++;
        lineno++;

        bool found;
        if (icase) {
            char low[1024];
            int j = 0;
            for (; line[j] && j < (int)sizeof(low) - 1; j++)
                low[j] = (char)tolower((unsigned char)line[j]);
            low[j] = '\0';
            found = strstr(low, pat_lc) != NULL;
        } else {
            found = strstr(line, pattern) != NULL;
        }

        if (found != invert) {
            matches++;
            if (!count) {
                if (number) kprintf("%d:", lineno);
                kprintf("%s\n", line);
            }
        }
    }
    if (count)
        kprintf("%d\n", matches);
    return 0;
}

/* echo with optional '>' (truncate) and '>>' (append) redirection */
static int cmd_echo(int argc, char **argv)
{
    shell_state_t *s = shell_get_state();
    int redirect = -1;
    bool append = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], ">") == 0) {
            redirect = i;
            break;
        }
        if (strcmp(argv[i], ">>") == 0) {
            redirect = i;
            append = true;
            break;
        }
    }

    char out[READ_BUF];
    out[0] = '\0';
    int end = (redirect == -1) ? argc : redirect;
    for (int i = 1; i < end; i++) {
        strncat(out, argv[i], sizeof(out) - strlen(out) - 2);
        if (i < end - 1)
            strncat(out, " ", sizeof(out) - strlen(out) - 1);
    }

    if (redirect != -1 && redirect + 1 < argc) {
        strncat(out, "\n", sizeof(out) - strlen(out) - 1);
        vfs_node_t *node = vfs_create(argv[redirect + 1], s->cwd);
        if (!node) {
            kprintf("echo: cannot write '%s': %s\n", argv[redirect + 1],
                    diagnose_create_failure(argv[redirect + 1], s->cwd));
            return 1;
        }
        if (!append) node->size = 0;   /* '>' truncates, '>>' appends */
        vfs_write(node, node->size, (uint32_t)strlen(out), (uint8_t *)out);
    } else {
        kprintf("%s\n", out);
    }
    return 0;
}

/* edit / nano - open the full-screen text editor on a file */
static int cmd_edit(int argc, char **argv)
{
    if (argc < 2) { kprintf("usage: edit <file>\n"); return 1; }
    shell_state_t *s = shell_get_state();
    /* create the file if it doesn't exist yet, so a fresh edit works */
    if (!vfs_resolve(argv[1], s->cwd))
        vfs_create(argv[1], s->cwd);
    return editor_run(argv[1]);
}

static int cmd_write(int argc, char **argv)
{
    if (argc < 3) { kprintf("usage: write <file> <text...>\n"); return 1; }
    shell_state_t *s = shell_get_state();
    vfs_node_t *node = vfs_create(argv[1], s->cwd);
    if (!node) { kprintf("write: cannot create '%s'\n", argv[1]); return 1; }

    char out[READ_BUF];
    out[0] = '\0';
    for (int i = 2; i < argc; i++) {
        strncat(out, argv[i], sizeof(out) - strlen(out) - 2);
        if (i < argc - 1) strncat(out, " ", sizeof(out) - strlen(out) - 1);
    }
    strncat(out, "\n", sizeof(out) - strlen(out) - 1);
    node->size = 0;
    vfs_write(node, 0, (uint32_t)strlen(out), (uint8_t *)out);
    return 0;
}

/* Copy one regular file's contents from src to dst. Both must already exist
 * as nodes; dst is truncated. */
static int copy_file_contents(vfs_node_t *src, vfs_node_t *dst)
{
    char buf[READ_BUF];
    uint32_t off = 0;
    dst->size = 0;
    for (;;) {
        uint32_t n = vfs_read(src, off, sizeof(buf), (uint8_t *)buf);
        if (n == 0) break;
        if (vfs_write(dst, off, n, (uint8_t *)buf) != n) return -1;
        off += n;
        if (n < sizeof(buf)) break;
    }
    return 0;
}

/* Recursively copy `src` into the directory `dst_parent` under name `dst_name`.
 * Used by `cp -r`. Returns 0 on success, -1 on first error. */
static int cp_recursive(vfs_node_t *src, vfs_node_t *dst_parent,
                        const char *dst_name)
{
    char dst_path[PATH_MAX], parent_path[PATH_MAX];
    vfs_get_path(dst_parent, parent_path);
    path_join(parent_path, dst_name, dst_path);

    if (src->type == VFS_DIRECTORY) {
        if (!vfs_resolve(dst_path, NULL) && !vfs_mkdir(dst_path, NULL))
            return -1;
        vfs_node_t *new_dir = vfs_resolve(dst_path, NULL);
        if (!new_dir) return -1;
        for (uint32_t i = 0; i < src->child_count; i++) {
            vfs_node_t *child = src->children[i];
            if (cp_recursive(child, new_dir, child->name) != 0)
                return -1;
        }
        return 0;
    }

    /* regular file */
    vfs_node_t *new_file = vfs_create(dst_path, NULL);
    if (!new_file) return -1;
    return copy_file_contents(src, new_file);
}

/* cp with optional `-r`/`-R` (recursive directory copy).
 *   cp src dst       -> copy a regular file
 *   cp -r dir newdir -> copy a whole directory tree */
static int cmd_cp(int argc, char **argv)
{
    bool recursive = false;
    int first = 1;
    if (argc >= 2 && (strcmp(argv[1], "-r") == 0 || strcmp(argv[1], "-R") == 0)) {
        recursive = true;
        first = 2;
    }
    if (argc - first < 2) {
        kprintf("usage: cp [-r] <src> <dst>\n");
        return 1;
    }

    shell_state_t *s = shell_get_state();
    vfs_node_t *src = vfs_resolve(argv[first], s->cwd);
    if (!src) {
        kprintf("cp: cannot stat '%s': No such file or directory\n", argv[first]);
        return 1;
    }
    if (src->type == VFS_DIRECTORY && !recursive) {
        kprintf("cp: -r not specified; omitting directory '%s'\n", argv[first]);
        return 1;
    }

    if (!recursive) {
        vfs_node_t *dst = vfs_create(argv[first + 1], s->cwd);
        if (!dst) {
            kprintf("cp: cannot create '%s': %s\n", argv[first + 1],
                    diagnose_create_failure(argv[first + 1], s->cwd));
            return 1;
        }
        return copy_file_contents(src, dst) == 0 ? 0 : 1;
    }

    /* -r: figure out the destination parent + name */
    char abs[PATH_MAX], parent_path[PATH_MAX], name[PATH_MAX], cwdpath[PATH_MAX];
    const char *dst_arg = argv[first + 1];
    if (path_is_absolute(dst_arg)) {
        path_normalize("/", dst_arg, abs);
    } else {
        vfs_get_path(s->cwd, cwdpath);
        path_normalize(cwdpath, dst_arg, abs);
    }
    path_dirname(abs, parent_path);
    path_basename(abs, name);
    vfs_node_t *dst_parent = vfs_resolve(parent_path, NULL);
    if (!dst_parent) {
        kprintf("cp: cannot create '%s': No such file or directory\n", dst_arg);
        return 1;
    }
    return cp_recursive(src, dst_parent, name) == 0 ? 0 : 1;
}

static int cmd_mv(int argc, char **argv)
{
    if (argc < 3) { kprintf("usage: mv <src> <dst>\n"); return 1; }
    /* implement as cp + rm for files */
    int r = cmd_cp(argc, argv);
    if (r == 0) {
        char *rm_argv[2] = { "rm", argv[1] };
        cmd_rm(2, rm_argv);
    }
    return r;
}

static int cmd_stat(int argc, char **argv)
{
    if (argc < 2) { kprintf("usage: stat <path>\n"); return 1; }
    shell_state_t *s = shell_get_state();
    stat_t st;
    if (vfs_stat(argv[1], s->cwd, &st) != 0) {
        kprintf("stat: cannot stat '%s'\n", argv[1]); return 1;
    }
    char perms[11];
    perm_string(st.type, st.permissions, perms);
    kprintf("  File: %s\n", argv[1]);
    kprintf("  Type: %s\n",
            st.type == VFS_DIRECTORY ? "directory" :
            st.type == VFS_DEVICE ? "device" : "regular file");
    kprintf("  Size: %u bytes\n", st.size);
    kprintf("  Perms: %s\n", perms);
    kprintf("  Owner: %s (uid=%u, gid=%u)\n",
            users_name_for_uid(st.owner_uid), st.owner_uid, st.owner_gid);
    kprintf("  Created : tick %u   Modified: tick %u\n", st.created, st.modified);
    return 0;
}

/* recursive tree printer */
static void tree_recurse(vfs_node_t *node, char *prefix)
{
    for (uint32_t i = 0; ; i++) {
        vfs_node_t *child = vfs_readdir(node, i);
        if (!child) break;
        vfs_node_t *next = vfs_readdir(node, i + 1);
        bool last = (next == NULL);

        kprintf("%s%s", prefix, last ? "`-- " : "|-- ");
        if (child->type == VFS_DIRECTORY)
            vga_print_color(child->name,
                            vga_entry_color(VGA_LIGHT_BLUE, VGA_BLACK));
        else
            kprintf("%s", child->name);
        kprintf("\n");

        if (child->type == VFS_DIRECTORY) {
            char newprefix[256];
            snprintf(newprefix, sizeof(newprefix), "%s%s",
                     prefix, last ? "    " : "|   ");
            tree_recurse(child, newprefix);
        }
    }
}

static int cmd_tree(int argc, char **argv)
{
    vfs_node_t *node = (argc > 1) ? resolve_arg(argv[1])
                                  : shell_get_state()->cwd;
    if (!node) { kprintf("tree: '%s': not found\n", argv[1]); return 1; }
    char path[PATH_MAX];
    vfs_get_path(node, path);
    kprintf("%s\n", path);
    char prefix[2] = "";
    tree_recurse(node, prefix);
    return 0;
}

/* recursive find by name */
static void find_recurse(vfs_node_t *node, const char *name)
{
    for (uint32_t i = 0; ; i++) {
        vfs_node_t *child = vfs_readdir(node, i);
        if (!child) break;
        if (strstr(child->name, name)) {
            char path[PATH_MAX];
            vfs_get_path(child, path);
            kprintf("%s\n", path);
        }
        if (child->type == VFS_DIRECTORY)
            find_recurse(child, name);
    }
}

static int cmd_find(int argc, char **argv)
{
    if (argc < 2) { kprintf("usage: find <name>\n"); return 1; }
    find_recurse(shell_get_state()->cwd, argv[1]);
    return 0;
}

static int cmd_head(int argc, char **argv)
{
    if (argc < 2) { kprintf("usage: head <file> [n]\n"); return 1; }
    int n = (argc > 2) ? atoi(argv[2]) : 10;
    vfs_node_t *node = resolve_arg(argv[1]);
    if (!node || node->type != VFS_FILE) { kprintf("head: %s: not a file\n", argv[1]); return 1; }
    char buf[READ_BUF];
    uint32_t r = vfs_read(node, 0, sizeof(buf) - 1, (uint8_t *)buf);
    buf[r] = '\0';
    int lines = 0;
    for (uint32_t i = 0; i < r && lines < n; i++) {
        vga_putchar(buf[i]);
        if (buf[i] == '\n') lines++;
    }
    return 0;
}

static int cmd_tail(int argc, char **argv)
{
    if (argc < 2) { kprintf("usage: tail <file> [n]\n"); return 1; }
    int n = (argc > 2) ? atoi(argv[2]) : 10;
    vfs_node_t *node = resolve_arg(argv[1]);
    if (!node || node->type != VFS_FILE) { kprintf("tail: %s: not a file\n", argv[1]); return 1; }
    char buf[READ_BUF];
    uint32_t r = vfs_read(node, 0, sizeof(buf) - 1, (uint8_t *)buf);
    buf[r] = '\0';
    /* count total lines */
    int total = 0;
    for (uint32_t i = 0; i < r; i++) if (buf[i] == '\n') total++;
    int skip = total - n;
    if (skip < 0) skip = 0;
    int seen = 0;
    uint32_t i = 0;
    while (i < r && seen < skip) { if (buf[i] == '\n') seen++; i++; }
    kprintf("%s", buf + i);
    return 0;
}

/* count lines/words/chars over an in-memory buffer */
static void wc_count(const char *buf, uint32_t len,
                     uint32_t *lines, uint32_t *words, uint32_t *chars)
{
    bool inword = false;
    for (uint32_t i = 0; i < len; i++) {
        (*chars)++;
        if (buf[i] == '\n') (*lines)++;
        if (isspace((unsigned char)buf[i])) inword = false;
        else if (!inword) { inword = true; (*words)++; }
    }
}

static int cmd_wc(int argc, char **argv)
{
    uint32_t lines = 0, words = 0, chars = 0;
    shell_state_t *s = shell_get_state();

    if (argc < 2) {
        /* read from the pipe (cmd | wc) */
        if (!s->pipe_in) { kprintf("usage: wc <file>\n"); return 1; }
        wc_count(s->pipe_in, s->pipe_in_len, &lines, &words, &chars);
        kprintf("  %u  %u  %u\n", lines, words, chars);
        return 0;
    }

    vfs_node_t *node = resolve_arg(argv[1]);
    if (!node || node->type != VFS_FILE) { kprintf("wc: %s: not a file\n", argv[1]); return 1; }
    char buf[READ_BUF];
    uint32_t off = 0;
    for (;;) {
        uint32_t r = vfs_read(node, off, sizeof(buf), (uint8_t *)buf);
        if (r == 0) break;
        wc_count(buf, r, &lines, &words, &chars);
        off += r;
        if (r < sizeof(buf)) break;
    }
    kprintf("  %u  %u  %u  %s\n", lines, words, chars, argv[1]);
    return 0;
}

/* ---------- text-processing filters (sort/uniq/cut/tee/seq) ----------
 *
 * These all share one pattern: gather input text from either a file argument
 * or the pipe (stdin), then transform it. gather_input() centralizes that. */

/* Load input text into `buf` (NUL-terminated). Source is the file `path` if
 * non-NULL, otherwise the shell pipe. Returns the length, or -1 on error
 * (after printing a message prefixed with `who`). */
static int gather_input(const char *who, const char *path,
                        char *buf, uint32_t bufsz)
{
    shell_state_t *s = shell_get_state();
    uint32_t len = 0;
    if (path) {
        vfs_node_t *node = resolve_arg(path);
        if (!node || node->type != VFS_FILE) {
            kprintf("%s: %s: No such file\n", who, path);
            return -1;
        }
        if (!vfs_check_access(node, ACC_READ)) {
            kprintf("%s: %s: Permission denied\n", who, path);
            return -1;
        }
        len = vfs_read(node, 0, bufsz - 1, (uint8_t *)buf);
    } else if (s->pipe_in) {
        len = s->pipe_in_len;
        if (len > bufsz - 1) len = bufsz - 1;
        memcpy(buf, s->pipe_in, len);
    } else {
        kprintf("%s: no input (give a file or use a pipe)\n", who);
        return -1;
    }
    buf[len] = '\0';
    return (int)len;
}

/* sort: print input lines in ascending order. Flags: -r (reverse),
 * -u (unique, drop adjacent-after-sort duplicates), -n (numeric). */
#define MAX_LINES 256
static int cmd_sort(int argc, char **argv)
{
    bool reverse = false, uniq = false, numeric = false;
    const char *file = NULL;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) {
            for (const char *f = argv[i] + 1; *f; f++) {
                if (*f == 'r') reverse = true;
                else if (*f == 'u') uniq = true;
                else if (*f == 'n') numeric = true;
            }
        } else file = argv[i];
    }

    static char data[8192];
    int len = gather_input("sort", file, data, sizeof(data));
    if (len < 0) return 1;

    /* split into lines (in place) */
    char *lines[MAX_LINES];
    int n = 0;
    char *p = data;
    while (*p && n < MAX_LINES) {
        lines[n++] = p;
        char *nl = strchr(p, '\n');
        if (!nl) break;
        *nl = '\0';
        p = nl + 1;
    }

    /* simple insertion sort (n is small, lines are short) */
    for (int i = 1; i < n; i++) {
        char *key = lines[i];
        int j = i - 1;
        while (j >= 0) {
            int cmp;
            if (numeric) cmp = atoi(lines[j]) - atoi(key);
            else         cmp = strcmp(lines[j], key);
            bool swap = reverse ? (cmp < 0) : (cmp > 0);
            if (!swap) break;
            lines[j + 1] = lines[j];
            j--;
        }
        lines[j + 1] = key;
    }

    const char *prev = NULL;
    for (int i = 0; i < n; i++) {
        if (uniq && prev && strcmp(prev, lines[i]) == 0) continue;
        kprintf("%s\n", lines[i]);
        prev = lines[i];
    }
    return 0;
}

/* uniq: collapse *adjacent* duplicate lines. -c prefixes each with its count. */
static int cmd_uniq(int argc, char **argv)
{
    bool count = false;
    const char *file = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) count = true;
        else file = argv[i];
    }

    static char data[8192];
    int len = gather_input("uniq", file, data, sizeof(data));
    if (len < 0) return 1;

    char *p = data;
    char prev[1024]; prev[0] = '\0';
    bool have_prev = false;
    uint32_t run = 0;
    while (*p) {
        char line[1024];
        int i = 0;
        while (*p && *p != '\n' && i < (int)sizeof(line) - 1) line[i++] = *p++;
        line[i] = '\0';
        if (*p == '\n') p++;

        if (have_prev && strcmp(prev, line) == 0) {
            run++;
        } else {
            if (have_prev) {
                if (count) kprintf("%7u %s\n", run, prev);
                else       kprintf("%s\n", prev);
            }
            strncpy(prev, line, sizeof(prev) - 1);
            prev[sizeof(prev) - 1] = '\0';
            have_prev = true;
            run = 1;
        }
    }
    if (have_prev) {
        if (count) kprintf("%7u %s\n", run, prev);
        else       kprintf("%s\n", prev);
    }
    return 0;
}

/* cut: select fields/characters from each line.
 *   cut -d<delim> -f<n>   field n (1-based) split on <delim>
 *   cut -c<n>             character n (1-based) of each line */
static int cmd_cut(int argc, char **argv)
{
    char delim = '\t';
    int field = 0, charpos = 0;
    const char *file = NULL;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] == 'd') {
            delim = argv[i][2] ? argv[i][2] : (i + 1 < argc ? argv[++i][0] : '\t');
        } else if (argv[i][0] == '-' && argv[i][1] == 'f') {
            field = argv[i][2] ? atoi(argv[i] + 2) : (i + 1 < argc ? atoi(argv[++i]) : 0);
        } else if (argv[i][0] == '-' && argv[i][1] == 'c') {
            charpos = argv[i][2] ? atoi(argv[i] + 2) : (i + 1 < argc ? atoi(argv[++i]) : 0);
        } else {
            file = argv[i];
        }
    }
    if (field == 0 && charpos == 0) {
        kprintf("usage: cut -f<n> [-d<delim>] | -c<n>  [file]\n");
        return 1;
    }

    static char data[8192];
    int len = gather_input("cut", file, data, sizeof(data));
    if (len < 0) return 1;

    char *p = data;
    while (*p) {
        char line[1024];
        int i = 0;
        while (*p && *p != '\n' && i < (int)sizeof(line) - 1) line[i++] = *p++;
        line[i] = '\0';
        if (*p == '\n') p++;

        if (charpos > 0) {
            int L = (int)strlen(line);
            if (charpos <= L) kprintf("%c\n", line[charpos - 1]);
            else kprintf("\n");
        } else {
            /* split on delim, print the field-th token */
            int f = 1; char *start = line; bool printed = false;
            for (char *q = line; ; q++) {
                if (*q == delim || *q == '\0') {
                    if (f == field) {
                        char save = *q; *q = '\0';
                        kprintf("%s\n", start); *q = save;
                        printed = true; break;
                    }
                    f++;
                    start = q + 1;
                    if (*q == '\0') break;
                }
            }
            if (!printed) kprintf("\n");
        }
    }
    return 0;
}

/* tee: copy stdin to a file AND to the terminal. */
static int cmd_tee(int argc, char **argv)
{
    bool append = false;
    const char *file = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0) append = true;
        else file = argv[i];
    }
    if (!file) { kprintf("usage: tee [-a] <file>\n"); return 1; }

    static char data[8192];
    int len = gather_input("tee", NULL, data, sizeof(data));
    if (len < 0) return 1;

    /* echo to terminal */
    for (int i = 0; i < len; i++) vga_putchar(data[i]);

    /* write to file */
    vfs_node_t *node = vfs_create(file, shell_get_state()->cwd);
    if (!node) {
        kprintf("tee: cannot write '%s': %s\n", file,
                diagnose_create_failure(file, shell_get_state()->cwd));
        return 1;
    }
    if (!append) node->size = 0;
    vfs_write(node, node->size, (uint32_t)len, (uint8_t *)data);
    return 0;
}

/* seq: print a sequence of numbers. seq N | seq A B | seq A STEP B */
static int cmd_seq(int argc, char **argv)
{
    if (argc < 2) { kprintf("usage: seq [first [incr]] last\n"); return 1; }
    int first = 1, incr = 1, last;
    if (argc == 2)      { last = atoi(argv[1]); }
    else if (argc == 3) { first = atoi(argv[1]); last = atoi(argv[2]); }
    else                { first = atoi(argv[1]); incr = atoi(argv[2]); last = atoi(argv[3]); }
    if (incr == 0) { kprintf("seq: increment must not be zero\n"); return 1; }

    if (incr > 0) for (int v = first; v <= last; v += incr) kprintf("%d\n", v);
    else          for (int v = first; v >= last; v += incr) kprintf("%d\n", v);
    return 0;
}

/* basename: strip directory (and optional suffix) from a path. */
static int cmd_basename(int argc, char **argv)
{
    if (argc < 2) { kprintf("usage: basename <path> [suffix]\n"); return 1; }
    char out[PATH_MAX];
    path_basename(argv[1], out);
    if (argc > 2) {
        int bl = (int)strlen(out), sl = (int)strlen(argv[2]);
        if (sl < bl && strcmp(out + bl - sl, argv[2]) == 0)
            out[bl - sl] = '\0';
    }
    kprintf("%s\n", out);
    return 0;
}

/* dirname: strip the last component from a path. */
static int cmd_dirname(int argc, char **argv)
{
    if (argc < 2) { kprintf("usage: dirname <path>\n"); return 1; }
    char out[PATH_MAX];
    path_dirname(argv[1], out);
    kprintf("%s\n", out);
    return 0;
}

/* which: report whether a name is a built-in command. */
static int cmd_which(int argc, char **argv)
{
    if (argc < 2) { kprintf("usage: which <command>\n"); return 1; }
    int rc = 0;
    const command_t *t = commands_table();
    int nt = commands_count();
    for (int i = 1; i < argc; i++) {
        bool found = false;
        for (int j = 0; j < nt; j++) {
            if (strcmp(t[j].name, argv[i]) == 0) { found = true; break; }
        }
        if (found) kprintf("%s: shell built-in command\n", argv[i]);
        else { kprintf("%s: not found\n", argv[i]); rc = 1; }
    }
    return rc;
}

/* env: print a few environment-like values (this kernel has no real env). */
static int cmd_env(int argc, char **argv)
{
    (void)argc; (void)argv;
    shell_state_t *s = shell_get_state();
    user_t *u = current_user();
    char cwd[PATH_MAX];
    vfs_get_path(s->cwd, cwd);
    kprintf("USER=%s\n", u ? u->name : s->user);
    kprintf("HOME=%s\n", (u && u->home[0]) ? u->home : "/home/user");
    kprintf("HOSTNAME=%s\n", s->hostname);
    kprintf("PWD=%s\n", cwd);
    kprintf("SHELL=mysh\n");
    kprintf("TERM=trinux-vga\n");
    return 0;
}

/* yes: repeatedly print a string (bounded so it can't hang the shell). */
static int cmd_yes(int argc, char **argv)
{
    const char *msg = (argc > 1) ? argv[1] : "y";
    for (int i = 0; i < 100; i++) kprintf("%s\n", msg);
    kprintf("(yes: stopped after 100 lines)\n");
    return 0;
}

/* ---------- system ---------- */

static int cmd_clear(int argc, char **argv)
{
    (void)argc; (void)argv;
    vga_clear();
    return 0;
}

static int cmd_uname(int argc, char **argv)
{
    bool all = (argc > 1 && strcmp(argv[1], "-a") == 0);
    if (all) {
        kprintf("%s %s %s %s\n", KERNEL_NAME, KERNEL_VERSION,
                KERNEL_ARCH, KERNEL_BUILD);
    } else {
        kprintf("%s\n", KERNEL_NAME);
    }
    return 0;
}

static int cmd_uptime(int argc, char **argv)
{
    (void)argc; (void)argv;
    uint32_t secs = uptime();
    uint32_t h = secs / 3600;
    uint32_t m = (secs % 3600) / 60;
    uint32_t s = secs % 60;
    kprintf("up %u:%02u:%02u  (%u ticks)\n", h, m, s, timer_get_ticks());
    return 0;
}

static int cmd_date(int argc, char **argv)
{
    (void)argc; (void)argv;
    datetime_t dt;
    rtc_read_datetime(&dt);
    kprintf("%04u-%02u-%02u %02u:%02u:%02u\n",
            dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
    return 0;
}

static int cmd_whoami(int argc, char **argv)
{
    (void)argc; (void)argv;
    user_t *u = current_user();
    kprintf("%s\n", u ? u->name : shell_get_state()->user);
    return 0;
}

/* ---------- permissions ---------- */

/* parse an octal mode string like "755" or "0644" */
static int parse_octal(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    if (*s == '0') s++;          /* optional leading 0 */
    if (*s == '\0') { *out = 0; return 0; }
    while (*s) {
        if (*s < '0' || *s > '7')
            return -1;
        v = v * 8 + (*s - '0');
        s++;
    }
    *out = v;
    return 0;
}

static int cmd_umask(int argc, char **argv)
{
    shell_state_t *s = shell_get_state();
    if (argc < 2) {
        /* print current umask as 4-digit octal */
        uint32_t m = s->umask;
        kprintf("0%u%u%u\n", (m >> 6) & 7, (m >> 3) & 7, m & 7);
        return 0;
    }
    uint32_t mask;
    if (parse_octal(argv[1], &mask) != 0 || mask > 0777) {
        kprintf("umask: invalid mask '%s'\n", argv[1]);
        return 1;
    }
    s->umask = mask;
    vfs_set_umask(mask);
    return 0;
}

static int cmd_chmod(int argc, char **argv)
{
    if (argc < 3) { kprintf("usage: chmod <octal> <path>  (e.g. chmod 644 file)\n"); return 1; }
    uint32_t mode;
    if (parse_octal(argv[1], &mode) != 0) {
        kprintf("chmod: invalid mode '%s'\n", argv[1]); return 1;
    }
    int r = vfs_chmod(argv[2], shell_get_state()->cwd, mode);
    if (r == -1) kprintf("chmod: cannot access '%s'\n", argv[2]);
    else if (r == -2) kprintf("chmod: changing perms of '%s': Operation not permitted\n", argv[2]);
    return r == 0 ? 0 : 1;
}

static int cmd_chown(int argc, char **argv)
{
    if (argc < 3) { kprintf("usage: chown <user> <path>\n"); return 1; }
    user_t *u = users_find(argv[1]);
    if (!u) { kprintf("chown: invalid user: '%s'\n", argv[1]); return 1; }
    int r = vfs_chown(argv[2], shell_get_state()->cwd, u->uid, u->gid);
    if (r == -1) kprintf("chown: cannot access '%s'\n", argv[2]);
    else if (r == -2) kprintf("chown: changing ownership of '%s': Operation not permitted\n", argv[2]);
    return r == 0 ? 0 : 1;
}

/* ---------- users ---------- */

static int cmd_id(int argc, char **argv)
{
    user_t *u;
    if (argc > 1) {
        u = users_find(argv[1]);
        if (!u) { kprintf("id: '%s': no such user\n", argv[1]); return 1; }
    } else {
        u = current_user();
        if (!u) { kprintf("id: no current user\n"); return 1; }
    }
    kprintf("uid=%u(%s) gid=%u groups=%u(%s)\n",
            u->uid, u->name, u->gid, u->gid, u->name);
    return 0;
}

static int cmd_users(int argc, char **argv)
{
    (void)argc; (void)argv;
    int n = users_count();
    for (int i = 0; i < n; i++) {
        user_t *u = users_at(i);
        kprintf("%s%s", u->name, (i < n - 1) ? " " : "");
    }
    kprintf("\n");
    return 0;
}

static int cmd_groups(int argc, char **argv)
{
    (void)argc; (void)argv;
    user_t *u = current_user();
    /* simple model: each user is in its own primary group */
    kprintf("%s\n", u ? u->name : "user");
    return 0;
}

/* su [user]  - switch user (defaults to root); asks for the target password. */
static int cmd_su(int argc, char **argv)
{
    shell_state_t *s = shell_get_state();
    const char *target = (argc > 1) ? argv[1] : "root";

    user_t *u = users_find(target);
    if (!u) { kprintf("su: user '%s' does not exist\n", target); return 1; }

    /* root can switch to anyone without a password */
    if (!is_root()) {
        char pass[64];
        kprintf("Password: ");
        shell_read_password(pass, sizeof(pass));
        if (!users_check_password(target, pass)) {
            kprintf("su: Authentication failure\n");
            return 1;
        }
    }

    set_current_user(u);
    strncpy(s->user, u->name, sizeof(s->user) - 1);
    s->cwd = vfs_resolve(u->home, s->cwd);
    if (!s->cwd) s->cwd = vfs_get_root();
    return 0;
}

/* login - log in as a different user (full username + password). */
static int cmd_login(int argc, char **argv)
{
    (void)argc; (void)argv;
    shell_login_prompt();
    user_t *u = current_user();
    if (u)
        strncpy(shell_get_state()->user, u->name,
                sizeof(shell_get_state()->user) - 1);
    return 0;
}

/* logout - drop back to the login screen. */
static int cmd_logout(int argc, char **argv)
{
    (void)argc; (void)argv;
    kprintf("logout\n");
    shell_login_prompt();
    user_t *u = current_user();
    if (u) {
        strncpy(shell_get_state()->user, u->name,
                sizeof(shell_get_state()->user) - 1);
        shell_get_state()->cwd = vfs_resolve(u->home, vfs_get_root());
        if (!shell_get_state()->cwd)
            shell_get_state()->cwd = vfs_get_root();
    }
    return 0;
}

/* useradd <name> [password]  - root only. */
static int cmd_useradd(int argc, char **argv)
{
    if (!is_root()) { kprintf("useradd: permission denied (root only)\n"); return 1; }
    if (argc < 2) { kprintf("usage: useradd <name> [password]\n"); return 1; }

    if (users_find(argv[1])) {
        kprintf("useradd: user '%s' already exists\n", argv[1]); return 1;
    }

    /* assign next free uid >= 1000 */
    uint32_t uid = 1000;
    while (users_find_uid(uid)) uid++;

    const char *pass = (argc > 2) ? argv[2] : "";
    user_t *u = users_add(argv[1], pass, uid, uid, NULL);
    if (!u) { kprintf("useradd: failed to add user\n"); return 1; }
    kprintf("added user '%s' (uid=%u, home=%s)\n", u->name, u->uid, u->home);
    return 0;
}

/* userdel <name> handled via passwd? keep it simple: passwd <user>. */
static int cmd_passwd(int argc, char **argv)
{
    shell_state_t *s = shell_get_state();
    const char *target = (argc > 1) ? argv[1] : s->user;

    user_t *u = users_find(target);
    if (!u) { kprintf("passwd: user '%s' does not exist\n", target); return 1; }

    /* Only root may change another user's password. */
    if (!is_root() && strcmp(target, current_user()->name) != 0) {
        kprintf("passwd: permission denied\n");
        return 1;
    }

    /* Non-root must confirm their current password. */
    if (!is_root()) {
        char old[64];
        kprintf("Current password: ");
        shell_read_password(old, sizeof(old));
        if (!users_check_password(target, old)) {
            kprintf("passwd: Authentication failure\n");
            return 1;
        }
    }

    char p1[64], p2[64];
    kprintf("New password: ");
    shell_read_password(p1, sizeof(p1));
    kprintf("Retype new password: ");
    shell_read_password(p2, sizeof(p2));
    if (strcmp(p1, p2) != 0) {
        kprintf("passwd: passwords do not match\n");
        return 1;
    }
    users_set_password(target, p1);
    kprintf("passwd: password updated successfully\n");
    return 0;
}

static int cmd_hostname(int argc, char **argv)
{
    shell_state_t *s = shell_get_state();
    if (argc > 1) {
        strncpy(s->hostname, argv[1], sizeof(s->hostname) - 1);
        /* persist to /etc/hostname */
        vfs_node_t *hn = vfs_create("/etc/hostname", s->cwd);
        if (hn) {
            char buf[80];
            snprintf(buf, sizeof(buf), "%s\n", s->hostname);
            hn->size = 0;
            vfs_write(hn, 0, (uint32_t)strlen(buf), (uint8_t *)buf);
        }
    } else {
        kprintf("%s\n", s->hostname);
    }
    return 0;
}

static int cmd_free(int argc, char **argv)
{
    (void)argc; (void)argv;
    uint32_t total = pmm_get_total_memory();
    uint32_t used  = pmm_get_used_memory();
    uint32_t freeb = pmm_get_free_memory();
    size_t huse, hfree, hlarge; uint32_t hblocks;
    kheap_stats(&huse, &hfree, &hlarge, &hblocks);

    kprintf("              total       used       free\n");
    kprintf("Phys mem: %8u K %8u K %8u K\n",
            total / 1024, used / 1024, freeb / 1024);
    kprintf("Kheap:    %8u K %8u K %8u K  (largest free %u K, %u blocks)\n",
            (huse + hfree) / 1024, huse / 1024, hfree / 1024,
            (uint32_t)(hlarge / 1024), hblocks);
    return 0;
}

/* df - report disk space usage (like Unix df). */
static int cmd_df(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!diskfs_available()) {
        kprintf("Filesystem     Size   Used   Avail  Use%%  Mounted\n");
        kprintf("ramfs          (RAM)     -       -     -   /\n");
        kprintf("\nNo disk attached: files live in RAM and are lost on reboot.\n");
        kprintf("Run QEMU with -drive file=disk.img,format=raw,if=ide to persist.\n");
        return 0;
    }

    uint32_t total_mb = diskfs_total_mb();
    uint32_t used  = diskfs_used_bytes();
    uint32_t used_mb = used / (1024 * 1024);
    uint32_t avail_mb = (total_mb > used_mb) ? total_mb - used_mb : 0;
    uint32_t permille = total_mb ? (used_mb * 1000) / total_mb : 0;

    kprintf("Filesystem      Size        Used       Avail   Use%%   Mounted\n");
    kprintf("mkfs (disk)  %6u MB  %6u MB  %6u MB  %u.%u%%   /\n",
            total_mb, used_mb, avail_mb,
            permille / 10, permille % 10);
    return 0;
}

/* ---------- top: interactive process/system monitor (htop-style) ---------- */

static const char *proc_state_str(proc_state_t s)
{
    switch (s) {
    case PROC_RUNNING:  return "RUN ";
    case PROC_READY:    return "RDY ";
    case PROC_SLEEPING: return "SLP ";
    case PROC_ZOMBIE:   return "ZOMB";
    default:            return "?   ";
    }
}

/* draw a [#####-----] bar of given width for pct (0..100) in a color */
static void top_bar(uint32_t pct, int width, uint8_t fillcolor)
{
    if (pct > 100) pct = 100;
    int filled = (int)((pct * width) / 100);
    kprintf("[");
    for (int i = 0; i < width; i++) {
        if (i < filled)
            vga_print_color("|", fillcolor);
        else
            kprintf("-");
    }
    kprintf("] %u%%", pct);
}

static void top_render(void)
{
    vga_clear();
    vga_set_cursor(0, 0);

    /* ---- header bar ---- */
    uint8_t hdr = vga_entry_color(VGA_BLACK, VGA_LIGHT_CYAN);
    vga_set_color(hdr);
    kprintf(" mytop - press q or ESC to quit, r to refresh now            ");
    kprintf("\n");
    vga_set_color(shell_get_state()->color);

    /* ---- uptime / clock ---- */
    uint32_t secs = uptime();
    kprintf("\n  Uptime: %u:%02u:%02u   Ticks: %u   Hz: %u\n",
            secs / 3600, (secs % 3600) / 60, secs % 60,
            timer_get_ticks(), timer_get_freq());

    /* ---- CPU usage ---- */
    uint32_t cpu = timer_cpu_usage();
    kprintf("  CPU  ");
    top_bar(cpu, 30, vga_entry_color(VGA_LIGHT_RED, VGA_BLACK));
    kprintf("\n");

    /* ---- physical memory ---- */
    uint32_t ptot = pmm_get_total_memory();
    uint32_t pused = pmm_get_used_memory();
    uint32_t ppct = (ptot/1024) ? ((pused/1024) * 100) / (ptot/1024) : 0;
    kprintf("  Mem  ");
    top_bar(ppct, 30, vga_entry_color(VGA_LIGHT_GREEN, VGA_BLACK));
    kprintf("  %u/%u MB\n", pused / (1024*1024), ptot / (1024*1024));

    /* ---- kernel heap ---- */
    size_t huse, hfree, hlarge; uint32_t hblocks;
    kheap_stats(&huse, &hfree, &hlarge, &hblocks);
    uint32_t htot = (uint32_t)(huse + hfree);
    uint32_t hpct = (htot/1024) ? ((uint32_t)(huse/1024) * 100) / (htot/1024) : 0;
    kprintf("  Heap ");
    top_bar(hpct, 30, vga_entry_color(VGA_LIGHT_MAGENTA, VGA_BLACK));
    kprintf("  %u/%u KB\n", (uint32_t)(huse/1024), htot/1024);

    /* ---- disk ---- */
    if (diskfs_available()) {
        uint32_t dtot_mb = diskfs_total_mb();
        uint32_t dused = diskfs_used_bytes();
        uint32_t dused_mb = dused / (1024*1024);
        uint32_t dpct = dtot_mb ? (dused_mb * 100) / dtot_mb : 0;
        kprintf("  Disk ");
        top_bar(dpct, 30, vga_entry_color(VGA_LIGHT_BLUE, VGA_BLACK));
        kprintf("  %u MB / %u MB\n", dused_mb, dtot_mb);
    } else {
        kprintf("  Disk  (none - RAM only)\n");
    }

    /* ---- battery ---- */
    battery_info_t bat;
    if (acpi_ec_read_battery(&bat)) {
        uint8_t batcol = bat.percentage <= 15
            ? vga_entry_color(VGA_LIGHT_RED, VGA_BLACK)
            : (bat.charging
                ? vga_entry_color(VGA_LIGHT_GREEN, VGA_BLACK)
                : vga_entry_color(VGA_LIGHT_BROWN, VGA_BLACK));
        kprintf("  Bat  ");
        if (bat.percentage != 0xFF) {
            top_bar(bat.percentage, 30, batcol);
            kprintf("  %s", bat.charging ? "CHG" :
                            bat.discharging ? "BAT" :
                            bat.ac_connected ? "AC" : "?");
            if (bat.voltage_mv)
                kprintf("  %u.%uV", bat.voltage_mv / 1000,
                        (bat.voltage_mv % 1000) / 100);
        } else {
            kprintf("  (present but % unknown)");
        }
        kprintf("\n");
    } else {
        kprintf("  Bat   (none / not detected)\n");
    }

    /* ---- process table ---- */
    /* Compute the total ticks consumed by all processes during this sample
     * window so each %CPU is normalised against the others.  When the box
     * is idle the bulk of those ticks belong to the `idle` task, which is
     * exactly what htop on Linux shows too. */
    uint32_t window_total = 0;
    uint32_t n = process_count();
    for (uint32_t i = 0; i < n; i++) {
        process_t *p = process_at(i);
        if (p) window_total += p->ticks_window;
    }
    if (window_total == 0) window_total = 1;  /* avoid /0 */

    kprintf("\n  Tasks: %u\n", n);
    uint8_t th = vga_entry_color(VGA_BLACK, VGA_LIGHT_GREY);
    vga_set_color(th);
    kprintf("  PID  PRI  STATE  %%CPU   US%%   TIME+      NAME             ");
    vga_set_color(shell_get_state()->color);
    kprintf("\n");

    for (uint32_t i = 0; i < n && i < 11; i++) {
        process_t *p = process_at(i);
        if (!p) continue;

        /* The `idle` slot is conceptually always "running" — whenever it
         * accumulates ticks the CPU was literally executing HLT.  Showing
         * it as "RDY" while it has 100% CPU was confusing, so display
         * RUN here regardless of its bookkeeping state. */
        proc_state_t display_state = p->state;
        if (p->priority >= PRIO_IDLE && p->cpu_ticks > 0)
            display_state = PROC_RUNNING;

        uint8_t sc;
        switch (display_state) {
        case PROC_RUNNING:  sc = vga_entry_color(VGA_LIGHT_GREEN, VGA_BLACK); break;
        case PROC_READY:    sc = vga_entry_color(VGA_LIGHT_CYAN, VGA_BLACK);  break;
        case PROC_SLEEPING: sc = vga_entry_color(VGA_LIGHT_BROWN, VGA_BLACK); break;
        default:            sc = vga_entry_color(VGA_LIGHT_RED, VGA_BLACK);   break;
        }

        uint32_t pcpu      = (p->ticks_window * 100) / window_total;
        if (pcpu > 100) pcpu = 100;   /* clamp tiny rounding overshoot */
        uint32_t hz        = timer_get_freq();
        if (hz == 0) hz = 100;
        uint32_t total_sec = p->cpu_ticks / hz;
        uint32_t total_cs  = (p->cpu_ticks * 100 / hz) % 100;
        /* US%% = fraction of THIS process' total time spent in ring 3 */
        uint32_t us_pct = p->cpu_ticks
            ? (p->cpu_ticks_user * 100) / p->cpu_ticks : 0;

        /* PRI: print as signed; the idle task is shown as "idl" to make
         * it visually obvious that it isn't a user-tweakable value. */
        kprintf("  %-3u  ", p->pid);
        if (p->priority >= PRIO_IDLE) kprintf("idl  ");
        else                          kprintf("%3d  ", p->priority);

        /* Print the STATE column.  We use kprintf for the text so the VGA
         * cursor advances (and the column on serial gets the same text);
         * the colour highlight is painted on top of the cells we just
         * wrote, which is what vga_print_color_at_cursor() does.  Using
         * vga_print_color() alone wrote directly to the framebuffer
         * without moving the cursor, which is why the column appeared
         * EMPTY on the screen (it was correct on serial only). */
        const char *st = proc_state_str(display_state);
        uint8_t saved = vga_get_color();
        vga_set_color(sc);
        kprintf("%s", st);
        vga_set_color(saved);

        kprintf("  %3u%%  %3u%%  %u:%02u.%02u    %s\n",
                pcpu, us_pct,
                total_sec / 60, total_sec % 60, total_cs,
                p->name);
    }

    /* Reset the sliding %CPU window for every process so the NEXT call to
     * top_render() reports CPU usage for the next interval only. */
    for (uint32_t i = 0; i < n; i++) {
        process_t *p = process_at(i);
        if (p) p->ticks_window = 0;
    }
}

static int cmd_top(int argc, char **argv)
{
    (void)argc; (void)argv;

    for (;;) {
        top_render();
        /* hint at the bottom so users know how to exit even from a phone
         * keyboard where ESC/q might not be obvious */
        kprintf("\n  [q/ESC/Enter to quit, r to refresh]\n");

        /* poll the keyboard for ~2 seconds, then auto-refresh.
         * Accept many possible "quit" keys because some terminals (curses
         * on Termux, Android keyboards) don't always deliver q/ESC cleanly. */
        for (int t = 0; t < 200; t++) {
            int k = keyboard_trygetchar();
            if (k == 'q' || k == 'Q' || k == KEY_ESC ||
                k == '\n' || k == '\r' || k == 3 /* Ctrl-C */) {
                vga_clear();
                vga_set_cursor(0, 0);
                vga_set_color(shell_get_state()->color);
                return 0;
            }
            if (k == 'r' || k == 'R' || k == ' ')
                break;   /* refresh immediately */
            sleep(10);   /* 10 ms * 200 = ~2 s between auto-refreshes */
        }
    }
}

static int cmd_ps(int argc, char **argv)
{
    (void)argc; (void)argv;
    process_list();
    return 0;
}

static int cmd_kill(int argc, char **argv)
{
    if (argc < 2) { kprintf("usage: kill <pid>\n"); return 1; }
    int pid = atoi(argv[1]);
    int r = process_kill((uint32_t)pid);
    if (r == -1) kprintf("kill: cannot kill init (pid 1)\n");
    else if (r == -2) kprintf("kill: (%d) - No such process\n", pid);
    else kprintf("killed %d\n", pid);
    return 0;
}

/* renice <prio> <pid>  -- change the scheduling priority of a process.
 *
 * Priority follows the Unix `nice` convention:
 *     -20 = top priority (gets the CPU first; can starve others)
 *       0 = default
 *     +19 = bottom priority (only runs when nobody else wants to)
 *
 * Examples:
 *     renice -5 12        ; boost pid 12 a bit
 *     renice 10 7         ; ask pid 7 to be polite, let others run first   */
static int cmd_renice(int argc, char **argv)
{
    if (argc < 3) {
        kprintf("usage: renice <prio> <pid>\n");
        kprintf("  prio: %d .. %d (lower = more CPU)\n", PRIO_MIN, PRIO_MAX);
        return 1;
    }
    int prio = atoi(argv[1]);
    int pid  = atoi(argv[2]);
    int r = process_set_priority((uint32_t)pid, prio);
    if (r == -1)      kprintf("renice: (%d) - No such process\n", pid);
    else if (r == -2) kprintf("renice: prio %d out of range (%d..%d)\n",
                              prio, PRIO_MIN, PRIO_MAX);
    else              kprintf("pid %d: priority set to %d\n", pid, prio);
    return 0;
}

/* nice <prio> <cmd...>  -- run <cmd> at the requested priority.
 *
 * Trinux doesn't fork yet, so we can't change a "child" before exec
 * the way Unix does.  Instead we stash the desired priority globally:
 * the next call to process_create() (which is what `exec`, internal
 * commands like `top`, and the shell-spawned background tasks all use)
 * picks it up and clears it.  So a typical session looks like:
 *
 *     nice 10 exec heavy.elf      # heavy.elf starts at +10 (low priority)
 *     nice -5 exec render.elf     # render.elf starts at -5 (boosted)
 *
 * After the command runs, you can confirm with `ps` or `top`. */
static int cmd_nice(int argc, char **argv)
{
    if (argc < 3) {
        kprintf("usage: nice <prio> <cmd> [args...]\n");
        kprintf("  prio: %d .. %d (lower = more CPU)\n", PRIO_MIN, PRIO_MAX);
        return 1;
    }
    int prio = atoi(argv[1]);
    if (prio < PRIO_MIN || prio > PRIO_MAX) {
        kprintf("nice: prio %d out of range (%d..%d)\n",
                prio, PRIO_MIN, PRIO_MAX);
        return 1;
    }
    process_set_next_priority(prio);
    /* One short confirmation, useful to spot accidentally swapped args
     * (e.g. `nice 5` vs `nice -5`).  The actual ELF then loads right
     * after on the same line of output. */
    kprintf("nice: prio=%d  ", prio);
    /* Re-dispatch argv[2..] as a normal command line. */
    return commands_dispatch(argc - 2, &argv[2]);
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;
    kprintf("Rebooting...\n");
    /* pulse the 8042 keyboard controller reset line */
    uint8_t good = 0x02;
    while (good & 0x02)
        good = inb(0x64);
    outb(0x64, 0xFE);
    /* fallback: triple fault */
    __asm__ volatile("cli");
    for (;;) __asm__ volatile("hlt");
    return 0;
}

static int cmd_shutdown(int argc, char **argv)
{
    (void)argc; (void)argv;
    kprintf("System halted. It is now safe to power off.\n");
    /* QEMU ACPI shutdown via port 0x604 (newer) / 0xB004 */
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    __asm__ volatile("cli");
    for (;;) __asm__ volatile("hlt");
    return 0;
}

/* ---------- extras ---------- */

static int cmd_neofetch(int argc, char **argv)
{
    (void)argc; (void)argv;
    shell_state_t *s = shell_get_state();
    uint32_t total = pmm_get_total_memory();
    uint32_t used  = pmm_get_used_memory();

    const char *art[] = {
        "      .--.      ",
        "     |o_o |     ",
        "     |:_/ |     ",
        "    //   \\ \\    ",
        "   (|     | )   ",
        "  /'\\_   _/`\\   ",
        "  \\___)=(___/   ",
    };
    const char *info[16];
    static char l0[64], l1[64], l2[64], l3[64], l4[64], l5[64], l6[64], l7[64], l8[64];
    snprintf(l0, sizeof(l0), "%s@%s", s->user, s->hostname);
    snprintf(l1, sizeof(l1), "OS: %s %s", KERNEL_NAME, KERNEL_VERSION);
    snprintf(l2, sizeof(l2), "Arch: %s", KERNEL_ARCH);
    snprintf(l3, sizeof(l3), "Kernel: %s", KERNEL_BUILD);
    snprintf(l4, sizeof(l4), "Uptime: %u s", uptime());
    snprintf(l5, sizeof(l5), "Memory: %u/%u MB",
             used / (1024*1024), total / (1024*1024));
    /* Disk: show used (current fs image) / total, or RAM-only if no disk. */
    if (diskfs_available()) {
        uint32_t dtotal_mb = diskfs_total_mb();
        uint32_t dused     = diskfs_used_bytes();
        snprintf(l6, sizeof(l6), "Disk: %u MB used / %u MB",
                 dused / (1024*1024), dtotal_mb);
    } else {
        snprintf(l6, sizeof(l6), "Disk: none (RAM-only)");
    }
    snprintf(l7, sizeof(l7), "Shell: mysh");
    /* Battery */
    battery_info_t neo_bat;
    if (acpi_ec_read_battery(&neo_bat) && neo_bat.percentage != 0xFF) {
        snprintf(l8, sizeof(l8), "Battery: %u%% %s",
                 neo_bat.percentage,
                 neo_bat.charging ? "(charging)" :
                 neo_bat.discharging ? "(discharging)" :
                 neo_bat.ac_connected ? "(AC)" : "");
    } else {
        snprintf(l8, sizeof(l8), "Battery: none");
    }
    info[0]=l0; info[1]=l1; info[2]=l2; info[3]=l3;
    info[4]=l4; info[5]=l5; info[6]=l6; info[7]=l7;
    info[8]=l8;

    int art_lines = (int)ARRAY_LEN(art);
    for (int i = 0; i < 9; i++) {
        /* pad with blanks once we run past the ASCII art so info keeps aligned */
        if (i < art_lines)
            vga_print_color(art[i], vga_entry_color(VGA_LIGHT_CYAN, VGA_BLACK));
        else
            kprintf("                ");   /* 16 spaces = art width */
        kprintf("  ");
        vga_print_color(info[i], vga_entry_color(VGA_LIGHT_GREEN, VGA_BLACK));
        kprintf("\n");
    }
    return 0;
}

/* tiny expression calculator: supports + - * / with two operands, left-assoc
 * chaining e.g. calc 2 + 3 * 4 evaluated left to right (no precedence). */
static int cmd_calc(int argc, char **argv)
{
    if (argc < 4) { kprintf("usage: calc <a> <op> <b> [op c ...]\n"); return 1; }
    int acc = atoi(argv[1]);
    int i = 2;
    while (i + 1 < argc) {
        const char *op = argv[i];
        int b = atoi(argv[i + 1]);
        if (strcmp(op, "+") == 0) acc += b;
        else if (strcmp(op, "-") == 0) acc -= b;
        else if (strcmp(op, "*") == 0 || strcmp(op, "x") == 0) acc *= b;
        else if (strcmp(op, "/") == 0) {
            if (b == 0) { kprintf("calc: division by zero\n"); return 1; }
            acc /= b;
        } else { kprintf("calc: unknown operator '%s'\n", op); return 1; }
        i += 2;
    }
    kprintf("= %d\n", acc);
    return 0;
}

static int cmd_hexdump(int argc, char **argv)
{
    if (argc < 2) { kprintf("usage: hexdump <file>\n"); return 1; }
    vfs_node_t *node = resolve_arg(argv[1]);
    if (!node || node->type != VFS_FILE) { kprintf("hexdump: %s: not a file\n", argv[1]); return 1; }
    char buf[READ_BUF];
    uint32_t total = vfs_read(node, 0, sizeof(buf), (uint8_t *)buf);
    for (uint32_t off = 0; off < total; off += 16) {
        kprintf("%08x  ", off);
        for (uint32_t i = 0; i < 16; i++) {
            if (off + i < total)
                kprintf("%02x ", (uint8_t)buf[off + i]);
            else
                kprintf("   ");
        }
        kprintf(" |");
        for (uint32_t i = 0; i < 16 && off + i < total; i++) {
            char c = buf[off + i];
            vga_putchar((c >= 32 && c < 127) ? c : '.');
        }
        kprintf("|\n");
    }
    return 0;
}

static int parse_color_name(const char *name)
{
    struct { const char *n; int v; } map[] = {
        {"black",VGA_BLACK},{"blue",VGA_BLUE},{"green",VGA_GREEN},
        {"cyan",VGA_CYAN},{"red",VGA_RED},{"magenta",VGA_MAGENTA},
        {"brown",VGA_BROWN},{"grey",VGA_LIGHT_GREY},{"gray",VGA_LIGHT_GREY},
        {"darkgrey",VGA_DARK_GREY},{"lightblue",VGA_LIGHT_BLUE},
        {"lightgreen",VGA_LIGHT_GREEN},{"lightcyan",VGA_LIGHT_CYAN},
        {"lightred",VGA_LIGHT_RED},{"yellow",VGA_LIGHT_BROWN},
        {"white",VGA_WHITE},
    };
    for (unsigned i = 0; i < ARRAY_LEN(map); i++)
        if (strcmp(name, map[i].n) == 0) return map[i].v;
    return -1;
}

static int cmd_color(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("usage: color <fg> [bg]\n");
        kprintf("colors: black blue green cyan red magenta brown grey\n");
        kprintf("        darkgrey lightblue lightgreen lightcyan lightred\n");
        kprintf("        yellow white\n");
        return 1;
    }
    int fg = parse_color_name(argv[1]);
    int bg = (argc > 2) ? parse_color_name(argv[2]) : VGA_BLACK;
    if (fg < 0 || bg < 0) { kprintf("color: unknown color\n"); return 1; }
    uint8_t c = vga_entry_color((enum vga_color)fg, (enum vga_color)bg);
    vga_set_color(c);
    shell_get_state()->color = c;
    return 0;
}

static int cmd_history(int argc, char **argv)
{
    (void)argc; (void)argv;
    int n = shell_history_count();
    for (int i = 0; i < n; i++)
        kprintf("  %3d  %s\n", i + 1, shell_history_get(i));
    return 0;
}

static int cmd_alias(int argc, char **argv)
{
    if (argc < 2) {
        for (int i = 0; i < alias_count; i++)
            kprintf("  %s='%s'\n", aliases[i].name, aliases[i].value);
        return 0;
    }
    if (argc < 3) { kprintf("usage: alias <name> <command>\n"); return 1; }
    /* build value from remaining args */
    char value[128] = "";
    for (int i = 2; i < argc; i++) {
        strncat(value, argv[i], sizeof(value) - strlen(value) - 2);
        if (i < argc - 1) strncat(value, " ", sizeof(value) - strlen(value) - 1);
    }
    /* replace if exists */
    for (int i = 0; i < alias_count; i++) {
        if (strcmp(aliases[i].name, argv[1]) == 0) {
            strncpy(aliases[i].value, value, sizeof(aliases[i].value) - 1);
            return 0;
        }
    }
    if (alias_count < SHELL_MAX_ALIAS) {
        strncpy(aliases[alias_count].name, argv[1], sizeof(aliases[0].name) - 1);
        strncpy(aliases[alias_count].value, value, sizeof(aliases[0].value) - 1);
        alias_count++;
    }
    return 0;
}

/* ---------- disk persistence ---------- */

static int cmd_sync(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!diskfs_available()) {
        kprintf("sync: no disk attached; filesystem is RAM-only.\n");
        kprintf("      (run QEMU with a -drive image to enable persistence)\n");
        return 1;
    }
    int r = diskfs_save();
    if (r == 0) {
        vga_set_color(vga_entry_color(VGA_LIGHT_GREEN, VGA_BLACK));
        kprintf("Filesystem written to disk. Changes will survive reboot.\n");
        vga_set_color(shell_get_state()->color);
        return 0;
    }
    kprintf("sync: failed to write filesystem (error %d)\n", r);
    return 1;
}

/* ---------- dd: convert and copy a file (Linux-style) ----------
 *
 * Supported operands (all of the classic ones that make sense here):
 *
 *   if=FILE        input file/device  (default: read from pipe stdin)
 *   of=FILE        output file/device (default: write to terminal)
 *   bs=N           block size in bytes (default 512). Accepts suffixes:
 *                  c=1, w=2, b=512, k/K=1024, M=1024*1024
 *   ibs=N, obs=N   separate input/output block sizes (override bs=)
 *   count=N        copy at most N input blocks (default: until EOF)
 *   skip=N         skip N ibs-sized blocks at the start of input
 *   seek=N         seek N obs-sized blocks at the start of output
 *   conv=LIST      comma-separated: notrunc, sync, noerror
 *                    notrunc - don't truncate output before writing
 *                    sync    - pad each input block with zeros to ibs
 *                    noerror - keep going past short reads
 *   status=LEVEL   none | noxfer | progress (progress prints a live counter)
 *
 * Examples:
 *   dd if=/dev/zero of=/tmp/big.bin bs=1K count=64
 *   dd if=/dev/sda  of=/tmp/mbr.bin bs=512 count=1
 *   dd if=hello.txt of=/dev/null
 *   dd if=/dev/random of=/tmp/noise bs=16 count=4
 */

/* Parse a size like "512", "4K", "2M", "1b". Returns 0 on parse error. */
static uint32_t dd_parse_size(const char *s)
{
    if (!s || !*s) return 0;
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (uint32_t)(*s - '0');
        s++;
    }
    /* unit suffix */
    switch (*s) {
        case 0:  return v;
        case 'c': return v * 1u;
        case 'w': return v * 2u;
        case 'b': return v * 512u;
        case 'k': case 'K': return v * 1024u;
        case 'M': return v * 1024u * 1024u;
        default:  return 0;
    }
}

static bool dd_str_startswith(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s++ != *prefix++) return false;
    }
    return true;
}

static int cmd_dd(int argc, char **argv)
{
    const char *if_path = NULL, *of_path = NULL;
    uint32_t ibs = 512, obs = 512;
    bool bs_set = false, ibs_set = false, obs_set = false;
    uint32_t count = 0;            /* 0 == unlimited */
    uint32_t skip = 0, seek = 0;
    bool conv_notrunc = false, conv_sync = false, conv_noerror = false;
    enum { ST_DEFAULT, ST_NONE, ST_NOXFER, ST_PROGRESS } status = ST_DEFAULT;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (dd_str_startswith(a, "if=")) {
            if_path = a + 3;
        } else if (dd_str_startswith(a, "of=")) {
            of_path = a + 3;
        } else if (dd_str_startswith(a, "bs=")) {
            uint32_t v = dd_parse_size(a + 3);
            if (v == 0) { kprintf("dd: invalid bs=%s\n", a + 3); return 1; }
            ibs = obs = v; bs_set = true;
        } else if (dd_str_startswith(a, "ibs=")) {
            uint32_t v = dd_parse_size(a + 4);
            if (v == 0) { kprintf("dd: invalid ibs=%s\n", a + 4); return 1; }
            ibs = v; ibs_set = true;
        } else if (dd_str_startswith(a, "obs=")) {
            uint32_t v = dd_parse_size(a + 4);
            if (v == 0) { kprintf("dd: invalid obs=%s\n", a + 4); return 1; }
            obs = v; obs_set = true;
        } else if (dd_str_startswith(a, "count=")) {
            count = (uint32_t)atoi(a + 6);
        } else if (dd_str_startswith(a, "skip=")) {
            skip = (uint32_t)atoi(a + 5);
        } else if (dd_str_startswith(a, "seek=")) {
            seek = (uint32_t)atoi(a + 5);
        } else if (dd_str_startswith(a, "conv=")) {
            const char *p = a + 5;
            char tok[32]; int ti = 0;
            for (;; p++) {
                if (*p == ',' || *p == 0) {
                    tok[ti] = 0; ti = 0;
                    if (strcmp(tok, "notrunc") == 0) conv_notrunc = true;
                    else if (strcmp(tok, "sync") == 0) conv_sync = true;
                    else if (strcmp(tok, "noerror") == 0) conv_noerror = true;
                    else if (tok[0]) {
                        kprintf("dd: unknown conv=%s\n", tok); return 1;
                    }
                    if (*p == 0) break;
                } else if (ti < (int)sizeof(tok) - 1) {
                    tok[ti++] = *p;
                }
            }
        } else if (dd_str_startswith(a, "status=")) {
            const char *v = a + 7;
            if (strcmp(v, "none") == 0)         status = ST_NONE;
            else if (strcmp(v, "noxfer") == 0)  status = ST_NOXFER;
            else if (strcmp(v, "progress") == 0) status = ST_PROGRESS;
            else { kprintf("dd: bad status=%s\n", v); return 1; }
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            kprintf("usage: dd [if=FILE] [of=FILE] [bs=N] [count=N] "
                    "[skip=N] [seek=N] [conv=notrunc,sync,noerror] "
                    "[status=progress]\n");
            return 0;
        } else {
            kprintf("dd: unrecognized operand '%s'\n", a);
            return 1;
        }
    }
    (void)bs_set; (void)ibs_set; (void)obs_set;

    /* ---- open input ---- */
    vfs_node_t *in_node  = NULL;
    uint32_t    in_off   = 0;
    shell_state_t *s = shell_get_state();
    const uint8_t *pipe_buf = NULL;
    uint32_t       pipe_len = 0;

    if (if_path) {
        in_node = resolve_arg(if_path);
        if (!in_node) {
            kprintf("dd: %s: No such file or directory\n", if_path); return 1;
        }
        if (in_node->type == VFS_DIRECTORY) {
            kprintf("dd: %s: Is a directory\n", if_path); return 1;
        }
        if (!vfs_check_access(in_node, ACC_READ)) {
            kprintf("dd: %s: Permission denied\n", if_path); return 1;
        }
        in_off = skip * ibs;
    } else if (s->pipe_in) {
        pipe_buf = (const uint8_t *)s->pipe_in;
        pipe_len = s->pipe_in_len;
        if (skip * ibs >= pipe_len) pipe_len = 0;
        else { pipe_buf += skip * ibs; pipe_len -= skip * ibs; }
    } else {
        kprintf("dd: no input (use if=FILE or pipe into dd)\n"); return 1;
    }

    /* ---- open output ---- */
    vfs_node_t *out_node = NULL;
    uint32_t    out_off  = 0;
    if (of_path) {
        out_node = resolve_arg(of_path);
        if (!out_node) {
            /* create it */
            out_node = vfs_create(of_path, s->cwd);
            if (!out_node) {
                kprintf("dd: cannot create '%s'\n", of_path); return 1;
            }
        }
        if (out_node->type == VFS_DIRECTORY) {
            kprintf("dd: %s: Is a directory\n", of_path); return 1;
        }
        if (!vfs_check_access(out_node, ACC_WRITE)) {
            kprintf("dd: %s: Permission denied\n", of_path); return 1;
        }
        /* Regular files: truncate unless conv=notrunc. Devices keep their
         * "size" field as a capacity hint, so we never touch it. */
        if (!conv_notrunc && out_node->type == VFS_FILE) {
            out_node->size = 0;
        }
        out_off = seek * obs;
    }

    /* Use a heap buffer so we can support large bs= (e.g. 1M). */
    uint8_t *buf = (uint8_t *)kmalloc(ibs);
    if (!buf) { kprintf("dd: out of memory\n"); return 1; }

    /* ---- copy loop ---- */
    uint32_t blocks_in_full = 0, blocks_in_part = 0;
    uint32_t blocks_out_full = 0, blocks_out_part = 0;
    uint32_t bytes_total = 0;
    uint32_t t_start = uptime();
    uint32_t next_progress = t_start + 1;

    for (uint32_t b = 0; count == 0 || b < count; b++) {
        uint32_t got;
        if (in_node) {
            got = vfs_read(in_node, in_off, ibs, buf);
            in_off += got;
        } else {
            got = (pipe_len > ibs) ? ibs : pipe_len;
            if (got) memcpy(buf, pipe_buf, got);
            pipe_buf += got; pipe_len -= got;
        }
        if (got == 0) break;        /* EOF */

        bool partial = (got < ibs);
        if (partial) {
            if (conv_sync) { memset(buf + got, 0, ibs - got); got = ibs; }
            if (!conv_noerror && got < ibs && !conv_sync) {
                /* still write what we got, then exit the loop normally */
            }
        }
        if (partial && (conv_sync || got == ibs)) blocks_in_full++;
        else if (partial) blocks_in_part++;
        else blocks_in_full++;

        /* write ---- */
        uint32_t left = got, src = 0;
        while (left > 0) {
            uint32_t chunk = (left > obs) ? obs : left;
            uint32_t wrote;
            if (out_node) {
                wrote = vfs_write(out_node, out_off, chunk, buf + src);
                out_off += wrote;
                if (out_node->type == VFS_FILE && out_off > out_node->size)
                    out_node->size = out_off;
            } else {
                /* stdout: emit to terminal */
                for (uint32_t i = 0; i < chunk; i++)
                    vga_putchar((char)buf[src + i]);
                wrote = chunk;
            }
            if (wrote == 0) {
                kprintf("\ndd: write error at offset %u\n", out_off);
                kfree(buf);
                return 1;
            }
            if (wrote < chunk) blocks_out_part++;
            else if (chunk == obs) blocks_out_full++;
            else blocks_out_part++;
            bytes_total += wrote;
            left -= wrote; src += wrote;
        }

        /* live progress */
        if (status == ST_PROGRESS && uptime() >= next_progress) {
            kprintf("\r%u bytes copied", bytes_total);
            next_progress = uptime() + 1;
        }

        if (partial && !conv_noerror) break;   /* short read -> stop */
    }

    kfree(buf);

    /* ---- final report ---- */
    if (status != ST_NONE) {
        if (status == ST_PROGRESS) kprintf("\n");
        kprintf("%u+%u records in\n",  blocks_in_full,  blocks_in_part);
        kprintf("%u+%u records out\n", blocks_out_full, blocks_out_part);
        if (status != ST_NOXFER) {
            uint32_t secs = uptime() - t_start;
            if (secs == 0) secs = 1;
            kprintf("%u bytes copied, %u s, %u B/s\n",
                    bytes_total, secs, bytes_total / secs);
        }
    }
    return 0;
}

/* usertest - launch a ring-3 (userspace) program. It runs unprivileged and
 * can only reach the kernel via int 0x80 syscalls. `usertest bad` runs a
 * program that tries a privileged instruction to prove isolation works. */
static int cmd_usertest(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "bad") == 0) {
        kprintf("Launching ring-3 program that violates privilege...\n");
        usermode_run("badboy", userprog_badboy);
        return 0;
    }
    kprintf("Dropping to ring 3 and running a userspace demo program.\n");
    kprintf("It talks to the kernel ONLY through int 0x80 syscalls.\n");
    int pid = usermode_run("usertest", userprog_main);
    vga_set_color(vga_entry_color(VGA_LIGHT_GREEN, VGA_BLACK));
    kprintf("\n  [ring0] User program launched as PID %d (runs asynchronously)\n", pid);
    vga_set_color(shell_get_state()->color);
    return 0;
}

/* asm <source.asm> [output] — assemble x86 ASM into an ELF binary */
static int cmd_asm(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("usage: asm <source.asm> [output]\n\n");
        kprintf("  Assembles x86 assembly into an ELF32 executable.\n");
        kprintf("  If no output is given, removes .asm extension.\n\n");
        kprintf("  Example:\n");
        kprintf("    edit hello.asm    (write your code)\n");
        kprintf("    asm hello.asm     (creates 'hello')\n");
        kprintf("    exec hello        (run it)\n\n");
        kprintf("  Supported: mov, add, sub, cmp, and, or, xor,\n");
        kprintf("    push, pop, inc, dec, int, call, jmp, je, jne,\n");
        kprintf("    ret, nop, hlt, db, labels (name:)\n");
        return 1;
    }

    char out_path[128];
    if (argc >= 3) {
        strncpy(out_path, argv[2], sizeof(out_path) - 1);
    } else {
        /* Remove .asm extension */
        strncpy(out_path, argv[1], sizeof(out_path) - 1);
        out_path[sizeof(out_path) - 1] = '\0';
        int l = (int)strlen(out_path);
        if (l > 4 && strcmp(out_path + l - 4, ".asm") == 0)
            out_path[l - 4] = '\0';
    }

    return tasm_assemble(argv[1], out_path);
}

/* tcc <source.c> [output] — compile C into an ELF binary */
static int cmd_tcc(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("usage: tcc <source.c> [output]\n\n");
        kprintf("  Compiles a C program into an ELF32 executable.\n");
        kprintf("  If no output is given, removes .c extension.\n\n");
        kprintf("  Built-in functions:\n");
        kprintf("    print(str)           print_num(n)\n");
        kprintf("    print_char(c)        getchar()\n");
        kprintf("    sleep(ms)            uptime()      getpid()\n");
        kprintf("    exit(code)           vga_clear(color)\n");
        kprintf("    vga_putchar(x,y,c,color)\n");
        kprintf("    vga_print(x,y,str,color)\n");
        return 1;
    }
    char out_path[128];
    if (argc >= 3) {
        strncpy(out_path, argv[2], sizeof(out_path) - 1);
    } else {
        strncpy(out_path, argv[1], sizeof(out_path) - 1);
        out_path[sizeof(out_path) - 1] = '\0';
        int l = (int)strlen(out_path);
        if (l > 2 && strcmp(out_path + l - 2, ".c") == 0)
            out_path[l - 2] = '\0';
    }
    return tcc_compile(argv[1], out_path);
}

/* exec <file> — load and run an ELF32 binary from the filesystem in ring 3 */
static int cmd_exec(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("usage: exec <elf-binary>\n");
        kprintf("  Loads an ELF32 i386 executable from the filesystem\n");
        kprintf("  and runs it in ring 3 (userspace).\n\n");
        kprintf("  To create a program:\n");
        kprintf("    1. Write C code using only int 0x80 syscalls\n");
        kprintf("    2. Compile: i686-elf-gcc -ffreestanding -nostdlib \\\n");
        kprintf("         -static -Wl,-Ttext=0x08048000 -o app app.c\n");
        kprintf("    3. Copy to Trinux filesystem (via dd or at build time)\n");
        kprintf("    4. Run: exec /path/to/app\n");
        return 1;
    }
    shell_state_t *s = shell_get_state();
    return elf_exec(argv[1], s->cwd);
}

static int cmd_battery(int argc, char **argv)
{
    /* battery dump — show all EC registers (diagnostic) */
    if (argc > 1 && strcmp(argv[1], "dump") == 0) {
        if (!acpi_ec_available()) {
            kprintf("EC not accessible.\n");
            return 1;
        }
        kprintf("EC register dump (0x00-0xFF):\n");
        kprintf("     00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F\n");
        for (uint32_t row = 0; row < 16; row++) {
            kprintf("%02x: ", row * 16);
            for (uint32_t col = 0; col < 16; col++) {
                int v = acpi_ec_read_register((uint8_t)(row * 16 + col));
                if (v >= 0)
                    kprintf("%02x ", v);
                else
                    kprintf("?? ");
            }
            kprintf("\n");
        }
        kprintf("\nLook for your battery percentage (e.g. if battery is ~60%%,\n"
                "find a register with value 0x3C = 60).\n"
                "Then tell me which register and I'll fix the driver.\n");
        return 0;
    }

    /* battery reset — force re-detection of EC layout */
    if (argc > 1 && strcmp(argv[1], "reset") == 0) {
        acpi_ec_reset_layout();
        kprintf("Battery layout reset. Run 'battery' to re-detect.\n");
        return 0;
    }

    battery_info_t bat;
    if (!acpi_ec_read_battery(&bat)) {
        kprintf("No battery detected (desktop PC or EC not accessible).\n");
        kprintf("Try 'battery dump' to see raw EC registers.\n");
        return 1;
    }

    kprintf("Battery:\n");
    if (bat.percentage != 0xFF)
        kprintf("  Level:   %u%%\n", bat.percentage);
    else
        kprintf("  Level:   unknown\n");

    kprintf("  Status:  %s\n",
            bat.charging ? "Charging" :
            bat.discharging ? "Discharging" :
            bat.ac_connected ? "AC (full)" : "Unknown");
    kprintf("  AC:      %s\n", bat.ac_connected ? "Connected" : "Not connected");

    if (bat.voltage_mv)
        kprintf("  Voltage: %u.%03u V\n", bat.voltage_mv / 1000,
                bat.voltage_mv % 1000);
    if (bat.remain_mah)
        kprintf("  Remain:  %u mAh\n", bat.remain_mah);
    if (bat.full_mah)
        kprintf("  Full:    %u mAh\n", bat.full_mah);
    if (bat.rate_ma)
        kprintf("  Rate:    %u mA\n", bat.rate_ma);
    return 0;
}

static int cmd_help(int argc, char **argv);   /* fwd */

/* ---------- command table ---------- */

static const command_t table[] = {
    {"ls",       cmd_ls,       "list directory contents (-l for details)"},
    {"cd",       cmd_cd,       "change directory (cd .., cd /, cd ~)"},
    {"pwd",      cmd_pwd,      "print working directory"},
    {"mkdir",    cmd_mkdir,    "create a directory"},
    {"rmdir",    cmd_rmdir,    "remove an empty directory"},
    {"touch",    cmd_touch,    "create an empty file"},
    {"rm",       cmd_rm,       "remove a file"},
    {"cat",      cmd_cat,      "print file contents"},
    {"echo",     cmd_echo,     "print text (supports > file)"},
    {"write",    cmd_write,    "write text to a file"},
    {"edit",     cmd_edit,     "full-screen text editor (^S save, ^X exit)"},
    {"nano",     cmd_edit,     "full-screen text editor (alias of edit)"},
    {"cp",       cmd_cp,       "copy a file"},
    {"mv",       cmd_mv,       "move/rename a file"},
    {"stat",     cmd_stat,     "show file/dir info"},
    {"chmod",    cmd_chmod,    "change permissions (chmod 644 file)"},
    {"chown",    cmd_chown,    "change owner (chown user file, root only)"},
    {"tree",     cmd_tree,     "print directory tree"},
    {"find",     cmd_find,     "find files by name (recursive)"},
    {"head",     cmd_head,     "show first n lines"},
    {"tail",     cmd_tail,     "show last n lines"},
    {"wc",       cmd_wc,       "count lines, words, chars (also via pipe)"},
    {"grep",     cmd_grep,     "filter lines matching a pattern (-ivnc)"},
    {"sort",     cmd_sort,     "sort lines (-r reverse, -u unique, -n numeric)"},
    {"uniq",     cmd_uniq,     "drop adjacent duplicate lines (-c count)"},
    {"cut",      cmd_cut,      "select fields/chars (cut -d: -f1, cut -c3)"},
    {"tee",      cmd_tee,      "copy stdin to a file and the screen (-a append)"},
    {"seq",      cmd_seq,      "print a number sequence (seq 1 5)"},
    {"basename", cmd_basename, "strip directory from a path"},
    {"dirname",  cmd_dirname,  "strip last component from a path"},
    {"which",    cmd_which,    "locate a built-in command"},
    {"env",      cmd_env,      "show environment-like values"},
    {"yes",      cmd_yes,      "repeat a string (bounded to 100 lines)"},
    {"umask",    cmd_umask,    "show/set default permission mask"},
    {"sync",     cmd_sync,     "save the filesystem to disk (persist)"},
    {"dd",       cmd_dd,       "convert and copy (dd if=/dev/zero of=f bs=1K count=4)"},
    {"clear",    cmd_clear,    "clear the screen"},
    {"cls",      cmd_clear,    "clear the screen"},
    {"uname",    cmd_uname,    "system info (-a for all)"},
    {"uptime",   cmd_uptime,   "time since boot"},
    {"date",     cmd_date,     "current date and time (RTC)"},
    {"whoami",   cmd_whoami,   "print current user"},
    {"id",       cmd_id,       "show user/group ids"},
    {"users",    cmd_users,    "list all accounts"},
    {"groups",   cmd_groups,   "show current user's groups"},
    {"login",    cmd_login,    "log in as a user"},
    {"logout",   cmd_logout,   "log out (back to login)"},
    {"su",       cmd_su,       "switch user (su [name], default root)"},
    {"useradd",  cmd_useradd,  "create a user (root only)"},
    {"passwd",   cmd_passwd,   "change a password"},
    {"hostname", cmd_hostname, "show/set hostname"},
    {"free",     cmd_free,     "memory usage"},
    {"df",       cmd_df,       "disk space usage"},
    {"ps",       cmd_ps,       "list processes"},
    {"top",      cmd_top,      "live system/process monitor (q to quit)"},
    {"kill",     cmd_kill,     "kill a process by pid"},
    {"nice",     cmd_nice,     "run command at given priority: nice <prio> <cmd>"},
    {"renice",   cmd_renice,   "change priority of running pid: renice <prio> <pid>"},
    {"reboot",   cmd_reboot,   "reboot the machine"},
    {"shutdown", cmd_shutdown, "power off"},
    {"halt",     cmd_shutdown, "power off"},
    {"neofetch", cmd_neofetch, "system info with ASCII art"},
    {"calc",     cmd_calc,     "calculator: calc 2 + 3"},
    {"hexdump",  cmd_hexdump,  "hex dump of a file"},
    {"color",    cmd_color,    "change terminal colors"},
    {"history",  cmd_history,  "show command history"},
    {"alias",    cmd_alias,    "create command aliases"},
    {"usertest", cmd_usertest, "run a ring-3 userspace program (int 0x80 demo)"},
    {"exec",     cmd_exec,     "load and run an ELF32 binary from the filesystem"},
    {"asm",      cmd_asm,      "assemble x86 ASM source into ELF binary"},
    {"tcc",      cmd_tcc,      "compile C source into ELF binary"},
    {"battery",  cmd_battery,  "show battery status (laptop only)"},
    {"help",     cmd_help,     "show this help"},
};

static int cmd_help(int argc, char **argv)
{
    /* help <cmd>: show detailed info for a single command. */
    if (argc > 1) {
        for (unsigned i = 0; i < ARRAY_LEN(table); i++) {
            if (strcmp(table[i].name, argv[1]) == 0) {
                vga_set_color(vga_entry_color(VGA_LIGHT_CYAN, VGA_BLACK));
                kprintf("%s", table[i].name);
                vga_set_color(shell_get_state()->color);
                kprintf(" - %s\n", table[i].help);
                return 0;
            }
        }
        kprintf("help: no help topic for '%s'. Type 'help' for the list.\n",
                argv[1]);
        return 1;
    }

    vga_set_color(vga_entry_color(VGA_LIGHT_CYAN, VGA_BLACK));
    kprintf("Available commands (try 'help <command>' for details):\n");
    vga_set_color(shell_get_state()->color);
    /* print 3 per row to fit the 80-col screen */
    int col = 0;
    for (unsigned i = 0; i < ARRAY_LEN(table); i++) {
        kprintf("  %-12s", table[i].name);
        if (++col == 4) { kprintf("\n"); col = 0; }
    }
    if (col != 0) kprintf("\n");
    return 0;
}

const command_t *commands_table(void) { return table; }
int commands_count(void) { return (int)ARRAY_LEN(table); }

int commands_dispatch(int argc, char **argv)
{
    if (argc == 0)
        return 0;

    /* Register this command as a transient process so `ps`/`top` show
     * something meaningful. We mark it RUNNING during execution and ZOMBIE
     * on return; process_create() reuses ZOMBIE slots so the table never
     * grows. Use the "tracking" variant so a pending `nice` hint passes
     * through this placeholder and lands on the real ELF that the user
     * actually wanted to renice (e.g. `nice -5 exec /bin/foo`). */
    process_t *cmd_proc = process_create_tracking(argv[0]);
    if (cmd_proc) cmd_proc->state = PROC_RUNNING;

    /* alias expansion (single level) */
    for (int i = 0; i < alias_count; i++) {
        if (strcmp(aliases[i].name, argv[0]) == 0) {
            /* re-tokenize alias value + remaining args */
            static char expanded[256];
            snprintf(expanded, sizeof(expanded), "%s", aliases[i].value);
            char *new_argv[SHELL_MAX_ARGS];
            int new_argc = 0;
            char *p = expanded;
            while (*p && new_argc < SHELL_MAX_ARGS - 1) {
                while (*p == ' ') p++;
                if (!*p) break;
                new_argv[new_argc++] = p;
                while (*p && *p != ' ') p++;
                if (*p) *p++ = '\0';
            }
            /* append original args (skip argv[0]) */
            for (int j = 1; j < argc && new_argc < SHELL_MAX_ARGS - 1; j++)
                new_argv[new_argc++] = argv[j];
            new_argv[new_argc] = NULL;
            /* prevent infinite recursion: only dispatch if not same name */
            if (new_argc > 0 && strcmp(new_argv[0], argv[0]) != 0) {
                int rc = commands_dispatch(new_argc, new_argv);
                if (cmd_proc) cmd_proc->state = PROC_ZOMBIE;
                return rc;
            }
            break;
        }
    }

    /* === Ring-3 routing ===
     * Antes de invocar el built-in del kernel, miramos si existe
     * /bin/<argv[0]>.  Si sí, lo ejecutamos como ELF en ring 3 vía
     * elf_exec_argv() — pasándole argv completo.  Esto reproduce el
     * comportamiento de Linux: `ls` se resuelve a /bin/ls y corre
     * unprivileged, no como código del kernel.
     *
     * Hay built-ins que NO podemos delegar a ring 3 porque cambian
     * estado del propio shell (cwd, alias, history, login).  Esos los
     * marcamos como "internal-only" y se quedan en kernel mode. */
    static const char *internal_only[] = {
        "cd", "alias", "history", "logout", "su", "login", "exit",
        "exec", "tcc", "asm", "edit", "color", "umask",
        "usertest", "neofetch",  /* requieren contexto compartido */
        NULL
    };
    int is_internal = 0;
    for (int k = 0; internal_only[k]; k++)
        if (strcmp(argv[0], internal_only[k]) == 0) { is_internal = 1; break; }

    if (!is_internal) {
        /* Construye "/bin/<cmd>" y prueba si existe como ELF. */
        char bin_path[80];
        snprintf(bin_path, sizeof(bin_path), "/bin/%s", argv[0]);
        shell_state_t *s = shell_get_state();
        vfs_node_t *bin_node = vfs_resolve(bin_path, s ? s->cwd : NULL);
        if (bin_node && bin_node->type == VFS_FILE && bin_node->size > 0) {
            int rc = elf_exec_argv(bin_path, s ? s->cwd : NULL, argc, argv);
            if (cmd_proc) cmd_proc->state = PROC_ZOMBIE;
            return rc;
        }
    }

    /* Fallback: built-ins del kernel. */
    for (unsigned i = 0; i < ARRAY_LEN(table); i++) {
        if (strcmp(argv[0], table[i].name) == 0) {
            int rc = table[i].fn(argc, argv);
            if (cmd_proc) cmd_proc->state = PROC_ZOMBIE;
            return rc;
        }
    }
    /* Not a built-in.  Si parece path (/ o ./), ejecutarlo como ELF. */
    if (argv[0][0] == '/' ||
        (argv[0][0] == '.' && argv[0][1] == '/')) {
        shell_state_t *s = shell_get_state();
        int rc = elf_exec_argv(argv[0], s->cwd, argc, argv);
        if (cmd_proc) cmd_proc->state = PROC_ZOMBIE;
        return rc;
    }

    if (cmd_proc) cmd_proc->state = PROC_ZOMBIE;
    return -1;   /* unknown */
}

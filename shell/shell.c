/* shell/shell.c  -  interactive shell: prompt, line editor, history, dispatch. */
#include "shell.h"
#include "commands.h"
#include "../drivers/vga.h"
#include "../drivers/keyboard.h"
#include "../fs/vfs.h"
#include "../fs/path.h"
#include "../auth/users.h"
#include "../lib/string.h"
#include "../lib/printf.h"
#include "../include/kernel.h"

static shell_state_t state;

/* ---- command history ---- */
static char history[SHELL_HISTORY][SHELL_LINE_MAX];
static int  history_count;
static int  history_head;   /* next write slot (circular) */

shell_state_t *shell_get_state(void) { return &state; }

void shell_add_history(const char *line)
{
    if (line[0] == '\0')
        return;
    /* avoid consecutive duplicates */
    if (history_count > 0) {
        int last = (history_head - 1 + SHELL_HISTORY) % SHELL_HISTORY;
        if (strcmp(history[last], line) == 0)
            return;
    }
    strncpy(history[history_head], line, SHELL_LINE_MAX - 1);
    history[history_head][SHELL_LINE_MAX - 1] = '\0';
    history_head = (history_head + 1) % SHELL_HISTORY;
    if (history_count < SHELL_HISTORY)
        history_count++;
}

int shell_history_count(void) { return history_count; }

/* index 0 = oldest */
const char *shell_history_get(int index)
{
    if (index < 0 || index >= history_count)
        return NULL;
    int start = (history_head - history_count + SHELL_HISTORY * 2) % SHELL_HISTORY;
    return history[(start + index) % SHELL_HISTORY];
}

/* ---- prompt ---- */
static void print_prompt(void)
{
    char path[PATH_MAX];
    vfs_get_path(state.cwd, path);

    user_t *u = current_user();
    const char *name = u ? u->name : state.user;
    bool root = is_root();

    /* root shows in red, normal users in green; '#' vs '$' like Unix. */
    vga_set_color(vga_entry_color(root ? VGA_LIGHT_RED : VGA_LIGHT_GREEN,
                                  VGA_BLACK));
    kprintf("%s@%s", name, state.hostname);
    vga_set_color(vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));
    kprintf(":");
    vga_set_color(vga_entry_color(VGA_LIGHT_BLUE, VGA_BLACK));
    kprintf("%s", path);
    vga_set_color(vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));
    kprintf("%s ", root ? "#" : "$");
}

/* Clean line editor: maintains buffer + cursor, supports backspace, left/right,
 * history up/down, and printable insertion. */
/* ----------------- Tab completion ---------------------------------------
 *
 * When the user hits Tab we look at the line up to the cursor and try to
 * complete the last partial word. There are two modes:
 *
 *   - Command completion: the cursor is in the first word of the line.
 *     We match against the built-in command table (`commands_table()`).
 *
 *   - Path completion: the cursor is past a space, so we're completing
 *     an argument. We split the partial path into directory + prefix and
 *     list directory entries whose name starts with that prefix.
 *
 * On a single match we splice it into the buffer (adding a space for
 * commands, '/' for directories). On multiple matches we print them on a
 * new line and re-render the prompt + current input.
 */

/* shared scratch for collected matches */
typedef struct {
    const char *name;
    bool        is_dir;
} compl_match_t;

static int collect_command_matches(const char *prefix, compl_match_t *out,
                                   int max)
{
    int n = 0;
    const command_t *t = commands_table();
    int ntab = commands_count();
    int plen = (int)strlen(prefix);
    for (int i = 0; i < ntab && n < max; i++) {
        if (strncmp(t[i].name, prefix, plen) == 0) {
            out[n].name = t[i].name;
            out[n].is_dir = false;
            n++;
        }
    }
    return n;
}

static int collect_path_matches(const char *partial, vfs_node_t *cwd,
                                compl_match_t *out, int max,
                                char *common_prefix_out, int cp_max)
{
    /* Split partial into "dir/" + "leaf-prefix". If no '/' present, dir = cwd. */
    char dir_part[PATH_MAX];
    const char *slash = NULL;
    for (const char *p = partial; *p; p++)
        if (*p == '/') slash = p;

    const char *leaf = partial;
    if (slash) {
        int dlen = (int)(slash - partial + 1);
        if (dlen >= PATH_MAX) dlen = PATH_MAX - 1;
        memcpy(dir_part, partial, dlen);
        dir_part[dlen] = '\0';
        leaf = slash + 1;
    } else {
        dir_part[0] = '\0';
    }

    vfs_node_t *dir = dir_part[0] ? vfs_resolve(dir_part, cwd) : cwd;
    if (!dir || dir->type != VFS_DIRECTORY) return 0;

    int n = 0;
    int leaf_len = (int)strlen(leaf);
    for (uint32_t i = 0; ; i++) {
        vfs_node_t *c = vfs_readdir(dir, i);
        if (!c) break;
        if (strncmp(c->name, leaf, leaf_len) == 0 && n < max) {
            out[n].name = c->name;
            out[n].is_dir = (c->type == VFS_DIRECTORY);
            n++;
        }
    }

    /* Compute the longest common prefix of all matches (after `leaf`),
     * so even when there are several matches we can still extend the
     * input by the unambiguous portion. */
    if (common_prefix_out && cp_max > 0 && n > 0) {
        strncpy(common_prefix_out, out[0].name, cp_max - 1);
        common_prefix_out[cp_max - 1] = '\0';
        for (int i = 1; i < n; i++) {
            int j = 0;
            while (common_prefix_out[j] && out[i].name[j] &&
                   common_prefix_out[j] == out[i].name[j])
                j++;
            common_prefix_out[j] = '\0';
        }
    }
    return n;
}

/* Apply a completion: erase the partial word from the line and replace it
 * with `replacement`, optionally appending a trailing char. Updates buf/len/cursor
 * AND the visible terminal. */
static void apply_completion(char *buf, int *len, int *cursor, int max,
                             int word_start,
                             const char *replacement, char trailing)
{
    int repl_len = (int)strlen(replacement);
    int extra    = trailing ? 1 : 0;
    int new_len  = word_start + repl_len + extra + (*len - *cursor);
    if (new_len >= max) return;

    /* Save the tail to the right of the cursor. */
    char tail[SHELL_LINE_MAX];
    int tail_len = *len - *cursor;
    memcpy(tail, buf + *cursor, tail_len);

    /* Visually erase from word_start to current cursor. */
    while (*cursor > word_start) {
        vga_putchar('\b'); vga_putchar(' '); vga_putchar('\b');
        (*cursor)--;
    }

    /* Splice the replacement in. */
    memcpy(buf + word_start, replacement, repl_len);
    if (trailing) buf[word_start + repl_len] = trailing;
    memcpy(buf + word_start + repl_len + extra, tail, tail_len);
    buf[new_len] = '\0';
    *len = new_len;

    /* Print the replacement (+ trailing + tail) and move cursor back over tail. */
    for (int i = 0; i < repl_len + extra; i++)
        vga_putchar(buf[word_start + i]);
    *cursor = word_start + repl_len + extra;
    for (int i = 0; i < tail_len; i++)
        vga_putchar(buf[*cursor + i]);
    for (int i = 0; i < tail_len; i++)
        vga_putchar('\b');
}

/* Re-render "prompt + current line" after we've printed something below.
 * The prompt is regenerated by shell_run() on each iteration, so for Tab
 * completion we just print "\n" + the existing input again. */
static void redraw_input_line(const char *buf, int len, int cursor);

static void handle_tab_completion(char *buf, int *len, int *cursor, int max)
{
    /* Find the start of the word the cursor sits in. */
    int word_start = *cursor;
    while (word_start > 0 && buf[word_start - 1] != ' ')
        word_start--;

    /* Is this the FIRST word of the line (i.e. the command)? */
    bool is_command = true;
    for (int i = 0; i < word_start; i++) {
        if (buf[i] != ' ') { is_command = false; break; }
    }

    char partial[SHELL_LINE_MAX];
    int  plen = *cursor - word_start;
    memcpy(partial, buf + word_start, plen);
    partial[plen] = '\0';

    compl_match_t matches[64];
    char common[PATH_MAX] = "";
    int n;
    if (is_command) {
        n = collect_command_matches(partial, matches, 64);
    } else {
        n = collect_path_matches(partial, shell_get_state()->cwd,
                                 matches, 64, common, sizeof(common));
    }

    if (n == 0) return;   /* nothing to complete */

    if (n == 1) {
        /* unique: replace the partial with the full name */
        const char *full = matches[0].name;
        char trail = is_command ? ' ' :
                     (matches[0].is_dir ? '/' : ' ');
        /* For path completion the partial may include "dir/", so we keep
         * everything before the leaf and only replace the leaf portion. */
        if (!is_command) {
            const char *slash = NULL;
            for (const char *p = partial; *p; p++)
                if (*p == '/') slash = p;
            if (slash) {
                /* word_start needs to advance past the existing dir part */
                int dlen = (int)(slash - partial + 1);
                word_start += dlen;
            }
        }
        apply_completion(buf, len, cursor, max, word_start, full, trail);
        return;
    }

    /* Multiple matches: extend by the common prefix (if it's longer than
     * what's already there), then list candidates on a new line. */
    if (!is_command && common[0]) {
        const char *slash = NULL;
        for (const char *p = partial; *p; p++)
            if (*p == '/') slash = p;
        const char *leaf = slash ? slash + 1 : partial;
        if (strlen(common) > strlen(leaf)) {
            int ws = word_start + (slash ? (int)(slash - partial + 1) : 0);
            apply_completion(buf, len, cursor, max, ws, common, 0);
        }
    }
    /* For command completion we could also extend by common prefix, but the
     * built-in command set is small enough that listing is fine. */

    vga_putchar('\n');
    for (int i = 0; i < n; i++) {
        if (matches[i].is_dir)
            vga_print_color(matches[i].name,
                            vga_entry_color(VGA_LIGHT_BLUE, VGA_BLACK));
        else
            kprintf("%s", matches[i].name);
        kprintf("%s", matches[i].is_dir ? "/  " : "  ");
    }
    vga_putchar('\n');

    /* Re-render the prompt + current input so the user can keep typing. */
    redraw_input_line(buf, *len, *cursor);
}

/* Re-render the prompt + current input after we've printed below it
 * (e.g. listing tab-completion candidates). */
static void redraw_input_line(const char *buf, int len, int cursor)
{
    print_prompt();
    for (int i = 0; i < len; i++)
        vga_putchar(buf[i]);
    /* Move logical cursor back from end to its real position. */
    for (int i = len; i > cursor; i--)
        vga_putchar('\b');
}

int shell_read_line(char *buf, int max)
{
    int len = 0;
    int cursor = 0;
    int hist_pos = history_count;   /* one past newest = "current empty" */
    char saved[SHELL_LINE_MAX];
    saved[0] = '\0';
    buf[0] = '\0';

    for (;;) {
        int key = keyboard_getchar();

        if (key == '\n') {
            vga_putchar('\n');
            buf[len] = '\0';
            return len;
        } else if (key == '\b') {
            if (cursor > 0) {
                /* delete char before cursor */
                for (int i = cursor - 1; i < len - 1; i++)
                    buf[i] = buf[i + 1];
                len--;
                cursor--;
                /* redraw tail */
                vga_putchar('\b');
                for (int i = cursor; i < len; i++)
                    vga_putchar(buf[i]);
                vga_putchar(' ');
                /* move cursor back to position */
                for (int i = len; i >= cursor; i--)
                    vga_putchar('\b');
            }
        } else if (key == KEY_LEFT) {
            if (cursor > 0) {
                cursor--;
                vga_putchar('\b');
            }
        } else if (key == KEY_RIGHT) {
            if (cursor < len) {
                vga_putchar(buf[cursor]);
                cursor++;
            }
        } else if (key == KEY_UP) {
            if (hist_pos > 0) {
                if (hist_pos == history_count) {
                    strncpy(saved, buf, SHELL_LINE_MAX - 1);
                    saved[len] = '\0';
                }
                hist_pos--;
                const char *h = shell_history_get(hist_pos);
                if (h) {
                    /* erase current line visually */
                    while (cursor < len) { vga_putchar(buf[cursor]); cursor++; }
                    while (len > 0) { vga_putchar('\b'); vga_putchar(' ');
                                      vga_putchar('\b'); len--; }
                    strncpy(buf, h, max - 1);
                    buf[max - 1] = '\0';
                    len = (int)strlen(buf);
                    cursor = len;
                    for (int i = 0; i < len; i++)
                        vga_putchar(buf[i]);
                }
            }
        } else if (key == KEY_DOWN) {
            if (hist_pos < history_count) {
                hist_pos++;
                const char *h = (hist_pos == history_count)
                                    ? saved : shell_history_get(hist_pos);
                /* erase current line */
                while (cursor < len) { vga_putchar(buf[cursor]); cursor++; }
                while (len > 0) { vga_putchar('\b'); vga_putchar(' ');
                                  vga_putchar('\b'); len--; }
                strncpy(buf, h ? h : "", max - 1);
                buf[max - 1] = '\0';
                len = (int)strlen(buf);
                cursor = len;
                for (int i = 0; i < len; i++)
                    vga_putchar(buf[i]);
            }
        } else if (key == KEY_HOME) {
            while (cursor > 0) { vga_putchar('\b'); cursor--; }
        } else if (key == KEY_END) {
            while (cursor < len) { vga_putchar(buf[cursor]); cursor++; }
        } else if (key == '\t') {
            handle_tab_completion(buf, &len, &cursor, max);
        } else if (key >= 32 && key < 127) {
            if (len < max - 1) {
                /* insert at cursor */
                for (int i = len; i > cursor; i--)
                    buf[i] = buf[i - 1];
                buf[cursor] = (char)key;
                len++;
                /* print from cursor to end */
                for (int i = cursor; i < len; i++)
                    vga_putchar(buf[i]);
                cursor++;
                /* move cursor back to logical position */
                for (int i = len; i > cursor; i--)
                    vga_putchar('\b');
            }
        }
        /* ignore other keys */
    }
}

/* Read a password without echoing characters (prints '*' as feedback). */
int shell_read_password(char *buf, int max)
{
    int len = 0;
    for (;;) {
        int key = keyboard_getchar();
        if (key == '\n') {
            vga_putchar('\n');
            buf[len] = '\0';
            return len;
        } else if (key == '\b') {
            if (len > 0) {
                len--;
                vga_putchar('\b');
            }
        } else if (key >= 32 && key < 127) {
            if (len < max - 1) {
                buf[len++] = (char)key;
                vga_putchar('*');
            }
        }
    }
}

/* Interactive login: prompt for username + password until valid. */
void shell_login_prompt(void)
{
    char username[64];
    char password[64];

    for (;;) {
        vga_set_color(vga_entry_color(VGA_WHITE, VGA_BLACK));
        kprintf("%s login: ", state.hostname);
        vga_set_color(state.color);
        shell_read_line(username, sizeof(username));
        if (username[0] == '\0')
            continue;

        kprintf("Password: ");
        shell_read_password(password, sizeof(password));

        user_t *u = users_find(username);
        if (u && users_check_password(username, password)) {
            set_current_user(u);
            state.cwd = vfs_resolve(u->home, vfs_get_root());
            if (!state.cwd)
                state.cwd = vfs_get_root();
            vga_set_color(vga_entry_color(VGA_LIGHT_GREEN, VGA_BLACK));
            kprintf("Welcome, %s!\n", u->name);
            vga_set_color(state.color);
            return;
        }
        vga_set_color(vga_entry_color(VGA_LIGHT_RED, VGA_BLACK));
        kprintf("Login incorrect\n\n");
        vga_set_color(state.color);
    }
}

/* ---- tokenization ---- */
/* Tokenize a command line into argv-style words.
 *
 * Improvements over a naive split-on-spaces:
 *   - Double quotes preserve spaces: echo "hola mundo" > f  -> 3 args
 *   - Redirection operators ('>' '>>') are always their own tokens, even
 *     when written without surrounding spaces:  echo hola>f.txt  -> 4 tokens
 *     (this fixes the classic "echo X >file" pitfall on small shells)
 *
 * Modifies `line` in place by inserting NULs between tokens. The argv[]
 * entries point inside `line`. Returns argc.
 */
/* Two-pass tokenizer.
 *
 * The previous in-place "compact while writing" approach was buggy:
 * argv[i] pointers ended up overlapping when we both removed quote chars
 * and inserted NUL separators.
 *
 * Now we do a simpler thing: write the cleaned-up tokens into the second
 * half of a stack buffer, separated by NULs, and have argv[] point into
 * that buffer. The original `line` is left untouched. We size `scratch`
 * the same as SHELL_LINE_MAX, plenty for any single command.
 *
 * Recognized syntax:
 *   - Whitespace splits tokens.
 *   - '>'  and '>>' are always their own tokens (even with no spaces).
 *   - Double quotes preserve spaces inside; the quote chars are stripped.
 */
static int tokenize(char *line, char **argv)
{
    static char scratch[SHELL_LINE_MAX];
    char *dst = scratch;
    int   argc = 0;
    const char *src = line;
    const char *scratch_end = scratch + sizeof(scratch) - 1;

    while (*src && argc < SHELL_MAX_ARGS - 1 && dst < scratch_end) {
        /* skip whitespace between tokens */
        while (*src == ' ' || *src == '\t')
            src++;
        if (!*src) break;

        /* '>' or '>>' is a standalone token, regardless of spacing */
        if (*src == '>') {
            argv[argc++] = dst;
            *dst++ = '>';
            src++;
            if (*src == '>' && dst < scratch_end) { *dst++ = '>'; src++; }
            if (dst < scratch_end) *dst++ = '\0';
            continue;
        }

        argv[argc++] = dst;
        bool in_quotes = false;
        while (*src && dst < scratch_end) {
            if (*src == '"') { in_quotes = !in_quotes; src++; continue; }
            if (!in_quotes && (*src == ' ' || *src == '\t')) break;
            if (!in_quotes && *src == '>') break;   /* break BEFORE the '>' */
            *dst++ = *src++;
        }
        if (dst < scratch_end) *dst++ = '\0';
    }
    argv[argc] = NULL;
    return argc;
}

/* Split a line on '|' into stage strings. Returns the number of stages. */
static int split_pipeline(char *line, char **stages, int max)
{
    int n = 0;
    char *p = line;
    stages[n++] = p;
    while (*p && n < max) {
        if (*p == '|') {
            *p = '\0';
            stages[n++] = p + 1;
        }
        p++;
    }
    return n;
}

/* Run a full command line, supporting pipes: cmd1 | cmd2 | cmd3.
 * Each stage's captured output becomes the next stage's pipe_in. */
#define PIPE_BUF 8192
static void run_pipeline(char *line)
{
    char *stages[SHELL_MAX_ARGS];
    int nstages = split_pipeline(line, stages, SHELL_MAX_ARGS);

    /* two ping-pong buffers for the data flowing between stages */
    static char buf_a[PIPE_BUF];
    static char buf_b[PIPE_BUF];
    char *cur_in = NULL;
    uint32_t cur_in_len = 0;

    char *argv[SHELL_MAX_ARGS];

    for (int i = 0; i < nstages; i++) {
        char work[SHELL_LINE_MAX];
        strncpy(work, stages[i], sizeof(work) - 1);
        work[sizeof(work) - 1] = '\0';

        int argc = tokenize(work, argv);
        if (argc == 0)
            continue;

        bool last = (i == nstages - 1);

        /* feed this stage the previous stage's output */
        state.pipe_in = cur_in;
        state.pipe_in_len = cur_in_len;

        char *out = (i & 1) ? buf_b : buf_a;   /* alternate output buffer */
        if (!last)
            vga_capture_begin(out, PIPE_BUF);

        int rc = commands_dispatch(argc, argv);

        if (!last) {
            cur_in_len = vga_capture_end();
            cur_in = out;
        }

        state.pipe_in = NULL;
        state.pipe_in_len = 0;

        if (rc == -1) {
            kprintf("%s: command not found. Type 'help'.\n", argv[0]);
            break;
        }
    }
}

void shell_run(void)
{
    char line[SHELL_LINE_MAX];

    /* init state */
    state.cwd = vfs_get_root();
    strcpy(state.user, DEFAULT_USER);
    state.color = vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK);
    state.running = true;
    state.umask = 022;
    state.pipe_in = NULL;
    state.pipe_in_len = 0;
    vfs_set_umask(state.umask);

    /* load hostname from /etc/hostname (defaults to lowercase kernel name) */
    strcpy(state.hostname, "trinux");
    vfs_node_t *hn = vfs_resolve("/etc/hostname", state.cwd);
    if (hn && hn->type == VFS_FILE && hn->size > 0) {
        char tmp[64];
        uint32_t n = vfs_read(hn, 0, sizeof(tmp) - 1, (uint8_t *)tmp);
        tmp[n] = '\0';
        /* strip trailing newline */
        char *nl = strchr(tmp, '\n');
        if (nl) *nl = '\0';
        if (tmp[0])
            strncpy(state.hostname, tmp, sizeof(state.hostname) - 1);
    }

    /* show motd */
    vfs_node_t *motd = vfs_resolve("/etc/motd", state.cwd);
    if (motd && motd->type == VFS_FILE && motd->size > 0) {
        char buf[512];
        uint32_t n = vfs_read(motd, 0, sizeof(buf) - 1, (uint8_t *)buf);
        buf[n] = '\0';
        vga_set_color(vga_entry_color(VGA_LIGHT_CYAN, VGA_BLACK));
        kprintf("%s\n", buf);
        vga_set_color(state.color);
    }

    /* require login before the shell starts */
    shell_login_prompt();
    if (current_user())
        strncpy(state.user, current_user()->name, sizeof(state.user) - 1);

    while (state.running) {
        print_prompt();
        int len = shell_read_line(line, sizeof(line));
        if (len <= 0)
            continue;

        shell_add_history(line);

        /* copy for the pipeline runner (keeps history intact) */
        char work[SHELL_LINE_MAX];
        strncpy(work, line, sizeof(work) - 1);
        work[sizeof(work) - 1] = '\0';

        run_pipeline(work);
    }
}

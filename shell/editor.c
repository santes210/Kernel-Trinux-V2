/* shell/editor.c  -  a tiny nano-style full-screen text editor.
 *
 * Layout (80x25):
 *   row 0      : title bar  (file name, modified flag)
 *   rows 1..22 : text area  (22 visible lines)
 *   row 23     : status/message line
 *   row 24     : help bar   (^S save  ^X exit ...)
 *
 * Keys: arrows / Home / End move; Enter splits a line; Backspace deletes;
 *       Ctrl-S or Ctrl-O save; Ctrl-X exit (asks to save if modified).
 */
#include "editor.h"
#include "shell.h"
#include "../drivers/vga.h"
#include "../drivers/keyboard.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../lib/printf.h"

#define ED_MAX_LINES 1024
#define ED_MAX_COLS  256
#define TEXT_ROWS    22          /* visible text rows (1..22) */
#define TEXT_TOP     1           /* first screen row used for text */

static char     lines[ED_MAX_LINES][ED_MAX_COLS];
static uint32_t line_len[ED_MAX_LINES];
static uint32_t num_lines;

static int cx, cy;        /* cursor: column, line index */
static int top;           /* first visible line (vertical scroll) */
static int coloff;        /* first visible column (horizontal scroll) */
static bool modified;

/* ---- helpers ---- */

static void draw_str(int x, int y, const char *s, uint8_t color)
{
    vga_set_cursor(x, y);
    vga_set_color(color);
    while (*s && x < VGA_WIDTH) { vga_putchar(*s++); x++; }
}

/* fill a whole row with spaces in the given color */
static void clear_row(int y, uint8_t color)
{
    vga_set_cursor(0, y);
    vga_set_color(color);
    for (int x = 0; x < VGA_WIDTH; x++)
        vga_putchar(' ');
}

/* ---- load / save ---- */

static void editor_reset(void)
{
    num_lines = 1;
    line_len[0] = 0;
    lines[0][0] = '\0';
    cx = cy = top = coloff = 0;
    modified = false;
}

static void load_file(vfs_node_t *node)
{
    editor_reset();
    if (!node || node->type != VFS_FILE || node->size == 0)
        return;

    static char buf[ED_MAX_LINES * 64];
    uint32_t n = vfs_read(node, 0, sizeof(buf) - 1, (uint8_t *)buf);
    buf[n] = '\0';

    uint32_t li = 0, col = 0;
    line_len[0] = 0;
    for (uint32_t i = 0; i < n; i++) {
        char ch = buf[i];
        if (ch == '\n') {
            lines[li][col] = '\0';
            line_len[li] = col;
            li++;
            if (li >= ED_MAX_LINES) { li = ED_MAX_LINES - 1; break; }
            col = 0;
            line_len[li] = 0;
            lines[li][0] = '\0';
        } else if (col < ED_MAX_COLS - 1) {
            lines[li][col++] = ch;
        }
    }
    lines[li][col] = '\0';
    line_len[li] = col;
    num_lines = li + 1;
}

static int save_file(const char *path)
{
    shell_state_t *s = shell_get_state();
    vfs_node_t *node = vfs_create(path, s->cwd);
    if (!node)
        return -1;

    static char out[ED_MAX_LINES * 64];
    uint32_t off = 0;
    for (uint32_t i = 0; i < num_lines; i++) {
        for (uint32_t j = 0; j < line_len[i] && off < sizeof(out) - 2; j++)
            out[off++] = lines[i][j];
        if (i + 1 < num_lines && off < sizeof(out) - 2)
            out[off++] = '\n';
    }
    node->size = 0;   /* truncate then rewrite */
    vfs_write(node, 0, off, (uint8_t *)out);
    modified = false;
    return 0;
}

/* ---- rendering ---- */

static void render(const char *path)
{
    uint8_t titlecol = vga_entry_color(VGA_BLACK, VGA_LIGHT_CYAN);
    uint8_t helpcol  = vga_entry_color(VGA_BLACK, VGA_LIGHT_GREY);
    uint8_t textcol  = vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK);

    /* title bar */
    clear_row(0, titlecol);
    char title[80];
    snprintf(title, sizeof(title), "  myedit   %s%s",
             path, modified ? "   [modified]" : "");
    draw_str(0, 0, title, titlecol);

    /* keep cursor on screen (vertical + horizontal scroll) */
    if (cy < top) top = cy;
    if (cy >= top + TEXT_ROWS) top = cy - TEXT_ROWS + 1;
    if (cx < coloff) coloff = cx;
    if (cx >= coloff + VGA_WIDTH) coloff = cx - VGA_WIDTH + 1;

    /* text area */
    for (int r = 0; r < TEXT_ROWS; r++) {
        int y = TEXT_TOP + r;
        clear_row(y, textcol);
        int li = top + r;
        if (li >= (int)num_lines)
            continue;
        vga_set_cursor(0, y);
        vga_set_color(textcol);
        for (int x = 0; x < VGA_WIDTH; x++) {
            int col = coloff + x;
            if (col < (int)line_len[li])
                vga_putchar(lines[li][col]);
            else
                break;
        }
    }

    /* status line */
    clear_row(23, textcol);
    char st[80];
    snprintf(st, sizeof(st), "  line %d/%u  col %d   %u lines",
             cy + 1, num_lines, cx + 1, num_lines);
    draw_str(0, 23, st, vga_entry_color(VGA_DARK_GREY, VGA_BLACK));

    /* help bar */
    clear_row(24, helpcol);
    draw_str(0, 24,
        "  ^S Save   ^X Exit   ^K Cut line   arrows move   Home/End",
        helpcol);

    /* place the hardware cursor at the edit position */
    vga_set_color(textcol);
    vga_set_cursor(cx - coloff, TEXT_TOP + (cy - top));
}

/* ---- editing primitives ---- */

static void insert_char(char c)
{
    if (line_len[cy] >= ED_MAX_COLS - 1)
        return;
    char *ln = lines[cy];
    for (int i = (int)line_len[cy]; i > cx; i--)
        ln[i] = ln[i - 1];
    ln[cx] = c;
    line_len[cy]++;
    ln[line_len[cy]] = '\0';
    cx++;
    modified = true;
}

static void insert_newline(void)
{
    if (num_lines >= ED_MAX_LINES)
        return;
    /* shift lines down to make room after cy */
    for (int i = (int)num_lines; i > cy + 1; i--) {
        memcpy(lines[i], lines[i - 1], ED_MAX_COLS);
        line_len[i] = line_len[i - 1];
    }
    /* tail of current line moves to the new line */
    int tail = (int)line_len[cy] - cx;
    memcpy(lines[cy + 1], &lines[cy][cx], (uint32_t)tail);
    lines[cy + 1][tail] = '\0';
    line_len[cy + 1] = (uint32_t)tail;

    lines[cy][cx] = '\0';
    line_len[cy] = (uint32_t)cx;

    num_lines++;
    cy++;
    cx = 0;
    modified = true;
}

static void delete_back(void)
{
    if (cx > 0) {
        char *ln = lines[cy];
        for (int i = cx - 1; i < (int)line_len[cy]; i++)
            ln[i] = ln[i + 1];
        line_len[cy]--;
        cx--;
        modified = true;
    } else if (cy > 0) {
        /* merge with previous line */
        int prev = cy - 1;
        int plen = (int)line_len[prev];
        int clen = (int)line_len[cy];
        if (plen + clen < ED_MAX_COLS - 1) {
            memcpy(&lines[prev][plen], lines[cy], (uint32_t)clen + 1);
            line_len[prev] = (uint32_t)(plen + clen);
            /* shift lines up */
            for (int i = cy; i + 1 < (int)num_lines; i++) {
                memcpy(lines[i], lines[i + 1], ED_MAX_COLS);
                line_len[i] = line_len[i + 1];
            }
            num_lines--;
            cy = prev;
            cx = plen;
            modified = true;
        }
    }
}

static void cut_line(void)
{
    if (num_lines == 1) {
        line_len[0] = 0;
        lines[0][0] = '\0';
        cx = 0;
        modified = true;
        return;
    }
    for (int i = cy; i + 1 < (int)num_lines; i++) {
        memcpy(lines[i], lines[i + 1], ED_MAX_COLS);
        line_len[i] = line_len[i + 1];
    }
    num_lines--;
    if (cy >= (int)num_lines) cy = (int)num_lines - 1;
    if (cx > (int)line_len[cy]) cx = (int)line_len[cy];
    modified = true;
}

/* simple yes/no prompt on the status line; returns true for yes */
static bool prompt_yes(const char *msg)
{
    clear_row(23, vga_entry_color(VGA_WHITE, VGA_RED));
    draw_str(0, 23, msg, vga_entry_color(VGA_WHITE, VGA_RED));
    for (;;) {
        int k = keyboard_getchar();
        if (k == 'y' || k == 'Y') return true;
        if (k == 'n' || k == 'N') return false;
        if (k == KEY_ESC) return false;
    }
}

/* ---- main loop ---- */

int editor_run(const char *path)
{
    shell_state_t *s = shell_get_state();
    vfs_node_t *node = vfs_resolve(path, s->cwd);
    load_file(node);

    for (;;) {
        render(path);
        int k = keyboard_getchar();

        if (k == 19 || k == 15) {            /* Ctrl-S / Ctrl-O = save */
            if (save_file(path) == 0) {
                clear_row(23, vga_entry_color(VGA_BLACK, VGA_LIGHT_GREEN));
                draw_str(0, 23, "  Saved.", vga_entry_color(VGA_BLACK, VGA_LIGHT_GREEN));
            } else {
                clear_row(23, vga_entry_color(VGA_WHITE, VGA_RED));
                draw_str(0, 23, "  Save failed!", vga_entry_color(VGA_WHITE, VGA_RED));
            }
            keyboard_getchar();   /* pause so the message is visible */
        } else if (k == 24) {                /* Ctrl-X = exit */
            if (modified && prompt_yes("  Save modified buffer? (y/n)"))
                save_file(path);
            break;
        } else if (k == 11) {                /* Ctrl-K = cut line */
            cut_line();
        } else if (k == KEY_UP) {
            if (cy > 0) { cy--; if (cx > (int)line_len[cy]) cx = (int)line_len[cy]; }
        } else if (k == KEY_DOWN) {
            if (cy + 1 < (int)num_lines) { cy++; if (cx > (int)line_len[cy]) cx = (int)line_len[cy]; }
        } else if (k == KEY_LEFT) {
            if (cx > 0) cx--;
            else if (cy > 0) { cy--; cx = (int)line_len[cy]; }
        } else if (k == KEY_RIGHT) {
            if (cx < (int)line_len[cy]) cx++;
            else if (cy + 1 < (int)num_lines) { cy++; cx = 0; }
        } else if (k == KEY_HOME) {
            cx = 0;
        } else if (k == KEY_END) {
            cx = (int)line_len[cy];
        } else if (k == '\n') {
            insert_newline();
        } else if (k == '\b') {
            delete_back();
        } else if (k == '\t') {
            insert_char(' '); insert_char(' ');   /* tab = 2 spaces */
        } else if (k >= 32 && k < 127) {
            insert_char((char)k);
        }
    }

    /* restore a clean screen for the shell */
    vga_set_color(s->color);
    vga_clear();
    vga_set_cursor(0, 0);
    return 0;
}

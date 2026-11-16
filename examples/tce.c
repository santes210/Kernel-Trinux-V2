/* tce.c  -  Trinux Code Editor
 *
 * Compiled with the in-kernel `tcc` compiler:
 *
 *     tcc tce.c          (produces ./tce)
 *     exec tce hello.c   (or just `tce`, will ask for a filename)
 *
 * Features
 *   - Full-screen nano-style editor over the VGA text buffer.
 *   - Loads a file into memory, lets you edit, saves with Ctrl-S.
 *   - Arrow keys, Home, End, Backspace, Enter.
 *   - Status bar with file name, cursor position, modified flag.
 *   - Ctrl-S = save, Ctrl-X = exit (asks to save if modified).
 *
 * Limitations of the underlying compiler
 *   The Trinux `tcc` is a tiny single-pass C subset: no structs, no enums,
 *   no preprocessor, no char literals. We use plain ints everywhere and
 *   spell every character by its ASCII value.
 *
 * Memory model
 *   All buffers live in .bss as globals (added to tcc in the same patch
 *   that ships this file). Locals on the stack are tiny — this fixes the
 *   "huge int txt[32768]" stack-overflow of the previous tce.c.
 */

/* ===========================================================
 *   Constants  (no #define in this dialect — use globals)
 * =========================================================== */
int W;          /* VGA width  (80)              */
int H;          /* VGA height (25)              */
int BUFCAP;     /* max bytes the buffer holds   */
int NAMECAP;    /* max length of the file name  */

/* Colors (foreground in low nibble, background in high nibble). */
int C_TEXT;
int C_TITLE;
int C_STAT;
int C_HELP;
int C_OK;
int C_ERR;

/* ===========================================================
 *   Editor state  (all global so they live in .bss)
 * =========================================================== */
char buf[32768];     /* the file contents (NUL-terminated)  */
int  blen;           /* current logical length              */
int  cursor;         /* byte offset of the cursor in buf    */
int  modified;       /* 1 if there are unsaved changes      */
int  top_line;       /* index of the line shown at row 1    */

char fname[128];     /* current file name                   */
int  fnamelen;

char msg[80];        /* one-shot status message             */
int  msglen;
int  msgcol;

/* ===========================================================
 *   Tiny helpers
 * =========================================================== */

int color(int fg, int bg) {
    return (bg * 16) + fg;
}

void put_str(int x, int y, char s[], int col) {
    int i;
    i = 0;
    while (s[i] != 0 && x + i < W) {
        vga_putchar(x + i, y, s[i], col);
        i = i + 1;
    }
}

void clear_row(int y, int col) {
    int x;
    x = 0;
    while (x < W) {
        vga_putchar(x, y, 32, col);  /* 32 = space */
        x = x + 1;
    }
}

/* Write the integer `v` as decimal into out[]. Returns its length. */
int int_to_str(int v, char out[]) {
    int i;
    int j;
    int neg;
    char tmp[16];

    neg = 0;
    if (v < 0) { neg = 1; v = -v; }

    i = 0;
    if (v == 0) {
        tmp[i] = 48;   /* '0' */
        i = i + 1;
    }
    while (v > 0) {
        tmp[i] = 48 + (v % 10);
        v = v / 10;
        i = i + 1;
    }
    if (neg) {
        tmp[i] = 45;   /* '-' */
        i = i + 1;
    }

    /* reverse */
    j = 0;
    while (j < i) {
        out[j] = tmp[i - 1 - j];
        j = j + 1;
    }
    out[i] = 0;
    return i;
}

/* ===========================================================
 *   Buffer primitives
 * =========================================================== */

void buf_insert(int pos, int ch) {
    int i;
    if (blen + 1 >= BUFCAP) return;
    i = blen;
    while (i > pos) {
        buf[i] = buf[i - 1];
        i = i - 1;
    }
    buf[pos] = ch;
    blen = blen + 1;
    buf[blen] = 0;
    modified = 1;
}

void buf_delete(int pos) {
    int i;
    if (pos < 0) return;
    if (pos >= blen) return;
    i = pos;
    while (i + 1 < blen) {
        buf[i] = buf[i + 1];
        i = i + 1;
    }
    blen = blen - 1;
    buf[blen] = 0;
    modified = 1;
}

/* Find the start-of-line offset for the line containing `pos`. */
int line_start(int pos) {
    int i;
    i = pos;
    while (i > 0 && buf[i - 1] != 10) i = i - 1;
    return i;
}

/* Find the end-of-line offset (the '\n' or blen) for line containing pos. */
int line_end(int pos) {
    int i;
    i = pos;
    while (i < blen && buf[i] != 10) i = i + 1;
    return i;
}

/* Total number of lines in the buffer (lines are separated by '\n'). */
int count_lines() {
    int i;
    int n;
    n = 1;
    i = 0;
    while (i < blen) {
        if (buf[i] == 10) n = n + 1;
        i = i + 1;
    }
    return n;
}

/* Line index (0-based) of `pos`. */
int line_of(int pos) {
    int i;
    int n;
    n = 0;
    i = 0;
    while (i < pos) {
        if (buf[i] == 10) n = n + 1;
        i = i + 1;
    }
    return n;
}

/* Column (0-based) of `pos` within its line. */
int col_of(int pos) {
    return pos - line_start(pos);
}

/* Offset of the start of line index `n`. */
int offset_of_line(int n) {
    int i;
    int k;
    if (n <= 0) return 0;
    k = 0;
    i = 0;
    while (i < blen) {
        if (buf[i] == 10) {
            k = k + 1;
            if (k == n) return i + 1;
        }
        i = i + 1;
    }
    return blen;
}

/* ===========================================================
 *   Rendering
 * =========================================================== */

void draw_title() {
    int i;
    int x;

    clear_row(0, C_TITLE);

    /* "  tce  "  */
    put_str(0, 0, "  tce  ", C_TITLE);

    /* file name */
    x = 7;
    i = 0;
    while (fname[i] != 0 && x < W - 12) {
        vga_putchar(x, 0, fname[i], C_TITLE);
        x = x + 1;
        i = i + 1;
    }
    if (modified) put_str(x + 1, 0, "[modified]", C_TITLE);
}

void draw_status() {
    char s[40];
    int x;
    int n;
    int l;
    int c;

    clear_row(H - 2, C_STAT);

    l = line_of(cursor) + 1;
    c = col_of(cursor) + 1;

    put_str(2, H - 2, "line ", C_STAT);
    x = 7;
    n = int_to_str(l, s);
    put_str(x, H - 2, s, C_STAT);
    x = x + n;
    put_str(x, H - 2, "  col ", C_STAT);
    x = x + 6;
    n = int_to_str(c, s);
    put_str(x, H - 2, s, C_STAT);
    x = x + n;
    put_str(x, H - 2, "    bytes ", C_STAT);
    x = x + 10;
    n = int_to_str(blen, s);
    put_str(x, H - 2, s, C_STAT);

    if (msglen > 0) {
        put_str(W - msglen - 2, H - 2, msg, msgcol);
    }
}

void draw_help() {
    clear_row(H - 1, C_HELP);
    put_str(1, H - 1,
            "^S Save   ^X Exit   arrows move   Home/End   Backspace",
            C_HELP);
}

/* Draw the text area: rows 1 .. H-3 (the bottom two rows are status+help). */
void draw_text() {
    int row;
    int y;
    int x;
    int off;
    int ch;
    int cur_screen_x;
    int cur_screen_y;
    int cur_line;

    cur_line     = line_of(cursor);
    cur_screen_x = col_of(cursor);
    cur_screen_y = cur_line - top_line + 1;

    /* keep cursor on screen vertically */
    if (cur_screen_y < 1) {
        top_line = cur_line;
        cur_screen_y = 1;
    }
    if (cur_screen_y > H - 3) {
        top_line = cur_line - (H - 3 - 1);
        cur_screen_y = cur_line - top_line + 1;
    }

    /* Paint each visible row from the buffer. */
    off = offset_of_line(top_line);
    row = 0;
    while (row < H - 3) {
        y = 1 + row;
        clear_row(y, C_TEXT);
        x = 0;
        while (off < blen && buf[off] != 10 && x < W) {
            ch = buf[off];
            if (ch == 9) ch = 32;          /* tab -> space */
            if (ch < 32) ch = 32;          /* unprintable */
            vga_putchar(x, y, ch, C_TEXT);
            x = x + 1;
            off = off + 1;
        }
        /* skip past the '\n' if any */
        if (off < blen && buf[off] == 10) off = off + 1;
        row = row + 1;
    }

    /* Show a tiny block where the cursor is, in inverted colors. */
    if (cur_screen_y >= 1 && cur_screen_y < H - 2 &&
        cur_screen_x >= 0 && cur_screen_x < W) {
        int chc;
        int p;
        p = cursor;
        if (p < blen) chc = buf[p];
        else          chc = 32;
        if (chc == 10) chc = 32;
        if (chc <  32) chc = 32;
        vga_putchar(cur_screen_x, cur_screen_y, chc, color(0, 7));
    }
}

void render() {
    draw_title();
    draw_text();
    draw_status();
    draw_help();
    /* one-shot message clears after it has been shown */
    msglen = 0;
}

void set_msg(char m[], int col) {
    int i;
    i = 0;
    while (m[i] != 0 && i < 79) {
        msg[i] = m[i];
        i = i + 1;
    }
    msg[i] = 0;
    msglen = i;
    msgcol = col;
}

/* ===========================================================
 *   File I/O   (kernel-provided syscalls in tcc)
 * =========================================================== */

void load_file() {
    int n;
    n = read_file(fname, buf, BUFCAP - 1);
    if (n < 0) n = 0;
    blen = n;
    buf[blen] = 0;
    cursor = 0;
    top_line = 0;
    modified = 0;
}

int save_file() {
    int n;
    n = write_file(fname, buf, blen);
    if (n < 0) {
        set_msg("Save FAILED", C_ERR);
        return 0;
    }
    modified = 0;
    set_msg("Saved", C_OK);
    return 1;
}

/* ===========================================================
 *   Cursor movement
 * =========================================================== */

void move_left() {
    if (cursor > 0) cursor = cursor - 1;
}

void move_right() {
    if (cursor < blen) cursor = cursor + 1;
}

void move_home() {
    cursor = line_start(cursor);
}

void move_end() {
    cursor = line_end(cursor);
}

void move_up() {
    int col;
    int ls;
    int prev_start;
    int prev_end;
    int target;

    col = col_of(cursor);
    ls  = line_start(cursor);
    if (ls == 0) { cursor = 0; return; }
    prev_end   = ls - 1;
    prev_start = line_start(prev_end);
    target     = prev_start + col;
    if (target > prev_end) target = prev_end;
    cursor = target;
}

void move_down() {
    int col;
    int le;
    int next_start;
    int next_end;
    int target;

    col = col_of(cursor);
    le  = line_end(cursor);
    if (le >= blen) {
        cursor = blen;
        return;
    }
    next_start = le + 1;
    next_end   = line_end(next_start);
    target     = next_start + col;
    if (target > next_end) target = next_end;
    cursor = target;
}

/* ===========================================================
 *   Main loop
 * =========================================================== */

int main() {
    int key;
    int running;
    int i;

    /* ---- initialize constants ---- */
    W = 80;
    H = 25;
    BUFCAP  = 32768;
    NAMECAP = 128;
    C_TEXT  = color(7,  0);    /* light grey on black */
    C_TITLE = color(0,  3);    /* black on cyan       */
    C_STAT  = color(0,  7);    /* black on grey       */
    C_HELP  = color(15, 1);    /* white on blue       */
    C_OK    = color(0, 10);    /* black on green      */
    C_ERR   = color(15, 4);    /* white on red        */

    /* ---- initialize state ---- */
    blen     = 0;
    cursor   = 0;
    top_line = 0;
    modified = 0;
    msglen   = 0;
    buf[0]   = 0;

    /* Default name. Real argv handling isn't in tcc yet, so ask the user. */
    fname[0] = 0; fnamelen = 0;

    vga_clear(C_TEXT);
    put_str(0, 0, "tce - file to open (Enter for a new blank buffer): ",
            color(11, 0));
    fnamelen = read_line(fname, NAMECAP);
    if (fnamelen > 0) {
        load_file();
        if (blen == 0) {
            /* the file might just not exist yet -> treat as new */
            set_msg("New file", C_OK);
        }
    } else {
        /* untitled.txt by default */
        fname[0] = 117; fname[1] = 110; fname[2] = 116; fname[3] = 105;
        fname[4] = 116; fname[5] = 108; fname[6] = 101; fname[7] = 100;
        fname[8] = 46;  fname[9] = 116; fname[10]= 120; fname[11]= 116;
        fname[12]= 0;   /* "untitled.txt" */
    }

    vga_clear(C_TEXT);
    running = 1;
    while (running == 1) {
        render();
        key = getchar();

        /* ----- control keys ----- */
        if (key == 19) {                 /* Ctrl-S */
            save_file();
        } else if (key == 24) {          /* Ctrl-X */
            if (modified == 1) {
                set_msg("Save? y/n", C_ERR);
                render();
                i = getchar();
                if (i == 121 || i == 89) save_file();   /* y / Y */
            }
            running = 0;
        }
        /* ----- arrows (KEY_* codes from drivers/keyboard.h) ----- */
        else if (key == 128) { move_up();    }   /* KEY_UP    */
        else if (key == 129) { move_down();  }   /* KEY_DOWN  */
        else if (key == 130) { move_left();  }   /* KEY_LEFT  */
        else if (key == 131) { move_right(); }   /* KEY_RIGHT */
        else if (key == 144) { move_home();  }   /* KEY_HOME  */
        else if (key == 145) { move_end();   }   /* KEY_END   */
        else if (key == 146) {                   /* KEY_DELETE */
            buf_delete(cursor);
        }
        /* ----- text input ----- */
        else if (key == 10) {            /* Enter */
            buf_insert(cursor, 10);
            cursor = cursor + 1;
        } else if (key == 8) {           /* Backspace */
            if (cursor > 0) {
                cursor = cursor - 1;
                buf_delete(cursor);
            }
        } else if (key == 9) {           /* Tab -> two spaces */
            buf_insert(cursor, 32); cursor = cursor + 1;
            buf_insert(cursor, 32); cursor = cursor + 1;
        } else if (key >= 32 && key < 127) {
            buf_insert(cursor, key);
            cursor = cursor + 1;
        }
        /* ignore everything else */
    }

    vga_clear(color(7, 0));
    put_str(0, 0, "tce: bye.", color(7, 0));
    return 0;
}

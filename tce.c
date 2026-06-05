/* tce.c — Trinux Code Editor
 * A VS Code-like full-screen code editor for Trinux.
 * Compile: tcc tce.c
 * Run:     exec tce
 *
 * Features:
 *   - Dark theme with syntax highlighting
 *   - Line numbers
 *   - Status bar with file info
 *   - Arrow keys, Home, End, Delete
 *   - Ctrl-Q to quit
 */

/* Helper: compare word in txt[] at offset `off` with string literal */
int match_word(int ta, int off, int len, int sa, int slen) {
    if (len != slen) return 0;
    int i;
    i = 0;
    while (i < len) {
        int a;
        int b;
        a = *(ta + (off + i) * 4) & 0xFF;
        b = *(sa + i) & 0xFF;
        if (a != b) return 0;
        i = i + 1;
    }
    return 1;
}

/* Check if word at txt[off..off+len-1] is a C keyword */
int is_kw(int ta, int off, int len) {
    if (len == 2) {
        if (match_word(ta, off, len, "do", 2)) return 1;
        if (match_word(ta, off, len, "if", 2)) return 1;
    }
    if (len == 3) {
        if (match_word(ta, off, len, "int", 3)) return 1;
        if (match_word(ta, off, len, "for", 3)) return 1;
    }
    if (len == 4) {
        if (match_word(ta, off, len, "char", 4)) return 1;
        if (match_word(ta, off, len, "void", 4)) return 1;
        if (match_word(ta, off, len, "else", 4)) return 1;
    }
    if (len == 5) {
        if (match_word(ta, off, len, "while", 5)) return 1;
        if (match_word(ta, off, len, "break", 5)) return 1;
    }
    if (len == 6) {
        if (match_word(ta, off, len, "return", 6)) return 1;
    }
    if (len == 7) {
        if (match_word(ta, off, len, "include", 7)) return 1;
    }
    if (len == 8) {
        if (match_word(ta, off, len, "continue", 8)) return 1;
    }
    return 0;
}

void main() {
    /* ==============================
     *  Constants
     * ============================== */
    int W;
    int H;
    int GUT;
    int CODEX;
    int VISIBLE;
    W = 80;
    H = 25;
    GUT = 6;
    CODEX = GUT + 1;
    VISIBLE = H - 2;

    /* Key codes */
    int K_UP;
    int K_DN;
    int K_LT;
    int K_RT;
    int K_HOME;
    int K_END;
    int K_DEL;
    K_UP = 0x80;
    K_DN = 0x81;
    K_LT = 0x82;
    K_RT = 0x83;
    K_HOME = 0x90;
    K_END = 0x91;
    K_DEL = 0x92;

    /* VGA colors */
    int C_DEF;
    int C_KW;
    int C_STR;
    int C_COM;
    int C_NUM;
    int C_LN;
    int C_LNC;
    int C_HDR;
    int C_STS;
    int C_SEP;
    int C_CUR;
    C_DEF = 0x07;
    C_KW = 0x09;
    C_STR = 0x0A;
    C_COM = 0x08;
    C_NUM = 0x0B;
    C_LN = 0x03;
    C_LNC = 0x0E;
    C_HDR = 0x1F;
    C_STS = 0x30;
    C_SEP = 0x08;
    C_CUR = 0x70;

    /* ==============================
     *  Text buffer  (32K chars = ~800 lines avg)
     * ============================== */
    int txt[32768];
    int tlen;
    tlen = 0;

    /* ==============================
     *  Line index  (1024 lines max)
     * ============================== */
    int loff[1024];
    int llen[1024];
    int nlines;

    /* ==============================
     *  Editor state
     * ============================== */
    int cx;
    int cy;
    int scr;
    int dirty;
    int running;
    cx = 0;
    cy = 0;
    scr = 0;
    dirty = 0;
    running = 1;

    /* ==============================
     *  Initialize with sample code
     * ============================== */
    int init;
    init = "int factorial(int n) {\n    if (n <= 1) return 1;\n    return n * factorial(n - 1);\n}\n\nvoid main() {\n    int i;\n    for (i = 0; i <= 10; i++) {\n        print(\"factorial(\");\n        print_num(i);\n        print(\") = \");\n        print_num(factorial(i));\n        print_char('\\n');\n    }\n}\n";
    tlen = 0;
    while (tlen < 32760) {
        int v;
        v = *(init + tlen) & 0xFF;
        if (v == 0) break;
        txt[tlen] = v;
        tlen = tlen + 1;
    }

    /* ==============================
     *  Build line index
     * ============================== */
    int rebuild_lines;
    rebuild_lines = 1;

    /* ==============================
     *  Temp variables
     * ============================== */
    int i;
    int j;
    int k;
    int key;
    int row;
    int col;
    int ch;
    int color;
    int in_str;
    int in_com;
    int in_lcom;
    int word_off;
    int word_len;
    int is_number;
    int line_num;
    int line_off;
    int line_len;
    int vis_row;

    /* Header text */
    int hdr;
    int hdr_len;
    hdr = "  TCE v1.0  |  Trinux Code Editor  |  Ctrl-Q: Quit";
    hdr_len = 51;

    /* Filename */
    int fname;
    int fname_len;
    fname = "untitled.c";
    fname_len = 10;

    /* ==============================
     *  MAIN LOOP
     * ============================== */
    while (running) {

        /* ---- Rebuild line index if needed ---- */
        if (rebuild_lines) {
            nlines = 0;
            loff[0] = 0;
            llen[0] = 0;
            i = 0;
            while (i < tlen) {
                if (txt[i] == 10) {
                    llen[nlines] = i - loff[nlines];
                    nlines = nlines + 1;
                    loff[nlines] = i + 1;
                    llen[nlines] = 0;
                }
                i = i + 1;
            }
            /* Last line (may not end with newline) */
            if (tlen > 0 || nlines == 0) {
                llen[nlines] = tlen - loff[nlines];
                nlines = nlines + 1;
            }
            rebuild_lines = 0;
        }

        /* ---- Clamp cursor ---- */
        if (cy >= nlines) cy = nlines - 1;
        if (cy < 0) cy = 0;
        if (cx > llen[cy]) cx = llen[cy];
        if (cx < 0) cx = 0;

        /* ---- Scroll ---- */
        if (cy < scr) scr = cy;
        if (cy >= scr + VISIBLE) scr = cy - VISIBLE + 1;
        if (scr < 0) scr = 0;

        /* ========================================
         *  RENDER HEADER BAR
         * ======================================== */
        col = 0;
        while (col < W) {
            if (col < hdr_len) {
                vga_putchar(col, 0, *(hdr + col) & 0xFF, C_HDR);
            } else {
                vga_putchar(col, 0, 32, C_HDR);
            }
            col = col + 1;
        }

        /* ========================================
         *  RENDER CODE AREA
         * ======================================== */
        vis_row = 0;
        while (vis_row < VISIBLE) {
            line_num = scr + vis_row;
            row = vis_row + 1;

            /* ---- Line number ---- */
            if (line_num < nlines) {
                int ln;
                int d1;
                int d2;
                int d3;
                int d4;
                ln = line_num + 1;
                d4 = ln % 10; ln = ln / 10;
                d3 = ln % 10; ln = ln / 10;
                d2 = ln % 10; ln = ln / 10;
                d1 = ln % 10;

                int ln_color;
                ln_color = C_LN;
                if (line_num == cy) ln_color = C_LNC;

                vga_putchar(0, row, 32, ln_color);
                vga_putchar(1, row, d1 + 48, ln_color);
                vga_putchar(2, row, d2 + 48, ln_color);
                vga_putchar(3, row, d3 + 48, ln_color);
                vga_putchar(4, row, d4 + 48, ln_color);
            } else {
                vga_putchar(0, row, 126, C_SEP);
                col = 1;
                while (col < GUT) {
                    vga_putchar(col, row, 32, C_SEP);
                    col = col + 1;
                }
            }

            /* ---- Gutter separator ---- */
            vga_putchar(GUT, row, 179, C_SEP);

            /* ---- Code with syntax highlighting ---- */
            if (line_num < nlines) {
                line_off = loff[line_num];
                line_len = llen[line_num];

                /* Track context from previous visible line instead of
                 * re-scanning from file start (O(n) → O(1) per line). */
                if (vis_row == 0) {
                    /* First visible line: scan from file start */
                    in_str = 0;
                    in_com = 0;
                    in_lcom = 0;
                    i = 0;
                    while (i < line_off) {
                        int c;
                        c = txt[i];
                        if (in_lcom) {
                            if (c == 10) in_lcom = 0;
                        } else if (in_str) {
                            if (c == 34) in_str = 0;
                            if (c == 92) i = i + 1;
                        } else if (in_com) {
                            if (c == 42 && i + 1 < tlen && txt[i + 1] == 47) {
                                in_com = 0;
                                i = i + 1;
                            }
                        } else {
                            if (c == 47 && i + 1 < tlen && txt[i + 1] == 47) in_lcom = 1;
                            if (c == 47 && i + 1 < tlen && txt[i + 1] == 42) { in_com = 1; i = i + 1; }
                            if (c == 34) in_str = 1;
                        }
                        i = i + 1;
                    }
                }

                /* Now render each character of this line */
                word_off = -1;
                word_len = 0;
                col = 0;
                while (col < line_len) {
                    int pos;
                    pos = line_off + col;
                    ch = txt[pos];

                    /* Determine color */
                    color = C_DEF;
                    if (in_lcom) {
                        color = C_COM;
                    } else if (in_com) {
                        color = C_COM;
                    } else if (in_str) {
                        color = C_STR;
                    } else {
                        /* Check for comment/string starts */
                        if (ch == 47 && col + 1 < line_len && txt[pos + 1] == 47) {
                            color = C_COM;
                            in_lcom = 1;
                        } else if (ch == 47 && col + 1 < line_len && txt[pos + 1] == 42) {
                            color = C_COM;
                            in_com = 1;
                        } else if (ch == 34) {
                            color = C_STR;
                            in_str = 1;
                        } else if (ch >= 48 && ch <= 57) {
                            /* Number */
                            if (word_off < 0) color = C_NUM;
                        } else if ((ch >= 97 && ch <= 122) || (ch >= 65 && ch <= 90) || ch == 95) {
                            /* Letter: part of a word */
                            if (word_off < 0) {
                                word_off = col;
                                word_len = 1;
                            } else {
                                word_len = word_len + 1;
                            }
                        } else {
                            /* Non-letter: end of word */
                            if (word_off >= 0) {
                                if (is_kw(txt, line_off + word_off, word_len)) {
                                    /* Re-color the keyword */
                                    int wc;
                                    wc = 0;
                                    while (wc < word_len) {
                                        vga_putchar(CODEX + word_off - 0 + wc, row,
                                                    txt[line_off + word_off + wc], C_KW);
                                        wc = wc + 1;
                                    }
                                }
                                word_off = -1;
                                word_len = 0;
                            }
                        }
                    }

                    /* Update context for string/comment escapes */
                    if (in_str && !in_lcom && !in_com) {
                        if (ch == 92) col = col + 1;
                    }

                    /* Draw character */
                    int draw_ch;
                    draw_ch = ch;
                    if (draw_ch == 9) draw_ch = 32;
                    if (draw_ch == 10) draw_ch = 32;

                    /* Cursor highlight */
                    int final_color;
                    final_color = color;
                    if (col == cx && line_num == cy) {
                        final_color = C_CUR;
                    }

                    if (CODEX + col < W) {
                        vga_putchar(CODEX + col, row, draw_ch, final_color);
                    }

                    /* Update context after drawing */
                    if (in_lcom && ch == 10) in_lcom = 0;
                    if (in_str && ch == 34) in_str = 0;
                    if (in_com && ch == 42 && col + 1 < line_len && txt[pos + 1] == 47) {
                        in_com = 0;
                    }

                    col = col + 1;
                }

                /* End-of-word check */
                if (word_off >= 0) {
                    if (is_kw(txt, line_off + word_off, word_len)) {
                        int wc;
                        wc = 0;
                        while (wc < word_len) {
                            vga_putchar(CODEX + word_off + wc, row,
                                        txt[line_off + word_off + wc], C_KW);
                            wc = wc + 1;
                        }
                    }
                }

                /* Fill rest of line with spaces */
                while (col < W - CODEX) {
                    int fc;
                    fc = C_DEF;
                    if (line_num == cy && col == cx) fc = C_CUR;
                    vga_putchar(CODEX + col, row, 32, fc);
                    col = col + 1;
                }

                /* Line comments end at newline */
                if (in_lcom) in_lcom = 0;
            } else {
                /* Empty line past EOF */
                col = CODEX;
                while (col < W) {
                    vga_putchar(col, row, 32, C_DEF);
                    col = col + 1;
                }
            }

            vis_row = vis_row + 1;
        }

        /* ========================================
         *  RENDER STATUS BAR
         * ======================================== */
        col = 0;
        while (col < W) {
            vga_putchar(col, H - 1, 32, C_STS);
            col = col + 1;
        }
        /* Filename */
        col = 1;
        while (col < 1 + fname_len && col < W) {
            vga_putchar(col, H - 1, *(fname + col - 1) & 0xFF, C_STS);
            col = col + 1;
        }
        /* Position info */
        int pos_str;
        pos_str = "  Ln ";
        col = fname_len + 2;
        i = 0;
        while (i < 5 && col < W) {
            vga_putchar(col, H - 1, *(pos_str + i) & 0xFF, C_STS);
            col = col + 1;
            i = i + 1;
        }
        /* Line number */
        int tmp;
        tmp = cy + 1;
        if (tmp >= 100) { vga_putchar(col, H - 1, (tmp / 100) + 48, C_STS); col = col + 1; }
        if (tmp >= 10)  { vga_putchar(col, H - 1, ((tmp / 10) % 10) + 48, C_STS); col = col + 1; }
        vga_putchar(col, H - 1, (tmp % 10) + 48, C_STS);
        col = col + 1;
        /* Col */
        pos_str = " Col ";
        i = 0;
        while (i < 5 && col < W) {
            vga_putchar(col, H - 1, *(pos_str + i) & 0xFF, C_STS);
            col = col + 1;
            i = i + 1;
        }
        tmp = cx + 1;
        if (tmp >= 100) { vga_putchar(col, H - 1, (tmp / 100) + 48, C_STS); col = col + 1; }
        if (tmp >= 10)  { vga_putchar(col, H - 1, ((tmp / 10) % 10) + 48, C_STS); col = col + 1; }
        vga_putchar(col, H - 1, (tmp % 10) + 48, C_STS);
        col = col + 1;
        /* Modified indicator */
        if (dirty) {
            pos_str = "  [Modified]";
            i = 0;
            while (i < 12 && col < W) {
                vga_putchar(col, H - 1, *(pos_str + i) & 0xFF, C_STS);
                col = col + 1;
                i = i + 1;
            }
        }

        /* ========================================
         *  GET KEY
         * ======================================== */
        key = getchar();

        /* ========================================
         *  HANDLE KEY
         * ======================================== */

        /* Ctrl-Q: quit */
        if (key == 17) {
            running = 0;
        }

        /* Arrow left */
        if (key == K_LT) {
            if (cx > 0) {
                cx = cx - 1;
            } else if (cy > 0) {
                cy = cy - 1;
                cx = llen[cy];
            }
        }

        /* Arrow right */
        if (key == K_RT) {
            if (cx < llen[cy]) {
                cx = cx + 1;
            } else if (cy + 1 < nlines) {
                cy = cy + 1;
                cx = 0;
            }
        }

        /* Arrow up */
        if (key == K_UP) {
            if (cy > 0) {
                cy = cy - 1;
                if (cx > llen[cy]) cx = llen[cy];
            }
        }

        /* Arrow down */
        if (key == K_DN) {
            if (cy + 1 < nlines) {
                cy = cy + 1;
                if (cx > llen[cy]) cx = llen[cy];
            }
        }

        /* Home */
        if (key == K_HOME) {
            cx = 0;
        }

        /* End */
        if (key == K_END) {
            cx = llen[cy];
        }

        /* Backspace */
        if (key == 8) {
            if (cx > 0 || cy > 0) {
                /* Delete character before cursor */
                int pos;
                if (cx > 0) {
                    pos = loff[cy] + cx - 1;
                    /* Shift text left */
                    i = pos;
                    while (i < tlen - 1) {
                        txt[i] = txt[i + 1];
                        i = i + 1;
                    }
                    tlen = tlen - 1;
                    cx = cx - 1;
                } else {
                    /* Join with previous line */
                    pos = loff[cy] - 1;
                    cx = llen[cy - 1];
                    /* Remove the newline */
                    i = pos;
                    while (i < tlen - 1) {
                        txt[i] = txt[i + 1];
                        i = i + 1;
                    }
                    tlen = tlen - 1;
                    cy = cy - 1;
                }
                dirty = 1;
                rebuild_lines = 1;
            }
        }

        /* Delete */
        if (key == K_DEL) {
            if (cx < llen[cy] || cy + 1 < nlines) {
                int pos;
                pos = loff[cy] + cx;
                i = pos;
                while (i < tlen - 1) {
                    txt[i] = txt[i + 1];
                    i = i + 1;
                }
                tlen = tlen - 1;
                dirty = 1;
                rebuild_lines = 1;
            }
        }

        /* Enter */
        if (key == 10) {
            if (tlen < 32760) {
                /* Shift text right */
                i = tlen;
                while (i > loff[cy] + cx) {
                    txt[i] = txt[i - 1];
                    i = i - 1;
                }
                /* Insert newline */
                txt[loff[cy] + cx] = 10;
                tlen = tlen + 1;
                cy = cy + 1;
                cx = 0;
                dirty = 1;
                rebuild_lines = 1;
            }
        }

        /* Printable character */
        if (key >= 32 && key <= 126) {
            if (tlen < 32760) {
                /* Shift text right */
                i = tlen;
                while (i > loff[cy] + cx) {
                    txt[i] = txt[i - 1];
                    i = i - 1;
                }
                /* Insert character */
                txt[loff[cy] + cx] = key;
                tlen = tlen + 1;
                cx = cx + 1;
                dirty = 1;
                rebuild_lines = 1;
            }
        }

    } /* end while(running) */

    /* Clean exit: clear screen and print message */
    vga_clear(0x07);
    print("TCE: editor closed.\n");
}

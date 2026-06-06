/* cide — Code IDE (editor + compilador integrado). Ring 3.
 *
 * Basado en /bin/edit, pero con:
 *   - Syntax highlighting para C (keywords azules, strings verdes,
 *     comments grises oscuro, numbers amarillos)
 *   - Ctrl-B: guarda y compila con tcc (usa SYS_TCC_COMPILE para
 *     no spawnear otro proceso, asi el state del editor sobrevive)
 *   - F1 (key 1) o '?': ayuda
 *   - F5 (key 5) o Ctrl-R: corre el binario compilado (sin .c)
 *   - Ctrl-S / Ctrl-X / Ctrl-Q como en edit
 *
 * Notas:
 *   - Trabaja con 80x50 (modo VGA actual).
 *   - 200 lineas x 80 col max.
 *   - Syntax highlight: muy simple, basado en clases lexicas por linea.
 */
#include "../trinux.h"

#define MAX_LINES   200
#define MAX_LINE    80
#define SCREEN_W    80
#define SCREEN_H    50
#define CONTENT_H   48    /* 50 - 2 status bars */

static char buf[MAX_LINES][MAX_LINE];
static int  line_len[MAX_LINES];
static int  n_lines = 1;
static int  cx = 0, cy = 0;
static int  view_top = 0;
static char filename[64] = "untitled.c";
static int  dirty = 0;
static char last_msg[80] = "";
static int  last_msg_fg = 7, last_msg_bg = 0;

#define BLACK 0
#define BLUE 1
#define GREEN 2
#define CYAN 3
#define RED 4
#define MAGENTA 5
#define BROWN 6
#define LIGHT_GREY 7
#define DARK_GREY 8
#define LIGHT_BLUE 9
#define LIGHT_GREEN 10
#define LIGHT_CYAN 11
#define LIGHT_RED 12
#define LIGHT_MAGENTA 13
#define YELLOW 14
#define WHITE 15

/* ----- C keywords para highlight ----- */
static const char *c_keywords[] = {
    "int", "char", "void", "if", "else", "while", "for", "do",
    "return", "break", "continue", "static", "const", "struct",
    "typedef", "enum", "union", "sizeof", "unsigned", "signed",
    "long", "short", "float", "double", "auto", "extern", "register",
    "switch", "case", "default", "goto", 0
};

static int strlen3(const char *s){int n=0;while(s[n])n++;return n;}
static int is_alpha(int c){ return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'; }
static int is_digit(int c){ return c>='0'&&c<='9'; }
static int is_alnum(int c){ return is_alpha(c)||is_digit(c); }

static int is_keyword(const char *s, int len) {
    for (int k = 0; c_keywords[k]; k++) {
        int i = 0;
        while (i < len && c_keywords[k][i] && s[i] == c_keywords[k][i]) i++;
        if (i == len && c_keywords[k][i] == 0) return 1;
    }
    return 0;
}

/* ----- Estado del lexer entre lineas (para /* comments multilinea) ----- */
static int in_block_comment;

/* ----- File I/O ----- */
static void load_file(const char *name){
    if(name){int i=0; while(name[i] && i<63){filename[i]=name[i]; i++;} filename[i]=0;}
    static char raw[16384];
    int n = readfile(filename, raw, sizeof(raw)-1);
    if(n<0){ n_lines=1; line_len[0]=0; buf[0][0]=0; return; }
    raw[n]=0;
    n_lines = 0;
    int li = 0;
    line_len[0] = 0; buf[0][0] = 0;
    for(int i=0;i<n;i++){
        if(raw[i]=='\n'){
            buf[li][line_len[li]] = 0;
            li++;
            if(li>=MAX_LINES) break;
            line_len[li]=0; buf[li][0]=0;
        } else if(raw[i]=='\r'){}
        else {
            if(line_len[li] < MAX_LINE-1){
                buf[li][line_len[li]++] = raw[i];
                buf[li][line_len[li]] = 0;
            }
        }
    }
    n_lines = li + 1;
    if(n_lines == 0) n_lines = 1;
}

static int save_file(void){
    static char out[16384];
    int o = 0;
    for(int i=0;i<n_lines && o<(int)sizeof(out)-2;i++){
        for(int j=0;j<line_len[i] && o<(int)sizeof(out)-2;j++) out[o++] = buf[i][j];
        out[o++] = '\n';
    }
    unlink(filename);
    return writefile(filename, out, o);
}

/* ----- Helpers de UI ----- */
static void set_message(const char *m, int fg, int bg){
    int i=0; while(m[i] && i<79){ last_msg[i]=m[i]; i++; } last_msg[i]=0;
    last_msg_fg = fg; last_msg_bg = bg;
}

static void draw_status_top(void){
    vga_goto(0, 0);
    vga_color(WHITE, BLUE);
    print("  cide ");
    int p=7;
    print(filename); p += strlen3(filename);
    if(dirty){ print(" [modified]"); p += 11; }
    /* indicar modo: C-source si .c, plain si no */
    int fl = strlen3(filename);
    if(fl >= 2 && filename[fl-2]=='.' && filename[fl-1]=='c'){
        print("  [C mode]"); p += 10;
    }
    for(int i=p; i<SCREEN_W; i++) putchar_(' ');
    vga_color(LIGHT_GREY, BLACK);
}

static void draw_status_bot(void){
    vga_goto(0, SCREEN_H-1);
    if(last_msg[0]){
        vga_color(last_msg_fg, last_msg_bg);
        print(" "); print(last_msg);
        int p = strlen3(last_msg) + 1;
        for(int i=p;i<SCREEN_W;i++) putchar_(' ');
    } else {
        vga_color(BLACK, LIGHT_GREY);
        print(" ^S save   ^X save+exit   ^Q quit   ^B build   F5 run   line ");
        print_num(cy+1); print("/"); print_num(n_lines);
        print("  col "); print_num(cx+1);
        int p = 56 + 8 + 8;  /* aproximado */
        for(int i=p;i<SCREEN_W;i++) putchar_(' ');
    }
    vga_color(LIGHT_GREY, BLACK);
}

/* ----- Highlight: pinta UNA linea con colores apropiados ----- */
static int line_ends_with_c(const char *fn){
    int n = strlen3(fn);
    return (n >= 2 && fn[n-2]=='.' && fn[n-1]=='c');
}

static void draw_line_highlighted(int li, int screen_row){
    vga_goto(0, screen_row);
    vga_color(LIGHT_GREY, BLACK);
    const char *s = buf[li];
    int len = line_len[li];

    if(!line_ends_with_c(filename)){
        /* plain mode: solo imprimir */
        for(int i=0;i<len && i<SCREEN_W;i++) putchar_(s[i]);
        for(int i=len;i<SCREEN_W;i++) putchar_(' ');
        return;
    }

    int col = 0;
    int i = 0;
    while(i < len && col < SCREEN_W){
        /* comentario de bloque /\* \.\.\. *\//
        if(in_block_comment){
            vga_color(DARK_GREY, BLACK);
            while(i < len && col < SCREEN_W){
                if(i+1 < len && s[i]=='*' && s[i+1]=='/'){
                    putchar_('*'); putchar_('/'); col+=2; i+=2;
                    in_block_comment = 0;
                    break;
                }
                putchar_(s[i++]); col++;
            }
            vga_color(LIGHT_GREY, BLACK);
            continue;
        }
        /* comentario //  */
        if(i+1 < len && s[i]=='/' && s[i+1]=='/'){
            vga_color(DARK_GREY, BLACK);
            while(i < len && col < SCREEN_W){ putchar_(s[i++]); col++; }
            vga_color(LIGHT_GREY, BLACK);
            continue;
        }
        /* inicio comentario bloque */
        if(i+1 < len && s[i]=='/' && s[i+1]=='*'){
            in_block_comment = 1;
            vga_color(DARK_GREY, BLACK);
            putchar_('/'); putchar_('*'); col+=2; i+=2;
            while(i < len && col < SCREEN_W){
                if(i+1 < len && s[i]=='*' && s[i+1]=='/'){
                    putchar_('*'); putchar_('/'); col+=2; i+=2;
                    in_block_comment = 0;
                    break;
                }
                putchar_(s[i++]); col++;
            }
            vga_color(LIGHT_GREY, BLACK);
            continue;
        }
        /* strings "..." */
        if(s[i]=='"'){
            vga_color(LIGHT_GREEN, BLACK);
            putchar_('"'); col++; i++;
            while(i < len && col < SCREEN_W && s[i] != '"'){
                if(s[i]=='\\' && i+1 < len){
                    putchar_(s[i++]); col++;
                    if(col < SCREEN_W && i < len){ putchar_(s[i++]); col++; }
                } else { putchar_(s[i++]); col++; }
            }
            if(i < len && col < SCREEN_W){ putchar_(s[i++]); col++; }
            vga_color(LIGHT_GREY, BLACK);
            continue;
        }
        /* chars '...' */
        if(s[i]=='\''){
            vga_color(LIGHT_GREEN, BLACK);
            putchar_('\''); col++; i++;
            while(i < len && col < SCREEN_W && s[i] != '\''){
                putchar_(s[i++]); col++;
            }
            if(i < len && col < SCREEN_W){ putchar_(s[i++]); col++; }
            vga_color(LIGHT_GREY, BLACK);
            continue;
        }
        /* numeros */
        if(is_digit(s[i])){
            vga_color(YELLOW, BLACK);
            while(i < len && col < SCREEN_W && (is_alnum(s[i]) || s[i]=='.' || s[i]=='x' || s[i]=='X')){
                putchar_(s[i++]); col++;
            }
            vga_color(LIGHT_GREY, BLACK);
            continue;
        }
        /* identifiers / keywords */
        if(is_alpha(s[i])){
            int start = i;
            while(i < len && is_alnum(s[i])) i++;
            int wlen = i - start;
            if(is_keyword(&s[start], wlen)){
                vga_color(LIGHT_BLUE, BLACK);
            } else {
                vga_color(LIGHT_CYAN, BLACK);  /* identifiers en cyan */
            }
            for(int k = start; k < i && col < SCREEN_W; k++){ putchar_(s[k]); col++; }
            vga_color(LIGHT_GREY, BLACK);
            continue;
        }
        /* preprocessor */
        if(s[i]=='#' && (col == 0 || s[i-1]==' ')){
            vga_color(LIGHT_MAGENTA, BLACK);
            while(i < len && col < SCREEN_W && s[i] != ' '){
                putchar_(s[i++]); col++;
            }
            vga_color(LIGHT_GREY, BLACK);
            continue;
        }
        /* operadores y demas */
        if(s[i]=='{' || s[i]=='}' || s[i]=='(' || s[i]==')' ||
           s[i]==';' || s[i]==',' || s[i]=='[' || s[i]==']'){
            vga_color(WHITE, BLACK);
            putchar_(s[i++]); col++;
            vga_color(LIGHT_GREY, BLACK);
            continue;
        }
        /* default */
        putchar_(s[i++]); col++;
    }
    /* limpiar resto de la linea */
    while(col < SCREEN_W){ putchar_(' '); col++; }
    vga_color(LIGHT_GREY, BLACK);
}

static void draw_content(void){
    in_block_comment = 0;
    /* re-procesar desde linea 0 hasta view_top para tracking de /\* ... *\/ */
    for(int li = 0; li < view_top; li++){
        const char *s = buf[li]; int len = line_len[li];
        for(int i = 0; i < len; i++){
            if(in_block_comment){
                if(i+1 < len && s[i]=='*' && s[i+1]=='/'){ in_block_comment = 0; i++; }
            } else {
                if(i+1 < len && s[i]=='/' && s[i+1]=='/') break; /* line comment */
                if(i+1 < len && s[i]=='/' && s[i+1]=='*'){ in_block_comment = 1; i++; }
            }
        }
    }
    /* dibujar lineas visibles */
    for(int row=0; row<CONTENT_H; row++){
        int li = view_top + row;
        if(li < n_lines){
            draw_line_highlighted(li, row+1);
        } else {
            vga_goto(0, row+1);
            for(int c=0;c<SCREEN_W;c++) putchar_(' ');
        }
    }
}

static void place_cursor(void){
    int sr = (cy - view_top) + 1;
    int sc = cx;
    if(sc >= SCREEN_W) sc = SCREEN_W-1;
    if(sr < 1) sr = 1; if(sr > CONTENT_H) sr = CONTENT_H;
    vga_goto(sc, sr);
}

static void redraw(void){
    if(cy < view_top) view_top = cy;
    if(cy >= view_top + CONTENT_H) view_top = cy - CONTENT_H + 1;
    if(view_top < 0) view_top = 0;
    draw_status_top();
    draw_content();
    draw_status_bot();
    place_cursor();
}

/* ----- Edicion ----- */
static void insert_char(int c){
    if(line_len[cy] >= MAX_LINE-1) return;
    for(int i = line_len[cy]; i > cx; i--) buf[cy][i] = buf[cy][i-1];
    buf[cy][cx] = (char)c;
    line_len[cy]++; buf[cy][line_len[cy]] = 0;
    cx++; dirty = 1;
}

static void newline(void){
    if(n_lines >= MAX_LINES-1) return;
    for(int i = n_lines; i > cy+1; i--){
        line_len[i] = line_len[i-1];
        for(int j=0;j<line_len[i];j++) buf[i][j] = buf[i-1][j];
        buf[i][line_len[i]] = 0;
    }
    int rest = line_len[cy] - cx;
    for(int j=0;j<rest;j++) buf[cy+1][j] = buf[cy][cx+j];
    line_len[cy+1] = rest; buf[cy+1][rest] = 0;
    line_len[cy] = cx; buf[cy][cx] = 0;
    n_lines++; cy++; cx = 0; dirty = 1;
}

static void backspace_op(void){
    if(cx > 0){
        for(int i = cx-1; i < line_len[cy]-1; i++) buf[cy][i] = buf[cy][i+1];
        line_len[cy]--; buf[cy][line_len[cy]] = 0;
        cx--; dirty = 1;
    } else if(cy > 0){
        int prev_len = line_len[cy-1];
        if(prev_len + line_len[cy] < MAX_LINE-1){
            for(int j=0;j<line_len[cy];j++) buf[cy-1][prev_len+j] = buf[cy][j];
            line_len[cy-1] = prev_len + line_len[cy];
            buf[cy-1][line_len[cy-1]] = 0;
            for(int i=cy;i<n_lines-1;i++){
                line_len[i] = line_len[i+1];
                for(int j=0;j<line_len[i];j++) buf[i][j] = buf[i+1][j];
                buf[i][line_len[i]] = 0;
            }
            n_lines--; cy--; cx = prev_len;
            dirty = 1;
        }
    }
}

/* ----- Build & Run ----- */
static int do_build(void){
    /* Guarda primero. */
    if(save_file() < 0){
        set_message("BUILD: save failed", WHITE, LIGHT_RED);
        return -1;
    }
    /* Llama al syscall TCC_COMPILE (compila usando el built-in del kernel)
     * en lugar del /bin/tcc puro, porque el wrapper de kernel no abandona
     * la pantalla mientras compila. */
    int rc = tcc_compile(filename);
    if(rc < 0){
        set_message("BUILD: compilation failed (syntax errors)", WHITE, LIGHT_RED);
        return -1;
    }
    char m[80]; int p=0;
    const char *prefix = "BUILD OK: compiled to ";
    int j=0; while(prefix[j] && p<79){ m[p++]=prefix[j++]; }
    j=0; while(filename[j] && p<76){ m[p++]=filename[j++]; }
    /* quitar .c si lo hay */
    if(p>=2 && m[p-2]=='.' && m[p-1]=='c') p -= 2;
    m[p]=0;
    set_message(m, WHITE, GREEN);
    return 0;
}

static int do_run(void){
    /* Construir nombre del binario (quitar .c) */
    char binpath[64];
    int i=0; while(filename[i] && i<63){ binpath[i]=filename[i]; i++; }
    binpath[i] = 0;
    if(i >= 2 && binpath[i-2]=='.' && binpath[i-1]=='c') binpath[i-2] = 0;
    /* Verificar que existe */
    trinux_stat_t st;
    if(stat(binpath, &st) < 0){
        set_message("RUN: binary not found - build first (^B)", WHITE, LIGHT_RED);
        return -1;
    }
    /* Salir de pantalla completa, correr, esperar tecla, volver */
    vga_clear_();
    vga_goto(0,0);
    vga_color(LIGHT_CYAN, BLACK);
    print("=== Running "); print(binpath); print(" ===\n");
    vga_color(LIGHT_GREY, BLACK);

    /* spawn */
    char *args[2]; args[0] = binpath; args[1] = 0;
    spawn(binpath, args, SPAWN_F_WAIT);

    vga_color(LIGHT_CYAN, BLACK);
    print("\n=== Press any key to return to editor ===\n");
    vga_color(LIGHT_GREY, BLACK);
    key_raw();
    vga_clear_();
    redraw();
    return 0;
}

static void show_help(void){
    vga_clear_();
    vga_goto(0,0);
    vga_color(LIGHT_CYAN, BLACK);
    print("=== cide: Code IDE para Trinux (ring 3) ===\n\n");
    vga_color(LIGHT_GREY, BLACK);
    print("Edicion:\n");
    print("  flechas        mover cursor\n");
    print("  letras         insertar\n");
    print("  Enter          nueva linea\n");
    print("  Backspace      borrar atras\n");
    print("\nArchivo:\n");
    print("  ^S             guardar\n");
    print("  ^X             guardar y salir\n");
    print("  ^Q             salir sin guardar\n");
    print("\nCompilador (solo para .c):\n");
    print("  ^B             build (compila con tcc)\n");
    print("  F5             run (ejecuta el binario)\n");
    print("\nHighlight de colores (modo .c):\n");
    vga_color(LIGHT_BLUE, BLACK);  print("  keywords ");
    vga_color(LIGHT_CYAN, BLACK);  print("identifiers ");
    vga_color(LIGHT_GREEN, BLACK); print("strings ");
    vga_color(YELLOW, BLACK);      print("numbers ");
    vga_color(DARK_GREY, BLACK);   print("comments ");
    vga_color(LIGHT_MAGENTA, BLACK); print("preproc");
    vga_color(LIGHT_GREY, BLACK);
    print("\n\nPress any key to continue...");
    key_raw();
    vga_clear_();
    redraw();
}

int main(int argc, char **argv){
    if(argc < 2){
        print("usage: cide <file>\n");
        print("  Code IDE: editor con syntax highlight + tcc integrado\n");
        print("  ^B compila, F5 ejecuta, F1 ayuda\n");
        return 1;
    }
    load_file(argv[1]);
    vga_clear_();
    redraw();
    for(;;){
        int k = key_raw();
        if(k == KEY_F_UP){if(cy > 0){cy--; if(cx > line_len[cy]) cx = line_len[cy];}}
        else if(k == KEY_F_DOWN){if(cy < n_lines-1){cy++; if(cx > line_len[cy]) cx = line_len[cy];}}
        else if(k == KEY_F_LEFT){if(cx > 0) cx--; else if(cy > 0){cy--; cx = line_len[cy];}}
        else if(k == KEY_F_RIGHT){if(cx < line_len[cy]) cx++; else if(cy < n_lines-1){cy++; cx = 0;}}
        else if(k == KEY_F_HOME){cx = 0;}
        else if(k == KEY_F_END){cx = line_len[cy];}
        else if(k == '\n'){newline(); last_msg[0]=0;}
        else if(k == '\b'){backspace_op(); last_msg[0]=0;}
        else if(k == 19){ /* ^S */
            if(save_file() >= 0){dirty = 0; set_message("saved", WHITE, GREEN);}
            else set_message("save failed", WHITE, LIGHT_RED);
        }
        else if(k == 24){ /* ^X */ if(dirty) save_file(); vga_clear_(); vga_goto(0,0); return 0; }
        else if(k == 17){ /* ^Q */ vga_clear_(); vga_goto(0,0); return 0; }
        else if(k == 2){  /* ^B = build */ do_build(); }
        else if(k == 18){ /* ^R = run (algunos teclados envian F5 como ese) */ do_run(); }
        else if(k == 5){  /* F5 si llega como ese */ do_run(); }
        else if(k == 6){  /* F1=6 alternativa, '?' tambien */ show_help(); }
        else if(k == '?'){show_help();}
        else if(k >= 32 && k < 127){insert_char(k); last_msg[0]=0;}
        redraw();
    }
}

#include "../trinux.h"
#define MAX_LINES   200
#define MAX_LINE    80
#define SCREEN_W    80
#define SCREEN_H    50    /* modo 80x50 con fuente 8x8 */
#define CONTENT_H   48    /* 50 - 2 (status bars arriba+abajo) */
static char buf[MAX_LINES][MAX_LINE];
static int  line_len[MAX_LINES];
static int  n_lines = 1;
static int  cx = 0, cy = 0;
static int  view_top = 0;
static char filename[64] = "untitled.txt";
static int  dirty = 0;
#define BLACK 0
#define LIGHT_GREY 7
#define WHITE 15
#define LIGHT_BLUE 9
#define LIGHT_RED 12
#define GREEN 2
static int strlen3(const char *s){int n=0;while(s[n])n++;return n;}
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
static void draw_status_top(void){
    vga_goto(0, 0);
    vga_color(WHITE, LIGHT_BLUE);
    print("  edit ");
    int p=0; print(filename); p += strlen3(filename);
    if(dirty){ print(" [modified]"); p += 11; }
    for(int i=7+p; i<SCREEN_W; i++) putchar_(' ');
    vga_color(LIGHT_GREY, BLACK);
}
static void draw_status_bot(void){
    vga_goto(0, SCREEN_H-1);
    vga_color(BLACK, LIGHT_GREY);
    print(" ^S save   ^X save+exit   ^Q quit   line ");
    print_num(cy+1); print("/"); print_num(n_lines);
    print("  col "); print_num(cx+1);
    for(int i=0;i<20;i++) putchar_(' ');
    vga_color(LIGHT_GREY, BLACK);
}
static void draw_content(void){
    for(int row=0; row<CONTENT_H; row++){
        vga_goto(0, row+1);
        int li = view_top + row;
        if(li < n_lines){
            int max = line_len[li] < SCREEN_W ? line_len[li] : SCREEN_W;
            for(int c=0;c<max;c++) putchar_(buf[li][c]);
            for(int c=max;c<SCREEN_W;c++) putchar_(' ');
        } else {
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
    draw_status_top(); draw_content(); draw_status_bot(); place_cursor();
}
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
static void backspace(void){
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
            n_lines--; cy--; cx = prev_len; dirty = 1;
        }
    }
}
static void show_message(const char *msg, int fg, int bg){
    vga_goto(0, SCREEN_H-1);
    vga_color(fg, bg);
    print(" "); print(msg);
    int p = strlen3(msg) + 1;
    for(int i=p;i<SCREEN_W;i++) putchar_(' ');
    vga_color(LIGHT_GREY, BLACK);
    msleep(700);
}
int main(int argc, char **argv){
    if(argc < 2){print("usage: edit <file>\n");return 1;}
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
        else if(k == '\n'){newline();}
        else if(k == '\b'){backspace();}
        else if(k == 19){
            if(save_file() >= 0){dirty = 0; show_message("saved", WHITE, GREEN);}
            else show_message("save failed", WHITE, LIGHT_RED);
        }
        else if(k == 24){if(dirty) save_file(); vga_clear_(); vga_goto(0,0); return 0;}
        else if(k == 17){vga_clear_(); vga_goto(0,0); return 0;}
        else if(k >= 32 && k < 127){insert_char(k);}
        redraw();
    }
}

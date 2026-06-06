#!/usr/bin/env bash
# FASE5_REGEN.sh — Recrea todos los archivos generados/perdidos de fases 3-5.
#
# Uso: bash FASE5_REGEN.sh
#
# Genera:
#   - user/usersh/sh.c          (shell ring-3 con pipes)
#   - user/coreutils/*.c        (35 archivos: cp, mv, top, edit, tcc, ...)
#   - user/coreutils/build.sh   (con la tabla completa de 64 binarios)
#
# Luego corre `bash user/coreutils/build.sh && make` para compilar todo.
set -e
cd "$(dirname "$0")"

mkdir -p user/usersh user/coreutils
echo "guard" > user/usersh/.gitkeep
echo "Trinux shell ring-3 sources" > user/usersh/README.txt

# ============================================================
# user/usersh/sh.c
# ============================================================
cat > user/usersh/sh.c <<'SHELL_C'
/* user/usersh/sh.c — Trinux shell ring-3 con pipes */
#include "../trinux.h"

#define LINE_MAX  256
#define ARGS_MAX  16
#define HIST_MAX  16

static char history[HIST_MAX][LINE_MAX];
static int  hist_count, hist_head;
static char g_user[64] = "user";
static char g_host[64] = "trinux";
static int  g_is_root  = 0;
static char tmpn[8][16];
static char gt_str[2];

#define BLACK 0
#define LIGHT_GREY 7
#define LIGHT_BLUE 9
#define LIGHT_GREEN 10
#define LIGHT_CYAN 11
#define LIGHT_RED 12
#define WHITE 15

static void strcpy_(char *d, const char *s){ while ((*d++=*s++)); }
static int  strlen2_(const char *s){ int n=0; while(s[n])n++; return n; }
static void str_append(char *d, const char *s){
    int dl = strlen2_(d);
    int i=0; while (s[i]){ d[dl+i]=s[i]; i++; }
    d[dl+i]=0;
}

/* Re-sincroniza identidad del proceso shell con el kernel.
 * Llamada después de SU o LOGOUT para que el prompt refleje el cambio. */
static void refresh_identity(void){
    hostname(g_host, sizeof(g_host));
    getuser(g_user, sizeof(g_user));
    g_is_root = (getuid() == 0);
}

static void show_prompt(void){
    char cwd[128]; getcwd(cwd, sizeof(cwd));
    vga_color(g_is_root ? LIGHT_RED : LIGHT_GREEN, BLACK);
    print(g_user); print("@"); print(g_host);
    vga_color(LIGHT_GREY, BLACK); print(":");
    vga_color(LIGHT_BLUE, BLACK); print(cwd);
    vga_color(LIGHT_GREY, BLACK); print(g_is_root ? "# " : "$ ");
}

static void add_history(const char *line){
    if (!line[0]) return;
    int i = 0;
    while (line[i] && i < LINE_MAX-1) { history[hist_head][i] = line[i]; i++; }
    history[hist_head][i] = 0;
    hist_head = (hist_head + 1) % HIST_MAX;
    if (hist_count < HIST_MAX) hist_count++;
}

static int read_line(char *buf, int max){
    int len = 0;
    buf[0] = 0;
    for (;;) {
        int k = key_raw();
        if (k == '\n') { putchar_('\n'); buf[len] = 0; return len; }
        if (k == '\b') {
            if (len > 0) { len--; putchar_('\b'); putchar_(' '); putchar_('\b'); }
        } else if (k >= 32 && k < 127) {
            if (len < max - 1) { buf[len++] = (char)k; putchar_((char)k); }
        }
    }
}

static int tokenize(char *line, char **argv){
    static char scratch[LINE_MAX];
    int argc = 0;
    char *src = line, *dst = scratch;
    char *end = scratch + sizeof(scratch) - 1;
    while (*src && argc < ARGS_MAX-1 && dst < end) {
        while (*src == ' ' || *src == '\t') src++;
        if (!*src) break;
        if (*src == '>') {
            argv[argc++] = dst;
            *dst++ = '>'; src++;
            if (*src == '>' && dst < end) { *dst++ = '>'; src++; }
            if (dst < end) *dst++ = 0;
            continue;
        }
        if (*src == '|') {
            argv[argc++] = dst;
            *dst++ = '|'; src++;
            if (dst < end) *dst++ = 0;
            continue;
        }
        argv[argc++] = dst;
        int q = 0;
        while (*src && dst < end) {
            if (*src == '"') { q = !q; src++; continue; }
            if (!q && (*src == ' ' || *src == '\t')) break;
            if (!q && (*src == '>' || *src == '|')) break;
            *dst++ = *src++;
        }
        if (dst < end) *dst++ = 0;
    }
    argv[argc] = 0;
    return argc;
}

static int run_one(int argc, char **argv){
    if (argc == 0) return 0;
    const char *redir_out = 0;
    int append = 0;
    for (int i = 0; i < argc; i++) {
        if (argv[i][0] == '>' && argv[i][1] == 0) {
            if (i+1 >= argc) { print("syntax: > needs filename\n"); return 2; }
            append = 0; redir_out = argv[i+1];
            for (int j = i; j+2 <= argc; j++) argv[j] = argv[j+2];
            argc -= 2; i--;
        } else if (argv[i][0] == '>' && argv[i][1] == '>' && argv[i][2] == 0) {
            if (i+1 >= argc) { print("syntax: >> needs filename\n"); return 2; }
            append = 1; redir_out = argv[i+1];
            for (int j = i; j+2 <= argc; j++) argv[j] = argv[j+2];
            argc -= 2; i--;
        }
    }
    if (argc == 0) return 0;
    char path[128];
    if (argv[0][0] == '/' || (argv[0][0]=='.' && argv[0][1]=='/')) {
        strcpy_(path, argv[0]);
    } else {
        strcpy_(path, "/bin/");
        str_append(path, argv[0]);
    }
    if (redir_out) {
        spawn_req_t r;
        r.path = path; r.argv = argv;
        r.stdin_path = 0; r.stdout_path = redir_out; r.append = append;
        return spawn_r(&r);
    }
    return spawn(path, argv, SPAWN_F_WAIT);
}

static void init_pipe_state(void){
    for (int i = 0; i < 8; i++) {
        tmpn[i][0]='/'; tmpn[i][1]='t'; tmpn[i][2]='m'; tmpn[i][3]='p';
        tmpn[i][4]='/'; tmpn[i][5]='.'; tmpn[i][6]='p';
        tmpn[i][7]='0' + i; tmpn[i][8] = 0;
    }
    gt_str[0] = '>'; gt_str[1] = 0;
}

static int dispatch(int argc, char **argv){
    if (argc == 0) return 0;
    if (streq(argv[0], "exit")) { exit(0); }
    if (streq(argv[0], "cd")) {
        const char *t = (argc >= 2) ? argv[1] : "/";
        if (chdir(t) < 0) { print("cd: "); print(t); print(": no such directory\n"); return 1; }
        return 0;
    }
    if (streq(argv[0], "help")) {
        print("Built-ins: cd exit help history clear\n");
        print("Comandos en /bin/<nombre>  (ring 3 reales)\n");
        print("Redir:  cmd > file   cmd >> file\n");
        print("Pipes:  cmd1 | cmd2 | cmd3  (max 8 stages)\n");
        print("Try:    top, edit FILE, tcc src.c, ringtest\n");
        return 0;
    }
    if (streq(argv[0], "history")) {
        for (int i = 0; i < hist_count; i++) {
            int idx = (hist_head - hist_count + HIST_MAX*2 + i) % HIST_MAX;
            print_num(i+1); print("  "); println(history[idx]);
        }
        return 0;
    }
    if (streq(argv[0], "clear")) { clrscr(); return 0; }

    int n_stages = 1;
    for (int i = 0; i < argc; i++)
        if (argv[i][0] == '|' && argv[i][1] == 0) n_stages++;

    int rc;
    if (n_stages == 1) {
        rc = run_one(argc, argv);
    } else if (n_stages > 8) {
        print("too many pipes (max 8 stages)\n");
        rc = 1;
    } else {
        char *stage[ARGS_MAX];
        int start = 0, si = 0;
        rc = 0;
        for (int i = 0; i <= argc; i++) {
            int boundary = (i == argc) || (argv[i][0] == '|' && argv[i][1] == 0);
            if (!boundary) continue;
            int sn = 0;
            for (int k = start; k < i && sn < ARGS_MAX-3; k++) stage[sn++] = argv[k];
            if (si > 0 && sn < ARGS_MAX-3) stage[sn++] = tmpn[si-1];
            if (si < n_stages-1 && sn < ARGS_MAX-3) { stage[sn++] = gt_str; stage[sn++] = tmpn[si]; }
            stage[sn] = 0;
            rc = run_one(sn, stage);
            si++; start = i + 1;
        }
        for (int i = 0; i < n_stages-1; i++) unlink(tmpn[i]);
    }

    /* === FIX BUG #2 ===
     * Algunos comandos cambian la identidad del proceso (su, login, logout).
     * Como esos comandos viven en procesos hijos separados, su efecto sobre
     * el shell padre solo se ve si refrescamos g_user/g_is_root desde el
     * kernel. Refrescamos siempre tras spawn — barato y correcto. */
    if (streq(argv[0], "su") || streq(argv[0], "login") || streq(argv[0], "logout")) {
        refresh_identity();
    }
    return rc;
}

static void do_login(void){
    char user[64], pass[64];
    hostname(g_host, sizeof(g_host));
    if (getuid() == 0) {
        getuser(g_user, sizeof(g_user));
        g_is_root = 1;
        return;
    }
    for (;;) {
        vga_color(WHITE, BLACK);
        print(g_host); print(" login: ");
        vga_color(LIGHT_GREY, BLACK);
        int n = getline_(user, sizeof(user));
        if (n <= 0) continue;
        print("Password: ");
        int pl = 0;
        for (;;) {
            int k = key_raw();
            if (k == '\n') { putchar_('\n'); pass[pl] = 0; break; }
            if (k == '\b') { if (pl > 0) { pl--; putchar_('\b'); putchar_(' '); putchar_('\b'); } }
            else if (k >= 32 && k < 127) {
                if (pl < (int)sizeof(pass)-1) { pass[pl++] = (char)k; putchar_('*'); }
            }
        }
        if (login_(user, pass) == 0) {
            refresh_identity();
            vga_color(LIGHT_GREEN, BLACK);
            print("Welcome, "); print(g_user); print("!\n");
            vga_color(LIGHT_GREY, BLACK);
            return;
        }
        vga_color(LIGHT_RED, BLACK);
        print("Login incorrect\n\n");
        vga_color(LIGHT_GREY, BLACK);
    }
}

int main(int argc, char **argv){
    (void)argc; (void)argv;
    init_pipe_state();
    do_login();
    vga_color(LIGHT_CYAN, BLACK);
    print("\n==============================================\n");
    print(" Trinux ring-3 shell (PID=");
    print_num(getpid());
    print(") - v0.2.3 (su/cp fixed)\n");
    print("==============================================\n");
    vga_color(LIGHT_GREY, BLACK);
    print("Type 'help' for built-ins. Try 'top' or 'edit FILE'.\n\n");

    char line[LINE_MAX];
    char *av[ARGS_MAX];
    for (;;) {
        show_prompt();
        int n = read_line(line, sizeof(line));
        if (n <= 0) continue;
        add_history(line);
        int ac = tokenize(line, av);
        if (ac == 0) continue;
        dispatch(ac, av);
    }
}
SHELL_C

# ============================================================
# user/coreutils/*.c  -  los 35 archivos extra (fase 3-5)
# ============================================================
cd user/coreutils

# Helper
wp() { cat > "$1"; }

# ---- BUG #1 FIX: cp ahora abre el dst con O_TRUNC explícito y maneja
# correctamente el caso donde el archivo destino YA existía. ----
wp cp_u.c <<'EOF'
#include "../trinux.h"
static char buf[16384];
int main(int argc, char **argv){
    if(argc<3){print("usage: cp <src> <dst>\n");return 1;}
    int n = readfile(argv[1], buf, sizeof(buf));
    if(n<0){print("cp: "); print(argv[1]); print(": no such file\n");return 1;}
    /* Si el destino existe, lo borramos primero para forzar un node fresh.
     * Sin esto, vfs_create devuelve el nodo existente sin truncar y los
     * datos viejos pueden quedar al final del archivo nuevo. */
    unlink(argv[2]);
    int w = writefile(argv[2], buf, n);
    if(w<0){print("cp: cannot write "); print(argv[2]); print("\n");return 1;}
    if(w!=n){print("cp: short write\n");return 1;}
    return 0;
}
EOF

wp mv_u.c <<'EOF'
#include "../trinux.h"
static char buf[16384];
int main(int argc, char **argv){
    if(argc<3){print("usage: mv <src> <dst>\n");return 1;}
    int n = readfile(argv[1], buf, sizeof(buf));
    if(n<0){print("mv: "); print(argv[1]); print(": no such file\n");return 1;}
    /* Igual que cp: borrar destino si existe antes de escribir. */
    unlink(argv[2]);
    int w = writefile(argv[2], buf, n);
    if(w<0){print("mv: cannot write\n");return 1;}
    if(w!=n){print("mv: short write\n");return 1;}
    unlink(argv[1]);
    return 0;
}
EOF

wp stat_u.c <<'EOF'
#include "../trinux.h"
int main(int argc, char **argv){
    if(argc<2){print("usage: stat <file>\n");return 1;}
    trinux_stat_t st;
    if(stat(argv[1], &st)<0){print("stat: no such file\n");return 1;}
    print("  File: "); print(argv[1]); print("\n  Type: ");
    if(st.type==1) print("regular file"); else if(st.type==2) print("directory");
    else if(st.type==3) print("device"); else print("?");
    print("\n  Size: "); print_unum(st.size);
    print("\n  Perms: 0"); print_unum(st.perm);
    print("\n  Uid: "); print_unum(st.uid); print("   Gid: "); print_unum(st.gid); print("\n");
    return 0;
}
EOF

wp chmod_u.c <<'EOF'
#include "../trinux.h"
static int oct(const char *s){int n=0;while(*s>='0'&&*s<='7'){n=n*8+*s-'0';s++;}return n;}
int main(int argc, char **argv){
    if(argc<3){print("usage: chmod <octal> <file>\n");return 1;}
    if(chmod_(argv[2], oct(argv[1]))<0){print("chmod: failed\n");return 1;}
    return 0;
}
EOF

wp chown_u.c <<'EOF'
#include "../trinux.h"
static int ai(const char*s){int n=0;while(*s>='0'&&*s<='9'){n=n*10+*s-'0';s++;}return n;}
int main(int argc, char **argv){
    if(argc<3){print("usage: chown <uid[:gid]> <file>\n");return 1;}
    int uid=ai(argv[1]), gid=uid;
    for(int i=0;argv[1][i];i++) if(argv[1][i]==':'){gid=ai(argv[1]+i+1);break;}
    if(chown_(argv[2], uid, gid)<0){print("chown: failed\n");return 1;}
    return 0;
}
EOF

wp date_u.c <<'EOF'
#include "../trinux.h"
static void pp2(int n){if(n<10) putchar_('0'); print_num(n);}
int main(int argc, char **argv){(void)argc;(void)argv;
    datetime_u_t t; if(datetime_(&t)<0){print("date: error\n");return 1;}
    print_unum(t.year); print("-"); pp2(t.month); print("-"); pp2(t.day);
    print(" "); pp2(t.hour); print(":"); pp2(t.minute); print(":"); pp2(t.second); print("\n");
    return 0;
}
EOF

wp free_u.c <<'EOF'
#include "../trinux.h"
int main(int argc, char **argv){(void)argc;(void)argv;
    mem_info_t m; if(meminfo_(&m)<0){print("free: error\n");return 1;}
    print("               total       used       free\n");
    print("Mem (bytes):  "); print_unum(m.total_bytes); print("  ");
    print_unum(m.used_bytes); print("  "); print_unum(m.free_bytes); print("\n");
    print("Mem (KiB)  :  "); print_unum(m.total_bytes/1024); print("        ");
    print_unum(m.used_bytes/1024); print("        "); print_unum(m.free_bytes/1024); print("\n");
    return 0;
}
EOF

wp df_u.c <<'EOF'
#include "../trinux.h"
int main(int argc, char **argv){(void)argc;(void)argv;
    df_info_t d; if(dfinfo_(&d)<0){print("df: error\n");return 1;}
    if(!d.have_disk){print("No disk. RAM only.\n");return 0;}
    print("Filesystem  Blocks  Used    Free    BlockSize\n");
    print("/dev/sda    "); print_unum(d.total_blocks); print("  ");
    print_unum(d.used_blocks); print("  "); print_unum(d.total_blocks-d.used_blocks); print("  ");
    print_unum(d.block_size); print("\n");
    return 0;
}
EOF

wp users_u.c <<'EOF'
#include "../trinux.h"
static struct {char name[32]; uint32_t uid,gid; char home[64];} list[16];
int main(int argc, char **argv){(void)argc;(void)argv;
    user_list_req_t r; r.list=(void*)list; r.max=16;
    int n=userlist_(&r);
    print("USER         UID   GID   HOME\n");
    for(int i=0;i<n;i++){
        print(list[i].name);
        int pad=13-strlen_(list[i].name); if(pad<1)pad=1;
        for(int k=0;k<pad;k++) putchar_(' ');
        print_unum(list[i].uid); print("    "); print_unum(list[i].gid); print("    ");
        print(list[i].home); print("\n");
    }
    return 0;
}
EOF

wp groups_u.c <<'EOF'
#include "../trinux.h"
static struct {uint32_t uid,gid;} gl[8];
int main(int argc, char **argv){(void)argc;(void)argv;
    groups_req_t r; r.list=(void*)gl; r.max=8;
    int n=getgroups_(&r);
    for(int i=0;i<n;i++){print_unum(gl[i].gid); print(" ");} print("\n");
    return 0;
}
EOF

wp logout_u.c <<'EOF'
#include "../trinux.h"
int main(int argc, char **argv){(void)argc;(void)argv;
    logout_(); print("logout (back to login on shell restart)\n"); exit(0);
}
EOF

# ---- BUG #2 FIX: su ahora SIEMPRE pide password (incluso si eres root,
# para ser explícito). Antes el flujo dependía de is_root_user que se
# evaluaba DENTRO del syscall en momento del SU, lo cual generaba
# resultados confusos. ----
wp su_u.c <<'EOF'
#include "../trinux.h"
int main(int argc, char **argv){
    const char *u = argc>=2 ? argv[1] : "root";
    char pass[64]; int pl=0;
    /* Pedir password siempre, incluso si eres root.
     * Root puede usar password vacío (Enter) para entrar a cualquier user. */
    print("Password for ");
    print(u);
    print(" (Enter if root): ");
    for(;;){
        int k=key_raw();
        if(k=='\n'){putchar_('\n'); pass[pl]=0; break;}
        if(k=='\b'){if(pl>0){pl--; putchar_('\b'); putchar_(' '); putchar_('\b');}}
        else if(k>=32&&k<127){if(pl<63){pass[pl++]=(char)k; putchar_('*');}}
    }
    if(su_(u, pass)<0){
        print("su: authentication failure\n");
        return 1;
    }
    print("Switched to "); print(u);
    print(". (Logout to revert.)\n");
    return 0;
}
EOF

wp useradd_u.c <<'EOF'
#include "../trinux.h"
static int ai(const char*s){int n=0;while(*s>='0'&&*s<='9'){n=n*10+*s-'0';s++;}return n;}
int main(int argc, char **argv){
    if(argc<3){print("usage: useradd <name> <pass> [uid] [gid] [home]\n");return 1;}
    useradd_req_t r;
    r.name=argv[1]; r.pass=argv[2];
    r.uid = argc>=4 ? ai(argv[3]) : 1001;
    r.gid = argc>=5 ? ai(argv[4]) : 1001;
    r.home = argc>=6 ? argv[5] : "/home/user";
    if(useradd_(&r)<0){print("useradd: failed (need root)\n");return 1;}
    print("user added: "); print(argv[1]); print("\n");
    return 0;
}
EOF

wp passwd_u.c <<'EOF'
#include "../trinux.h"
int main(int argc, char **argv){
    char user[64];
    if(argc>=2){int i=0; while(argv[1][i]&&i<63){user[i]=argv[1][i];i++;} user[i]=0;}
    else getuser(user, sizeof(user));
    char p1[64],p2[64]; int pl;
    print("New password: "); pl=0;
    for(;;){int k=key_raw(); if(k=='\n'){putchar_('\n');p1[pl]=0;break;}
        if(k=='\b'){if(pl>0){pl--;putchar_('\b');putchar_(' ');putchar_('\b');}}
        else if(k>=32&&k<127){if(pl<63){p1[pl++]=(char)k;putchar_('*');}}}
    print("Repeat   : "); pl=0;
    for(;;){int k=key_raw(); if(k=='\n'){putchar_('\n');p2[pl]=0;break;}
        if(k=='\b'){if(pl>0){pl--;putchar_('\b');putchar_(' ');putchar_('\b');}}
        else if(k>=32&&k<127){if(pl<63){p2[pl++]=(char)k;putchar_('*');}}}
    if(!streq(p1,p2)){print("passwd: don't match\n");return 1;}
    passwd_req_t r; r.user=user; r.new_pass=p1;
    if(passwd_(&r)<0){print("passwd: failed\n");return 1;}
    print("password updated\n");
    return 0;
}
EOF

wp hostname_set_u.c <<'EOF'
#include "../trinux.h"
int main(int argc, char **argv){
    if(argc<2){char b[64]; hostname(b,sizeof(b)); println(b); return 0;}
    int n=strlen_(argv[1]);
    if(writefile("/etc/hostname", argv[1], n)<0){print("cannot write\n");return 1;}
    print("/etc/hostname updated. Reboot to apply.\n");
    return 0;
}
EOF

wp ps_u.c <<'EOF'
#include "../trinux.h"
static struct {uint32_t pid; char name[32]; uint32_t cpu_ticks; uint32_t state; int priority;} list[64];
static const char *sn(uint32_t s){switch(s){case 0:return"RUN";case 1:return"RDY";case 2:return"SLP";case 3:return"ZMB";} return "?";}
int main(int argc, char **argv){(void)argc;(void)argv;
    plist_req_t r; r.list=(void*)list; r.max=64;
    int n=listproc_(&r);
    print("  PID  ST   PRI  TICKS  NAME\n");
    for(int i=0;i<n;i++){
        if(list[i].pid<100) putchar_(' ');
        if(list[i].pid<10) putchar_(' ');
        print_unum(list[i].pid); print("   ");
        print(sn(list[i].state)); print("  ");
        print_num(list[i].priority); print("    ");
        print_unum(list[i].cpu_ticks); print("    ");
        print(list[i].name); print("\n");
    }
    return 0;
}
EOF

wp renice_u.c <<'EOF'
#include "../trinux.h"
static int ai(const char*s){int n=0,sg=1;if(*s=='-'){sg=-1;s++;} while(*s>='0'&&*s<='9'){n=n*10+*s-'0';s++;} return sg*n;}
int main(int argc, char **argv){
    if(argc<3){print("usage: renice <prio> <pid>\n");return 1;}
    if(renice_(ai(argv[2]), ai(argv[1]))<0){print("renice: failed\n");return 1;}
    return 0;
}
EOF

wp battery_u_cmd.c <<'EOF'
#include "../trinux.h"
int main(int argc, char **argv){(void)argc;(void)argv;
    battery_u_t b;
    if(battery_(&b)<0||!b.present){print("battery: no battery (VM/AC)\n");return 1;}
    print("battery: "); print_num(b.percent); print("%  (");
    print(b.discharging?"discharging":"charging"); print(")\n");
    return 0;
}
EOF

wp tree_u.c <<'EOF'
#include "../trinux.h"
int main(int argc, char **argv){
    tree_req_t r; r.root_path = argc>=2?argv[1]:"."; r.max_depth=8;
    if(tree_(&r)<0){print("tree: error\n");return 1;}
    return 0;
}
EOF

wp find_u.c <<'EOF'
#include "../trinux.h"
static char out[64][256];
int main(int argc, char **argv){
    find_req_t r;
    r.root_path = argc>=2?argv[1]:"/";
    r.name_substr = (argc>=4 && streq(argv[2],"-name")) ? argv[3] : 0;
    r.max = 64; r.out_paths = out;
    int n = find_(&r);
    for(int i=0;i<n;i++) println(out[i]);
    return 0;
}
EOF

wp sort_u.c <<'EOF'
#include "../trinux.h"
#define ML 256
static char buf[16384];
static char *lines[ML];
static int slt(const char*a,const char*b){while(*a&&*a==*b){a++;b++;} return (uint8_t)*a-(uint8_t)*b;}
int main(int argc, char **argv){
    int rev=0,num=0,uniq=0; const char *file=0;
    for(int i=1;i<argc;i++){
        if(streq(argv[i],"-r"))rev=1;
        else if(streq(argv[i],"-n"))num=1;
        else if(streq(argv[i],"-u"))uniq=1;
        else file=argv[i];
    }
    if(!file){print("usage: sort [-rnu] <file>\n");return 1;}
    int n=readfile(file,buf,sizeof(buf)-1);
    if(n<0){print("sort: no such file\n");return 1;}
    buf[n]=0;
    int nl=0; lines[nl++]=buf;
    for(int i=0;i<n && nl<ML;i++) if(buf[i]=='\n'){buf[i]=0; if(i+1<n) lines[nl++]=&buf[i+1];}
    for(int i=0;i<nl-1;i++) for(int j=0;j<nl-1-i;j++){
        int cmp;
        if(num){int a=0,b=0; const char *pa=lines[j],*pb=lines[j+1];
            while(*pa>='0'&&*pa<='9'){a=a*10+*pa-'0';pa++;}
            while(*pb>='0'&&*pb<='9'){b=b*10+*pb-'0';pb++;}
            cmp = a-b;
        } else cmp = slt(lines[j], lines[j+1]);
        if((!rev&&cmp>0)||(rev&&cmp<0)){char *t=lines[j]; lines[j]=lines[j+1]; lines[j+1]=t;}
    }
    const char *prev=0;
    for(int i=0;i<nl;i++){
        if(uniq&&prev&&slt(prev,lines[i])==0) continue;
        print(lines[i]); print("\n"); prev=lines[i];
    }
    return 0;
}
EOF

wp uniq_u.c <<'EOF'
#include "../trinux.h"
static char buf[16384];
static int slt(const char*a,const char*b){while(*a&&*a==*b){a++;b++;}return(uint8_t)*a-(uint8_t)*b;}
int main(int argc, char **argv){
    int cflag=0; const char *file=0;
    for(int i=1;i<argc;i++){if(streq(argv[i],"-c"))cflag=1; else file=argv[i];}
    if(!file){print("usage: uniq [-c] <file>\n");return 1;}
    int n=readfile(file,buf,sizeof(buf)-1);
    if(n<0){print("uniq: no such file\n");return 1;}
    buf[n]=0;
    char *p=buf,*prev=0; int cnt=1;
    while(p<buf+n){
        char *eol=p; while(*eol&&*eol!='\n') eol++;
        char saved=*eol; *eol=0;
        if(prev&&slt(prev,p)==0) cnt++;
        else {if(prev){if(cflag){print_num(cnt);print(" ");} print(prev); print("\n");} prev=p; cnt=1;}
        *eol=saved;
        p = (*eol) ? eol+1 : eol;
    }
    if(prev){if(cflag){print_num(cnt);print(" ");} print(prev); print("\n");}
    return 0;
}
EOF

wp cut_u.c <<'EOF'
#include "../trinux.h"
static char buf[8192];
static int ai(const char*s){int n=0;while(*s>='0'&&*s<='9'){n=n*10+*s-'0';s++;}return n;}
int main(int argc, char **argv){
    char delim=' '; int field=1; int char_mode=0; int first=1,last=0;
    const char *file=0;
    for(int i=1;i<argc;i++){
        if(argv[i][0]=='-'&&argv[i][1]=='d'&&i+1<argc){delim=argv[++i][0];}
        else if(argv[i][0]=='-'&&argv[i][1]=='f'&&i+1<argc){field=ai(argv[++i]);}
        else if(argv[i][0]=='-'&&argv[i][1]=='c'&&i+1<argc){
            const char *r=argv[++i]; char_mode=1; first=ai(r); last=first;
            for(int k=0;r[k];k++) if(r[k]=='-'){last=ai(r+k+1);break;}
        } else file=argv[i];
    }
    if(!file){print("usage: cut [-d X -f N | -c A-B] <file>\n");return 1;}
    int n=readfile(file,buf,sizeof(buf)-1);
    if(n<0){print("cut: no such file\n");return 1;}
    buf[n]=0;
    int ls=0;
    for(int i=0;i<=n;i++){
        if(buf[i]=='\n'||buf[i]==0){
            buf[i]=0;
            if(char_mode){
                int len=i-ls; int a=first-1,b=last;
                if(a<0)a=0; if(b>len)b=len;
                for(int k=a;k<b;k++) putchar_(buf[ls+k]);
            } else {
                int cf=1,fs=ls;
                for(int k=ls;k<=i;k++){
                    if(buf[k]==delim||buf[k]==0){
                        if(cf==field){for(int q=fs;q<k;q++) putchar_(buf[q]); break;}
                        cf++; fs=k+1;
                    }
                }
            }
            putchar_('\n');
            ls=i+1;
        }
    }
    return 0;
}
EOF

wp tee_u.c <<'EOF'
#include "../trinux.h"
static char buf[2048];
int main(int argc, char **argv){
    int append=0; const char *file=0;
    for(int i=1;i<argc;i++){if(streq(argv[i],"-a"))append=1; else file=argv[i];}
    if(!file){print("usage: tee [-a] <file>\n");return 1;}
    int n=getline_(buf,sizeof(buf));
    print(buf); print("\n");
    if(append){
        static char old[4096]; int o=readfile(file,old,sizeof(old)); if(o<0)o=0;
        for(int i=0;i<n&&o+i<(int)sizeof(old)-2;i++) old[o++]=buf[i];
        old[o++]='\n';
        unlink(file);
        writefile(file,old,o);
    } else {
        buf[n]='\n';
        unlink(file);
        writefile(file,buf,n+1);
    }
    return 0;
}
EOF

wp seq_u.c <<'EOF'
#include "../trinux.h"
static int ai(const char*s){int n=0,sg=1;if(*s=='-'){sg=-1;s++;} while(*s>='0'&&*s<='9'){n=n*10+*s-'0';s++;} return sg*n;}
int main(int argc, char **argv){
    int a=1,b=0;
    if(argc==2){a=1; b=ai(argv[1]);}
    else if(argc>=3){a=ai(argv[1]); b=ai(argv[2]);}
    else {print("usage: seq [start] end\n"); return 1;}
    for(int i=a;i<=b;i++){print_num(i); print("\n");}
    return 0;
}
EOF

wp basename_u.c <<'EOF'
#include "../trinux.h"
int main(int argc, char **argv){
    if(argc<2){print("usage: basename <path>\n");return 1;}
    const char *s=argv[1]; const char *last=s;
    for(const char *p=s;*p;p++) if(*p=='/') last=p+1;
    println(last);
    return 0;
}
EOF

wp dirname_u.c <<'EOF'
#include "../trinux.h"
int main(int argc, char **argv){
    if(argc<2){print("usage: dirname <path>\n");return 1;}
    char buf[256]; int n=0,ls=-1;
    for(int i=0;argv[1][i];i++){if(argv[1][i]=='/') ls=i; buf[n++]=argv[1][i];}
    buf[n]=0;
    if(ls<0){print(".\n");return 0;}
    if(ls==0){print("/\n");return 0;}
    buf[ls]=0; println(buf);
    return 0;
}
EOF

wp which_u.c <<'EOF'
#include "../trinux.h"
int main(int argc, char **argv){
    if(argc<2){print("usage: which <cmd>\n");return 1;}
    char path[128]="/bin/";
    int i=0; while(argv[1][i]&&i<120){path[5+i]=argv[1][i]; i++;}
    path[5+i]=0;
    trinux_stat_t st;
    if(stat(path,&st)<0){print(argv[1]); print(": not found\n"); return 1;}
    println(path);
    return 0;
}
EOF

wp env_u.c <<'EOF'
#include "../trinux.h"
int main(int argc, char **argv){(void)argc;(void)argv;
    char b[64];
    getuser(b,sizeof(b)); print("USER="); println(b);
    hostname(b,sizeof(b)); print("HOSTNAME="); println(b);
    char cwd[256]; getcwd(cwd,sizeof(cwd));
    print("PWD="); println(cwd);
    print("PATH=/bin\nSHELL=/bin/sh\nTERM=trinux-vga\n");
    return 0;
}
EOF

wp calc_u.c <<'EOF'
#include "../trinux.h"
static int ai(const char*s){int n=0;while(*s>='0'&&*s<='9'){n=n*10+*s-'0';s++;}return n;}
int main(int argc, char **argv){
    if(argc<4){print("usage: calc <a> <op> <b>\n");return 1;}
    int a=ai(argv[1]), b=ai(argv[3]);
    int r=0;
    switch(argv[2][0]){
        case '+': r=a+b; break;
        case '-': r=a-b; break;
        case '*': case 'x': case 'X': r=a*b; break;
        case '/': r = b ? a/b : 0; break;
        default: print("calc: unknown op\n"); return 1;
    }
    print_num(r); print("\n");
    return 0;
}
EOF

wp hexdump_u.c <<'EOF'
#include "../trinux.h"
static char buf[4096];
static void h2(int v){const char*h="0123456789abcdef"; putchar_(h[(v>>4)&15]); putchar_(h[v&15]);}
static void h8(uint32_t v){const char*h="0123456789abcdef"; for(int i=28;i>=0;i-=4) putchar_(h[(v>>i)&15]);}
int main(int argc, char **argv){
    if(argc<2){print("usage: hexdump <file>\n");return 1;}
    int n=readfile(argv[1],buf,sizeof(buf));
    if(n<0){print("hexdump: no such file\n");return 1;}
    for(int off=0;off<n;off+=16){
        h8(off); print("  ");
        for(int i=0;i<16;i++){
            if(off+i<n){h2((uint8_t)buf[off+i]); putchar_(' ');} else print("   ");
            if(i==7) putchar_(' ');
        }
        print(" |");
        for(int i=0;i<16&&off+i<n;i++){uint8_t c=(uint8_t)buf[off+i]; putchar_((c>=32&&c<127)?c:'.');}
        print("|\n");
    }
    return 0;
}
EOF

wp neofetch_u.c <<'EOF'
#include "../trinux.h"
int main(int argc, char **argv){(void)argc;(void)argv;
    char u[64],h[64]; getuser(u,sizeof(u)); hostname(h,sizeof(h));
    mem_info_t mi; meminfo_(&mi);
    df_info_t di; dfinfo_(&di);
    print("      .--.        ");
    vga_color(11,0); print(u); vga_color(7,0); print("@"); vga_color(11,0); print(h); vga_color(7,0); print("\n");
    print("     |o_o |       OS:     Trinux 0.2.3 (ring 3 shell)\n");
    print("     |:_/ |       Kernel: x86 32-bit protected mode\n");
    print("    //   \\ \\      Uptime: "); print_unum(uptime()); print(" s\n");
    print("   (|     | )     Shell:  /bin/sh (PID="); print_num(getpid()); print(")\n");
    print("  /'\\_   _/`\\     Memory: ");
    print_unum(mi.used_bytes/(1024*1024)); print("/"); print_unum(mi.total_bytes/(1024*1024)); print(" MiB\n");
    print("  \\___)=(___/     Disk:   ");
    if(di.have_disk){print_unum(di.used_blocks*(di.block_size/1024)); print("/");
        print_unum(di.total_blocks*(di.block_size/1024)); print(" KiB\n");}
    else print("RAM only\n");
    return 0;
}
EOF

wp write_u.c <<'EOF'
#include "../trinux.h"
static char buf[2048];
int main(int argc, char **argv){
    if(argc<2){print("usage: write <file>\n");return 1;}
    int total=0;
    for(;;){
        char line[256]; int n=getline_(line,sizeof(line));
        if(n<=0) break;
        for(int i=0;i<n&&total<(int)sizeof(buf)-2;i++) buf[total++]=line[i];
        buf[total++]='\n';
    }
    unlink(argv[1]);
    if(writefile(argv[1],buf,total)<0){print("write: failed\n");return 1;}
    return 0;
}
EOF

wp color_u.c <<'EOF'
#include "../trinux.h"
static int ai(const char*s){int n=0;while(*s>='0'&&*s<='9'){n=n*10+*s-'0';s++;}return n;}
int main(int argc, char **argv){
    if(argc<3){print("usage: color <fg> <bg> (0-15)\n");return 1;}
    vga_color(ai(argv[1]),ai(argv[2]));
    return 0;
}
EOF

wp halt_u.c <<'EOF'
#include "../trinux.h"
int main(int a,char**v){(void)a;(void)v; if(sys_shutdown()<0){print("halt: denied\n");return 1;} return 0;}
EOF

# ---- lscpu: info de cores via ACPI MADT ----
wp lscpu_u.c <<'EOF'
#include "../trinux.h"
int main(int argc, char **argv){(void)argc;(void)argv;
    smp_info_t info;
    if (smp_info(&info) < 0) {print("lscpu: SYS_SMP_INFO no soportado\n");return 1;}
    print("Architecture:        i386 (x86 32-bit protected mode)\n");
    print("CPU(s):              "); print_num(info.n_cpus); print("\n");
    print("On-line CPU(s):      ");
    if (info.online == 1) print("0 (BSP only — APs detected but not started)\n");
    else { print("0-"); print_num(info.online-1); print("\n"); }
    print("BSP APIC ID:         "); print_num(info.bsp_apic_id); print("\n");
    print("LAPIC base address:  0x");
    char hexbuf[9]; const char *hex = "0123456789abcdef";
    uint32_t v = info.lapic_base; int i = 7; hexbuf[8] = 0;
    while (i >= 0) { hexbuf[i--] = hex[v & 0xF]; v >>= 4; }
    print(hexbuf); print("\n");
    print("\nDetected cores:\n  #    APIC ID    Status\n");
    for (int k = 0; k < info.n_cpus; k++) {
        print("  "); print_num(k); print("    "); print_num(info.apic_ids[k]);
        if (info.apic_ids[k] == info.bsp_apic_id) print("          RUNNING (BSP)\n");
        else print("          halted (AP, awaiting wake-up)\n");
    }
    return 0;
}
EOF

# ---- screeninfo: info del framebuffer ----
wp screeninfo_u.c <<'EOF'
#include "../trinux.h"
static void print_hex32(uint32_t v) {
    const char *hex = "0123456789abcdef";
    char buf[9]; int i = 7; buf[8] = 0;
    while (i >= 0) { buf[i--] = hex[v & 0xF]; v >>= 4; }
    print("0x"); print(buf);
}
int main(int argc, char **argv){(void)argc;(void)argv;
    fb_info_t info;
    if (fb_info(&info) < 0) {print("screeninfo: SYS_FB_INFO no soportado\n");return 1;}
    print("Display mode:        ");
    print(info.active ? "GRAPHICS (VBE framebuffer)\n" : "TEXT (VGA legacy)\n");
    print("Text grid:           "); print_num(info.text_cols); print(" cols x ");
    print_num(info.text_rows); print(" rows\n");
    if (info.active) {
        print("Resolution:          "); print_num(info.width); print(" x "); print_num(info.height);
        print(" pixels, "); print_num(info.bpp); print(" bpp\n");
        print("Framebuffer addr:    "); print_hex32(info.fb_addr); print("\n");
        print("Pitch:               "); print_num((int)info.pitch); print(" bytes/scanline\n");
        uint32_t fb_size = info.pitch * info.height;
        print("Framebuffer size:    "); print_num((int)(fb_size / 1024)); print(" KiB\n");
        print("\nMatched profile:     ");
        if (info.width == 1366 && info.height == 768) print("HP Stream 14 (1366x768 nativo)\n");
        else if (info.width == 1280 && info.height == 800) print("QEMU default (1280x800)\n");
        else if (info.width == 1024 && info.height == 768) print("XGA fallback (1024x768)\n");
        else if (info.width == 1920 && info.height == 1080) print("Full HD (1920x1080)\n");
        else if (info.width == 800 && info.height == 600) print("SVGA fallback (800x600)\n");
        else print("custom\n");
    } else {
        print("\nNote: estas en modo texto VGA. Posibles causas:\n");
        print("  - GRUB no ofrecio framebuffer (BIOS muy vieja o UEFI puro)\n");
        print("  - El bpp pedido (32) no esta disponible en tu adaptador\n");
    }
    return 0;
}
EOF

# ---- sysinfo: resumen del estado del sistema ----
wp sysinfo_u.c <<'EOF'
#include "../trinux.h"
#define LIGHT_GREY 7
#define LIGHT_GREEN 10
#define LIGHT_CYAN 11
#define YELLOW 14
int main(int argc, char **argv){(void)argc;(void)argv;
    vga_color(LIGHT_CYAN, 0);
    print("================================================\n");
    print("  Trinux System Info\n");
    print("================================================\n");
    vga_color(LIGHT_GREY, 0);
    smp_info_t cpu;
    if (smp_info(&cpu) == 0) {
        vga_color(YELLOW, 0); print("\n[CPU]\n"); vga_color(LIGHT_GREY, 0);
        print("  Cores detected:  "); print_num(cpu.n_cpus); print("\n");
        print("  Cores online:    "); print_num(cpu.online);
        if (cpu.n_cpus > 1 && cpu.online == 1) print(" (BSP only)");
        print("\n");
        print("  BSP APIC ID:     "); print_num(cpu.bsp_apic_id); print("\n");
    }
    fb_info_t fb;
    if (fb_info(&fb) == 0) {
        vga_color(YELLOW, 0); print("\n[Display]\n"); vga_color(LIGHT_GREY, 0);
        if (fb.active) {
            print("  Mode:            graphics (VBE)\n");
            print("  Resolution:      "); print_num(fb.width); print(" x "); print_num(fb.height);
            print(" @ "); print_num(fb.bpp); print(" bpp\n");
        } else {
            print("  Mode:            text VGA\n");
        }
        print("  Text grid:       "); print_num(fb.text_cols); print(" x "); print_num(fb.text_rows); print("\n");
    }
    mem_info_t mem;
    if (meminfo_(&mem) == 0) {
        vga_color(YELLOW, 0); print("\n[Memory]\n"); vga_color(LIGHT_GREY, 0);
        print("  Total:           "); print_unum(mem.total_bytes / (1024*1024)); print(" MiB\n");
        print("  Used:            "); print_unum(mem.used_bytes / (1024*1024)); print(" MiB\n");
        print("  Free:            "); print_unum(mem.free_bytes / (1024*1024)); print(" MiB\n");
    }
    df_info_t df;
    if (dfinfo_(&df) == 0) {
        vga_color(YELLOW, 0); print("\n[Storage]\n"); vga_color(LIGHT_GREY, 0);
        if (df.have_disk) {
            print("  Disk:            present\n");
            print("  Total:           "); print_unum((df.total_blocks * df.block_size) / (1024*1024)); print(" MiB\n");
            print("  Used:            "); print_unum((df.used_blocks * df.block_size) / (1024*1024)); print(" MiB\n");
        } else print("  Disk:            none (RAM-only)\n");
    }
    battery_u_t bat;
    if (battery_(&bat) == 0 && bat.present) {
        vga_color(YELLOW, 0); print("\n[Battery]\n"); vga_color(LIGHT_GREY, 0);
        print("  Status:          ");
        if (bat.discharging) {vga_color(YELLOW, 0); print("discharging");}
        else {vga_color(LIGHT_GREEN, 0); print("charging or AC");}
        vga_color(LIGHT_GREY, 0); print("\n");
        print("  Charge:          "); print_num(bat.percent); print(" %\n");
    } else {
        vga_color(YELLOW, 0); print("\n[Battery]\n"); vga_color(LIGHT_GREY, 0);
        print("  No battery detected (AC-only / VM)\n");
    }
    vga_color(YELLOW, 0); print("\n[Time]\n"); vga_color(LIGHT_GREY, 0);
    print("  Uptime:          "); print_unum(uptime()); print(" seconds\n");
    char h[64]; hostname(h, sizeof(h));
    char u[64]; getuser(u, sizeof(u));
    vga_color(YELLOW, 0); print("\n[System]\n"); vga_color(LIGHT_GREY, 0);
    print("  Hostname:        "); print(h); print("\n");
    print("  Current user:    "); print(u); print(" (uid="); print_num(getuid()); print(")\n");
    print("  Shell PID:       "); print_num(getpid()); print("\n");
    vga_color(LIGHT_CYAN, 0);
    print("\n================================================\n");
    vga_color(LIGHT_GREY, 0);
    return 0;
}
EOF

# ---- top: monitor live ----
wp top_u.c <<'EOF'
#include "../trinux.h"
static struct {uint32_t pid; char name[32]; uint32_t cpu_ticks; uint32_t state; int priority;} proc_list[64];
static const char *st_name(uint32_t s){switch(s){case 0:return"RUN";case 1:return"RDY";case 2:return"SLP";case 3:return"ZMB";} return "?";}
static void pad_to(int n, int width){
    int seen=0; uint32_t v=(uint32_t)n; if(n==0) seen=1;
    while(v){seen++; v/=10;}
    for(int i=seen;i<width;i++) putchar_(' ');
}
static void draw(void){
    vga_goto(0,0);
    vga_color(11, 4);
    print("Trinux top    ");
    char host[32]; hostname(host,sizeof(host));
    print(host); print("    uptime: "); print_unum(uptime()); print(" s        (press q to quit)");
    for(int i=0;i<14;i++) putchar_(' ');
    vga_color(7, 0); print("\n");
    mem_info_t mi; meminfo_(&mi);
    print("Mem: "); print_unum(mi.used_bytes/(1024*1024)); print("/");
    print_unum(mi.total_bytes/(1024*1024)); print(" MiB used  (");
    print_unum(mi.free_bytes/(1024*1024)); print(" MiB free)");
    for(int i=0;i<30;i++) putchar_(' '); print("\n");
    df_info_t di; dfinfo_(&di);
    print("Disk: ");
    if(di.have_disk){print_unum(di.used_blocks*(di.block_size/1024)); print("/");
        print_unum(di.total_blocks*(di.block_size/1024)); print(" KiB");}
    else print("RAM only");
    for(int i=0;i<40;i++) putchar_(' '); print("\n\n");
    vga_color(0, 7);
    print("  PID  ST   PRI    TICKS   NAME                              ");
    vga_color(7, 0); print("\n");
    plist_req_t r; r.list = (void*)proc_list; r.max = 64;
    int n = listproc_(&r);
    for(int i=0;i<n && i<42;i++){   /* 50 filas - 8 de header/etc */
        pad_to((int)proc_list[i].pid, 5);
        print_unum(proc_list[i].pid); print("  ");
        print(st_name(proc_list[i].state)); print("  ");
        if(proc_list[i].priority>=0) putchar_(' ');
        print_num(proc_list[i].priority); print("   ");
        pad_to((int)proc_list[i].cpu_ticks, 8);
        print_unum(proc_list[i].cpu_ticks); print("    ");
        int j=0; while(proc_list[i].name[j] && j<31){ putchar_(proc_list[i].name[j]); j++; }
        while(j<32){ putchar_(' '); j++; }
        print(" \n");
    }
    for(int row=n+5; row<49; row++){
        for(int c=0;c<78;c++) putchar_(' '); print("\n");
    }
}
int main(int argc, char **argv){(void)argc;(void)argv;
    vga_clear_();
    for(;;){
        draw();
        for(int i=0;i<20;i++){
            int k = key_poll();
            if(k=='q' || k=='Q'){ vga_clear_(); vga_goto(0,0); return 0; }
            msleep(50);
        }
    }
}
EOF

# ---- edit: editor full-screen ----
wp edit_u.c <<'EOF'
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
EOF

# ---- tcc: wrapper userland ----
wp tcc_u.c <<'EOF'
#include "../trinux.h"
int main(int argc, char **argv){
    if(argc < 2){
        print("usage: tcc <source.c>\n");
        print("  Compila .c -> ELF ring 3 (mismo nombre sin .c)\n");
        print("  Helpers: print, print_num, getchar, sleep, uptime, getpid,\n");
        print("           exit, vga_clear, vga_putchar, vga_print\n");
        return 1;
    }
    int rc = tcc_compile(argv[1]);
    if(rc < 0){print("tcc: compilation failed\n"); return 1;}
    char out[128]; int i=0;
    while(argv[1][i] && i<127){out[i]=argv[1][i]; i++;} out[i] = 0;
    if(i > 2 && out[i-2]=='.' && out[i-1]=='c') out[i-2] = 0;
    print("tcc: compiled -> "); println(out);
    return 0;
}
EOF

cd ../..

# ============================================================
# user/coreutils/build.sh — tabla completa
# ============================================================
cat > user/coreutils/build.sh <<'BUILD_SH'
#!/usr/bin/env bash
set -e
cd "$(dirname "$0")"
CC="${CC:-gcc}"
CFLAGS="-m32 -ffreestanding -nostdlib -static -fno-pic -fno-pie -fno-stack-protector \
        -fno-asynchronous-unwind-tables -Os -Wall -Wno-unused-parameter -I.."
LD="ld -m elf_i386 -T user.ld -nostdlib --build-id=none"
OUT="bin"; HDRS="hdrs"
mkdir -p "$OUT" "$HDRS"
$CC $CFLAGS -c crt0.S -o "$OUT/crt0.o"

declare -A PROGS=(
  [echo]=echo.c [cat]=cat.c [ls]=ls.c [pwd]=pwd.c
  [whoami]=whoami.c [hostname]=hostname_u.c [uname]=uname.c
  [uptime]=uptime_u.c [clear]=clear.c
  [mkdir]=mkdir_u.c [rmdir]=rmdir_u.c [rm]=rm_u.c [touch]=touch_u.c
  [wc]=wc_u.c [head]=head_u.c [tail]=tail_u.c [grep]=grep_u.c
  [sleep]=sleep_u.c [yes]=yes_u.c [true]=true_u.c [false]=false_u.c
  [id]=id_u.c [reboot]=reboot_u.c [shutdown]=shutdown_u.c
  [sync]=sync_u.c [kill]=kill_u.c [ringtest]=ringtest.c
  [cp]=cp_u.c [mv]=mv_u.c [stat]=stat_u.c
  [chmod]=chmod_u.c [chown]=chown_u.c
  [date]=date_u.c [free]=free_u.c [df]=df_u.c
  [users]=users_u.c [groups]=groups_u.c [logout]=logout_u.c
  [su]=su_u.c [useradd]=useradd_u.c [passwd]=passwd_u.c
  [ps]=ps_u.c [renice]=renice_u.c [battery]=battery_u_cmd.c
  [tree]=tree_u.c [find]=find_u.c
  [sort]=sort_u.c [uniq]=uniq_u.c [cut]=cut_u.c [tee]=tee_u.c
  [seq]=seq_u.c [basename]=basename_u.c [dirname]=dirname_u.c
  [which]=which_u.c [env]=env_u.c
  [calc]=calc_u.c [hexdump]=hexdump_u.c [neofetch]=neofetch_u.c
  [write]=write_u.c [color]=color_u.c [halt]=halt_u.c
  [lscpu]=lscpu_u.c [screeninfo]=screeninfo_u.c [sysinfo]=sysinfo_u.c
  [top]=top_u.c [edit]=edit_u.c [tcc]=tcc_u.c
)
declare -A ALIASES=([cls]=clear [nano]=edit)

for name in "${!PROGS[@]}"; do
    src="${PROGS[$name]}"
    $CC $CFLAGS -c "$src" -o "$OUT/${name}.o" 2>/dev/null
    $LD "$OUT/crt0.o" "$OUT/${name}.o" -o "$OUT/${name}" 2>/dev/null
    (cd "$OUT" && xxd -i -n "u_${name}" "${name}") > "$HDRS/${name}.h"
done
echo "Compilados ${#PROGS[@]} programas"

{
  echo "/* AUTO-GENERATED. */"
  echo "#ifndef USER_BIN_TABLE_H"
  echo "#define USER_BIN_TABLE_H"
  for name in "${!PROGS[@]}"; do echo "#include \"hdrs/${name}.h\""; done
  echo "typedef struct { const char *name; unsigned char *data; unsigned int *size_p; } user_bin_t;"
  echo "static user_bin_t user_bins[] = {"
  for name in "${!PROGS[@]}"; do echo "    { \"$name\", u_${name}, &u_${name}_len },"; done
  for a in "${!ALIASES[@]}"; do tgt="${ALIASES[$a]}"; echo "    { \"$a\", u_${tgt}, &u_${tgt}_len },"; done
  echo "};"
  echo "#define user_bins_count (sizeof(user_bins)/sizeof(user_bins[0]))"
  echo "#endif"
} > user_bins.h

SH_SRC="../usersh/sh.c"
if [ -f "$SH_SRC" ]; then
    $CC $CFLAGS -c "$SH_SRC" -o "$OUT/sh.o" 2>/dev/null
    $LD "$OUT/crt0.o" "$OUT/sh.o" -o "$OUT/sh" 2>/dev/null
    (cd "$OUT" && xxd -i -n "u_sh" sh) > "../usersh/sh_elf.h"
    echo "Shell ring-3: $(stat -c%s "$OUT/sh") bytes"
fi
echo "Total: ${#PROGS[@]} binarios + ${#ALIASES[@]} alias"
BUILD_SH
chmod +x user/coreutils/build.sh

echo ""
echo "============================================================"
echo "FASE5_REGEN.sh OK. Archivos regenerados:"
echo "  - user/usersh/sh.c (con fix BUG #2 su)"
echo "  - user/coreutils/cp_u.c, mv_u.c (con fix BUG #1)"
echo "  - user/coreutils/su_u.c (con fix BUG #2)"
echo "  - user/coreutils/{top,edit,tcc}_u.c (de Fase 5)"
echo "  - 32 más coreutils (Fase 3-4)"
echo "  - user/coreutils/build.sh (tabla completa)"
echo ""
echo "Ahora corre:"
echo "  bash user/coreutils/build.sh"
echo "  make"
echo "  bash make-usb-image.sh 512"
echo "============================================================"

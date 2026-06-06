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

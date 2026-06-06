/* user/trinux.h  -  Trinux userspace library (mini libc for ring 3 programs).
 *
 * Esta cabecera es la "fuente única de verdad" de la ABI de syscalls.
 * El kernel (cpu/syscall.h) la incluye también, para evitar que los
 * números entre user y kernel se desincronicen.
 *
 * Programas en ring 3 pueden:
 *   - Usar int 0x80 con los SYS_* de abajo.
 *   - Escribir directo al framebuffer VGA en 0xB8000 (esa página está
 *     mapeada con PAGE_USER en el identity-map).
 *
 * Lo que NO pueden:
 *   - cli/sti, inb/outb, mov a CR0/CR3, ni ningún ring-0 instruction.
 *   - Llamar funciones del kernel directamente.
 *
 * Compilar un programa para Trinux:
 *   gcc -m32 -ffreestanding -nostdlib -static -fno-pic -fno-pie \
 *       -Wl,-Ttext=0x08048000 -Wl,--entry=_start \
 *       -o myapp myapp.c crt0.o
 */
#ifndef USER_TRINUX_H
#define USER_TRINUX_H

#ifndef TRINUX_KERNEL_INCLUDE
typedef unsigned int   uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char  uint8_t;
typedef int            int32_t;
#endif

/* ============================================================================
 * SYSCALLS – ABI ÚNICA, compartida entre kernel y userspace.
 * NO REORDENAR sin recompilar TODOS los ELFs de /bin.
 * ============================================================================ */

#define SYS_EXIT       1   /* ebx = exit code                                  */
#define SYS_WRITE      2   /* ebx = fd, ecx = buf, edx = len -> bytes          */
#define SYS_GETPID     3   /* -> pid                                           */
#define SYS_YIELD      4   /* -                                                */
#define SYS_SLEEP      5   /* ebx = ms                                         */
#define SYS_GETC       6   /* -> char (bloqueante)                             */
#define SYS_UPTIME     7   /* -> segundos desde boot                           */
#define SYS_READFILE   8   /* ebx=path, ecx=buf, edx=max -> bytes leídos (-1)  */
#define SYS_WRITEFILE  9   /* ebx=path, ecx=buf, edx=len -> bytes (-1)         */
#define SYS_GETLINE   10   /* ebx=buf, ecx=max -> length                       */

/* --- ABI 2.0: nueva en V2.1 (todos por encima de 10 son seguros de añadir) - */
#define SYS_GETCWD    11   /* ebx=buf, ecx=max  -> length                      */
#define SYS_CHDIR     12   /* ebx=path          -> 0/-1                        */
#define SYS_OPENDIR   13   /* ebx=path          -> dir handle (>=1) o -1       */
#define SYS_READDIR   14   /* ebx=dh, ecx=entry_t*  -> 0=ok, -1=fin            */
#define SYS_CLOSEDIR  15   /* ebx=dh            -> 0                           */
#define SYS_STAT      16   /* ebx=path, ecx=stat_t*  -> 0/-1                   */
#define SYS_UNLINK    17   /* ebx=path          -> 0/-1                        */
#define SYS_MKDIR     18   /* ebx=path          -> 0/-1                        */
#define SYS_RMDIR     19   /* ebx=path          -> 0/-1                        */
#define SYS_HOSTNAME  20   /* ebx=buf, ecx=max  -> length                      */
#define SYS_REBOOT    21   /* (privilegio root) -> no regresa                  */
#define SYS_SHUTDOWN  22   /* (privilegio root) -> no regresa si éxito         */
#define SYS_KILL      23   /* ebx=pid, ecx=sig  -> 0/-1   (ROOT o dueño)       */
#define SYS_SYNC      24   /* -                                                */
#define SYS_GETUID    25   /* -> uid del proceso actual                        */
#define SYS_GETUSER   26   /* ebx=buf, ecx=max  -> length (login name)         */
#define SYS_RENAME    27   /* ebx=src, ecx=dst  -> 0/-1                        */
#define SYS_TIME      28   /* -> uptime en ticks (deprec, usar uptime)         */
#define SYS_PUTPIXEL  29   /* reservado para futuro vbe                        */
#define SYS_CLEAR     30   /* limpia pantalla VGA                              */

/* --- ABI 3.0: shell en ring-3 (fase 2) --- */
#define SYS_SPAWN     31   /* ebx=path, ecx=argv, edx=flags -> pid o -1        */
#define SYS_WAITPID   32   /* ebx=pid, ecx=&exit_code -> 0/-1                  */
#define SYS_SPAWN_R   33   /* ebx=&spawn_req_t -> pid o -1   (con I/O redir)   */
#define SYS_DUP2_FD   34   /* (placeholder, no usado aún)                      */
#define SYS_FILE_OPEN  35  /* ebx=path, ecx=mode -> fd o -1                    */
#define SYS_FILE_READ  36  /* ebx=fd, ecx=buf, edx=len -> bytes o -1           */
#define SYS_FILE_WRITE 37  /* ebx=fd, ecx=buf, edx=len -> bytes o -1           */
#define SYS_FILE_CLOSE 38  /* ebx=fd -> 0/-1                                   */
#define SYS_FILE_SEEK  39  /* ebx=fd, ecx=off, edx=whence -> new pos           */
#define SYS_KEY_RAW    40  /* lee tecla cruda (incluye flechas/Tab) -> int     */
#define SYS_VGA_COLOR  41  /* ebx=fg, ecx=bg                                   */
#define SYS_USERADD    42  /* ROOT: ebx=&useradd_req_t -> 0/-1                 */
#define SYS_PASSWD     43  /* ebx=&passwd_req_t -> 0/-1                        */
#define SYS_LISTPROC  44  /* ebx=&plist_req_t -> n procesos                   */
#define SYS_LOGIN     45  /* ebx=user, ecx=pass -> 0 / -1                     */
#define SYS_VFS_CAP   46  /* ebx=&out (char**, &len) - dump vga capture       */
#define SYS_CHMOD     47  /* ebx=path, ecx=perms -> 0/-1                      */
#define SYS_CHOWN     48  /* ebx=path, ecx=uid, edx=gid -> 0/-1               */
#define SYS_MEMINFO   49  /* ebx=&mem_info_t -> 0                              */
#define SYS_DFINFO    50  /* ebx=&df_info_t -> 0                               */
#define SYS_DATETIME  51  /* ebx=&datetime_u_t -> 0                            */
#define SYS_USERLIST  52  /* ebx=&user_list_req_t -> n                         */
#define SYS_RENICE    53  /* ebx=pid, ecx=prio -> 0/-1                         */
#define SYS_BATTERY   54  /* ebx=&battery_u_t -> 0/-1                          */
#define SYS_LOGOUT    55  /* -> 0 (vuelve a login)                             */
#define SYS_SU        56  /* ebx=user, ecx=pass -> 0/-1                        */
#define SYS_GETGROUPS 57  /* ebx=&groups_req_t -> n                            */
#define SYS_VFS_FIND  58  /* ebx=&find_req_t -> n (recursive name search)      */
#define SYS_VFS_TREE  59  /* ebx=&tree_req_t -> 0 (imprime árbol via SYS_WRITE)*/
#define SYS_RESOLVE   60  /* ebx=path, ecx=out_buf, edx=max -> length          */

#define SPAWN_F_WAIT      0x01    /* esperar a que el hijo termine             */
#define SPAWN_F_BACKGROUND 0x02   /* lanzar en background                      */

#define O_RDONLY  0x0
#define O_WRONLY  0x1
#define O_RDWR    0x2
#define O_CREAT   0x40
#define O_TRUNC   0x200
#define O_APPEND  0x400
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2

#define KEY_F_LEFT   1000
#define KEY_F_RIGHT  1001
#define KEY_F_UP     1002
#define KEY_F_DOWN   1003
#define KEY_F_HOME   1004
#define KEY_F_END    1005
#define KEY_F_DEL    1006

/* Request structs (planos, ABI estable) */
typedef struct {
    const char *path;       /* programa a ejecutar */
    char      **argv;       /* argv[0..n], terminado en NULL */
    const char *stdin_path; /* si != NULL, redirige stdin desde este archivo */
    const char *stdout_path;/* si != NULL, redirige stdout a este archivo */
    int         append;     /* 1 = '>>' (append), 0 = '>' (truncar) */
} spawn_req_t;

typedef struct {
    const char *name;
    const char *pass;
    int         uid;
    int         gid;
    const char *home;
} useradd_req_t;

typedef struct {
    const char *user;
    const char *new_pass;
} passwd_req_t;

typedef struct {
    int          max;       /* tamaño del array */
    int          got;       /* lo llena el kernel */
    struct {
        uint32_t pid;
        char     name[32];
        uint32_t cpu_ticks;
        uint32_t state;
        int      priority;
    } *list;
} plist_req_t;

typedef struct {
    uint32_t total_bytes;
    uint32_t used_bytes;
    uint32_t free_bytes;
} mem_info_t;

typedef struct {
    int      have_disk;
    uint32_t total_blocks;
    uint32_t used_blocks;
    uint32_t block_size;
} df_info_t;

typedef struct {
    uint16_t year;
    uint8_t  month, day, hour, minute, second;
    uint8_t  _pad;
} datetime_u_t;

typedef struct {
    int     max, got;
    struct {
        char     name[32];
        uint32_t uid, gid;
        char     home[64];
    } *list;
} user_list_req_t;

typedef struct {
    int      present;        /* 0 si no hay batería */
    int      percent;
    int      discharging;
    int      time_minutes;   /* -1 si desconocido */
} battery_u_t;

typedef struct {
    int   max, got;
    struct { uint32_t uid, gid; } *list;
} groups_req_t;

typedef struct {
    const char *root_path;
    const char *name_substr; /* coincide si el nombre lo contiene */
    int         max;
    int         got;
    char      (*out_paths)[256];
} find_req_t;

typedef struct {
    const char *root_path;
    int         max_depth;
} tree_req_t;

/* Tipos compartidos */
typedef struct {
    char     name[64];
    uint32_t size;
    uint32_t type;     /* 1=file 2=dir 3=device 4=symlink                      */
    uint32_t perm;     /* permisos unix                                         */
    uint32_t uid;
    uint32_t gid;
} trinux_stat_t;

typedef struct {
    char     name[64];
    uint32_t type;
} trinux_dirent_t;

#ifndef TRINUX_KERNEL_INCLUDE
/* ============================================================================
 *  Wrappers de syscall
 * ============================================================================ */
static inline int _syscall0(int num) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num) : "memory");
    return ret;
}
static inline int _syscall1(int num, int a) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num), "b"(a) : "memory");
    return ret;
}
static inline int _syscall2(int num, int a, int b) {
    int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret) : "a"(num), "b"(a), "c"(b) : "memory");
    return ret;
}
static inline int _syscall3(int num, int a, int b, int c) {
    int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret) : "a"(num), "b"(a), "c"(b), "d"(c) : "memory");
    return ret;
}

/* ============================================================================
 *  API básica
 * ============================================================================ */
static inline void exit(int status) {
    _syscall1(SYS_EXIT, status);
    for(;;);
}
static inline int  write(int fd, const void *buf, int len) {
    return _syscall3(SYS_WRITE, fd, (int)buf, len);
}
static inline int  getpid(void)         { return _syscall0(SYS_GETPID); }
static inline void msleep(int ms)       { _syscall1(SYS_SLEEP, ms); }
static inline int  getchar(void)        { return _syscall0(SYS_GETC); }
static inline uint32_t uptime(void)     { return (uint32_t)_syscall0(SYS_UPTIME); }
static inline int  getuid(void)         { return _syscall0(SYS_GETUID); }
static inline int  getuser(char *b, int m){ return _syscall2(SYS_GETUSER,(int)b,m); }
static inline int  hostname(char *b, int m){return _syscall2(SYS_HOSTNAME,(int)b,m);}
static inline int  getcwd(char *b, int m){ return _syscall2(SYS_GETCWD,(int)b,m); }
static inline int  chdir(const char *p) { return _syscall1(SYS_CHDIR,(int)p); }
static inline int  unlink(const char *p){ return _syscall1(SYS_UNLINK,(int)p); }
static inline int  mkdir(const char *p) { return _syscall1(SYS_MKDIR,(int)p); }
static inline int  rmdir(const char *p) { return _syscall1(SYS_RMDIR,(int)p); }
static inline int  stat(const char *p, trinux_stat_t *st){
    return _syscall2(SYS_STAT,(int)p,(int)st);
}
static inline int  opendir(const char *p){return _syscall1(SYS_OPENDIR,(int)p);}
static inline int  readdir(int dh, trinux_dirent_t *d){
    return _syscall2(SYS_READDIR,dh,(int)d);
}
static inline int  closedir(int dh)     { return _syscall1(SYS_CLOSEDIR,dh); }
static inline int  rename(const char *a, const char *b){
    return _syscall2(SYS_RENAME,(int)a,(int)b);
}
static inline int  readfile(const char *p, void *buf, int max){
    return _syscall3(SYS_READFILE,(int)p,(int)buf,max);
}
static inline int  writefile(const char *p, const void *buf, int len){
    return _syscall3(SYS_WRITEFILE,(int)p,(int)buf,len);
}
static inline int  getline_(char *b, int m){return _syscall2(SYS_GETLINE,(int)b,m);}
static inline int  sys_reboot(void)     { return _syscall0(SYS_REBOOT); }
static inline int  sys_shutdown(void)   { return _syscall0(SYS_SHUTDOWN); }
static inline int  sys_kill(int pid, int sig){ return _syscall2(SYS_KILL,pid,sig);}
static inline int  sys_sync(void)       { return _syscall0(SYS_SYNC); }
static inline void clrscr(void)         { (void)_syscall0(SYS_CLEAR); }

/* --- spawn / waitpid --- */
static inline int spawn(const char *path, char **argv, int flags){
    return _syscall3(SYS_SPAWN, (int)path, (int)argv, flags);
}
static inline int waitpid(int pid, int *exit_code){
    return _syscall2(SYS_WAITPID, pid, (int)exit_code);
}
static inline int spawn_r(spawn_req_t *req){
    return _syscall1(SYS_SPAWN_R, (int)req);
}

/* --- IO con fd --- */
static inline int open_(const char *path, int flags){
    return _syscall2(SYS_FILE_OPEN, (int)path, flags);
}
static inline int read_(int fd, void *buf, int len){
    return _syscall3(SYS_FILE_READ, fd, (int)buf, len);
}
static inline int write_(int fd, const void *buf, int len){
    return _syscall3(SYS_FILE_WRITE, fd, (int)buf, len);
}
static inline int close_(int fd){ return _syscall1(SYS_FILE_CLOSE, fd); }
static inline int lseek_(int fd, int off, int whence){
    return _syscall3(SYS_FILE_SEEK, fd, off, whence);
}
static inline int key_raw(void){ return _syscall0(SYS_KEY_RAW); }
static inline void vga_color(int fg, int bg){ _syscall2(SYS_VGA_COLOR, fg, bg); }
static inline int useradd_(useradd_req_t *r){ return _syscall1(SYS_USERADD, (int)r); }
static inline int passwd_(passwd_req_t *r){ return _syscall1(SYS_PASSWD, (int)r); }
static inline int listproc_(plist_req_t *r){ return _syscall1(SYS_LISTPROC, (int)r); }
static inline int login_(const char *u, const char *p){
    return _syscall2(SYS_LOGIN, (int)u, (int)p);
}
static inline int chmod_(const char *p, int perms){
    return _syscall2(SYS_CHMOD, (int)p, perms);
}
static inline int chown_(const char *p, int uid, int gid){
    return _syscall3(SYS_CHOWN, (int)p, uid, gid);
}
static inline int meminfo_(mem_info_t *m){ return _syscall1(SYS_MEMINFO, (int)m); }
static inline int dfinfo_(df_info_t *d){ return _syscall1(SYS_DFINFO, (int)d); }
static inline int datetime_(datetime_u_t *t){ return _syscall1(SYS_DATETIME, (int)t); }
static inline int userlist_(user_list_req_t *r){ return _syscall1(SYS_USERLIST, (int)r); }
static inline int renice_(int pid, int prio){ return _syscall2(SYS_RENICE, pid, prio); }
static inline int battery_(battery_u_t *b){ return _syscall1(SYS_BATTERY, (int)b); }
static inline int logout_(void){ return _syscall0(SYS_LOGOUT); }
static inline int su_(const char *u, const char *p){ return _syscall2(SYS_SU, (int)u, (int)p); }
static inline int getgroups_(groups_req_t *r){ return _syscall1(SYS_GETGROUPS, (int)r); }
static inline int find_(find_req_t *r){ return _syscall1(SYS_VFS_FIND, (int)r); }
static inline int tree_(tree_req_t *r){ return _syscall1(SYS_VFS_TREE, (int)r); }
static inline int resolve_(const char *path, char *out, int max){
    return _syscall3(SYS_RESOLVE, (int)path, (int)out, max);
}

/* ============================================================================
 *  Helpers de string puramente ring-3 (no son syscalls)
 * ============================================================================ */
static inline int strlen_(const char *s) {
    int n = 0; while (s[n]) n++; return n;
}
static inline int streq(const char *a, const char *b){
    while (*a && *b && *a == *b){a++;b++;} return *a==*b;
}
static inline int strncmp_(const char *a, const char *b, int n){
    int i;for(i=0;i<n;i++){if(a[i]!=b[i])return (uint8_t)a[i]-(uint8_t)b[i];
        if(!a[i])return 0;} return 0;
}
static inline void *memset_(void *dst, int c, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (uint32_t i = 0; i < n; i++) d[i] = (uint8_t)c;
    return dst;
}
static inline void *memcpy_(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

/* ============================================================================
 *  IO: print y print_num (envuelven SYS_WRITE a fd 1)
 * ============================================================================ */
static inline void print(const char *s) {
    int len = 0; while (s[len]) len++;
    _syscall3(SYS_WRITE, 1, (int)s, len);
}
static inline void println(const char *s) { print(s); print("\n"); }
static inline void putchar_(int c) {
    char b = (char)c;
    _syscall3(SYS_WRITE, 1, (int)&b, 1);
}
static inline void print_num(int32_t n) {
    char buf[12]; int i = 10; buf[11] = '\0';
    uint32_t v;
    if (n < 0) { print("-"); v = (uint32_t)(-n); } else v = (uint32_t)n;
    if (v == 0) { buf[i--] = '0'; }
    while (v && i >= 0) { buf[i--] = '0' + (v % 10); v /= 10; }
    print(&buf[i + 1]);
}
static inline void print_unum(uint32_t v) {
    char buf[12]; int i = 10; buf[11] = '\0';
    if (v == 0) { buf[i--] = '0'; }
    while (v && i >= 0) { buf[i--] = '0' + (v % 10); v /= 10; }
    print(&buf[i + 1]);
}

#endif /* !TRINUX_KERNEL_INCLUDE */
#endif /* USER_TRINUX_H */

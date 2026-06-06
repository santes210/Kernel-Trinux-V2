#include "syscall.h"
#include "idt.h"
#include "../drivers/vga.h"
#include "../drivers/serial.h"
#include "../drivers/keyboard.h"
#include "../drivers/timer.h"
#include "ports.h"
#include "../mm/kheap.h"
#include "../process/process.h"
#include "../process/scheduler.h"
#include "../lib/printf.h"
#include "../lib/string.h"
#include "../fs/vfs.h"
#include "../fs/diskfs.h"
#include "../shell/shell.h"
#include "../auth/users.h"
#include "../kernel/elf.h"
#include "../mm/pmm.h"
#include "../fs/blockfs.h"
#include "../drivers/rtc.h"
#include "../drivers/acpi_ec.h"

extern void syscall_stub(void);
extern void enter_usermode(uint32_t entry, uint32_t user_stack);
extern void tss_set_kernel_stack(uint32_t esp0);

static bool g_in_usermode;        /* unused now but kept for sig */

/* ---- ELF exit jump buffer ----
 * (Misma maquinaria que antes; ver elf_jmp.asm para el setjmp/longjmp.) */
typedef struct {
    uint32_t valid;
    uint32_t esp, ebp, ebx, esi, edi, eip;
    int      exit_code;
} elf_jmp_t;

static elf_jmp_t g_elf_jmp;

extern int  elf_jmp_setjmp (elf_jmp_t *dst);
extern void elf_jmp_longjmp(elf_jmp_t *src) __attribute__((noreturn));

void elf_arm_exit_jmp(void)
{
    g_elf_jmp.valid = 0;
    if (elf_jmp_setjmp(&g_elf_jmp) == 0)
        g_elf_jmp.valid = 1;
}

int elf_get_exit_code(void) { return g_elf_jmp.exit_code; }

void elf_disarm_exit_jmp(void) { g_elf_jmp.valid = 0; }

bool usermode_fault_kill(int signal_code)
{
    process_exit(signal_code);
    if (g_elf_jmp.valid) {
        g_elf_jmp.exit_code = signal_code;
        g_elf_jmp.valid     = 0;
        elf_jmp_longjmp(&g_elf_jmp);
    }
    schedule();
    return true;
}

void syscall_install(void)
{
    idt_set_gate(0x80, (uint32_t)syscall_stub, 0x08, 0xEE);
}

/* ---------------- helpers privados ---------------- */

static shell_state_t *S(void) { return shell_get_state(); }

static vfs_node_t *cwd_of_caller(void)
{
    shell_state_t *s = S();
    return s ? s->cwd : vfs_get_root();
}

static int is_root_user(void)
{
    user_t *u = current_user();
    return (u && u->uid == 0) ? 1 : 0;
}

/* Mini "handle table" para opendir/readdir/closedir.
 * Sin fd-table por proceso todavía, así que es global cooperativa. */
#define DH_MAX 32
static vfs_node_t *dh_table[DH_MAX];
static uint32_t    dh_idx[DH_MAX];   /* índice del siguiente readdir */

/* === fd-table simple, global por ahora ===
 *
 * Cada fd es {nodo VFS, posición actual, flags}.  Como aún no tenemos
 * fd-table por proceso, los descriptores se comparten entre todos los
 * programas ring-3.  El shell ring-3 los usa secuencialmente (open, read,
 * close) sin solaparse con otros procesos, así que es suficiente.
 *
 * fds 0/1/2 (stdin/stdout/stderr) son "virtuales": read en 0 -> teclado,
 * write en 1/2 -> VGA + serial.  Esos no consumen slot.
 */
#define FD_MAX 64
typedef struct {
    vfs_node_t *node;
    uint32_t    pos;
    int         flags;
    int         used;
} kfd_t;
static kfd_t kfds[FD_MAX];

static int alloc_fd(void) {
    for (int i = 3; i < FD_MAX; i++)   /* 0/1/2 reservados */
        if (!kfds[i].used) return i;
    return -1;
}

/* ---------------- handler ---------------- */

void syscall_handler(registers_t *regs)
{
    uint32_t num = regs->eax;
    uint32_t a1  = regs->ebx;
    uint32_t a2  = regs->ecx;
    uint32_t a3  = regs->edx;

    switch (num) {

    /* ---- proceso ---- */
    case SYS_EXIT:
        process_exit((int)a1);
        if (g_elf_jmp.valid) {
            g_elf_jmp.exit_code = (int)a1;
            g_elf_jmp.valid     = 0;
            elf_jmp_longjmp(&g_elf_jmp);   /* nunca regresa */
        }
        schedule();
        break;

    case SYS_GETPID:
        regs->eax = process_get_current()->pid;
        break;

    case SYS_YIELD:
        schedule();
        regs->eax = 0;
        break;

    case SYS_SLEEP:
        sleep(a1);
        regs->eax = 0;
        break;

    case SYS_UPTIME:
        regs->eax = uptime();
        break;

    /* ---- io básico ---- */
    case SYS_WRITE: {
        const char *buf = (const char *)a2;
        uint32_t len = a3;
        for (uint32_t i = 0; i < len; i++) {
            vga_putchar(buf[i]);  /* respeta vga_capture_begin internamente */
            /* Si vga capture está activa, no duplicamos al serial — el
             * usuario quiere redir a archivo, no inundar el debug serial. */
            extern int vga_capture_active(void);
            if (!vga_capture_active())
                serial_write_char(buf[i]);
        }
        regs->eax = len;
        break;
    }

    case SYS_GETC:
        regs->eax = (uint32_t)keyboard_getchar();
        break;

    case SYS_GETLINE: {
        char    *buf = (char *)a1;
        uint32_t max = a2;
        if (max == 0) { regs->eax = 0; break; }
        regs->eax = (uint32_t)keyboard_readline(buf, (int)max);
        break;
    }

    case SYS_CLEAR:
        vga_init();
        regs->eax = 0;
        break;

    /* ---- fs: alto nivel ---- */
    case SYS_READFILE: {
        const char *path  = (const char *)a1;
        uint8_t    *buf   = (uint8_t    *)a2;
        uint32_t    maxsz = a3;
        vfs_node_t *n = vfs_resolve(path, cwd_of_caller());
        if (!n || n->type != VFS_FILE) { regs->eax = (uint32_t)-1; break; }
        uint32_t want = n->size < maxsz ? n->size : maxsz;
        regs->eax = vfs_read(n, 0, want, buf);
        break;
    }

    case SYS_WRITEFILE: {
        const char *path = (const char *)a1;
        uint8_t    *buf  = (uint8_t    *)a2;
        uint32_t    len  = a3;
        vfs_node_t *n = vfs_create(path, cwd_of_caller());
        if (!n) { regs->eax = (uint32_t)-1; break; }
        n->size = 0;
        regs->eax = vfs_write(n, 0, len, buf);
        break;
    }

    /* ---- fs: directorios ---- */
    case SYS_GETCWD: {
        char *buf = (char *)a1; int max = (int)a2;
        if (max <= 0) { regs->eax = 0; break; }
        char tmp[256];
        vfs_get_path(cwd_of_caller(), tmp);
        int n = 0; while (tmp[n] && n < max - 1) { buf[n] = tmp[n]; n++; }
        buf[n] = '\0';
        regs->eax = (uint32_t)n;
        break;
    }

    case SYS_CHDIR: {
        const char *p = (const char *)a1;
        vfs_node_t *n = vfs_resolve(p, cwd_of_caller());
        if (!n || n->type != VFS_DIRECTORY) { regs->eax = (uint32_t)-1; break; }
        S()->cwd = n;
        regs->eax = 0;
        break;
    }

    case SYS_MKDIR: {
        const char *p = (const char *)a1;
        regs->eax = vfs_mkdir(p, cwd_of_caller()) ? 0 : (uint32_t)-1;
        break;
    }

    case SYS_RMDIR: {
        const char *p = (const char *)a1;
        vfs_node_t *n = vfs_resolve(p, cwd_of_caller());
        if (!n || n->type != VFS_DIRECTORY) { regs->eax = (uint32_t)-1; break; }
        if (n->child_count > 0)              { regs->eax = (uint32_t)-1; break; }
        regs->eax = vfs_delete(p, cwd_of_caller()) >= 0 ? 0 : (uint32_t)-1;
        break;
    }

    case SYS_UNLINK: {
        const char *p = (const char *)a1;
        regs->eax = vfs_delete(p, cwd_of_caller()) >= 0 ? 0 : (uint32_t)-1;
        break;
    }

    case SYS_RENAME: {
        const char *src = (const char *)a1;
        const char *dst = (const char *)a2;
        vfs_node_t *sn = vfs_resolve(src, cwd_of_caller());
        if (!sn) { regs->eax = (uint32_t)-1; break; }
        /* implementación simple: leer src, escribir dst, borrar src.
         * No conserva metadata fina, pero suficiente para mv simple. */
        if (sn->type == VFS_FILE) {
            uint8_t *tmp = (uint8_t *)kmalloc(sn->size + 1);
            if (!tmp) { regs->eax = (uint32_t)-1; break; }
            uint32_t got = vfs_read(sn, 0, sn->size, tmp);
            vfs_node_t *dn = vfs_create(dst, cwd_of_caller());
            if (!dn) { kfree(tmp); regs->eax = (uint32_t)-1; break; }
            dn->size = 0;
            vfs_write(dn, 0, got, tmp);
            kfree(tmp);
            vfs_delete(src, cwd_of_caller());
            regs->eax = 0;
        } else {
            regs->eax = (uint32_t)-1;  /* dirs no soportado todavía */
        }
        break;
    }

    case SYS_OPENDIR: {
        const char *p = (const char *)a1;
        vfs_node_t *n = vfs_resolve(p, cwd_of_caller());
        if (!n || n->type != VFS_DIRECTORY) { regs->eax = (uint32_t)-1; break; }
        int slot = -1;
        for (int i = 1; i < DH_MAX; i++) if (!dh_table[i]) { slot = i; break; }
        if (slot < 0) { regs->eax = (uint32_t)-1; break; }
        dh_table[slot] = n;
        dh_idx[slot] = 0;
        regs->eax = (uint32_t)slot;
        break;
    }

    case SYS_READDIR: {
        int dh = (int)a1;
        trinux_dirent_t *de = (trinux_dirent_t *)a2;
        if (dh <= 0 || dh >= DH_MAX || !dh_table[dh]) {
            regs->eax = (uint32_t)-1; break;
        }
        vfs_node_t *c = vfs_readdir(dh_table[dh], dh_idx[dh]);
        if (!c) { regs->eax = (uint32_t)-1; break; }
        dh_idx[dh]++;
        int i = 0;
        while (c->name[i] && i < 63) { de->name[i] = c->name[i]; i++; }
        de->name[i] = '\0';
        de->type = c->type;
        regs->eax = 0;
        break;
    }

    case SYS_CLOSEDIR: {
        int dh = (int)a1;
        if (dh > 0 && dh < DH_MAX) {
            dh_table[dh] = NULL;
            dh_idx[dh] = 0;
        }
        regs->eax = 0;
        break;
    }

    case SYS_STAT: {
        const char *p = (const char *)a1;
        trinux_stat_t *st = (trinux_stat_t *)a2;
        vfs_node_t *n = vfs_resolve(p, cwd_of_caller());
        if (!n) { regs->eax = (uint32_t)-1; break; }
        int i = 0;
        while (n->name[i] && i < 63) { st->name[i] = n->name[i]; i++; }
        st->name[i] = '\0';
        st->size = n->size;
        st->type = n->type;
        st->perm = n->permissions;
        st->uid  = n->owner_uid;
        st->gid  = n->owner_gid;
        regs->eax = 0;
        break;
    }

    /* ---- sistema / privilegio ---- */
    case SYS_HOSTNAME: {
        char *buf = (char *)a1; int max = (int)a2;
        const char *h = S() ? S()->hostname : "trinux";
        int n = 0;
        while (h[n] && n < max - 1) { buf[n] = h[n]; n++; }
        buf[n] = '\0';
        regs->eax = (uint32_t)n;
        break;
    }

    case SYS_GETUID:
        regs->eax = (uint32_t)(current_user() ? current_user()->uid : 0);
        break;

    case SYS_GETUSER: {
        char *buf = (char *)a1; int max = (int)a2;
        const char *u = current_user() ? current_user()->name : "?";
        int n = 0;
        while (u[n] && n < max - 1) { buf[n] = u[n]; n++; }
        buf[n] = '\0';
        regs->eax = (uint32_t)n;
        break;
    }

    case SYS_REBOOT: {
        if (!is_root_user()) { regs->eax = (uint32_t)-1; break; }
        kprintf("Rebooting...\n");
        uint8_t good = 0x02;
        while (good & 0x02) good = inb(0x64);
        outb(0x64, 0xFE);
        __asm__ volatile("cli");
        for (;;) __asm__ volatile("hlt");
        break;
    }

    case SYS_SHUTDOWN: {
        if (!is_root_user()) { regs->eax = (uint32_t)-1; break; }
        kprintf("System halted. It is now safe to power off.\n");
        outw(0x604, 0x2000);
        outw(0xB004, 0x2000);
        __asm__ volatile("cli");
        for (;;) __asm__ volatile("hlt");
        break;
    }

    case SYS_KILL: {
        int pid = (int)a1; (void)a2;
        if (!is_root_user()) { regs->eax = (uint32_t)-1; break; }
        regs->eax = process_kill((uint32_t)pid) >= 0 ? 0 : (uint32_t)-1;
        break;
    }

    case SYS_SYNC: {
        int r = diskfs_save();
        regs->eax = (r >= 0) ? 0 : (uint32_t)-1;
        break;
    }

    /* ============================================================
     * ABI 3.0  -  shell en ring-3
     * ============================================================ */
    case SYS_FILE_OPEN: {
        const char *path = (const char *)a1;
        int flags = (int)a2;
        vfs_node_t *n = vfs_resolve(path, cwd_of_caller());
        if (!n && (flags & O_CREAT))
            n = vfs_create(path, cwd_of_caller());
        if (!n) { regs->eax = (uint32_t)-1; break; }
        int fd = alloc_fd();
        if (fd < 0) { regs->eax = (uint32_t)-1; break; }
        kfds[fd].node = n;
        kfds[fd].pos = (flags & O_APPEND) ? n->size : 0;
        if (flags & O_TRUNC) n->size = 0;
        kfds[fd].flags = flags;
        kfds[fd].used = 1;
        regs->eax = (uint32_t)fd;
        break;
    }

    case SYS_FILE_READ: {
        int fd = (int)a1;
        if (fd == 0) {
            char *b = (char *)a2;
            int len = (int)a3;
            regs->eax = (uint32_t)keyboard_readline(b, len);
            break;
        }
        if (fd < 3 || fd >= FD_MAX || !kfds[fd].used) {
            regs->eax = (uint32_t)-1; break;
        }
        kfd_t *k = &kfds[fd];
        if (k->pos >= k->node->size) { regs->eax = 0; break; }
        uint32_t want = a3;
        if (k->pos + want > k->node->size) want = k->node->size - k->pos;
        uint32_t got = vfs_read(k->node, k->pos, want, (uint8_t *)a2);
        k->pos += got;
        regs->eax = got;
        break;
    }

    case SYS_FILE_WRITE: {
        int fd = (int)a1;
        const char *buf = (const char *)a2;
        uint32_t len = a3;
        if (fd == 1 || fd == 2) {
            for (uint32_t i = 0; i < len; i++) {
                vga_putchar(buf[i]);
                serial_write_char(buf[i]);
            }
            regs->eax = len;
            break;
        }
        if (fd < 3 || fd >= FD_MAX || !kfds[fd].used) {
            regs->eax = (uint32_t)-1; break;
        }
        kfd_t *k = &kfds[fd];
        uint32_t wrote = vfs_write(k->node, k->pos, len, (uint8_t *)buf);
        k->pos += wrote;
        regs->eax = wrote;
        break;
    }

    case SYS_FILE_CLOSE: {
        int fd = (int)a1;
        if (fd >= 3 && fd < FD_MAX) kfds[fd].used = 0;
        regs->eax = 0;
        break;
    }

    case SYS_FILE_SEEK: {
        int fd = (int)a1;
        int off = (int)a2;
        int whence = (int)a3;
        if (fd < 3 || fd >= FD_MAX || !kfds[fd].used) {
            regs->eax = (uint32_t)-1; break;
        }
        kfd_t *k = &kfds[fd];
        uint32_t newpos = k->pos;
        if (whence == SEEK_SET) newpos = (uint32_t)off;
        else if (whence == SEEK_CUR) newpos = k->pos + off;
        else if (whence == SEEK_END) newpos = k->node->size + off;
        k->pos = newpos;
        regs->eax = newpos;
        break;
    }

    case SYS_KEY_RAW: {
        int k = keyboard_getchar();
        /* Traducimos los códigos kernel-internos a los KEY_F_* públicos
         * para que la ABI no dependa del enum del driver. */
        switch (k) {
            case KEY_UP:    k = KEY_F_UP;    break;
            case KEY_DOWN:  k = KEY_F_DOWN;  break;
            case KEY_LEFT:  k = KEY_F_LEFT;  break;
            case KEY_RIGHT: k = KEY_F_RIGHT; break;
            case KEY_HOME:  k = KEY_F_HOME;  break;
            case KEY_END:   k = KEY_F_END;   break;
            default: break;
        }
        regs->eax = (uint32_t)k;
        break;
    }

    case SYS_VGA_COLOR: {
        /* vga_entry_color es static inline en vga.h; lo replicamos: byte =
         * (bg << 4) | fg, ambos en 0..15. */
        uint8_t color = (uint8_t)(((a2 & 0x0F) << 4) | (a1 & 0x0F));
        vga_set_color(color);
        regs->eax = 0;
        break;
    }

    /* === SPAWN: lanza un ELF en ring-3 (sincrónicamente con WAIT) === */
    case SYS_SPAWN: {
        const char *path = (const char *)a1;
        char **argv = (char **)a2;
        uint32_t flags = a3;
        (void)flags;  /* hoy siempre WAIT */
        /* Copia local de path por la misma razón que SPAWN_R */
        static char k_path2[128];
        int i=0; while (path[i] && i<127){ k_path2[i]=path[i]; i++; } k_path2[i]=0;
        int argc = 0;
        if (argv) while (argv[argc]) argc++;
        int rc = elf_exec_argv(k_path2, cwd_of_caller(), argc, argv);
        regs->eax = (uint32_t)rc;
        break;
    }

    case SYS_WAITPID: {
        /* Como SPAWN ya espera, WAITPID es no-op. Devuelve el último exit_code
         * en *(int*)a2 si lo dan. */
        int *out = (int *)a2;
        if (out) *out = 0;
        regs->eax = 0;
        break;
    }

    case SYS_SPAWN_R: {
        spawn_req_t *r = (spawn_req_t *)a1;
        if (!r || !r->path) { regs->eax = (uint32_t)-1; break; }
        int argc = 0;
        if (r->argv) while (r->argv[argc]) argc++;

        /* COPIAMOS los punteros y campos críticos a memoria local del kernel
         * porque la memoria del shell padre se RESPALDA y se restaura
         * durante elf_exec_argv() — pero también se PISA por el ELF hijo
         * que vive en el mismo VA 0x08048000.  El backup conserva el código
         * binario; las strings de r-> viven en stack/scratch del shell y
         * SÍ se pisan transitoriamente, así que las leemos antes. */
        static char k_path[128];
        static char k_outpath[128];
        int   has_out = (r->stdout_path != NULL);
        int   append  = r->append;
        const char *src;
        src = r->path;
        { int i=0; while (src[i] && i<127){ k_path[i]=src[i]; i++; } k_path[i]=0; }
        if (has_out) {
            src = r->stdout_path;
            int i=0; while (src[i] && i<127){ k_outpath[i]=src[i]; i++; } k_outpath[i]=0;
        }

        char **argv_user = r->argv;   /* elf_exec_argv copia argv en kheap
                                         si g_nest >= 1, así que está OK */

        if (has_out) {
            static char cap[16384];
            vga_capture_begin(cap, sizeof(cap));
            int rc = elf_exec_argv(k_path, cwd_of_caller(), argc, argv_user);
            uint32_t n = vga_capture_end();
            vfs_node_t *f = vfs_create(k_outpath, cwd_of_caller());
            if (f) {
                if (append) {
                    vfs_write(f, f->size, n, (uint8_t *)cap);
                } else {
                    f->size = 0;
                    vfs_write(f, 0, n, (uint8_t *)cap);
                }
            }
            regs->eax = (uint32_t)rc;
        } else {
            int rc = elf_exec_argv(k_path, cwd_of_caller(), argc, argv_user);
            regs->eax = (uint32_t)rc;
        }
        break;
    }

    case SYS_USERADD: {
        if (!is_root_user()) { regs->eax = (uint32_t)-1; break; }
        useradd_req_t *r = (useradd_req_t *)a1;
        if (!r) { regs->eax = (uint32_t)-1; break; }
        user_t *u = users_add(r->name, r->pass, r->uid, r->gid, r->home);
        regs->eax = u ? 0 : (uint32_t)-1;
        break;
    }

    case SYS_PASSWD: {
        passwd_req_t *r = (passwd_req_t *)a1;
        if (!r) { regs->eax = (uint32_t)-1; break; }
        /* sólo root puede cambiar passwords de otros; un usuario sólo
         * puede cambiar el suyo. */
        user_t *me = current_user();
        if (!me) { regs->eax = (uint32_t)-1; break; }
        if (me->uid != 0 && strcmp(me->name, r->user) != 0) {
            regs->eax = (uint32_t)-1; break;
        }
        regs->eax = users_set_password(r->user, r->new_pass) ? 0 : (uint32_t)-1;
        break;
    }

    case SYS_LOGIN: {
        const char *u = (const char *)a1;
        const char *p = (const char *)a2;
        user_t *uu = users_find(u);
        if (!uu || !users_check_password(u, p)) { regs->eax = (uint32_t)-1; break; }
        set_current_user(uu);
        /* sincronizar el cwd del shell con el home del usuario */
        vfs_node_t *home = vfs_resolve(uu->home, vfs_get_root());
        if (home && S()) S()->cwd = home;
        regs->eax = 0;
        break;
    }

    case SYS_LISTPROC: {
        plist_req_t *r = (plist_req_t *)a1;
        if (!r || !r->list || r->max <= 0) { regs->eax = 0; break; }
        int n = 0;
        for (uint32_t i = 0; i < process_count() && n < r->max; i++) {
            process_t *p = process_at(i);
            if (!p) continue;
            r->list[n].pid = p->pid;
            for (int k = 0; k < 31 && p->name[k]; k++) r->list[n].name[k] = p->name[k];
            r->list[n].name[31] = 0;
            r->list[n].cpu_ticks = p->cpu_ticks;
            r->list[n].state     = (uint32_t)p->state;
            r->list[n].priority  = p->priority;
            n++;
        }
        r->got = n;
        regs->eax = (uint32_t)n;
        break;
    }

    /* ============================================================
     * ABI 3.1 - finalizing all coreutils on ring 3
     * ============================================================ */
    case SYS_CHMOD: {
        const char *p = (const char *)a1;
        regs->eax = vfs_chmod(p, cwd_of_caller(), a2) >= 0 ? 0 : (uint32_t)-1;
        break;
    }
    case SYS_CHOWN: {
        const char *p = (const char *)a1;
        if (!is_root_user()) { regs->eax = (uint32_t)-1; break; }
        regs->eax = vfs_chown(p, cwd_of_caller(), a2, a3) >= 0 ? 0 : (uint32_t)-1;
        break;
    }
    case SYS_MEMINFO: {
        mem_info_t *m = (mem_info_t *)a1;
        if (!m) { regs->eax = (uint32_t)-1; break; }
        m->total_bytes = pmm_get_total_memory();
        m->used_bytes  = pmm_get_used_memory();
        m->free_bytes  = pmm_get_free_memory();
        regs->eax = 0;
        break;
    }
    case SYS_DFINFO: {
        df_info_t *d = (df_info_t *)a1;
        if (!d) { regs->eax = (uint32_t)-1; break; }
        d->have_disk = blockfs_available() ? 1 : 0;
        d->block_size = BLOCK_SIZE;
        if (d->have_disk) {
            d->total_blocks = blockfs_total_blocks();
            d->used_blocks  = blockfs_used_blocks();
        } else {
            d->total_blocks = 0;
            d->used_blocks  = 0;
        }
        regs->eax = 0;
        break;
    }
    case SYS_DATETIME: {
        datetime_u_t *t = (datetime_u_t *)a1;
        if (!t) { regs->eax = (uint32_t)-1; break; }
        datetime_t dt;
        rtc_read_datetime(&dt);
        t->year = dt.year; t->month = dt.month; t->day = dt.day;
        t->hour = dt.hour; t->minute = dt.minute; t->second = dt.second;
        regs->eax = 0;
        break;
    }
    case SYS_USERLIST: {
        user_list_req_t *r = (user_list_req_t *)a1;
        if (!r || !r->list || r->max <= 0) { regs->eax = 0; break; }
        int n = 0;
        int nu = users_count();
        for (int i = 0; i < nu && n < r->max; i++) {
            user_t *u = users_at(i);
            if (!u) continue;
            int k=0; while (u->name[k] && k<31){ r->list[n].name[k]=u->name[k]; k++; }
            r->list[n].name[k]=0;
            r->list[n].uid = u->uid;
            r->list[n].gid = u->gid;
            k=0; while (u->home[k] && k<63){ r->list[n].home[k]=u->home[k]; k++; }
            r->list[n].home[k]=0;
            n++;
        }
        r->got = n;
        regs->eax = (uint32_t)n;
        break;
    }
    case SYS_RENICE: {
        int pid = (int)a1, prio = (int)a2;
        regs->eax = process_set_priority((uint32_t)pid, prio) >= 0 ? 0 : (uint32_t)-1;
        break;
    }
    case SYS_BATTERY: {
        battery_u_t *b = (battery_u_t *)a1;
        if (!b) { regs->eax = (uint32_t)-1; break; }
        battery_info_t info;
        if (!acpi_ec_read_battery(&info) || !info.present) {
            b->present = 0; b->percent = 0; b->discharging = 0; b->time_minutes = -1;
            regs->eax = (uint32_t)-1; break;
        }
        b->present = 1;
        b->percent = (int)info.percentage;
        b->discharging = info.discharging ? 1 : 0;
        b->time_minutes = -1;
        regs->eax = 0;
        break;
    }
    case SYS_LOGOUT: {
        set_current_user(NULL);
        regs->eax = 0;
        break;
    }
    case SYS_SU: {
        const char *u = (const char *)a1;
        const char *p = (const char *)a2;
        user_t *uu = users_find(u);
        if (!uu) { regs->eax = (uint32_t)-1; break; }
        /* root no necesita password */
        if (!is_root_user() && !users_check_password(u, p)) {
            regs->eax = (uint32_t)-1; break;
        }
        set_current_user(uu);
        regs->eax = 0;
        break;
    }
    case SYS_GETGROUPS: {
        groups_req_t *r = (groups_req_t *)a1;
        if (!r || !r->list || r->max <= 0) { regs->eax = 0; break; }
        user_t *u = current_user();
        if (!u) { r->got = 0; regs->eax = 0; break; }
        r->list[0].uid = u->uid;
        r->list[0].gid = u->gid;
        r->got = 1;
        regs->eax = 1;
        break;
    }
    case SYS_VFS_FIND: {
        find_req_t *r = (find_req_t *)a1;
        if (!r || !r->out_paths || r->max <= 0) { regs->eax = 0; break; }
        vfs_node_t *root = vfs_resolve(r->root_path ? r->root_path : "/", cwd_of_caller());
        if (!root) { r->got = 0; regs->eax = 0; break; }
        /* Recursive find, BFS sencilla */
        typedef struct { vfs_node_t *n; char path[256]; } stk_t;
        static stk_t stk[256];
        int sp = 0;
        stk[sp].n = root;
        char rp[256]; vfs_get_path(root, rp);
        int rl = 0; while (rp[rl] && rl<255){ stk[sp].path[rl]=rp[rl]; rl++; }
        stk[sp].path[rl] = 0;
        sp++;
        int got = 0;
        const char *needle = r->name_substr;
        while (sp > 0 && got < r->max) {
            sp--;
            vfs_node_t *n = stk[sp].n;
            char cur[256];
            int cl = 0; while (stk[sp].path[cl] && cl<255){ cur[cl]=stk[sp].path[cl]; cl++; }
            cur[cl] = 0;
            /* check name */
            if (needle) {
                int match = 0;
                for (int i = 0; n->name[i]; i++) {
                    int j = 0;
                    while (needle[j] && n->name[i+j] == needle[j]) j++;
                    if (!needle[j]) { match = 1; break; }
                }
                if (match) {
                    int k=0; while (cur[k] && k<255){ r->out_paths[got][k]=cur[k]; k++; }
                    r->out_paths[got][k]=0;
                    got++;
                }
            } else {
                int k=0; while (cur[k] && k<255){ r->out_paths[got][k]=cur[k]; k++; }
                r->out_paths[got][k]=0;
                got++;
            }
            if (n->type == VFS_DIRECTORY) {
                for (uint32_t i = 0; sp < 256; i++) {
                    vfs_node_t *c = vfs_readdir(n, i);
                    if (!c) break;
                    stk[sp].n = c;
                    int p = 0;
                    while (cur[p] && p<200){ stk[sp].path[p]=cur[p]; p++; }
                    if (p == 0 || stk[sp].path[p-1] != '/') stk[sp].path[p++] = '/';
                    int q = 0;
                    while (c->name[q] && p<255){ stk[sp].path[p++]=c->name[q++]; }
                    stk[sp].path[p] = 0;
                    sp++;
                }
            }
        }
        r->got = got;
        regs->eax = (uint32_t)got;
        break;
    }
    case SYS_VFS_TREE: {
        /* Imprime el árbol del root_path via vga_putchar (recursión limitada). */
        tree_req_t *r = (tree_req_t *)a1;
        if (!r) { regs->eax = (uint32_t)-1; break; }
        vfs_node_t *root = vfs_resolve(r->root_path ? r->root_path : "/", cwd_of_caller());
        if (!root) { regs->eax = (uint32_t)-1; break; }
        /* iterativo DFS */
        typedef struct { vfs_node_t *n; int depth; } it_t;
        static it_t st[256];
        int sp = 0;
        st[sp].n = root; st[sp].depth = 0; sp++;
        int max_d = r->max_depth > 0 ? r->max_depth : 32;
        while (sp > 0) {
            sp--;
            vfs_node_t *n = st[sp].n;
            int d = st[sp].depth;
            for (int i = 0; i < d; i++) { vga_putchar(' '); vga_putchar(' '); }
            for (int i = 0; n->name[i]; i++) vga_putchar(n->name[i]);
            if (n->type == VFS_DIRECTORY) vga_putchar('/');
            vga_putchar('\n');
            if (n->type == VFS_DIRECTORY && d < max_d) {
                /* push children en orden inverso para mantener orden DFS */
                int count = 0;
                for (uint32_t i = 0; ; i++) { if (!vfs_readdir(n, i)) break; count++; }
                for (int i = count - 1; i >= 0 && sp < 256; i--) {
                    st[sp].n = vfs_readdir(n, (uint32_t)i);
                    st[sp].depth = d + 1;
                    sp++;
                }
            }
        }
        regs->eax = 0;
        break;
    }
    case SYS_RESOLVE: {
        const char *path = (const char *)a1;
        char *out = (char *)a2;
        int max = (int)a3;
        if (!path || !out || max <= 0) { regs->eax = (uint32_t)-1; break; }
        vfs_node_t *n = vfs_resolve(path, cwd_of_caller());
        if (!n) { regs->eax = (uint32_t)-1; break; }
        char tmp[256]; vfs_get_path(n, tmp);
        int i = 0; while (tmp[i] && i < max-1){ out[i]=tmp[i]; i++; }
        out[i] = 0;
        regs->eax = (uint32_t)i;
        break;
    }

    default:
        kprintf("\n[syscall] unknown syscall %u\n", num);
        regs->eax = (uint32_t)-1;
        break;
    }
}

/* ----------------------------------------------------------------------- */

static void usermode_trampoline(void) {
    process_t* p = process_get_current();
    uint8_t *ustack = (uint8_t *)kmalloc_aligned(8192);
    uint32_t user_stack_top = (uint32_t)(ustack + 8192) & ~0xF;

    enter_usermode((uint32_t)p->entry, user_stack_top);
    /* nunca regresa */
}

int usermode_run(const char *name, void (*entry)(void))
{
    process_t *p = process_create(name, usermode_trampoline);
    if (!p) return -1;
    p->entry = entry;
    scheduler_add(p);
    return p->pid;
}

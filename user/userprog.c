/* user/userprog.c  -  a demonstration program that runs in RING 3.
 *
 * Everything here executes unprivileged: it CANNOT touch hardware, kernel
 * memory, or I/O ports directly. Its only window to the system is `int 0x80`.
 * If it tried `cli`, `outb`, or to read kernel memory, the CPU would fault.
 *
 * To prove the isolation is real, the program also (optionally) attempts a
 * privileged instruction at the end if asked — see userprog_badboy().
 */
#include "../lib/types.h"
#include "../cpu/syscall.h"

/* ---- tiny ring-3 "libc": each is a single `int 0x80` ---- */
static inline int sys1(int n, int a) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "b"(a) : "memory");
    return ret;
}
static inline int sys3(int n, int a, int b, int c) {
    int ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return ret;
}
static inline int sys0(int n) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n) : "memory");
    return ret;
}

static void u_write(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys3(SYS_WRITE, 1, (int)s, len);
}

/* itoa for unsigned, base 10, into a small ring-3 buffer */
static void u_write_u(uint32_t v) {
    char buf[12];
    int i = 10;
    buf[11] = '\0';
    if (v == 0) { buf[i--] = '0'; }
    while (v && i >= 0) { buf[i--] = '0' + (v % 10); v /= 10; }
    u_write(&buf[i + 1]);
}

/* The actual ring-3 entry point. Note: NO kernel calls, only syscalls. */
void userprog_main(void)
{
    u_write("\n  [ring3] Hello from userspace! (CPL=3)\n");

    int pid = sys0(SYS_GETPID);
    u_write("  [ring3] my pid via SYS_GETPID = ");
    u_write_u((uint32_t)pid);
    u_write("\n");

    uint32_t up = (uint32_t)sys0(SYS_UPTIME);
    u_write("  [ring3] uptime (SYS_UPTIME)   = ");
    u_write_u(up);
    u_write(" s\n");

    u_write("  [ring3] counting via syscalls: ");
    for (int i = 1; i <= 5; i++) {
        u_write_u((uint32_t)i);
        u_write(" ");
        sys1(SYS_SLEEP, 120);   /* 120 ms between numbers, via SYS_SLEEP */
    }
    u_write("\n  [ring3] done, calling SYS_EXIT(0)\n");

    sys1(SYS_EXIT, 0);          /* returns control to the kernel */

    /* never reached */
    for (;;) { }
}

/* A program that *tries* to break out of ring 3 to prove isolation works.
 * The privileged instruction triggers a General Protection Fault, which the
 * kernel reports — the machine does NOT let userspace do this. */
void userprog_badboy(void)
{
    u_write("\n  [ring3] attempting privileged 'cli' (should FAULT)...\n");
    __asm__ volatile("cli");    /* #GP in ring 3 */
    u_write("  [ring3] if you see this, isolation FAILED\n");
    sys1(SYS_EXIT, 1);
}

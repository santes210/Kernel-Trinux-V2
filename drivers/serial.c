/* drivers/serial.c  -  COM1 (16550 UART) driver.
 *
 *   - TX: synchronous serial_write_char() for kernel logs / panic output.
 *   - RX: interrupt-driven (IRQ4).  Bytes coming in are decoded as TTY
 *         input (ANSI escape sequences for arrows / Home / End / Del are
 *         translated to KEY_* codes) and pushed into the same input queue
 *         the PS/2 keyboard driver uses via keyboard_inject_char().
 *
 * This is the fastest, lossless input path for Android / Termux clipboards:
 * launch QEMU with `-display none -serial mon:stdio` (or `-nographic`) and
 * paste straight into the terminal.  The host kernel hands the bytes to
 * QEMU, QEMU pushes them into the UART RX FIFO, IRQ4 fires, we drain the
 * FIFO completely on every IRQ — no scancodes, no Shift/Caps machinery,
 * so 700-line pastes go in without losing a byte.
 */
#include "serial.h"
#include "keyboard.h"
#include "../cpu/ports.h"
#include "../cpu/irq.h"
#include "../lib/printf.h"

/* 16550 register offsets (relative to COM1 base 0x3F8). */
#define UART_RBR  0   /* RX buffer (read)              -- DLAB=0 */
#define UART_THR  0   /* TX holding (write)            -- DLAB=0 */
#define UART_DLL  0   /* divisor low                    -- DLAB=1 */
#define UART_IER  1   /* interrupt enable              -- DLAB=0 */
#define UART_DLH  1   /* divisor high                   -- DLAB=1 */
#define UART_IIR  2   /* interrupt id (read)           */
#define UART_FCR  2   /* FIFO control (write)          */
#define UART_LCR  3   /* line control                  */
#define UART_MCR  4   /* modem control                 */
#define UART_LSR  5   /* line status                   */
#define UART_MSR  6   /* modem status                  */

#define LSR_DATA_READY  0x01
#define LSR_THR_EMPTY   0x20

/* ---- ANSI / TTY input decoder ----
 *
 * We get a raw byte stream.  Translate the common control sequences so the
 * shell / editor see the same KEY_* codes the PS/2 driver produces:
 *
 *   \r        -> '\n'   (Enter on a TTY is CR)
 *   0x7F      -> '\b'   (DEL on most terminals = Backspace)
 *   0x1B      -> start of escape sequence (CSI / SS3)
 *   0x1B [ A  -> KEY_UP        0x1B O A  -> KEY_UP
 *   0x1B [ B  -> KEY_DOWN      0x1B O B  -> KEY_DOWN
 *   0x1B [ C  -> KEY_RIGHT     0x1B O C  -> KEY_RIGHT
 *   0x1B [ D  -> KEY_LEFT      0x1B O D  -> KEY_LEFT
 *   0x1B [ H  -> KEY_HOME      0x1B O H  -> KEY_HOME
 *   0x1B [ F  -> KEY_END       0x1B O F  -> KEY_END
 *   0x1B [ 1 ~  -> KEY_HOME    0x1B [ 7 ~ -> KEY_HOME
 *   0x1B [ 4 ~  -> KEY_END     0x1B [ 8 ~ -> KEY_END
 *   0x1B [ 3 ~  -> KEY_DELETE
 *   0x1B [ 1 1 ~ ... 0x1B [ 2 4 ~  -> KEY_F1 .. KEY_F12
 *   lone 0x1B (no continuation within a few bytes) -> KEY_ESC
 *
 * The decoder is a tiny state machine driven byte-by-byte; we never need
 * to know how the host fragments the bytes across IRQs.
 */
typedef enum {
    DEC_NORMAL = 0,
    DEC_ESC,         /* got 0x1B */
    DEC_CSI,         /* got 0x1B [ */
    DEC_SS3,         /* got 0x1B O */
    DEC_CSI_NUM      /* got 0x1B [ <digits> */
} dec_state_t;

static volatile dec_state_t dec_state = DEC_NORMAL;
static volatile int dec_num;            /* numeric parameter being parsed */
static volatile int dec_pending_esc;    /* lone-ESC age (in incoming bytes) */
#define ESC_FLUSH_BYTES 8               /* if no continuation in N bytes, emit ESC */

static void decoder_flush_lone_esc(void)
{
    /* Called whenever we want to commit a pending lone ESC as KEY_ESC. */
    if (dec_state == DEC_ESC && dec_pending_esc > 0) {
        keyboard_inject_char(KEY_ESC);
        dec_state = DEC_NORMAL;
        dec_pending_esc = 0;
    }
}

static void decoder_emit_csi_final(char fin)
{
    int n = (dec_state == DEC_CSI_NUM) ? dec_num : 0;

    if (fin == '~') {
        switch (n) {
        case 1: case 7: keyboard_inject_char(KEY_HOME);   break;
        case 4: case 8: keyboard_inject_char(KEY_END);    break;
        case 3:         keyboard_inject_char(KEY_DELETE); break;
        case 11: keyboard_inject_char(KEY_F1);  break;
        case 12: keyboard_inject_char(KEY_F2);  break;
        case 13: keyboard_inject_char(KEY_F3);  break;
        case 14: keyboard_inject_char(KEY_F4);  break;
        case 15: keyboard_inject_char(KEY_F5);  break;
        case 17: keyboard_inject_char(KEY_F6);  break;
        case 18: keyboard_inject_char(KEY_F7);  break;
        case 19: keyboard_inject_char(KEY_F8);  break;
        case 20: keyboard_inject_char(KEY_F9);  break;
        case 21: keyboard_inject_char(KEY_F10); break;
        case 23: keyboard_inject_char(KEY_F11); break;
        case 24: keyboard_inject_char(KEY_F12); break;
        default: /* unknown — drop quietly */     break;
        }
    } else {
        switch (fin) {
        case 'A': keyboard_inject_char(KEY_UP);    break;
        case 'B': keyboard_inject_char(KEY_DOWN);  break;
        case 'C': keyboard_inject_char(KEY_RIGHT); break;
        case 'D': keyboard_inject_char(KEY_LEFT);  break;
        case 'H': keyboard_inject_char(KEY_HOME);  break;
        case 'F': keyboard_inject_char(KEY_END);   break;
        default:                                   break;
        }
    }
    dec_state = DEC_NORMAL;
    dec_num = 0;
    dec_pending_esc = 0;
}

static void decoder_feed(uint8_t b)
{
    /* Age the lone-ESC timer so a real ESC press eventually fires even if
     * the user is also typing something else. */
    if (dec_state == DEC_ESC) {
        dec_pending_esc++;
        if (dec_pending_esc > ESC_FLUSH_BYTES)
            decoder_flush_lone_esc();
    }

    switch (dec_state) {
    case DEC_NORMAL:
        if (b == 0x1B) {            /* ESC -> start sequence */
            dec_state = DEC_ESC;
            dec_pending_esc = 1;
            return;
        }
        if (b == '\r') {            /* TTY Enter is CR */
            keyboard_inject_char('\n');
            return;
        }
        if (b == 0x7F) {            /* DEL -> Backspace */
            keyboard_inject_char('\b');
            return;
        }
        /* Everything else (printable, \n, \t, control chars like ^C/^S/^X)
         * goes through verbatim — the shell and editor already understand
         * them.  This is what makes paste "just work". */
        keyboard_inject_char((int)b);
        return;

    case DEC_ESC:
        dec_pending_esc = 0;
        if (b == '[') { dec_state = DEC_CSI; dec_num = 0; return; }
        if (b == 'O') { dec_state = DEC_SS3; return; }
        /* Some terminals send Alt-X as ESC X.  We don't have Alt support
         * in the queue, so just emit the second char (closest we can do). */
        if (b == 0x1B) {            /* ESC ESC -> emit one KEY_ESC, stay in ESC */
            keyboard_inject_char(KEY_ESC);
            dec_pending_esc = 1;
            return;
        }
        keyboard_inject_char(KEY_ESC);
        dec_state = DEC_NORMAL;
        /* Re-feed this byte as a normal char. */
        decoder_feed(b);
        return;

    case DEC_CSI:
        if (b >= '0' && b <= '9') {
            dec_num = (b - '0');
            dec_state = DEC_CSI_NUM;
            return;
        }
        decoder_emit_csi_final((char)b);
        return;

    case DEC_CSI_NUM:
        if (b >= '0' && b <= '9') {
            dec_num = dec_num * 10 + (b - '0');
            if (dec_num > 999) dec_num = 999;   /* sanity clamp */
            return;
        }
        if (b == ';') {
            /* Ignore secondary parameters (modifier flags etc.). */
            return;
        }
        decoder_emit_csi_final((char)b);
        return;

    case DEC_SS3:
        decoder_emit_csi_final((char)b);
        return;
    }
}

/* ---- IRQ4 handler: drain the entire RX FIFO ---- */
static void serial_callback(registers_t *regs)
{
    (void)regs;
    int guard = 0;
    while (inb(COM1 + UART_LSR) & LSR_DATA_READY) {
        uint8_t b = inb(COM1 + UART_RBR);
        decoder_feed(b);
        if (++guard > 256) break;   /* never spin forever in an IRQ */
    }
}

void serial_init(void)
{
    /* TX-only bring-up so the very early boot logs work even before the
     * IDT / PIC are installed.  RX with IRQs is enabled later by
     * serial_enable_input(), which must be called AFTER irq_install(). */

    /* Disable interrupts while we configure. */
    outb(COM1 + UART_IER, 0x00);

    /* 115200 baud (divisor = 1) for high-throughput paste. */
    outb(COM1 + UART_LCR, 0x80);     /* enable DLAB */
    outb(COM1 + UART_DLL, 0x01);     /* divisor low  */
    outb(COM1 + UART_DLH, 0x00);     /* divisor high */
    outb(COM1 + UART_LCR, 0x03);     /* 8N1, DLAB off */

    /* Enable + clear FIFOs, set 14-byte RX trigger (deepest standard
     * threshold).  Combined with our IRQ handler draining the entire FIFO,
     * this lets ~700-line pastes through without dropping a byte. */
    outb(COM1 + UART_FCR, 0xC7);

    /* RTS/DSR set, OUT2 enabled (required to route the UART IRQ through
     * the PIC on PC hardware / QEMU). */
    outb(COM1 + UART_MCR, 0x0B);

    /* Reset decoder state. */
    dec_state = DEC_NORMAL;
    dec_num = 0;
    dec_pending_esc = 0;
}

void serial_enable_input(void)
{
    /* Drain anything sitting in the FIFO from boot. */
    while (inb(COM1 + UART_LSR) & LSR_DATA_READY)
        (void)inb(COM1 + UART_RBR);

    /* Hook IRQ4 and enable Received-Data-Available interrupt. */
    irq_register_handler(4, serial_callback);
    outb(COM1 + UART_IER, 0x01);

    /* CRITICAL: BIOS / GRUB hand off with IRQ4 masked in the PIC, so even
     * though the UART is now raising the interrupt the CPU never sees it.
     * Unmask it here, otherwise nothing the user pastes into the serial
     * console ever reaches the shell. */
    irq_clear_mask(4);
}

static int serial_tx_empty(void)
{
    return inb(COM1 + UART_LSR) & LSR_THR_EMPTY;
}

void serial_write_char(char c)
{
    if (c == '\n')
        serial_write_char('\r');
    while (!serial_tx_empty())
        ;
    outb(COM1 + UART_THR, (uint8_t)c);
}

void serial_write(const char *str)
{
    while (*str)
        serial_write_char(*str++);
}

void serial_printf(const char *fmt, ...)
{
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    serial_write(buf);
}

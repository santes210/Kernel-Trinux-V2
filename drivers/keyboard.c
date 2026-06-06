/* drivers/keyboard.c  -  PS/2 keyboard driver (IRQ1), scancode set 1. */
#include "keyboard.h"
#include "../cpu/irq.h"
#include "../cpu/ports.h"
#include "../drivers/vga.h"
#include "../process/scheduler.h"

#define KBD_DATA   0x60
#define KBD_STATUS 0x64
/* Big enough to absorb a full Android-clipboard paste (hundreds of lines)
 * without the circular buffer dropping bytes — the previous 16 KiB was the
 * second reason a ~700-line paste appeared to "freeze" at line 5. */
#define BUF_SIZE   65536

/* Circular buffer of decoded key codes. */
static volatile int kbuf[BUF_SIZE];
static volatile int khead;
static volatile int ktail;

/* Modifier state. */
static volatile bool shift_down;
static volatile bool ctrl_down;
static volatile bool alt_down;
static volatile bool caps_lock;
static volatile bool extended;   /* set after 0xE0 prefix */

/* Safety: if shift has been held for this many consecutive key events
 * without a new shift-make scancode, assume the release was lost and
 * reset.  Same for ctrl/alt.  This fixes stuck modifiers after fast
 * paste from Android / Termux where the PS/2 controller drops bytes. */
static volatile int shift_age;
static volatile int ctrl_age;
static volatile int alt_age;
#define MOD_STUCK_LIMIT  300

/* US QWERTY scancode set 1 -> ASCII (unshifted). */
static const char scancode_normal[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,  'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,  '\\','z','x','c','v','b','n','m',',','.','/',
    0,  '*', 0,  ' ',
    /* rest 0 */
};

static const char scancode_shift[128] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,  'A','S','D','F','G','H','J','K','L',':','"','~',
    0,  '|','Z','X','C','V','B','N','M','<','>','?',
    0,  '*', 0,  ' ',
};

static void buf_push(int key)
{
    int next = (khead + 1) % BUF_SIZE;
    if (next != ktail) {       /* drop on overflow */
        kbuf[khead] = key;
        khead = next;
        /* A byte just landed in the input queue, which most likely means
         * SOMETHING is waiting in keyboard_getchar() / SYS_GETC.  Force
         * the scheduler to re-evaluate on the next timer tick so the
         * waiting task doesn't sit blocked for a whole quantum. */
        scheduler_kick();
    }
}

/* Public: inject a fully-decoded key code (ASCII or KEY_*) into the same
 * input queue the PS/2 driver feeds.  Used by the serial driver so that
 * input pasted into the QEMU serial console reaches the shell / editor
 * exactly as if it had been typed on the PS/2 keyboard — no scancode
 * translation, no Shift/Caps state machine to lose bytes on. */
void keyboard_inject_char(int key)
{
    buf_push(key);
}

static int buf_pop(void)
{
    if (khead == ktail)
        return -1;
    int key = kbuf[ktail];
    ktail = (ktail + 1) % BUF_SIZE;
    return key;
}

static void handle_scancode(uint8_t sc)
{
    /* Extended-key prefix. */
    if (sc == 0xE0) {
        extended = true;
        return;
    }

    bool released = sc & 0x80;
    uint8_t code = sc & 0x7F;

    if (extended) {
        extended = false;
        if (!released) {
            switch (code) {
            case 0x48: buf_push(KEY_UP);     return;
            case 0x50: buf_push(KEY_DOWN);   return;
            case 0x4B: buf_push(KEY_LEFT);   return;
            case 0x4D: buf_push(KEY_RIGHT);  return;
            case 0x47: buf_push(KEY_HOME);   return;
            case 0x4F: buf_push(KEY_END);    return;
            case 0x53: buf_push(KEY_DELETE); return;
            }
        }
        /* extended ctrl/alt */
        if (code == 0x1D) { ctrl_down = !released; ctrl_age = 0; return; }
        if (code == 0x38) { alt_down  = !released; alt_age  = 0; return; }
        return;
    }

    /* Modifiers — reset age counter when a new press/release happens. */
    switch (code) {
    case 0x2A: case 0x36:
        shift_down = !released;
        shift_age = 0;
        return;
    case 0x1D:
        ctrl_down = !released;
        ctrl_age = 0;
        return;
    case 0x38:
        alt_down = !released;
        alt_age = 0;
        return;
    case 0x3A:
        if (!released) caps_lock = !caps_lock;
        return;
    }

    if (released)
        return;

    /* ---- Safety: auto-release stuck modifiers ---- */
    if (shift_down) {
        shift_age++;
        if (shift_age > MOD_STUCK_LIMIT)
            shift_down = false;
    }
    if (ctrl_down) {
        ctrl_age++;
        if (ctrl_age > MOD_STUCK_LIMIT)
            ctrl_down = false;
    }
    if (alt_down) {
        alt_age++;
        if (alt_age > MOD_STUCK_LIMIT)
            alt_down = false;
    }

    /* Function keys F1-F12. */
    if (code >= 0x3B && code <= 0x44) { buf_push(KEY_F1 + (code - 0x3B)); return; }
    if (code == 0x57) { buf_push(KEY_F11); return; }
    if (code == 0x58) { buf_push(KEY_F12); return; }

    char c = shift_down ? scancode_shift[code] : scancode_normal[code];
    if (c == 0)
        return;

    /* Apply caps lock to letters. */
    if (caps_lock && c >= 'a' && c <= 'z')
        c -= 32;
    else if (caps_lock && shift_down && c >= 'A' && c <= 'Z')
        c += 32;

    /* Ctrl combos -> control characters (e.g. Ctrl-C = 3). */
    if (ctrl_down && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
        c = (char)(c & 0x1F);

    buf_push((int)(uint8_t)c);
}

static void keyboard_callback(registers_t *regs)
{
    (void)regs;
    /* Drain ALL pending bytes from the 8042 output buffer.
     *
     * A single IRQ1 can correspond to several scancodes when the host is
     * pumping the controller faster than we are reading it — exactly what
     * happens when QEMU (or a virtual PS/2 layer used by Android emulators
     * / Termux) translates a clipboard paste into scancodes: shift-make /
     * key-make / key-break / shift-break ... arrive back-to-back and only
     * the first one raises the IRQ before more bytes pile up. If we only
     * read once per IRQ we silently lose bytes and the editor "freezes" or
     * desyncs after a handful of lines, and modifiers like Shift / CapsLock
     * end up stuck because their release scancode was the one we dropped.
     *
     * Loop while the Output Buffer Full bit (bit 0 of status port 0x64) is
     * set AND the byte belongs to the keyboard (bit 5 set = mouse/aux). */
    int guard = 0;
    while ((inb(KBD_STATUS) & 0x21) == 0x01) {
        uint8_t sc = inb(KBD_DATA);
        handle_scancode(sc);
        if (++guard > 64) break;   /* safety: never spin forever in an IRQ */
    }
}

void keyboard_init(void)
{
    khead = ktail = 0;
    shift_down = ctrl_down = alt_down = caps_lock = extended = false;
    shift_age = ctrl_age = alt_age = 0;
    irq_register_handler(1, keyboard_callback);
    /* Make sure IRQ1 is unmasked in the PIC (BIOS/GRUB usually leave it
     * unmasked, but be defensive — this is what makes the keyboard work). */
    irq_clear_mask(1);
    /* drain any pending byte */
    if (inb(KBD_STATUS) & 1)
        (void)inb(KBD_DATA);
}

/* Public: force-reset all modifier state.  Call after a paste batch
 * or whenever modifiers might be stuck. */
void keyboard_reset_modifiers(void)
{
    shift_down = false;
    ctrl_down = false;
    alt_down = false;
    extended = false;
    shift_age = ctrl_age = alt_age = 0;
    /* Don't reset caps_lock — user may have intentionally toggled it. */
}

/* Full reset, INCLUDING caps_lock.  Used by callers that finish a long
 * burst of injected scancodes (e.g. the editor returning from a paste):
 * during such bursts a stray 0x3A scancode may toggle CapsLock without
 * the user pressing it, leaving the shell typing in ALL CAPS until you
 * press Caps again.  This wipes that ghost state. */
void keyboard_reset_all(void)
{
    shift_down = false;
    ctrl_down = false;
    alt_down = false;
    caps_lock = false;
    extended = false;
    shift_age = ctrl_age = alt_age = 0;
}

int keyboard_trygetchar(void)
{
    return buf_pop();
}

int keyboard_getchar(void)
{
    int key;
    __asm__ volatile("sti");
    while ((key = buf_pop()) < 0)
        __asm__ volatile("hlt");
    return key;
}

/* Public: non-blocking variant. Returns -1 if no key is queued. */
int keyboard_try_getchar(void) { return buf_pop(); }

int keyboard_readline(char *buffer, int max_len)
{
    int len = 0;
    for (;;) {
        int key = keyboard_getchar();
        if (key == '\n') {
            vga_putchar('\n');
            buffer[len] = '\0';
            return len;
        } else if (key == '\b') {
            if (len > 0) {
                len--;
                vga_putchar('\b');
            }
        } else if (key >= 32 && key < 127) {
            if (len < max_len - 1) {
                buffer[len++] = (char)key;
                vga_putchar((char)key);
            }
        }
        /* ignore special keys in basic readline */
    }
}

/* drivers/keyboard.c  -  PS/2 keyboard driver (IRQ1), scancode set 1. */
#include "keyboard.h"
#include "../cpu/irq.h"
#include "../cpu/ports.h"
#include "../drivers/vga.h"

#define KBD_DATA   0x60
#define KBD_STATUS 0x64
#define BUF_SIZE   256

/* Circular buffer of decoded key codes. */
static volatile int kbuf[BUF_SIZE];
static volatile int khead;
static volatile int ktail;

/* Modifier state. */
static bool shift_down;
static bool ctrl_down;
static bool alt_down;
static bool caps_lock;
static bool extended;   /* set after 0xE0 prefix */

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
    }
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
        if (code == 0x1D) { ctrl_down = !released; return; }
        if (code == 0x38) { alt_down  = !released; return; }
        return;
    }

    /* Modifiers. */
    switch (code) {
    case 0x2A: case 0x36: shift_down = !released; return;  /* L/R shift */
    case 0x1D: ctrl_down = !released; return;              /* ctrl */
    case 0x38: alt_down  = !released; return;              /* alt */
    case 0x3A: if (!released) caps_lock = !caps_lock; return; /* caps */
    }

    if (released)
        return;

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
    uint8_t sc = inb(KBD_DATA);
    handle_scancode(sc);
}

void keyboard_init(void)
{
    khead = ktail = 0;
    shift_down = ctrl_down = alt_down = caps_lock = extended = false;
    irq_register_handler(1, keyboard_callback);
    /* drain any pending byte */
    if (inb(KBD_STATUS) & 1)
        (void)inb(KBD_DATA);
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

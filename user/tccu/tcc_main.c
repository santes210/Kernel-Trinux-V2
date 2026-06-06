/* shell/tcc.c  -  Trinux C Compiler: compile C subset → ELF32 binary.
 *
 * Architecture: single-pass recursive-descent compiler.
 *   Source → Tokens → x86 machine code → ELF32
 *
 * All variables are 32-bit (int). Strings are stored in a data section
 * appended after the code.  Local variables live on the stack (ebp-relative).
 * Function calls use cdecl convention.
 *
 * Built-in functions are implemented as inline syscall sequences or
 * direct VGA buffer writes, emitted at the call site.
 *
 * v2 improvements:
 *   - break / continue
 *   - do-while loops
 *   - ++ / --  (prefix and postfix)
 *   - compound assignment  += -= *= /= %=
 *   - address-of  &var
 *   - new builtins: strlen, strcmp, strncmp, strcpy, getline, itoa
 *   - fixed print_num (proper itoa, handles negatives)
 *   - multiple declarations: int x, y, z;
 *   - array read/write on both sides of assignment
 */






#include "../trinux.h"

/* =========================================================================
 *  Shim userland: reemplazos de las APIs del kernel que tcc.c usa.
 *  Mantiene la misma firma para no tener que tocar el resto del codigo.
 * ========================================================================= */

/* Tipos basicos del kernel que tcc.c usa */
typedef unsigned char bool;
#define true 1
#define false 0

/* --- bump allocator userland (256 KB) ---
 * tcc solo hace alloc de cosas que no necesita liberar (text buffer,
 * ELF output). Un bump alloc es lo mas simple y suficiente. */
#define BUMP_HEAP_SIZE  (256 * 1024)
static unsigned char bump_heap[BUMP_HEAP_SIZE];
static unsigned int  bump_pos = 0;

static void *kmalloc(unsigned int n) {
    n = (n + 7) & ~7u;  /* align 8 */
    if (bump_pos + n > BUMP_HEAP_SIZE) return 0;
    void *p = &bump_heap[bump_pos];
    bump_pos += n;
    return p;
}
static void kfree(void *p) { (void)p; /* no-op con bump alloc */ }

static void bump_reset(void) { bump_pos = 0; }

/* --- vfs shim: usar readfile/writefile/unlink que ya tenemos --- */
typedef struct vfs_node_t_dummy {
    int type;
    unsigned int size;
    /* tcc solo lee .size y .type, y los pasa de vuelta a vfs_read/write.
     * Como readfile/writefile ya manejan el path directamente, los wrappers
     * que vienen abajo ignoran el "node" y leen/escriben por path. */
    const char *path;
    unsigned int perms;
} vfs_node_t;

#define VFS_FILE 1

static vfs_node_t shim_node;

static vfs_node_t *shim_get_root(void) { return 0; }
#define vfs_get_root shim_get_root

/* "resolve" devuelve un fake node si readfile dice que el path existe. */
static vfs_node_t *vfs_resolve(const char *path, vfs_node_t *cwd) {
    (void)cwd;
    trinux_stat_t st;
    if (stat(path, &st) < 0) return 0;
    if (st.type != 1) return 0;  /* solo files */
    shim_node.type = VFS_FILE;
    shim_node.size = st.size;
    shim_node.path = path;
    return &shim_node;
}

static unsigned int vfs_read(vfs_node_t *n, unsigned int off, unsigned int sz, unsigned char *buf) {
    (void)off;
    int r = readfile(n->path, buf, (int)sz);
    return r < 0 ? 0 : (unsigned int)r;
}

static vfs_node_t *vfs_create(const char *path, vfs_node_t *cwd) {
    (void)cwd;
    /* unlink primero para forzar archivo virgen (mismo fix que cp/mv) */
    unlink(path);
    shim_node.type = VFS_FILE;
    shim_node.size = 0;
    shim_node.path = path;
    return &shim_node;
}

static unsigned int vfs_write(vfs_node_t *n, unsigned int off, unsigned int sz, unsigned char *buf) {
    (void)off;
    int w = writefile(n->path, buf, (int)sz);
    return w < 0 ? 0 : (unsigned int)w;
}

/* --- kprintf ---
 * tcc usa kprintf con %s %d %u %x. Implementacion mini. */
static void k_print_hex(unsigned int v) {
    const char *h = "0123456789abcdef";
    char buf[9]; int i = 7; buf[8] = 0;
    if (v == 0) { putchar_('0'); return; }
    while (v && i >= 0) { buf[i--] = h[v & 0xF]; v >>= 4; }
    print(&buf[i+1]);
}
static void k_print_unum(unsigned int v) {
    char buf[12]; int i = 10; buf[11] = 0;
    if (v == 0) { putchar_('0'); return; }
    while (v && i >= 0) { buf[i--] = '0' + (v % 10); v /= 10; }
    print(&buf[i+1]);
}
static void k_print_num(int n) {
    if (n < 0) { putchar_('-'); k_print_unum((unsigned)(-n)); }
    else k_print_unum((unsigned)n);
}
static void kprintf_internal(const char *fmt, long *args) {
    int ai = 0;
    while (*fmt) {
        if (*fmt != '%') { putchar_(*fmt++); continue; }
        fmt++;
        /* opcional padding como %08x: ignoramos para simplicidad */
        while ((*fmt >= '0' && *fmt <= '9') || *fmt == '0') fmt++;
        switch (*fmt) {
            case 'd': k_print_num((int)args[ai++]); break;
            case 'u': k_print_unum((unsigned)args[ai++]); break;
            case 'x': k_print_hex((unsigned)args[ai++]); break;
            case 's': print((const char *)args[ai++]); break;
            case 'c': putchar_((int)args[ai++]); break;
            case '%': putchar_('%'); break;
            default:  putchar_('?'); break;
        }
        fmt++;
    }
}
/* macro varargs simple via puntero: solo funciona con max 6 args */
#define kprintf(fmt, ...) do {     long _kargs[] = { 0, ##__VA_ARGS__ };     kprintf_internal(fmt, &_kargs[1]); } while (0)

/* --- string ops que faltan en trinux.h --- */
static int strlen(const char *s) { int n=0; while(s[n])n++; return n; }
static int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
static int strncmp(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}
static char *strncpy(char *d, const char *s, int n) {
    int i;
    for (i = 0; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
static char *strcpy(char *d, const char *s) {
    char *r = d;
    while ((*d++ = *s++));
    return r;
}
static void *memcpy(void *d, const void *s, int n) {
    unsigned char *dd = (unsigned char *)d;
    const unsigned char *ss = (const unsigned char *)s;
    for (int i = 0; i < n; i++) dd[i] = ss[i];
    return d;
}
static void *memset(void *d, int c, int n) {
    unsigned char *dd = (unsigned char *)d;
    for (int i = 0; i < n; i++) dd[i] = (unsigned char)c;
    return d;
}

/* tcc.c hace referencia a SYS_EXIT del kernel (cpu/syscall.h).
 * Como trinux.h lo define igual con el mismo valor, solo aseguramos: */
#ifndef SYS_EXIT
#define SYS_EXIT 1
#endif

/* =========================================================================
 *  Fin del shim. A continuacion el codigo ORIGINAL de tcc.c sin cambios.
 * ========================================================================= */

#define MAX_CODE    32768
#define MAX_DATA    8192
#define MAX_VARS    128
#define MAX_FUNCS   32
#define MAX_STRINGS 64
#define MAX_BREAKS  64
#define BASE_ADDR   0x08048000u

/* ================================================================
 *  Lexer
 * ================================================================ */

enum token_type {
    T_EOF=0, T_NUM, T_STR, T_IDENT,
    T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE, T_LBRACK, T_RBRACK,
    T_SEMI, T_COMMA, T_ASSIGN,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT,
    T_EQ, T_NEQ, T_LT, T_GT, T_LTE, T_GTE,
    T_AND, T_OR, T_NOT, T_AMP, T_PIPE, T_CARET,
    T_IF, T_ELSE, T_WHILE, T_FOR, T_DO,
    T_RETURN, T_BREAK, T_CONTINUE,
    T_INT, T_CHAR, T_VOID,
    T_PLUS_PLUS, T_MINUS_MINUS,
    T_PLUS_EQ, T_MINUS_EQ, T_STAR_EQ, T_SLASH_EQ, T_PERCENT_EQ,
};

static const char *src;
static int         src_pos;
static int         cur_token;
static int         tok_num;
static char        tok_str[256];
static char        tok_ident[64];
static int         line_num;
/* Number of errors seen during this compilation.  Used to refuse writing
 * an ELF when the parser produced garbage (otherwise tcc would emit a
 * "valid" but corrupt binary that crashes silently on exec). */
static int         err_count;

/* Read the current source byte as UNSIGNED — `char` may be signed on
 * gcc i386, and bytes >= 0x80 (e.g. UTF-8 em-dash 0xE2 0x80 0x94 in a
 * comment) would otherwise compare as negative and bypass every
 * `c == '...'` check, corrupting the lexer state. */
static int peek_byte(int off) {
    return (int)(unsigned char)src[src_pos + off];
}

static int next_char(void) {
    int c = peek_byte(0);
    if (c) { if (c == '\n') line_num++; src_pos++; }
    return c;
}

static void skip_ws(void) {
    while (peek_byte(0)) {
        int c = peek_byte(0);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { next_char(); continue; }
        if (c == '/' && peek_byte(1) == '/') {
            while (peek_byte(0) && peek_byte(0) != '\n') src_pos++;
            continue;
        }
        if (c == '/' && peek_byte(1) == '*') {
            src_pos += 2;
            while (peek_byte(0) && !(peek_byte(0)=='*' && peek_byte(1)=='/'))
                { if (peek_byte(0)=='\n') line_num++; src_pos++; }
            if (peek_byte(0)) src_pos += 2;
            continue;
        }
        break;
    }
}

static int next_token(void) {
    skip_ws();
    char c = src[src_pos];
    if (!c) return (cur_token = T_EOF);

    /* Numbers */
    if (c >= '0' && c <= '9') {
        tok_num = 0;
        if (c == '0' && src[src_pos+1] == 'x') {
            src_pos += 2;
            while (1) {
                c = src[src_pos];
                if (c >= '0' && c <= '9') tok_num = tok_num*16+(c-'0');
                else if (c >= 'a' && c <= 'f') tok_num = tok_num*16+(c-'a'+10);
                else if (c >= 'A' && c <= 'F') tok_num = tok_num*16+(c-'A'+10);
                else break;
                src_pos++;
            }
        } else {
            while (src[src_pos] >= '0' && src[src_pos] <= '9')
                tok_num = tok_num * 10 + (src[src_pos++] - '0');
        }
        return (cur_token = T_NUM);
    }

    /* Strings */
    if (c == '"') {
        src_pos++;
        int i = 0;
        while (src[src_pos] && src[src_pos] != '"' && i < 255) {
            if (src[src_pos] == '\\') {
                src_pos++;
                switch (src[src_pos]) {
                    case 'n': tok_str[i++] = '\n'; break;
                    case 't': tok_str[i++] = '\t'; break;
                    case '\\': tok_str[i++] = '\\'; break;
                    case '"': tok_str[i++] = '"'; break;
                    case '0': tok_str[i++] = '\0'; break;
                    default: tok_str[i++] = src[src_pos]; break;
                }
            } else {
                tok_str[i++] = src[src_pos];
            }
            src_pos++;
        }
        tok_str[i] = '\0';
        if (src[src_pos] == '"') src_pos++;
        return (cur_token = T_STR);
    }

    /* Character literals */
    if (c == '\'') {
        src_pos++;
        if (src[src_pos] == '\\') { src_pos++;
            switch(src[src_pos]) {
                case 'n': tok_num='\n'; break; case 't': tok_num='\t'; break;
                case '0': tok_num=0; break; default: tok_num=src[src_pos];
            }
        } else { tok_num = src[src_pos]; }
        src_pos++;
        if (src[src_pos] == '\'') src_pos++;
        return (cur_token = T_NUM);
    }

    /* Identifiers and keywords */
    if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_') {
        int i = 0;
        while (((c=src[src_pos])>='a'&&c<='z')||(c>='A'&&c<='Z')||
               (c>='0'&&c<='9')||c=='_')
            tok_ident[i++] = src[src_pos++];
        tok_ident[i] = '\0';
        if (strcmp(tok_ident,"if")==0) return (cur_token=T_IF);
        if (strcmp(tok_ident,"else")==0) return (cur_token=T_ELSE);
        if (strcmp(tok_ident,"while")==0) return (cur_token=T_WHILE);
        if (strcmp(tok_ident,"for")==0) return (cur_token=T_FOR);
        if (strcmp(tok_ident,"do")==0) return (cur_token=T_DO);
        if (strcmp(tok_ident,"return")==0) return (cur_token=T_RETURN);
        if (strcmp(tok_ident,"break")==0) return (cur_token=T_BREAK);
        if (strcmp(tok_ident,"continue")==0) return (cur_token=T_CONTINUE);
        if (strcmp(tok_ident,"int")==0) return (cur_token=T_INT);
        if (strcmp(tok_ident,"char")==0) return (cur_token=T_CHAR);
        if (strcmp(tok_ident,"void")==0) return (cur_token=T_VOID);
        return (cur_token = T_IDENT);
    }

    /* Operators — multi-char first, then single-char */
    src_pos++;
    switch (c) {
    case '(': return (cur_token=T_LPAREN);
    case ')': return (cur_token=T_RPAREN);
    case '{': return (cur_token=T_LBRACE);
    case '}': return (cur_token=T_RBRACE);
    case '[': return (cur_token=T_LBRACK);
    case ']': return (cur_token=T_RBRACK);
    case ';': return (cur_token=T_SEMI);
    case ',': return (cur_token=T_COMMA);
    case '%': if (src[src_pos]=='='){src_pos++;return(cur_token=T_PERCENT_EQ);}
              return (cur_token=T_PERCENT);
    case '^': return (cur_token=T_CARET);
    case '~': return (cur_token=T_MINUS); /* treat ~ as bitwise NOT placeholder */
    case '!': if (src[src_pos]=='='){src_pos++;return(cur_token=T_NEQ);}
              return (cur_token=T_NOT);
    case '=': if (src[src_pos]=='='){src_pos++;return(cur_token=T_EQ);}
              return (cur_token=T_ASSIGN);
    case '<': if (src[src_pos]=='='){src_pos++;return(cur_token=T_LTE);}
              return (cur_token=T_LT);
    case '>': if (src[src_pos]=='='){src_pos++;return(cur_token=T_GTE);}
              return (cur_token=T_GT);
    case '&': if (src[src_pos]=='&'){src_pos++;return(cur_token=T_AND);}
              return (cur_token=T_AMP);
    case '|': if (src[src_pos]=='|'){src_pos++;return(cur_token=T_OR);}
              return (cur_token=T_PIPE);
    case '+': if (src[src_pos]=='+'){src_pos++;return(cur_token=T_PLUS_PLUS);}
              if (src[src_pos]=='='){src_pos++;return(cur_token=T_PLUS_EQ);}
              return (cur_token=T_PLUS);
    case '-': if (src[src_pos]=='-'){src_pos++;return(cur_token=T_MINUS_MINUS);}
              if (src[src_pos]=='='){src_pos++;return(cur_token=T_MINUS_EQ);}
              return (cur_token=T_MINUS);
    case '*': if (src[src_pos]=='='){src_pos++;return(cur_token=T_STAR_EQ);}
              return (cur_token=T_STAR);
    case '/': if (src[src_pos]=='='){src_pos++;return(cur_token=T_SLASH_EQ);}
              return (cur_token=T_SLASH);
    }
    return (cur_token = T_EOF);
}

static void expect(int tok) {
    if (cur_token != tok) {
        kprintf("tcc:%d: expected token %d, got %d\n", line_num, tok, cur_token);
        err_count++;
        err_count++;
    }
    next_token();
}

/* ================================================================
 *  Code emitter
 * ================================================================ */

static uint8_t  code[MAX_CODE];
static uint32_t cp;   /* code position */
static uint8_t  data_sec[MAX_DATA];
static uint32_t dp;   /* data position */

static void e8(uint8_t b)   { if (cp < MAX_CODE) code[cp++] = b; }
static void e32(uint32_t v) { e8(v&0xFF); e8((v>>8)&0xFF); e8((v>>16)&0xFF); e8((v>>24)&0xFF); }

static void patch32(uint32_t off, uint32_t val) {
    code[off]=val&0xFF; code[off+1]=(val>>8)&0xFF;
    code[off+2]=(val>>16)&0xFF; code[off+3]=(val>>24)&0xFF;
}

/* Store string in data section, return its offset from data_sec start */
static uint32_t store_string(const char *s) {
    uint32_t off = dp;
    while (*s && dp < MAX_DATA) data_sec[dp++] = (uint8_t)*s++;
    if (dp < MAX_DATA) data_sec[dp++] = 0;
    return off;
}

/* ================================================================
 *  Symbol table (local variables on stack)
 * ================================================================ */

/* `elem_size`: byte width when the variable is indexed with [].
 *   - 4 = declared as `int x` or `int arr[N]` (default)
 *   - 1 = declared as `char arr[N]`, or used as a parameter that is then
 *         indexed (we treat parameters as `char *` for indexing purposes).
 * `is_array`: 1 when the symbol owns the storage (`int arr[N]` / `char arr[N]`).
 *   - For locals, this means the array sits inline at  ebp+offset.
 *   - For globals, the array sits inline in .bss at bss_base + bss_off.
 *   - For a plain `int x` (or a parameter), is_array = 0 and the variable
 *     itself holds a pointer when used with `[]`. */
typedef struct {
    char name[32];
    int  offset;       /* ebp-relative */
    int  elem_size;    /* 1 or 4 */
    int  is_array;     /* 1 if storage is inline at offset, 0 if it's a pointer */
} local_t;
static local_t locals[MAX_VARS];
static int     nlocals;
static int     stack_offset;  /* next free stack slot */

/* ---- Global variables (.bss) ----
 * Declared at file scope as `int name;` or `int name[N];` (no initializer).
 * They live in a zero-initialized BSS region that the ELF loader allocates
 * right after the .data section.  We track their byte-offsets relative to
 * the BSS base and patch the absolute addresses into the code at link time,
 * exactly like we already do for string literals. */
#define MAX_GLOBALS 128
typedef struct {
    char     name[32];
    uint32_t bss_off;
    uint32_t size;
    int      elem_size;   /* 1 (char[]) or 4 (int / int[])      */
    int      is_array;    /* 1 if storage is inline at bss_off  */
} global_t;
static global_t globals[MAX_GLOBALS];
static int      nglobals;
static uint32_t bss_size;       /* total bytes reserved in .bss            */
static uint32_t bss_base_addr;  /* runtime base address of .bss            */

/* Code positions that reference a global by absolute address; patched at
 * the end of compilation once bss_base_addr is known. */
typedef struct { uint32_t code_off; uint32_t bss_off; } glob_fixup_t;
static glob_fixup_t glob_fixups[512];
static int          nglob_fixups;

static void add_glob_fixup(uint32_t code_off, uint32_t bss_off) {
    if (nglob_fixups < 512) {
        glob_fixups[nglob_fixups].code_off = code_off;
        glob_fixups[nglob_fixups].bss_off  = bss_off;
        nglob_fixups++;
    }
}

static int find_global(const char *name) {
    for (int i = 0; i < nglobals; i++)
        if (strcmp(globals[i].name, name) == 0) return i;
    return -1;
}

static int add_global(const char *name, uint32_t size,
                      int elem_size, int is_array) {
    if (nglobals >= MAX_GLOBALS) return -1;
    strncpy(globals[nglobals].name, name, 31);
    globals[nglobals].name[31]  = 0;
    globals[nglobals].bss_off   = bss_size;
    globals[nglobals].size      = size;
    globals[nglobals].elem_size = elem_size;
    globals[nglobals].is_array  = is_array;
    bss_size += size;
    /* 4-byte align next allocation */
    if (bss_size & 3) bss_size = (bss_size + 3) & ~3u;
    return nglobals++;
}

typedef struct { char name[32]; uint32_t code_offset; } func_t;
static func_t funcs[MAX_FUNCS];
static int     nfuncs;

/* Forward reference fixups for function calls */
typedef struct { uint32_t code_off; char name[32]; } call_fixup_t;
static call_fixup_t call_fixups[128];
static int          ncall_fixups;

/* String address fixups */
typedef struct { uint32_t code_off; uint32_t data_off; } str_fixup_t;
static str_fixup_t str_fixups[MAX_STRINGS];
static int         nstr_fixups;

static int find_local(const char *name) {
    for (int i = nlocals-1; i >= 0; i--)
        if (strcmp(locals[i].name, name) == 0) return locals[i].offset;
    return 0x7FFFFFFF;
}

static int find_local_idx(const char *name) {
    for (int i = nlocals-1; i >= 0; i--)
        if (strcmp(locals[i].name, name) == 0) return i;
    return -1;
}

static int add_local(const char *name) {
    stack_offset -= 4;
    if (nlocals < MAX_VARS) {
        strncpy(locals[nlocals].name, name, 31);
        locals[nlocals].offset    = stack_offset;
        locals[nlocals].elem_size = 4;
        locals[nlocals].is_array  = 0;
        nlocals++;
    }
    return stack_offset;
}

/* ================================================================
 *  Loop context (break / continue)
 * ================================================================ */

typedef struct {
    uint32_t break_fixups[MAX_BREAKS];
    int      nbreaks;
    uint32_t cont_fixups[MAX_BREAKS];
    int      ncontinues;
} loop_ctx_t;

#define MAX_LOOP_DEPTH 8
static loop_ctx_t loop_stack[MAX_LOOP_DEPTH];
static int        loop_depth;

static void loop_enter(void) {
    if (loop_depth < MAX_LOOP_DEPTH) {
        loop_stack[loop_depth].nbreaks = 0;
        loop_stack[loop_depth].ncontinues = 0;
    }
    loop_depth++;
}

static void loop_exit_patch_breaks(void) {
    if (loop_depth <= 0) return;
    loop_depth--;
    loop_ctx_t *ctx = &loop_stack[loop_depth];
    for (int i = 0; i < ctx->nbreaks; i++)
        patch32(ctx->break_fixups[i], cp - ctx->break_fixups[i] - 4);
}

static void loop_patch_continues(uint32_t target) {
    if (loop_depth <= 0) return;
    loop_ctx_t *ctx = &loop_stack[loop_depth - 1];
    for (int i = 0; i < ctx->ncontinues; i++)
        patch32(ctx->cont_fixups[i], target - ctx->cont_fixups[i] - 4);
}

static void emit_break(void) {
    if (loop_depth <= 0 || loop_depth > MAX_LOOP_DEPTH) {
        kprintf("tcc:%d: break outside loop\n", line_num); err_count++; return;
    }
    e8(0xE9);  /* jmp rel32 */
    loop_ctx_t *ctx = &loop_stack[loop_depth - 1];
    if (ctx->nbreaks < MAX_BREAKS)
        ctx->break_fixups[ctx->nbreaks++] = cp;
    e32(0);
}

static void emit_continue(void) {
    if (loop_depth <= 0 || loop_depth > MAX_LOOP_DEPTH) {
        kprintf("tcc:%d: continue outside loop\n", line_num); err_count++; return;
    }
    e8(0xE9);  /* jmp rel32 */
    loop_ctx_t *ctx = &loop_stack[loop_depth - 1];
    if (ctx->ncontinues < MAX_BREAKS)
        ctx->cont_fixups[ctx->ncontinues++] = cp;
    e32(0);
}

static uint32_t hdr_size;
static uint32_t data_base_addr;

/* ================================================================
 *  Built-in function emitter
 * ================================================================ */

/* Helper used at the end of every builtin: push a 0 onto the stack so that
 * the caller's "discard result" (add esp, 4) doesn't blow away an unrelated
 * stack slot.  Every value-producing expression in this compiler is expected
 * to leave its result on the stack, and the caller's `parse_expr; add esp,4`
 * pops that result.  Builtins that don't compute anything still need to push
 * a placeholder so the bookkeeping stays balanced — otherwise we silently
 * drift esp upward on every call and eventually `ret` to garbage. */
static void e_push0(void) {
    e8(0x6A); e8(0x00);    /* push byte 0 (sign-extended to 32 bits) */
}

static int emit_builtin(const char *name, int argc) {

    /* ---- print(str) ---- */
    if (strcmp(name, "print") == 0 && argc == 1) {
        e8(0x59);              /* pop ecx = buf */
        e8(0x51);              /* push ecx (save buf) */
        e8(0x89); e8(0xCE);   /* mov esi, ecx */
        e8(0x31); e8(0xD2);   /* xor edx, edx */
        /* .loop: */
        e8(0x80); e8(0x3E); e8(0x00);  /* cmp byte [esi], 0 */
        e8(0x74); e8(0x04);   /* je .done (skip 4: inc esi, inc edx, jmp .loop) */
        e8(0x46);             /* inc esi */
        e8(0x42);             /* inc edx */
        e8(0xEB); e8(0xF7);   /* jmp .loop (back 9 bytes) */
        /* .done: */
        e8(0x59);             /* pop ecx (buf) */
        e8(0xB8); e32(SYS_WRITE);    /* mov eax, SYS_WRITE */
        e8(0xBB); e32(1);    /* mov ebx, 1 */
        /* edx=len, ecx=buf */
        e8(0xCD); e8(0x80);  /* int 0x80 */
        e_push0();           /* dummy return value */
        return 1;
    }

    /* ---- print_num(n) — proper itoa with negative support ---- */
    if (strcmp(name, "print_num") == 0 && argc == 1) {
        e8(0x58);             /* pop eax = number */
        /* handle negative */
        e8(0x85); e8(0xC0);  /* test eax, eax */
        /* The "skip negative path" branch is computed from the actual code
         * we emit below (counting the bytes is what bites you: the previous
         * hard-coded 0x16 was the leftover from an even older version, and
         * it dropped the jump 11 bytes short — straight into the middle of
         * the next `mov edx, 1` operand. The CPU then executed garbage and
         * eventually faulted with EIP looking sane.  Patch it after the
         * fact with the real distance.) */
        uint32_t pn_jns = cp;
        e8(0x79); e8(0x00);   /* jns .positive (patched below) */
        uint32_t pn_neg_start = cp;
        /* print '-' */
        e8(0x50);             /* save eax */
        e8(0x68); e32('-');   /* push '-' as dword */
        e8(0xB8); e32(SYS_WRITE);    /* mov eax, SYS_WRITE */
        e8(0xBB); e32(1);    /* mov ebx, 1 */
        e8(0x89); e8(0xE1);  /* mov ecx, esp */
        e8(0xBA); e32(1);    /* mov edx, 1 */
        e8(0xCD); e8(0x80);  /* int 0x80 */
        e8(0x83); e8(0xC4); e8(4);  /* add esp, 4 */
        e8(0x58);             /* restore eax */
        e8(0xF7); e8(0xD8);  /* neg eax */
        /* .positive: patch the jns to skip exactly the bytes between
         * pn_neg_start and here. */
        {
            uint32_t skip = cp - pn_neg_start;
            /* short jump (8-bit signed displacement) — bail if it overflows */
            code[pn_jns + 1] = (uint8_t)skip;
        }
        /* Push digits right-to-left onto stack. All control-flow offsets
         * below are patched after-the-fact from real positions, instead of
         * the broken hard-coded short-jump distances the original code
         * shipped with. */
        e8(0x31); e8(0xC9);  /* xor ecx, ecx (digit count) */
        e8(0xBB); e32(10);   /* mov ebx, 10 */
        /* handle zero specially */
        e8(0x85); e8(0xC0);  /* test eax, eax */
        uint32_t pn_jnz_to_div = cp;
        e8(0x75); e8(0x00);  /* jnz .div_loop (patched) */

        /* number is 0: push '0' */
        e8(0x68); e32('0');  /* push '0' */
        e8(0x41);             /* inc ecx */
        uint32_t pn_jmp_to_print = cp;
        e8(0xEB); e8(0x00);  /* jmp .print (patched) */

        /* .div_loop: */
        uint32_t pn_div_loop = cp;
        code[pn_jnz_to_div + 1] = (uint8_t)(pn_div_loop - (pn_jnz_to_div + 2));
        e8(0x31); e8(0xD2);  /* xor edx, edx */
        e8(0xF7); e8(0xF3);  /* div ebx */
        e8(0x83); e8(0xC2); e8(0x30);  /* add edx, '0' */
        e8(0x52);             /* push edx */
        e8(0x41);             /* inc ecx */
        e8(0x85); e8(0xC0);  /* test eax, eax */
        e8(0x75); e8((uint8_t)(pn_div_loop - (cp + 1)));  /* jnz .div_loop (back) */

        /* .print: */
        uint32_t pn_print = cp;
        code[pn_jmp_to_print + 1] = (uint8_t)(pn_print - (pn_jmp_to_print + 2));
        e8(0x89); e8(0xCE);  /* mov esi, ecx (save count) */

        /* .print_loop: */
        uint32_t pn_print_loop = cp;
        e8(0x85); e8(0xF6);  /* test esi, esi */
        uint32_t pn_jz_to_done = cp;
        e8(0x74); e8(0x00);  /* jz .print_done (patched) */
        e8(0xB8); e32(SYS_WRITE);    /* mov eax, SYS_WRITE */
        e8(0xBB); e32(1);    /* mov ebx, 1 */
        e8(0x89); e8(0xE1);  /* mov ecx, esp */
        e8(0xBA); e32(1);    /* mov edx, 1 */
        e8(0xCD); e8(0x80);  /* int 0x80 */
        e8(0x83); e8(0xC4); e8(4);  /* add esp, 4 */
        e8(0x4E);             /* dec esi */
        e8(0xEB); e8((uint8_t)(pn_print_loop - (cp + 1)));  /* jmp .print_loop */

        /* .print_done: */
        code[pn_jz_to_done + 1] = (uint8_t)(cp - (pn_jz_to_done + 2));
        e_push0();
        return 1;
    }

    /* ---- print_char(c) ---- */
    if (strcmp(name, "print_char") == 0 && argc == 1) {
        e8(0x58);             /* pop eax */
        e8(0x50);             /* push eax */
        e8(0xB8); e32(SYS_WRITE);    /* mov eax, SYS_WRITE */
        e8(0xBB); e32(1);    /* mov ebx, 1 */
        e8(0x89); e8(0xE1);  /* mov ecx, esp */
        e8(0xBA); e32(1);    /* mov edx, 1 */
        e8(0xCD); e8(0x80);
        e8(0x83); e8(0xC4); e8(4);
        e_push0();
        return 1;
    }

    /* ---- getchar() ---- */
    if (strcmp(name, "getchar") == 0) {
        e8(0xFB);             /* sti */
        e8(0xB8); e32(SYS_GETC);    /* mov eax, SYS_GETC */
        e8(0xCD); e8(0x80);
        e8(0x50);             /* push eax */
        return 1;
    }

    /* ---- getline(buf, max) — read line with echo + backspace ---- */
    if (strcmp(name, "getline") == 0 && argc == 2) {
        /* args pushed left-to-right: [esp]=max, [esp+4]=buf */
        e8(0x5A);             /* pop edx = max */
        e8(0x5F);             /* pop edi = buf */
        e8(0x31); e8(0xC9);  /* xor ecx, ecx (count) */
        /* NOTE: int 0x80 preserves ebx/ecx/edx/esi/edi (only eax is return). */
        /* .loop: (all jumps use rel32 to avoid reach issues) */
        uint32_t gl_loop = cp;
        e8(0xFB);             /* sti */
        e8(0xB8); e32(SYS_GETC);    /* mov eax, SYS_GETC */
        e8(0xCD); e8(0x80);  /* int 0x80 → char in al */
        e8(0x3C); e8(0x0A);  /* cmp al, 0x0A (newline) */
        e8(0x0F); e8(0x84); uint32_t gl_done = cp; e32(0);  /* je .done */
        e8(0x3C); e8(0x08);  /* cmp al, 0x08 (backspace) */
        e8(0x0F); e8(0x84); uint32_t gl_bs = cp; e32(0);  /* je .bs */
        /* store char */
        e8(0x88); e8(0x04); e8(0x0F);  /* mov [edi+ecx], al */
        e8(0x41);             /* inc ecx */
        e8(0x4A);             /* dec edx */
        /* echo char */
        e8(0x0F); e8(0xB6); e8(0xF0);  /* movzx esi, al */
        e8(0x51);             /* push ecx (save count) */
        e8(0x52);             /* push edx (save max) */
        e8(0x57);             /* push edi (save buf) */
        e8(0x56);             /* push esi (char for write) */
        e8(0xB8); e32(SYS_WRITE);    /* mov eax, SYS_WRITE */
        e8(0xBB); e32(1);    /* mov ebx, 1 */
        e8(0x89); e8(0xE1);  /* mov ecx, esp */
        e8(0xBA); e32(1);    /* mov edx, 1 */
        e8(0xCD); e8(0x80);  /* int 0x80 */
        e8(0x83); e8(0xC4); e8(4);  /* add esp, 4 (pop char) */
        e8(0x5F);             /* pop edi */
        e8(0x5A);             /* pop edx */
        e8(0x59);             /* pop ecx */
        /* check if max reached */
        e8(0x85); e8(0xD2);  /* test edx, edx */
        e8(0x0F); e8(0x84); e32(gl_done - cp - 4);  /* je .done */
        e8(0xE9); e32(gl_loop - cp - 4);  /* jmp .loop */
        /* .bs: */
        patch32(gl_bs, cp - gl_bs - 4);
        e8(0x85); e8(0xC9);  /* test ecx, ecx */
        e8(0x0F); e8(0x84); e32(gl_loop - cp - 4);  /* jz .loop */
        e8(0x49);             /* dec ecx */
        e8(0x42);             /* inc edx */
        /* echo "\b \b" */
        e8(0x51); e8(0x52); e8(0x57);  /* push ecx,edx,edi */
        /* backspace */
        e8(0x68); e32(0x08);  /* push 8 */
        e8(0xB8); e32(SYS_WRITE); e8(0xBB); e32(1);
        e8(0x89); e8(0xE1); e8(0xBA); e32(1);
        e8(0xCD); e8(0x80);
        /* space */
        e8(0xC7); e8(0x04); e8(0x24); e32(0x20);
        e8(0xB8); e32(SYS_WRITE); e8(0xBB); e32(1);
        e8(0x89); e8(0xE1); e8(0xBA); e32(1);
        e8(0xCD); e8(0x80);
        /* backspace again */
        e8(0xC7); e8(0x04); e8(0x24); e32(0x08);
        e8(0xB8); e32(SYS_WRITE); e8(0xBB); e32(1);
        e8(0x89); e8(0xE1); e8(0xBA); e32(1);
        e8(0xCD); e8(0x80);
        e8(0x83); e8(0xC4); e8(4);  /* pop char */
        e8(0x5F); e8(0x5A); e8(0x59);  /* pop edi,edx,ecx */
        e8(0xE9); e32(gl_loop - cp - 4);  /* jmp .loop */
        /* .done: */
        patch32(gl_done, cp - gl_done - 4);
        /* echo newline */
        e8(0x68); e32(0x0A);  /* push '\n' */
        e8(0xB8); e32(SYS_WRITE); e8(0xBB); e32(1);
        e8(0x89); e8(0xE1); e8(0xBA); e32(1);
        e8(0xCD); e8(0x80);
        e8(0x83); e8(0xC4); e8(4);
        /* null terminate and return count */
        e8(0xC6); e8(0x04); e8(0x0F); e8(0x00);  /* mov byte [edi+ecx], 0 */
        e8(0x51);             /* push ecx (return count) */
        return 1;
    }

    /* ---- sleep(ms) ---- */
    if (strcmp(name, "sleep") == 0 && argc == 1) {
        e8(0x5B);             /* pop ebx = ms */
        e8(0xB8); e32(SYS_SLEEP);    /* mov eax, SYS_SLEEP */
        e8(0xCD); e8(0x80);
        e_push0();
        return 1;
    }

    /* ---- uptime() ---- */
    if (strcmp(name, "uptime") == 0) {
        e8(0xB8); e32(SYS_UPTIME);    /* mov eax, SYS_UPTIME */
        e8(0xCD); e8(0x80);
        e8(0x50);             /* push result */
        return 1;
    }

    /* ---- getpid() ---- */
    if (strcmp(name, "getpid") == 0) {
        e8(0xB8); e32(3);
        e8(0xCD); e8(0x80);
        e8(0x50);
        return 1;
    }

    /* ---- exit(code) ---- */
    if (strcmp(name, "exit") == 0 && argc == 1) {
        e8(0x58);             /* pop eax */
        e8(0x89); e8(0xEC);  /* mov esp, ebp */
        e8(0x5D);             /* pop ebp */
        e8(0xC3);             /* ret */
        return 1;
    }

    /* ---- strlen(str) ---- */
    if (strcmp(name, "strlen") == 0 && argc == 1) {
        e8(0x5E);             /* pop esi = str */
        e8(0x31); e8(0xC0);  /* xor eax, eax (count) */
        /* .loop: */
        e8(0x80); e8(0x3C); e8(0x06); e8(0x00);  /* cmp byte [esi+eax], 0 */
        e8(0x74); e8(0x03);   /* je .done (skip 3) */
        e8(0x40);             /* inc eax */
        e8(0xEB); e8(0xF7);   /* jmp .loop (back 9) */
        /* .done: */
        e8(0x50);             /* push eax */
        return 1;
    }

    /* ---- strcmp(a, b) ---- */
    if (strcmp(name, "strcmp") == 0 && argc == 2) {
        e8(0x5E);             /* pop esi = b */
        e8(0x5F);             /* pop edi = a */
        /* .loop: */
        e8(0x8A); e8(0x07);  /* mov al, [edi] */
        e8(0x3A); e8(0x06);  /* cmp al, [esi] */
        e8(0x75); e8(0x0A);  /* jne .diff (skip 10) */
        e8(0x84); e8(0xC0);  /* test al, al */
        e8(0x74); e8(0x0E);  /* jz .equal (skip 14) */
        e8(0x47);             /* inc edi */
        e8(0x46);             /* inc esi */
        e8(0xEB); e8(0xF0);   /* jmp .loop (back 16) */
        /* .diff: */
        e8(0x0F); e8(0xB6); e8(0xC0);  /* movzx eax, al */
        e8(0x0F); e8(0xB6); e8(0x0E);  /* movzx ecx, byte [esi] */
        e8(0x29); e8(0xC8);  /* sub eax, ecx */
        e8(0xEB); e8(0x02);  /* jmp .done (skip 2) */
        /* .equal: */
        e8(0x31); e8(0xC0);  /* xor eax, eax */
        /* .done: */
        e8(0x50);             /* push eax */
        return 1;
    }

    /* ---- strncmp(a, b, n) ---- */
    if (strcmp(name, "strncmp") == 0 && argc == 3) {
        e8(0x59);             /* pop ecx = n */
        e8(0x5E);             /* pop esi = b */
        e8(0x5F);             /* pop edi = a */
        /* .loop: */
        e8(0xE3); e8(0x12);  /* jecxz .equal (skip 18) */
        e8(0x8A); e8(0x07);  /* mov al, [edi] */
        e8(0x3A); e8(0x06);  /* cmp al, [esi] */
        e8(0x75); e8(0x0C);  /* jne .diff (skip 12) */
        e8(0x84); e8(0xC0);  /* test al, al */
        e8(0x74); e8(0x10);  /* jz .equal (skip 16) */
        e8(0x47);             /* inc edi */
        e8(0x46);             /* inc esi */
        e8(0x49);             /* dec ecx */
        e8(0xEB); e8(0xEA);   /* jmp .loop (back 22) */
        /* .diff: */
        e8(0x0F); e8(0xB6); e8(0xC0);  /* movzx eax, al */
        e8(0x0F); e8(0xB6); e8(0x0E);  /* movzx ecx, byte [esi] */
        e8(0x29); e8(0xC8);  /* sub eax, ecx */
        e8(0xEB); e8(0x02);  /* jmp .done */
        /* .equal: */
        e8(0x31); e8(0xC0);  /* xor eax, eax */
        /* .done: */
        e8(0x50);
        return 1;
    }

    /* ---- strcpy(dst, src) ---- */
    if (strcmp(name, "strcpy") == 0 && argc == 2) {
        e8(0x5E);             /* pop esi = src */
        e8(0x5F);             /* pop edi = dst (return value) */
        e8(0x89); e8(0xF9);  /* mov ecx, edi (save dst) */
        /* .loop: */
        e8(0x8A); e8(0x06);  /* mov al, [esi] */
        e8(0x88); e8(0x07);  /* mov [edi], al */
        e8(0x46);             /* inc esi */
        e8(0x47);             /* inc edi */
        e8(0x84); e8(0xC0);  /* test al, al */
        e8(0x75); e8(0xF4);  /* jnz .loop (back 12) */
        e8(0x51);             /* push ecx (dst) */
        return 1;
    }

    /* ---- itoa(n, buf) — convert int to string, return buf ---- */
    if (strcmp(name, "itoa") == 0 && argc == 2) {
        e8(0x5F);             /* pop edi = buf */
        e8(0x58);             /* pop eax = n */
        e8(0x89); e8(0xFB);  /* mov ebx, edi (save buf start) */
        /* handle negative */
        e8(0x85); e8(0xC0);  /* test eax, eax */
        e8(0x79); e8(0x06);  /* jns .positive (skip 6) */
        e8(0xC6); e8(0x07); e8('-');  /* mov byte [edi], '-' */
        e8(0x47);             /* inc edi */
        e8(0xF7); e8(0xD8);  /* neg eax */
        /* .positive: */
        e8(0x31); e8(0xC9);  /* xor ecx, ecx (digit count) */
        e8(0xBE); e32(10);   /* mov esi, 10 */
        /* .div_loop: */
        e8(0x31); e8(0xD2);  /* xor edx, edx */
        e8(0xF7); e8(0xF6);  /* div esi */
        e8(0x83); e8(0xC2); e8(0x30);  /* add edx, '0' */
        e8(0x52);             /* push edx */
        e8(0x41);             /* inc ecx */
        e8(0x85); e8(0xC0);  /* test eax, eax */
        e8(0x75); e8(0xF3);  /* jnz .div_loop */
        /* pop digits into buf */
        /* .pop_loop: */
        e8(0x85); e8(0xC9);  /* test ecx, ecx */
        e8(0x74); e8(0x06);  /* jz .done (skip 6) */
        e8(0x58);             /* pop eax */
        e8(0x88); e8(0x07);  /* mov [edi], al */
        e8(0x47);             /* inc edi */
        e8(0x49);             /* dec ecx */
        e8(0xEB); e8(0xF4);  /* jmp .pop_loop */
        /* .done: */
        e8(0xC6); e8(0x07); e8(0x00);  /* mov byte [edi], 0 */
        e8(0x53);             /* push ebx (buf start) */
        return 1;
    }

    /* ---- vga_clear(color) ---- */
    if (strcmp(name, "vga_clear") == 0 && argc == 1) {
        e8(0x58);             /* pop eax = color */
        e8(0x88); e8(0xC4);  /* mov ah, al */
        e8(0xB0); e8(0x20);  /* mov al, ' ' */
        e8(0xB9); e32(0xB8000);
        e8(0xBA); e32(2000);
        /* .loop: */
        e8(0x66); e8(0x89); e8(0x01);  /* mov [ecx], ax */
        e8(0x83); e8(0xC1); e8(2);
        e8(0x4A);
        e8(0x75); e8(0xF7);  /* jnz .loop (back 9) */
        e_push0();
        return 1;
    }

    /* ---- vga_putchar(x, y, ch, color) ---- */
    if (strcmp(name, "vga_putchar") == 0 && argc == 4) {
        e8(0x58);  /* pop eax = color */
        e8(0x5B);  /* pop ebx = ch */
        e8(0x59);  /* pop ecx = y */
        e8(0x5A);  /* pop edx = x */
        e8(0x6B); e8(0xC9); e8(80);  /* imul ecx, 80 */
        e8(0x01); e8(0xD1);          /* add ecx, edx */
        e8(0xD1); e8(0xE1);          /* shl ecx, 1 */
        e8(0x88); e8(0x99); e32(0xB8000);  /* mov [0xB8000+ecx], bl */
        e8(0x88); e8(0x81); e32(0xB8001);  /* mov [0xB8001+ecx], al */
        e_push0();
        return 1;
    }

    /* ---- read_file(path, buf, max) -> bytes (or -1 on error) ---- */
    if (strcmp(name, "read_file") == 0 && argc == 3) {
        e8(0x5A);                            /* pop edx = max     */
        e8(0x59);                            /* pop ecx = buf     */
        e8(0x5B);                            /* pop ebx = path    */
        e8(0xB8); e32(SYS_READFILE);         /* mov eax, 8        */
        e8(0xCD); e8(0x80);                  /* int 0x80          */
        e8(0x50);                            /* push eax (result) */
        return 1;
    }

    /* ---- write_file(path, buf, len) -> bytes (or -1 on error) ---- */
    if (strcmp(name, "write_file") == 0 && argc == 3) {
        e8(0x5A);                            /* pop edx = len     */
        e8(0x59);                            /* pop ecx = buf     */
        e8(0x5B);                            /* pop ebx = path    */
        e8(0xB8); e32(SYS_WRITEFILE);        /* mov eax, 9        */
        e8(0xCD); e8(0x80);                  /* int 0x80          */
        e8(0x50);
        return 1;
    }

    /* ---- read_line(buf, max) -> length ---- */
    if (strcmp(name, "read_line") == 0 && argc == 2) {
        e8(0x59);                            /* pop ecx = max     */
        e8(0x5B);                            /* pop ebx = buf     */
        e8(0xB8); e32(SYS_GETLINE);          /* mov eax, 10       */
        e8(0xCD); e8(0x80);
        e8(0x50);
        return 1;
    }

    /* ---- vga_print(x, y, str, color) ---- */
    if (strcmp(name, "vga_print") == 0 && argc == 4) {
        e8(0x58);  /* pop eax = color */
        e8(0x5E);  /* pop esi = str */
        e8(0x59);  /* pop ecx = y */
        e8(0x5A);  /* pop edx = x */
        e8(0x6B); e8(0xC9); e8(80);
        e8(0x01); e8(0xD1);
        e8(0xD1); e8(0xE1);
        e8(0x81); e8(0xC1); e32(0xB8000);
        /* .loop: */
        e8(0x8A); e8(0x1E);  /* mov bl, [esi] */
        e8(0x84); e8(0xDB);  /* test bl, bl */
        e8(0x74); e8(0x0B);  /* jz .done (skip 11) */
        e8(0x88); e8(0x19);  /* mov [ecx], bl */
        e8(0x88); e8(0x41); e8(0x01);  /* mov [ecx+1], al */
        e8(0x46);             /* inc esi */
        e8(0x83); e8(0xC1); e8(2);
        e8(0xEB); e8(0xEF);  /* jmp .loop (back 17) */
        /* .done: */
        e_push0();
        return 1;
    }

    return 0;
}

/* ================================================================
 *  Parser + Code Generator (recursive descent)
 * ================================================================ */

static void parse_expr(void);
static void parse_statement(void);

/* Expression parser: result is pushed on stack */

static void parse_primary(void) {
    if (cur_token == T_NUM) {
        e8(0x68); e32((uint32_t)tok_num);
        next_token();
    } else if (cur_token == T_STR) {
        uint32_t soff = store_string(tok_str);
        e8(0x68);
        if (nstr_fixups < MAX_STRINGS) {
            str_fixups[nstr_fixups].code_off = cp;
            str_fixups[nstr_fixups].data_off = soff;
            nstr_fixups++;
        }
        e32(0);
        next_token();
    } else if (cur_token == T_IDENT) {
        char name[64];
        strncpy(name, tok_ident, 63);
        next_token();

        if (cur_token == T_LPAREN) {
            /* Function call */
            next_token();
            int argc = 0;
            if (cur_token != T_RPAREN) {
                parse_expr(); argc++;
                while (cur_token == T_COMMA) {
                    next_token();
                    parse_expr(); argc++;
                }
            }
            expect(T_RPAREN);
            if (!emit_builtin(name, argc)) {
                e8(0xE8);  /* call rel32 */
                if (ncall_fixups < 128) {
                    call_fixups[ncall_fixups].code_off = cp;
                    strncpy(call_fixups[ncall_fixups].name, name, 31);
                    ncall_fixups++;
                }
                e32(0);
                if (argc > 0) {
                    e8(0x83); e8(0xC4); e8((uint8_t)(argc * 4));
                }
                e8(0x50);  /* push eax */
            }
        } else if (cur_token == T_ASSIGN) {
            /* Assignment: name = expr */
            next_token();
            parse_expr();
            int off = find_local(name);
            int gi  = (off == 0x7FFFFFFF) ? find_global(name) : -1;
            if (off == 0x7FFFFFFF && gi < 0) off = add_local(name);
            e8(0x58);  /* pop eax */
            if (gi >= 0) {
                /* mov [abs32], eax  → store to global */
                e8(0xA3); add_glob_fixup(cp, globals[gi].bss_off); e32(0);
            } else {
                e8(0x89); e8(0x85); e32((uint32_t)off);
            }
            e8(0x50);  /* push eax (assignment returns the value) */
        } else if (cur_token == T_PLUS_EQ || cur_token == T_MINUS_EQ ||
                   cur_token == T_STAR_EQ || cur_token == T_SLASH_EQ ||
                   cur_token == T_PERCENT_EQ) {
            /* Compound assignment: name op= expr */
            int op = cur_token;
            next_token();
            int off = find_local(name);
            int gi  = (off == 0x7FFFFFFF) ? find_global(name) : -1;
            if (off == 0x7FFFFFFF && gi < 0) {
                kprintf("tcc:%d: undefined variable '%s'\n", line_num, name);
                err_count++;
                parse_expr();
                e8(0x83); e8(0xC4); e8(4);
                e8(0x68); e32(0);
                return;
            }
            /* load current value */
            if (gi >= 0) {
                e8(0xA1); add_glob_fixup(cp, globals[gi].bss_off); e32(0); /* mov eax, [abs] */
            } else {
                e8(0x8B); e8(0x85); e32((uint32_t)off);
            }
            e8(0x50);  /* push eax */
            parse_expr();
            /* apply operator */
            e8(0x5B);  /* pop ebx = new expr */
            e8(0x58);  /* pop eax = old value */
            if (op == T_PLUS_EQ)    { e8(0x01); e8(0xD8); }
            if (op == T_MINUS_EQ)   { e8(0x29); e8(0xD8); }
            if (op == T_STAR_EQ)    { e8(0x0F); e8(0xAF); e8(0xC3); }
            if (op == T_SLASH_EQ)   { e8(0x99); e8(0xF7); e8(0xFB); }
            if (op == T_PERCENT_EQ) { e8(0x99); e8(0xF7); e8(0xFB); e8(0x89); e8(0xD0); }
            /* store and push result */
            if (gi >= 0) {
                e8(0xA3); add_glob_fixup(cp, globals[gi].bss_off); e32(0);
            } else {
                e8(0x89); e8(0x85); e32((uint32_t)off);
            }
            e8(0x50);
        } else if (cur_token == T_PLUS_PLUS || cur_token == T_MINUS_MINUS) {
            /* Postfix ++ / -- */
            int op = cur_token;
            next_token();
            int off = find_local(name);
            int gi  = (off == 0x7FFFFFFF) ? find_global(name) : -1;
            if (off == 0x7FFFFFFF && gi < 0) {
                kprintf("tcc:%d: undefined variable '%s'\n", line_num, name);
                err_count++;
                e8(0x68); e32(0);
                return;
            }
            /* push old value */
            if (gi >= 0) {
                e8(0xA1); add_glob_fixup(cp, globals[gi].bss_off); e32(0);
            } else {
                e8(0x8B); e8(0x85); e32((uint32_t)off);
            }
            e8(0x50);  /* push eax */
            /* mutate in memory */
            if (gi >= 0) {
                /* inc/dec dword [abs32] */
                if (op == T_PLUS_PLUS) { e8(0xFF); e8(0x05); }
                else                   { e8(0xFF); e8(0x0D); }
                add_glob_fixup(cp, globals[gi].bss_off); e32(0);
            } else {
                if (op == T_PLUS_PLUS) { e8(0xFF); e8(0x85); e32((uint32_t)off); }
                else                   { e8(0xFF); e8(0x8D); e32((uint32_t)off); }
            }
        } else {
            /* Variable read (might be array indexing) */
            int off = find_local(name);
            int gi  = (off == 0x7FFFFFFF) ? find_global(name) : -1;
            if (off == 0x7FFFFFFF && gi < 0) {
                kprintf("tcc:%d: undefined variable '%s'\n", line_num, name);
                err_count++;
                e8(0x68); e32(0);
                return;
            }
            if (cur_token == T_LBRACK) {
                /* Array indexing: name[expr]  — both read and write.
                 *
                 * Determine element size:
                 *   - global array  -> globals[gi].elem_size  (1 or 4)
                 *   - local  array  -> locals[li].elem_size
                 *   - bare int      -> treat as pointer to int (size 4)
                 *   - char* / param -> treat as pointer to char (size 1) */
                int li = (gi >= 0) ? -1 : find_local_idx(name);
                int elem_size;
                int is_inline;     /* 1 = storage is inline (array), 0 = pointer */
                if (gi >= 0) {
                    elem_size = globals[gi].elem_size;
                    is_inline = globals[gi].is_array;
                } else if (li >= 0) {
                    elem_size = locals[li].elem_size;
                    is_inline = locals[li].is_array;
                } else {
                    elem_size = 4;
                    is_inline = 0;
                }

                next_token();
                parse_expr();
                expect(T_RBRACK);
                /* Index is on the stack.  Build address in edx, then either
                 * load (read) or pop a value and store (write). */
                e8(0x58);              /* pop eax = index */
                if (elem_size == 4) {
                    e8(0xC1); e8(0xE0); e8(2);  /* shl eax, 2 */
                }
                /* eax = byte offset within the array */

                if (is_inline) {
                    /* The symbol owns the storage. edx = base address. */
                    if (gi >= 0) {
                        e8(0xBA); add_glob_fixup(cp, globals[gi].bss_off); e32(0);
                    } else {
                        e8(0x89); e8(0xEA);                              /* mov edx, ebp */
                        e8(0x81); e8(0xC2); e32((uint32_t)off);          /* add edx, off */
                    }
                } else {
                    /* The symbol HOLDS a pointer. Load it into edx. */
                    if (gi >= 0) {
                        e8(0x8B); e8(0x15); add_glob_fixup(cp, globals[gi].bss_off); e32(0);
                    } else {
                        e8(0x8B); e8(0x95); e32((uint32_t)off);          /* mov edx, [ebp+off] */
                    }
                }
                e8(0x01); e8(0xC2);    /* add edx, eax  ->  &arr[i] */

                if (cur_token == T_ASSIGN) {
                    /* Array store: arr[i] = expr */
                    e8(0x52);          /* push edx */
                    next_token();
                    parse_expr();
                    e8(0x58);          /* pop eax = value */
                    e8(0x5A);          /* pop edx = &arr[i] */
                    if (elem_size == 1) {
                        e8(0x88); e8(0x02);     /* mov [edx], al  (byte) */
                    } else {
                        e8(0x89); e8(0x02);     /* mov [edx], eax */
                    }
                    e8(0x50);          /* push eax (assignment returns value) */
                } else {
                    /* Array load */
                    if (elem_size == 1) {
                        e8(0x31); e8(0xC0);             /* xor eax, eax (zero-extend) */
                        e8(0x8A); e8(0x02);             /* mov al, [edx] */
                    } else {
                        e8(0x8B); e8(0x02);             /* mov eax, [edx] */
                    }
                    e8(0x50);                            /* push eax */
                }
            } else {
                /* Simple variable read */
                if (gi >= 0) {
                    e8(0xA1); add_glob_fixup(cp, globals[gi].bss_off); e32(0);
                } else {
                    e8(0x8B); e8(0x85); e32((uint32_t)off);
                }
                e8(0x50);
            }
        }
    } else if (cur_token == T_LPAREN) {
        next_token();
        parse_expr();
        expect(T_RPAREN);
    } else {
        kprintf("tcc:%d: unexpected token %d\n", line_num, cur_token);
        err_count++;
        next_token();
        e8(0x68); e32(0);
    }
}

static void parse_unary(void) {
    if (cur_token == T_MINUS) {
        next_token();
        parse_unary();  /* recurse for --x etc. */
        e8(0x58);
        e8(0xF7); e8(0xD8);  /* neg eax */
        e8(0x50);
    } else if (cur_token == T_NOT) {
        next_token();
        parse_unary();
        e8(0x58);
        e8(0x85); e8(0xC0);
        e8(0x0F); e8(0x94); e8(0xC0);  /* setz al */
        e8(0x0F); e8(0xB6); e8(0xC0);  /* movzx eax, al */
        e8(0x50);
    } else if (cur_token == T_AMP) {
        /* Address-of: &var */
        next_token();
        if (cur_token == T_IDENT) {
            char name[64];
            strncpy(name, tok_ident, 63);
            next_token();
            int off = find_local(name);
            int gi  = (off == 0x7FFFFFFF) ? find_global(name) : -1;
            if (off == 0x7FFFFFFF && gi < 0) {
                kprintf("tcc:%d: undefined variable '%s'\n", line_num, name);
                err_count++;
                e8(0x68); e32(0);
                return;
            }
            if (gi >= 0) {
                /* mov eax, imm32 (bss_base + bss_off) */
                e8(0xB8); add_glob_fixup(cp, globals[gi].bss_off); e32(0);
            } else {
                /* lea eax, [ebp+off]  → address of local */
                e8(0x8D); e8(0x85); e32((uint32_t)off);
            }
            e8(0x50);
        } else {
            kprintf("tcc:%d: & requires a variable name\n", line_num);
            err_count++;
            parse_primary();
        }
    } else if (cur_token == T_STAR) {
        /* Dereference: *expr */
        next_token();
        parse_unary();
        e8(0x58);  /* pop eax = address */
        e8(0x8B); e8(0x00);  /* mov eax, [eax] */
        e8(0x50);
    } else if (cur_token == T_PLUS_PLUS) {
        /* Prefix ++ */
        next_token();
        if (cur_token == T_IDENT) {
            char name[64];
            strncpy(name, tok_ident, 63);
            next_token();
            int off = find_local(name);
            if (off == 0x7FFFFFFF) {
                kprintf("tcc:%d: undefined variable '%s'\n", line_num, name);
                err_count++;
                e8(0x68); e32(0);
                return;
            }
            e8(0xFF); e8(0x85); e32((uint32_t)off);  /* inc dword [ebp+off] */
            e8(0x8B); e8(0x85); e32((uint32_t)off);  /* mov eax, [ebp+off] */
            e8(0x50);
        } else {
            parse_primary();
        }
    } else if (cur_token == T_MINUS_MINUS) {
        /* Prefix -- */
        next_token();
        if (cur_token == T_IDENT) {
            char name[64];
            strncpy(name, tok_ident, 63);
            next_token();
            int off = find_local(name);
            if (off == 0x7FFFFFFF) {
                kprintf("tcc:%d: undefined variable '%s'\n", line_num, name);
                err_count++;
                e8(0x68); e32(0);
                return;
            }
            e8(0xFF); e8(0x8D); e32((uint32_t)off);  /* dec dword [ebp+off] */
            e8(0x8B); e8(0x85); e32((uint32_t)off);  /* mov eax, [ebp+off] */
            e8(0x50);
        } else {
            parse_primary();
        }
    } else {
        parse_primary();
    }
}

static void parse_mul(void) {
    parse_unary();
    while (cur_token == T_STAR || cur_token == T_SLASH || cur_token == T_PERCENT) {
        int op = cur_token;
        next_token();
        parse_unary();
        e8(0x5B);  /* pop ebx (right) */
        e8(0x58);  /* pop eax (left) */
        if (op == T_STAR) {
            e8(0x0F); e8(0xAF); e8(0xC3);  /* imul eax, ebx */
        } else {
            e8(0x99);  /* cdq */
            e8(0xF7); e8(0xFB);  /* idiv ebx */
            if (op == T_PERCENT) { e8(0x89); e8(0xD0); } /* mov eax, edx */
        }
        e8(0x50);
    }
}

static void parse_add(void) {
    parse_mul();
    while (cur_token == T_PLUS || cur_token == T_MINUS) {
        int op = cur_token;
        next_token();
        parse_mul();
        e8(0x5B); e8(0x58);
        if (op == T_PLUS) { e8(0x01); e8(0xD8); }
        else               { e8(0x29); e8(0xD8); }
        e8(0x50);
    }
}

static void parse_compare(void) {
    parse_add();
    while (cur_token==T_EQ||cur_token==T_NEQ||cur_token==T_LT||
           cur_token==T_GT||cur_token==T_LTE||cur_token==T_GTE) {
        int op = cur_token;
        next_token();
        parse_add();
        e8(0x5B); e8(0x58);
        e8(0x39); e8(0xD8);  /* cmp eax, ebx */
        uint8_t setcc;
        switch (op) {
            case T_EQ:  setcc = 0x94; break;
            case T_NEQ: setcc = 0x95; break;
            case T_LT:  setcc = 0x9C; break;
            case T_GT:  setcc = 0x9F; break;
            case T_LTE: setcc = 0x9E; break;
            case T_GTE: setcc = 0x9D; break;
            default:    setcc = 0x94; break;
        }
        e8(0x0F); e8(setcc); e8(0xC0);
        e8(0x0F); e8(0xB6); e8(0xC0);
        e8(0x50);
    }
}

static void parse_expr(void) {
    parse_compare();
    while (cur_token == T_AND || cur_token == T_OR) {
        int op = cur_token;
        next_token();
        parse_compare();
        e8(0x5B); e8(0x58);
        if (op == T_AND) {
            e8(0x85); e8(0xC0);
            e8(0x0F); e8(0x95); e8(0xC0);
            e8(0x85); e8(0xDB);
            e8(0x0F); e8(0x95); e8(0xC3);
            e8(0x20); e8(0xD8);
        } else {
            e8(0x09); e8(0xD8);
            e8(0x85); e8(0xC0);
            e8(0x0F); e8(0x95); e8(0xC0);
        }
        e8(0x0F); e8(0xB6); e8(0xC0);
        e8(0x50);
    }
}

/* Statement parser */
static void parse_statement(void) {
    if (cur_token == T_LBRACE) {
        next_token();
        while (cur_token != T_RBRACE && cur_token != T_EOF)
            parse_statement();
        expect(T_RBRACE);

    } else if (cur_token == T_BREAK) {
        next_token();
        emit_break();
        if (cur_token == T_SEMI) next_token();

    } else if (cur_token == T_CONTINUE) {
        next_token();
        emit_continue();
        if (cur_token == T_SEMI) next_token();

    } else if (cur_token == T_IF) {
        next_token();
        expect(T_LPAREN);
        parse_expr();
        expect(T_RPAREN);
        e8(0x58);
        e8(0x85); e8(0xC0);
        e8(0x0F); e8(0x84); uint32_t jz_pos = cp; e32(0);
        parse_statement();
        if (cur_token == T_ELSE) {
            next_token();
            e8(0xE9); uint32_t jmp_pos = cp; e32(0);
            patch32(jz_pos, cp - jz_pos - 4);
            parse_statement();
            patch32(jmp_pos, cp - jmp_pos - 4);
        } else {
            patch32(jz_pos, cp - jz_pos - 4);
        }

    } else if (cur_token == T_WHILE) {
        next_token();
        loop_enter();
        uint32_t loop_top = cp;
        expect(T_LPAREN);
        parse_expr();
        expect(T_RPAREN);
        e8(0x58);
        e8(0x85); e8(0xC0);
        e8(0x0F); e8(0x84); uint32_t jz_pos = cp; e32(0);
        parse_statement();
        /* continue jumps back to condition */
        loop_patch_continues(loop_top);
        e8(0xE9); e32(loop_top - cp - 4);
        patch32(jz_pos, cp - jz_pos - 4);
        loop_exit_patch_breaks();

    } else if (cur_token == T_DO) {
        next_token();
        loop_enter();
        uint32_t loop_top = cp;
        parse_statement();
        expect(T_WHILE);
        expect(T_LPAREN);
        /* Save condition position for continue jumps */
        uint32_t cond_pos = cp;
        parse_expr();
        expect(T_RPAREN);
        /* continue jumps to re-evaluate condition */
        loop_patch_continues(cond_pos);
        e8(0x58);  /* pop eax */
        e8(0x85); e8(0xC0);  /* test eax, eax */
        e8(0x0F); e8(0x85); e32(loop_top - cp - 4);  /* jnz loop_top */
        if (cur_token == T_SEMI) next_token();
        loop_exit_patch_breaks();

    } else if (cur_token == T_FOR) {
        next_token();
        expect(T_LPAREN);
        loop_enter();
        /* init */
        if (cur_token != T_SEMI) { parse_expr(); e8(0x83); e8(0xC4); e8(4); }
        expect(T_SEMI);
        uint32_t loop_top = cp;
        /* condition */
        if (cur_token != T_SEMI) parse_expr();
        else { e8(0x68); e32(1); }
        /* Save position for increment re-parse */
        int save_incr_src  = src_pos;
        int save_incr_line = line_num;
        expect(T_SEMI);
        /* skip increment expr */
        int depth = 0;
        while (src[src_pos] && !(src[src_pos]==')' && depth==0)) {
            if (src[src_pos]=='(') depth++;
            if (src[src_pos]==')') depth--;
            src_pos++;
        }
        if (src[src_pos]==')') src_pos++;
        next_token();
        /* condition check */
        e8(0x58);
        e8(0x85); e8(0xC0);
        e8(0x0F); e8(0x84); uint32_t jz_pos = cp; e32(0);
        /* body */
        parse_statement();
        /* increment: go back and parse it */
        uint32_t incr_land = cp;  /* continue jumps here */
        int body_end_pos = src_pos;
        int body_end_line = line_num;
        int body_end_tok = cur_token;
        src_pos = save_incr_src;
        line_num = save_incr_line;
        next_token();
        if (cur_token != T_RPAREN) {
            parse_expr();
            e8(0x83); e8(0xC4); e8(4);
        }
        src_pos = body_end_pos;
        line_num = body_end_line;
        cur_token = body_end_tok;
        /* patch continues to increment code */
        loop_patch_continues(incr_land);
        /* jump back to condition */
        e8(0xE9); e32(loop_top - cp - 4);
        patch32(jz_pos, cp - jz_pos - 4);
        loop_exit_patch_breaks();

    } else if (cur_token == T_RETURN) {
        next_token();
        if (cur_token != T_SEMI && cur_token != T_RBRACE) {
            parse_expr();
            e8(0x58);  /* pop eax */
        }
        e8(0x89); e8(0xEC);
        e8(0x5D);
        e8(0xC3);
        if (cur_token == T_SEMI) next_token();

    } else if (cur_token == T_INT || cur_token == T_CHAR) {
        /* Variable declaration(s): int x; int x = expr; int x, y, z;
         *                          char buf[N];  (byte array on stack)
         *                          int  arr[N];  (4-byte array on stack) */
        int decl_elem = (cur_token == T_CHAR) ? 1 : 4;
        next_token();

        for (;;) {
            char vname[64];
            strncpy(vname, tok_ident, 63);
            next_token();  /* consume identifier */

            if (cur_token == T_LBRACK) {
                /* Array: T arr[N]; */
                next_token();
                int size = tok_num;
                next_token();
                expect(T_RBRACK);
                int bytes = size * decl_elem;
                /* round up to 4-byte stack alignment */
                if (bytes & 3) bytes = (bytes + 3) & ~3;
                int off = find_local(vname);
                if (off == 0x7FFFFFFF) {
                    stack_offset -= bytes;
                    off = stack_offset;
                    if (nlocals < MAX_VARS) {
                        strncpy(locals[nlocals].name, vname, 31);
                        locals[nlocals].offset    = off;
                        locals[nlocals].elem_size = decl_elem;
                        locals[nlocals].is_array  = 1;
                        nlocals++;
                    }
                }
                e8(0x83); e8(0xEC); e8((uint8_t)bytes);
            } else if (cur_token == T_ASSIGN) {
                next_token();
                parse_expr();
                int off = add_local(vname);
                e8(0x58);
                e8(0x89); e8(0x85); e32((uint32_t)off);
                e8(0x83); e8(0xEC); e8(4);
            } else {
                add_local(vname);
                e8(0x83); e8(0xEC); e8(4);
            }

            if (cur_token == T_COMMA) {
                next_token();
                continue;
            }
            break;
        }
        if (cur_token == T_SEMI) next_token();

    } else {
        /* Expression statement */
        parse_expr();
        e8(0x83); e8(0xC4); e8(4);  /* discard result */
        if (cur_token == T_SEMI) next_token();
    }
}

/* Parse a function definition */
static void parse_function(void) {
    char fname[64];
    strncpy(fname, tok_ident, 63);
    next_token();

    funcs[nfuncs].code_offset = cp;
    strncpy(funcs[nfuncs].name, fname, 31);
    nfuncs++;

    expect(T_LPAREN);
    int param_count = 0;
    nlocals = 0;
    stack_offset = 0;
    while (cur_token != T_RPAREN && cur_token != T_EOF) {
        if (cur_token == T_INT || cur_token == T_CHAR || cur_token == T_VOID)
            next_token();
        strncpy(locals[nlocals].name, tok_ident, 31);
        locals[nlocals].offset = 8 + param_count * 4;
        nlocals++;
        param_count++;
        next_token();
        if (cur_token == T_COMMA) next_token();
    }
    expect(T_RPAREN);

    e8(0x55);              /* push ebp */
    e8(0x89); e8(0xE5);   /* mov ebp, esp */

    expect(T_LBRACE);
    while (cur_token != T_RBRACE && cur_token != T_EOF)
        parse_statement();
    expect(T_RBRACE);

    /* implicit return 0 */
    e8(0x31); e8(0xC0);
    e8(0x89); e8(0xEC);
    e8(0x5D);
    e8(0xC3);
}

/* ================================================================
 *  Main compiler entry
 * ================================================================ */

int tcc_compile_internal(const char *src_path, const char *out_path)
{
    

    vfs_node_t *sf = vfs_resolve(src_path, 0);
    if (!sf || sf->type != VFS_FILE) {
        kprintf("tcc: %s: not found\n", src_path);
        return -1;
    }

    char *text = (char *)kmalloc(sf->size + 1);
    if (!text) { kprintf("tcc: out of memory\n"); return -1; }
    uint32_t len = vfs_read(sf, 0, sf->size, (uint8_t *)text);
    text[len] = '\0';

    /* Init */
    src = text;
    src_pos = 0;
    line_num = 1;
    err_count = 0;
    cp = 0; dp = 0;
    nlocals = 0; stack_offset = 0;
    nfuncs = 0; ncall_fixups = 0; nstr_fixups = 0;
    nglobals = 0; bss_size = 0; nglob_fixups = 0;
    loop_depth = 0;
    memset(code, 0, sizeof(code));
    memset(data_sec, 0, sizeof(data_sec));

    hdr_size = 52 + 32;

    uint32_t call_main_pos;
    e8(0xE8); call_main_pos = cp; e32(0);  /* call main                       */
    /* After main() returns, do exit(eax) instead of `ret`.  The user stack
     * doesn't have a sensible return address on top, so plain `ret` jumps
     * into garbage and faults.  Calling SYS_EXIT terminates the process
     * cleanly with main's return value as the exit code. */
    e8(0x89); e8(0xC3);                /* mov ebx, eax  (exit code = main's ret) */
    e8(0xB8); e32(SYS_EXIT);           /* mov eax, 1                          */
    e8(0xCD); e8(0x80);                /* int 0x80    -> never returns        */
    e8(0xC3);                          /* ret (unreachable, kept as a safety) */

    next_token();
    while (cur_token != T_EOF) {
        if (cur_token == T_INT || cur_token == T_CHAR || cur_token == T_VOID) {
            int type_tok = cur_token;     /* remember T_INT / T_CHAR / T_VOID */
            next_token();
            if (cur_token != T_IDENT) {
                kprintf("tcc:%d: expected identifier after type\n", line_num);
                err_count++;
                next_token();
                continue;
            }
            /* Snapshot identifier, peek the next token: '(' = function,
             * anything else = global variable / array. */
            char gname[64];
            strncpy(gname, tok_ident, 63);
            gname[63] = 0;

            /* Save lexer position to be able to "rewind" if it turns out
             * this is a function definition (we need to leave tok_ident
             * on the function name for parse_function()). */
            int save_pos    = src_pos;
            int save_line   = line_num;
            int save_tok    = cur_token;
            int save_num    = tok_num;
            char save_id[64];
            strncpy(save_id, tok_ident, 63); save_id[63] = 0;

            next_token();   /* peek past the identifier */
            if (cur_token == T_LPAREN) {
                /* It's a function — rewind so parse_function sees the name. */
                src_pos   = save_pos;
                line_num  = save_line;
                cur_token = save_tok;
                tok_num   = save_num;
                strncpy(tok_ident, save_id, 63);
                parse_function();
            } else {
                /* Global variable / array.  Syntax we accept:
                 *     int  x ;
                 *     int  x , y , z ;
                 *     int  arr [ NUM ] ;            (no initializer)
                 *     char buf [ NUM ] ;            (byte array)
                 * No initializer is supported — the BSS region starts
                 * zero-filled, so assign values inside main() if needed.
                 *
                 * `type_tok` is the original type keyword; char[] = byte
                 * array, int[] (and the default) = 4-byte array. */
                int gelem = (type_tok == T_CHAR) ? 1 : 4;
                for (;;) {
                    if (cur_token == T_LBRACK) {
                        next_token();
                        int n = tok_num;
                        next_token();
                        expect(T_RBRACK);
                        if (n <= 0) n = 1;
                        if (find_global(gname) < 0)
                            add_global(gname, (uint32_t)n * (uint32_t)gelem,
                                       gelem, 1);
                    } else {
                        if (find_global(gname) < 0)
                            add_global(gname, 4, 4, 0);
                    }
                    if (cur_token == T_COMMA) {
                        next_token();
                        if (cur_token != T_IDENT) {
                            kprintf("tcc:%d: expected identifier\n", line_num);
                            err_count++;
                            break;
                        }
                        strncpy(gname, tok_ident, 63); gname[63] = 0;
                        next_token();
                        continue;
                    }
                    break;
                }
                if (cur_token == T_SEMI) next_token();
            }
        } else {
            kprintf("tcc:%d: expected function or global declaration\n", line_num);
            err_count++;
            next_token();
        }
    }

    kfree(text);

    /* Refuse to emit garbage: if the parser saw any error, the generated
     * code stream is almost certainly malformed (jumps to nowhere, bad
     * stack discipline...).  Writing the ELF anyway gave the user a
     * "successful" file that crashed silently on exec.  Bail out cleanly
     * so they can fix the source first. */
    if (err_count > 0) {
        kprintf("tcc: %d error(s); aborting (no output written).\n", err_count);
        return -1;
    }

    /* Fix call to main */
    int main_idx = -1;
    for (int i = 0; i < nfuncs; i++)
        if (strcmp(funcs[i].name, "main") == 0) { main_idx = i; break; }
    if (main_idx < 0) {
        kprintf("tcc: error: no main() function found\n");
        err_count++;
        return -1;
    }
    patch32(call_main_pos, funcs[main_idx].code_offset - call_main_pos - 4);

    /* Fix user function call fixups */
    for (int i = 0; i < ncall_fixups; i++) {
        int fi = -1;
        for (int j = 0; j < nfuncs; j++)
            if (strcmp(funcs[j].name, call_fixups[i].name) == 0) { fi = j; break; }
        if (fi < 0) {
            kprintf("tcc: undefined function '%s'\n", call_fixups[i].name);
            err_count++;
            continue;
        }
        patch32(call_fixups[i].code_off,
                funcs[fi].code_offset - call_fixups[i].code_off - 4);
    }

    /* Compute data section address and fix string references */
    data_base_addr = BASE_ADDR + hdr_size + cp;
    for (int i = 0; i < nstr_fixups; i++)
        patch32(str_fixups[i].code_off, data_base_addr + str_fixups[i].data_off);

    /* Place BSS right after .data in the runtime address space.  It is NOT
     * stored in the file — the ELF loader zero-fills the gap between
     * filesz and memsz, which is exactly what we want. */
    bss_base_addr = data_base_addr + dp;
    for (int i = 0; i < nglob_fixups; i++)
        patch32(glob_fixups[i].code_off, bss_base_addr + glob_fixups[i].bss_off);

    /* Build ELF */
    uint32_t total = hdr_size + cp + dp;       /* on-disk size */
    uint32_t memsz = total + bss_size;         /* in-memory size (incl. BSS) */
    uint8_t *elf = (uint8_t *)kmalloc(total + 16);
    if (!elf) { kprintf("tcc: out of memory for ELF\n"); return -1; }
    memset(elf, 0, total);

    elf[0]=0x7F; elf[1]='E'; elf[2]='L'; elf[3]='F';
    elf[4]=1; elf[5]=1; elf[6]=1;
    *(uint16_t*)(elf+16) = 2;
    *(uint16_t*)(elf+18) = 3;
    *(uint32_t*)(elf+20) = 1;
    *(uint32_t*)(elf+24) = BASE_ADDR + hdr_size;
    *(uint32_t*)(elf+28) = 52;
    *(uint16_t*)(elf+40) = 52;
    *(uint16_t*)(elf+42) = 32;
    *(uint16_t*)(elf+44) = 1;

    uint8_t *ph = elf + 52;
    *(uint32_t*)(ph+0)  = 1;
    *(uint32_t*)(ph+8)  = BASE_ADDR;
    *(uint32_t*)(ph+12) = BASE_ADDR;
    *(uint32_t*)(ph+16) = total;          /* p_filesz: bytes in the file */
    *(uint32_t*)(ph+20) = memsz;          /* p_memsz : bytes at runtime (BSS gets zero-filled) */
    *(uint32_t*)(ph+24) = 7;
    *(uint32_t*)(ph+28) = 0x1000;

    memcpy(elf + hdr_size, code, cp);
    memcpy(elf + hdr_size + cp, data_sec, dp);

    vfs_node_t *out = vfs_create(out_path, 0);
    if (!out) { kfree(elf); kprintf("tcc: cannot create '%s'\n", out_path); return -1; }
    out->size = 0;
    /* skip chmod in ring 3 wrapper */
    vfs_write(out, 0, total, elf);
    kfree(elf);

    kprintf("tcc: compiled %u bytes code + %u bytes data + %u bytes bss -> %s (%u file, %u mem)\n",
            cp, dp, bss_size, out_path, total, memsz);
    return 0;
}

/* =========================================================================
 *  main() userland: parsea argv y llama a tcc_compile
 * ========================================================================= */
int main(int argc, char **argv) {
    if (argc < 2) {
        print("usage: tcc <source.c> [output]\n\n");
        print("  Compiles a C program into an ELF32 ring-3 executable.\n");
        print("  If no output is given, removes .c extension from source.\n\n");
        print("  Built-in functions available in your C source:\n");
        print("    print(str)           print_num(n)\n");
        print("    print_char(c)        getchar()\n");
        print("    sleep(ms)            uptime()      getpid()\n");
        print("    exit(code)           vga_clear(color)\n");
        print("    vga_putchar(x,y,c,color)\n");
        print("    vga_print(x,y,str,color)\n");
        print("    strlen, strcmp, strncmp, strcpy, getline, itoa\n");
        print("\nThis is /bin/tcc — runs in RING 3 (CPL=3).\n");
        return 1;
    }

    char out_buf[128];
    const char *out_path;
    if (argc >= 3) {
        out_path = argv[2];
    } else {
        /* derivar: srcpath sin .c */
        int i = 0;
        while (argv[1][i] && i < 127) { out_buf[i] = argv[1][i]; i++; }
        out_buf[i] = 0;
        if (i > 2 && out_buf[i-2] == '.' && out_buf[i-1] == 'c')
            out_buf[i-2] = 0;
        out_path = out_buf;
    }

    int rc = tcc_compile_internal(argv[1], out_path);
    if (rc == 0) {
        print("tcc[ring3]: success\n");
    }
    return rc;
}

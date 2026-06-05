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
#include "tcc.h"
#include "shell.h"
#include "../fs/vfs.h"
#include "../mm/kheap.h"
#include "../lib/string.h"
#include "../lib/printf.h"

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

static int next_char(void) {
    char c = src[src_pos];
    if (c) { if (c == '\n') line_num++; src_pos++; }
    return c;
}

static void skip_ws(void) {
    while (src[src_pos]) {
        char c = src[src_pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { next_char(); continue; }
        if (c == '/' && src[src_pos+1] == '/') {
            while (src[src_pos] && src[src_pos] != '\n') src_pos++;
            continue;
        }
        if (c == '/' && src[src_pos+1] == '*') {
            src_pos += 2;
            while (src[src_pos] && !(src[src_pos]=='*' && src[src_pos+1]=='/'))
                { if (src[src_pos]=='\n') line_num++; src_pos++; }
            if (src[src_pos]) src_pos += 2;
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

typedef struct { char name[32]; int offset; /* ebp-relative */ } local_t;
static local_t locals[MAX_VARS];
static int     nlocals;
static int     stack_offset;  /* next free stack slot */

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

static int add_local(const char *name) {
    stack_offset -= 4;
    if (nlocals < MAX_VARS) {
        strncpy(locals[nlocals].name, name, 31);
        locals[nlocals].offset = stack_offset;
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
        kprintf("tcc:%d: break outside loop\n", line_num); return;
    }
    e8(0xE9);  /* jmp rel32 */
    loop_ctx_t *ctx = &loop_stack[loop_depth - 1];
    if (ctx->nbreaks < MAX_BREAKS)
        ctx->break_fixups[ctx->nbreaks++] = cp;
    e32(0);
}

static void emit_continue(void) {
    if (loop_depth <= 0 || loop_depth > MAX_LOOP_DEPTH) {
        kprintf("tcc:%d: continue outside loop\n", line_num); return;
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
        e8(0xB8); e32(1);    /* mov eax, 1 (SYS_WRITE) */
        e8(0xBB); e32(1);    /* mov ebx, 1 */
        /* edx=len, ecx=buf */
        e8(0xCD); e8(0x80);  /* int 0x80 */
        return 1;
    }

    /* ---- print_num(n) — proper itoa with negative support ---- */
    if (strcmp(name, "print_num") == 0 && argc == 1) {
        e8(0x58);             /* pop eax = number */
        /* handle negative */
        e8(0x85); e8(0xC0);  /* test eax, eax */
        e8(0x79); e8(0x16);  /* jns .positive (skip 22 bytes) */
        /* print '-' */
        e8(0x50);             /* save eax */
        e8(0x68); e32('-');   /* push '-' as dword */
        e8(0xB8); e32(1);    /* mov eax, SYS_WRITE */
        e8(0xBB); e32(1);    /* mov ebx, 1 */
        e8(0x89); e8(0xE1);  /* mov ecx, esp */
        e8(0xBA); e32(1);    /* mov edx, 1 */
        e8(0xCD); e8(0x80);  /* int 0x80 */
        e8(0x83); e8(0xC4); e8(4);  /* add esp, 4 */
        e8(0x58);             /* restore eax */
        e8(0xF7); e8(0xD8);  /* neg eax */
        /* .positive: */
        /* Push digits right-to-left onto stack */
        e8(0x31); e8(0xC9);  /* xor ecx, ecx (digit count) */
        e8(0xBB); e32(10);   /* mov ebx, 10 */
        /* handle zero specially */
        e8(0x85); e8(0xC0);  /* test eax, eax */
        e8(0x75); e8(0x0B);  /* jnz .div_loop */
        /* number is 0: push '0' */
        e8(0x68); e32('0');  /* push '0' */
        e8(0x41);             /* inc ecx */
        e8(0xEB); e8(0x12);  /* jmp .print */
        /* .div_loop: */
        e8(0x31); e8(0xD2);  /* xor edx, edx */
        e8(0xF7); e8(0xF3);  /* div ebx */
        e8(0x83); e8(0xC2); e8(0x30);  /* add edx, '0' */
        e8(0x52);             /* push edx */
        e8(0x41);             /* inc ecx */
        e8(0x85); e8(0xC0);  /* test eax, eax */
        e8(0x75); e8(0xF3);  /* jnz .div_loop */
        /* .print: ecx=digit count, digits on stack */
        e8(0x89); e8(0xCE);  /* mov esi, ecx (save count) */
        /* .print_loop: */
        e8(0x85); e8(0xF6);  /* test esi, esi */
        e8(0x74); e8(0x19);  /* jz .print_done (skip 25) */
        e8(0xB8); e32(1);    /* mov eax, SYS_WRITE */
        e8(0xBB); e32(1);    /* mov ebx, 1 */
        e8(0x89); e8(0xE1);  /* mov ecx, esp */
        e8(0xBA); e32(1);    /* mov edx, 1 */
        e8(0xCD); e8(0x80);  /* int 0x80 */
        e8(0x83); e8(0xC4); e8(4);  /* add esp, 4 */
        e8(0x4E);             /* dec esi */
        e8(0xEB); e8(0xE3);  /* jmp .print_loop (back 29) */
        /* .print_done: */
        return 1;
    }

    /* ---- print_char(c) ---- */
    if (strcmp(name, "print_char") == 0 && argc == 1) {
        e8(0x58);             /* pop eax */
        e8(0x50);             /* push eax */
        e8(0xB8); e32(1);    /* mov eax, SYS_WRITE */
        e8(0xBB); e32(1);    /* mov ebx, 1 */
        e8(0x89); e8(0xE1);  /* mov ecx, esp */
        e8(0xBA); e32(1);    /* mov edx, 1 */
        e8(0xCD); e8(0x80);
        e8(0x83); e8(0xC4); e8(4);
        return 1;
    }

    /* ---- getchar() ---- */
    if (strcmp(name, "getchar") == 0) {
        e8(0xFB);             /* sti */
        e8(0xB8); e32(5);    /* mov eax, SYS_GETC */
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
        e8(0xB8); e32(5);    /* mov eax, SYS_GETC */
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
        e8(0xB8); e32(1);    /* mov eax, SYS_WRITE */
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
        e8(0xB8); e32(1); e8(0xBB); e32(1);
        e8(0x89); e8(0xE1); e8(0xBA); e32(1);
        e8(0xCD); e8(0x80);
        /* space */
        e8(0xC7); e8(0x04); e8(0x24); e32(0x20);
        e8(0xB8); e32(1); e8(0xBB); e32(1);
        e8(0x89); e8(0xE1); e8(0xBA); e32(1);
        e8(0xCD); e8(0x80);
        /* backspace again */
        e8(0xC7); e8(0x04); e8(0x24); e32(0x08);
        e8(0xB8); e32(1); e8(0xBB); e32(1);
        e8(0x89); e8(0xE1); e8(0xBA); e32(1);
        e8(0xCD); e8(0x80);
        e8(0x83); e8(0xC4); e8(4);  /* pop char */
        e8(0x5F); e8(0x5A); e8(0x59);  /* pop edi,edx,ecx */
        e8(0xE9); e32(gl_loop - cp - 4);  /* jmp .loop */
        /* .done: */
        patch32(gl_done, cp - gl_done - 4);
        /* echo newline */
        e8(0x68); e32(0x0A);  /* push '\n' */
        e8(0xB8); e32(1); e8(0xBB); e32(1);
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
        e8(0xB8); e32(4);    /* mov eax, SYS_SLEEP */
        e8(0xCD); e8(0x80);
        return 1;
    }

    /* ---- uptime() ---- */
    if (strcmp(name, "uptime") == 0) {
        e8(0xB8); e32(7);    /* mov eax, SYS_UPTIME */
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
            if (off == 0x7FFFFFFF) off = add_local(name);
            e8(0x58);  /* pop eax */
            e8(0x89); e8(0x85); e32((uint32_t)off);
            e8(0x50);  /* push eax */
        } else if (cur_token == T_PLUS_EQ || cur_token == T_MINUS_EQ ||
                   cur_token == T_STAR_EQ || cur_token == T_SLASH_EQ ||
                   cur_token == T_PERCENT_EQ) {
            /* Compound assignment: name op= expr */
            int op = cur_token;
            next_token();
            int off = find_local(name);
            if (off == 0x7FFFFFFF) {
                kprintf("tcc:%d: undefined variable '%s'\n", line_num, name);
                parse_expr();
                e8(0x83); e8(0xC4); e8(4);
                e8(0x68); e32(0);
                return;
            }
            /* load current value */
            e8(0x8B); e8(0x85); e32((uint32_t)off);  /* mov eax, [ebp+off] */
            e8(0x50);  /* push eax */
            parse_expr();
            /* apply operator */
            e8(0x5B);  /* pop ebx = new expr */
            e8(0x58);  /* pop eax = old value */
            if (op == T_PLUS_EQ)    { e8(0x01); e8(0xD8); }  /* add eax, ebx */
            if (op == T_MINUS_EQ)   { e8(0x29); e8(0xD8); }  /* sub eax, ebx */
            if (op == T_STAR_EQ)    { e8(0x0F); e8(0xAF); e8(0xC3); }  /* imul eax, ebx */
            if (op == T_SLASH_EQ)   { e8(0x99); e8(0xF7); e8(0xFB); }  /* cdq; idiv ebx */
            if (op == T_PERCENT_EQ) { e8(0x99); e8(0xF7); e8(0xFB); e8(0x89); e8(0xD0); }
            /* store and push result */
            e8(0x89); e8(0x85); e32((uint32_t)off);  /* mov [ebp+off], eax */
            e8(0x50);  /* push eax */
        } else if (cur_token == T_PLUS_PLUS || cur_token == T_MINUS_MINUS) {
            /* Postfix ++ / -- */
            int op = cur_token;
            next_token();
            int off = find_local(name);
            if (off == 0x7FFFFFFF) {
                kprintf("tcc:%d: undefined variable '%s'\n", line_num, name);
                e8(0x68); e32(0);
                return;
            }
            /* push old value */
            e8(0x8B); e8(0x85); e32((uint32_t)off);  /* mov eax, [ebp+off] */
            e8(0x50);  /* push eax (old value for expression result) */
            /* increment/decrement in memory */
            if (op == T_PLUS_PLUS) {
                e8(0xFF); e8(0x85); e32((uint32_t)off);  /* inc dword [ebp+off] */
            } else {
                e8(0xFF); e8(0x8D); e32((uint32_t)off);  /* dec dword [ebp+off] */
            }
        } else {
            /* Variable read (might be array indexing) */
            int off = find_local(name);
            if (off == 0x7FFFFFFF) {
                kprintf("tcc:%d: undefined variable '%s'\n", line_num, name);
                e8(0x68); e32(0);
                return;
            }
            if (cur_token == T_LBRACK) {
                /* Array indexing: name[expr] */
                next_token();
                parse_expr();
                expect(T_RBRACK);
                /* eax = index (on stack). Compute addr = ebp + off + index*4 */
                e8(0x58);  /* pop eax = index */
                e8(0xC1); e8(0xE0); e8(2);  /* shl eax, 2 */
                e8(0x89); e8(0xEA);  /* mov edx, ebp */
                e8(0x81); e8(0xC2); e32((uint32_t)off);  /* add edx, off (=arr base) */
                e8(0x01); e8(0xC2);  /* add edx, eax */
                e8(0x8B); e8(0x02);  /* mov eax, [edx] */
                e8(0x50);  /* push eax */
            } else {
                /* Simple variable read */
                e8(0x8B); e8(0x85); e32((uint32_t)off);
                e8(0x50);
            }
        }
    } else if (cur_token == T_LPAREN) {
        next_token();
        parse_expr();
        expect(T_RPAREN);
    } else {
        kprintf("tcc:%d: unexpected token %d\n", line_num, cur_token);
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
            if (off == 0x7FFFFFFF) {
                kprintf("tcc:%d: undefined variable '%s'\n", line_num, name);
                e8(0x68); e32(0);
                return;
            }
            /* push ebp + offset */
            e8(0x8D); e8(0x85); e32((uint32_t)off);  /* lea eax, [ebp+off] */
            e8(0x50);
        } else {
            kprintf("tcc:%d: & requires a variable name\n", line_num);
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
        /* Variable declaration(s): int x; int x = expr; int x, y, z; */
        int is_char = (cur_token == T_CHAR);
        (void)is_char;
        next_token();

        for (;;) {
            char vname[64];
            strncpy(vname, tok_ident, 63);
            next_token();  /* consume identifier */

            if (cur_token == T_LBRACK) {
                /* Array: int arr[N]; */
                next_token();
                int size = tok_num;
                next_token();
                expect(T_RBRACK);
                int off = find_local(vname);
                if (off == 0x7FFFFFFF) {
                    stack_offset -= size * 4;
                    off = stack_offset;
                    if (nlocals < MAX_VARS) {
                        strncpy(locals[nlocals].name, vname, 31);
                        locals[nlocals].offset = off;
                        nlocals++;
                    }
                }
                e8(0x83); e8(0xEC); e8((uint8_t)(size * 4));
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

int tcc_compile(const char *src_path, const char *out_path)
{
    shell_state_t *s = shell_get_state();

    vfs_node_t *sf = vfs_resolve(src_path, s->cwd);
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
    cp = 0; dp = 0;
    nlocals = 0; stack_offset = 0;
    nfuncs = 0; ncall_fixups = 0; nstr_fixups = 0;
    loop_depth = 0;
    memset(code, 0, sizeof(code));
    memset(data_sec, 0, sizeof(data_sec));

    hdr_size = 52 + 32;

    uint32_t call_main_pos;
    e8(0xE8); call_main_pos = cp; e32(0);  /* call main */
    e8(0xC3);  /* ret */

    next_token();
    while (cur_token != T_EOF) {
        if (cur_token == T_INT || cur_token == T_CHAR || cur_token == T_VOID) {
            next_token();
            if (cur_token == T_IDENT) {
                parse_function();
            }
        } else {
            kprintf("tcc:%d: expected function definition\n", line_num);
            next_token();
        }
    }

    kfree(text);

    /* Fix call to main */
    int main_idx = -1;
    for (int i = 0; i < nfuncs; i++)
        if (strcmp(funcs[i].name, "main") == 0) { main_idx = i; break; }
    if (main_idx < 0) {
        kprintf("tcc: error: no main() function found\n");
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
            continue;
        }
        patch32(call_fixups[i].code_off,
                funcs[fi].code_offset - call_fixups[i].code_off - 4);
    }

    /* Compute data section address and fix string references */
    data_base_addr = BASE_ADDR + hdr_size + cp;
    for (int i = 0; i < nstr_fixups; i++)
        patch32(str_fixups[i].code_off, data_base_addr + str_fixups[i].data_off);

    /* Build ELF */
    uint32_t total = hdr_size + cp + dp;
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
    *(uint32_t*)(ph+16) = total;
    *(uint32_t*)(ph+20) = total;
    *(uint32_t*)(ph+24) = 7;
    *(uint32_t*)(ph+28) = 0x1000;

    memcpy(elf + hdr_size, code, cp);
    memcpy(elf + hdr_size + cp, data_sec, dp);

    vfs_node_t *out = vfs_create(out_path, s->cwd);
    if (!out) { kfree(elf); kprintf("tcc: cannot create '%s'\n", out_path); return -1; }
    out->size = 0;
    out->permissions = 0755;
    vfs_write(out, 0, total, elf);
    kfree(elf);

    kprintf("tcc: compiled %u bytes code + %u bytes data -> %s (%u bytes)\n",
            cp, dp, out_path, total);
    return 0;
}

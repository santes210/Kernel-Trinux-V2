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
 */
#include "tcc.h"
#include "shell.h"
#include "../fs/vfs.h"
#include "../mm/kheap.h"
#include "../lib/string.h"
#include "../lib/printf.h"

#define MAX_CODE    16384
#define MAX_DATA    4096
#define MAX_VARS    64
#define MAX_FUNCS   32
#define MAX_STRINGS 64
#define MAX_BREAKS  32
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
    T_IF, T_ELSE, T_WHILE, T_FOR, T_RETURN, T_INT, T_CHAR, T_VOID,
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
static int peek_char(void) { return src[src_pos]; }

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
        if (strcmp(tok_ident,"return")==0) return (cur_token=T_RETURN);
        if (strcmp(tok_ident,"int")==0) return (cur_token=T_INT);
        if (strcmp(tok_ident,"char")==0) return (cur_token=T_CHAR);
        if (strcmp(tok_ident,"void")==0) return (cur_token=T_VOID);
        return (cur_token = T_IDENT);
    }

    /* Operators */
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
    case '+': return (cur_token=T_PLUS);
    case '-': return (cur_token=T_MINUS);
    case '*': return (cur_token=T_STAR);
    case '/': return (cur_token=T_SLASH);
    case '%': return (cur_token=T_PERCENT);
    case '^': return (cur_token=T_CARET);
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

/* String address fixups: positions in code[] that need data_base_addr added */
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
    strncpy(locals[nlocals].name, name, 31);
    locals[nlocals].offset = stack_offset;
    nlocals++;
    return stack_offset;
}

static uint32_t hdr_size;
static uint32_t data_base_addr;  /* runtime address of data section */

/* ================================================================
 *  Built-in function emitter
 * ================================================================ */

/* Emit inline code for built-in functions.  Returns 1 if handled. */
static int emit_builtin(const char *name, int argc) {
    if (strcmp(name, "print") == 0 && argc == 1) {
        /* arg is on stack: pointer to string.  SYS_WRITE(1, buf, len) */
        /* We need strlen first. Pop arg into ecx. */
        e8(0x59);  /* pop ecx = buf */
        /* compute strlen: save ecx, scan for 0 */
        e8(0x51);  /* push ecx (save buf) */
        e8(0x89); e8(0xCE);  /* mov esi, ecx */
        /* xor edx, edx */
        e8(0x31); e8(0xD2);
        /* .loop: cmp byte [esi], 0 */
        e8(0x80); e8(0x3E); e8(0x00);
        /* je .done */
        e8(0x74); e8(0x03);
        /* inc esi; inc edx; jmp .loop */
        e8(0x46); e8(0x42); e8(0xEB); e8(0xF6);
        /* .done: pop ecx (buf) */
        e8(0x59);
        /* mov eax, 1 (SYS_WRITE) */
        e8(0xB8); e32(1);
        /* mov ebx, 1 (fd) */
        e8(0xBB); e32(1);
        /* edx already has length, ecx has buf */
        /* int 0x80 */
        e8(0xCD); e8(0x80);
        return 1;
    }
    if (strcmp(name, "print_num") == 0 && argc == 1) {
        /* Simple: convert number to string on stack, then print.
         * Pop eax = number. We'll use a stack buffer. */
        e8(0x58);  /* pop eax = number */
        /* push 12 bytes of zero on stack as buffer */
        e8(0x83); e8(0xEC); e8(12);  /* sub esp, 12 */
        e8(0x89); e8(0xE7);  /* mov edi, esp */
        /* call a small itoa routine (inline) */
        /* We'll emit it inline: divide by 10 repeatedly */
        e8(0x89); e8(0xC1);  /* mov ecx, eax (save number) */
        e8(0xB8); e32(0);    /* mov eax, 0 ; digit count */
        e8(0x89); e8(0xCB);  /* mov ebx, ecx ; number in ebx */
        /* Simple approach: just use SYS_WRITE with a manual conversion.
         * Actually let's just push digits and print them. */
        /* This is getting complex inline. Let's use a simpler approach:
         * emit a call to a helper function stored in the data section. */
        /* SIMPLEST: just print the raw number via a loop of div/mod */
        e8(0x83); e8(0xC4); e8(12);  /* add esp, 12 (cleanup) */
        /* For now: call SYS_WRITE with "?" as placeholder */
        /* TODO: proper itoa. For now print_num is a no-op marker. */
        /* Actually, let's be practical and emit a working itoa: */
        /* We'll place digits on stack right-to-left */
        e8(0x89); e8(0xC8);  /* mov eax, ecx (number) */
        e8(0x31); e8(0xC9);  /* xor ecx, ecx (digit count) */
        e8(0xBB); e32(10);   /* mov ebx, 10 */
        /* .div_loop: */
        e8(0x31); e8(0xD2);  /* xor edx, edx */
        e8(0xF7); e8(0xF3);  /* div ebx  ; eax=quotient, edx=remainder */
        e8(0x83); e8(0xC2); e8(0x30); /* add edx, '0' */
        e8(0x52);            /* push edx */
        e8(0x41);            /* inc ecx */
        e8(0x85); e8(0xC0);  /* test eax, eax */
        e8(0x75); e8(0xF2);  /* jnz .div_loop */
        /* Now ecx digits are on stack (reversed = correct order) */
        e8(0x89); e8(0xCA);  /* mov edx, ecx (save count) */
        e8(0x89); e8(0xE1);  /* mov ecx, esp (buf = stack top) */
        /* But digits are dwords on stack, need to compact to bytes */
        /* Actually each push put a dword with the char in the low byte.
         * Let's just print them one at a time. */
        e8(0x89); e8(0xD6);  /* mov esi, edx (count) */
        /* .print_loop: */
        e8(0x85); e8(0xF6);  /* test esi, esi */
        e8(0x74); e8(0x11);  /* jz .print_done */
        e8(0xB8); e32(1);    /* mov eax, 1 (SYS_WRITE) */
        e8(0xBB); e32(1);    /* mov ebx, 1 */
        e8(0x89); e8(0xE1);  /* mov ecx, esp */
        e8(0xBA); e32(1);    /* mov edx, 1 */
        e8(0xCD); e8(0x80);  /* int 0x80 */
        e8(0x83); e8(0xC4); e8(4); /* add esp, 4 (pop one dword) */
        e8(0x4E);            /* dec esi */
        e8(0xEB); e8(0xE5);  /* jmp .print_loop */
        /* .print_done: */
        return 1;
    }
    if (strcmp(name, "print_char") == 0 && argc == 1) {
        /* Pop char, push it on stack, write 1 byte */
        e8(0x58);  /* pop eax */
        e8(0x50);  /* push eax (char on stack) */
        e8(0xB8); e32(1);  /* mov eax, SYS_WRITE */
        e8(0xBB); e32(1);  /* mov ebx, 1 */
        e8(0x89); e8(0xE1);/* mov ecx, esp */
        e8(0xBA); e32(1);  /* mov edx, 1 */
        e8(0xCD); e8(0x80);
        e8(0x83); e8(0xC4); e8(4); /* add esp, 4 */
        return 1;
    }
    if (strcmp(name, "getchar") == 0) {
        e8(0xFB);           /* sti — ensure interrupts are on */
        e8(0xB8); e32(5);  /* mov eax, SYS_GETC */
        e8(0xCD); e8(0x80);
        e8(0x50);           /* push eax (result) */
        return 1;
    }
    if (strcmp(name, "sleep") == 0 && argc == 1) {
        e8(0x5B);           /* pop ebx = ms */
        e8(0xB8); e32(4);  /* mov eax, SYS_SLEEP */
        e8(0xCD); e8(0x80);
        return 1;
    }
    if (strcmp(name, "uptime") == 0) {
        e8(0xB8); e32(6);  /* mov eax, SYS_UPTIME */
        e8(0xCD); e8(0x80);
        e8(0x50);           /* push result */
        return 1;
    }
    if (strcmp(name, "getpid") == 0) {
        e8(0xB8); e32(2);
        e8(0xCD); e8(0x80);
        e8(0x50);
        return 1;
    }
    if (strcmp(name, "exit") == 0 && argc == 1) {
        e8(0x58);           /* pop eax = status (return code) */
        e8(0x89); e8(0xEC); /* mov esp, ebp */
        e8(0x5D);           /* pop ebp */
        e8(0xC3);           /* ret (back to stub, then to elf_exec) */
        return 1;
    }
    if (strcmp(name, "vga_clear") == 0 && argc == 1) {
        /* Pop color byte, fill 80*25 cells at 0xB8000 with ' '+color */
        e8(0x58);  /* pop eax = color */
        e8(0x88); e8(0xC4);  /* mov ah, al (color in high byte) */
        e8(0xB0); e8(0x20);  /* mov al, ' ' (space in low byte) */
        /* Now ax = color<<8 | ' '.  Write as 16-bit words. */
        /* mov ecx, 0xB8000 */
        e8(0xB9); e32(0xB8000);
        /* mov edx, 2000 */
        e8(0xBA); e32(2000);
        /* .loop: mov [ecx], ax */
        e8(0x66); e8(0x89); e8(0x01);
        /* add ecx, 2 */
        e8(0x83); e8(0xC1); e8(2);
        /* dec edx */
        e8(0x4A);
        /* jnz .loop */
        e8(0x75); e8(0xF6);
        return 1;
    }
    if (strcmp(name, "vga_putchar") == 0 && argc == 4) {
        /* args on stack: x, y, ch, color (pushed right to left) */
        e8(0x58);  /* pop eax = color */
        e8(0x5B);  /* pop ebx = ch */
        e8(0x59);  /* pop ecx = y */
        e8(0x5A);  /* pop edx = x */
        /* offset = (y * 80 + x) * 2 */
        e8(0x6B); e8(0xC9); e8(80);  /* imul ecx, 80 */
        e8(0x01); e8(0xD1);          /* add ecx, edx */
        e8(0xD1); e8(0xE1);          /* shl ecx, 1 */
        /* mov byte [0xB8000 + ecx], bl (char) */
        e8(0x88); e8(0x99); e32(0xB8000);
        /* mov byte [0xB8001 + ecx], al (color) */
        e8(0x88); e8(0x81); e32(0xB8001);
        return 1;
    }
    if (strcmp(name, "vga_print") == 0 && argc == 4) {
        /* args: x, y, str, color */
        e8(0x58);  /* pop eax = color */
        e8(0x5E);  /* pop esi = str */
        e8(0x59);  /* pop ecx = y */
        e8(0x5A);  /* pop edx = x */
        /* offset = (y * 80 + x) * 2 */
        e8(0x6B); e8(0xC9); e8(80);
        e8(0x01); e8(0xD1);
        e8(0xD1); e8(0xE1);
        e8(0x81); e8(0xC1); e32(0xB8000);  /* add ecx, 0xB8000 */
        /* loop: */
        e8(0x8A); e8(0x1E);  /* mov bl, [esi] */
        e8(0x84); e8(0xDB);  /* test bl, bl */
        e8(0x74); e8(0x08);  /* jz done */
        e8(0x88); e8(0x19);  /* mov [ecx], bl */
        e8(0x88); e8(0x41); e8(0x01);  /* mov [ecx+1], al */
        e8(0x46);            /* inc esi */
        e8(0x83); e8(0xC1); e8(2); /* add ecx, 2 */
        e8(0xEB); e8(0xEF);  /* jmp loop */
        /* done: */
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
        e8(0x68); e32((uint32_t)tok_num);  /* push imm32 */
        next_token();
    } else if (cur_token == T_STR) {
        uint32_t soff = store_string(tok_str);
        /* push address of string — store a fixup so we can patch it
         * once we know data_base_addr (after all code is emitted). */
        e8(0x68);  /* push imm32 */
        if (nstr_fixups < MAX_STRINGS) {
            str_fixups[nstr_fixups].code_off = cp;
            str_fixups[nstr_fixups].data_off = soff;
            nstr_fixups++;
        }
        e32(0);  /* placeholder — patched in finalize */
        next_token();
    } else if (cur_token == T_IDENT) {
        char name[64];
        strncpy(name, tok_ident, 63);
        next_token();

        if (cur_token == T_LPAREN) {
            /* Function call */
            next_token();  /* skip ( */
            int argc = 0;
            /* Parse args and push them (left to right, will be on stack) */
            /* We need right-to-left for cdecl. Collect first, then push reversed. */
            /* Simpler: just push left-to-right and let builtins handle it. */
            if (cur_token != T_RPAREN) {
                parse_expr(); argc++;
                while (cur_token == T_COMMA) {
                    next_token();
                    parse_expr(); argc++;
                }
            }
            expect(T_RPAREN);

            if (!emit_builtin(name, argc)) {
                /* User function call */
                e8(0xE8);  /* call rel32 */
                call_fixups[ncall_fixups].code_off = cp;
                strncpy(call_fixups[ncall_fixups].name, name, 31);
                ncall_fixups++;
                e32(0);  /* placeholder */
                if (argc > 0) {
                    e8(0x83); e8(0xC4); e8((uint8_t)(argc * 4)); /* add esp, N */
                }
                e8(0x50);  /* push eax (return value) */
            }
        } else if (cur_token == T_ASSIGN) {
            /* Assignment: name = expr */
            next_token();
            parse_expr();
            int off = find_local(name);
            if (off == 0x7FFFFFFF) off = add_local(name);
            e8(0x58);  /* pop eax */
            e8(0x89); e8(0x85); e32((uint32_t)off);  /* mov [ebp+off], eax */
            e8(0x50);  /* push eax (assignment is an expression) */
        } else {
            /* Variable read */
            int off = find_local(name);
            if (off == 0x7FFFFFFF) {
                kprintf("tcc:%d: undefined variable '%s'\n", line_num, name);
                e8(0x68); e32(0);  /* push 0 as fallback */
            } else {
                e8(0x8B); e8(0x85); e32((uint32_t)off);  /* mov eax, [ebp+off] */
                e8(0x50);  /* push eax */
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
        parse_primary();
        e8(0x58);  /* pop eax */
        e8(0xF7); e8(0xD8);  /* neg eax */
        e8(0x50);  /* push eax */
    } else if (cur_token == T_NOT) {
        next_token();
        parse_primary();
        e8(0x58);
        e8(0x85); e8(0xC0);  /* test eax, eax */
        e8(0x0F); e8(0x94); e8(0xC0);  /* setz al */
        e8(0x0F); e8(0xB6); e8(0xC0);  /* movzx eax, al */
        e8(0x50);
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
        e8(0x5B); e8(0x58);  /* pop ebx, pop eax */
        if (op == T_PLUS) { e8(0x01); e8(0xD8); }  /* add eax, ebx */
        else               { e8(0x29); e8(0xD8); }  /* sub eax, ebx */
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
        e8(0x5B); e8(0x58);  /* pop ebx, pop eax */
        e8(0x39); e8(0xD8);  /* cmp eax, ebx */
        uint8_t setcc;
        switch (op) {
            case T_EQ:  setcc = 0x94; break; /* sete */
            case T_NEQ: setcc = 0x95; break;
            case T_LT:  setcc = 0x9C; break;
            case T_GT:  setcc = 0x9F; break;
            case T_LTE: setcc = 0x9E; break;
            case T_GTE: setcc = 0x9D; break;
            default:    setcc = 0x94; break;
        }
        e8(0x0F); e8(setcc); e8(0xC0);  /* setcc al */
        e8(0x0F); e8(0xB6); e8(0xC0);  /* movzx eax, al */
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
            e8(0x85); e8(0xC0);  /* test eax */
            e8(0x0F); e8(0x95); e8(0xC0);  /* setne al */
            e8(0x85); e8(0xDB);
            e8(0x0F); e8(0x95); e8(0xC3);
            e8(0x20); e8(0xD8);  /* and al, bl */
        } else {
            e8(0x09); e8(0xD8);  /* or eax, ebx */
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
    } else if (cur_token == T_IF) {
        next_token();
        expect(T_LPAREN);
        parse_expr();
        expect(T_RPAREN);
        e8(0x58);  /* pop eax */
        e8(0x85); e8(0xC0);  /* test eax, eax */
        e8(0x0F); e8(0x84); uint32_t jz_pos = cp; e32(0);  /* jz else */
        parse_statement();
        if (cur_token == T_ELSE) {
            next_token();
            e8(0xE9); uint32_t jmp_pos = cp; e32(0);  /* jmp end */
            patch32(jz_pos, cp - jz_pos - 4);
            parse_statement();
            patch32(jmp_pos, cp - jmp_pos - 4);
        } else {
            patch32(jz_pos, cp - jz_pos - 4);
        }
    } else if (cur_token == T_WHILE) {
        next_token();
        uint32_t loop_top = cp;
        expect(T_LPAREN);
        parse_expr();
        expect(T_RPAREN);
        e8(0x58);
        e8(0x85); e8(0xC0);
        e8(0x0F); e8(0x84); uint32_t jz_pos = cp; e32(0);
        parse_statement();
        e8(0xE9); e32(loop_top - cp - 4);  /* jmp loop_top */
        patch32(jz_pos, cp - jz_pos - 4);
    } else if (cur_token == T_FOR) {
        next_token();
        expect(T_LPAREN);
        /* init */
        if (cur_token != T_SEMI) { parse_expr(); e8(0x83); e8(0xC4); e8(4); }
        expect(T_SEMI);
        uint32_t loop_top = cp;
        /* condition */
        if (cur_token != T_SEMI) parse_expr();
        else { e8(0x68); e32(1); }  /* always true */
        expect(T_SEMI);
        /* save increment source position for later */
        int save_pos = src_pos;
        int save_line = line_num;
        int save_tok = cur_token;
        /* skip increment expr for now */
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
        int body_end_pos = src_pos;
        int body_end_line = line_num;
        int body_end_tok = cur_token;
        src_pos = save_pos;
        line_num = save_line;
        next_token();
        if (cur_token != T_RPAREN) {
            parse_expr();
            e8(0x83); e8(0xC4); e8(4); /* discard result */
        }
        /* restore position after body */
        src_pos = body_end_pos;
        line_num = body_end_line;
        cur_token = body_end_tok;
        /* jump back to condition */
        e8(0xE9); e32(loop_top - cp - 4);
        patch32(jz_pos, cp - jz_pos - 4);
    } else if (cur_token == T_RETURN) {
        next_token();
        if (cur_token != T_SEMI) {
            parse_expr();
            e8(0x58);  /* pop eax = return value */
        }
        /* epilogue */
        e8(0x89); e8(0xEC);  /* mov esp, ebp */
        e8(0x5D);            /* pop ebp */
        e8(0xC3);            /* ret */
        if (cur_token == T_SEMI) next_token();
    } else if (cur_token == T_INT || cur_token == T_CHAR) {
        /* Variable declaration: int x; or int x = expr; */
        next_token();
        char vname[64];
        strncpy(vname, tok_ident, 63);
        next_token();  /* consume identifier */

        if (cur_token == T_LBRACK) {
            /* Array: int arr[N]; */
            next_token();
            int size = tok_num;
            next_token();  /* consume size */
            expect(T_RBRACK);
            /* Allocate N*4 bytes on stack */
            int off = find_local(vname);
            if (off == 0x7FFFFFFF) {
                stack_offset -= size * 4;
                off = stack_offset;
                strncpy(locals[nlocals].name, vname, 31);
                locals[nlocals].offset = off;
                nlocals++;
            }
            e8(0x83); e8(0xEC); e8((uint8_t)(size * 4));
        } else if (cur_token == T_ASSIGN) {
            next_token();
            parse_expr();
            int off = add_local(vname);
            e8(0x58);
            e8(0x89); e8(0x85); e32((uint32_t)off);
            e8(0x83); e8(0xEC); e8(4);  /* reserve space */
        } else {
            add_local(vname);
            e8(0x83); e8(0xEC); e8(4);
        }
        if (cur_token == T_SEMI) next_token();
    } else {
        /* Expression statement */
        parse_expr();
        e8(0x83); e8(0xC4); e8(4);  /* discard result: add esp, 4 */
        if (cur_token == T_SEMI) next_token();
    }
}

/* Parse a function definition */
static void parse_function(void) {
    /* Already consumed return type. Current token is the function name. */
    char fname[64];
    strncpy(fname, tok_ident, 63);
    next_token();  /* consume name */

    /* Record function address */
    funcs[nfuncs].code_offset = cp;
    strncpy(funcs[nfuncs].name, fname, 31);
    nfuncs++;

    expect(T_LPAREN);
    /* Parse parameters */
    int param_count = 0;
    nlocals = 0;
    stack_offset = 0;
    while (cur_token != T_RPAREN && cur_token != T_EOF) {
        if (cur_token == T_INT || cur_token == T_CHAR || cur_token == T_VOID)
            next_token();
        /* Parameter is at ebp+8, ebp+12, etc. */
        strncpy(locals[nlocals].name, tok_ident, 31);
        locals[nlocals].offset = 8 + param_count * 4;
        nlocals++;
        param_count++;
        next_token();
        if (cur_token == T_COMMA) next_token();
    }
    expect(T_RPAREN);

    /* Function prologue */
    e8(0x55);              /* push ebp */
    e8(0x89); e8(0xE5);   /* mov ebp, esp */

    /* Parse body */
    expect(T_LBRACE);
    while (cur_token != T_RBRACE && cur_token != T_EOF)
        parse_statement();
    expect(T_RBRACE);

    /* Epilogue (implicit return 0) */
    e8(0x31); e8(0xC0);   /* xor eax, eax */
    e8(0x89); e8(0xEC);   /* mov esp, ebp */
    e8(0x5D);              /* pop ebp */
    e8(0xC3);              /* ret */
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
    memset(code, 0, sizeof(code));
    memset(data_sec, 0, sizeof(data_sec));

    hdr_size = 52 + 32;  /* ELF header + 1 program header */

    /* Emit a call to main() then return cleanly to elf_exec.
     * We DON'T use SYS_EXIT because that goes through int 0x80 and
     * never returns to our caller. Instead, just ret. */
    uint32_t call_main_pos;
    e8(0xE8); call_main_pos = cp; e32(0);  /* call main */
    /* return to elf_exec (fn() call returns here) */
    e8(0xC3);  /* ret */

    /* Parse all top-level declarations */
    next_token();
    while (cur_token != T_EOF) {
        if (cur_token == T_INT || cur_token == T_CHAR || cur_token == T_VOID) {
            next_token();  /* consume type */
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

    /* ELF header */
    elf[0]=0x7F; elf[1]='E'; elf[2]='L'; elf[3]='F';
    elf[4]=1; elf[5]=1; elf[6]=1;
    *(uint16_t*)(elf+16) = 2;
    *(uint16_t*)(elf+18) = 3;
    *(uint32_t*)(elf+20) = 1;
    *(uint32_t*)(elf+24) = BASE_ADDR + hdr_size;  /* entry */
    *(uint32_t*)(elf+28) = 52;
    *(uint16_t*)(elf+40) = 52;
    *(uint16_t*)(elf+42) = 32;
    *(uint16_t*)(elf+44) = 1;

    /* Program header */
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

    /* Write output */
    vfs_node_t *out = vfs_create(out_path, s->cwd);
    if (!out) { kfree(elf); kprintf("tcc: cannot create '%s'\n", out_path); return -1; }
    out->size = 0;
    out->permissions = 0755;
    vfs_write(out, 0, total, elf);
    kfree(elf);

    kprintf("tcc: compiled %u bytes code + %u bytes data → %s (%u bytes)\n",
            cp, dp, out_path, total);
    return 0;
}

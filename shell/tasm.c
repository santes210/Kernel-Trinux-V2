/* shell/tasm.c  -  Trinux Assembler: assemble x86 ASM source → ELF32 binary.
 *
 * Two-pass assembler:
 *   Pass 1: scan for labels, compute sizes
 *   Pass 2: emit machine code, resolve label references
 *
 * Output: a minimal ELF32 executable loaded at 0x08048000 with _start at
 * the beginning of the code.  The user can then run it with `exec`.
 */
#include "tasm.h"
#include "shell.h"
#include "../fs/vfs.h"
#include "../mm/kheap.h"
#include "../lib/string.h"
#include "../lib/printf.h"
#include "../drivers/serial.h"

#define MAX_LABELS  128
#define MAX_CODE    8192
#define BASE_ADDR   0x08048000u
#define READ_BUF    8192

/* ---- label table ---- */
typedef struct { char name[32]; uint32_t offset; } label_t;
static label_t labels[MAX_LABELS];
static int     nlabels;

/* ---- fixups (label references to resolve in pass 2) ---- */
typedef struct { uint32_t code_offset; char name[32]; int rel; /* 1=relative */ } fixup_t;
static fixup_t fixups[MAX_LABELS];
static int     nfixups;

/* ---- code buffer ---- */
static uint8_t  code[MAX_CODE];
static uint32_t code_pos;

static void emit8(uint8_t b)  { if (code_pos < MAX_CODE) code[code_pos++] = b; }
static void emit32(uint32_t v) {
    emit8(v & 0xFF); emit8((v>>8)&0xFF); emit8((v>>16)&0xFF); emit8((v>>24)&0xFF);
}

static int find_label(const char *name) {
    for (int i = 0; i < nlabels; i++)
        if (strcmp(labels[i].name, name) == 0) return i;
    return -1;
}

static void add_label(const char *name, uint32_t off) {
    if (nlabels < MAX_LABELS) {
        strncpy(labels[nlabels].name, name, 31);
        labels[nlabels].offset = off;
        nlabels++;
    }
}

static void add_fixup(uint32_t off, const char *name, int rel) {
    if (nfixups < MAX_LABELS) {
        fixups[nfixups].code_offset = off;
        fixups[nfixups].rel = rel;
        strncpy(fixups[nfixups].name, name, 31);
        nfixups++;
    }
}

/* ---- register encoding ---- */
static int parse_reg(const char *s) {
    if (strcmp(s,"eax")==0) return 0; if (strcmp(s,"ecx")==0) return 1;
    if (strcmp(s,"edx")==0) return 2; if (strcmp(s,"ebx")==0) return 3;
    if (strcmp(s,"esp")==0) return 4; if (strcmp(s,"ebp")==0) return 5;
    if (strcmp(s,"esi")==0) return 6; if (strcmp(s,"edi")==0) return 7;
    return -1;
}

static int is_number(const char *s) {
    if (!s || !*s) return 0;
    if (*s == '-') s++;
    if (s[0]=='0' && s[1]=='x') return 1;
    while (*s) { if (*s < '0' || *s > '9') return 0; s++; }
    return 1;
}

static uint32_t parse_hex(const char *s) {
    uint32_t v = 0;
    while (*s) {
        char c = *s;
        if (c >= '0' && c <= '9') v = v*16 + (uint32_t)(c-'0');
        else if (c >= 'a' && c <= 'f') v = v*16 + (uint32_t)(c-'a'+10);
        else if (c >= 'A' && c <= 'F') v = v*16 + (uint32_t)(c-'A'+10);
        else break;
        s++;
    }
    return v;
}

static uint32_t parse_imm(const char *s) {
    if (s[0]=='0' && s[1]=='x') return parse_hex(s+2);
    return (uint32_t)atoi(s);
}

/* ---- tokenize a line ---- */
static void strip(char *s) {
    /* remove leading/trailing whitespace and comments */
    while (*s == ' ' || *s == '\t') { char *d=s; while(*d){*d=*(d+1);d++;} }
    char *c = strchr(s, ';');
    if (c) *c = '\0';
    int l = strlen(s);
    while (l > 0 && (s[l-1]==' '||s[l-1]=='\t'||s[l-1]=='\r'||s[l-1]=='\n'))
        s[--l] = '\0';
}

/* Split "op arg1, arg2" into parts. Returns number of parts. */
static int split_line(char *line, char *parts[], int max) {
    int n = 0;
    char *p = line;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        parts[n++] = p;
        if (*p == '"') {
            p++; while (*p && *p != '"') p++; if (*p) p++;
        } else {
            while (*p && *p != ' ' && *p != '\t' && *p != ',') p++;
        }
        if (*p) *p++ = '\0';
    }
    return n;
}

/* ---- assemble one line ---- */
static int asm_line(char *line, int pass) {
    strip(line);
    if (line[0] == '\0') return 0;

    /* Check for label: "name:" */
    int len = strlen(line);
    if (len > 1 && line[len-1] == ':') {
        line[len-1] = '\0';
        if (pass == 1) add_label(line, code_pos);
        return 0;
    }

    char *parts[8];
    int np = split_line(line, parts, 8);
    if (np == 0) return 0;

    char *op = parts[0];

    /* ---- db "string", byte, ... ---- */
    if (strcmp(op, "db") == 0) {
        for (int i = 1; i < np; i++) {
            if (parts[i][0] == '"') {
                char *s = parts[i] + 1;
                int sl = strlen(s);
                if (sl > 0 && s[sl-1] == '"') s[sl-1] = '\0';
                while (*s) { emit8(*s == '\\' && *(s+1)=='n' ? (s++,'\n') : *s); s++; }
            } else {
                emit8((uint8_t)parse_imm(parts[i]));
            }
        }
        return 0;
    }

    /* ---- nop ---- */
    if (strcmp(op, "nop") == 0) { emit8(0x90); return 0; }
    /* ---- ret ---- */
    if (strcmp(op, "ret") == 0) { emit8(0xC3); return 0; }
    /* ---- hlt ---- */
    if (strcmp(op, "hlt") == 0) { emit8(0xF4); return 0; }

    /* ---- int imm ---- */
    if (strcmp(op, "int") == 0 && np >= 2) {
        emit8(0xCD); emit8((uint8_t)parse_imm(parts[1]));
        return 0;
    }

    /* ---- push reg ---- */
    if (strcmp(op, "push") == 0 && np >= 2) {
        int r = parse_reg(parts[1]);
        if (r >= 0) { emit8(0x50 + r); return 0; }
        /* push imm32 */
        if (is_number(parts[1])) { emit8(0x68); emit32(parse_imm(parts[1])); return 0; }
    }
    /* ---- pop reg ---- */
    if (strcmp(op, "pop") == 0 && np >= 2) {
        int r = parse_reg(parts[1]);
        if (r >= 0) { emit8(0x58 + r); return 0; }
    }

    /* ---- inc/dec reg ---- */
    if (strcmp(op, "inc") == 0 && np >= 2) {
        int r = parse_reg(parts[1]);
        if (r >= 0) { emit8(0x40 + r); return 0; }
    }
    if (strcmp(op, "dec") == 0 && np >= 2) {
        int r = parse_reg(parts[1]);
        if (r >= 0) { emit8(0x48 + r); return 0; }
    }

    /* ---- mov reg, imm/reg/label ---- */
    if (strcmp(op, "mov") == 0 && np >= 3) {
        int rd = parse_reg(parts[1]);
        int rs = parse_reg(parts[2]);
        if (rd >= 0 && rs >= 0) {
            /* mov reg, reg */
            emit8(0x89); emit8(0xC0 | (rs << 3) | rd);
            return 0;
        }
        if (rd >= 0 && is_number(parts[2])) {
            /* mov reg, imm32 */
            emit8(0xB8 + rd); emit32(parse_imm(parts[2]));
            return 0;
        }
        if (rd >= 0) {
            /* mov reg, label (address) */
            emit8(0xB8 + rd);
            if (pass == 2) add_fixup(code_pos, parts[2], 0);
            emit32(0);  /* placeholder */
            return 0;
        }
    }

    /* ---- ALU: add/sub/and/or/xor/cmp reg, imm/reg ---- */
    struct { const char *name; uint8_t alu_op; } alus[] = {
        {"add",0}, {"or",1}, {"and",4}, {"sub",5}, {"xor",6}, {"cmp",7}
    };
    for (unsigned a = 0; a < sizeof(alus)/sizeof(alus[0]); a++) {
        if (strcmp(op, alus[a].name) == 0 && np >= 3) {
            int rd = parse_reg(parts[1]);
            int rs = parse_reg(parts[2]);
            if (rd >= 0 && rs >= 0) {
                /* alu reg, reg */
                emit8(0x01 | (alus[a].alu_op << 3));
                emit8(0xC0 | (rs << 3) | rd);
                return 0;
            }
            if (rd == 0 && is_number(parts[2])) {
                /* alu eax, imm32 (short form) */
                emit8(0x05 | (alus[a].alu_op << 3));
                emit32(parse_imm(parts[2]));
                return 0;
            }
            if (rd >= 0 && is_number(parts[2])) {
                /* alu reg, imm32 */
                emit8(0x81); emit8(0xC0 | (alus[a].alu_op << 3) | rd);
                emit32(parse_imm(parts[2]));
                return 0;
            }
        }
    }

    /* ---- jmp/call/je/jne/jz/jnz label ---- */
    if (strcmp(op,"jmp")==0 && np>=2) {
        emit8(0xE9);
        if (pass == 2) add_fixup(code_pos, parts[1], 1);
        emit32(0); return 0;
    }
    if (strcmp(op,"call")==0 && np>=2) {
        emit8(0xE8);
        if (pass == 2) add_fixup(code_pos, parts[1], 1);
        emit32(0); return 0;
    }
    if ((strcmp(op,"je")==0 || strcmp(op,"jz")==0) && np>=2) {
        emit8(0x0F); emit8(0x84);
        if (pass == 2) add_fixup(code_pos, parts[1], 1);
        emit32(0); return 0;
    }
    if ((strcmp(op,"jne")==0 || strcmp(op,"jnz")==0) && np>=2) {
        emit8(0x0F); emit8(0x85);
        if (pass == 2) add_fixup(code_pos, parts[1], 1);
        emit32(0); return 0;
    }

    kprintf("asm: unknown instruction: %s\n", line);
    return -1;
}

/* ---- build ELF32 header + program header ---- */
static void build_elf(uint8_t *out, uint32_t *out_size)
{
    /* Minimal ELF: 52-byte ELF header + 32-byte program header + code */
    uint32_t hdr_size = 52 + 32;   /* ehdr + 1 phdr */
    uint32_t total = hdr_size + code_pos;

    /* ELF header */
    memset(out, 0, hdr_size);
    out[0]=0x7F; out[1]='E'; out[2]='L'; out[3]='F';
    out[4]=1;    /* 32-bit */
    out[5]=1;    /* little-endian */
    out[6]=1;    /* ELF version */
    *(uint16_t*)(out+16) = 2;      /* ET_EXEC */
    *(uint16_t*)(out+18) = 3;      /* EM_386 */
    *(uint32_t*)(out+20) = 1;      /* version */
    *(uint32_t*)(out+24) = BASE_ADDR + hdr_size;   /* entry point */
    *(uint32_t*)(out+28) = 52;     /* phoff */
    *(uint16_t*)(out+40) = 52;     /* ehsize */
    *(uint16_t*)(out+42) = 32;     /* phentsize */
    *(uint16_t*)(out+44) = 1;      /* phnum */

    /* Program header (PT_LOAD: load everything) */
    uint8_t *ph = out + 52;
    *(uint32_t*)(ph+0)  = 1;          /* PT_LOAD */
    *(uint32_t*)(ph+4)  = 0;          /* offset in file */
    *(uint32_t*)(ph+8)  = BASE_ADDR;  /* vaddr */
    *(uint32_t*)(ph+12) = BASE_ADDR;  /* paddr */
    *(uint32_t*)(ph+16) = total;      /* filesz */
    *(uint32_t*)(ph+20) = total;      /* memsz */
    *(uint32_t*)(ph+24) = 7;          /* flags: RWX */
    *(uint32_t*)(ph+28) = 0x1000;     /* align */

    /* Copy code after headers */
    memcpy(out + hdr_size, code, code_pos);

    *out_size = total;
}

/* ---- main entry ---- */
int tasm_assemble(const char *src_path, const char *out_path)
{
    shell_state_t *s;
    extern shell_state_t *shell_get_state(void);
    s = shell_get_state();

    vfs_node_t *src = vfs_resolve(src_path, s->cwd);
    if (!src || src->type != VFS_FILE) {
        kprintf("asm: %s: not found\n", src_path);
        return -1;
    }

    char *text = (char *)kmalloc(READ_BUF);
    if (!text) { kprintf("asm: out of memory\n"); return -1; }
    uint32_t len = vfs_read(src, 0, READ_BUF - 1, (uint8_t *)text);
    text[len] = '\0';

    /* Pass 1: find labels and compute sizes */
    nlabels = 0; nfixups = 0; code_pos = 0;
    char *work = (char *)kmalloc(READ_BUF);
    if (!work) { kfree(text); return -1; }

    memcpy(work, text, len + 1);
    char *p = work;
    int line_num = 0;
    int errors = 0;
    uint32_t hdr_size = 52 + 32;

    while (*p) {
        char line[256];
        int i = 0;
        while (*p && *p != '\n' && i < 255) line[i++] = *p++;
        line[i] = '\0';
        if (*p == '\n') p++;
        line_num++;
        if (asm_line(line, 1) < 0) errors++;
    }

    if (errors) {
        kfree(text); kfree(work);
        kprintf("asm: %d error(s) in pass 1\n", errors);
        return -1;
    }

    /* Update label addresses to include ELF header offset */
    for (int i = 0; i < nlabels; i++)
        labels[i].offset += BASE_ADDR + hdr_size;

    /* Pass 2: emit code and resolve fixups */
    code_pos = 0; nfixups = 0;
    memcpy(work, text, len + 1);
    p = work;
    while (*p) {
        char line[256];
        int i = 0;
        while (*p && *p != '\n' && i < 255) line[i++] = *p++;
        line[i] = '\0';
        if (*p == '\n') p++;
        asm_line(line, 2);
    }

    /* Resolve fixups */
    for (int i = 0; i < nfixups; i++) {
        int li = find_label(fixups[i].name);
        if (li < 0) {
            kprintf("asm: undefined label '%s'\n", fixups[i].name);
            errors++;
            continue;
        }
        uint32_t target = labels[li].offset;
        uint32_t *patch = (uint32_t *)&code[fixups[i].code_offset];
        if (fixups[i].rel) {
            /* relative: target - (current_addr + 4) */
            uint32_t here = BASE_ADDR + hdr_size + fixups[i].code_offset + 4;
            *patch = target - here;
        } else {
            *patch = target;
        }
    }

    kfree(text); kfree(work);

    if (errors) {
        kprintf("asm: %d error(s) in pass 2\n", errors);
        return -1;
    }

    /* Build ELF */
    uint8_t *elf = (uint8_t *)kmalloc(MAX_CODE + 256);
    if (!elf) { kprintf("asm: out of memory for ELF\n"); return -1; }
    uint32_t elf_size;
    build_elf(elf, &elf_size);

    /* Write to output file */
    vfs_node_t *out = vfs_create(out_path, s->cwd);
    if (!out) {
        kfree(elf);
        kprintf("asm: cannot create '%s'\n", out_path);
        return -1;
    }
    out->size = 0;
    out->permissions = 0755;
    vfs_write(out, 0, elf_size, elf);
    kfree(elf);

    kprintf("asm: assembled %u bytes of code → %s (%u bytes ELF)\n",
            code_pos, out_path, elf_size);
    return 0;
}

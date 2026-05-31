/* lib/string.c  -  freestanding string / memory / conversion routines. */
#include "string.h"

/* kmalloc for strdup */
extern void *kmalloc(size_t size);

/* ---------------- memory ---------------- */

void *memcpy(void *dest, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++)
        d[i] = s[i];
    return dest;
}

void *memset(void *dest, int c, size_t n)
{
    uint8_t *d = (uint8_t *)dest;
    for (size_t i = 0; i < n; i++)
        d[i] = (uint8_t)c;
    return dest;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i])
            return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

void *memmove(void *dest, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    if (d == s || n == 0)
        return dest;
    if (d < s) {
        for (size_t i = 0; i < n; i++)
            d[i] = s[i];
    } else {
        for (size_t i = n; i > 0; i--)
            d[i - 1] = s[i - 1];
    }
    return dest;
}

/* ---------------- string ---------------- */

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

char *strcpy(char *dest, const char *src)
{
    char *d = dest;
    while ((*d++ = *src++))
        ;
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++)
        dest[i] = src[i];
    for (; i < n; i++)
        dest[i] = '\0';
    return dest;
}

int strcmp(const char *a, const char *b)
{
    while (*a && (*a == *b)) {
        a++;
        b++;
    }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || a[i] == '\0')
            return (int)(uint8_t)a[i] - (int)(uint8_t)b[i];
    }
    return 0;
}

char *strcat(char *dest, const char *src)
{
    char *d = dest + strlen(dest);
    while ((*d++ = *src++))
        ;
    return dest;
}

char *strncat(char *dest, const char *src, size_t n)
{
    char *d = dest + strlen(dest);
    size_t i = 0;
    for (; i < n && src[i]; i++)
        d[i] = src[i];
    d[i] = '\0';
    return dest;
}

char *strchr(const char *s, int c)
{
    while (*s) {
        if (*s == (char)c)
            return (char *)s;
        s++;
    }
    return (c == '\0') ? (char *)s : NULL;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    do {
        if (*s == (char)c)
            last = s;
    } while (*s++);
    return (char *)last;
}

char *strstr(const char *haystack, const char *needle)
{
    if (!*needle)
        return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && (*h == *n)) {
            h++;
            n++;
        }
        if (!*n)
            return (char *)haystack;
    }
    return NULL;
}

/* Reentrant-ish strtok using a static save pointer (single-threaded kernel). */
char *strtok(char *str, const char *delim)
{
    static char *save;
    if (str)
        save = str;
    if (!save)
        return NULL;

    /* skip leading delimiters */
    while (*save && strchr(delim, *save))
        save++;
    if (!*save) {
        save = NULL;
        return NULL;
    }

    char *token = save;
    while (*save && !strchr(delim, *save))
        save++;
    if (*save) {
        *save = '\0';
        save++;
    } else {
        save = NULL;
    }
    return token;
}

char *strdup(const char *s)
{
    size_t len = strlen(s) + 1;
    char *p = (char *)kmalloc(len);
    if (p)
        memcpy(p, s, len);
    return p;
}

/* ---------------- conversion ---------------- */

int atoi(const char *s)
{
    int sign = 1;
    int result = 0;
    while (isspace(*s))
        s++;
    if (*s == '-') {
        sign = -1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    while (isdigit(*s)) {
        result = result * 10 + (*s - '0');
        s++;
    }
    return sign * result;
}

static void str_reverse(char *buf, int len)
{
    int i = 0, j = len - 1;
    while (i < j) {
        char t = buf[i];
        buf[i] = buf[j];
        buf[j] = t;
        i++;
        j--;
    }
}

void itoa(int value, char *buf, int base)
{
    int i = 0;
    bool negative = false;
    unsigned int uv;

    if (value == 0) {
        buf[i++] = '0';
        buf[i] = '\0';
        return;
    }

    if (base == 10 && value < 0) {
        negative = true;
        uv = (unsigned int)(-value);
    } else {
        uv = (unsigned int)value;
    }

    while (uv) {
        int rem = uv % base;
        buf[i++] = (rem < 10) ? ('0' + rem) : ('a' + rem - 10);
        uv /= base;
    }
    if (negative)
        buf[i++] = '-';
    buf[i] = '\0';
    str_reverse(buf, i);
}

void itoa_hex(uint32_t value, char *buf, bool upper)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int i = 0;
    if (value == 0) {
        buf[i++] = '0';
        buf[i] = '\0';
        return;
    }
    while (value) {
        buf[i++] = digits[value & 0xF];
        value >>= 4;
    }
    buf[i] = '\0';
    str_reverse(buf, i);
}

/* ---------------- ctype ---------------- */

int toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
int tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isspace(int c) { return c == ' ' || c == '\t' || c == '\n' ||
                            c == '\r' || c == '\v' || c == '\f'; }

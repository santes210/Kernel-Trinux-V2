#ifndef LIB_STRING_H
#define LIB_STRING_H

#include "types.h"

/* memory */
void  *memcpy(void *dest, const void *src, size_t n);
void  *memset(void *dest, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
void  *memmove(void *dest, const void *src, size_t n);

/* string */
size_t strlen(const char *s);
char  *strcpy(char *dest, const char *src);
char  *strncpy(char *dest, const char *src, size_t n);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcat(char *dest, const char *src);
char  *strncat(char *dest, const char *src, size_t n);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *haystack, const char *needle);
char  *strtok(char *str, const char *delim);
char  *strdup(const char *s);

/* conversion */
int    atoi(const char *s);
void   itoa(int value, char *buf, int base);
void   itoa_hex(uint32_t value, char *buf, bool upper);

/* ctype */
int    toupper(int c);
int    tolower(int c);
int    isdigit(int c);
int    isalpha(int c);
int    isalnum(int c);
int    isspace(int c);

#endif /* LIB_STRING_H */

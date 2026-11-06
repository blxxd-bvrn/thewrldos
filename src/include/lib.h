#ifndef LIB_H
#define LIB_H
#include "types.h"
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memchr(const void *s, int c, size_t n);
char *strtok(char *s, const char *delim);
char *strpbrk(const char *s, const char *accept);
void *memset(void *d, int c, size_t n);
void *memset32(uint32_t *d, uint32_t v, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
char *strcpy(char *d, const char *s);
char *strncpy(char *d, const char *s, size_t n);
size_t strlcpy(char *d, const char *s, size_t n);
size_t strlcat(char *d, const char *s, size_t n);
char *strcat(char *d, const char *s);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *h, const char *n);
char *strdup(const char *s);
char *strndup(const char *s, size_t n);
int toupper(int c);
int tolower(int c);
int isdigit(int c);
int isalpha(int c);
int isalnum(int c);
int isspace(int c);
int isxdigit(int c);
long strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
int atoi(const char *s);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int snprintf(char *buf, size_t n, const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
const char *str_ends_with_ci(const char *s, const char *suffix);
void format_size(char *buf, size_t n, uint32_t bytes);
uint32_t rand_next(void);
void rand_seed(uint32_t s);
#endif

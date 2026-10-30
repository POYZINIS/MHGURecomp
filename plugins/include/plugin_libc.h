// The C library functions a plugin can use (plugins/, built freestanding): the game's own (its SDK exports them;
// the port binds them by name, runtime/loader.c). Only these, whose arguments are all in core registers: the SDK's
// float functions take their floats in VFP registers (use the compiler's __builtin_*).
#pragma once
#include <stddef.h>

size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strstr(const char *s, const char *sub);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
int atoi(const char *s);
int snprintf(char *out, size_t n, const char *fmt, ...);
void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);

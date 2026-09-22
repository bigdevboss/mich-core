#ifndef MICH64_STRING_H
#define MICH64_STRING_H

#include <stddef.h>

void *memcpy(void *destination, const void *source, size_t length);
void *memmove(void *destination, const void *source, size_t length);
void *memset(void *destination, int value, size_t length);
int memcmp(const void *left, const void *right, size_t length);
size_t strlen(const char *text);
int strcmp(const char *left, const char *right);
int strncmp(const char *left, const char *right, size_t length);
char *strchr(const char *text, int character);
char *strrchr(const char *text, int character);

#endif

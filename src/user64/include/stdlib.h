#ifndef MICH64_STDLIB_H
#define MICH64_STDLIB_H

#include <stddef.h>

void *malloc(size_t size);
void free(void *pointer);
void *calloc(size_t count, size_t size);
void *realloc(void *pointer, size_t size);

#endif

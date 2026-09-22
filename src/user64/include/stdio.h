#ifndef MICH64_STDIO_H
#define MICH64_STDIO_H

#include <stddef.h>

/* The userland build is freestanding, so the va_* facility comes from
   compiler builtins instead of a hosted <stdarg.h>. */
typedef __builtin_va_list va_list;
#define va_start __builtin_va_start
#define va_end __builtin_va_end
#define va_arg __builtin_va_arg

#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

typedef struct stdio_file FILE;

int printf(const char *format, ...);
int vprintf(const char *format, va_list arguments);
int snprintf(char *buffer, size_t size, const char *format, ...);
int vsnprintf(char *buffer, size_t size, const char *format,
              va_list arguments);
int fprintf(FILE *file, const char *format, ...);
int vfprintf(FILE *file, const char *format, va_list arguments);
int puts(const char *text);
int putchar(int character);

FILE *fopen(const char *path, const char *mode);
int fclose(FILE *file);
int fflush(FILE *file);
size_t fread(void *pointer, size_t size, size_t count, FILE *file);
size_t fwrite(const void *pointer, size_t size, size_t count, FILE *file);
int fgetc(FILE *file);
int fputc(int character, FILE *file);
char *fgets(char *destination, int size, FILE *file);
int fputs(const char *text, FILE *file);
int fseek(FILE *file, long offset, int whence);
long ftell(FILE *file);
void rewind(FILE *file);
int feof(FILE *file);
int ferror(FILE *file);
void clearerr(FILE *file);

#endif

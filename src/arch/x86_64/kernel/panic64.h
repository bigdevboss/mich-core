#ifndef PANIC64_H
#define PANIC64_H

struct exception_frame64;

void panic64_halt(const char *reason) __attribute__((noreturn));
void panic64_frame(const char *reason,
                   const struct exception_frame64 *frame)
    __attribute__((noreturn));

#endif

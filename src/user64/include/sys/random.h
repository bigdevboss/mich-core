#ifndef MICH64_SYS_RANDOM_H
#define MICH64_SYS_RANDOM_H

#include <sys/types.h>

ssize_t getrandom(void *buffer, size_t length, unsigned int flags);

#endif

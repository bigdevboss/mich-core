#ifndef ARCH_CPU_H
#define ARCH_CPU_H

#include "types.h"

void arch_cpu_halt(void);
void arch_cpu_wait(void);
void arch_cpu_pause(void);
void arch_cpu_stop(void) __attribute__((noreturn));
void arch_set_address_space(paddr_t root);

#endif

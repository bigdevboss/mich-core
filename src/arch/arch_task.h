#ifndef ARCH_TASK_H
#define ARCH_TASK_H

#include "types.h"

struct task;

void arch_task_init(void);
void arch_task_activate(struct task *task);
int arch_task_allow_io(struct task *task, u32 port, u32 count);
void arch_task_release(struct task *task);

#endif

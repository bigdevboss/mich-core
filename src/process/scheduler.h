#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "types.h"

void scheduler_init_idle(paddr_t kernel_dir_phys);
reg_t schedule_c(reg_t esp_now);
int scheduler_current(void);
void scheduler_set_current(int current);
// The pick has to know the CPU that asks, because a task's on_cpu is a
// claim by one CPU and every other CPU must leave that task alone.
int scheduler_cpu_id(void);
u32 scheduler_now(void);
int scheduler_pick_next(int current);

#endif

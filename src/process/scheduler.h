#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "types.h"

void scheduler_init_idle(paddr_t kernel_dir_phys);
reg_t schedule_c(reg_t esp_now);
int scheduler_current(void);
void scheduler_set_current(int current);
void scheduler_set_this_cpu(int cpu);
u32 scheduler_now(void);
int scheduler_pick_next(int current);

#endif

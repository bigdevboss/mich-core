#ifndef SCHEDULER_H
#define SCHEDULER_H

void scheduler_init_idle(unsigned int kernel_dir_phys);
unsigned int schedule_c(unsigned int esp_now);
int scheduler_current(void);

#endif

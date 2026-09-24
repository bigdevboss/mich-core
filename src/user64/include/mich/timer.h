#ifndef MICH64_USER_TIMER_H
#define MICH64_USER_TIMER_H

unsigned int mich_ticks(void);

// Unix seconds, UTC, from the CMOS clock anchored at boot. 0 when the kernel
// could not read the clock. Never local time: the hardware carries no zone.
unsigned long long mich_wall_clock(void);
int mich_timer_create(void);
int mich_timer_arm(unsigned int handle, unsigned int delay_ticks,
                   unsigned int interval_ticks);
int mich_timer_cancel(unsigned int handle);
int mich_timer_wait(unsigned int handle);

#endif

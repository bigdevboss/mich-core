#ifndef MICH64_USER_COMPLETION_H
#define MICH64_USER_COMPLETION_H

#include <completion_abi.h>

#define mich_completion_update completion_update
#define mich_completion_poll_result completion_poll

int mich_completion_create(void);
unsigned long long mich_completion_begin(unsigned int handle,
                                         unsigned int timeout_ticks);
int mich_completion_finish(unsigned int handle,
                           struct mich_completion_update *update);
int mich_completion_cancel(unsigned int handle, unsigned long long id);
int mich_completion_poll(unsigned int handle,
                         struct mich_completion_poll_result *result);
int mich_completion_wait(unsigned int handle);

#endif

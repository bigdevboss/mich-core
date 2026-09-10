#ifndef MICH64_USER_WAIT_H
#define MICH64_USER_WAIT_H

#include <wait_many.h>

#define MICH_WAIT_MANY_MAX WAIT_MANY_MAX
#define mich_wait_many_request wait_many_request

int mich_wait_many(struct mich_wait_many_request *request);

#endif

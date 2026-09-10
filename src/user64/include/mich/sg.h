#ifndef MICH64_USER_SG_H
#define MICH64_USER_SG_H

#include <sg.h>

#define MICH_SG_ENTRY_MAX SG_USER_ENTRY_MAX
#define mich_sg_entry sg_user_entry
#define mich_sg_create_request sg_create_request

int mich_sg_create(struct mich_sg_create_request *request);
int mich_sg_revoke(unsigned int handle);

#endif

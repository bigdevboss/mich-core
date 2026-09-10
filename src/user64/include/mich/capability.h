#ifndef MICH64_USER_CAPABILITY_H
#define MICH64_USER_CAPABILITY_H

#define MICH_CAP_SERVICE_REGISTER (1u << 0)
#define MICH_CAP_IRQ (1u << 1)
#define MICH_CAP_IOPORT (1u << 2)
#define MICH_CAP_TASK_ADMIN (1u << 3)
#define MICH_CAP_TASK_ENUM (1u << 4)
#define MICH_CAP_DISPLAY_ADMIN (1u << 5)
#define MICH_CAP_RESOURCE_ADMIN (1u << 6)
#define MICH_CAP_VFS_ADMIN (1u << 7)

unsigned int mich_cap_get(void);
int mich_cap_drop(unsigned int capabilities);
int mich_cap_grant(int pid, unsigned int capabilities);

#endif

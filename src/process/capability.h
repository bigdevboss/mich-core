#ifndef CAPABILITY_H
#define CAPABILITY_H

#define CAP_SERVICE_REGISTER (1u << 0)
#define CAP_IRQ              (1u << 1)
#define CAP_IOPORT           (1u << 2)
#define CAP_TASK_ADMIN       (1u << 3)
#define CAP_TASK_ENUM        (1u << 4)
#define CAP_DISPLAY_ADMIN    (1u << 5)
#define CAP_RESOURCE_ADMIN   (1u << 6)
#define CAP_VFS_ADMIN        (1u << 7)

#define CAP_BOOT_ALLOWED (CAP_SERVICE_REGISTER | CAP_TASK_ADMIN | CAP_TASK_ENUM | \
                          CAP_DISPLAY_ADMIN | CAP_RESOURCE_ADMIN | CAP_VFS_ADMIN)

#endif

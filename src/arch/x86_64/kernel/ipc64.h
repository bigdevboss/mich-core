#ifndef IPC64_H
#define IPC64_H

#include "types.h"
#include "ipc.h"

void ipc64_init(void);
int ipc64_send(u32 destination, vaddr_t message);
int ipc64_send_timeout(u32 destination, vaddr_t message, u32 deadline);
int ipc64_send_nb(u32 destination, vaddr_t message);
int ipc64_recv(u32 sender, vaddr_t message);
void ipc64_task_died(u32 pid);
void ipc64_tick(u32 now);

#endif

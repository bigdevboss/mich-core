#ifndef IPC_H
#define IPC_H

#include "types.h"

struct message {
    u32 from_id;
    u32 type;
    u8 data[56];
};

int ipc_send(u32 dest, const struct message *msg);
int ipc_send_nb(u32 dest, const struct message *msg);
int ipc_recv(struct message *msg);
int ipc_try_deliver(u32 dest, const struct message *msg);
void ipc_flush_task(u32 id);
int ipc_recv_from(u32 expect, struct message *msg);
int ipc_send_timeout(u32 dest, const struct message *msg, u32 timeout_ticks);
void ipc_tick(u32 now);

#endif

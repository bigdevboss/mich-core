#ifndef IPC_H
#define IPC_H

struct message {
    unsigned int from_id;
    unsigned int type;
    unsigned char data[56];
};

int ipc_send(unsigned int dest, const struct message *msg);
int ipc_send_nb(unsigned int dest, const struct message *msg);
int ipc_recv(struct message *msg);
int ipc_try_deliver(unsigned int dest, const struct message *msg);
void ipc_flush_task(unsigned int id);
int ipc_recv_from(unsigned int expect, struct message *msg);

#endif

#ifndef MICH_USER_IPC_H
#define MICH_USER_IPC_H

#define MICH_MESSAGE_DATA_SIZE 56
#define MICH_MSG_DIED 60

struct mich_message {
    unsigned int from_id;
    unsigned int type;
    unsigned char data[MICH_MESSAGE_DATA_SIZE];
};

int mich_send(unsigned int destination, const struct mich_message *message);
int mich_send_nb(unsigned int destination, const struct mich_message *message);
int mich_send_timeout(unsigned int destination, const struct mich_message *message,
                      unsigned int timeout_ticks);
int mich_recv(struct mich_message *message);
int mich_recv_from(unsigned int sender, struct mich_message *message);

#endif

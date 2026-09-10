#ifndef MICH64_USER_BRIDGE_H
#define MICH64_USER_BRIDGE_H

struct mich_bridge_notification {
    unsigned int source;
    unsigned int sequence;
};

int mich_bridge_create(void);
int mich_bridge_wait(unsigned int endpoint);
int mich_bridge_read(unsigned int endpoint,
                     struct mich_bridge_notification *notification);
int mich_irq_bind(unsigned int irq, unsigned int endpoint);
int mich_irq_unbind(unsigned int irq);
int mich_irq_set_mask(unsigned int irq, int masked);
int mich_irq_wait(unsigned int endpoint);

#endif

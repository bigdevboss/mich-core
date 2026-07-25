#ifndef IRQ_H
#define IRQ_H

void irq_init(void);
int irq_register(unsigned int irq);
void irq_handler_main(unsigned int irq);
unsigned int irq_poll_pending(void);

#endif

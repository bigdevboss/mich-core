#ifndef IRQ_H
#define IRQ_H

static inline void intr_off(void) { __asm__ volatile("cli" ::: "memory"); }
static inline void intr_on(void) { __asm__ volatile("sti" ::: "memory"); }

void irq_init(void);
int irq_register(unsigned int irq);
void irq_handler_main(unsigned int irq);
unsigned int irq_poll_pending(void);

#endif

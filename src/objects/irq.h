#ifndef IRQ_H
#define IRQ_H

#include "types.h"

typedef reg_t irq_state_t;

#define IRQ_STATE_IF ((irq_state_t)1 << 9)

#include "arch_irq.h"

static inline void intr_off(void) { arch_intr_off(); }
static inline void intr_on(void) { arch_intr_on(); }
static inline irq_state_t irq_save(void) { return arch_irq_save(); }
static inline void irq_restore(irq_state_t state) { arch_irq_restore(state); }

void irq_init(void);
int irq_register(u32 irq);
void irq_release_owner(u32 full_pid);
void irq_handler_main(u32 irq);
u32 irq_poll_pending(void);

#endif

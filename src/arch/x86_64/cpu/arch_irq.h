#ifndef ARCH_IRQ_H
#define ARCH_IRQ_H

static inline void arch_intr_off(void) {
    __asm__ volatile("cli" ::: "memory");
}

static inline void arch_intr_on(void) {
    __asm__ volatile("sti" ::: "memory");
}

static inline irq_state_t arch_irq_save(void) {
    irq_state_t state;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(state) : : "memory");
    return state;
}

static inline void arch_irq_restore(irq_state_t state) {
    __asm__ volatile("pushq %0; popfq" : : "r"(state) : "memory", "cc");
}

#endif

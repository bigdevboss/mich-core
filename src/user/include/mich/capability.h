#ifndef MICH_USER_CAPABILITY_H
#define MICH_USER_CAPABILITY_H

#define MICH_CAP_SERVICE_REGISTER (1u << 0)
#define MICH_CAP_IRQ              (1u << 1)
#define MICH_CAP_IOPORT           (1u << 2)
#define MICH_CAP_TASK_ADMIN       (1u << 3)
#define MICH_CAP_TASK_ENUM        (1u << 4)
#define MICH_CAP_DISPLAY_ADMIN    (1u << 5)
#define MICH_CAP_RESOURCE_ADMIN   (1u << 6)

unsigned int mich_cap_get(void);
int mich_cap_drop(unsigned int capabilities);
int mich_cap_grant(int pid, unsigned int capabilities);
int mich_irq_register(unsigned int irq);
int mich_irq_grant(int pid, unsigned int irq);
int mich_ioport_grant(int pid, unsigned int port, unsigned int count);
int mich_mmio_grant(int pid, unsigned int phys, unsigned int length);
int mich_mmio_map(unsigned int handle, unsigned int user_va);
int mich_dma_grant(int pid, unsigned int pages, unsigned int max_addr);
int mich_dma_alloc(unsigned int pages, unsigned int user_va);

#endif

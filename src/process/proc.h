#ifndef PROC_H
#define PROC_H

int proc_fork(void);
int proc_exec(unsigned int user_path_ptr, unsigned int user_args_ptr);
int proc_exit(int code);
int proc_wait(int pid);
int proc_getpid(void);
int proc_kill(int pid);
int proc_cap_grant(int pid, unsigned int capabilities);
int proc_irq_grant(int pid, unsigned int irq);
int proc_ioport_grant(int pid, unsigned int port, unsigned int count);
int proc_mmio_grant(int pid, unsigned int phys, unsigned int length);
int proc_mmio_map(unsigned int handle, unsigned int user_va);
int proc_dma_grant(int pid, unsigned int pages, unsigned int max_addr);
int proc_dma_alloc(unsigned int pages, unsigned int user_va);
void proc_reap_orphans(int current_slot);

#endif

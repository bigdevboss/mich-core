#ifndef PROC_H
#define PROC_H

int proc_fork(void);
int proc_exec(unsigned int user_path_ptr, unsigned int user_args_ptr);
int proc_exit(int code);
int proc_wait(int pid);
int proc_getpid(void);
int proc_kill(int pid);

#endif

#ifndef MICH64_SYS_WAIT_H
#define MICH64_SYS_WAIT_H

#include <sys/types.h>

#define WNOHANG 1

/* Mich v0 has no signals, so a status word only encodes a bounded exit
   code; kill-terminated tasks surface as WIFEXITED with the kill code. */
#define WIFEXITED(status) (((status) & 0xFF) == 0)
#define WEXITSTATUS(status) (((status) >> 8) & 0xFF)

pid_t waitpid(pid_t pid, int *status, int options);

#endif

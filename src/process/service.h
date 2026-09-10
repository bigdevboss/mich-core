#ifndef SERVICE_H
#define SERVICE_H

#define SERVICE_KBD  1
#define SERVICE_ATA  2
#define SERVICE_PCI  3
#define SERVICE_FS   4
#define SERVICE_INIT 5
#define SERVICE_MAX  16

void service_init(void);
int service_register(unsigned int service);
int service_lookup(unsigned int service);
void service_release_owner(unsigned int full_pid);

#endif

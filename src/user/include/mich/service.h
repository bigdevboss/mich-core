#ifndef MICH_USER_SERVICE_H
#define MICH_USER_SERVICE_H

#define MICH_SERVICE_KBD  1
#define MICH_SERVICE_ATA  2
#define MICH_SERVICE_PCI  3
#define MICH_SERVICE_FS   4
#define MICH_SERVICE_INIT 5
#define MICH_SERVICE_TEST 15

int mich_service_register(unsigned int service);
int mich_service_lookup(unsigned int service);

#endif

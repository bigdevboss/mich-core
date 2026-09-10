#ifndef MICH64_USER_RING_H
#define MICH64_USER_RING_H

#include <ring_abi.h>

#define MICH_RING_CAPACITY_MAX RING_CAPACITY_MAX
#define MICH_RING_DESCRIPTOR_MIN RING_DESCRIPTOR_MIN
#define MICH_RING_DESCRIPTOR_MAX RING_DESCRIPTOR_MAX
#define mich_ring_shared_header ring_shared_header

int mich_ring_create(unsigned int capacity, unsigned int descriptor_size);
int mich_ring_map(unsigned int handle, unsigned long long virtual_address);
int mich_ring_submit(unsigned int handle, unsigned int count);
int mich_ring_consume(unsigned int handle, unsigned int count);
int mich_ring_revoke(unsigned int handle);

#endif

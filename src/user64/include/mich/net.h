#ifndef MICH64_USER_NET_H
#define MICH64_USER_NET_H

#include <net_abi.h>

#define mich_net_packet_descriptor net_packet_descriptor
#define mich_vnic_create_request vnic_create_request
#define mich_vnic_frame_request vnic_frame_request
#define mich_vnic_benchmark_request vnic_benchmark_request

int mich_vnic_create(struct mich_vnic_create_request *request);
int mich_packet_pool_map(unsigned int handle,
                         unsigned long long virtual_address);
int mich_vnic_inject(unsigned int handle, const void *frame,
                     unsigned int length);
int mich_vnic_receive(unsigned int handle,
                      struct mich_net_packet_descriptor *descriptor);
int mich_vnic_release_rx(unsigned int handle, unsigned long long buffer_id);
unsigned long long mich_vnic_acquire_tx(unsigned int handle);
int mich_vnic_submit_tx(unsigned int handle,
                        struct mich_vnic_frame_request *request);
int mich_vnic_drain_tx(unsigned int handle,
                       struct mich_net_packet_descriptor *descriptor);
int mich_vnic_benchmark(unsigned int handle,
                        struct mich_vnic_benchmark_request *request);
int mich_vnic_revoke(unsigned int handle);

#endif

#ifndef MICH64_USER_NET_INTERFACE_H
#define MICH64_USER_NET_INTERFACE_H

#include <net_interface_abi.h>
#include <net_abi.h>

#define mich_net_interface_create_request net_interface_create_request
#define mich_net_interface_info net_interface_info
#define mich_net_interface_buffer_request net_interface_buffer_request
#define mich_net_interface_ipv4_request net_interface_ipv4_request
#define mich_net_interface_echo_request net_interface_echo_request
#define mich_net_interface_ipv6_info net_interface_ipv6_info
#define mich_net_packet_descriptor net_packet_descriptor

int mich_net_interface_create(struct mich_net_interface_create_request *request);
int mich_net_interface_set_link(unsigned int handle, int up);
int mich_net_interface_get_info(unsigned int handle,
                                struct mich_net_interface_info *info);
int mich_net_interface_revoke(unsigned int handle);
unsigned long long mich_net_interface_driver_acquire_rx(unsigned int handle);
int mich_net_interface_driver_receive(
    unsigned int handle, struct mich_net_interface_buffer_request *request);
int mich_net_interface_driver_release_rx(
    unsigned int handle, unsigned long long buffer_id);
int mich_net_interface_driver_dequeue_tx(
    unsigned int handle, struct mich_net_packet_descriptor *descriptor);
int mich_net_interface_driver_complete_tx(
    unsigned int handle, unsigned long long buffer_id);
unsigned int mich_net_interface_driver_acquire_rx_batch(
    unsigned int handle, unsigned int count,
    unsigned long long *buffer_ids);
unsigned int mich_net_interface_driver_receive_batch(
    unsigned int handle, unsigned int count,
    struct mich_net_interface_buffer_request *requests);
unsigned int mich_net_interface_driver_dequeue_tx_batch(
    unsigned int handle, unsigned int max_count,
    struct mich_net_packet_descriptor *descriptors);
unsigned int mich_net_interface_driver_complete_tx_batch(
    unsigned int handle, unsigned int count,
    const unsigned long long *buffer_ids);
int mich_net_interface_configure_ipv4(
    unsigned int handle, struct mich_net_interface_ipv4_request *request);
int mich_net_interface_send_echo(
    unsigned int handle, struct mich_net_interface_echo_request *request);
unsigned long long mich_net_interface_echo_replies(unsigned int handle);
int mich_net_interface_send_udp_probe(unsigned int handle,
                                      unsigned int destination);
int mich_net_interface_poll_udp_probe(unsigned int handle);
int mich_net_interface_ipv6_start(unsigned int handle);
int mich_net_interface_ipv6_complete_dad(unsigned int handle);
int mich_net_interface_ipv6_get_info(
    unsigned int handle, struct mich_net_interface_ipv6_info *info);
int mich_net_interface_ipv6_send_echo(unsigned int handle);
unsigned long long mich_net_interface_ipv6_echo_replies(unsigned int handle);
int mich_net_interface_udpv6_start_probe(unsigned int handle);
int mich_net_interface_udpv6_poll_probe(unsigned int handle);
int mich_net_interface_ipv6_maintenance(unsigned int handle);
int mich_net_interface_tcp_probe_start(unsigned int handle,
                                       unsigned int destination,
                                       unsigned int port);
int mich_net_interface_tcp_probe_poll(unsigned int handle);
int mich_net_interface_tcpv6_probe_start(unsigned int handle,
                                         unsigned int port);
int mich_net_interface_tcpv6_probe_poll(unsigned int handle);

#endif

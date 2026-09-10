#ifndef MICH64_USER_SOCKET_H
#define MICH64_USER_SOCKET_H

#include <socket_abi.h>

#define MICH_SOCKET_PAYLOAD_MAX SOCKET_PAYLOAD_MAX
#define MICH_SOCKET_EPHEMERAL_FIRST SOCKET_EPHEMERAL_FIRST
#define MICH_SOCKET_EPHEMERAL_LAST SOCKET_EPHEMERAL_LAST
#define MICH_SOCKET_STREAM_PAYLOAD_MAX SOCKET_STREAM_PAYLOAD_MAX
#define mich_socket_bind_request socket_bind_request
#define mich_socket_send_request socket_send_request
#define mich_socket_receive_result socket_receive_result
#define mich_socket_ipv6_bind_request socket_ipv6_bind_request
#define mich_socket_ipv6_send_request socket_ipv6_send_request
#define mich_socket_ipv6_receive_result socket_ipv6_receive_result
#define mich_socket_stream_connect_request socket_stream_connect_request
#define mich_socket_stream_listen_request socket_stream_listen_request
#define mich_socket_stream_data socket_stream_data
#define mich_socket_stream_state_result socket_stream_state_result
#define mich_socket_stream_error_result socket_stream_error_result

int mich_socket_create(void);
int mich_socket_bind(unsigned int handle,
                     struct mich_socket_bind_request *request);
int mich_socket_send_to(unsigned int handle,
                        struct mich_socket_send_request *request);
int mich_socket_receive_from(unsigned int handle,
                             struct mich_socket_receive_result *result);
int mich_socket_wait(unsigned int handle);
int mich_socket_ipv6_create(void);
int mich_socket_ipv6_bind(unsigned int handle,
                          struct mich_socket_ipv6_bind_request *request);
int mich_socket_ipv6_send_to(unsigned int handle,
                             struct mich_socket_ipv6_send_request *request);
int mich_socket_ipv6_receive_from(
    unsigned int handle, struct mich_socket_ipv6_receive_result *result);
int mich_socket_stream_create(void);
int mich_socket_stream_connect(
    unsigned int handle, struct mich_socket_stream_connect_request *request);
int mich_socket_stream_send(unsigned int handle,
                            struct mich_socket_stream_data *data);
int mich_socket_stream_receive(unsigned int handle,
                               struct mich_socket_stream_data *data);
int mich_socket_stream_state(
    unsigned int handle, struct mich_socket_stream_state_result *result);
int mich_socket_stream_shutdown(unsigned int handle);
int mich_socket_stream_listen(
    unsigned int handle, struct mich_socket_stream_listen_request *request);
int mich_socket_stream_listen_ipv6(
    unsigned int handle, struct mich_socket_stream_listen_request *request);
int mich_socket_stream_accept(unsigned int handle);
int mich_socket_stream_take_error(
    unsigned int handle, struct mich_socket_stream_error_result *result);

#endif

#ifndef SOCKET_ABI_H
#define SOCKET_ABI_H

#include "types.h"

#define SOCKET_PAYLOAD_MAX 1472
#define SOCKET_EPHEMERAL_FIRST 49152
#define SOCKET_EPHEMERAL_LAST 65535
#define SOCKET_STREAM_PAYLOAD_MAX 512
#define SOCKET_READY_CONNECTED (1u << 0)
#define SOCKET_READY_READABLE (1u << 1)
#define SOCKET_READY_WRITABLE (1u << 2)
#define SOCKET_READY_ACCEPT (1u << 3)
#define SOCKET_READY_HANGUP (1u << 4)
#define SOCKET_READY_ERROR (1u << 5)

struct socket_bind_request {
    u32 address;
    u16 port;
    u16 reserved;
};

struct socket_send_request {
    u32 destination_address;
    u16 destination_port;
    u16 length;
    u8 payload[SOCKET_PAYLOAD_MAX];
};

struct socket_ipv6_bind_request {
    u8 address[16];
    u16 port;
    u16 reserved;
};

struct socket_ipv6_send_request {
    u8 destination_address[16];
    u16 destination_port;
    u16 length;
    u8 payload[SOCKET_PAYLOAD_MAX];
};

struct socket_ipv6_receive_result {
    u8 source_address[16];
    u8 destination_address[16];
    u16 source_port;
    u16 destination_port;
    u16 length;
    u16 reserved;
    u8 payload[SOCKET_PAYLOAD_MAX];
};

struct socket_stream_connect_request {
    u32 interface_handle;
    u32 destination_address;
    u16 destination_port;
    u16 reserved;
};

struct socket_stream_listen_request {
    u32 interface_handle;
    u16 local_port;
    u16 backlog;
};

struct socket_stream_data {
    u32 length;
    u32 reserved;
    u8 data[SOCKET_STREAM_PAYLOAD_MAX];
};

struct socket_stream_state_result {
    u32 state;
    u32 readiness;
    i32 error;
    u32 eof;
};

struct socket_stream_error_result {
    i32 error;
    u32 reserved;
};

struct socket_receive_result {
    u32 source_address;
    u32 destination_address;
    u16 source_port;
    u16 destination_port;
    u16 length;
    u16 reserved;
    u8 payload[SOCKET_PAYLOAD_MAX];
};

#endif

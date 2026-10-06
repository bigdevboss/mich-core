#ifndef SOCKET_H
#define SOCKET_H

#include "types.h"
#include "object.h"
#include "udp.h"
#include "udpv6.h"
#include "tcp.h"
#include "route.h"

#define SOCKET_MAX 32
#define SOCKET_UDP_CONTEXT_MAX 17

int socket_init(struct udp_context *udp, struct route_table *routes);
int socket_register_udp(struct udp_context *udp, u32 interface_id,
                        u32 interface_generation);
void socket_unregister_udp(struct udp_context *udp);
int socket_register_udpv6(struct udpv6_context *udp, u32 interface_id,
                          u32 interface_generation);
void socket_unregister_udpv6(struct udpv6_context *udp);
struct kernel_object *socket_create(void);
struct kernel_object *socket_create_ipv6(void);
struct kernel_object *socket_create_stream(void);
int socket_stream_connect(struct kernel_object *socket,
                          struct kernel_object *interface,
                          u32 destination, u16 port);
int socket_stream_listen(struct kernel_object *socket,
                         struct kernel_object *interface,
                         u16 port, u32 backlog);
int socket_stream_listen_ipv6(struct kernel_object *socket,
                              struct kernel_object *interface,
                              u16 port, u32 backlog);
struct kernel_object *socket_stream_accept(struct kernel_object *socket);
int socket_stream_send(struct kernel_object *socket,
                       const void *data, u32 length);
int socket_stream_send_file(struct kernel_object *socket,
                            struct kernel_object *node, u32 offset,
                            u32 length);
int socket_stream_receive_file(struct kernel_object *socket,
                               struct kernel_object *node, u32 offset,
                               u32 length);
int socket_stream_receive(struct kernel_object *socket,
                          void *data, u32 capacity, u32 *received);
int socket_stream_shutdown(struct kernel_object *socket);
int socket_stream_state(struct kernel_object *socket,
                        u32 *state, u32 *readiness,
                        i32 *error, u32 *eof, u32 *granted_bytes);
int socket_stream_take_error(struct kernel_object *socket, i32 *error);
void socket_tcp_notify(struct tcp_context *tcp, u64 connection_id);
void socket_tcp_abort_context(struct tcp_context *tcp, i32 error);
int socket_bind(struct kernel_object *socket, u32 address, u16 port);
int socket_bind_ipv6(struct kernel_object *socket,
                     const u8 address[IPV6_ADDRESS_SIZE], u16 port);
int socket_send_to(struct kernel_object *socket, u32 address, u16 port,
                   const void *payload, u32 length);
int socket_receive_from(struct kernel_object *socket,
                        struct udp_datagram *datagram);
int socket_send_to_ipv6(struct kernel_object *socket,
                        const u8 address[IPV6_ADDRESS_SIZE], u16 port,
                        const void *payload, u32 length);
int socket_receive_from_ipv6(struct kernel_object *socket,
                             struct udpv6_datagram *datagram);
int socket_local_address(struct kernel_object *socket,
                         u32 *address, u16 *port);
struct kernel_object *socket_wait_event(struct kernel_object *socket);
int socket_stream_take_notify(struct kernel_object *socket);
u32 socket_active_count(void);

// Resolve the interface object a route through the given address leaves
// by. The answer is borrowed registry state; stream listen takes its own
// reference the way connect does.
struct kernel_object *socket_interface_for(u32 address);

// The posix layer registers a wake hook so a state change on a socket can
// complete a parked application waiter; the net layer stays free of any
// process knowledge. The index is the socket slot, zero based, and the
// answer counts the waiters the change reached, so a socket nobody was
// parked on can keep the fact for the park that is about to arrive.
typedef u32 (*socket_wake_hook)(u32 index);
void socket_set_wake_hook(socket_wake_hook hook);

// A datagram landing on a binding wakes the application waiters parked on
// sockets bound to it; the udp layer calls this beside its event signal.
void socket_udp_notify(struct udp_context *udp, u64 binding_id);

#endif

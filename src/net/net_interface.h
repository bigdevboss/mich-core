#ifndef NET_INTERFACE_H
#define NET_INTERFACE_H

#include "types.h"
#include "object.h"
#include "driver_supervisor.h"
#include "route.h"
#include "ethernet.h"
#include "arp.h"
#include "net_abi.h"
#include "ipv4.h"
#include "icmp.h"
#include "udp.h"
#include "ipv6.h"
#include "icmpv6.h"
#include "udpv6.h"
#include "tcp.h"
#include "pmtu.h"
#include "net_interface_abi.h"

#define NET_INTERFACE_PENDING_MAX 16

#define NET_INTERFACE_BATCH_MAX 8
// How often (ms) the per-frame RX path is allowed to run the ARP cache sweep.
// The sweep is periodic housekeeping; running it on every received frame was
// pure overhead (a full ARP_CACHE_MAX scan per packet).
#define NET_INTERFACE_ARP_RX_TICK_MS 1000

#define NET_INTERFACE_MAX 16
#define NET_INTERFACE_NAME_MAX 16
#define NET_INTERFACE_TCP_MAX 4

#define NET_INTERFACE_CREATED 0
#define NET_INTERFACE_DOWN 1
#define NET_INTERFACE_UP 2
#define NET_INTERFACE_QUIESCING 3
#define NET_INTERFACE_REVOKED 4
#define NET_INTERFACE_REMOVED 5

#define NET_INTERFACE_IPV6_DISABLED 0
#define NET_INTERFACE_IPV6_TENTATIVE 1
#define NET_INTERFACE_IPV6_LINK_LOCAL 2
#define NET_INTERFACE_IPV6_SLAAC 3

struct net_interface_pending {
    u64 buffer_id;
    u32 destination;
    u32 next_hop;
    u32 expires;
    u16 offset;
    u16 length;
    u32 active;
};

struct net_interface_stats {
    u64 rx_packets;
    u64 rx_bytes;
    u64 rx_drops;
    u64 tx_packets;
    u64 tx_bytes;
    u64 tx_drops;
    u64 arp_queued;
    u64 arp_flushed;
    u64 arp_timeouts;
    u64 mtu_drops;
};

struct net_interface {
    struct kernel_object *pool;
    struct kernel_object *rx_ring;
    struct kernel_object *tx_ring;
    struct kernel_object *event;
    struct driver_domain *owner;
    struct kernel_object *self;
    struct ethernet_port ethernet;
    struct arp_context arp;
    struct ipv4_context ipv4;
    struct icmp_context icmp;
    struct udp_context udp;
    struct tcp_context *tcp;
    struct ipv6_context ipv6;
    struct icmpv6_context icmpv6;
    struct udpv6_context udpv6;
    struct pmtu_cache pmtu;
    struct net_interface_pending pending[NET_INTERFACE_PENDING_MAX];
    u32 pending_count;
    u32 now;
    // Last time the per-frame RX path ran the ARP cache sweep. The sweep is
    // periodic housekeeping (expire stale cache entries), not per-packet work,
    // so the RX path throttles it to once per NET_INTERFACE_ARP_RX_TICK_MS.
    // net_interface_tick() still runs it unconditionally on the timer path.
    u32 last_arp_tick;
    u32 arp_ready;
    u32 owner_domain_id;
    u32 interface_id;
    u32 generation;
    u32 state;
    u32 mtu;
    u32 ipv4_address;
    u32 ipv4_netmask;
    u32 ipv4_gateway;
    u32 ipv4_ready;
    u32 udp_ready;
    u32 udp_probe_complete;
    u64 udp_probe_binding;
    u64 tcp_probe_connection;
    u32 tcp_slot;
    u32 tcp_probe_sent;
    u32 tcp_probe_complete;
    u64 tcpv6_probe_connection;
    u32 tcpv6_probe_sent;
    u32 tcpv6_probe_complete;
    u8 ipv6_link_local[IPV6_ADDRESS_SIZE];
    u8 ipv6_global[IPV6_ADDRESS_SIZE];
    u8 ipv6_router[IPV6_ADDRESS_SIZE];
    u32 ipv6_state;
    u32 ipv6_deprecated;
    u32 ipv6_rs_retries;
    u32 ipv6_rs_deadline;
    u32 ipv6_echo_replies;
    u64 udpv6_probe_binding;
    u32 udpv6_probe_complete;
    u32 registered;
    u8 mac[6];
    char name[NET_INTERFACE_NAME_MAX];
    struct net_interface_stats stats;
    u32 active;
};

int net_interface_init(struct route_table *routes);
void net_interface_set_entropy(u64 entropy);
struct kernel_object *net_interface_create(
    struct driver_domain *owner, struct kernel_object *pool,
    struct kernel_object *rx_ring, struct kernel_object *tx_ring,
    const u8 mac[6], u32 mtu, const char *name);
int net_interface_register(struct kernel_object *object);
int net_interface_set_link(struct kernel_object *object,
                           struct driver_domain *owner, int up);
int net_interface_set_ipv4(struct kernel_object *object,
                           struct driver_domain *owner,
                           u32 address, u32 netmask);
int net_interface_configure_ipv4(struct kernel_object *object,
                                 struct driver_domain *owner,
                                 u32 address, u32 netmask, u32 gateway);
int net_interface_send_echo(struct kernel_object *object,
                            struct driver_domain *owner, u32 destination,
                            u16 identifier, u16 sequence);
u64 net_interface_echo_replies(struct kernel_object *object,
                               struct driver_domain *owner);
int net_interface_send_udp_probe(struct kernel_object *object,
                                 struct driver_domain *owner,
                                 u32 destination);
int net_interface_poll_udp_probe(struct kernel_object *object,
                                 struct driver_domain *owner);
int net_interface_tcp_probe_start(struct kernel_object *object,
                                  struct driver_domain *owner,
                                  u32 destination, u16 port);
int net_interface_tcp_probe_poll(struct kernel_object *object,
                                 struct driver_domain *owner);
int net_interface_tcpv6_probe_start(struct kernel_object *object,
                                    struct driver_domain *owner, u16 port);
int net_interface_tcpv6_probe_poll(struct kernel_object *object,
                                   struct driver_domain *owner);
int net_interface_tcp_connect(struct kernel_object *object,
                              u32 destination, u16 port,
                              struct tcp_context **tcp, u64 *connection);
int net_interface_tcp_listen(struct kernel_object *object, u16 port,
                             struct tcp_context **tcp, u64 *listener);
int net_interface_tcp_listen_ipv6(struct kernel_object *object, u16 port,
                                  struct tcp_context **tcp, u64 *listener);
int net_interface_tcp_accept(struct kernel_object *object, u64 listener,
                             u64 *connection);
int net_interface_tcp_send(struct kernel_object *object, u64 connection,
                           const void *data, u32 length);
int net_interface_tcp_send_pages(struct kernel_object *object,
                                  u64 connection,
                                  struct kernel_object *pages, u32 offset,
                                  u32 length);
int net_interface_tcp_receive(struct kernel_object *object, u64 connection,
                              void *data, u32 capacity, u32 *received);
int net_interface_tcp_receive_pages(struct kernel_object *object,
                                    u64 connection,
                                    struct kernel_object *pages,
                                    u32 offset, u32 length);
int net_interface_tcp_shutdown(struct kernel_object *object, u64 connection);
int net_interface_tcp_state(struct kernel_object *object, u64 connection,
                            u32 *state);
int net_interface_tcp_take_error(struct kernel_object *object,
                                 u64 connection, i32 *error);
int net_interface_tcp_close(struct kernel_object *object, u64 connection);
int net_interface_ipv6_start(struct kernel_object *object,
                             struct driver_domain *owner);
int net_interface_ipv6_complete_dad(struct kernel_object *object,
                                    struct driver_domain *owner);
int net_interface_ipv6_send_echo(struct kernel_object *object,
                                 struct driver_domain *owner);
u64 net_interface_ipv6_echo_replies(struct kernel_object *object,
                                    struct driver_domain *owner);
int net_interface_udpv6_start_probe(struct kernel_object *object,
                                    struct driver_domain *owner);
int net_interface_udpv6_poll_probe(struct kernel_object *object,
                                   struct driver_domain *owner);
int net_interface_ipv6_info(struct kernel_object *object,
                            struct driver_domain *owner,
                            u32 *state, u32 *duplicate, u32 *deprecated,
                            u8 link_local[IPV6_ADDRESS_SIZE],
                            u8 global[IPV6_ADDRESS_SIZE],
                            u8 router[IPV6_ADDRESS_SIZE]);
u64 net_interface_acquire_tx(struct kernel_object *object);
void *net_interface_packet_data(struct kernel_object *object, u64 buffer_id);
int net_interface_send_ipv4(struct kernel_object *object, u64 buffer_id,
                            u32 offset, u32 length, u32 destination,
                            u32 next_hop, u32 now);
int net_interface_route_send(struct route_table *routes, u64 buffer_id,
                             u32 offset, u32 length,
                             u32 destination, u32 now);
int net_interface_receive_frame(struct kernel_object *object,
                                const void *frame, u32 length, u32 now);
u64 net_interface_driver_acquire_rx(struct kernel_object *object,
                                    struct driver_domain *owner);
int net_interface_driver_receive(struct kernel_object *object,
                                 struct driver_domain *owner, u64 buffer_id,
                                 u32 offset, u32 length, u32 now);
int net_interface_driver_release_rx(struct kernel_object *object,
                                    struct driver_domain *owner,
                                    u64 buffer_id);
int net_interface_driver_dequeue_tx(struct kernel_object *object,
                                    struct driver_domain *owner,
                                    struct net_packet_descriptor *descriptor);
u32 net_interface_driver_acquire_rx_batch(struct kernel_object *object,
        struct driver_domain *owner, u32 count, u64 *buffer_ids);
u32 net_interface_driver_receive_batch(struct kernel_object *object,
        struct driver_domain *owner,
        const struct net_interface_buffer_request *requests, u32 count,
        u32 now);
u32 net_interface_driver_dequeue_tx_batch(struct kernel_object *object,
        struct driver_domain *owner,
        struct net_packet_descriptor *descriptors, u32 max_count);
u32 net_interface_driver_complete_tx_batch(struct kernel_object *object,
        struct driver_domain *owner, const u64 *buffer_ids, u32 count);
int net_interface_driver_complete_tx(struct kernel_object *object,
                                     struct driver_domain *owner,
                                     u64 buffer_id);
struct kernel_object *net_interface_pool(struct kernel_object *object,
                                         struct driver_domain *owner);
void net_interface_tick(struct kernel_object *object, u32 now);
int net_interface_revoke(struct kernel_object *object);
int net_interface_remove(struct kernel_object *object);
struct net_interface *net_interface_get(const struct kernel_object *object);
struct kernel_object *net_interface_lookup(u32 interface_id, u32 generation);
struct kernel_object *net_interface_wait_event(struct kernel_object *object);
void net_interface_task_died(int pid);
u32 net_interface_active_count(void);

#endif

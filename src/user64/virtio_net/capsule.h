#ifndef VIRTIO_NET_CAPSULE_H
#define VIRTIO_NET_CAPSULE_H

#include <virtio/virtio.h>

#define VIRTIO_NET_STATE_CREATED 0
#define VIRTIO_NET_STATE_BOOTSTRAPPED 1
#define VIRTIO_NET_STATE_FEATURES 2
#define VIRTIO_NET_STATE_CONFIG 3
#define VIRTIO_NET_STATE_QUEUES 4
#define VIRTIO_NET_STATE_READY 5
#define VIRTIO_NET_STATE_RUNNING 6

// Fixed guest virtual addresses in the driver mapping window. The transport maps
// device BARs into slots at the base (six 0x100000 strides up to 0x110600000), so
// the ring DMA and the packet pool sit above that, none overlapping.
#define VIRTIO_NET_BAR_WINDOW 0x110000000ULL
#define VIRTIO_NET_VRING_ADDRESS 0x110800000ULL
#define VIRTIO_NET_POOL_ADDRESS 0x110900000ULL

// The split ring for queue-size 128 fits in one page; two pages per queue leave
// room to grow to 256, and RX then TX sit back to back in one DMA grant.
#define VIRTIO_NET_QUEUE_SIZE 128u
#define VIRTIO_NET_VRING_PAGES 2u

// MSI-X table entries the manifest grants (config 0, RX 1, TX 2); the capsule
// points each queue at its entry now that no in-kernel transport does.
#define VIRTIO_NET_RX_MSIX_ENTRY 1u
#define VIRTIO_NET_TX_MSIX_ENTRY 2u

// virtio spec 5.1.3 feature bits this capsule acts on.
#define VIRTIO_NET_FEATURE_MAC (1ULL << 5)
#define VIRTIO_NET_FEATURE_STATUS (1ULL << 16)

#define VIRTIO_NET_HEADER_SIZE 12
#define VIRTIO_NET_RX_POSTED 16
#define VIRTIO_NET_TX_OUTSTANDING 16
#define VIRTIO_NET_BATCH_BUDGET 8
#define VIRTIO_NET_BATCH_MIN 4
#define VIRTIO_NET_BATCH_MAX 16
#define VIRTIO_NET_DRIVER_BATCH 8
#define VIRTIO_NET_IWR_WAKE_CAP 32
#define VIRTIO_NET_PASSIVE_PORT 8082
#define DHCP_STATE_SELECTING 0
#define DHCP_STATE_REQUESTING 1
#define DHCP_STATE_BOUND 2
#define DHCP_STATE_RENEWING 3
#define DHCP_STATE_REBINDING 4
#define DHCP_RETRY_MAX 5
#define DHCP_TICKS_PER_SECOND 100

struct virtio_net_capsule {
    unsigned int state;
    unsigned int pci_handle;
    unsigned int dma_handle;
    unsigned long long dma_physical;
    struct virtio_device device;
    struct virtqueue rx_queue;
    struct virtqueue tx_queue;
    unsigned int vnic_handle;
    unsigned int pool_handle;
    unsigned int interface_handle;
    unsigned int bridge_handle;
    unsigned int config_irq_handle;
    unsigned int rx_irq_handle;
    unsigned int tx_irq_handle;
    unsigned int interrupt_seen;
    unsigned int timer_handle;
    unsigned int ipv6_timer_handle;
    unsigned int ipv6_started;
    unsigned int ipv6_dad_complete;
    unsigned int ipv6_slaac_ready;
    unsigned int ipv6_ping_sent;
    unsigned int ipv6_ping_complete;
    unsigned int ipv6_ping_retries;
    unsigned int ipv6_ping_retry_at;
    unsigned int tcpv6_probe_started;
    unsigned int tcpv6_probe_complete;
    unsigned int udpv6_probe_sent;
    unsigned int udpv6_probe_complete;
    unsigned int udpv6_probe_retries;
    unsigned int udpv6_probe_retry_at;
    unsigned int socket6_handle;
    unsigned int socket6_sent;
    unsigned int dhcp_state;
    unsigned int dhcp_retries;
    unsigned int dhcp_xid;
    unsigned int dhcp_lease_seconds;
    unsigned int dhcp_t1_seconds;
    unsigned int dhcp_t2_seconds;
    unsigned int restart_requested;
    unsigned int config_generation;
    unsigned int external_probe_enabled;
    unsigned int restart_test_enabled;
    unsigned int timing_enabled;
    unsigned int circuit_test_enabled;
    unsigned int recovery_test_enabled;
    unsigned int restart_count;
    unsigned int restart_test_complete;
    unsigned int rx_seen;
    unsigned int rx_packets;
    unsigned int rx_drops;
    unsigned int tx_packets;
    unsigned int dhcp_offer_seen;
    unsigned int dhcp_request_sent;
    unsigned int dhcp_ack_seen;
    unsigned int ipv4_configured;
    unsigned int ping_sent;
    unsigned int ping_complete;
    unsigned int udp_probe_sent;
    unsigned int udp_probe_complete;
    unsigned int socket_handle;
    unsigned int socket_udp_sent;
    unsigned int socket_udp_complete;
    unsigned int tcp_probe_started;
    unsigned int tcp_probe_complete;
    unsigned int stream_handle;
    unsigned int stream_sent;
    unsigned int stream_complete;
    unsigned int stream_shutdown;
    unsigned int stream_closed;
    unsigned int stream_rounds;
    unsigned int stream_received;
    unsigned int readiness_reported;
    unsigned int listener_handle;
    unsigned int listener_timer_snapshot_pending;
    unsigned int listener_snapshot_due;
    unsigned int listener_snapshot_reported;
    unsigned int listener_rx_packets;
    unsigned int listener_rx_drops;
    unsigned int passive_ingress_syn_seen;
    unsigned int passive_ingress_ack_seen;
    unsigned int passive_ingress_data_seen;
    unsigned int accepted_handle;
    unsigned int passive_received;
    unsigned int passive_receive_polled;
    unsigned int passive_wait_reported;
    unsigned int passive_wake_reported;
    unsigned int passive_empty_snapshot_reported;
    unsigned int passive_wake_snapshot_reported;
    unsigned int passive_rx_packets;
    unsigned int passive_rx_drops;
    unsigned int passive_closed;
    unsigned int offered_address;
    unsigned int offered_netmask;
    unsigned int offered_gateway;
    unsigned int offered_dns;
    unsigned int dhcp_server;
    unsigned long long negotiated_features;
    unsigned short link_status;
    unsigned char mac[6];
    unsigned long long rx_tokens[VIRTIO_NET_RX_POSTED];
    unsigned long long rx_buffers[VIRTIO_NET_RX_POSTED];
    unsigned long long tx_tokens[VIRTIO_NET_TX_OUTSTANDING];
    unsigned long long tx_buffers[VIRTIO_NET_TX_OUTSTANDING];
    unsigned int tx_outstanding;
    unsigned long long rx_batch_cycles;
    unsigned long long tx_batch_cycles;
    unsigned int batch_reported;
    unsigned int tx_batch_reported;
    unsigned int itr_budget_rx;
    unsigned int itr_budget_tx;
    unsigned int itr_packets_masked;
    unsigned int itr_adapt_up;
    unsigned int itr_adapt_down;
    unsigned int itr_reported;
};

// QEMU integration support. Not virtio device work.
void virtio_net_timing_mark(const struct virtio_net_capsule *capsule,
                            const char *phase);
void probes_on_ipv4_up(struct virtio_net_capsule *capsule);
void probes_on_slaac(struct virtio_net_capsule *capsule);
int probes_poll(struct virtio_net_capsule *capsule);
int probes_external_complete(const struct virtio_net_capsule *capsule);

#endif

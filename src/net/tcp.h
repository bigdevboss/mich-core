#ifndef TCP_H
#define TCP_H

#include "types.h"

#define TCP_HEADER_MIN 20
#define TCP_OPTION_MAX 40
// 32 connections * 16 KiB * 2 plus MSS-sized retransmit/OOO slots. 64
// connections at this window would blow the 63 MiB kernel span once the
// test binary's many static tcp_context copies are included.
#define TCP_CONNECTION_MAX 32
#define TCP_SEND_BUFFER_MAX 16384
#define TCP_RECEIVE_BUFFER_MAX 16384
#define TCP_RETRANSMISSION_MAX 8
#define TCP_RETRANSMIT_DATA_MAX 1460
#define TCP_OUT_OF_ORDER_MAX 8
#define TCP_RTO_INITIAL 100
#define TCP_RTO_MAX 6400
#define TCP_RETRY_MAX 5
#define TCP_TIME_WAIT_TICKS 6000
#define TCP_DEADLINE_MAX 1024
#define TCP_MSS_DEFAULT 1460
#define TCP_ACK_FILTER_INTERVAL_MIN TCP_RTO_INITIAL
#define TCP_PMTU_HEADER4 40
#define TCP_PMTU_HEADER6 60
#define TCP_PMTU_IPV4_MIN 576
#define TCP_PMTU_IPV6_MIN 1280
#define TCP_PMTU_PLATEAU 1280
#define TCP_PMTU_BLACKHOLE_RTOS 2

#define TCP_FLAG_FIN 0x01
#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_RST 0x04
#define TCP_FLAG_PSH 0x08
#define TCP_FLAG_ACK 0x10
#define TCP_FLAG_URG 0x20
#define TCP_FLAG_ECE 0x40
#define TCP_FLAG_CWR 0x80

#define TCP_STATE_CLOSED 0
#define TCP_STATE_LISTEN 1
#define TCP_STATE_SYN_SENT 2
#define TCP_STATE_SYN_RECEIVED 3
#define TCP_STATE_ESTABLISHED 4
#define TCP_STATE_FIN_WAIT_1 5
#define TCP_STATE_FIN_WAIT_2 6
#define TCP_STATE_CLOSE_WAIT 7
#define TCP_STATE_LAST_ACK 8
#define TCP_STATE_TIME_WAIT 9
#define TCP_STATE_CLOSING 10

// Deadline-heap event kinds.
#define TCP_TIMER_RETRANSMIT 0
#define TCP_TIMER_PERSIST 1
#define TCP_TIMER_TIME_WAIT 2
#define TCP_TIMER_ACK_FILTER 3

// BBR phase machine (see docs/phase3-tcp-modernization.md).
#define TCP_BBR_STARTUP 0
#define TCP_BBR_DRAIN 1
#define TCP_BBR_PROBE_BW 2
#define TCP_BBR_PROBE_RTT 3
#define TCP_BBR_WINDOW 10000
#define TCP_BBR_PROBE_RTT_DURATION 200

struct tcp_segment_view {
    const u8 *segment;
    const u8 *payload;
    u32 segment_length;
    u32 header_length;
    u32 payload_length;
    u32 sequence;
    u32 acknowledgement;
    u16 source_port;
    u16 destination_port;
    u16 window;
    u16 urgent_pointer;
    u16 mss;
    u32 timestamp_value;
    u32 timestamp_echo;
    u8 flags;
    u8 window_scale;
    u8 sack_permitted;
    u8 timestamp_present;
    u8 sack_block_count;
    u32 sack_blocks[2][2];
};

struct tcp_retransmission {
    u32 sequence;
    u32 end_sequence;
    u32 deadline;
    u32 sent_at;
    u16 length;
    u8 flags;
    u8 retries;
    u8 retransmitted;
    u8 fast_retransmit;
    u8 reserved[2];
    u8 data[TCP_RETRANSMIT_DATA_MAX];
    u32 active;
};

struct tcp_out_of_order {
    u32 sequence;
    u16 length;
    u8 flags;
    u8 data[TCP_RETRANSMIT_DATA_MAX];
    u32 active;
};

//
// Pluggable congestion control (FreeBSD cc_* kld pattern). The connection
// owns the state; the ops table owns the policy. on_ack/on_rtt receive the
// post-event flight size; on_rto runs before the retransmit is emitted.
//
struct tcp_connection;

struct tcp_cc_ops {
    const char *name;
    void (*init)(struct tcp_connection *c);
    void (*on_ack)(struct tcp_connection *c, u32 acked_bytes,
                   u32 flight, u32 now);
    void (*on_rtt)(struct tcp_connection *c, u32 sample,
                   u32 flight, u32 now);
    void (*on_fast_retransmit)(struct tcp_connection *c, u32 duplicate_acks);
    void (*on_rto)(struct tcp_connection *c, u32 now);
};

struct tcp_connection {
    u32 generation;
    u32 family;
    u32 local_address;
    u32 remote_address;
    u8 local_address6[16];
    u8 remote_address6[16];
    u32 send_unacknowledged;
    u32 send_next;
    u32 receive_next;
    u16 local_port;
    u16 remote_port;
    u16 receive_window;
    u32 send_window;
    u16 remote_mss;
    u32 congestion_window;
    u32 slow_start_threshold;
    u32 retransmission_timeout;
    u32 smoothed_rtt;
    u32 rtt_variance;
    u32 duplicate_acks;
    u32 persist_deadline;
    u32 persist_interval;
    u32 time_wait_deadline;
    i32 error;
    u32 eof;
    u32 send_fin;
    u64 parent_listener;
    u32 listen_backlog;
    u32 passive;
    u32 accepted;
    u32 detached;
    u32 send_buffer_length;
    u32 send_buffer_offset;
    u32 receive_buffer_length;
    u32 timer_version;
    u32 peer_sack;
    u32 bbr_phase;
    u32 bbr_min_rtt;
    u32 bbr_min_rtt_deadline;
    u32 bbr_bw;
    u32 bbr_bw_window_end;
    u32 bbr_delivered;
    u32 bbr_interval_start;
    u32 bbr_probe_count;
    u32 bbr_probe_rtt_start;
    u32 bbr_startup_rounds;
    u32 bbr_round;
    u32 ack_filter_until;
    u32 ack_pending;
    u32 pmtu_blackhole_rtos;
    u8 send_buffer[TCP_SEND_BUFFER_MAX];
    u8 receive_buffer[TCP_RECEIVE_BUFFER_MAX];
    struct tcp_retransmission retransmissions[TCP_RETRANSMISSION_MAX];
    struct tcp_out_of_order out_of_order[TCP_OUT_OF_ORDER_MAX];
    u32 state;
    u32 active;
};

struct tcp_response {
    u64 connection_id;
    u32 sequence;
    u32 acknowledgement;
    u16 source_port;
    u16 destination_port;
    u16 window;
    u8 flags;
    u8 valid;
    u8 options[12];
    u16 option_length;
    u8 sack[20];
    u16 sack_length;
};

struct tcp_stats {
    u64 segments_received;
    u64 segments_sent;
    u64 connections_opened;
    u64 connections_closed;
    u64 malformed;
    u64 checksum_errors;
    u64 sequence_errors;
    u64 reset_received;
    u64 retransmissions;
    u64 retransmission_failures;
    u64 duplicate_acknowledgements;
    u64 out_of_order_queued;
    u64 out_of_order_dropped;
    u64 bytes_sent;
    u64 bytes_received;
    u64 fast_retransmits;
    u64 persist_probes;
    u64 syn_drops;
    u64 time_wait_expired;
    u64 sack_blocks_sent;
    u64 sack_blocks_received;
    u64 sack_retired_segments;
    u64 sack_fast_retransmits;
    u64 sack_duplicate_acks;
    u64 acks_sent;
    u64 acks_filtered;
    u64 cc_recovery_events;
    u64 pmtu_blackholes;
};

struct tcp_transmit {
    u64 connection_id;
    u32 family;
    u32 source_address;
    u32 destination_address;
    u8 source_address6[16];
    u8 destination_address6[16];
    u32 sequence;
    u32 acknowledgement;
    u16 source_port;
    u16 destination_port;
    u16 window;
    u16 length;
    u8 flags;
    u8 retransmission;
    u8 options[TCP_OPTION_MAX];
    u16 option_length;
    u8 data[TCP_RETRANSMIT_DATA_MAX];
};

// One pending timer in the global deadline min-heap.
struct tcp_deadline {
    u32 deadline;
    u32 slot;
    u32 kind;
    u32 entry;
    u32 version;
};

struct tcp_context {
    struct tcp_connection connections[TCP_CONNECTION_MAX];
    u64 sequence_key0;
    u64 sequence_key1;
    u32 next_sequence;
    u32 now;
    struct tcp_stats stats;
    const struct tcp_cc_ops *cc;
    u32 mss;
    u32 ack_filter;
    u32 timer_epoch;
    u32 deadline_count;
    struct tcp_deadline deadlines[TCP_DEADLINE_MAX];
    void (*pmtu_blackhole)(u32 family, u32 address4,
                           const u8 address6[16], u32 ip_mtu,
                           void *context);
    void *pmtu_blackhole_context;
};

u16 tcp_checksum_ipv4(u32 source, u32 destination,
                      const void *segment, u32 length);
u16 tcp_checksum_ipv6(const u8 source[16], const u8 destination[16],
                      const void *segment, u32 length);
int tcp_parse(const void *data, u32 length, struct tcp_segment_view *segment);
int tcp_build_ipv4(void *buffer, u32 buffer_length,
                   u32 source_address, u32 destination_address,
                   u16 source_port, u16 destination_port,
                   u32 sequence, u32 acknowledgement,
                   u8 flags, u16 window,
                   const void *payload, u32 payload_length);
int tcp_build_ipv6(void *buffer, u32 buffer_length,
                   const u8 source_address[16],
                   const u8 destination_address[16],
                   u16 source_port, u16 destination_port,
                   u32 sequence, u32 acknowledgement,
                   u8 flags, u16 window,
                   const void *payload, u32 payload_length);
int tcp_build_ipv4_opts(void *buffer, u32 buffer_length,
                        u32 source_address, u32 destination_address,
                        u16 source_port, u16 destination_port,
                        u32 sequence, u32 acknowledgement,
                        u8 flags, u16 window,
                        const void *options, u32 option_length,
                        const void *payload, u32 payload_length);
int tcp_build_ipv6_opts(void *buffer, u32 buffer_length,
                        const u8 source_address[16],
                        const u8 destination_address[16],
                        u16 source_port, u16 destination_port,
                        u32 sequence, u32 acknowledgement,
                        u8 flags, u16 window,
                        const void *options, u32 option_length,
                        const void *payload, u32 payload_length);
void tcp_init(struct tcp_context *tcp, u64 sequence_seed);
u64 tcp_listen(struct tcp_context *tcp, u32 local_address, u16 local_port);
u64 tcp_listen_ipv6(struct tcp_context *tcp,
                    const u8 local_address[16], u16 local_port);
int tcp_set_listener_backlog(struct tcp_context *tcp, u64 listener_id,
                             u32 backlog);
u64 tcp_active_open(struct tcp_context *tcp,
                    u32 local_address, u16 local_port,
                    u32 remote_address, u16 remote_port);
u64 tcp_active_open_ipv6(struct tcp_context *tcp,
                         const u8 local_address[16], u16 local_port,
                         const u8 remote_address[16], u16 remote_port);
int tcp_accept(struct tcp_context *tcp, u64 listener_id,
               u64 *connection_id);
int tcp_accept_pending(struct tcp_context *tcp, u64 listener_id);
int tcp_receive_ipv4(struct tcp_context *tcp,
                     u32 source_address, u32 destination_address,
                     const void *data, u32 length,
                     struct tcp_response *response);
int tcp_receive_ipv6(struct tcp_context *tcp,
                     const u8 source_address[16],
                     const u8 destination_address[16],
                     const void *data, u32 length,
                     struct tcp_response *response);
int tcp_connection_state(struct tcp_context *tcp, u64 id, u32 *state);
int tcp_parent_listener(struct tcp_context *tcp, u64 id, u64 *listener_id);
int tcp_connection_status(struct tcp_context *tcp, u64 id,
                          u32 *state, u32 *readable, u32 *writable,
                          i32 *error, u32 *eof);
int tcp_take_error(struct tcp_context *tcp, u64 id, i32 *error);
int tcp_abort(struct tcp_context *tcp, u64 id, i32 error);
void tcp_abort_all(struct tcp_context *tcp, i32 error);
int tcp_queue_send(struct tcp_context *tcp, u64 id,
                   const void *data, u32 length);
int tcp_prepare_transmit(struct tcp_context *tcp, u64 id, u32 now,
                         struct tcp_transmit *transmit);
int tcp_receive_data(struct tcp_context *tcp, u64 id,
                     void *buffer, u32 capacity, u32 *received);
int tcp_shutdown(struct tcp_context *tcp, u64 id, u32 now,
                 struct tcp_transmit *transmit);
int tcp_detach(struct tcp_context *tcp, u64 id, u32 now,
               struct tcp_transmit *transmit);
int tcp_tick(struct tcp_context *tcp, u32 now,
             struct tcp_transmit *transmit);
int tcp_close(struct tcp_context *tcp, u64 id);
const struct tcp_cc_ops *tcp_cc_reno(void);
const struct tcp_cc_ops *tcp_cc_bbr(void);
void tcp_set_cc(struct tcp_context *tcp, const struct tcp_cc_ops *cc);
int tcp_cc_debug(struct tcp_context *tcp, u64 id,
                 u32 *cwnd, u32 *btl_bw, u32 *min_rtt, u32 *phase);
void tcp_set_ack_filter(struct tcp_context *tcp, u32 enabled);
void tcp_set_mss(struct tcp_context *tcp, u16 mss);
void tcp_set_pmtu_blackhole_callback(
    struct tcp_context *tcp,
    void (*callback)(u32 family, u32 address4, const u8 address6[16],
                     u32 ip_mtu, void *context),
    void *context);
int tcp_clamp_pmtu(struct tcp_context *tcp, u32 family, u32 address4,
                   const u8 address6[16], u32 ip_mtu);

#endif

BITS 64

global mich_syscall0
global mich_syscall1
global mich_write
global mich_send
global mich_send_nb
global mich_send_timeout
global mich_recv
global mich_recv_from
global mich_memfree
global mich_exit
global mich_wait
global mich_getpid
global mich_kill
global mich_service_register
global mich_service_lookup
global mich_cap_get
global mich_cap_drop
global mich_cap_grant
global mich_yield
global mich_fork
global mich_exec
global mich_spawn
global mich_handle_close
global mich_event_create
global mich_event_wait
global mich_event_wait_timeout
global mich_handle_transfer_batch
global mich_event_signal
global mich_event_reset
global mich_handle_duplicate
global mich_bridge_create
global mich_bridge_wait
global mich_bridge_read
global mich_irq_bind
global mich_irq_unbind
global mich_irq_set_mask
global mich_irq_wait
global mich_pci_count
global mich_pci_open
global mich_pci_bar_open
global mich_dma_allocate
global mich_msi_open
global mich_msix_table_open
global mich_irq_open
global mich_msix_irq_open
global mich_mmio_map
global mich_dma_map
global mich_resource_unmap
global mich_resource_length
global mich_driver_bootstrap
global mich_driver_stop_ack
global mich_msi_group_open
global mich_msix_group_open
global mich_page_create
global mich_shared_memory_create
global mich_page_map
global mich_page_pin
global mich_page_unpin
global mich_page_revoke
global mich_sg_create
global mich_sg_revoke
global mich_ring_create
global mich_ring_map
global mich_ring_submit
global mich_ring_consume
global mich_ring_revoke
global mich_completion_create
global mich_completion_begin
global mich_completion_finish
global mich_completion_cancel
global mich_completion_poll
global mich_completion_wait
global mich_timer_create
global mich_timer_arm
global mich_timer_cancel
global mich_timer_wait
global mich_wait_many
global mich_vnic_create
global mich_packet_pool_map
global mich_vnic_inject
global mich_vnic_receive
global mich_vnic_release_rx
global mich_vnic_acquire_tx
global mich_vnic_submit_tx
global mich_vnic_drain_tx
global mich_vnic_benchmark
global mich_vnic_revoke
global mich_socket_create
global mich_socket_bind
global mich_socket_send_to
global mich_socket_receive_from
global mich_socket_wait
global mich_net_interface_create
global mich_net_interface_set_link
global mich_net_interface_get_info
global mich_net_interface_revoke
global mich_virtio_open
global mich_virtio_negotiate
global mich_virtqueue_create
global mich_virtqueue_map
global mich_virtqueue_notify
global mich_virtio_driver_ok
global mich_virtio_read_config
global mich_virtqueue_chain_allocate
global mich_virtqueue_set_packet
global mich_virtqueue_publish
global mich_virtqueue_collect
global mich_virtqueue_chain_release
global mich_net_interface_driver_acquire_rx
global mich_net_interface_driver_receive
global mich_net_interface_driver_dequeue_tx
global mich_net_interface_driver_complete_tx
global mich_net_interface_driver_release_rx
global mich_net_interface_driver_acquire_rx_batch
global mich_net_interface_driver_receive_batch
global mich_net_interface_driver_dequeue_tx_batch
global mich_net_interface_driver_complete_tx_batch
global mich_net_interface_configure_ipv4
global mich_net_interface_send_echo
global mich_net_interface_echo_replies
global mich_net_interface_send_udp_probe
global mich_net_interface_poll_udp_probe
global mich_virtqueue_set_msix
global mich_virtqueue_collect_batch
global mich_net_interface_ipv6_start
global mich_net_interface_ipv6_complete_dad
global mich_net_interface_ipv6_get_info
global mich_net_interface_ipv6_send_echo
global mich_net_interface_ipv6_echo_replies
global mich_net_interface_udpv6_start_probe
global mich_net_interface_udpv6_poll_probe
global mich_socket_ipv6_create
global mich_socket_ipv6_bind
global mich_socket_ipv6_send_to
global mich_socket_ipv6_receive_from
global mich_net_interface_ipv6_maintenance
global mich_net_interface_tcp_probe_start
global mich_net_interface_tcp_probe_poll
global mich_socket_stream_create
global mich_socket_stream_connect
global mich_socket_stream_send
global mich_socket_stream_receive
global mich_socket_stream_state
global mich_socket_stream_shutdown
global mich_socket_stream_listen
global mich_socket_stream_listen_ipv6
global mich_socket_stream_accept
global mich_net_interface_tcpv6_probe_start
global mich_net_interface_tcpv6_probe_poll
global mich_socket_stream_take_error
global mich_vfs_root
global mich_vfs_create
global mich_vfs_lookup
global mich_vfs_open
global mich_vfs_read
global mich_vfs_write
global mich_vfs_truncate
global mich_vfs_stat
global mich_vfs_unlink
global mich_vfs_resolve
global mich_vfs_create_path
global mich_vfs_unlink_path
global mich_firmware_open
global mich_block_create
global mich_block_info
global mich_block_submit
global mich_block_collect
global mich_block_revoke
global mich_block_service
global mich_virtio_blk_open
global mich_ticks
global mich_pci_config_read8
global mich_pci_config_read16
global mich_pci_config_read32
global mich_pci_set_command

%define SYS_WRITE 1
%define SYS_SEND 5
%define SYS_RECV 6
%define SYS_SEND_NB 7
%define SYS_RECV_FROM 24
%define SYS_SEND_TIMEOUT 31
%define SYS_MEMFREE 11
%define SYS_EXIT 14
%define SYS_WAIT 15
%define SYS_GETPID 16
%define SYS_KILL 17
%define SYS_SERVICE_REGISTER 18
%define SYS_SERVICE_LOOKUP 19
%define SYS_CAP_GET 20
%define SYS_CAP_DROP 21
%define SYS_CAP_GRANT 22
%define SYS_YIELD 23
%define SYS_FORK 12
%define SYS_EXEC 13
%define SYS_SPAWN 32
%define SYS_HANDLE_CLOSE 33
%define SYS_EVENT_CREATE 34
%define SYS_EVENT_WAIT 35
%define SYS_EVENT_SIGNAL 36
%define SYS_EVENT_RESET 37
%define SYS_HANDLE_DUPLICATE 38
%define SYS_BRIDGE_CREATE 39
%define SYS_BRIDGE_WAIT 40
%define SYS_BRIDGE_READ 41
%define SYS_IRQ_BIND 42
%define SYS_IRQ_UNBIND 43
%define SYS_IRQ_SET_MASK 44
%define SYS_IRQ_WAIT 45
%define SYS_PCI_COUNT 46
%define SYS_PCI_OPEN 47
%define SYS_PCI_BAR_OPEN 48
%define SYS_DMA_ALLOCATE 49
%define SYS_MSI_OPEN 50
%define SYS_MSIX_OPEN 51
%define SYS_IRQ_OPEN 52
%define SYS_MSIX_IRQ_OPEN 53
%define SYS_MMIO_MAP 54
%define SYS_DMA_MAP 55
%define SYS_RESOURCE_UNMAP 56
%define SYS_RESOURCE_LENGTH 57
%define SYS_EVENT_WAIT_TIMEOUT 58
%define SYS_HANDLE_TRANSFER_BATCH 59
%define SYS_DRIVER_BOOTSTRAP 60
%define SYS_MSI_GROUP_OPEN 61
%define SYS_MSIX_GROUP_OPEN 62
%define SYS_PAGE_CREATE 63
%define SYS_SHARED_MEMORY_CREATE 64
%define SYS_PAGE_MAP 65
%define SYS_PAGE_PIN 66
%define SYS_PAGE_UNPIN 67
%define SYS_PAGE_REVOKE 68
%define SYS_SG_CREATE 69
%define SYS_SG_REVOKE 70
%define SYS_RING_CREATE 71
%define SYS_RING_MAP 72
%define SYS_RING_SUBMIT 73
%define SYS_RING_CONSUME 74
%define SYS_RING_REVOKE 75
%define SYS_COMPLETION_CREATE 76
%define SYS_COMPLETION_BEGIN 77
%define SYS_COMPLETION_FINISH 78
%define SYS_COMPLETION_CANCEL 79
%define SYS_COMPLETION_POLL 80
%define SYS_COMPLETION_WAIT 81
%define SYS_TIMER_CREATE 82
%define SYS_TIMER_ARM 83
%define SYS_TIMER_CANCEL 84
%define SYS_TIMER_WAIT 85
%define SYS_WAIT_MANY 86
%define SYS_VNIC_CREATE 87
%define SYS_PACKET_POOL_MAP 88
%define SYS_VNIC_INJECT 89
%define SYS_VNIC_RECEIVE 90
%define SYS_VNIC_RELEASE_RX 91
%define SYS_VNIC_ACQUIRE_TX 92
%define SYS_VNIC_SUBMIT_TX 93
%define SYS_VNIC_DRAIN_TX 94
%define SYS_VNIC_BENCHMARK 95
%define SYS_VNIC_REVOKE 96
%define SYS_SOCKET_CREATE 97
%define SYS_SOCKET_BIND 98
%define SYS_SOCKET_SEND_TO 99
%define SYS_SOCKET_RECEIVE_FROM 100
%define SYS_SOCKET_WAIT 101
%define SYS_NET_INTERFACE_CREATE 102
%define SYS_NET_INTERFACE_SET_LINK 103
%define SYS_NET_INTERFACE_GET_INFO 104
%define SYS_NET_INTERFACE_REVOKE 105
%define SYS_VIRTIO_OPEN 106
%define SYS_VIRTIO_NEGOTIATE 107
%define SYS_VIRTQUEUE_CREATE 108
%define SYS_VIRTQUEUE_MAP 109
%define SYS_VIRTQUEUE_NOTIFY 110
%define SYS_VIRTIO_DRIVER_OK 111
%define SYS_VIRTIO_READ_CONFIG 112
%define SYS_VIRTQUEUE_CHAIN_ALLOCATE 113
%define SYS_VIRTQUEUE_SET_PACKET 114
%define SYS_VIRTQUEUE_PUBLISH 115
%define SYS_VIRTQUEUE_COLLECT 116
%define SYS_VIRTQUEUE_CHAIN_RELEASE 117
%define SYS_NET_INTERFACE_DRIVER_ACQUIRE_RX 118
%define SYS_NET_INTERFACE_DRIVER_RECEIVE 119
%define SYS_NET_INTERFACE_DRIVER_DEQUEUE_TX 120
%define SYS_NET_INTERFACE_DRIVER_COMPLETE_TX 121
%define SYS_NET_INTERFACE_DRIVER_RELEASE_RX 122
%define SYS_NET_INTERFACE_CONFIGURE_IPV4 123
%define SYS_NET_INTERFACE_SEND_ECHO 124
%define SYS_NET_INTERFACE_ECHO_REPLIES 125
%define SYS_NET_INTERFACE_SEND_UDP_PROBE 126
%define SYS_NET_INTERFACE_POLL_UDP_PROBE 127
%define SYS_VIRTQUEUE_SET_MSIX 128
%define SYS_VIRTQUEUE_COLLECT_BATCH 129
%define SYS_NET_INTERFACE_IPV6_START 130
%define SYS_NET_INTERFACE_IPV6_COMPLETE_DAD 131
%define SYS_NET_INTERFACE_IPV6_GET_INFO 132
%define SYS_NET_INTERFACE_IPV6_SEND_ECHO 133
%define SYS_NET_INTERFACE_IPV6_ECHO_REPLIES 134
%define SYS_NET_INTERFACE_UDPV6_START_PROBE 135
%define SYS_NET_INTERFACE_UDPV6_POLL_PROBE 136
%define SYS_SOCKET_IPV6_CREATE 137
%define SYS_SOCKET_IPV6_BIND 138
%define SYS_SOCKET_IPV6_SEND_TO 139
%define SYS_SOCKET_IPV6_RECEIVE_FROM 140
%define SYS_NET_INTERFACE_IPV6_MAINTENANCE 141
%define SYS_NET_INTERFACE_TCP_PROBE_START 142
%define SYS_NET_INTERFACE_TCP_PROBE_POLL 143
%define SYS_SOCKET_STREAM_CREATE 144
%define SYS_SOCKET_STREAM_CONNECT 145
%define SYS_SOCKET_STREAM_SEND 146
%define SYS_SOCKET_STREAM_RECEIVE 147
%define SYS_SOCKET_STREAM_STATE 148
%define SYS_SOCKET_STREAM_SHUTDOWN 149
%define SYS_SOCKET_STREAM_LISTEN 150
%define SYS_SOCKET_STREAM_LISTEN_IPV6 167
%define SYS_SOCKET_STREAM_ACCEPT 151
%define SYS_NET_INTERFACE_TCPV6_PROBE_START 152
%define SYS_NET_INTERFACE_TCPV6_PROBE_POLL 153
%define SYS_SOCKET_STREAM_TAKE_ERROR 154
%define SYS_VFS_ROOT 155
%define SYS_VFS_CREATE 156
%define SYS_VFS_LOOKUP 157
%define SYS_VFS_OPEN 158
%define SYS_VFS_READ 159
%define SYS_VFS_WRITE 160
%define SYS_VFS_TRUNCATE 161
%define SYS_VFS_STAT 162
%define SYS_VFS_UNLINK 163
%define SYS_VFS_RESOLVE 164
%define SYS_VFS_CREATE_PATH 165
%define SYS_VFS_UNLINK_PATH 166
%define SYS_FIRMWARE_OPEN 168
%define SYS_BLOCK_CREATE 179
%define SYS_BLOCK_INFO 180
%define SYS_BLOCK_SUBMIT 181
%define SYS_BLOCK_COLLECT 182
%define SYS_BLOCK_REVOKE 183
%define SYS_BLOCK_SERVICE 184
%define SYS_VIRTIO_BLK_OPEN 185
%define SYS_DRIVER_STOP_ACK 169
%define SYS_NET_INTERFACE_DRIVER_ACQUIRE_RX_BATCH 170
%define SYS_NET_INTERFACE_DRIVER_RECEIVE_BATCH 171
%define SYS_NET_INTERFACE_DRIVER_DEQUEUE_TX_BATCH 172
%define SYS_NET_INTERFACE_DRIVER_COMPLETE_TX_BATCH 173
%define SYS_TICKS 174
%define SYS_PCI_CONFIG_READ8 175
%define SYS_PCI_CONFIG_READ16 176
%define SYS_PCI_CONFIG_READ32 177
%define SYS_PCI_SET_COMMAND 178

section .text
mich_syscall0:
    mov rax, rdi
    syscall
    ret

mich_syscall1:
    mov rax, rdi
    mov rdi, rsi
    syscall
    ret

mich_write:
    mov eax, SYS_WRITE
    syscall
    ret

mich_send:
    mov eax, SYS_SEND
    syscall
    ret

mich_send_nb:
    mov eax, SYS_SEND_NB
    syscall
    ret

mich_send_timeout:
    mov eax, SYS_SEND_TIMEOUT
    syscall
    ret

mich_recv:
    mov eax, SYS_RECV
    syscall
    ret

mich_recv_from:
    mov eax, SYS_RECV_FROM
    syscall
    ret

mich_memfree:
    mov eax, SYS_MEMFREE
    syscall
    ret

mich_exit:
    mov eax, SYS_EXIT
    syscall
.hang:
    jmp .hang

mich_wait:
    mov eax, SYS_WAIT
    syscall
    ret

mich_getpid:
    mov eax, SYS_GETPID
    syscall
    ret

mich_kill:
    mov eax, SYS_KILL
    syscall
    ret

mich_service_register:
    mov eax, SYS_SERVICE_REGISTER
    syscall
    ret

mich_service_lookup:
    mov eax, SYS_SERVICE_LOOKUP
    syscall
    ret

mich_cap_get:
    mov eax, SYS_CAP_GET
    syscall
    ret

mich_cap_drop:
    mov eax, SYS_CAP_DROP
    syscall
    ret

mich_cap_grant:
    mov eax, SYS_CAP_GRANT
    syscall
    ret

mich_yield:
    mov eax, SYS_YIELD
    syscall
    ret

mich_fork:
    mov eax, SYS_FORK
    syscall
    ret

mich_exec:
    mov eax, SYS_EXEC
    syscall
    ret

mich_spawn:
    mov eax, SYS_SPAWN
    syscall
    ret

mich_handle_close:
    mov eax, SYS_HANDLE_CLOSE
    syscall
    ret

mich_event_create:
    mov eax, SYS_EVENT_CREATE
    syscall
    ret

mich_event_wait:
    mov eax, SYS_EVENT_WAIT
    syscall
    ret

mich_event_signal:
    mov eax, SYS_EVENT_SIGNAL
    syscall
    ret

mich_event_reset:
    mov eax, SYS_EVENT_RESET
    syscall
    ret

mich_handle_duplicate:
    mov eax, SYS_HANDLE_DUPLICATE
    syscall
    ret

mich_bridge_create:
    mov eax, SYS_BRIDGE_CREATE
    syscall
    ret

mich_bridge_wait:
    mov eax, SYS_BRIDGE_WAIT
    syscall
    ret

mich_bridge_read:
    mov eax, SYS_BRIDGE_READ
    syscall
    ret

mich_irq_bind:
    mov eax, SYS_IRQ_BIND
    syscall
    ret

mich_irq_unbind:
    mov eax, SYS_IRQ_UNBIND
    syscall
    ret

mich_irq_set_mask:
    mov eax, SYS_IRQ_SET_MASK
    syscall
    ret

mich_irq_wait:
    mov eax, SYS_IRQ_WAIT
    syscall
    ret

mich_pci_count:
    mov eax, SYS_PCI_COUNT
    syscall
    ret

mich_pci_open:
    mov eax, SYS_PCI_OPEN
    syscall
    ret

mich_pci_bar_open:
    mov eax, SYS_PCI_BAR_OPEN
    syscall
    ret

mich_dma_allocate:
    mov eax, SYS_DMA_ALLOCATE
    syscall
    ret

mich_msi_open:
    mov eax, SYS_MSI_OPEN
    syscall
    ret

mich_msix_table_open:
    mov eax, SYS_MSIX_OPEN
    syscall
    ret

mich_irq_open:
    mov eax, SYS_IRQ_OPEN
    syscall
    ret

mich_msix_irq_open:
    mov eax, SYS_MSIX_IRQ_OPEN
    syscall
    ret

mich_mmio_map:
    mov eax, SYS_MMIO_MAP
    syscall
    ret

mich_dma_map:
    mov eax, SYS_DMA_MAP
    syscall
    ret

mich_resource_unmap:
    mov eax, SYS_RESOURCE_UNMAP
    syscall
    ret

mich_resource_length:
    mov eax, SYS_RESOURCE_LENGTH
    syscall
    ret

mich_event_wait_timeout:
    mov eax, SYS_EVENT_WAIT_TIMEOUT
    syscall
    ret

mich_handle_transfer_batch:
    mov eax, SYS_HANDLE_TRANSFER_BATCH
    syscall
    ret

mich_driver_bootstrap:
    mov eax, SYS_DRIVER_BOOTSTRAP
    syscall
    ret

mich_driver_stop_ack:
    mov eax, SYS_DRIVER_STOP_ACK
    syscall
    ret

mich_msi_group_open:
    mov eax, SYS_MSI_GROUP_OPEN
    syscall
    ret

mich_msix_group_open:
    mov eax, SYS_MSIX_GROUP_OPEN
    syscall
    ret

mich_page_create:
    mov eax, SYS_PAGE_CREATE
    syscall
    ret

mich_shared_memory_create:
    mov eax, SYS_SHARED_MEMORY_CREATE
    syscall
    ret

mich_page_map:
    mov eax, SYS_PAGE_MAP
    syscall
    ret

mich_page_pin:
    mov eax, SYS_PAGE_PIN
    syscall
    ret

mich_page_unpin:
    mov eax, SYS_PAGE_UNPIN
    syscall
    ret

mich_page_revoke:
    mov eax, SYS_PAGE_REVOKE
    syscall
    ret

mich_sg_create:
    mov eax, SYS_SG_CREATE
    syscall
    ret

mich_sg_revoke:
    mov eax, SYS_SG_REVOKE
    syscall
    ret

mich_ring_create:
    mov eax, SYS_RING_CREATE
    syscall
    ret

mich_ring_map:
    mov eax, SYS_RING_MAP
    syscall
    ret

mich_ring_submit:
    mov eax, SYS_RING_SUBMIT
    syscall
    ret

mich_ring_consume:
    mov eax, SYS_RING_CONSUME
    syscall
    ret

mich_ring_revoke:
    mov eax, SYS_RING_REVOKE
    syscall
    ret

mich_completion_create:
    mov eax, SYS_COMPLETION_CREATE
    syscall
    ret

mich_completion_begin:
    mov eax, SYS_COMPLETION_BEGIN
    syscall
    ret

mich_completion_finish:
    mov eax, SYS_COMPLETION_FINISH
    syscall
    ret

mich_completion_cancel:
    mov eax, SYS_COMPLETION_CANCEL
    syscall
    ret

mich_completion_poll:
    mov eax, SYS_COMPLETION_POLL
    syscall
    ret

mich_completion_wait:
    mov eax, SYS_COMPLETION_WAIT
    syscall
    ret

mich_timer_create:
    mov eax, SYS_TIMER_CREATE
    syscall
    ret

mich_timer_arm:
    mov eax, SYS_TIMER_ARM
    syscall
    ret

mich_timer_cancel:
    mov eax, SYS_TIMER_CANCEL
    syscall
    ret

mich_timer_wait:
    mov eax, SYS_TIMER_WAIT
    syscall
    ret

mich_wait_many:
    mov eax, SYS_WAIT_MANY
    syscall
    ret

mich_vnic_create:
    mov eax, SYS_VNIC_CREATE
    syscall
    ret

mich_packet_pool_map:
    mov eax, SYS_PACKET_POOL_MAP
    syscall
    ret

mich_vnic_inject:
    mov eax, SYS_VNIC_INJECT
    syscall
    ret

mich_vnic_receive:
    mov eax, SYS_VNIC_RECEIVE
    syscall
    ret

mich_vnic_release_rx:
    mov eax, SYS_VNIC_RELEASE_RX
    syscall
    ret

mich_vnic_acquire_tx:
    mov eax, SYS_VNIC_ACQUIRE_TX
    syscall
    ret

mich_vnic_submit_tx:
    mov eax, SYS_VNIC_SUBMIT_TX
    syscall
    ret

mich_vnic_drain_tx:
    mov eax, SYS_VNIC_DRAIN_TX
    syscall
    ret

mich_vnic_benchmark:
    mov eax, SYS_VNIC_BENCHMARK
    syscall
    ret

mich_vnic_revoke:
    mov eax, SYS_VNIC_REVOKE
    syscall
    ret

mich_socket_create:
    mov eax, SYS_SOCKET_CREATE
    syscall
    ret

mich_socket_bind:
    mov eax, SYS_SOCKET_BIND
    syscall
    ret

mich_socket_send_to:
    mov eax, SYS_SOCKET_SEND_TO
    syscall
    ret

mich_socket_receive_from:
    mov eax, SYS_SOCKET_RECEIVE_FROM
    syscall
    ret

mich_socket_wait:
    mov eax, SYS_SOCKET_WAIT
    syscall
    ret

mich_net_interface_create:
    mov eax, SYS_NET_INTERFACE_CREATE
    syscall
    ret

mich_net_interface_set_link:
    mov eax, SYS_NET_INTERFACE_SET_LINK
    syscall
    ret

mich_net_interface_get_info:
    mov eax, SYS_NET_INTERFACE_GET_INFO
    syscall
    ret

mich_net_interface_revoke:
    mov eax, SYS_NET_INTERFACE_REVOKE
    syscall
    ret

mich_virtio_open:
    mov eax, SYS_VIRTIO_OPEN
    syscall
    ret

mich_virtio_negotiate:
    mov eax, SYS_VIRTIO_NEGOTIATE
    syscall
    ret

mich_virtqueue_create:
    mov eax, SYS_VIRTQUEUE_CREATE
    syscall
    ret

mich_virtqueue_map:
    mov eax, SYS_VIRTQUEUE_MAP
    syscall
    ret

mich_virtqueue_notify:
    mov eax, SYS_VIRTQUEUE_NOTIFY
    syscall
    ret

mich_virtio_driver_ok:
    mov eax, SYS_VIRTIO_DRIVER_OK
    syscall
    ret

mich_virtio_read_config:
    mov eax, SYS_VIRTIO_READ_CONFIG
    syscall
    ret

mich_virtqueue_chain_allocate:
    mov eax, SYS_VIRTQUEUE_CHAIN_ALLOCATE
    syscall
    ret

mich_virtqueue_set_packet:
    mov eax, SYS_VIRTQUEUE_SET_PACKET
    syscall
    ret

mich_virtqueue_publish:
    mov eax, SYS_VIRTQUEUE_PUBLISH
    syscall
    ret

mich_virtqueue_collect:
    mov eax, SYS_VIRTQUEUE_COLLECT
    syscall
    ret

mich_virtqueue_chain_release:
    mov eax, SYS_VIRTQUEUE_CHAIN_RELEASE
    syscall
    ret

mich_net_interface_driver_acquire_rx:
    mov eax, SYS_NET_INTERFACE_DRIVER_ACQUIRE_RX
    syscall
    ret

mich_net_interface_driver_receive:
    mov eax, SYS_NET_INTERFACE_DRIVER_RECEIVE
    syscall
    ret

mich_net_interface_driver_dequeue_tx:
    mov eax, SYS_NET_INTERFACE_DRIVER_DEQUEUE_TX
    syscall
    ret

mich_net_interface_driver_complete_tx:
    mov eax, SYS_NET_INTERFACE_DRIVER_COMPLETE_TX
    syscall
    ret

mich_net_interface_driver_release_rx:
    mov eax, SYS_NET_INTERFACE_DRIVER_RELEASE_RX
    syscall
    ret

mich_net_interface_driver_acquire_rx_batch:
    mov eax, SYS_NET_INTERFACE_DRIVER_ACQUIRE_RX_BATCH
    syscall
    ret

mich_net_interface_driver_receive_batch:
    mov eax, SYS_NET_INTERFACE_DRIVER_RECEIVE_BATCH
    syscall
    ret

mich_net_interface_driver_dequeue_tx_batch:
    mov eax, SYS_NET_INTERFACE_DRIVER_DEQUEUE_TX_BATCH
    syscall
    ret

mich_net_interface_driver_complete_tx_batch:
    mov eax, SYS_NET_INTERFACE_DRIVER_COMPLETE_TX_BATCH
    syscall
    ret

mich_net_interface_configure_ipv4:
    mov eax, SYS_NET_INTERFACE_CONFIGURE_IPV4
    syscall
    ret

mich_net_interface_send_echo:
    mov eax, SYS_NET_INTERFACE_SEND_ECHO
    syscall
    ret

mich_net_interface_echo_replies:
    mov eax, SYS_NET_INTERFACE_ECHO_REPLIES
    syscall
    ret

mich_net_interface_send_udp_probe:
    mov eax, SYS_NET_INTERFACE_SEND_UDP_PROBE
    syscall
    ret

mich_net_interface_poll_udp_probe:
    mov eax, SYS_NET_INTERFACE_POLL_UDP_PROBE
    syscall
    ret

mich_virtqueue_set_msix:
    mov eax, SYS_VIRTQUEUE_SET_MSIX
    syscall
    ret

mich_virtqueue_collect_batch:
    mov eax, SYS_VIRTQUEUE_COLLECT_BATCH
    syscall
    ret

mich_net_interface_ipv6_start:
    mov eax, SYS_NET_INTERFACE_IPV6_START
    syscall
    ret

mich_net_interface_ipv6_complete_dad:
    mov eax, SYS_NET_INTERFACE_IPV6_COMPLETE_DAD
    syscall
    ret

mich_net_interface_ipv6_get_info:
    mov eax, SYS_NET_INTERFACE_IPV6_GET_INFO
    syscall
    ret

mich_net_interface_ipv6_send_echo:
    mov eax, SYS_NET_INTERFACE_IPV6_SEND_ECHO
    syscall
    ret

mich_net_interface_ipv6_echo_replies:
    mov eax, SYS_NET_INTERFACE_IPV6_ECHO_REPLIES
    syscall
    ret

mich_net_interface_udpv6_start_probe:
    mov eax, SYS_NET_INTERFACE_UDPV6_START_PROBE
    syscall
    ret

mich_net_interface_udpv6_poll_probe:
    mov eax, SYS_NET_INTERFACE_UDPV6_POLL_PROBE
    syscall
    ret

mich_socket_ipv6_create:
    mov eax, SYS_SOCKET_IPV6_CREATE
    syscall
    ret

mich_socket_ipv6_bind:
    mov eax, SYS_SOCKET_IPV6_BIND
    syscall
    ret

mich_socket_ipv6_send_to:
    mov eax, SYS_SOCKET_IPV6_SEND_TO
    syscall
    ret

mich_socket_ipv6_receive_from:
    mov eax, SYS_SOCKET_IPV6_RECEIVE_FROM
    syscall
    ret

mich_net_interface_ipv6_maintenance:
    mov eax, SYS_NET_INTERFACE_IPV6_MAINTENANCE
    syscall
    ret

mich_net_interface_tcp_probe_start:
    mov eax, SYS_NET_INTERFACE_TCP_PROBE_START
    syscall
    ret

mich_net_interface_tcp_probe_poll:
    mov eax, SYS_NET_INTERFACE_TCP_PROBE_POLL
    syscall
    ret

mich_socket_stream_create:
    mov eax, SYS_SOCKET_STREAM_CREATE
    syscall
    ret

mich_socket_stream_connect:
    mov eax, SYS_SOCKET_STREAM_CONNECT
    syscall
    ret

mich_socket_stream_send:
    mov eax, SYS_SOCKET_STREAM_SEND
    syscall
    ret

mich_socket_stream_receive:
    mov eax, SYS_SOCKET_STREAM_RECEIVE
    syscall
    ret

mich_socket_stream_state:
    mov eax, SYS_SOCKET_STREAM_STATE
    syscall
    ret

mich_socket_stream_shutdown:
    mov eax, SYS_SOCKET_STREAM_SHUTDOWN
    syscall
    ret

mich_socket_stream_listen:
    mov eax, SYS_SOCKET_STREAM_LISTEN
    syscall
    ret

mich_socket_stream_listen_ipv6:
    mov eax, SYS_SOCKET_STREAM_LISTEN_IPV6
    syscall
    ret

mich_socket_stream_accept:
    mov eax, SYS_SOCKET_STREAM_ACCEPT
    syscall
    ret

mich_net_interface_tcpv6_probe_start:
    mov eax, SYS_NET_INTERFACE_TCPV6_PROBE_START
    syscall
    ret

mich_net_interface_tcpv6_probe_poll:
    mov eax, SYS_NET_INTERFACE_TCPV6_PROBE_POLL
    syscall
    ret

mich_socket_stream_take_error:
    mov eax, SYS_SOCKET_STREAM_TAKE_ERROR
    syscall
    ret

mich_vfs_root:
    mov eax, SYS_VFS_ROOT
    syscall
    ret

mich_vfs_create:
    mov eax, SYS_VFS_CREATE
    syscall
    ret

mich_vfs_lookup:
    mov eax, SYS_VFS_LOOKUP
    syscall
    ret

mich_vfs_open:
    mov eax, SYS_VFS_OPEN
    syscall
    ret

mich_vfs_read:
    mov eax, SYS_VFS_READ
    syscall
    ret

mich_vfs_write:
    mov eax, SYS_VFS_WRITE
    syscall
    ret

mich_vfs_truncate:
    mov eax, SYS_VFS_TRUNCATE
    syscall
    ret

mich_vfs_stat:
    mov eax, SYS_VFS_STAT
    syscall
    ret

mich_vfs_unlink:
    mov eax, SYS_VFS_UNLINK
    syscall
    ret

mich_vfs_resolve:
    mov eax, SYS_VFS_RESOLVE
    syscall
    ret

mich_vfs_create_path:
    mov eax, SYS_VFS_CREATE_PATH
    syscall
    ret

mich_vfs_unlink_path:
    mov eax, SYS_VFS_UNLINK_PATH
    syscall
    ret

mich_firmware_open:
    mov eax, SYS_FIRMWARE_OPEN
    syscall
    ret

mich_block_create:
    mov eax, SYS_BLOCK_CREATE
    syscall
    ret

mich_block_info:
    mov eax, SYS_BLOCK_INFO
    syscall
    ret

mich_block_submit:
    mov eax, SYS_BLOCK_SUBMIT
    syscall
    ret

mich_block_collect:
    mov eax, SYS_BLOCK_COLLECT
    syscall
    ret

mich_block_revoke:
    mov eax, SYS_BLOCK_REVOKE
    syscall
    ret

mich_block_service:
    mov eax, SYS_BLOCK_SERVICE
    syscall
    ret

mich_virtio_blk_open:
    mov eax, SYS_VIRTIO_BLK_OPEN
    syscall
    ret

mich_ticks:
    mov eax, SYS_TICKS
    syscall
    ret

mich_pci_config_read8:
    mov eax, SYS_PCI_CONFIG_READ8
    syscall
    ret

mich_pci_config_read16:
    mov eax, SYS_PCI_CONFIG_READ16
    syscall
    ret

mich_pci_config_read32:
    mov eax, SYS_PCI_CONFIG_READ32
    syscall
    ret

mich_pci_set_command:
    mov eax, SYS_PCI_SET_COMMAND
    syscall
    ret

section .note.GNU-stack noalloc noexec nowrite progbits

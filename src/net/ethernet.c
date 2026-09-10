#include "ethernet.h"
#include "vnic.h"
#include "net_buffer.h"
#include "ring.h"

static u16 read_be16(const u8 *bytes) {
    return (u16)((u16)bytes[0] << 8) | bytes[1];
}

static int address_equal(const u8 *left, const u8 *right) {
    for (u32 index = 0; index < ETHERNET_ADDRESS_SIZE; index++)
        if (left[index] != right[index]) return 0;
    return 1;
}

static int address_broadcast(const u8 *address) {
    for (u32 index = 0; index < ETHERNET_ADDRESS_SIZE; index++)
        if (address[index] != 0xFF) return 0;
    return 1;
}

static int address_zero(const u8 *address) {
    for (u32 index = 0; index < ETHERNET_ADDRESS_SIZE; index++)
        if (address[index]) return 0;
    return 1;
}

int ethernet_port_init(struct ethernet_port *port, const u8 address[6]) {
    if (!port || !address || (address[0] & 1) || address_zero(address))
        return -1;
    for (u32 index = 0; index < ETHERNET_ADDRESS_SIZE; index++)
        port->address[index] = address[index];
    port->promiscuous = 0;
    port->accept_multicast = 0;
    port->handler_count = 0;
    for (u32 index = 0; index < ETHERNET_HANDLER_MAX; index++) {
        port->handlers[index].ether_type = 0;
        port->handlers[index].function = 0;
        port->handlers[index].context = 0;
    }
    u8 *stats = (u8 *)&port->stats;
    for (usize_t index = 0; index < sizeof(port->stats); index++) stats[index] = 0;
    return 0;
}

int ethernet_port_set_filter(struct ethernet_port *port, int promiscuous,
                             int accept_multicast) {
    if (!port) return -1;
    port->promiscuous = promiscuous != 0;
    port->accept_multicast = accept_multicast != 0;
    return 0;
}

int ethernet_register_handler(struct ethernet_port *port, u16 ether_type,
                              ethernet_handler_fn function, void *context) {
    if (!port || ether_type < 0x0600 || !function ||
        port->handler_count >= ETHERNET_HANDLER_MAX)
        return -1;
    for (u32 index = 0; index < port->handler_count; index++)
        if (port->handlers[index].ether_type == ether_type) return -1;
    struct ethernet_handler *handler = &port->handlers[port->handler_count++];
    handler->ether_type = ether_type;
    handler->function = function;
    handler->context = context;
    return 0;
}

int ethernet_parse(const void *data, u32 length,
                   struct ethernet_frame_view *frame) {
    if (!data || !frame || length < ETHERNET_HEADER_SIZE ||
        length > ETHERNET_FRAME_MAX)
        return -1;
    const u8 *bytes = (const u8 *)data;
    const u8 *source = bytes + ETHERNET_ADDRESS_SIZE;
    if ((source[0] & 1) || address_zero(source)) return -2;
    u16 type = read_be16(bytes + 12);
    u32 header_length = ETHERNET_HEADER_SIZE;
    u16 vlan_tci = 0;
    u32 flags = 0;
    if (address_broadcast(bytes)) flags |= ETHERNET_FLAG_BROADCAST;
    else if (bytes[0] & 1) flags |= ETHERNET_FLAG_MULTICAST;
    if (type == 0x8100 || type == 0x88A8) {
        if (length < ETHERNET_VLAN_HEADER_SIZE) return -1;
        vlan_tci = read_be16(bytes + 14);
        type = read_be16(bytes + 16);
        header_length = ETHERNET_VLAN_HEADER_SIZE;
        flags |= ETHERNET_FLAG_VLAN;
        if (type == 0x8100 || type == 0x88A8) return -3;
    }
    if (type < 0x0600) return -3;
    if (!(flags & ETHERNET_FLAG_VLAN) &&
        length > ETHERNET_FRAME_MAX_UNTAGGED)
        return -1;
    frame->frame = bytes;
    frame->payload = bytes + header_length;
    frame->frame_length = length;
    frame->payload_length = length - header_length;
    frame->ether_type = type;
    frame->vlan_tci = vlan_tci;
    frame->flags = flags;
    return 0;
}

static int destination_allowed(const struct ethernet_port *port,
                               const struct ethernet_frame_view *frame) {
    const u8 *destination = frame->frame;
    if (port->promiscuous || address_equal(destination, port->address) ||
        (frame->flags & ETHERNET_FLAG_BROADCAST))
        return 1;
    return (frame->flags & ETHERNET_FLAG_MULTICAST) &&
           port->accept_multicast;
}

int ethernet_receive(struct ethernet_port *port, const void *data, u32 length) {
    if (!port) return -1;
    struct ethernet_frame_view frame;
    int parsed = ethernet_parse(data, length, &frame);
    if (parsed) {
        if (parsed == -1) port->stats.drops_length++;
        else if (parsed == -2) port->stats.drops_source++;
        else port->stats.drops_protocol++;
        return -1;
    }
    if (!destination_allowed(port, &frame)) {
        port->stats.drops_filter++;
        return -1;
    }
    port->stats.frames++;
    port->stats.bytes += frame.frame_length;
    if (frame.flags & ETHERNET_FLAG_BROADCAST) port->stats.broadcast++;
    else if (frame.flags & ETHERNET_FLAG_MULTICAST) port->stats.multicast++;
    else port->stats.unicast++;
    if (frame.flags & ETHERNET_FLAG_VLAN) port->stats.vlan++;
    for (u32 index = 0; index < port->handler_count; index++) {
        struct ethernet_handler *handler = &port->handlers[index];
        if (handler->ether_type != frame.ether_type) continue;
        if (handler->function(port, &frame, handler->context)) {
            port->stats.drops_protocol++;
            return -1;
        }
        return 0;
    }
    port->stats.drops_protocol++;
    return -1;
}

int ethernet_receive_vnic(struct ethernet_port *port,
                          struct kernel_object *vnic) {
    if (!port || !vnic) return -1;
    struct net_packet_descriptor descriptor;
    if (vnic_receive(vnic, &descriptor)) return -1;
    struct kernel_object *pool = vnic_pool(vnic);
    u8 *data = (u8 *)packet_pool_data(
        pool, descriptor.buffer_id, NET_BUFFER_STACK);
    int result = !data ? -1 : ethernet_receive(
        port, data + descriptor.offset, descriptor.length);
    if (vnic_release_rx(vnic, descriptor.buffer_id)) return -1;
    return result;
}

u32 ethernet_receive_vnic_batch(struct ethernet_port *port,
                                struct kernel_object *vnic, u32 budget) {
    if (!port || !vnic || !budget) return 0;
    u32 processed = 0;
    while (processed < budget) {
        struct ring_resource *ring = ring_resource_get(vnic_rx_ring(vnic));
        if (!ring || ring->producer == ring->consumer) break;
        ethernet_receive_vnic(port, vnic);
        processed++;
    }
    return processed;
}

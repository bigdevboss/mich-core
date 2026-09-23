#include "socket.h"
#include "net_interface.h"
#include "vfs.h"
#include "event.h"
#include "socket_abi.h"

struct socket_state {
    struct udp_context *udp;
    struct udpv6_context *udpv6;
    struct kernel_object *interface;
    struct kernel_object *event;
    struct tcp_context *tcp;
    u64 binding_id;
    u64 tcp_connection;
    u32 family;
    u32 bound;
    u32 listening;
    u32 backlog;
    u32 active;
};

struct socket_udp_context {
    struct udp_context *udp;
    struct udpv6_context *udpv6;
    u32 interface_id;
    u32 interface_generation;
    u32 active;
};

static struct socket_state sockets[SOCKET_MAX];
static struct socket_udp_context udp_contexts[SOCKET_UDP_CONTEXT_MAX];
static struct socket_udp_context udpv6_contexts[SOCKET_UDP_CONTEXT_MAX];
static struct route_table *socket_routes;

static struct socket_state *state_for(const struct kernel_object *object) {
    if (!object || !object->active || object->type != KOBJECT_SOCKET ||
        !object->value || object->value > SOCKET_MAX)
        return 0;
    struct socket_state *state = &sockets[object->value - 1];
    return state->active ? state : 0;
}

static struct socket_udp_context *context_for_address(u32 address) {
    for (u32 index = 0; index < SOCKET_UDP_CONTEXT_MAX; index++) {
        struct socket_udp_context *context = &udp_contexts[index];
        if (!context->active || !context->udp) continue;
        if (address == context->udp->ipv4->local_address) return context;
    }
    if (!address && udp_contexts[0].active) return &udp_contexts[0];
    return 0;
}

static struct socket_udp_context *context_for_route(
    const struct route_entry *route) {
    if (!route) return 0;
    for (u32 index = 0; index < SOCKET_UDP_CONTEXT_MAX; index++) {
        struct socket_udp_context *context = &udp_contexts[index];
        if (!context->active) continue;
        if (route->interface_id == ROUTE_INTERFACE_LOOPBACK &&
            !route->interface_generation)
            return index == 0 ? context : 0;
        if (context->interface_id == route->interface_id &&
            context->interface_generation == route->interface_generation)
            return context;
    }
    return 0;
}

static void socket_destroy(struct kernel_object *object) {
    if (!object || !object->value || object->value > SOCKET_MAX) return;
    struct socket_state *state = &sockets[object->value - 1];
    if (!state->active) return;
    if (state->bound && state->family == 4 && state->udp)
        udp_unbind(state->udp, state->binding_id);
    if (state->bound && state->family == 6 && state->udpv6)
        udpv6_unbind(state->udpv6, state->binding_id);
    if (state->family == 14 && state->interface && state->tcp_connection)
        net_interface_tcp_close(state->interface, state->tcp_connection);
    if (state->interface) object_release(state->interface);
    if (state->event) object_release(state->event);
    state->udp = 0;
    state->udpv6 = 0;
    state->interface = 0;
    state->event = 0;
    state->tcp = 0;
    state->binding_id = 0;
    state->tcp_connection = 0;
    state->family = 0;
    state->bound = 0;
    state->listening = 0;
    state->backlog = 0;
    state->active = 0;
}

int socket_init(struct udp_context *udp, struct route_table *routes) {
    if (!udp || !routes) return -1;
    for (u32 index = 0; index < SOCKET_MAX; index++)
        if (sockets[index].active) return -1;
    socket_routes = routes;
    for (u32 index = 0; index < SOCKET_MAX; index++) {
        sockets[index].udp = 0;
        sockets[index].udpv6 = 0;
        sockets[index].interface = 0;
        sockets[index].event = 0;
        sockets[index].tcp = 0;
        sockets[index].binding_id = 0;
        sockets[index].tcp_connection = 0;
        sockets[index].family = 0;
        sockets[index].bound = 0;
        sockets[index].listening = 0;
        sockets[index].backlog = 0;
        sockets[index].active = 0;
    }
    for (u32 index = 0; index < SOCKET_UDP_CONTEXT_MAX; index++) {
        udp_contexts[index].udp = 0;
        udp_contexts[index].udpv6 = 0;
        udp_contexts[index].interface_id = 0;
        udp_contexts[index].interface_generation = 0;
        udp_contexts[index].active = 0;
        udpv6_contexts[index].udp = 0;
        udpv6_contexts[index].udpv6 = 0;
        udpv6_contexts[index].interface_id = 0;
        udpv6_contexts[index].interface_generation = 0;
        udpv6_contexts[index].active = 0;
    }
    udp_contexts[0].udp = udp;
    udp_contexts[0].interface_id = ROUTE_INTERFACE_LOOPBACK;
    udp_contexts[0].interface_generation = 0;
    udp_contexts[0].active = 1;
    return 0;
}

int socket_register_udp(struct udp_context *udp, u32 interface_id,
                        u32 interface_generation) {
    if (!udp || !interface_id || !interface_generation) return -1;
    for (u32 index = 0; index < SOCKET_UDP_CONTEXT_MAX; index++)
        if (udp_contexts[index].active &&
            (udp_contexts[index].udp == udp ||
             (udp_contexts[index].interface_id == interface_id &&
              udp_contexts[index].interface_generation ==
                  interface_generation)))
            return -1;
    for (u32 index = 1; index < SOCKET_UDP_CONTEXT_MAX; index++) {
        struct socket_udp_context *context = &udp_contexts[index];
        if (context->active) continue;
        context->udp = udp;
        context->interface_id = interface_id;
        context->interface_generation = interface_generation;
        context->active = 1;
        return 0;
    }
    return -1;
}

void socket_unregister_udp(struct udp_context *udp) {
    if (!udp) return;
    for (u32 index = 0; index < SOCKET_MAX; index++) {
        struct socket_state *state = &sockets[index];
        if (!state->active || !state->bound || state->udp != udp) continue;
        udp_unbind(state->udp, state->binding_id);
        state->udp = 0;
        state->binding_id = 0;
        state->bound = 0;
    }
    for (u32 index = 1; index < SOCKET_UDP_CONTEXT_MAX; index++) {
        struct socket_udp_context *context = &udp_contexts[index];
        if (!context->active || context->udp != udp) continue;
        context->udp = 0;
        context->interface_id = 0;
        context->interface_generation = 0;
        context->active = 0;
    }
}

int socket_register_udpv6(struct udpv6_context *udp, u32 interface_id,
                          u32 interface_generation) {
    if (!udp || !interface_id || !interface_generation) return -1;
    for (u32 index = 0; index < SOCKET_UDP_CONTEXT_MAX; index++)
        if (udpv6_contexts[index].active &&
            (udpv6_contexts[index].udpv6 == udp ||
             (udpv6_contexts[index].interface_id == interface_id &&
              udpv6_contexts[index].interface_generation ==
                  interface_generation)))
            return -1;
    for (u32 index = 0; index < SOCKET_UDP_CONTEXT_MAX; index++) {
        struct socket_udp_context *context = &udpv6_contexts[index];
        if (context->active) continue;
        context->udpv6 = udp;
        context->interface_id = interface_id;
        context->interface_generation = interface_generation;
        context->active = 1;
        return 0;
    }
    return -1;
}

void socket_unregister_udpv6(struct udpv6_context *udp) {
    if (!udp) return;
    for (u32 index = 0; index < SOCKET_MAX; index++) {
        struct socket_state *state = &sockets[index];
        if (!state->active || !state->bound || state->udpv6 != udp) continue;
        udpv6_unbind(state->udpv6, state->binding_id);
        state->udpv6 = 0;
        state->binding_id = 0;
        state->bound = 0;
    }
    for (u32 index = 0; index < SOCKET_UDP_CONTEXT_MAX; index++) {
        struct socket_udp_context *context = &udpv6_contexts[index];
        if (!context->active || context->udpv6 != udp) continue;
        context->udpv6 = 0;
        context->interface_id = 0;
        context->interface_generation = 0;
        context->active = 0;
    }
}

struct kernel_object *socket_create(void) {
    if (!udp_contexts[0].active || !socket_routes) return 0;
    for (u32 index = 0; index < SOCKET_MAX; index++) {
        struct socket_state *state = &sockets[index];
        if (state->active) continue;
        state->udp = 0;
        state->udpv6 = 0;
        state->binding_id = 0;
        state->family = 4;
        state->bound = 0;
        state->active = 1;
        struct kernel_object *object = object_create(
            KOBJECT_SOCKET, index + 1, socket_destroy);
        if (!object) state->active = 0;
        return object;
    }
    return 0;
}

struct kernel_object *socket_create_ipv6(void) {
    if (!socket_routes) return 0;
    for (u32 index = 0; index < SOCKET_MAX; index++) {
        struct socket_state *state = &sockets[index];
        if (state->active) continue;
        state->udp = 0;
        state->udpv6 = 0;
        state->binding_id = 0;
        state->family = 6;
        state->bound = 0;
        state->active = 1;
        struct kernel_object *object = object_create(
            KOBJECT_SOCKET, index + 1, socket_destroy);
        if (!object) state->active = 0;
        return object;
    }
    return 0;
}

struct kernel_object *socket_create_stream(void) {
    if (!socket_routes) return 0;
    for (u32 index = 0; index < SOCKET_MAX; index++) {
        struct socket_state *state = &sockets[index];
        if (state->active) continue;
        struct kernel_object *event = event_create(EVENT_AUTO_RESET, 0);
        if (!event) return 0;
        state->udp = 0;
        state->udpv6 = 0;
        state->interface = 0;
        state->event = event;
        state->tcp = 0;
        state->binding_id = 0;
        state->tcp_connection = 0;
        state->family = 14;
        state->bound = 0;
        state->listening = 0;
        state->backlog = 0;
        state->active = 1;
        struct kernel_object *object = object_create(
            KOBJECT_SOCKET, index + 1, socket_destroy);
        if (!object) {
            object_release(event);
            state->event = 0;
            state->active = 0;
        }
        return object;
    }
    return 0;
}

int socket_stream_connect(struct kernel_object *object,
                          struct kernel_object *interface,
                          u32 destination, u16 port) {
    struct socket_state *state = state_for(object);
    if (!state || state->family != 14 || state->bound || !interface ||
        !destination || !port || object_retain(interface))
        return -1;
    struct tcp_context *tcp = 0;
    u64 connection = 0;
    if (net_interface_tcp_connect(interface, destination, port,
                                  &tcp, &connection)) {
        object_release(interface);
        return -1;
    }
    state->interface = interface;
    state->tcp = tcp;
    state->tcp_connection = connection;
    state->bound = 1;
    return 0;
}

int socket_stream_listen(struct kernel_object *object,
                         struct kernel_object *interface,
                         u16 port, u32 backlog) {
    struct socket_state *state = state_for(object);
    if (!state || state->family != 14 || state->bound || !interface ||
        !port || !backlog || backlog > 32 || object_retain(interface))
        return -1;
    struct tcp_context *tcp = 0;
    u64 listener = 0;
    if (net_interface_tcp_listen(interface, port, &tcp, &listener) ||
        tcp_set_listener_backlog(tcp, listener, backlog)) {
        if (listener) tcp_close(tcp, listener);
        object_release(interface);
        return -1;
    }
    state->interface = interface;
    state->tcp = tcp;
    state->tcp_connection = listener;
    state->bound = 1;
    state->listening = 1;
    state->backlog = backlog;
    return 0;
}

int socket_stream_listen_ipv6(struct kernel_object *object,
                              struct kernel_object *interface,
                              u16 port, u32 backlog) {
    struct socket_state *state = state_for(object);
    if (!state || state->family != 14 || state->bound || !interface ||
        !port || !backlog || backlog > 32 || object_retain(interface))
        return -1;
    struct tcp_context *tcp = 0;
    u64 listener = 0;
    if (net_interface_tcp_listen_ipv6(interface, port, &tcp, &listener) ||
        tcp_set_listener_backlog(tcp, listener, backlog)) {
        if (listener) tcp_close(tcp, listener);
        object_release(interface);
        return -1;
    }
    state->interface = interface;
    state->tcp = tcp;
    state->tcp_connection = listener;
    state->bound = 1;
    state->listening = 1;
    state->backlog = backlog;
    return 0;
}

struct kernel_object *socket_stream_accept(struct kernel_object *object) {
    struct socket_state *listener = state_for(object);
    if (!listener || listener->family != 14 || !listener->listening ||
        !listener->interface || !listener->tcp)
        return 0;
    u64 connection = 0;
    int result = net_interface_tcp_accept(
        listener->interface, listener->tcp_connection, &connection);
    if (result) return 0;
    struct kernel_object *accepted = socket_create_stream();
    struct socket_state *state = state_for(accepted);
    if (!accepted || !state || object_retain(listener->interface)) {
        if (accepted) object_release(accepted);
        tcp_close(listener->tcp, connection);
        return 0;
    }
    state->interface = listener->interface;
    state->tcp = listener->tcp;
    state->tcp_connection = connection;
    state->bound = 1;
    event_signal(state->event);
    return accepted;
}

int socket_stream_send(struct kernel_object *object,
                       const void *data, u32 length) {
    struct socket_state *state = state_for(object);
    return state && state->family == 14 && state->bound ?
        net_interface_tcp_send(state->interface, state->tcp_connection,
                               data, length) : -1;
}

int socket_stream_send_file(struct kernel_object *object,
                            struct kernel_object *node, u32 offset,
                            u32 length) {
    struct socket_state *state = state_for(object);
    if (!state || state->family != 14 || !state->bound)
        return -1;
    u32 size = 0;
    struct kernel_object *pages = vfs_node_pages(node, &size);
    if (!pages) return -1;
    int result = offset > size || length > size - offset ? -1 :
        net_interface_tcp_send_pages(state->interface,
                                     state->tcp_connection, pages, offset,
                                     length);
    object_release(pages);
    return result;
}

int socket_stream_receive_file(struct kernel_object *object,
                               struct kernel_object *node, u32 offset,
                               u32 length) {
    struct socket_state *state = state_for(object);
    if (!state || state->family != 14 || !state->bound)
        return -1;
    u32 size = 0;
    struct kernel_object *pages = vfs_node_pages(node, &size);
    if (!pages) return -1;
    int result = offset > size || length > size - offset ? -1 :
        net_interface_tcp_receive_pages(state->interface,
                                        state->tcp_connection, pages,
                                        offset, length);
    object_release(pages);
    return result;
}

int socket_stream_receive(struct kernel_object *object,
                          void *data, u32 capacity, u32 *received) {
    struct socket_state *state = state_for(object);
    return state && state->family == 14 && state->bound ?
        net_interface_tcp_receive(state->interface, state->tcp_connection,
                                  data, capacity, received) : -1;
}

int socket_stream_shutdown(struct kernel_object *object) {
    struct socket_state *state = state_for(object);
    return state && state->family == 14 && state->bound ?
        net_interface_tcp_shutdown(state->interface,
                                   state->tcp_connection) : -1;
}

int socket_stream_state(struct kernel_object *object,
                        u32 *state_out, u32 *readiness,
                        i32 *error_out, u32 *eof_out,
                        u32 *granted_out) {
    struct socket_state *state = state_for(object);
    if (!state || state->family != 14 || !state->bound ||
        !state_out || !readiness || !error_out || !eof_out || !granted_out)
        return -1;
    *readiness = 0;
    if (state->listening) {
        *state_out = TCP_STATE_LISTEN;
        *error_out = 0;
        *eof_out = 0;
        int pending = tcp_accept_pending(state->tcp, state->tcp_connection);
        if (pending < 0) return -1;
        if (pending) *readiness |= SOCKET_READY_ACCEPT;
        return 0;
    }
    u32 readable;
    u32 writable;
    if (tcp_connection_status(state->tcp, state->tcp_connection,
                              state_out, &readable, &writable,
                              error_out, eof_out, granted_out))
        return -1;
    if (*state_out == TCP_STATE_ESTABLISHED)
        *readiness |= SOCKET_READY_CONNECTED;
    if (readable) *readiness |= SOCKET_READY_READABLE;
    if (writable) *readiness |= SOCKET_READY_WRITABLE;
    if (*eof_out || *state_out == TCP_STATE_CLOSE_WAIT ||
        *state_out == TCP_STATE_LAST_ACK ||
        *state_out == TCP_STATE_CLOSING ||
        *state_out == TCP_STATE_CLOSED ||
        *state_out == TCP_STATE_TIME_WAIT)
        *readiness |= SOCKET_READY_HANGUP;
    if (*error_out) *readiness |= SOCKET_READY_ERROR;
    return 0;
}

int socket_stream_take_error(struct kernel_object *object, i32 *error) {
    struct socket_state *state = state_for(object);
    return state && state->family == 14 && state->bound && error ?
        net_interface_tcp_take_error(state->interface,
                                     state->tcp_connection, error) : -1;
}

void socket_tcp_abort_context(struct tcp_context *tcp, i32 error) {
    if (!tcp || !error) return;
    tcp_abort_all(tcp, error);
    for (u32 index = 0; index < SOCKET_MAX; index++) {
        struct socket_state *state = &sockets[index];
        if (!state->active || state->family != 14 || state->tcp != tcp ||
            !state->tcp_connection)
            continue;
        if (state->event) event_signal(state->event);
    }
}

void socket_tcp_notify(struct tcp_context *tcp, u64 connection_id) {
    if (!tcp || !connection_id) return;
    u64 listener_id = 0;
    tcp_parent_listener(tcp, connection_id, &listener_id);
    for (u32 index = 0; index < SOCKET_MAX; index++) {
        struct socket_state *state = &sockets[index];
        if (!state->active || state->family != 14 || state->tcp != tcp ||
            !state->event)
            continue;
        if (state->tcp_connection == connection_id ||
            (state->listening && state->tcp_connection == listener_id))
            event_signal(state->event);
    }
}

int socket_bind(struct kernel_object *object, u32 address, u16 port) {
    struct socket_state *state = state_for(object);
    struct socket_udp_context *context = context_for_address(address);
    if (!state || state->family != 4 || state->bound || !context) return -1;
    u64 binding = udp_bind(context->udp, address, port);
    if (!binding) return -1;
    state->udp = context->udp;
    state->binding_id = binding;
    state->bound = 1;
    return 0;
}

int socket_bind_ipv6(struct kernel_object *object,
                     const u8 address[IPV6_ADDRESS_SIZE], u16 port) {
    struct socket_state *state = state_for(object);
    if (!state || state->family != 6 || state->bound || !address) return -1;
    struct socket_udp_context *selected = 0;
    for (u32 index = 0; index < SOCKET_UDP_CONTEXT_MAX; index++) {
        struct socket_udp_context *context = &udpv6_contexts[index];
        if (context->active && context->udpv6 &&
            ipv6_has_local_address(context->udpv6->ipv6, address)) {
            selected = context;
            break;
        }
    }
    if (!selected) return -1;
    u64 binding = udpv6_bind(selected->udpv6, address, port);
    if (!binding) return -1;
    state->udpv6 = selected->udpv6;
    state->binding_id = binding;
    state->bound = 1;
    return 0;
}

int socket_send_to(struct kernel_object *object, u32 address, u16 port,
                   const void *payload, u32 length) {
    struct socket_state *state = state_for(object);
    if (!state || state->family != 4 || !state->bound ||
        !state->udp || !address || !port)
        return -1;
    const struct route_entry *route = route_lookup(socket_routes, address);
    struct socket_udp_context *context = context_for_route(route);
    if (!context || context->udp != state->udp) return -1;
    return udp_send(state->udp, state->binding_id, address, port,
                    payload, length);
}

int socket_send_to_ipv6(struct kernel_object *object,
                        const u8 address[IPV6_ADDRESS_SIZE], u16 port,
                        const void *payload, u32 length) {
    struct socket_state *state = state_for(object);
    if (!state || state->family != 6 || !state->bound ||
        !state->udpv6 || !address || !port)
        return -1;
    return udpv6_send(state->udpv6, state->binding_id,
                      address, port, payload, length);
}

int socket_receive_from_ipv6(struct kernel_object *object,
                             struct udpv6_datagram *datagram) {
    struct socket_state *state = state_for(object);
    return state && state->family == 6 && state->bound && state->udpv6 ?
        udpv6_receive(state->udpv6, state->binding_id, datagram) : -1;
}

int socket_receive_from(struct kernel_object *object,
                        struct udp_datagram *datagram) {
    struct socket_state *state = state_for(object);
    return state && state->bound && state->udp ?
        udp_receive(state->udp, state->binding_id, datagram) : -1;
}

int socket_local_address(struct kernel_object *object,
                         u32 *address, u16 *port) {
    struct socket_state *state = state_for(object);
    return state && state->bound && state->udp ?
        udp_binding_local(state->udp, state->binding_id, address, port) : -1;
}

struct kernel_object *socket_wait_event(struct kernel_object *object) {
    struct socket_state *state = state_for(object);
    if (!state || !state->bound) return 0;
    if (state->family == 4 && state->udp)
        return udp_binding_event(state->udp, state->binding_id);
    if (state->family == 6 && state->udpv6)
        return udpv6_binding_event(state->udpv6, state->binding_id);
    if (state->family == 14 && state->event) return state->event;
    return 0;
}

u32 socket_active_count(void) {
    u32 count = 0;
    for (u32 index = 0; index < SOCKET_MAX; index++)
        if (sockets[index].active) count++;
    return count;
}

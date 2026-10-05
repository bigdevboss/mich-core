#include "types.h"
#include "posix_abi.h"
#include "posix_socket.h"
#include "posix_fd.h"
#include "posix_pledge.h"
#include "posix_profile.h"
#include "posix_vfs.h"
#include "task.h"
#include "socket.h"
#include "net_test.h"
#include "tests64.h"

// The park paths need a live scheduler and peer tasks, which the prod demo
// exercises; the unit scope is what the layer alone can prove over a
// loopback runtime the test raises itself, the way the udp socket test
// does: the family, type and state gates, the datagram round trip, the
// option surface, and the descriptor lifecycle.

static void fill_address(struct posix_sockaddr_in *address, u32 host_address,
                         u16 host_port) {
    address->family = POSIX_AF_INET;
    address->port = (u16)((host_port >> 8) | (host_port << 8));
    address->address = ((host_address & 0x000000FFu) << 24) |
                       ((host_address & 0x0000FF00u) << 8) |
                       ((host_address & 0x00FF0000u) >> 8) |
                       ((host_address & 0xFF000000u) >> 24);
    for (u32 index = 0; index < 8; index++) address->zero[index] = 0;
}

static int same_address(const struct posix_sockaddr_in *left,
                        const struct posix_sockaddr_in *right) {
    if (left->family != right->family || left->port != right->port ||
        left->address != right->address)
        return 0;
    for (u32 index = 0; index < 8; index++)
        if (left->zero[index] != right->zero[index]) return 0;
    return 1;
}

int test_posix_socket64(const struct test64_env *env) {
    struct task *owner = env->owner;
    int valid = 1;

    // The unit battery runs before the boot network runtime, so the test
    // raises the same loopback contexts the udp socket test does: a port,
    // an ipv4 loopback address, a udp context, and a loopback route the
    // socket layer can resolve through.
    static struct ethernet_port port;
    static struct ipv4_context ipv4;
    static struct loopback_context loopback;
    static struct udp_context udp;
    static struct route_table routes;
    const u8 mac[6] = {0x02, 0x4D, 0x49, 0x43, 0x48, 0x60};
    route_init(&routes);
    int ready = !ethernet_port_init(&port, mac) &&
        !ipv4_init(&ipv4, &port, 0x7F000001u, 0xFF000000u, 0) &&
        !loopback_init(&loopback, &ipv4, 16) && !udp_init(&udp, &ipv4) &&
        !route_add(&routes, 0x7F000000u, 0xFF000000u, 0,
                   ROUTE_INTERFACE_LOOPBACK, 0) &&
        !socket_init(&udp, &routes);
    if (!ready) return -1;
    u32 sockets = socket_active_count();

    // The family and type gates answer the posix errors, not near misses.
    valid = valid && posix_socket_socket(owner, 10, POSIX_SOCK_DGRAM, 0) ==
        POSIX_SOCKET_EAFNOSUPPORT;
    valid = valid && posix_socket_socket(owner, POSIX_AF_INET, 5, 0) ==
        POSIX_SOCKET_EOPNOTSUPP;
    valid = valid && posix_socket_socket(owner, POSIX_AF_INET,
                                         POSIX_SOCK_DGRAM, 17) ==
        POSIX_SOCKET_EPROTONOSUPPORT;

    int receiver = posix_socket_socket(owner, POSIX_AF_INET,
                                       POSIX_SOCK_DGRAM, 0);
    int sender = posix_socket_socket(owner, POSIX_AF_INET, POSIX_SOCK_DGRAM,
                                     0);
    int stream = posix_socket_socket(owner, POSIX_AF_INET,
                                     POSIX_SOCK_STREAM, 0);
    valid = valid && receiver >= 0 && sender >= 0 && stream >= 0;
    valid = valid && socket_active_count() == sockets + 3u;

    // bind rows: a taken port answers EADDRINUSE, a second bind on the
    // same descriptor EINVAL, and an address no context owns EADDRINUSE
    // too, because the stack does not distinguish those failures.
    struct posix_sockaddr_in receiver_address;
    fill_address(&receiver_address, 0x7F000001u, 31000);
    valid = valid && posix_socket_bind(owner, receiver,
                                       &receiver_address) == 0;
    struct posix_sockaddr_in named;
    valid = valid && posix_socket_getsockname(owner, receiver, &named) == 0;
    valid = valid && same_address(&named, &receiver_address);
    valid = valid && posix_socket_bind(owner, receiver,
                                       &receiver_address) ==
        POSIX_VFS_EINVAL;
    int rival = posix_socket_socket(owner, POSIX_AF_INET, POSIX_SOCK_DGRAM,
                                    0);
    valid = valid && rival >= 0;
    valid = valid && posix_socket_bind(owner, rival,
                                       &receiver_address) ==
        POSIX_SOCKET_EADDRINUSE;
    struct posix_sockaddr_in away;
    fill_address(&away, 0xC0A80105u, 31000);
    valid = valid && posix_socket_bind(owner, rival, &away) ==
        POSIX_SOCKET_EADDRINUSE;
    struct posix_sockaddr_in sender_address;
    fill_address(&sender_address, 0x7F000001u, 31001);
    valid = valid && posix_socket_bind(owner, sender, &sender_address) == 0;

    // The send gates: an unbound datagram socket, a zero port and a
    // foreign family all refuse before any packet is built.
    static const u8 payload[5] = {'m', 'i', 'c', 'h', '!'};
    valid = valid && posix_socket_send_to(owner, rival, &receiver_address,
                                          payload, sizeof(payload)) ==
        POSIX_VFS_EINVAL;
    struct posix_sockaddr_in no_port;
    fill_address(&no_port, 0x7F000001u, 0);
    valid = valid && posix_socket_send_to(owner, sender, &no_port, payload,
                                          sizeof(payload)) ==
        POSIX_SOCKET_EDESTADDRREQ;
    no_port.family = 10;
    no_port.port = 31000;
    valid = valid && posix_socket_send_to(owner, sender, &no_port, payload,
                                          sizeof(payload)) ==
        POSIX_SOCKET_EDESTADDRREQ;
    valid = valid && posix_socket_send_to(owner, sender, &receiver_address,
                                          payload, 0) == POSIX_VFS_EINVAL;

    // The loopback round trip: one datagram lands whole with its source
    // recorded, and the plain read answers the next one without an
    // address slot.
    valid = valid && posix_socket_send_to(owner, sender,
                                          &receiver_address, payload,
                                          sizeof(payload)) ==
        (i64)sizeof(payload);
    u8 buffer[POSIX_IO_MAX];
    struct posix_sockaddr_in source;
    valid = valid && posix_socket_recv_from(owner, receiver, &source,
                                            buffer, sizeof(buffer), 0) ==
        (i64)sizeof(payload);
    for (u32 index = 0; index < sizeof(payload); index++)
        valid = valid && buffer[index] == payload[index];
    valid = valid && same_address(&source, &sender_address);
    static const u8 second[4] = {'e', 'c', 'h', 'o'};
    valid = valid && posix_socket_send_to(owner, sender,
                                          &receiver_address, second,
                                          sizeof(second)) ==
        (i64)sizeof(second);
    valid = valid && posix_socket_io_read(owner, receiver, buffer,
                                          sizeof(buffer), 0) ==
        (i64)sizeof(second);
    for (u32 index = 0; index < sizeof(second); index++)
        valid = valid && buffer[index] == second[index];
    // A datagram write has no destination to name, so the plain write
    // refuses rather than guess one.
    valid = valid && posix_socket_io_write(owner, sender, second,
                                           sizeof(second), 0) ==
        POSIX_SOCKET_EDESTADDRREQ;

    // The option surface: the four names the profile answers, and the
    // loud refusal outside them.
    u32 length = 32;
    u8 value[32];
    valid = valid && posix_socket_getsockopt(owner, receiver,
                                             POSIX_SOL_SOCKET, POSIX_SO_TYPE,
                                             value, &length) == 0;
    valid = valid && length == 4 &&
        value[0] == POSIX_SOCK_DGRAM && !value[1] && !value[2] && !value[3];
    length = 32;
    valid = valid && posix_socket_getsockopt(owner, stream,
                                             POSIX_SOL_SOCKET, POSIX_SO_TYPE,
                                             value, &length) == 0;
    valid = valid && length == 4 && value[0] == POSIX_SOCK_STREAM;
    length = 32;
    valid = valid && posix_socket_getsockopt(owner, receiver,
                                             POSIX_SOL_SOCKET,
                                             POSIX_SO_ERROR, value,
                                             &length) == 0 &&
        !value[0] && !value[1] && !value[2] && !value[3];
    length = 32;
    valid = valid && posix_socket_getsockopt(owner, receiver, 6,
                                             POSIX_SO_TYPE, value,
                                             &length) ==
        POSIX_SOCKET_ENOPROTOOPT;
    valid = valid && posix_socket_getsockopt(owner, receiver,
                                             POSIX_SOL_SOCKET, 999, value,
                                             &length) ==
        POSIX_SOCKET_ENOPROTOOPT;
    valid = valid && posix_socket_setsockopt(owner, receiver,
                                             POSIX_SOL_SOCKET,
                                             POSIX_SO_REUSEADDR, value,
                                             4) == 0;
    valid = valid && posix_socket_setsockopt(owner, receiver,
                                             POSIX_SOL_SOCKET, 999, value,
                                             4) ==
        POSIX_SOCKET_ENOPROTOOPT;

    // The stream state gates: no route to a foreign net answers
    // EHOSTUNREACH, the unconnected calls ENOTCONN, and a listen without
    // a real interface behind the address answers EADDRNOTAVAIL, because
    // the loopback runtime carries datagrams only in v0.
    struct posix_sockaddr_in foreign;
    fill_address(&foreign, 0x0A090909u, 80);
    valid = valid && posix_socket_connect(owner, stream, &foreign, 0) ==
        POSIX_SOCKET_EHOSTUNREACH;
    valid = valid && posix_socket_send(owner, stream, payload,
                                       sizeof(payload), 0) ==
        POSIX_SOCKET_ENOTCONN;
    valid = valid && posix_socket_recv(owner, stream, buffer,
                                       sizeof(buffer), 0) ==
        POSIX_SOCKET_ENOTCONN;
    valid = valid && posix_socket_shutdown(owner, stream,
                                           POSIX_SHUT_RDWR) ==
        POSIX_SOCKET_ENOTCONN;
    valid = valid && posix_socket_getpeername(owner, stream, &named) ==
        POSIX_SOCKET_ENOTCONN;
    struct posix_sockaddr_in stream_address;
    fill_address(&stream_address, 0x7F000001u, 31010);
    valid = valid && posix_socket_listen(owner, stream, 8) ==
        POSIX_VFS_EINVAL;
    valid = valid && posix_socket_bind(owner, stream, &stream_address) == 0;
    valid = valid && posix_socket_listen(owner, stream, 8) ==
        POSIX_SOCKET_EADDRNOTAVAIL;
    valid = valid && posix_socket_connect(owner, receiver, &foreign, 0) ==
        POSIX_SOCKET_EOPNOTSUPP;
    valid = valid && posix_socket_listen(owner, receiver, 8) ==
        POSIX_SOCKET_EOPNOTSUPP;

    // The descriptor lifecycle: a dup keeps the kernel socket alive, and
    // the last close returns it to the pool.
    int alias = posix_fd_dup(owner, receiver);
    valid = valid && alias >= 0;
    valid = valid && posix_fd_close(owner, receiver) == 0;
    receiver = alias;
    valid = valid && socket_active_count() == sockets + 4u;
    valid = valid && posix_socket_send_to(owner, sender, &receiver_address,
                                          payload, sizeof(payload)) ==
        (i64)sizeof(payload);
    valid = valid && posix_socket_recv_from(owner, receiver, &source,
                                            buffer, sizeof(buffer), 0) ==
        (i64)sizeof(payload);

    // The promise gate row: sockets ride with net the way OpenBSD carries
    // them, and a narrowed set denies them loudly or quietly.
    struct task *probe = task_alloc_slot();
    if (probe && !posix_profile_admit(probe)) {
        valid = valid && posix_pledge_promise(probe, "net error", 0) == 0;
        valid = valid && posix_pledge_gate(probe,
                                           POSIX_SYSCALL_SOCKET) == 0;
        valid = valid && posix_pledge_promise(probe, "error", 0) == 0;
        valid = valid && posix_pledge_gate(probe,
                                           POSIX_SYSCALL_SOCKET) ==
                        POSIX_PLEDGE_ENOSYS;
        valid = valid && posix_pledge_promise(probe, "", 0) == 0;
        valid = valid && posix_pledge_gate(probe,
                                           POSIX_SYSCALL_SOCKET) == 1;
        posix_pledge_reset(probe);
        posix_profile_release(probe);
        task_free_slot(probe);
    } else {
        if (probe) task_free_slot(probe);
        valid = 0;
    }

    valid = valid && posix_fd_close(owner, receiver) == 0;
    valid = valid && posix_fd_close(owner, sender) == 0;
    valid = valid && posix_fd_close(owner, stream) == 0;
    valid = valid && posix_fd_close(owner, rival) == 0;
    valid = valid && socket_active_count() == sockets;

    return valid ? 0 : -1;
}

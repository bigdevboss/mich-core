#include "posix_socket.h"
#include "posix_poll.h"
#include "posix_fd.h"
#include "posix_signal.h"
#include "posix_vfs.h"
#include "task.h"
#include "vm64.h"
#include "scheduler.h"
#include "runtime64.h"
#include "object.h"
#include "socket.h"
#include "socket_abi.h"

// The parked dispatch frame is abandoned on the switch, so the net wake
// that completes the operation patches the parked caller's request in
// place: the data array, the transferred word, and for the from calls the
// source address, all at fixed offsets the request layouts define.
#define STD_TRANSFERRED_OFFSET 8u
#define STD_DATA_OFFSET 12u
#define MSG_ADDRESS_OFFSET 0u
#define MSG_ADDRESS_LENGTH_OFFSET 16u

#define POSIX_SOCKET_WAIT_NONE 0u
#define POSIX_SOCKET_WAIT_CONNECT 1u
#define POSIX_SOCKET_WAIT_ACCEPT 2u
#define POSIX_SOCKET_WAIT_RECV 3u
#define POSIX_SOCKET_WAIT_RECVFROM 4u
#define POSIX_SOCKET_WAIT_SEND 5u

// The answer a completion attempt gives while the wait stays open, so a
// park that ran one knows to fall through to the blocking switch. It sits
// far below the POSIX error range on purpose.
#define POSIX_SOCKET_WAIT_OPEN (-4096)

// Which user layout a parked request patches: the plain io pair the read
// and write paths share, the socket io request the dedicated calls use,
// or a message header whose vectors the wake scatters into.
#define POSIX_SOCKET_FLAVOR_STD 0u
#define POSIX_SOCKET_FLAVOR_IO 1u
#define POSIX_SOCKET_FLAVOR_MSG 2u

struct posix_socket_wait {
    struct kernel_object *socket;
    uptr_t request;
    u32 descriptor;
    u32 kind;
    u32 flavor;
    u32 length;
    u32 offset;
    struct posix_msghdr message;
    u8 staging[POSIX_IO_MAX];
};

static struct posix_socket_wait waits[MAX_TASKS];

// The zero message a park without vectors records, so the wake scatter
// skips it uniformly instead of testing the flavor twice.
static const struct posix_msghdr empty_message;

// Syscalls run with interrupts masked on this single CPU, so socket wait
// state mutates from one context at a time: the syscall that parks, the
// net wake that completes, or the signal that interrupts. The pipe layer
// relies on the same serialization.

static u16 byte_swap16(u16 value) {
    return (u16)((value >> 8) | (value << 8));
}

static u32 byte_swap32(u32 value) {
    return ((value & 0x000000FFu) << 24) | ((value & 0x0000FF00u) << 8) |
           ((value & 0x00FF0000u) >> 8) | ((value & 0xFF000000u) >> 24);
}

static void sockaddr_fill(struct posix_sockaddr_in *out, u32 address,
                          u16 port) {
    out->family = POSIX_AF_INET;
    out->port = byte_swap16(port);
    out->address = byte_swap32(address);
    for (u32 index = 0; index < 8; index++) out->zero[index] = 0;
}

static void copy_word(u8 *destination, u32 word) {
    for (u32 index = 0; index < 4; index++)
        destination[index] = (u8)(word >> (8 * index));
}

// The stream readiness probe every blocking path shares: it answers the
// error the connection carries, or 0 when the wait is only back-pressure.
static i64 stream_take_error(struct kernel_object *socket) {
    i32 error = 0;
    if (socket_stream_take_error(socket, &error)) return POSIX_VFS_EIO;
    return error ? (i64)error : POSIX_VFS_EIO;
}

static i64 complete_wait(u32 slot);

static i64 socket_park(struct task *task, struct kernel_object *socket,
                       u32 descriptor, u32 kind, u32 flavor, uptr_t request,
                       u32 length, u32 offset, const u8 *staging,
                       const struct posix_msghdr *message) {
    int slot = scheduler_current();
    if (slot < 0 || &task_pool[slot] != task) return POSIX_VFS_EIO;
    if (length > POSIX_IO_MAX) return POSIX_VFS_EINVAL;
    if (object_retain(socket)) return POSIX_VFS_EIO;
    waits[slot].socket = socket;
    waits[slot].request = request;
    waits[slot].descriptor = (u32)descriptor;
    waits[slot].kind = kind;
    waits[slot].flavor = flavor;
    waits[slot].length = length;
    waits[slot].offset = offset;
    waits[slot].message = message ? *message : empty_message;
    for (u32 index = 0; index < length; index++)
        waits[slot].staging[index] = staging ? staging[index] : 0;
    task_state_set(task, TASK_BLOCKED_SOCKET);
    // A notification that lands between the caller's readiness read and
    // this commit reaches no parked waiter, and the socket keeps that
    // fact: one completion attempt spends it instead of sleeping a wakeup
    // away, which is how a half-close read parked forever.
    if (socket_stream_take_notify(socket)) {
        i64 answer = complete_wait(slot);
        if (answer != POSIX_SOCKET_WAIT_OPEN) return answer;
    }
    if (scheduler_pick_next(slot) < 0) {
        // Nothing else can run, so the park would freeze the CPU inside
        // the syscall; the pipe and ipc parks answer the same deadlock.
        waits[slot].socket = 0;
        waits[slot].kind = POSIX_SOCKET_WAIT_NONE;
        waits[slot].request = 0;
        task_state_set(task, TASK_RUNNING);
        object_release(socket);
        return POSIX_VFS_EDEADLK;
    }
    return task64_block_switch();
}

static i64 finish_wait(u32 slot, i64 answer) {
    struct kernel_object *socket = waits[slot].socket;
    waits[slot].socket = 0;
    waits[slot].request = 0;
    waits[slot].descriptor = 0;
    waits[slot].kind = POSIX_SOCKET_WAIT_NONE;
    waits[slot].flavor = 0;
    waits[slot].length = 0;
    waits[slot].offset = 0;
    task_state_set(&task_pool[slot], TASK_RUNNING);
    if (socket) object_release(socket);
    task64_set_result(slot, answer);
    return answer;
}

// Hand received bytes to a parked caller across whichever request layout
// it parked with. The message flavor scatters through the recorded
// vectors; the io flavors land in one flat data array.
static int deliver_received(u32 slot, const u8 *data, u32 length) {
    struct task *task = &task_pool[slot];
    uptr_t request = waits[slot].request;
    u32 flavor = waits[slot].flavor;
    if (flavor == POSIX_SOCKET_FLAVOR_IO ||
        flavor == POSIX_SOCKET_FLAVOR_STD) {
        u32 data_offset = flavor == POSIX_SOCKET_FLAVOR_STD ?
            STD_DATA_OFFSET : POSIX_SOCKET_IO_DATA_OFFSET;
        u32 transferred_offset = flavor == POSIX_SOCKET_FLAVOR_STD ?
            STD_TRANSFERRED_OFFSET : POSIX_SOCKET_IO_TRANSFERRED_OFFSET;
        if (vm64_copy_to(task->page_dir, request + data_offset, data,
                         length))
            return -1;
        u8 word[4];
        copy_word(word, length);
        return vm64_copy_to(task->page_dir, request + transferred_offset,
                            word, sizeof(word)) ? -1 : 0;
    }
    const struct posix_msghdr *message = &waits[slot].message;
    u32 written = 0;
    for (u32 index = 0; index < message->iov_count &&
                       index < POSIX_MSG_IOV_MAX; index++) {
        if (written >= length) break;
        u32 take = message->iov[index].length;
        if (take > length - written) take = length - written;
        if (take && vm64_copy_to(task->page_dir, message->iov[index].base,
                                 data + written, take))
            return -1;
        written += take;
    }
    return 0;
}

// Record the source a datagram came from in the parked caller's request:
// the socket io layout carries the address inline, the message header
// carries it as the name, and the plain read has no address slot at all.
static int deliver_source(u32 slot, u32 address, u16 port) {
    struct task *task = &task_pool[slot];
    u32 flavor = waits[slot].flavor;
    if (flavor == POSIX_SOCKET_FLAVOR_STD) return 0;
    struct posix_sockaddr_in source;
    sockaddr_fill(&source, address, port);
    u32 address_offset = flavor == POSIX_SOCKET_FLAVOR_MSG ?
        MSG_ADDRESS_OFFSET : 8u;
    u32 length_offset = flavor == POSIX_SOCKET_FLAVOR_MSG ?
        MSG_ADDRESS_LENGTH_OFFSET : 24u;
    if (vm64_copy_to(task->page_dir, waits[slot].request + address_offset,
                     &source, sizeof(source)))
        return -1;
    u32 length = POSIX_SOCKADDR_IN_SIZE;
    return vm64_copy_to(task->page_dir, waits[slot].request + length_offset,
                        &length, sizeof(length)) ? -1 : 0;
}

static i64 complete_wait(u32 slot) {
    struct task *task = &task_pool[slot];
    struct kernel_object *socket = waits[slot].socket;
    u32 kind = waits[slot].kind;

    if (kind == POSIX_SOCKET_WAIT_CONNECT) {
        u32 state = 0;
        u32 readiness = 0;
        u32 eof = 0;
        u32 granted = 0;
        i32 error = 0;
        if (socket_stream_state(socket, &state, &readiness, &error, &eof,
                                &granted)) {
            return finish_wait(slot, POSIX_VFS_EIO);
        }
        if (readiness & SOCKET_READY_CONNECTED) {
            // The connect flag lands here rather than at initiation, so
            // an interrupted or still settling connect stays answerable.
            posix_fd_socket_update(task, waits[slot].descriptor,
                                   POSIX_SOCKET_OFD_CONNECTED, 0, 0);
            return finish_wait(slot, 0);
        }
        if (readiness & (SOCKET_READY_ERROR | SOCKET_READY_HANGUP)) {
            return finish_wait(slot, stream_take_error(socket));
        }
        return POSIX_SOCKET_WAIT_OPEN;
    }

    if (kind == POSIX_SOCKET_WAIT_ACCEPT) {
        u32 state = 0;
        u32 readiness = 0;
        u32 eof = 0;
        u32 granted = 0;
        i32 error = 0;
        if (socket_stream_state(socket, &state, &readiness, &error, &eof,
                                &granted)) {
            return finish_wait(slot, POSIX_VFS_EIO);
        }
        if (readiness & SOCKET_READY_ERROR) {
            return finish_wait(slot, stream_take_error(socket));
        }
        if (!(readiness & SOCKET_READY_ACCEPT)) return POSIX_SOCKET_WAIT_OPEN;
        struct kernel_object *accepted = socket_stream_accept(socket);
        if (!accepted) {
            return finish_wait(slot, POSIX_VFS_EIO);
        }
        int fresh = posix_fd_install_socket(task, accepted,
                                            POSIX_SOCK_STREAM);
        object_release(accepted);
        if (fresh < 0) {
            return finish_wait(slot, POSIX_VFS_EMFILE);
        }
        posix_fd_socket_update(task, fresh, POSIX_SOCKET_OFD_CONNECTED, 0,
                               0);
        return finish_wait(slot, (i64)fresh);
    }

    if (kind == POSIX_SOCKET_WAIT_RECV) {
        u32 state = 0;
        u32 readiness = 0;
        u32 eof = 0;
        u32 granted = 0;
        i32 error = 0;
        if (socket_stream_state(socket, &state, &readiness, &error, &eof,
                                &granted)) {
            return finish_wait(slot, POSIX_VFS_EIO);
        }
        if (readiness & SOCKET_READY_ERROR) {
            return finish_wait(slot, stream_take_error(socket));
        }
        if (readiness & SOCKET_READY_READABLE) {
            u8 staging[POSIX_IO_MAX];
            u32 capacity = waits[slot].length;
            if (capacity > POSIX_IO_MAX) capacity = POSIX_IO_MAX;
            u32 received = 0;
            if (socket_stream_receive(socket, staging, capacity,
                                      &received) ||
                !received) {
                return finish_wait(slot, POSIX_VFS_EIO);
            }
            if (deliver_received(slot, staging, received)) {
                return finish_wait(slot, POSIX_VFS_EIO);
            }
            return finish_wait(slot, (i64)received);
        }
        if (eof || (readiness & SOCKET_READY_HANGUP)) {
            // The buffered bytes drained above outrank the hangup, so a
            // zero here only answers after the last byte left.
            return finish_wait(slot, 0);
        }
        return POSIX_SOCKET_WAIT_OPEN;
    }

    if (kind == POSIX_SOCKET_WAIT_RECVFROM) {
        static struct udp_datagram datagram;
        if (!socket_receive_from(socket, &datagram)) {
            u32 capacity = waits[slot].length;
            u32 take = datagram.length;
            if (take > capacity) take = capacity;
            if (deliver_received(slot, datagram.payload, take) ||
                deliver_source(slot, datagram.source_address,
                               datagram.source_port)) {
                return finish_wait(slot, POSIX_VFS_EIO);
            }
            return finish_wait(slot, (i64)take);
        }
        return POSIX_SOCKET_WAIT_OPEN;
    }

    if (kind == POSIX_SOCKET_WAIT_SEND) {
        u32 total = waits[slot].length;
        u32 offset = waits[slot].offset;
        if (offset >= total) {
            return finish_wait(slot, (i64)total);
        }
        u32 remaining = total - offset;
        if (socket_stream_send(socket, waits[slot].staging + offset,
                               remaining)) {
            u32 state = 0;
            u32 readiness = 0;
            u32 eof = 0;
            u32 granted = 0;
            i32 error = 0;
            if (socket_stream_state(socket, &state, &readiness, &error,
                                    &eof, &granted)) {
                return finish_wait(slot, POSIX_VFS_EIO);
            }
            if (readiness & (SOCKET_READY_ERROR | SOCKET_READY_HANGUP)) {
                // Bytes already queued are lost when the peer goes away,
                // which POSIX answers EPIPE for; v0 reports the errno and
                // leaves the signal ride to the stream demo.
                i32 error_taken = 0;
                if (!socket_stream_take_error(socket, &error_taken) &&
                    error_taken) {
                    return finish_wait(slot, (i64)error_taken);
                }
                return finish_wait(slot, POSIX_VFS_EPIPE);
            }
            // Back-pressure only: stay parked until the acks open room.
            return POSIX_SOCKET_WAIT_OPEN;
        }
        waits[slot].offset = total;
        return finish_wait(slot, (i64)total);
    }
    // Nothing the wait was for happened, so the park keeps sleeping until
    // the next change on the socket tries again.
    return POSIX_SOCKET_WAIT_OPEN;
}

// The net layer hands a socket slot index here on every state change it
// signals, and each parked waiter on that socket gets one completion try.
static u32 posix_socket_wake_index(u32 index) {
    u32 matched = 0;
    for (u32 slot = 0; slot < (u32)task_pool_count; slot++) {
        if (task_pool[slot].state != TASK_BLOCKED_SOCKET ||
            !waits[slot].socket ||
            waits[slot].socket->value != (u64)index + 1u)
            continue;
        matched++;
        complete_wait(slot);
    }
    // The poll waiters ride the same change: one scan covers whichever
    // sockets each of them listed.
    posix_poll_notify();
    return matched;
}

i64 posix_socket_signal(struct task *target) {
    u32 slot = (u32)(target - task_pool);
    if (slot >= (u32)MAX_TASKS || target->state != TASK_BLOCKED_SOCKET ||
        !waits[slot].socket)
        return 0;
    struct kernel_object *socket = waits[slot].socket;
    waits[slot].socket = 0;
    waits[slot].kind = POSIX_SOCKET_WAIT_NONE;
    waits[slot].request = 0;
    object_release(socket);
    return POSIX_SIGNAL_EINTR;
}

u16 posix_socket_poll(struct kernel_object *socket, u32 type) {
    if (!socket || socket->type != KOBJECT_SOCKET) return POSIX_POLLNVAL;
    if (type == POSIX_SOCK_DGRAM) {
        u16 ready = POSIX_POLLOUT;
        if (socket_datagram_pending(socket) ||
            socket_stream_take_notify(socket))
            ready |= POSIX_POLLIN;
        return ready;
    }
    u32 state = 0;
    u32 readiness = 0;
    i32 error = 0;
    u32 eof = 0;
    u32 granted = 0;
    if (socket_stream_state(socket, &state, &readiness, &error, &eof,
                            &granted))
        return 0;
    u16 ready = 0;
    if (readiness & SOCKET_READY_ERROR) ready |= POSIX_POLLERR;
    if (readiness & (SOCKET_READY_READABLE | SOCKET_READY_ACCEPT))
        ready |= POSIX_POLLIN;
    if (readiness & SOCKET_READY_WRITABLE) ready |= POSIX_POLLOUT;
    if (readiness & SOCKET_READY_HANGUP) ready |= POSIX_POLLHUP;
    return ready;
}

void posix_socket_init(void) {
    for (u32 slot = 0; slot < MAX_TASKS; slot++) {
        waits[slot].socket = 0;
        waits[slot].request = 0;
        waits[slot].descriptor = 0;
        waits[slot].kind = POSIX_SOCKET_WAIT_NONE;
        waits[slot].flavor = 0;
        waits[slot].length = 0;
        waits[slot].offset = 0;
    }
    socket_set_wake_hook(posix_socket_wake_index);
}

int posix_socket_socket(struct task *task, u32 domain, u32 type,
                        u32 protocol) {
    if (domain != POSIX_AF_INET)
        return POSIX_SOCKET_EAFNOSUPPORT;
    if (type != POSIX_SOCK_STREAM && type != POSIX_SOCK_DGRAM)
        return protocol ? POSIX_SOCKET_EPROTONOSUPPORT :
            POSIX_SOCKET_EOPNOTSUPP;
    if (protocol) return POSIX_SOCKET_EPROTONOSUPPORT;
    struct kernel_object *socket = type == POSIX_SOCK_STREAM ?
        socket_create_stream() : socket_create();
    if (!socket) return POSIX_VFS_EMFILE;
    int descriptor = posix_fd_install_socket(task, socket, type);
    if (descriptor < 0) {
        object_release(socket);
        return descriptor == POSIX_FD_TABLE_FULL ||
            descriptor == POSIX_FD_OFD_FULL ? POSIX_VFS_EMFILE :
            POSIX_VFS_EIO;
    }
    // The install took the reference the ofd owns; this drops the one
    // the creation handed back, so the last close is what destroys.
    object_release(socket);
    return descriptor;
}

int posix_socket_bind(struct task *task, int descriptor,
                      const struct posix_sockaddr_in *address) {
    struct kernel_object *socket = 0;
    u32 type = 0;
    u32 flags = 0;
    int resolved = posix_fd_socket_of(task, descriptor, 0, &socket, &type,
                                      &flags, 0, 0);
    if (resolved) return resolved;
    if (address->family != POSIX_AF_INET) return POSIX_VFS_EINVAL;
    if (flags & (POSIX_SOCKET_OFD_BOUND | POSIX_SOCKET_OFD_LISTENING |
                 POSIX_SOCKET_OFD_CONNECTED))
        return POSIX_VFS_EINVAL;
    u32 network = byte_swap32(address->address);
    u16 port = byte_swap16(address->port);
    if (type == POSIX_SOCK_DGRAM) {
        if (socket_bind(socket, network, port))
            return POSIX_SOCKET_EADDRINUSE;
        struct posix_sockaddr_in local = *address;
        u32 assigned_address = 0;
        u16 assigned_port = 0;
        // An ephemeral ask comes back with the port the binding took, so
        // the name call answers the real one.
        if (!socket_local_address(socket, &assigned_address,
                                  &assigned_port))
            sockaddr_fill(&local, assigned_address, assigned_port);
        return posix_fd_socket_update(
            task, descriptor, flags | POSIX_SOCKET_OFD_BOUND, &local, 0);
    }
    // A stream bind only records: the port applies at listen, and v0
    // takes no kernel-side ephemeral pick for a listening socket.
    if (!port) return POSIX_VFS_EINVAL;
    return posix_fd_socket_update(
        task, descriptor, flags | POSIX_SOCKET_OFD_BOUND, address, 0);
}

int posix_socket_listen(struct task *task, int descriptor, u32 backlog) {
    struct kernel_object *socket = 0;
    u32 type = 0;
    u32 flags = 0;
    struct posix_sockaddr_in local;
    int resolved = posix_fd_socket_of(task, descriptor, 0, &socket, &type,
                                      &flags, &local, 0);
    if (resolved) return resolved;
    if (type != POSIX_SOCK_STREAM) return POSIX_SOCKET_EOPNOTSUPP;
    if (!(flags & POSIX_SOCKET_OFD_BOUND) ||
        (flags & (POSIX_SOCKET_OFD_LISTENING |
                  POSIX_SOCKET_OFD_CONNECTED)))
        return POSIX_VFS_EINVAL;
    // The tcp listener caps its backlog at 32; a zero ask behaves as one
    // rather than refusing, the way most stacks round it up.
    if (!backlog) backlog = 1;
    if (backlog > 32) backlog = 32;
    struct kernel_object *interface =
        socket_interface_for(byte_swap32(local.address));
    if (!interface) return POSIX_SOCKET_EADDRNOTAVAIL;
    if (socket_stream_listen(socket, interface, byte_swap16(local.port),
                             backlog))
        return POSIX_SOCKET_EADDRINUSE;
    return posix_fd_socket_update(
        task, descriptor, flags | POSIX_SOCKET_OFD_LISTENING, 0, 0);
}

i64 posix_socket_accept(struct task *task, int descriptor,
                        uptr_t request) {
    struct kernel_object *socket = 0;
    u32 type = 0;
    u32 flags = 0;
    int resolved = posix_fd_socket_of(task, descriptor, 0, &socket, &type,
                                      &flags, 0, 0);
    if (resolved) return resolved;
    if (type != POSIX_SOCK_STREAM) return POSIX_SOCKET_EOPNOTSUPP;
    if (!(flags & POSIX_SOCKET_OFD_LISTENING)) return POSIX_VFS_EINVAL;
    u32 state = 0;
    u32 readiness = 0;
    u32 eof = 0;
    u32 granted = 0;
    i32 error = 0;
    if (socket_stream_state(socket, &state, &readiness, &error, &eof,
                            &granted))
        return POSIX_VFS_EIO;
    if (readiness & SOCKET_READY_ERROR)
        return stream_take_error(socket);
    if (readiness & SOCKET_READY_ACCEPT) {
        struct kernel_object *accepted = socket_stream_accept(socket);
        if (!accepted) return POSIX_VFS_EIO;
        int fresh = posix_fd_install_socket(task, accepted,
                                            POSIX_SOCK_STREAM);
        object_release(accepted);
        if (fresh < 0)
            return fresh == POSIX_FD_TABLE_FULL ||
                fresh == POSIX_FD_OFD_FULL ? POSIX_VFS_EMFILE :
                POSIX_VFS_EIO;
        posix_fd_socket_update(task, fresh, POSIX_SOCKET_OFD_CONNECTED, 0,
                               0);
        return fresh;
    }
    return socket_park(task, socket, descriptor,
                       POSIX_SOCKET_WAIT_ACCEPT, POSIX_SOCKET_FLAVOR_IO,
                       request, 0, 0, 0, 0);
}

i64 posix_socket_connect(struct task *task, int descriptor,
                         const struct posix_sockaddr_in *address,
                         uptr_t request) {
    struct kernel_object *socket = 0;
    u32 type = 0;
    u32 flags = 0;
    int resolved = posix_fd_socket_of(task, descriptor, 0, &socket, &type,
                                      &flags, 0, 0);
    if (resolved) return resolved;
    if (type != POSIX_SOCK_STREAM) return POSIX_SOCKET_EOPNOTSUPP;
    if (address->family != POSIX_AF_INET) return POSIX_VFS_EINVAL;
    if (flags & POSIX_SOCKET_OFD_CONNECTED) return POSIX_SOCKET_EISCONN;
    u32 destination = byte_swap32(address->address);
    u16 port = byte_swap16(address->port);
    if (!port) return POSIX_VFS_EINVAL;
    if (socket_stream_connect(socket, 0, destination, port))
        return POSIX_SOCKET_EHOSTUNREACH;
    u32 state = 0;
    u32 readiness = 0;
    u32 eof = 0;
    u32 granted = 0;
    i32 error = 0;
    if (socket_stream_state(socket, &state, &readiness, &error, &eof,
                            &granted))
        return POSIX_VFS_EIO;
    if (readiness & SOCKET_READY_CONNECTED) {
        return posix_fd_socket_update(
            task, descriptor, flags | POSIX_SOCKET_OFD_CONNECTED, 0,
            address) ? POSIX_VFS_EIO : 0;
    }
    if (readiness & (SOCKET_READY_ERROR | SOCKET_READY_HANGUP))
        return stream_take_error(socket);
    // The peer name is recorded at initiation rather than at the wake, so
    // a parked connect answers getpeername the moment it connects; the
    // connected flag stays the gate, so a failed or interrupted attempt
    // never exposes the name.
    if (posix_fd_socket_update(task, descriptor, flags, 0, address))
        return POSIX_VFS_EIO;
    return socket_park(task, socket, descriptor,
                       POSIX_SOCKET_WAIT_CONNECT, POSIX_SOCKET_FLAVOR_IO,
                       request, 0, 0, 0, 0);
}

// The stream receive core both recv flavors share. The buffer is kernel
// staging on the syscall side; a park hands the user request over and the
// wake writes through it instead.
static i64 stream_recv(struct task *task, struct kernel_object *socket,
                       int descriptor, u32 type, u32 flags, u8 *buffer,
                       u32 capacity, uptr_t request, u32 flavor) {
    if (!capacity) return 0;
    if (type != POSIX_SOCK_STREAM) return POSIX_SOCKET_ENOTSOCK;
    if (!(flags & POSIX_SOCKET_OFD_CONNECTED)) return POSIX_SOCKET_ENOTCONN;
    u32 state = 0;
    u32 readiness = 0;
    u32 eof = 0;
    u32 granted = 0;
    i32 error = 0;
    if (socket_stream_state(socket, &state, &readiness, &error, &eof,
                            &granted))
        return POSIX_VFS_EIO;
    if (readiness & SOCKET_READY_ERROR) return stream_take_error(socket);
    if (readiness & SOCKET_READY_READABLE) {
        u32 receive_capacity = capacity > POSIX_IO_MAX ?
            POSIX_IO_MAX : capacity;
        u32 received = 0;
        if (socket_stream_receive(socket, buffer, receive_capacity,
                                  &received) ||
            !received)
            return POSIX_VFS_EIO;
        return (i64)received;
    }
    if (eof || (readiness & SOCKET_READY_HANGUP)) return 0;
    return socket_park(task, socket, descriptor, POSIX_SOCKET_WAIT_RECV,
                       flavor, request, capacity, 0, 0, 0);
}

// The datagram receive core recvfrom and the plain read share. The source
// lands in the caller's address slot when the layout has one.
static i64 dgram_recv(struct task *task, struct kernel_object *socket,
                      int descriptor, u32 type, u32 flags,
                      struct posix_sockaddr_in *source, u8 *buffer,
                      u32 capacity, uptr_t request, u32 flavor) {
    if (!capacity) return 0;
    if (type != POSIX_SOCK_DGRAM) return POSIX_SOCKET_ENOTSOCK;
    if (!(flags & POSIX_SOCKET_OFD_BOUND)) return POSIX_VFS_EINVAL;
    static struct udp_datagram datagram;
    if (!socket_receive_from(socket, &datagram)) {
        u32 take = datagram.length;
        if (take > capacity) take = capacity;
        for (u32 index = 0; index < take; index++)
            buffer[index] = datagram.payload[index];
        if (source)
            sockaddr_fill(source, datagram.source_address,
                          datagram.source_port);
        return (i64)take;
    }
    return socket_park(task, socket, descriptor,
                       POSIX_SOCKET_WAIT_RECVFROM, flavor, request,
                       capacity, 0, 0, 0);
}

i64 posix_socket_recv(struct task *task, int descriptor, u8 *buffer,
                      u32 capacity, uptr_t request) {
    struct kernel_object *socket = 0;
    u32 type = 0;
    u32 flags = 0;
    int resolved = posix_fd_socket_of(task, descriptor,
                                      POSIX_FD_ACCESS_READ, &socket, &type,
                                      &flags, 0, 0);
    if (resolved) return resolved;
    if (type == POSIX_SOCK_DGRAM)
        return dgram_recv(task, socket, descriptor, type, flags, 0,
                          buffer, capacity, request,
                          POSIX_SOCKET_FLAVOR_IO);
    return stream_recv(task, socket, descriptor, type, flags, buffer,
                       capacity, request, POSIX_SOCKET_FLAVOR_IO);
}

i64 posix_socket_recv_from(struct task *task, int descriptor,
                           struct posix_sockaddr_in *source, u8 *buffer,
                           u32 capacity, uptr_t request) {
    struct kernel_object *socket = 0;
    u32 type = 0;
    u32 flags = 0;
    int resolved = posix_fd_socket_of(task, descriptor,
                                      POSIX_FD_ACCESS_READ, &socket, &type,
                                      &flags, 0, 0);
    if (resolved) return resolved;
    if (type == POSIX_SOCK_STREAM)
        return stream_recv(task, socket, descriptor, type, flags, buffer,
                           capacity, request, POSIX_SOCKET_FLAVOR_IO);
    return dgram_recv(task, socket, descriptor, type, flags, source,
                      buffer, capacity, request, POSIX_SOCKET_FLAVOR_IO);
}

static i64 stream_send(struct task *task, struct kernel_object *socket,
                       int descriptor, u32 type, u32 flags,
                       const u8 *data, u32 length, uptr_t request,
                       u32 flavor) {
    if (type != POSIX_SOCK_STREAM) return POSIX_SOCKET_ENOTSOCK;
    if (!(flags & POSIX_SOCKET_OFD_CONNECTED)) return POSIX_SOCKET_ENOTCONN;
    if (socket_stream_send(socket, data, length)) {
        u32 state = 0;
        u32 readiness = 0;
        u32 eof = 0;
        u32 granted = 0;
        i32 error = 0;
        if (socket_stream_state(socket, &state, &readiness, &error, &eof,
                                &granted))
            return POSIX_VFS_EIO;
        if (readiness & (SOCKET_READY_ERROR | SOCKET_READY_HANGUP))
            return stream_take_error(socket);
        // Back-pressure: the queue refused the chunk, so park until the
        // peer's acks open room. The payload rides in the wait entry
        // because the abandoned frame cannot be trusted to re-run.
        return socket_park(task, socket, descriptor,
                           POSIX_SOCKET_WAIT_SEND, flavor, request,
                           length, 0, data, 0);
    }
    return (i64)length;
}

i64 posix_socket_send(struct task *task, int descriptor, const u8 *data,
                      u32 length, uptr_t request) {
    struct kernel_object *socket = 0;
    u32 type = 0;
    u32 flags = 0;
    int resolved = posix_fd_socket_of(task, descriptor,
                                      POSIX_FD_ACCESS_WRITE, &socket, &type,
                                      &flags, 0, 0);
    if (resolved) return resolved;
    if (!length || length > POSIX_IO_MAX) return POSIX_VFS_EINVAL;
    if (type == POSIX_SOCK_DGRAM) return POSIX_SOCKET_EDESTADDRREQ;
    return stream_send(task, socket, descriptor, type, flags, data,
                       length, request, POSIX_SOCKET_FLAVOR_IO);
}

i64 posix_socket_send_to(struct task *task, int descriptor,
                         const struct posix_sockaddr_in *destination,
                         const u8 *data, u32 length) {
    struct kernel_object *socket = 0;
    u32 type = 0;
    u32 flags = 0;
    int resolved = posix_fd_socket_of(task, descriptor,
                                      POSIX_FD_ACCESS_WRITE, &socket, &type,
                                      &flags, 0, 0);
    if (resolved) return resolved;
    if (type == POSIX_SOCK_STREAM) return POSIX_SOCKET_EISCONN;
    if (!length || length > POSIX_IO_MAX) return POSIX_VFS_EINVAL;
    if (!destination || destination->family != POSIX_AF_INET)
        return POSIX_SOCKET_EDESTADDRREQ;
    if (!(flags & POSIX_SOCKET_OFD_BOUND)) return POSIX_VFS_EINVAL;
    u32 address = byte_swap32(destination->address);
    u16 port = byte_swap16(destination->port);
    if (!address || !port) return POSIX_SOCKET_EDESTADDRREQ;
    if (socket_send_to(socket, address, port, data, length))
        return POSIX_SOCKET_EAGAIN;
    return (i64)length;
}

// Gather the caller's vectors into one staging buffer. A message rides
// whole or not at all, so the total is bounded by the staging size the
// way the flat calls are.
static int gather_vectors(struct task *task,
                          const struct posix_msghdr *message,
                          u8 *staging, u32 *total) {
    u32 length = 0;
    if (!message->iov_count || message->iov_count > POSIX_MSG_IOV_MAX ||
        message->control_length)
        return -1;
    for (u32 index = 0; index < message->iov_count; index++) {
        if (message->iov[index].reserved ||
            message->iov[index].length > POSIX_IO_MAX - length)
            return -1;
        if (message->iov[index].length &&
            vm64_copy_from(task->page_dir, staging + length,
                           message->iov[index].base,
                           message->iov[index].length))
            return -1;
        length += message->iov[index].length;
    }
    *total = length;
    return 0;
}

i64 posix_socket_sendmsg(struct task *task, int descriptor,
                         const struct posix_msghdr *message,
                         uptr_t request) {
    struct kernel_object *socket = 0;
    u32 type = 0;
    u32 flags = 0;
    int resolved = posix_fd_socket_of(task, descriptor,
                                      POSIX_FD_ACCESS_WRITE, &socket, &type,
                                      &flags, 0, 0);
    if (resolved) return resolved;
    static u8 staging[POSIX_IO_MAX];
    u32 total = 0;
    if (gather_vectors(task, message, staging, &total))
        return POSIX_VFS_EINVAL;
    if (!total) return 0;
    if (type == POSIX_SOCK_DGRAM) {
        if (message->address_length != POSIX_SOCKADDR_IN_SIZE ||
            message->address.family != POSIX_AF_INET)
            return POSIX_SOCKET_EDESTADDRREQ;
        if (!(flags & POSIX_SOCKET_OFD_BOUND)) return POSIX_VFS_EINVAL;
        u32 address = byte_swap32(message->address.address);
        u16 port = byte_swap16(message->address.port);
        if (!address || !port) return POSIX_SOCKET_EDESTADDRREQ;
        if (socket_send_to(socket, address, port, staging, total))
            return POSIX_SOCKET_EAGAIN;
        return (i64)total;
    }
    return stream_send(task, socket, descriptor, type, flags, staging,
                       total, request, POSIX_SOCKET_FLAVOR_MSG);
}

i64 posix_socket_recvmsg(struct task *task, int descriptor,
                         struct posix_msghdr *message, uptr_t request) {
    struct kernel_object *socket = 0;
    u32 type = 0;
    u32 flags = 0;
    int resolved = posix_fd_socket_of(task, descriptor,
                                      POSIX_FD_ACCESS_READ, &socket, &type,
                                      &flags, 0, 0);
    if (resolved) return resolved;
    u32 capacity = 0;
    if (!message->iov_count || message->iov_count > POSIX_MSG_IOV_MAX ||
        message->control_length)
        return POSIX_VFS_EINVAL;
    for (u32 index = 0; index < message->iov_count; index++) {
        if (message->iov[index].reserved ||
            message->iov[index].length > POSIX_IO_MAX - capacity)
            return POSIX_VFS_EINVAL;
        capacity += message->iov[index].length;
    }
    if (!capacity) return 0;
    if (type == POSIX_SOCK_DGRAM) {
        static struct udp_datagram datagram;
        if (!socket_receive_from(socket, &datagram)) {
            u32 take = datagram.length;
            if (take > capacity) take = capacity;
            u32 written = 0;
            for (u32 index = 0; index < message->iov_count; index++) {
                if (written >= take) break;
                u32 piece = message->iov[index].length;
                if (piece > take - written) piece = take - written;
                if (vm64_copy_to(task->page_dir, message->iov[index].base,
                                 datagram.payload + written, piece))
                    return POSIX_VFS_EIO;
                written += piece;
            }
            message->address_length = POSIX_SOCKADDR_IN_SIZE;
            sockaddr_fill(&message->address, datagram.source_address,
                          datagram.source_port);
            message->flags = 0;
            return (i64)take;
        }
        return socket_park(task, socket, descriptor,
                           POSIX_SOCKET_WAIT_RECVFROM,
                           POSIX_SOCKET_FLAVOR_MSG, request, capacity, 0,
                           0, message);
    }
    u32 state = 0;
    u32 readiness = 0;
    u32 eof = 0;
    u32 granted = 0;
    i32 error = 0;
    if (socket_stream_state(socket, &state, &readiness, &error, &eof,
                            &granted))
        return POSIX_VFS_EIO;
    if (readiness & SOCKET_READY_ERROR) return stream_take_error(socket);
    if (readiness & SOCKET_READY_READABLE) {
        static u8 staging[POSIX_IO_MAX];
        u32 receive_capacity = capacity > POSIX_IO_MAX ?
            POSIX_IO_MAX : capacity;
        u32 received = 0;
        if (socket_stream_receive(socket, staging, receive_capacity,
                                  &received) ||
            !received)
            return POSIX_VFS_EIO;
        u32 written = 0;
        for (u32 index = 0; index < message->iov_count; index++) {
            if (written >= received) break;
            u32 piece = message->iov[index].length;
            if (piece > received - written) piece = received - written;
            if (vm64_copy_to(task->page_dir, message->iov[index].base,
                             staging + written, piece))
                return POSIX_VFS_EIO;
            written += piece;
        }
        message->flags = 0;
        return (i64)received;
    }
    if (eof || (readiness & SOCKET_READY_HANGUP)) return 0;
    return socket_park(task, socket, descriptor, POSIX_SOCKET_WAIT_RECV,
                       POSIX_SOCKET_FLAVOR_MSG, request, capacity, 0, 0,
                       message);
}

int posix_socket_shutdown(struct task *task, int descriptor, u32 how) {
    struct kernel_object *socket = 0;
    u32 type = 0;
    u32 flags = 0;
    int resolved = posix_fd_socket_of(task, descriptor, 0, &socket, &type,
                                      &flags, 0, 0);
    if (resolved) return resolved;
    if (type != POSIX_SOCK_STREAM) return POSIX_SOCKET_ENOTCONN;
    if (!(flags & POSIX_SOCKET_OFD_CONNECTED)) return POSIX_SOCKET_ENOTCONN;
    if (how > POSIX_SHUT_RDWR) return POSIX_VFS_EINVAL;
    // The tcp layer offers the full close; every direction maps onto it
    // and the argument only survives validation.
    return socket_stream_shutdown(socket) ? POSIX_VFS_EIO : 0;
}

int posix_socket_getsockopt(struct task *task, int descriptor, u32 level,
                            u32 name, u8 *value, u32 *length) {
    struct kernel_object *socket = 0;
    u32 type = 0;
    u32 flags = 0;
    int resolved = posix_fd_socket_of(task, descriptor, 0, &socket, &type,
                                      &flags, 0, 0);
    if (resolved) return resolved;
    if (level != POSIX_SOL_SOCKET) return POSIX_SOCKET_ENOPROTOOPT;
    if (!value || !length || !*length || *length > 32u)
        return POSIX_VFS_EINVAL;
    i32 answer = 0;
    if (name == POSIX_SO_TYPE) answer = (i32)type;
    else if (name == POSIX_SO_DOMAIN) answer = (i32)POSIX_AF_INET;
    else if (name == POSIX_SO_PROTOCOL) answer = 0;
    else if (name == POSIX_SO_ERROR) {
        // The take consumes the error the way POSIX asks; a datagram
        // socket carries none in v0.
        if (type == POSIX_SOCK_STREAM &&
            socket_stream_take_error(socket, &answer))
            return POSIX_VFS_EIO;
        if (answer < 0) answer = -answer;
    } else {
        return POSIX_SOCKET_ENOPROTOOPT;
    }
    copy_word(value, (u32)answer);
    *length = 4;
    return 0;
}

int posix_socket_setsockopt(struct task *task, int descriptor, u32 level,
                            u32 name, const u8 *value, u32 length) {
    struct kernel_object *socket = 0;
    u32 type = 0;
    u32 flags = 0;
    int resolved = posix_fd_socket_of(task, descriptor, 0, &socket, &type,
                                      &flags, 0, 0);
    if (resolved) return resolved;
    if (level != POSIX_SOL_SOCKET) return POSIX_SOCKET_ENOPROTOOPT;
    if (!value || !length || length > 32) return POSIX_VFS_EINVAL;
    // Port reuse is stack policy, not caller option, so the accepted name
    // answers success without state; the rest stay unsupported.
    if (name == POSIX_SO_REUSEADDR) return 0;
    return POSIX_SOCKET_ENOPROTOOPT;
}

int posix_socket_getsockname(struct task *task, int descriptor,
                             struct posix_sockaddr_in *address) {
    struct kernel_object *socket = 0;
    u32 type = 0;
    u32 flags = 0;
    struct posix_sockaddr_in local;
    int resolved = posix_fd_socket_of(task, descriptor, 0, &socket, &type,
                                      &flags, &local, 0);
    if (resolved) return resolved;
    if (flags & (POSIX_SOCKET_OFD_BOUND | POSIX_SOCKET_OFD_CONNECTED)) {
        *address = local;
        return 0;
    }
    sockaddr_fill(address, 0, 0);
    return 0;
}

int posix_socket_getpeername(struct task *task, int descriptor,
                             struct posix_sockaddr_in *address) {
    struct kernel_object *socket = 0;
    u32 type = 0;
    u32 flags = 0;
    struct posix_sockaddr_in peer;
    int resolved = posix_fd_socket_of(task, descriptor, 0, &socket, &type,
                                      &flags, 0, &peer);
    if (resolved) return resolved;
    if (!(flags & POSIX_SOCKET_OFD_CONNECTED) ||
        peer.family != POSIX_AF_INET)
        return POSIX_SOCKET_ENOTCONN;
    *address = peer;
    return 0;
}

i64 posix_socket_io_read(struct task *task, int descriptor, u8 *buffer,
                         u32 capacity, uptr_t request) {
    struct kernel_object *socket = 0;
    u32 type = 0;
    u32 flags = 0;
    int resolved = posix_fd_socket_of(task, descriptor,
                                      POSIX_FD_ACCESS_READ, &socket, &type,
                                      &flags, 0, 0);
    if (resolved) return resolved;
    if (type == POSIX_SOCK_DGRAM)
        return dgram_recv(task, socket, descriptor, type, flags, 0,
                          buffer, capacity, request,
                          POSIX_SOCKET_FLAVOR_STD);
    return stream_recv(task, socket, descriptor, type, flags, buffer,
                       capacity, request, POSIX_SOCKET_FLAVOR_STD);
}

i64 posix_socket_io_write(struct task *task, int descriptor,
                          const u8 *data, u32 length, uptr_t request) {
    struct kernel_object *socket = 0;
    u32 type = 0;
    u32 flags = 0;
    int resolved = posix_fd_socket_of(task, descriptor,
                                      POSIX_FD_ACCESS_WRITE, &socket, &type,
                                      &flags, 0, 0);
    if (resolved) return resolved;
    if (!length || length > POSIX_IO_MAX) return POSIX_VFS_EINVAL;
    if (type == POSIX_SOCK_DGRAM) return POSIX_SOCKET_EDESTADDRREQ;
    return stream_send(task, socket, descriptor, type, flags, data,
                       length, request, POSIX_SOCKET_FLAVOR_STD);
}

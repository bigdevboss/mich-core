#ifndef POSIX_SOCKET_H
#define POSIX_SOCKET_H

#include "types.h"
#include "posix_abi.h"

// Socket errno answers, negative the way the vfs codes are. The values
// are the errno numbers the userkit headers carry, so a wrapper hands the
// answer straight to the caller after the sign flip.
#define POSIX_SOCKET_EAGAIN (-11)
#define POSIX_SOCKET_ENOTSOCK (-88)
#define POSIX_SOCKET_EDESTADDRREQ (-89)
#define POSIX_SOCKET_EMSGSIZE (-90)
#define POSIX_SOCKET_ENOPROTOOPT (-92)
#define POSIX_SOCKET_EPROTONOSUPPORT (-93)
#define POSIX_SOCKET_EOPNOTSUPP (-95)
#define POSIX_SOCKET_EAFNOSUPPORT (-97)
#define POSIX_SOCKET_EADDRINUSE (-98)
#define POSIX_SOCKET_EADDRNOTAVAIL (-99)
#define POSIX_SOCKET_EISCONN (-106)
#define POSIX_SOCKET_ENOTCONN (-107)
#define POSIX_SOCKET_EHOSTUNREACH (-113)

// Socket option levels and names the profile understands; everything
// outside this set answers ENOPROTOOPT rather than a silent no-op.
#define POSIX_SOL_SOCKET 1u
#define POSIX_SO_REUSEADDR 2u
#define POSIX_SO_TYPE 3u
#define POSIX_SO_ERROR 4u
#define POSIX_SO_DOMAIN 39u
#define POSIX_SO_PROTOCOL 38u

// Shutdown directions; the tcp layer offers the full close, so every
// direction maps onto it and the argument only survives validation.
#define POSIX_SHUT_RD 0u
#define POSIX_SHUT_WR 1u
#define POSIX_SHUT_RDWR 2u

// Descriptor state the posix layer records beside the kernel object: the
// kernel socket knows its own connection facts, but the local and peer
// addresses a name call answers with live here, at the fd the app holds.
#define POSIX_SOCKET_OFD_BOUND (1u << 0)
#define POSIX_SOCKET_OFD_LISTENING (1u << 1)
#define POSIX_SOCKET_OFD_CONNECTED (1u << 2)

struct task;
struct kernel_object;

void posix_socket_init(void);
int posix_socket_socket(struct task *task, u32 domain, u32 type,
                        u32 protocol);
int posix_socket_bind(struct task *task, int descriptor,
                      const struct posix_sockaddr_in *address);
int posix_socket_listen(struct task *task, int descriptor, u32 backlog);
i64 posix_socket_accept(struct task *task, int descriptor, uptr_t request);
i64 posix_socket_connect(struct task *task, int descriptor,
                         const struct posix_sockaddr_in *address,
                         uptr_t request);
i64 posix_socket_send(struct task *task, int descriptor, const u8 *data,
                      u32 length, uptr_t request);
i64 posix_socket_recv(struct task *task, int descriptor, u8 *buffer,
                      u32 capacity, uptr_t request);
i64 posix_socket_send_to(struct task *task, int descriptor,
                         const struct posix_sockaddr_in *destination,
                         const u8 *data, u32 length);
i64 posix_socket_recv_from(struct task *task, int descriptor,
                           struct posix_sockaddr_in *source, u8 *buffer,
                           u32 capacity, uptr_t request);
i64 posix_socket_sendmsg(struct task *task, int descriptor,
                         const struct posix_msghdr *message,
                         uptr_t request);
i64 posix_socket_recvmsg(struct task *task, int descriptor,
                         struct posix_msghdr *message, uptr_t request);
int posix_socket_shutdown(struct task *task, int descriptor, u32 how);
int posix_socket_getsockopt(struct task *task, int descriptor, u32 level,
                            u32 name, u8 *value, u32 *length);
int posix_socket_setsockopt(struct task *task, int descriptor, u32 level,
                            u32 name, const u8 *value, u32 length);
int posix_socket_getsockname(struct task *task, int descriptor,
                             struct posix_sockaddr_in *address);
int posix_socket_getpeername(struct task *task, int descriptor,
                             struct posix_sockaddr_in *address);

// The read and write pair on a socket descriptor answers the same bytes
// recv and send do, through the plain io request the dispatcher already
// copied for the file path.
i64 posix_socket_io_read(struct task *task, int descriptor, u8 *buffer,
                         u32 capacity, uptr_t request);
i64 posix_socket_io_write(struct task *task, int descriptor,
                          const u8 *data, u32 length, uptr_t request);

// A signal interrupts a parked socket waiter the way it does a pipe one:
// clean the wait and answer EINTR.
i64 posix_socket_signal(struct task *target);

#endif

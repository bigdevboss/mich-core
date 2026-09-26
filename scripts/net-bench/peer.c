// Host-side peer for the mich-core network benchmark. The guest under test is
// always the active side: it opens a TCP control connection, then sends one or
// more newline-terminated commands over it and drives each data phase. It
// pipelines the whole session over a single connection because slirp does not
// reliably carry a guest's second outbound connection to a host-bound peer. This
// peer only exists so the guest has something real on the wire to talk to when it
// is not looping back to itself, so it stays deliberately dumb: no measurement
// happens here, all timing is taken inside the guest with rdtsc. Keeping the peer
// out of the measurement path is what lets the numbers describe the mich stack
// rather than this program or the host's scheduler.
//
// Protocol (all fields decimal ASCII; a connection carries a sequence of command
// lines, each followed by its own data phase, until the guest closes):
//   TX  <total>              guest sends exactly <total> bytes on this TCP
//                            connection; peer reads them and replies
//                            "OK <received>\n".
//   RX  <total>              peer sends <total> bytes on this TCP connection as
//                            fast as it can; the guest reads exactly that many.
//   RR  <count> <size>       peer echoes <size> bytes, <count> times (latency).
//   UTX <port> <count> <size> peer binds UDP <port>, replies "READY\n", then
//                            counts arriving datagrams until a quiet gap and
//                            replies "OK <packets> <bytes>\n" (guest UDP TX).
//   URX <port> <count> <size> peer binds UDP <port>, replies "READY\n", waits
//                            for one datagram to learn the guest's address, then
//                            blasts <count> datagrams of <size> back (guest RX).
//   URR <port> <count> <size> UDP ping-pong echo (datagram round-trip latency).
//
// Build: cc -O2 -o peer peer.c

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define CONTROL_PORT_DEFAULT 4500
#define COMMAND_MAX 128
#define BLAST_CHUNK 65536

// A single scratch buffer serves both directions: the payload bytes carry no
// meaning to the benchmark, only their count and timing do, so we never inspect
// them and one shared buffer is enough.
static uint8_t scratch[BLAST_CHUNK];

// UDP receive quiescence. A one-way UDP blast has no in-band end marker, so the
// peer declares the run finished once no datagram has arrived for this long.
// Long enough to survive a scheduling hiccup on a loaded host, short enough not
// to pad every run.
#define UDP_QUIET_USEC 300000

static void log_errno(const char *what) {
    fprintf(stderr, "peer: %s: %s\n", what, strerror(errno));
}

// Writes the whole buffer, retrying short writes. Returns 0 on success. A short
// write is normal on a fast socket and is not an error, so it is not logged.
static int write_all(int fd, const void *data, size_t length) {
    const uint8_t *cursor = data;
    while (length) {
        ssize_t wrote = write(fd, cursor, length);
        if (wrote < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        cursor += wrote;
        length -= (size_t)wrote;
    }
    return 0;
}

// Reads exactly length bytes. Returns 0 on success, 1 on a clean EOF before the
// count was met (the caller decides whether that is expected), -1 on error.
static int read_exact(int fd, void *data, size_t length) {
    uint8_t *cursor = data;
    while (length) {
        ssize_t got = read(fd, cursor, length);
        if (got < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (got == 0) return 1;
        cursor += got;
        length -= (size_t)got;
    }
    return 0;
}

// Reads one newline-terminated command line into out. The guest sends the
// command as its first bytes, so we read one byte at a time to avoid swallowing
// any data-phase bytes that follow the newline on the same TCP stream.
static int read_command(int fd, char *out, size_t capacity) {
    size_t used = 0;
    while (used + 1 < capacity) {
        char c;
        ssize_t got = read(fd, &c, 1);
        if (got < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (got == 0) return 1;
        if (c == '\n') break;
        if (c != '\r') out[used++] = c;
    }
    out[used] = '\0';
    return 0;
}

static int handle_tx(int fd, uint64_t total) {
    uint64_t received = 0;
    while (received < total) {
        uint64_t remaining = total - received;
        size_t want = remaining < sizeof(scratch) ? (size_t)remaining
                                                   : sizeof(scratch);
        ssize_t got = read(fd, scratch, want);
        if (got < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (got == 0) break;
        received += (uint64_t)got;
    }
    char reply[64];
    int n = snprintf(reply, sizeof(reply), "OK %llu\n",
                     (unsigned long long)received);
    return write_all(fd, reply, (size_t)n);
}

static int handle_rx(int fd, uint64_t total) {
    for (size_t i = 0; i < sizeof(scratch); i++) scratch[i] = (uint8_t)i;
    uint64_t sent = 0;
    while (sent < total) {
        uint64_t remaining = total - sent;
        size_t take = remaining < sizeof(scratch) ? (size_t)remaining
                                                  : sizeof(scratch);
        if (write_all(fd, scratch, take)) return -1;
        sent += take;
    }
    return 0;
}

static int handle_rr(int fd, uint64_t count, uint32_t size) {
    if (!size || size > sizeof(scratch)) return -1;
    for (uint64_t round = 0; round < count; round++) {
        int status = read_exact(fd, scratch, size);
        if (status) return status < 0 ? -1 : 0;
        if (write_all(fd, scratch, size)) return -1;
    }
    return 0;
}

// Binds a UDP socket on port, sends "READY\n" on the control connection so the
// guest does not race the bind, and returns the UDP fd (or -1). Reporting READY
// only after the bind succeeds is what removes the startup sleep the TLS test
// still needs.
static int udp_ready(int control_fd, uint16_t port) {
    int udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp < 0) {
        log_errno("udp socket");
        return -1;
    }
    int reuse = 1;
    setsockopt(udp, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(udp, (struct sockaddr *)&addr, sizeof(addr))) {
        log_errno("udp bind");
        close(udp);
        return -1;
    }
    if (write_all(control_fd, "READY\n", 6)) {
        close(udp);
        return -1;
    }
    return udp;
}

static int handle_utx(int control_fd, uint16_t port) {
    int udp = udp_ready(control_fd, port);
    if (udp < 0) return -1;
    uint64_t packets = 0;
    uint64_t bytes = 0;
    // Block for the first datagram, then switch to a bounded wait so a finished
    // run ends on its own. Without the initial blocking wait a slow guest start
    // would look like an empty run.
    struct timeval first = {10, 0};
    struct timeval quiet = {0, UDP_QUIET_USEC};
    setsockopt(udp, SOL_SOCKET, SO_RCVTIMEO, &first, sizeof(first));
    for (;;) {
        ssize_t got = recv(udp, scratch, sizeof(scratch), 0);
        if (got < 0) {
            if (errno == EINTR) continue;
            break;
        }
        packets++;
        bytes += (uint64_t)got;
        setsockopt(udp, SOL_SOCKET, SO_RCVTIMEO, &quiet, sizeof(quiet));
    }
    close(udp);
    char reply[96];
    int n = snprintf(reply, sizeof(reply), "OK %llu %llu\n",
                     (unsigned long long)packets, (unsigned long long)bytes);
    return write_all(control_fd, reply, (size_t)n);
}

static int handle_urx(int control_fd, uint16_t port, uint64_t count,
                      uint32_t size) {
    if (!size || size > sizeof(scratch)) return -1;
    int udp = udp_ready(control_fd, port);
    if (udp < 0) return -1;
    // The guest is often behind slirp or a tap, so the peer cannot know its
    // address up front. One priming datagram from the guest teaches us where to
    // send the blast back.
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    struct timeval first = {10, 0};
    setsockopt(udp, SOL_SOCKET, SO_RCVTIMEO, &first, sizeof(first));
    ssize_t primed = recvfrom(udp, scratch, sizeof(scratch), 0,
                              (struct sockaddr *)&peer, &peer_len);
    if (primed < 0) {
        close(udp);
        return -1;
    }
    for (size_t i = 0; i < size; i++) scratch[i] = (uint8_t)i;
    uint64_t sent = 0;
    for (uint64_t i = 0; i < count; i++) {
        ssize_t wrote = sendto(udp, scratch, size, 0,
                               (struct sockaddr *)&peer, peer_len);
        if (wrote < 0) {
            if (errno == EINTR || errno == ENOBUFS) {
                // A UDP blast can briefly outrun the host's transmit queue; that
                // is a local backpressure signal, not a failure, so retry.
                i--;
                continue;
            }
            break;
        }
        sent++;
    }
    close(udp);
    char reply[64];
    int n = snprintf(reply, sizeof(reply), "OK %llu\n",
                     (unsigned long long)sent);
    return write_all(control_fd, reply, (size_t)n);
}

static int handle_urr(int control_fd, uint16_t port, uint64_t count,
                      uint32_t size) {
    if (!size || size > sizeof(scratch)) return -1;
    int udp = udp_ready(control_fd, port);
    if (udp < 0) return -1;
    struct timeval budget = {10, 0};
    setsockopt(udp, SOL_SOCKET, SO_RCVTIMEO, &budget, sizeof(budget));
    for (uint64_t round = 0; round < count; round++) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        ssize_t got = recvfrom(udp, scratch, sizeof(scratch), 0,
                               (struct sockaddr *)&peer, &peer_len);
        if (got < 0) break;
        sendto(udp, scratch, (size_t)got, 0, (struct sockaddr *)&peer,
               peer_len);
    }
    close(udp);
    return 0;
}

// Dispatches one command line. Unknown or malformed commands are reported and
// the connection is dropped rather than guessed at, so a protocol mismatch
// fails loud instead of producing a plausible but wrong number.
static int dispatch(int fd, const char *command) {
    char verb[16];
    unsigned long long a = 0;
    unsigned long long b = 0;
    unsigned long long c = 0;
    int fields = sscanf(command, "%15s %llu %llu %llu", verb, &a, &b, &c);
    if (fields < 1) return -1;
    if (!strcmp(verb, "TX") && fields >= 2) return handle_tx(fd, a);
    if (!strcmp(verb, "RX") && fields >= 2) return handle_rx(fd, a);
    if (!strcmp(verb, "RR") && fields >= 3) return handle_rr(fd, a, (uint32_t)b);
    if (!strcmp(verb, "UTX") && fields >= 2)
        return handle_utx(fd, (uint16_t)a);
    if (!strcmp(verb, "URX") && fields >= 4)
        return handle_urx(fd, (uint16_t)a, b, (uint32_t)c);
    if (!strcmp(verb, "URR") && fields >= 4)
        return handle_urr(fd, (uint16_t)a, b, (uint32_t)c);
    fprintf(stderr, "peer: bad command: %s\n", command);
    return -1;
}

// Runs one control connection to completion: read command lines and drive each
// data phase until the guest closes. The guest pipelines several tests over one
// connection because slirp does not reliably carry its second outbound
// connection to a host-bound peer, so a working connection has to serve the
// whole session. Every handler consumes exactly its data phase (TX reads exactly
// <total> bytes, RR exactly <count>*<size>, UDP runs on a side socket), so the
// stream is always positioned at the next command line on return. Blocking I/O
// is fine here because the connection has its own process, so a slow or silent
// client only stalls itself.
static void serve_connection(int fd) {
    // Nagle would batch the tiny RR replies and corrupt the latency numbers,
    // which is exactly what this peer must not do.
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    char command[COMMAND_MAX];
    while (!read_command(fd, command, sizeof(command)) && command[0])
        if (dispatch(fd, command)) return;
}

static void serve(int listen_fd, int once) {
    // Handle each connection in its own child so one stalled client cannot wedge
    // the accept loop and starve the others. A port scanner that connects and
    // then sends nothing (as happens in shared CI sandboxes) would otherwise
    // block read_command forever in a single-threaded loop and every later test
    // connection with it. SIG_IGN on SIGCHLD lets the kernel reap the children,
    // so there is nothing to wait on here.
    signal(SIGCHLD, SIG_IGN);
    for (;;) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int fd = accept(listen_fd, (struct sockaddr *)&from, &from_len);
        if (fd < 0) {
            if (errno == EINTR) continue;
            log_errno("accept");
            return;
        }
        if (once) {
            serve_connection(fd);
            close(fd);
            return;
        }
        pid_t child = fork();
        if (child == 0) {
            close(listen_fd);
            serve_connection(fd);
            close(fd);
            _exit(0);
        }
        // The parent never touches this connection again; the child owns it.
        close(fd);
        if (child < 0) log_errno("fork");
    }
}

int main(int argc, char **argv) {
    uint16_t port = CONTROL_PORT_DEFAULT;
    int once = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc)
            port = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--once"))
            once = 1;
        else {
            fprintf(stderr, "usage: %s [--port N] [--once]\n", argv[0]);
            return 2;
        }
    }
    // A broken guest connection must not take the peer down with SIGPIPE; the
    // per-call write error handling already covers it.
    signal(SIGPIPE, SIG_IGN);
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        log_errno("socket");
        return 1;
    }
    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr))) {
        log_errno("bind");
        return 1;
    }
    if (listen(listen_fd, 16)) {
        log_errno("listen");
        return 1;
    }
    fprintf(stderr, "peer: listening on 0.0.0.0:%u\n", port);
    serve(listen_fd, once);
    close(listen_fd);
    return 0;
}

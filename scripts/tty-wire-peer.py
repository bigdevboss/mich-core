#!/usr/bin/env python3
# The host end of the console wire for the tty smoke profile.
#
# The wire profile gives the guest a second serial port (COM2) and hands it
# to this peer over a unix socket, so the peer is the keyboard: it waits for
# the guest's ready marker in the runner's log and then types a line into
# the receive register, where the kernel's receive interrupt hands it to the
# line discipline. The console itself stays on COM1 as a plain file, because
# a QEMU socket chardev drops guest output when its buffer fills and a log
# with holes cannot gate a run.
#
# The peer writes its own result file the moment it has typed, which is the
# only thing the runner cannot read off the log: whether the host ever sent
# the line at all. Everything else is checked in the log, in order, by the
# profile gate.
import socket
import sys
import time

READY = "Mich x86_64: POSIX tty wire ready"
TYPED = b"wire1\r"
DEADLINE_SECONDS = 240
POLL_SECONDS = 0.05


def wait_for_ready(log_path, deadline):
    """Watch the runner's log for the guest's ready marker."""
    seen = ""
    handle = None
    try:
        while time.monotonic() < deadline:
            if handle is None:
                try:
                    handle = open(log_path, "r", encoding="ascii",
                                  errors="replace")
                except OSError:
                    time.sleep(POLL_SECONDS)
                    continue
            chunk = handle.read()
            if chunk:
                seen += chunk
                if READY in seen:
                    return True
            else:
                time.sleep(POLL_SECONDS)
    finally:
        if handle is not None:
            handle.close()
    return False


def main():
    if len(sys.argv) != 4:
        print("usage: tty-wire-peer.py SOCKET LOG RESULT", file=sys.stderr)
        return 2
    socket_path, log_path, result_path = sys.argv[1:4]
    deadline = time.monotonic() + DEADLINE_SECONDS
    connection = None
    while time.monotonic() < deadline:
        try:
            connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            connection.connect(socket_path)
            break
        except OSError:
            if connection is not None:
                connection.close()
            connection = None
            time.sleep(POLL_SECONDS)
    if connection is None:
        print("tty-wire-peer: no console socket to connect to", file=sys.stderr)
        return 1
    if not wait_for_ready(log_path, deadline):
        connection.close()
        print("tty-wire-peer: the guest never asked for input", file=sys.stderr)
        return 1
    connection.sendall(TYPED)
    connection.close()
    with open(result_path, "w", encoding="ascii") as result:
        result.write("TYPED")
    return 0


if __name__ == "__main__":
    sys.exit(main())

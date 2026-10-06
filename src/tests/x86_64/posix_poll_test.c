#include "types.h"
#include "object.h"
#include "posix_abi.h"
#include "posix_fd.h"
#include "posix_pipe.h"
#include "posix_pledge.h"
#include "posix_profile.h"
#include "task.h"
#include "vfs.h"
#include "tests64.h"

// The park, its wake sources and the timeout need a live scheduler, a peer
// task and user memory, which the prod demo exercises; the unit scope is
// what the descriptor scan alone can prove: the ignore and POLLNVAL
// answers, the direction mask a descriptor's access sets, and the hangup
// and error transitions a pipe reports as its ends close.

int test_posix_poll64(struct task *owner) {
    u32 objects = object_active_count();
    u32 nodes = vfs_node_active_count();
    u32 files = vfs_file_active_count();
    int valid = posix_fd_active_count() == 0;

    struct kernel_object *root = vfs_root();
    struct kernel_object *node = root ?
        vfs_create(root, "poll", VFS_NODE_REGULAR) : 0;
    struct kernel_object *file = node ? vfs_open(node) : 0;
    int both = file ? posix_fd_install_vfs(owner, file,
        POSIX_FD_ACCESS_READ | POSIX_FD_ACCESS_WRITE, 0, 0) : -1;
    if (file) {
        object_release(file);
        file = 0;
    }
    file = node ? vfs_open(node) : 0;
    int read_only = file ? posix_fd_install_vfs(owner, file,
        POSIX_FD_ACCESS_READ, 0, 0) : -1;
    if (file) {
        object_release(file);
        file = 0;
    }
    // A file answers whatever direction it was opened for without parking,
    // a negative entry is the POSIX ignore slot, and an entry that names
    // no open descriptor is POLLNVAL.
    valid = valid && root && node && both >= 0 && read_only >= 0 &&
        posix_fd_poll_events(owner, both) ==
            (POSIX_POLLIN | POSIX_POLLOUT) &&
        posix_fd_poll_events(owner, read_only) == POSIX_POLLIN &&
        posix_fd_poll_events(owner, -1) == 0 &&
        posix_fd_poll_events(owner, POSIX_FD_MAX - 1) == POSIX_POLLNVAL;

    // A fresh pipe has nothing to read and room to write; the last writer
    // leaving turns the read end into a hangup, which is where a parked
    // reader's EOF comes from.
    int index = posix_pipe_create();
    int read_end = index >= 0 ? posix_fd_install_pipe(owner, (u32)index,
        POSIX_PIPE_END_READ, POSIX_FD_ACCESS_READ) : -1;
    int write_end = index >= 0 ? posix_fd_install_pipe(owner, (u32)index,
        POSIX_PIPE_END_WRITE, POSIX_FD_ACCESS_WRITE) : -1;
    valid = valid && index == 0 && read_end >= 0 && write_end >= 0 &&
        posix_fd_poll_events(owner, read_end) == 0 &&
        posix_fd_poll_events(owner, write_end) == POSIX_POLLOUT &&
        !posix_fd_close(owner, write_end) &&
        posix_fd_poll_events(owner, read_end) == POSIX_POLLHUP;
    // The read end reports the hangup on the descriptor that is still open;
    // the one just closed is gone from the table.
    valid = valid && posix_fd_poll_events(owner, write_end) == POSIX_POLLNVAL &&
        !posix_fd_close(owner, read_end) &&
        posix_fd_poll_events(owner, read_end) == POSIX_POLLNVAL;

    // The mirror case: the last reader leaving errors the write end, so a
    // poll on it never parks on a pipe nothing can drain.
    index = posix_pipe_create();
    int held_write = index >= 0 ? posix_fd_install_pipe(owner, (u32)index,
        POSIX_PIPE_END_WRITE, POSIX_FD_ACCESS_WRITE) : -1;
    int doomed_read = index >= 0 ? posix_fd_install_pipe(owner, (u32)index,
        POSIX_PIPE_END_READ, POSIX_FD_ACCESS_READ) : -1;
    valid = valid && index == 0 && held_write >= 0 && doomed_read >= 0 &&
        posix_fd_poll_events(owner, held_write) == POSIX_POLLOUT &&
        !posix_fd_close(owner, doomed_read) &&
        posix_fd_poll_events(owner, held_write) == POSIX_POLLERR &&
        !posix_fd_close(owner, held_write) &&
        posix_pipe_active_count() == 0;

    // A pool index no pipe holds reads back as POLLNVAL rather than as a
    // waitable end.
    valid = valid &&
        posix_pipe_poll(0, POSIX_PIPE_END_READ) == POSIX_POLLNVAL &&
        posix_pipe_poll(POSIX_PIPE_MAX, POSIX_PIPE_END_READ) == POSIX_POLLNVAL;

    // The promise gate row: poll rides with stdio, the way OpenBSD carries
    // it, and a narrowed set denies it loudly or quietly.
    struct task *probe = task_alloc_slot();
    if (probe && !posix_profile_admit(probe)) {
        valid = valid && posix_pledge_promise(probe, "stdio error", 0) == 0;
        valid = valid && posix_pledge_gate(probe, POSIX_SYSCALL_POLL) == 0;
        valid = valid && posix_pledge_promise(probe, "error", 0) == 0;
        valid = valid && posix_pledge_gate(probe, POSIX_SYSCALL_POLL) ==
                        POSIX_PLEDGE_ENOSYS;
        valid = valid && posix_pledge_promise(probe, "", 0) == 0;
        valid = valid && posix_pledge_gate(probe, POSIX_SYSCALL_POLL) == 1;
        posix_pledge_reset(probe);
        posix_profile_release(probe);
        task_free_slot(probe);
    } else {
        if (probe) task_free_slot(probe);
        valid = 0;
    }

    posix_fd_close_all(owner);
    if (node && vfs_unlink(root, "poll")) valid = 0;
    if (node) object_release(node);
    if (root) object_release(root);
    valid = valid && object_active_count() == objects &&
        vfs_node_active_count() == nodes &&
        vfs_file_active_count() == files &&
        posix_fd_active_count() == 0;
    return valid ? 0 : -1;
}

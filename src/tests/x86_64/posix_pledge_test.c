#include "types.h"
#include "posix_abi.h"
#include "posix_pledge.h"
#include "posix_profile.h"
#include "posix_vfs.h"
#include "task.h"
#include "object.h"
#include "vfs.h"
#include "tests64.h"

// The pledge probe borrows a slot through task_alloc_slot, which grows the
// pool count rather than touching runner owned slots, and hands it back
// through task_free_slot. The count itself stays monotonic: the allocator
// hands FREE slots above the count out without raising it, so shrinking it
// would alias a later grow onto a live slot.
static struct task *borrow_slot(int *live) {
    struct task *task = task_alloc_slot();
    *live = task ? (int)(task - task_pool) : -1;
    return task;
}

// Every borrowed slot needs an admitted profile: the resolve paths behind
// unveil and the veil checks run through the profile machinery.
static struct task *borrow_admitted(int *live) {
    struct task *task = borrow_slot(live);
    if (!task) return 0;
    if (posix_profile_admit(task)) {
        task_free_slot(task);
        return 0;
    }
    posix_pledge_reset(task);
    return task;
}

// The veil matcher runs against a scratch tree in the ramfs root; the
// identity rules are the point, not the file semantics, so plain
// vfs_create/vfs_unlink build exactly the shapes the matcher must tell
// apart.
int test_posix_pledge64(void) {
    int valid = 1;

    int probe_slot = -1;
    struct task *probe = borrow_admitted(&probe_slot);
    if (!probe) return -1;

    // A fresh slot is unrestricted: no promise mask, no veil.
    valid = valid && posix_pledge_gate(probe, POSIX_SYSCALL_OPEN) == 0;
    valid = valid && posix_pledge_gate(probe, POSIX_SYSCALL_FORK) == 0;
    valid = valid && posix_pledge_gate_open(probe, POSIX_OPEN_WRONLY |
                                                    POSIX_OPEN_CREAT) == 0;
    valid = valid && posix_pledge_veil_check(probe, "/", 0, 0) == 0;

    // Unknown promise names are EINVAL, never silently ignored.
    valid = valid && posix_pledge_promise(probe, "stdio rpath tyop", 0) ==
                    POSIX_PLEDGE_EINVAL;
    // The first call installs what it says; "error" has to ride along
    // from the start because the narrowing rule applies to it too.
    valid = valid && posix_pledge_promise(
        probe,
        "stdio rpath wpath cpath fattr proc exec unveil error", 0) == 0;
    // A later call may only narrow, and "error" narrows away with the
    // rest: putting it back afterwards is a widening attempt.
    // With "error" in the set a denial answers a quiet ENOSYS; without
    // it the verdict is the loud 1 the dispatcher turns into SIGABRT.
    valid = valid && posix_pledge_promise(probe, "stdio rpath error", 0) == 0;
    valid = valid && posix_pledge_gate(probe, POSIX_SYSCALL_FORK) ==
                    POSIX_PLEDGE_ENOSYS;
    valid = valid && posix_pledge_promise(probe, "stdio rpath", 0) == 0;
    valid = valid && posix_pledge_gate(probe, POSIX_SYSCALL_FORK) == 1;
    valid = valid && posix_pledge_promise(probe, "stdio wpath", 0) ==
                    POSIX_PLEDGE_EPERM;
    valid = valid && posix_pledge_promise(probe, "stdio error", 0) ==
                    POSIX_PLEDGE_EPERM;
    // The narrowed set still covers what it named.
    valid = valid && posix_pledge_gate(probe, POSIX_SYSCALL_WRITE) == 0;
    valid = valid && posix_pledge_gate(probe, POSIX_SYSCALL_STAT) == 0;
    valid = valid && posix_pledge_gate_open(probe, POSIX_OPEN_RDONLY) == 0;
    valid = valid && posix_pledge_gate(probe, POSIX_SYSCALL_CHMOD) == 1;
    valid = valid && posix_pledge_gate_open(probe, POSIX_OPEN_WRONLY) == 1;
    // The unpoliced core stays open.
    valid = valid && posix_pledge_gate(probe, POSIX_SYSCALL_EXIT) == 0;
    valid = valid && posix_pledge_gate(probe, POSIX_SYSCALL_GETPID) == 0;
    valid = valid && posix_pledge_gate(probe, POSIX_SYSCALL_PLEDGE) == 0;

    // Exec promises narrow on their own ladder and take over on exec;
    // without a replacement the current set survives.
    valid = valid && posix_pledge_promise(probe, "stdio", "stdio proc") == 0;
    valid = valid && posix_pledge_promise(probe, 0, "stdio exec") ==
                    POSIX_PLEDGE_EPERM;
    posix_pledge_exec(probe);
    valid = valid && posix_pledge_gate(probe, POSIX_SYSCALL_FORK) == 0;
    valid = valid && posix_pledge_gate(probe, POSIX_SYSCALL_EXECVE) == 1;
    posix_pledge_exec(probe);
    valid = valid && posix_pledge_gate(probe, POSIX_SYSCALL_FORK) == 0;

    // Scratch tree: root/pledge-box/{keep.txt, inner/}, root/outside.txt
    struct kernel_object *root = vfs_root();
    if (!root) { task_free_slot(probe); return -1; }
    struct kernel_object *box =
        vfs_create(root, "pledge-box", VFS_NODE_DIRECTORY);
    struct kernel_object *inner =
        box ? vfs_create(box, "inner", VFS_NODE_DIRECTORY) : 0;
    struct kernel_object *keep =
        box ? vfs_create(box, "keep.txt", VFS_NODE_REGULAR) : 0;
    struct kernel_object *outside =
        vfs_create(root, "pledge-out.txt", VFS_NODE_REGULAR);
    if (!box || !inner || !keep || !outside) {
        if (inner) object_release(inner);
        if (keep) object_release(keep);
        if (outside) object_release(outside);
        if (box) object_release(box);
        task_free_slot(probe);
        return -1;
    }
    object_release(inner);
    object_release(keep);
    object_release(outside);

    // veiled proves directory rules: the box opens wide, narrows, a
    // deeper rule overrides it, and the lock closes the table.
    int veiled_slot = -1;
    struct task *veiled = borrow_admitted(&veiled_slot);
    if (!veiled) { object_release(box); task_free_slot(probe); return -1; }
    valid = valid && posix_pledge_unveil(veiled, "/pledge-box", "rq") ==
                    POSIX_PLEDGE_EINVAL;
    valid = valid && posix_pledge_unveil(veiled, "/no-such-dir", "r") ==
                    POSIX_PLEDGE_ENOENT;
    valid = valid && posix_pledge_unveil(veiled, "/pledge-box", "rwc") == 0;
    valid = valid && posix_pledge_unveil(veiled, "/pledge-box", "rwcx") ==
                    POSIX_PLEDGE_EPERM;
    valid = valid && posix_pledge_unveil(veiled, "/pledge-box", "rw") == 0;
    valid = valid &&
        posix_pledge_unveil(veiled, "/pledge-box/inner", "r") == 0;
    valid = valid && posix_pledge_unveil(veiled, 0, 0) == 0;
    valid = valid && posix_pledge_unveil(veiled, 0, 0) ==
                    POSIX_PLEDGE_EPERM;
    valid = valid && posix_pledge_unveil(veiled, "/pledge-box", "r") ==
                    POSIX_PLEDGE_EPERM;

    // The veil is up: covered paths answer by their rule, everything
    // else is ENOENT no matter that it exists.
    valid = valid && posix_pledge_veil_check(
        veiled, "/pledge-box/keep.txt", POSIX_VEIL_READ, 0) == 0;
    valid = valid && posix_pledge_veil_check(
        veiled, "/pledge-box/keep.txt", POSIX_VEIL_WRITE, 0) == 0;
    valid = valid && posix_pledge_veil_check(
        veiled, "/pledge-box/keep.txt", POSIX_VEIL_EXECUTE, 0) ==
                    POSIX_PLEDGE_EACCES;
    valid = valid && posix_pledge_veil_check(
        veiled, "/pledge-out.txt", POSIX_VEIL_READ, 0) ==
                    POSIX_PLEDGE_ENOENT;
    valid = valid && posix_pledge_veil_check(
        veiled, "/elsewhere/hidden", POSIX_VEIL_READ, 0) ==
                    POSIX_PLEDGE_ENOENT;
    // The deeper "r" rule overrides the "rw" of the parent.
    valid = valid && posix_pledge_veil_check(
        veiled, "/pledge-box/inner", POSIX_VEIL_READ, 0) == 0;
    valid = valid && posix_pledge_veil_check(
        veiled, "/pledge-box/inner", POSIX_VEIL_WRITE, 0) ==
                    POSIX_PLEDGE_EACCES;
    // Creation asks the parent rule for the c permission.
    valid = valid && posix_pledge_veil_check(
        veiled, "/pledge-box/new.txt", 0, POSIX_VEIL_CREATE) ==
                    POSIX_PLEDGE_EACCES;
    valid = valid && posix_pledge_veil_check(
        veiled, "/pledge-box/inner/new.txt", 0, POSIX_VEIL_CREATE) ==
                    POSIX_PLEDGE_EACCES;
    task_free_slot(veiled);

    // third carries the c permission plus a file rule, which is what the
    // re-created file and swapped directory cases need.
    int third_slot = -1;
    struct task *third = borrow_admitted(&third_slot);
    if (!third) { object_release(box); task_free_slot(probe); return -1; }
    valid = valid && posix_pledge_unveil(third, "/pledge-box", "rwc") == 0;
    valid = valid &&
        posix_pledge_unveil(third, "/pledge-box/keep.txt", "r") == 0;
    valid = valid && posix_pledge_veil_check(
        third, "/pledge-box/new.txt", 0, POSIX_VEIL_CREATE) == 0;
    valid = valid && posix_pledge_veil_check(
        third, "/pledge-box/../pledge-out.txt", POSIX_VEIL_READ, 0) ==
                    POSIX_PLEDGE_ENOENT;

    // Inode-sticky directories: a removed and re-created file stays
    // covered through its (parent, name) rule, and the rule beats the
    // wider parent rule the way a more specific unveil must.
    if (!vfs_unlink_path(root, "/pledge-box/keep.txt")) {
        struct kernel_object *again =
            vfs_create(box, "keep.txt", VFS_NODE_REGULAR);
        if (again) {
            struct vfs_node_identity identity;
            if (!vfs_node_identity(again, &identity))
                valid = valid && identity.generation != 0;
            object_release(again);
            valid = valid && posix_pledge_veil_check(
                third, "/pledge-box/keep.txt", POSIX_VEIL_READ, 0) == 0;
            valid = valid && posix_pledge_veil_check(
                third, "/pledge-box/keep.txt", POSIX_VEIL_WRITE, 0) ==
                            POSIX_PLEDGE_EACCES;
        } else {
            valid = 0;
        }
    } else {
        valid = 0;
    }
    // Drop the whole box and build it again: the directory rule was
    // written on the old generation, the new one is invisible even
    // under the same name.
    if (!vfs_unlink_path(root, "/pledge-box/keep.txt") &&
        !vfs_unlink_path(root, "/pledge-box/inner") &&
        !vfs_unlink_path(root, "/pledge-box")) {
        struct kernel_object *fresh =
            vfs_create(root, "pledge-box", VFS_NODE_DIRECTORY);
        if (fresh) {
            valid = valid && posix_pledge_veil_check(
                third, "/pledge-box", POSIX_VEIL_READ, 0) ==
                            POSIX_PLEDGE_ENOENT;
            object_release(fresh);
            vfs_unlink(root, "pledge-box");
        } else {
            valid = 0;
        }
    } else {
        valid = 0;
    }

    // Fork inheritance: the child runs inside the parent sandbox, and a
    // pledge without "unveil" has already locked the third slot's table.
    valid = valid && posix_pledge_promise(third, "stdio", 0) == 0;
    int child_slot = -1;
    struct task *child = borrow_slot(&child_slot);
    if (child) {
        posix_pledge_fork(third, child);
        valid = valid && posix_pledge_gate(child, POSIX_SYSCALL_FORK) == 1;
        valid = valid && posix_pledge_unveil(child, "/pledge-box", "r") ==
                        POSIX_PLEDGE_EPERM;
        valid = valid && posix_pledge_veil_check(
            child, "/pledge-out.txt", POSIX_VEIL_READ, 0) ==
                        POSIX_PLEDGE_ENOENT;
        task_free_slot(child);
    } else {
        valid = 0;
    }

    task_free_slot(third);
    task_free_slot(probe);
    object_release(box);
    // The outside file is scratch like the box: the root listing the
    // demo checks later must come back to boot and dev only.
    valid = valid && !vfs_unlink(root, "pledge-out.txt");
    object_release(root);
    return valid ? 0 : -1;
}

#include "posix_pledge.h"
#include "posix_abi.h"
#include "posix_profile.h"
#include "posix_vfs.h"
#include "task.h"
#include "object.h"
#include "vfs.h"

// Per-slot pledge state rather than fields in struct task, the same shape
// the signal module uses: a zeroed entry is the unrestricted default that
// task reset restores. The veil table travels with the state on fork so
// a child inherits the sandbox whole.
struct posix_veil_entry {
    // The (slot, generation) pair of the unveiled node. A recycled slot
    // always carries a new generation, so a remembered pair can never
    // name a different node.
    u32 slot;
    u32 generation;
    // Directory rules cover their whole subtree; file rules also remember
    // the (parent slot, parent generation, name) triple so a file removed
    // and re-created under the same name stays visible, the way unveil(2)
    // documents it.
    u32 parent;
    u32 parent_generation;
    char name[VFS_NAME_MAX + 1];
    u8 permissions;
    u8 is_directory;
    u8 used;
};

struct posix_pledge_state {
    u32 promises;
    u32 exec_promises;
    u16 entry_count;
    u8 promised;
    u8 has_exec_promises;
    u8 veiled;
    u8 locked;
    struct posix_veil_entry entries[POSIX_PLEDGE_UNVEIL_MAX];
};

static struct posix_pledge_state states[MAX_TASKS];

static struct posix_pledge_state *state_for(struct task *task) {
    u32 slot = (u32)(task - task_pool);
    if (slot >= MAX_TASKS) return 0;
    return &states[slot];
}

void posix_pledge_reset(struct task *task) {
    struct posix_pledge_state *state = state_for(task);
    if (!state) return;
    u8 *bytes = (u8 *)state;
    for (u32 index = 0; index < sizeof(*state); index++) bytes[index] = 0;
}

void posix_pledge_fork(struct task *parent, struct task *child) {
    struct posix_pledge_state *from = state_for(parent);
    struct posix_pledge_state *to = state_for(child);
    if (!from || !to) return;
    *to = *from;
}

void posix_pledge_exec(struct task *task) {
    struct posix_pledge_state *state = state_for(task);
    if (!state) return;
    if (state->has_exec_promises) {
        state->promises = state->exec_promises;
        state->has_exec_promises = 0;
    }
    state->exec_promises = 0;
}

// The promise vocabulary. Unknown names are EINVAL rather than ignored:
// a typo must never silently widen the sandbox.
static int promise_token(const char *token, u32 length, u32 *bit) {
    static const struct { const char *name; u32 value; } table[] = {
        { "stdio", POSIX_PLEDGE_STDIO },
        { "rpath", POSIX_PLEDGE_RPATH },
        { "wpath", POSIX_PLEDGE_WPATH },
        { "cpath", POSIX_PLEDGE_CPATH },
        { "fattr", POSIX_PLEDGE_FATTR },
        { "proc", POSIX_PLEDGE_PROC },
        { "exec", POSIX_PLEDGE_EXEC },
        { "unveil", POSIX_PLEDGE_UNVEIL },
        { "error", POSIX_PLEDGE_ERROR },
    };
    for (u32 index = 0; index < sizeof(table) / sizeof(table[0]); index++) {
        const char *name = table[index].name;
        u32 name_length = 0;
        while (name[name_length]) name_length++;
        if (name_length != length) continue;
        u32 equal = 1;
        for (u32 offset = 0; offset < length; offset++)
            if (name[offset] != token[offset]) { equal = 0; break; }
        if (equal) { *bit = table[index].value; return 0; }
    }
    return -1;
}

// Space separated promise names; an empty string is a valid empty set.
static int parse_promises(const char *string, u32 *mask) {
    *mask = 0;
    if (!string) return 0;
    u32 offset = 0;
    for (;;) {
        while (string[offset] == ' ') offset++;
        if (!string[offset]) return 0;
        u32 start = offset;
        while (string[offset] && string[offset] != ' ') offset++;
        u32 bit;
        if (promise_token(&string[start], offset - start, &bit))
            return -1;
        *mask |= bit;
    }
}

int posix_pledge_promise(struct task *task, const char *promises,
                         const char *execpromises) {
    struct posix_pledge_state *state = state_for(task);
    if (!state) return POSIX_PLEDGE_EPERM;
    if (promises) {
        u32 next;
        if (parse_promises(promises, &next)) return POSIX_PLEDGE_EINVAL;
        if (state->promised && (next & ~state->promises))
            return POSIX_PLEDGE_EPERM;
        state->promises = next;
        state->promised = 1;
        // A promise set without "unveil" closes the unveil window for
        // good, the way pledge(2) documents: the sandbox is done.
        if (!(next & POSIX_PLEDGE_UNVEIL)) state->locked = 1;
    }
    if (execpromises) {
        u32 next;
        if (parse_promises(execpromises, &next)) return POSIX_PLEDGE_EINVAL;
        if (state->has_exec_promises &&
            (next & ~state->exec_promises))
            return POSIX_PLEDGE_EPERM;
        state->exec_promises = next;
        state->has_exec_promises = 1;
    }
    return 0;
}

static int parse_permissions(const char *string, u32 *permissions) {
    *permissions = 0;
    if (!string) return -1;
    for (u32 index = 0; string[index]; index++) {
        u32 bit = 0;
        if (string[index] == 'r') bit = POSIX_VEIL_READ;
        else if (string[index] == 'w') bit = POSIX_VEIL_WRITE;
        else if (string[index] == 'x') bit = POSIX_VEIL_EXECUTE;
        else if (string[index] == 'c') bit = POSIX_VEIL_CREATE;
        else return -1;
        *permissions |= bit;
    }
    return 0;
}

// Directory rules match the node itself or any ancestor: the deepest
// match wins, so a rule lower in the tree overrides one higher up. File
// rules match the node by identity, or by name inside its parent when
// the file was re-created after the rule was written.
static int veil_permissions(struct posix_pledge_state *state,
                            struct kernel_object *node, u32 *permissions) {
    struct vfs_node_identity chain[VFS_PATH_COMPONENT_MAX + 2];
    u32 depth = 0;
    struct kernel_object *walk = node;
    // node arrives retained by the caller; each parent comes back
    // retained and is dropped again once its identity is recorded.
    while (walk && depth < VFS_PATH_COMPONENT_MAX + 2) {
        if (vfs_node_identity(walk, &chain[depth])) break;
        depth++;
        struct kernel_object *up = vfs_node_parent(walk);
        if (walk != node) object_release(walk);
        walk = up;
    }
    if (walk && walk != node) object_release(walk);
    if (!depth) return 0;
    // A file rule on the node itself beats every directory rule above.
    for (u32 index = 0; index < state->entry_count; index++) {
        struct posix_veil_entry *entry = &state->entries[index];
        if (!entry->used || entry->is_directory) continue;
        if (entry->slot == chain[0].slot &&
            entry->generation == chain[0].generation) {
            *permissions = entry->permissions;
            return 1;
        }
        if (entry->parent == chain[0].parent &&
            entry->parent_generation == chain[0].parent_generation) {
            const char *left = entry->name;
            const char *right = chain[0].name;
            u32 equal = 1;
            for (u32 offset = 0; offset <= VFS_NAME_MAX; offset++) {
                if (left[offset] != right[offset]) { equal = 0; break; }
                if (!left[offset]) break;
            }
            if (equal) {
                *permissions = entry->permissions;
                return 1;
            }
        }
    }
    // The deepest directory rule covering the chain wins.
    for (u32 level = 0; level < depth; level++) {
        for (u32 index = 0; index < state->entry_count; index++) {
            struct posix_veil_entry *entry = &state->entries[index];
            if (!entry->used || !entry->is_directory) continue;
            if (entry->slot == chain[level].slot &&
                entry->generation == chain[level].generation) {
                *permissions = entry->permissions;
                return 1;
            }
        }
    }
    return 0;
}

int posix_pledge_unveil(struct task *task, const char *path,
                        const char *permissions) {
    struct posix_pledge_state *state = state_for(task);
    if (!state) return POSIX_PLEDGE_EPERM;
    if (!path && !permissions) {
        if (state->locked) return POSIX_PLEDGE_EPERM;
        state->locked = 1;
        return 0;
    }
    if (!path || !permissions) return POSIX_PLEDGE_EINVAL;
    if (state->locked) return POSIX_PLEDGE_EPERM;
    // The unveil promise controls the call once any pledge is in force.
    if (state->promised && !(state->promises & POSIX_PLEDGE_UNVEIL))
        return POSIX_PLEDGE_EPERM;
    u32 bits;
    if (parse_permissions(permissions, &bits)) return POSIX_PLEDGE_EINVAL;
    struct kernel_object *node = 0;
    if (posix_profile_resolve(task, path, &node) || !node)
        return POSIX_PLEDGE_ENOENT;
    struct vfs_node_identity identity;
    if (vfs_node_identity(node, &identity)) {
        object_release(node);
        return POSIX_PLEDGE_ENOENT;
    }
    int is_directory = identity.type == VFS_NODE_DIRECTORY;
    // The same path may come back only with fewer permissions.
    for (u32 index = 0; index < state->entry_count; index++) {
        struct posix_veil_entry *entry = &state->entries[index];
        if (!entry->used || entry->is_directory != (u8)is_directory)
            continue;
        int same = entry->slot == identity.slot &&
                   entry->generation == identity.generation;
        if (!same && !is_directory &&
            entry->parent == identity.parent &&
            entry->parent_generation == identity.parent_generation) {
            const char *left = entry->name;
            const char *right = identity.name;
            same = 1;
            for (u32 offset = 0; offset <= VFS_NAME_MAX; offset++) {
                if (left[offset] != right[offset]) { same = 0; break; }
                if (!left[offset]) break;
            }
        }
        if (!same) continue;
        if (bits & ~entry->permissions) {
            object_release(node);
            return POSIX_PLEDGE_EPERM;
        }
        entry->permissions = (u8)bits;
        state->veiled = 1;
        object_release(node);
        return 0;
    }
    if (state->entry_count >= POSIX_PLEDGE_UNVEIL_MAX) {
        object_release(node);
        return POSIX_PLEDGE_E2BIG;
    }
    struct posix_veil_entry *entry =
        &state->entries[state->entry_count++];
    entry->used = 1;
    entry->is_directory = (u8)is_directory;
    entry->permissions = (u8)bits;
    entry->slot = identity.slot;
    entry->generation = identity.generation;
    entry->parent = identity.parent;
    entry->parent_generation = identity.parent_generation;
    for (u32 index = 0; index <= VFS_NAME_MAX; index++)
        entry->name[index] = identity.name[index];
    state->veiled = 1;
    object_release(node);
    return 0;
}

// The promise each syscall number needs; 0 for the unpoliced core
// (exit, getpid, getppid) and the pledge calls themselves.
static u32 required_promises(u32 number) {
    switch (number) {
    case POSIX_SYSCALL_READ:
    case POSIX_SYSCALL_WRITE:
    case POSIX_SYSCALL_CLOSE:
    case POSIX_SYSCALL_LSEEK:
    case POSIX_SYSCALL_DUP:
    case POSIX_SYSCALL_DUP2:
    case POSIX_SYSCALL_FCNTL:
    case POSIX_SYSCALL_FSTAT:
    case POSIX_SYSCALL_PREAD:
    case POSIX_SYSCALL_PWRITE:
    case POSIX_SYSCALL_FSYNC:
    case POSIX_SYSCALL_FDATASYNC:
    case POSIX_SYSCALL_BRK:
    case POSIX_SYSCALL_GETRANDOM:
    case POSIX_SYSCALL_GETCWD:
    case POSIX_SYSCALL_CLOCK_GETTIME:
    case POSIX_SYSCALL_CLOCK_GETRES:
    case POSIX_SYSCALL_NANOSLEEP:
    case POSIX_SYSCALL_SIGACTION:
    case POSIX_SYSCALL_SIGPROCMASK:
    case POSIX_SYSCALL_SIGRETURN:
    case POSIX_SYSCALL_SIGPENDING:
        return POSIX_PLEDGE_STDIO;
    case POSIX_SYSCALL_STAT:
    case POSIX_SYSCALL_LSTAT:
    case POSIX_SYSCALL_ACCESS:
    case POSIX_SYSCALL_READLINK:
    case POSIX_SYSCALL_CHDIR:
    case POSIX_SYSCALL_GETDENTS:
        return POSIX_PLEDGE_RPATH;
    case POSIX_SYSCALL_TRUNCATE:
    case POSIX_SYSCALL_FTRUNCATE:
        return POSIX_PLEDGE_WPATH;
    case POSIX_SYSCALL_MKDIR:
    case POSIX_SYSCALL_RMDIR:
    case POSIX_SYSCALL_UNLINK:
    case POSIX_SYSCALL_LINK:
    case POSIX_SYSCALL_RENAME:
    case POSIX_SYSCALL_SYMLINK:
        return POSIX_PLEDGE_CPATH;
    case POSIX_SYSCALL_CHMOD:
    case POSIX_SYSCALL_FCHMOD:
    case POSIX_SYSCALL_CHOWN:
    case POSIX_SYSCALL_UMASK:
    case POSIX_SYSCALL_UTIMENSAT:
    case POSIX_SYSCALL_FUTIMENS:
        return POSIX_PLEDGE_FATTR;
    case POSIX_SYSCALL_FORK:
    case POSIX_SYSCALL_WAITPID:
    case POSIX_SYSCALL_KILL:
        return POSIX_PLEDGE_PROC;
    case POSIX_SYSCALL_EXECVE:
        return POSIX_PLEDGE_EXEC;
    default:
        return 0;
    }
}

static int gate_mask(struct posix_pledge_state *state, u32 need) {
    if (!state->promised || !need) return 0;
    if ((state->promises & need) == need) return 0;
    if (state->promises & POSIX_PLEDGE_ERROR) return POSIX_PLEDGE_ENOSYS;
    return 1;
}

int posix_pledge_gate(struct task *task, u32 number) {
    struct posix_pledge_state *state = state_for(task);
    if (!state) return 0;
    return gate_mask(state, required_promises(number));
}

int posix_pledge_gate_open(struct task *task, u32 flags) {
    struct posix_pledge_state *state = state_for(task);
    if (!state) return 0;
    u32 need = 0;
    u32 mode = flags & POSIX_OPEN_ACCMODE;
    if (mode == POSIX_OPEN_RDONLY) need |= POSIX_PLEDGE_RPATH;
    else need |= POSIX_PLEDGE_WPATH;
    if (mode == POSIX_OPEN_RDWR) need |= POSIX_PLEDGE_RPATH;
    if (flags & POSIX_OPEN_CREAT) need |= POSIX_PLEDGE_CPATH;
    return gate_mask(state, need);
}

int posix_pledge_veil_check(struct task *task, const char *path,
                            u32 leaf_need, u32 create_need) {
    struct posix_pledge_state *state = state_for(task);
    if (!state || !state->veiled) return 0;
    struct kernel_object *node = 0;
    if (!posix_profile_resolve(task, path, &node) && node) {
        u32 permissions;
        if (!veil_permissions(state, node, &permissions)) {
            object_release(node);
            return POSIX_PLEDGE_ENOENT;
        }
        object_release(node);
        if ((permissions & leaf_need) != leaf_need)
            return POSIX_PLEDGE_EACCES;
        return 0;
    }
    if (node) object_release(node);
    // A hidden path and a missing one answer the same ENOENT; only a
    // creation may fall back to the parent rule.
    if (!create_need) return POSIX_PLEDGE_ENOENT;
    struct kernel_object *parent = 0;
    char leaf[VFS_NAME_MAX + 1];
    if (posix_profile_parent(task, path, &parent, leaf) || !parent)
        return POSIX_PLEDGE_ENOENT;
    u32 permissions;
    int covered = veil_permissions(state, parent, &permissions);
    object_release(parent);
    if (!covered) return POSIX_PLEDGE_ENOENT;
    if ((permissions & create_need) != create_need)
        return POSIX_PLEDGE_EACCES;
    return 0;
}

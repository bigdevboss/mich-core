#include "vfs.h"
#include "spinlock.h"
#ifdef __x86_64__
#include "blockfs.h"
#endif

struct vfs_node_state {
    struct kernel_object *self;
    const u8 *external_data;
    u8 data[VFS_FILE_SIZE_MAX];
    char name[VFS_NAME_MAX];
    u32 parent;
    u32 type;
    u32 generation;
    u32 size;
    u32 filesystem;
    u32 mount;
    u32 mount_generation;
    u32 readonly;
    u32 mode;
    u32 linked;
    u32 fs_id;
    u32 active;
};

struct vfs_file_state {
    struct kernel_object *node;
    u32 active;
};

struct vfs_mount_state {
    struct kernel_object *self;
    struct kernel_object *root;
    struct kernel_object *mountpoint;
    u32 generation;
    u32 filesystem;
    u32 active;
};

static struct vfs_node_state nodes[VFS_NODE_MAX];
static struct vfs_file_state files[VFS_FILE_MAX];
static struct vfs_mount_state mounts[VFS_MOUNT_MAX];
/* Pairs file-table admission/final close with blockfs unmount preflight. */
static struct spinlock vfs_file_lock = SPINLOCK_INIT;
/* One VFS domain makes EOF selection and append write indivisible. */
static struct spinlock vfs_write_lock = SPINLOCK_INIT;
static struct kernel_object *root_object;
static struct kernel_object *root_mount;

static int valid_name(const char *name) {
    if (!name || !name[0]) return 0;
    u32 length = 0;
    while (length < VFS_NAME_MAX && name[length]) {
        if (name[length] == '/') return 0;
        length++;
    }
    if (!length || length == VFS_NAME_MAX) return 0;
    if (length == 1 && name[0] == '.') return 0;
    if (length == 2 && name[0] == '.' && name[1] == '.') return 0;
    return 1;
}

static int names_equal(const char *left, const char *right) {
    for (u32 index = 0; index < VFS_NAME_MAX; index++) {
        if (left[index] != right[index]) return 0;
        if (!left[index]) return 1;
    }
    return 1;
}

static struct vfs_node_state *node_for(const struct kernel_object *object) {
    if (!object || !object->active ||
        (object->type != KOBJECT_VNODE && object->type != KOBJECT_DIRECTORY) ||
        !object->value || object->value > VFS_NODE_MAX)
        return 0;
    struct vfs_node_state *node = &nodes[object->value - 1];
    return node->active && node->self == object ? node : 0;
}

static struct vfs_file_state *file_for(const struct kernel_object *object) {
    if (!object || !object->active || object->type != KOBJECT_FILE ||
        !object->value || object->value > VFS_FILE_MAX)
        return 0;
    struct vfs_file_state *file = &files[object->value - 1];
    return file->active ? file : 0;
}

static u32 node_index(const struct vfs_node_state *node) {
    return (u32)(node - nodes);
}

static struct vfs_mount_state *mount_for_point(
    const struct kernel_object *object) {
    for (u32 index = 1; index < VFS_MOUNT_MAX; index++)
        if (mounts[index].active && mounts[index].mountpoint == object)
            return &mounts[index];
    return 0;
}

static struct vfs_mount_state *mount_for_root(
    const struct kernel_object *object) {
    for (u32 index = 1; index < VFS_MOUNT_MAX; index++)
        if (mounts[index].active && mounts[index].root == object)
            return &mounts[index];
    return 0;
}

static int node_backing_live(const struct vfs_node_state *node) {
#ifdef __x86_64__
    if (node && node->filesystem == VFS_FILESYSTEM_BLOCKFS) {
        if (!node->mount || node->mount >= VFS_MOUNT_MAX) return 0;
        const struct vfs_mount_state *mount = &mounts[node->mount];
        return mount->active && mount->filesystem == VFS_FILESYSTEM_BLOCKFS &&
               mount->generation == node->mount_generation;
    }
#else
    (void)node;
#endif
    return 1;
}

static int mount_has_open_file(u32 mount_index, u32 generation) {
    for (u32 index = 0; index < VFS_FILE_MAX; index++) {
        if (!files[index].active) continue;
        struct vfs_node_state *node = node_for(files[index].node);
        if (node && node->filesystem == VFS_FILESYSTEM_BLOCKFS &&
            node->mount == mount_index &&
            node->mount_generation == generation)
            return 1;
    }
    return 0;
}

static u32 child_count(u32 parent) {
    u32 count = 0;
    for (u32 index = 0; index < VFS_NODE_MAX; index++)
        if (nodes[index].active && nodes[index].linked &&
            nodes[index].parent == parent)
            count++;
    return count;
}

static void node_destroy(struct kernel_object *object) {
    if (!object || !object->value || object->value > VFS_NODE_MAX) return;
    struct vfs_node_state *node = &nodes[object->value - 1];
    if (!node->active || node->self != object) return;
    node->self = 0;
    node->external_data = 0;
    for (u32 index = 0; index < VFS_FILE_SIZE_MAX; index++) node->data[index] = 0;
    for (u32 index = 0; index < VFS_NAME_MAX; index++) node->name[index] = 0;
    node->parent = VFS_NODE_MAX;
    node->type = 0;
    node->size = 0;
    node->filesystem = 0;
    node->mount = VFS_MOUNT_MAX;
    node->mount_generation = 0;
    node->readonly = 0;
    node->mode = 0;
    node->linked = 0;
    node->fs_id = 0;
    node->active = 0;
    node->generation++;
    if (!node->generation) node->generation = 1;
}

static void file_destroy(struct kernel_object *object) {
    if (!object || !object->value || object->value > VFS_FILE_MAX) return;
    struct vfs_file_state *file = &files[object->value - 1];
    spin_lock(&vfs_file_lock);
    if (!file->active) {
        spin_unlock(&vfs_file_lock);
        return;
    }
    struct kernel_object *node = file->node;
    file->node = 0;
    file->active = 0;
    spin_unlock(&vfs_file_lock);
    if (node) object_release(node);
}

static void mount_destroy(struct kernel_object *object) {
    if (!object || !object->value || object->value > VFS_MOUNT_MAX) return;
    u32 mount_index = (u32)object->value - 1;
    struct vfs_mount_state *mount = &mounts[mount_index];
    if (!mount->active || mount->self != object) return;
    mount->self = 0;
#ifdef __x86_64__
    if (mount->filesystem == VFS_FILESYSTEM_BLOCKFS)
        blockfs_detach(mount_index);
#endif
    if (mount_index) {
        for (u32 index = 1; index < VFS_NODE_MAX; index++) {
            struct vfs_node_state *node = &nodes[index];
            if (!node->active || node->mount != mount_index || !node->linked)
                continue;
            node->linked = 0;
            node->parent = VFS_NODE_MAX;
            object_release(node->self);
        }
    }
    if (mount->root) object_release(mount->root);
    if (mount->mountpoint) object_release(mount->mountpoint);
    mount->root = 0;
    mount->mountpoint = 0;
    mount->filesystem = 0;
    mount->active = 0;
    mount->generation++;
    if (!mount->generation) mount->generation = 1;
}

void vfs_init(void) {
    vfs_file_lock.ticket = 0;
    vfs_file_lock.served = 0;
    vfs_write_lock.ticket = 0;
    vfs_write_lock.served = 0;
    root_object = 0;
    root_mount = 0;
    for (u32 index = 0; index < VFS_NODE_MAX; index++) {
        nodes[index].self = 0;
        nodes[index].external_data = 0;
        nodes[index].parent = VFS_NODE_MAX;
        nodes[index].type = 0;
        nodes[index].generation = 1;
        nodes[index].size = 0;
        nodes[index].filesystem = 0;
        nodes[index].mount = VFS_MOUNT_MAX;
        nodes[index].mount_generation = 0;
        nodes[index].readonly = 0;
        nodes[index].mode = 0;
        nodes[index].linked = 0;
        nodes[index].fs_id = 0;
        nodes[index].active = 0;
    }
    for (u32 index = 0; index < VFS_FILE_MAX; index++) {
        files[index].node = 0;
        files[index].active = 0;
    }
    for (u32 index = 0; index < VFS_MOUNT_MAX; index++) {
        mounts[index].self = 0;
        mounts[index].root = 0;
        mounts[index].mountpoint = 0;
        mounts[index].generation = 1;
        mounts[index].filesystem = 0;
        mounts[index].active = 0;
    }
    struct vfs_node_state *root = &nodes[0];
    root->parent = VFS_NODE_MAX;
    root->type = VFS_NODE_DIRECTORY;
    root->size = 0;
    root->filesystem = VFS_FILESYSTEM_RAMFS;
    root->mount = 0;
    root->mount_generation = 0;
    root->readonly = 0;
    root->mode = VFS_MODE_DIRECTORY_DEFAULT;
    root->linked = 1;
    root->active = 1;
    root->name[0] = 0;
    root_object = object_create(KOBJECT_DIRECTORY, 1, node_destroy);
    if (!root_object) {
        root->active = 0;
        return;
    }
    root->self = root_object;
    struct vfs_mount_state *mount = &mounts[0];
    if (object_retain(root_object)) return;
    mount->root = root_object;
    mount->mountpoint = 0;
    mount->filesystem = VFS_FILESYSTEM_RAMFS;
    mount->active = 1;
    root_mount = object_create(KOBJECT_MOUNT, 1, mount_destroy);
    if (!root_mount) {
        mount->active = 0;
        object_release(root_object);
    } else {
        mount->self = root_mount;
    }
}

struct kernel_object *vfs_root(void) {
    if (!root_object || !root_object->active || object_retain(root_object)) return 0;
    return root_object;
}

struct kernel_object *vfs_mount_root(void) {
    if (!root_mount || !root_mount->active || object_retain(root_mount)) return 0;
    return root_mount;
}

int vfs_unmount(struct kernel_object *directory) {
    struct vfs_mount_state *mount = mount_for_point(directory);
    if (!mount || !mount->self || !mount->active) return -1;
    u32 mount_index = (u32)(mount - mounts);
    spin_lock(&vfs_file_lock);
    if (mount->filesystem == VFS_FILESYSTEM_BLOCKFS &&
        mount_has_open_file(mount_index, mount->generation)) {
        spin_unlock(&vfs_file_lock);
        return -1;
    }
    object_release(mount->self);
    spin_unlock(&vfs_file_lock);
    return 0;
}

int vfs_mount_bootfs(struct kernel_object *directory,
                     const struct vfs_bootfs_entry *entries, u32 count) {
    struct vfs_node_state *point = node_for(directory);
    if (!point || point->type != VFS_NODE_DIRECTORY || !point->linked ||
        point->readonly || !entries || !count ||
        count > VFS_BOOTFS_ENTRY_MAX || child_count(node_index(point)) ||
        mount_for_point(directory))
        return -1;
    for (u32 index = 0; index < count; index++) {
        if (!valid_name(entries[index].name) || !entries[index].data ||
            !entries[index].size ||
            entries[index].size > VFS_BOOTFS_FILE_SIZE_MAX)
            return -1;
        for (u32 other = 0; other < index; other++)
            if (names_equal(entries[index].name, entries[other].name))
                return -1;
    }
    u32 mount_index = VFS_MOUNT_MAX;
    for (u32 index = 1; index < VFS_MOUNT_MAX; index++)
        if (!mounts[index].active) {
            mount_index = index;
            break;
        }
    if (mount_index == VFS_MOUNT_MAX) return -1;
    u32 slots[VFS_BOOTFS_ENTRY_MAX + 1];
    u32 found = 0;
    for (u32 index = 1; index < VFS_NODE_MAX && found < count + 1; index++)
        if (!nodes[index].active) slots[found++] = index;
    if (found != count + 1) return -1;
    u32 created = 0;
    for (u32 item = 0; item < count + 1; item++) {
        struct vfs_node_state *node = &nodes[slots[item]];
        node->external_data = item ? (const u8 *)entries[item - 1].data : 0;
        node->parent = item ? slots[0] : VFS_NODE_MAX;
        node->type = item ? VFS_NODE_REGULAR : VFS_NODE_DIRECTORY;
        node->size = item ? entries[item - 1].size : 0;
        node->filesystem = VFS_FILESYSTEM_BOOTFS;
        node->mount = mount_index;
        node->mount_generation = mounts[mount_index].generation;
        node->readonly = 1;
        node->mode = item ? VFS_MODE_REGULAR_READONLY :
            VFS_MODE_DIRECTORY_READONLY;
        node->linked = 1;
        node->fs_id = 0;
        node->active = 1;
        const char *name = item ? entries[item - 1].name : point->name;
        for (u32 byte = 0; byte < VFS_NAME_MAX; byte++) node->name[byte] = 0;
        for (u32 byte = 0; name[byte]; byte++) node->name[byte] = name[byte];
        node->self = object_create(
            item ? KOBJECT_VNODE : KOBJECT_DIRECTORY,
            slots[item] + 1, node_destroy);
        if (!node->self) {
            node->active = 0;
            for (u32 undo = 0; undo < created; undo++)
                object_release(nodes[slots[undo]].self);
            return -1;
        }
        created++;
    }
    struct vfs_mount_state *mount = &mounts[mount_index];
    if (object_retain(nodes[slots[0]].self) || object_retain(directory)) {
        if (nodes[slots[0]].self->references > 1)
            object_release(nodes[slots[0]].self);
        for (u32 undo = 0; undo < created; undo++)
            object_release(nodes[slots[undo]].self);
        return -1;
    }
    mount->root = nodes[slots[0]].self;
    mount->mountpoint = directory;
    mount->filesystem = VFS_FILESYSTEM_BOOTFS;
    mount->active = 1;
    mount->self = object_create(
        KOBJECT_MOUNT, mount_index + 1, mount_destroy);
    if (!mount->self) {
        mount->active = 0;
        mount->root = 0;
        mount->mountpoint = 0;
        mount->filesystem = 0;
        object_release(directory);
        object_release(nodes[slots[0]].self);
        for (u32 undo = 0; undo < created; undo++)
            object_release(nodes[slots[undo]].self);
        return -1;
    }
    return 0;
}

#ifdef __x86_64__
int vfs_mount_blockfs(struct kernel_object *directory,
                      struct kernel_object *device) {
    struct vfs_node_state *point = node_for(directory);
    if (!point || point->type != VFS_NODE_DIRECTORY || !point->linked ||
        !directory || !device || child_count(node_index(point)) ||
        mount_for_point(directory))
        return -1;
    u32 mount_index = VFS_MOUNT_MAX;
    for (u32 index = 1; index < VFS_MOUNT_MAX; index++)
        if (!mounts[index].active) {
            mount_index = index;
            break;
        }
    if (mount_index == VFS_MOUNT_MAX || blockfs_attach(mount_index, device))
        return -1;
    u32 inode_count = blockfs_inode_count(mount_index);
    u32 used[BLOCKFS_INODE_MAX];
    u32 types[BLOCKFS_INODE_MAX];
    u32 sizes[BLOCKFS_INODE_MAX];
    u32 parents[BLOCKFS_INODE_MAX];
    u32 modes[BLOCKFS_INODE_MAX];
    char names[BLOCKFS_INODE_MAX][VFS_NAME_MAX];
    u32 inode_item[BLOCKFS_INODE_MAX];
    u32 count = 0;
    for (u32 inode = 0; inode < inode_count; inode++) {
        inode_item[inode] = VFS_NODE_MAX;
        if (blockfs_inode_get(mount_index, inode, &used[inode], &types[inode],
                              &sizes[inode], &parents[inode], &modes[inode],
                              names[inode])) {
            blockfs_detach(mount_index);
            return -1;
        }
        if (!used[inode]) continue;
        if ((inode && (types[inode] != VFS_NODE_REGULAR &&
                       types[inode] != VFS_NODE_DIRECTORY)) ||
            (modes[inode] & ~VFS_MODE_MASK) ||
            (inode && (!names[inode][0] ||
                       names[inode][VFS_NAME_MAX - 1] ||
                       !valid_name(names[inode]))) ||
            (inode && parents[inode] >= inode_count)) {
            blockfs_detach(mount_index);
            return -1;
        }
        inode_item[inode] = count++;
    }
    if (!used[0] || types[0] != VFS_NODE_DIRECTORY || parents[0] || !count) {
        blockfs_detach(mount_index);
        return -1;
    }
    for (u32 inode = 1; inode < inode_count; inode++) {
        if (!used[inode]) continue;
        u32 parent = parents[inode];
        if (!used[parent] || types[parent] != VFS_NODE_DIRECTORY) {
            blockfs_detach(mount_index);
            return -1;
        }
        u32 ancestor = inode;
        for (u32 depth = 0; depth < inode_count; depth++) {
            if (!ancestor) break;
            ancestor = parents[ancestor];
            if (ancestor >= inode_count || !used[ancestor]) {
                blockfs_detach(mount_index);
                return -1;
            }
            if (depth + 1 == inode_count) {
                blockfs_detach(mount_index);
                return -1;
            }
        }
    }
    u32 slots[BLOCKFS_INODE_MAX];
    u32 found = 0;
    for (u32 index = 1; index < VFS_NODE_MAX && found < count; index++)
        if (!nodes[index].active) slots[found++] = index;
    if (found != count) {
        blockfs_detach(mount_index);
        return -1;
    }
    u32 created = 0;
    for (u32 inode = 0; inode < inode_count; inode++) {
        if (!used[inode]) continue;
        u32 item = inode_item[inode];
        struct vfs_node_state *node = &nodes[slots[item]];
        node->external_data = 0;
        node->parent = inode ? slots[inode_item[parents[inode]]] : VFS_NODE_MAX;
        node->type = types[inode];
        node->size = sizes[inode];
        node->filesystem = VFS_FILESYSTEM_BLOCKFS;
        node->mount = mount_index;
        node->mount_generation = mounts[mount_index].generation;
        node->readonly = 0;
        node->mode = modes[inode] ? modes[inode] :
            (node->type == VFS_NODE_DIRECTORY ? VFS_MODE_DIRECTORY_DEFAULT :
             VFS_MODE_REGULAR_DEFAULT);
        node->linked = 1;
        node->fs_id = inode;
        node->active = 1;
        const char *name = inode ? names[inode] : point->name;
        for (u32 byte = 0; byte < VFS_NAME_MAX; byte++) node->name[byte] = 0;
        for (u32 byte = 0; name[byte]; byte++) node->name[byte] = name[byte];
        u32 object_type = node->type == VFS_NODE_DIRECTORY ?
            KOBJECT_DIRECTORY : KOBJECT_VNODE;
        node->self = object_create(object_type, slots[item] + 1, node_destroy);
        if (!node->self) {
            node->active = 0;
            for (u32 undo = 0; undo < created; undo++)
                object_release(nodes[slots[undo]].self);
            blockfs_detach(mount_index);
            return -1;
        }
        created++;
    }
    struct vfs_mount_state *mount = &mounts[mount_index];
    if (object_retain(nodes[slots[0]].self) || object_retain(directory)) {
        if (nodes[slots[0]].self->references > 1)
            object_release(nodes[slots[0]].self);
        for (u32 undo = 0; undo < created; undo++)
            object_release(nodes[slots[undo]].self);
        blockfs_detach(mount_index);
        return -1;
    }
    mount->root = nodes[slots[0]].self;
    mount->mountpoint = directory;
    mount->filesystem = VFS_FILESYSTEM_BLOCKFS;
    mount->active = 1;
    mount->self = object_create(
        KOBJECT_MOUNT, mount_index + 1, mount_destroy);
    if (!mount->self) {
        mount->active = 0;
        mount->root = 0;
        mount->mountpoint = 0;
        mount->filesystem = 0;
        object_release(directory);
        object_release(nodes[slots[0]].self);
        for (u32 undo = 0; undo < created; undo++)
            object_release(nodes[slots[undo]].self);
        blockfs_detach(mount_index);
        return -1;
    }
    return 0;
}
#else
int vfs_mount_blockfs(struct kernel_object *directory,
                      struct kernel_object *device) {
    (void)directory;
    (void)device;
    return -1;
}
#endif

struct kernel_object *vfs_create_mode(struct kernel_object *directory,
                                      const char *name, u32 type, u32 mode) {
    struct vfs_node_state *parent = node_for(directory);
    if (!parent || parent->type != VFS_NODE_DIRECTORY || !parent->linked ||
        parent->readonly || !valid_name(name) ||
        (type != VFS_NODE_REGULAR && type != VFS_NODE_DIRECTORY) ||
        (mode & ~VFS_MODE_MASK))
        return 0;
    u32 parent_index = node_index(parent);
    for (u32 index = 0; index < VFS_NODE_MAX; index++)
        if (nodes[index].active && nodes[index].linked &&
            nodes[index].parent == parent_index &&
            names_equal(nodes[index].name, name))
            return 0;
    for (u32 index = 1; index < VFS_NODE_MAX; index++) {
        struct vfs_node_state *node = &nodes[index];
        if (node->active) continue;
        node->external_data = 0;
        node->parent = parent_index;
        node->type = type;
        node->size = 0;
        node->filesystem = parent->filesystem;
        node->mount = parent->mount;
        node->mount_generation = parent->mount_generation;
        node->readonly = 0;
        node->mode = mode;
        node->linked = 1;
        node->fs_id = 0;
        node->active = 1;
        for (u32 byte = 0; byte < VFS_NAME_MAX; byte++) node->name[byte] = 0;
        for (u32 byte = 0; name[byte]; byte++) node->name[byte] = name[byte];
#ifdef __x86_64__
        if (parent->filesystem == VFS_FILESYSTEM_BLOCKFS &&
            blockfs_inode_create(parent->mount, name, parent->fs_id, type,
                                 mode, &node->fs_id)) {
            node->linked = 0;
            node->active = 0;
            return 0;
        }
#endif
        u32 object_type = type == VFS_NODE_DIRECTORY ?
            KOBJECT_DIRECTORY : KOBJECT_VNODE;
        struct kernel_object *object = object_create(
            object_type, index + 1, node_destroy);
        if (!object) {
#ifdef __x86_64__
            if (node->fs_id)
                blockfs_inode_remove(parent->mount, node->fs_id);
#endif
            node->linked = 0;
            node->active = 0;
            return 0;
        }
        node->self = object;
        if (object_retain(object)) {
#ifdef __x86_64__
            if (node->fs_id)
                blockfs_inode_remove(parent->mount, node->fs_id);
#endif
            node->linked = 0;
            object_release(object);
            return 0;
        }
        return object;
    }
    return 0;
}

struct kernel_object *vfs_create(struct kernel_object *directory,
                                 const char *name, u32 type) {
    u32 mode = type == VFS_NODE_DIRECTORY ? VFS_MODE_DIRECTORY_DEFAULT :
        VFS_MODE_REGULAR_DEFAULT;
    return vfs_create_mode(directory, name, type, mode);
}

struct kernel_object *vfs_lookup(struct kernel_object *directory,
                                 const char *name) {
    struct vfs_node_state *parent = node_for(directory);
    if (!parent || parent->type != VFS_NODE_DIRECTORY || !valid_name(name))
        return 0;
    u32 parent_index = node_index(parent);
    for (u32 index = 0; index < VFS_NODE_MAX; index++) {
        struct vfs_node_state *node = &nodes[index];
        if (!node->active || !node->linked || node->parent != parent_index ||
            !names_equal(node->name, name))
            continue;
        struct vfs_mount_state *mount = mount_for_point(node->self);
        struct kernel_object *result = mount ? mount->root : node->self;
        if (object_retain(result)) return 0;
        return result;
    }
    return 0;
}

static int path_length(const char *path, u32 *length) {
    if (!path || !length || !path[0]) return -1;
    u32 count = 0;
    while (count < VFS_PATH_MAX && path[count]) count++;
    if (!count || count == VFS_PATH_MAX) return -1;
    *length = count;
    return 0;
}

struct kernel_object *vfs_resolve(struct kernel_object *start,
                                  const char *path) {
    u32 length;
    if (path_length(path, &length)) return 0;
    struct kernel_object *current;
    u32 offset = 0;
    if (path[0] == '/') {
        current = vfs_root();
        while (offset < length && path[offset] == '/') offset++;
    } else {
        struct vfs_node_state *node = node_for(start);
        if (!node || node->type != VFS_NODE_DIRECTORY ||
            object_retain(start))
            return 0;
        current = start;
    }
    if (!current) return 0;
    u32 components = 0;
    while (offset < length) {
        while (offset < length && path[offset] == '/') offset++;
        if (offset == length) break;
        char component[VFS_NAME_MAX];
        for (u32 index = 0; index < VFS_NAME_MAX; index++) component[index] = 0;
        u32 component_length = 0;
        while (offset < length && path[offset] != '/') {
            if (component_length + 1 >= VFS_NAME_MAX) {
                object_release(current);
                return 0;
            }
            component[component_length++] = path[offset++];
        }
        components++;
        if (components > VFS_PATH_COMPONENT_MAX) {
            object_release(current);
            return 0;
        }
        if (component_length == 1 && component[0] == '.') continue;
        if (component_length == 2 && component[0] == '.' &&
            component[1] == '.') {
            struct vfs_node_state *node = node_for(current);
            if (!node || node->type != VFS_NODE_DIRECTORY) {
                object_release(current);
                return 0;
            }
            u32 parent_index = node->parent;
            if (parent_index == VFS_NODE_MAX) {
                struct vfs_mount_state *mount = mount_for_root(current);
                struct vfs_node_state *point = mount ?
                    node_for(mount->mountpoint) : 0;
                if (!point) continue;
                parent_index = point->parent;
                if (parent_index == VFS_NODE_MAX) continue;
            }
            struct vfs_node_state *parent = &nodes[parent_index];
            if (!parent->active || !parent->linked || !parent->self ||
                object_retain(parent->self)) {
                object_release(current);
                return 0;
            }
            object_release(current);
            current = parent->self;
            continue;
        }
        struct kernel_object *next = vfs_lookup(current, component);
        object_release(current);
        if (!next) return 0;
        current = next;
    }
    if (length > 1 && path[length - 1] == '/') {
        struct vfs_node_state *node = node_for(current);
        if (!node || node->type != VFS_NODE_DIRECTORY) {
            object_release(current);
            return 0;
        }
    }
    return current;
}

static struct kernel_object *path_parent(struct kernel_object *start,
                                         const char *path, char *leaf) {
    u32 length;
    if (path_length(path, &length) || !leaf) return 0;
    while (length > 1 && path[length - 1] == '/') length--;
    u32 leaf_start = length;
    while (leaf_start && path[leaf_start - 1] != '/') leaf_start--;
    u32 leaf_length = length - leaf_start;
    if (!leaf_length || leaf_length >= VFS_NAME_MAX) return 0;
    for (u32 index = 0; index < VFS_NAME_MAX; index++) leaf[index] = 0;
    for (u32 index = 0; index < leaf_length; index++)
        leaf[index] = path[leaf_start + index];
    if (!valid_name(leaf)) return 0;
    if (!leaf_start) {
        struct vfs_node_state *node = node_for(start);
        if (!node || node->type != VFS_NODE_DIRECTORY || object_retain(start))
            return 0;
        return start;
    }
    char parent_path[VFS_PATH_MAX];
    for (u32 index = 0; index < VFS_PATH_MAX; index++) parent_path[index] = 0;
    u32 parent_length = leaf_start;
    while (parent_length > 1 && path[parent_length - 1] == '/')
        parent_length--;
    for (u32 index = 0; index < parent_length; index++)
        parent_path[index] = path[index];
    if (!parent_length) {
        parent_path[0] = '.';
        parent_path[1] = 0;
    }
    return vfs_resolve(start, parent_path);
}

struct kernel_object *vfs_create_path(struct kernel_object *start,
                                      const char *path, u32 type) {
    char leaf[VFS_NAME_MAX];
    struct kernel_object *parent = path_parent(start, path, leaf);
    if (!parent) return 0;
    struct kernel_object *created = vfs_create(parent, leaf, type);
    object_release(parent);
    return created;
}

int vfs_unlink_path(struct kernel_object *start, const char *path) {
    char leaf[VFS_NAME_MAX];
    struct kernel_object *parent = path_parent(start, path, leaf);
    if (!parent) return -1;
    int result = vfs_unlink(parent, leaf);
    object_release(parent);
    return result;
}

int vfs_unlink(struct kernel_object *directory, const char *name) {
    struct vfs_node_state *parent = node_for(directory);
    if (!parent || parent->type != VFS_NODE_DIRECTORY || parent->readonly ||
        !valid_name(name))
        return -1;
    u32 parent_index = node_index(parent);
    for (u32 index = 1; index < VFS_NODE_MAX; index++) {
        struct vfs_node_state *node = &nodes[index];
        if (!node->active || !node->linked || node->parent != parent_index ||
            !names_equal(node->name, name))
            continue;
        if (node->readonly || mount_for_point(node->self) ||
            (node->type == VFS_NODE_DIRECTORY && child_count(index)))
            return -1;
#ifdef __x86_64__
        if (node->filesystem == VFS_FILESYSTEM_BLOCKFS && node->fs_id &&
            blockfs_inode_remove(node->mount, node->fs_id))
            return -1;
#endif
        node->linked = 0;
        node->parent = VFS_NODE_MAX;
        object_release(node->self);
        return 0;
    }
    return -1;
}

int vfs_image(struct kernel_object *object, const u8 **data, u32 *size) {
    struct vfs_node_state *node = node_for(object);
    if (!node || !node_backing_live(node) ||
        node->type != VFS_NODE_REGULAR || !node->size || !data || !size)
        return -1;
    *data = node->external_data ? node->external_data : node->data;
    *size = node->size;
    return 0;
}

struct kernel_object *vfs_open(struct kernel_object *object) {
    spin_lock(&vfs_file_lock);
    struct vfs_node_state *node = node_for(object);
    if (!node || !node_backing_live(node) ||
        node->type != VFS_NODE_REGULAR) {
        spin_unlock(&vfs_file_lock);
        return 0;
    }
    for (u32 index = 0; index < VFS_FILE_MAX; index++) {
        struct vfs_file_state *file = &files[index];
        if (file->active) continue;
        if (object_retain(object)) {
            spin_unlock(&vfs_file_lock);
            return 0;
        }
        file->node = object;
        file->active = 1;
        struct kernel_object *opened = object_create(
            KOBJECT_FILE, index + 1, file_destroy);
        if (!opened) {
            file->node = 0;
            file->active = 0;
            object_release(object);
        }
        spin_unlock(&vfs_file_lock);
        return opened;
    }
    spin_unlock(&vfs_file_lock);
    return 0;
}

int vfs_read(struct kernel_object *object, u32 offset,
             void *buffer, u32 length, u32 *transferred) {
    struct vfs_file_state *file = file_for(object);
    struct vfs_node_state *node = file ? node_for(file->node) : 0;
    if (!node || !node_backing_live(node) || !buffer || !transferred) return -1;
#ifdef __x86_64__
    if (node->filesystem == VFS_FILESYSTEM_BLOCKFS)
        return blockfs_read(node->mount, node->fs_id, offset, buffer,
                            length, transferred);
#endif
    if (offset >= node->size) {
        *transferred = 0;
        return 0;
    }
    u32 count = node->size - offset;
    if (count > length) count = length;
    const u8 *source = node->external_data ? node->external_data : node->data;
    for (u32 index = 0; index < count; index++)
        ((u8 *)buffer)[index] = source[offset + index];
    *transferred = count;
    return 0;
}

static int write_node(struct vfs_node_state *node, u32 offset,
                      const void *buffer, u32 length, u32 *transferred) {
    if (!node || !node_backing_live(node) || node->readonly ||
        node->external_data || !buffer || !transferred ||
        offset > VFS_FILE_SIZE_MAX || length > VFS_FILE_SIZE_MAX - offset)
        return -1;
#ifdef __x86_64__
    if (node->filesystem == VFS_FILESYSTEM_BLOCKFS) {
        u32 size = node->size;
        int result = blockfs_write(node->mount, node->fs_id, offset, buffer,
                                   length, transferred, &size);
        if (!result) node->size = size;
        return result;
    }
#endif
    for (u32 index = 0; index < length; index++)
        node->data[offset + index] = ((const u8 *)buffer)[index];
    if (offset + length > node->size) node->size = offset + length;
    *transferred = length;
    return 0;
}

int vfs_write(struct kernel_object *object, u32 offset,
              const void *buffer, u32 length, u32 *transferred) {
    struct vfs_file_state *file = file_for(object);
    struct vfs_node_state *node = file ? node_for(file->node) : 0;
    spin_lock(&vfs_write_lock);
    int result = write_node(node, offset, buffer, length, transferred);
    spin_unlock(&vfs_write_lock);
    return result;
}

int vfs_append(struct kernel_object *object, const void *buffer, u32 length,
               u32 *transferred, u32 *position) {
    struct vfs_file_state *file = file_for(object);
    struct vfs_node_state *node = file ? node_for(file->node) : 0;
    if (!position) return -1;
    spin_lock(&vfs_write_lock);
    u32 offset = node ? node->size : 0;
    int result = write_node(node, offset, buffer, length, transferred);
    if (!result) *position = offset + *transferred;
    spin_unlock(&vfs_write_lock);
    return result;
}

static int truncate_node(struct vfs_node_state *node, u32 size) {
    if (!node || !node_backing_live(node) || node->readonly ||
        node->external_data || size > VFS_FILE_SIZE_MAX)
        return -1;
#ifdef __x86_64__
    if (node->filesystem == VFS_FILESYSTEM_BLOCKFS) {
        u32 new_size = 0;
        int result = blockfs_truncate(node->mount, node->fs_id, size, &new_size);
        if (!result) node->size = new_size;
        return result;
    }
#endif
    if (size < node->size)
        for (u32 index = size; index < node->size; index++) node->data[index] = 0;
    else
        for (u32 index = node->size; index < size; index++) node->data[index] = 0;
    node->size = size;
    return 0;
}

int vfs_truncate(struct kernel_object *object, u32 size) {
    struct vfs_file_state *file = file_for(object);
    struct vfs_node_state *node = file ? node_for(file->node) : 0;
    spin_lock(&vfs_write_lock);
    int result = truncate_node(node, size);
    spin_unlock(&vfs_write_lock);
    return result;
}

int vfs_stat(struct kernel_object *object, struct vfs_node_info *info) {
    struct vfs_node_state *node = node_for(object);
    if (!node) {
        struct vfs_file_state *file = file_for(object);
        node = file ? node_for(file->node) : 0;
    }
    if (!node || !node_backing_live(node) || !info) return -1;
    info->type = node->type;
    info->generation = node->generation;
    info->size = node->size;
    info->child_count = node->type == VFS_NODE_DIRECTORY ?
        child_count(node_index(node)) : 0;
    info->linked = node->linked;
    info->filesystem = node->filesystem;
    info->readonly = node->readonly;
    info->mode = node->mode;
    for (u32 index = 0; index < VFS_NAME_MAX; index++)
        info->name[index] = node->name[index];
    return 0;
}

u32 vfs_node_active_count(void) {
    u32 count = 0;
    for (u32 index = 0; index < VFS_NODE_MAX; index++)
        if (nodes[index].active) count++;
    return count;
}

u32 vfs_file_active_count(void) {
    u32 count = 0;
    for (u32 index = 0; index < VFS_FILE_MAX; index++)
        if (files[index].active) count++;
    return count;
}

u32 vfs_mount_active_count(void) {
    u32 count = 0;
    for (u32 index = 0; index < VFS_MOUNT_MAX; index++)
        if (mounts[index].active) count++;
    return count;
}

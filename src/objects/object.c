#include "object.h"
#include "task.h"
#include "posix_fd.h"
#include "klock.h"

struct handle_slot {
    struct kernel_object *object;
    u32 rights;
    u32 generation;
};

static struct kernel_object objects[KOBJECT_MAX];
static struct handle_slot handles[MAX_TASKS][KHANDLE_MAX];

// S2's transitional cover for the object table, the handle tables and the
// refcounts. A destroy callback runs outside it: releasing a vfs file
// releases its node object, so the callback would take the lock again.
static struct klock object_klock = KLOCK_INIT(KLOCK_LEVEL_OBJECT);

static int task_index(const struct task *task) {
    uptr_t address = (uptr_t)task;
    uptr_t base = (uptr_t)task_pool;
    uptr_t size = sizeof(task_pool);
    if (!task || address < base || address >= base + size ||
        (address - base) % sizeof(struct task))
        return -1;
    int index = (int)((address - base) / sizeof(struct task));
    if (task_pool[index].state == TASK_FREE ||
        task_pool[index].state == TASK_ZOMBIE)
        return -1;
    return index;
}

static u32 handle_value(const struct handle_slot *slot, u32 index) {
    return (slot->generation << 8) | (index + 1);
}

static struct handle_slot *handle_slot_for(struct task *task, u32 handle) {
    int owner = task_index(task);
    u32 low = handle & 0xFFu;
    if (owner < 0 || !low || low > KHANDLE_MAX) return 0;
    struct handle_slot *slot = &handles[owner][low - 1];
    if (!slot->object || slot->generation != (handle >> 8)) return 0;
    return slot;
}

void object_init(void) {
    for (u32 index = 0; index < KOBJECT_MAX; index++) {
        objects[index].type = KOBJECT_NONE;
        objects[index].references = 0;
        objects[index].value = 0;
        objects[index].destroy = 0;
        objects[index].active = 0;
    }
    for (u32 owner = 0; owner < MAX_TASKS; owner++) {
        for (u32 index = 0; index < KHANDLE_MAX; index++) {
            handles[owner][index].object = 0;
            handles[owner][index].rights = 0;
            handles[owner][index].generation = 1;
        }
    }
}

struct kernel_object *object_create(u32 type, u64 value,
                                    object_destroy_fn destroy) {
    if (type == KOBJECT_NONE) return 0;
    klock_acquire(&object_klock);
    for (u32 index = 0; index < KOBJECT_MAX; index++) {
        struct kernel_object *object = &objects[index];
        if (object->active) continue;
        object->type = type;
        object->references = 1;
        object->value = value;
        object->destroy = destroy;
        object->active = 1;
        klock_release(&object_klock);
        return object;
    }
    klock_release(&object_klock);
    return 0;
}

static int object_retain_locked(struct kernel_object *object) {
    if (!object->active || !object->references ||
        object->references == 0xFFFFFFFFu)
        return -1;
    object->references++;
    return 0;
}

int object_retain(struct kernel_object *object) {
    if (!object) return -1;
    klock_acquire(&object_klock);
    int answer = object_retain_locked(object);
    klock_release(&object_klock);
    return answer;
}

void object_release(struct kernel_object *object) {
    if (!object) return;
    klock_acquire(&object_klock);
    if (!object->active || !object->references) {
        klock_release(&object_klock);
        return;
    }
    object->references--;
    if (object->references) {
        klock_release(&object_klock);
        return;
    }
    object_destroy_fn destroy = object->destroy;
    object->destroy = 0;
    klock_release(&object_klock);
    // The slot stays claimed while the callback runs: it sees the object's
    // type and value, which are how a vfs file destroy finds its state, and
    // object_create cannot hand the slot out and have its fields cleared
    // underneath by this release. The reference count is already zero, so no
    // caller can retain it back to life from here.
    if (destroy) destroy(object);
    klock_acquire(&object_klock);
    object->type = KOBJECT_NONE;
    object->value = 0;
    object->active = 0;
    klock_release(&object_klock);
}

static u32 handle_open_locked(int owner, struct kernel_object *object,
                              u32 rights) {
    for (u32 index = 0; index < KHANDLE_MAX; index++) {
        struct handle_slot *slot = &handles[owner][index];
        if (slot->object) continue;
        if (object_retain_locked(object)) return 0;
        slot->object = object;
        slot->rights = rights;
        return handle_value(slot, index);
    }
    return 0;
}

u32 handle_open(struct task *task, struct kernel_object *object, u32 rights) {
    int owner = task_index(task);
    if (owner < 0 || !object || !rights || (rights & ~KRIGHT_ALL))
        return 0;
    klock_acquire(&object_klock);
    if (!object->active) {
        klock_release(&object_klock);
        return 0;
    }
    u32 handle = handle_open_locked(owner, object, rights);
    klock_release(&object_klock);
    return handle;
}

struct kernel_object *handle_get(struct task *task, u32 handle,
                                 u32 required_rights, u32 required_type) {
    klock_acquire(&object_klock);
    struct handle_slot *slot = handle_slot_for(task, handle);
    if (!slot || (required_rights & ~slot->rights) ||
        (required_type != KOBJECT_NONE &&
         slot->object->type != required_type)) {
        klock_release(&object_klock);
        return 0;
    }
    struct kernel_object *object = slot->object;
    klock_release(&object_klock);
    return object;
}

int handle_close(struct task *task, u32 handle) {
    klock_acquire(&object_klock);
    struct handle_slot *slot = handle_slot_for(task, handle);
    if (!slot) {
        klock_release(&object_klock);
        return -1;
    }
    struct kernel_object *object = slot->object;
    slot->object = 0;
    slot->rights = 0;
    slot->generation = (slot->generation + 1) & 0xFFFFFFu;
    if (!slot->generation) slot->generation = 1;
    klock_release(&object_klock);
    object_release(object);
    return 0;
}

u32 handle_duplicate(struct task *source, struct task *target,
                     u32 handle, u32 rights) {
    int owner = task_index(target);
    if (owner < 0) return 0;
    klock_acquire(&object_klock);
    struct handle_slot *slot = handle_slot_for(source, handle);
    if (!slot || !(slot->rights & KRIGHT_TRANSFER) || !rights ||
        (rights & ~slot->rights)) {
        klock_release(&object_klock);
        return 0;
    }
    u32 duplicate = handle_open_locked(owner, slot->object, rights);
    klock_release(&object_klock);
    return duplicate;
}

u32 handle_revoke_object(struct kernel_object *object) {
    if (!object) return 0;
    klock_acquire(&object_klock);
    if (!object->active) {
        klock_release(&object_klock);
        return 0;
    }
    klock_release(&object_klock);
    posix_fd_revoke_object(object);
    u32 revoked = 0;
    // One slot per pass, with the table lock dropped before the release: the
    // release can run the destroy callback, and that takes the lock again.
    for (;;) {
        klock_acquire(&object_klock);
        struct kernel_object *found = 0;
        for (u32 owner = 0; owner < MAX_TASKS && !found; owner++) {
            for (u32 index = 0; index < KHANDLE_MAX; index++) {
                struct handle_slot *slot = &handles[owner][index];
                if (slot->object != object) continue;
                slot->object = 0;
                slot->rights = 0;
                slot->generation = (slot->generation + 1) & 0xFFFFFFu;
                if (!slot->generation) slot->generation = 1;
                found = object;
                break;
            }
        }
        klock_release(&object_klock);
        if (!found) break;
        object_release(object);
        revoked++;
    }
    return revoked;
}

void handle_close_all(struct task *task) {
    int owner = task_index(task);
    if (owner < 0) return;
    for (u32 index = 0; index < KHANDLE_MAX; index++) {
        struct handle_slot *slot = &handles[owner][index];
        if (!slot->object) continue;
        u32 handle = handle_value(slot, index);
        handle_close(task, handle);
    }
}

u32 object_active_count(void) {
    u32 count = 0;
    klock_acquire(&object_klock);
    for (u32 index = 0; index < KOBJECT_MAX; index++)
        if (objects[index].active) count++;
    klock_release(&object_klock);
    return count;
}

u32 handle_active_count(void) {
    u32 count = 0;
    klock_acquire(&object_klock);
    for (u32 owner = 0; owner < MAX_TASKS; owner++)
        for (u32 index = 0; index < KHANDLE_MAX; index++)
            if (handles[owner][index].object) count++;
    klock_release(&object_klock);
    return count;
}

u32 handle_task_count(struct task *task) {
    int owner = task_index(task);
    if (owner < 0) return 0;
    u32 count = 0;
    klock_acquire(&object_klock);
    for (u32 index = 0; index < KHANDLE_MAX; index++)
        if (handles[owner][index].object) count++;
    klock_release(&object_klock);
    return count;
}

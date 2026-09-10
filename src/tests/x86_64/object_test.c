#include "object.h"
#include "task.h"
#include "tests64.h"

int test_object64(struct task *owner, struct task *target) {
    struct kernel_object *object = object_create(KOBJECT_EVENT,
                                                  0x4D4943484F424AULL, 0);
    if (!object) return -1;
    u32 first = handle_open(owner, object,
                            KRIGHT_READ | KRIGHT_WAIT | KRIGHT_TRANSFER);
    if (!first || !handle_get(owner, first, KRIGHT_READ, KOBJECT_EVENT) ||
        handle_get(owner, first, KRIGHT_WRITE, KOBJECT_EVENT)) {
        object_release(object);
        return -1;
    }
    u32 copied = handle_duplicate(owner, target, first, KRIGHT_READ);
    if (!copied || !handle_get(target, copied, KRIGHT_READ, KOBJECT_EVENT) ||
        handle_get(target, copied, KRIGHT_WAIT, KOBJECT_EVENT)) {
        if (first) handle_close(owner, first);
        handle_close_all(target);
        object_release(object);
        return -1;
    }
    if (handle_close(owner, first)) {
        handle_close_all(target);
        object_release(object);
        return -1;
    }
    u32 second = handle_open(owner, object, KRIGHT_READ);
    if (!second || second == first || handle_close(owner, second)) {
        handle_close_all(owner);
        handle_close_all(target);
        object_release(object);
        return -1;
    }
    for (u32 cycle = 0; cycle < 40000; cycle++) {
        u32 current = handle_open(owner, object, KRIGHT_READ);
        if (!current || current == first ||
            handle_get(owner, first, KRIGHT_READ, KOBJECT_EVENT) ||
            handle_close(owner, current)) {
            handle_close_all(owner);
            handle_close_all(target);
            object_release(object);
            return -1;
        }
    }
    handle_close_all(target);
    object_release(object);
    return object->active || object->references ? -1 : 0;
}

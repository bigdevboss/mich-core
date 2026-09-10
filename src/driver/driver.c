#include "driver.h"
#include "resource.h"

struct driver_module {
    char name[32];
    u32 abi_version;
    u32 flags;
    driver_probe_fn probe;
    driver_start_fn start;
    driver_stop_fn stop;
    u32 priority;
    u32 restart_policy;
    u32 max_restarts;
    u32 dependency_count;
    char dependencies[DRIVER_DEPENDENCY_MAX][32];
    u32 match_count;
    struct driver_match_rule matches[DRIVER_MATCH_MAX];
    u32 autobind_done;
    u32 instances;
    u32 active;
};

static struct driver_module modules[DRIVER_MODULE_MAX];
static struct driver_instance instances[DRIVER_INSTANCE_MAX];
static u32 next_instance_id;

static int valid_name(const char *name) {
    if (!name || !name[0]) return 0;
    for (u32 index = 0; index < 32; index++)
        if (!name[index]) return 1;
    return 0;
}

static struct driver_module *module_at(int module_id) {
    if (module_id <= 0 || module_id > DRIVER_MODULE_MAX) return 0;
    struct driver_module *module = &modules[module_id - 1];
    return module->active ? module : 0;
}

void driver_init(void) {
    for (u32 index = 0; index < DRIVER_MODULE_MAX; index++) {
        modules[index].name[0] = 0;
        modules[index].abi_version = 0;
        modules[index].flags = 0;
        modules[index].probe = 0;
        modules[index].start = 0;
        modules[index].stop = 0;
        modules[index].priority = 0;
        modules[index].restart_policy = DRIVER_RESTART_NEVER;
        modules[index].max_restarts = 0;
        modules[index].dependency_count = 0;
        modules[index].match_count = 0;
        modules[index].autobind_done = 0;
        modules[index].instances = 0;
        modules[index].active = 0;
    }
    for (u32 index = 0; index < DRIVER_INSTANCE_MAX; index++) {
        instances[index].id = 0;
        instances[index].module_id = 0;
        instances[index].state = DRIVER_STOPPED;
        instances[index].device = 0;
        instances[index].resource_count = 0;
        instances[index].restart_count = 0;
        instances[index].private_data = 0;
        instances[index].active = 0;
    }
    next_instance_id = 1;
}

int driver_register(const struct driver_descriptor *descriptor) {
    if (!descriptor || !valid_name(descriptor->name) ||
        descriptor->abi_version != DRIVER_ABI_VERSION || !descriptor->start)
        return -1;
    for (u32 existing = 0; existing < DRIVER_MODULE_MAX; existing++) {
        if (!modules[existing].active) continue;
        u32 index = 0;
        while (index < 32 && modules[existing].name[index] &&
               descriptor->name[index] &&
               modules[existing].name[index] == descriptor->name[index])
            index++;
        if (index < 32 && !modules[existing].name[index] &&
            !descriptor->name[index])
            return -1;
    }
    for (u32 index = 0; index < DRIVER_MODULE_MAX; index++) {
        struct driver_module *module = &modules[index];
        if (module->active) continue;
        u32 length = 0;
        while (descriptor->name[length]) {
            module->name[length] = descriptor->name[length];
            length++;
        }
        module->name[length] = 0;
        module->abi_version = descriptor->abi_version;
        module->flags = descriptor->flags;
        module->probe = descriptor->probe;
        module->start = descriptor->start;
        module->stop = descriptor->stop;
        module->priority = 0;
        module->restart_policy = DRIVER_RESTART_NEVER;
        module->max_restarts = 0;
        module->dependency_count = 0;
        module->match_count = 0;
        module->autobind_done = 0;
        module->instances = 0;
        module->active = 1;
        return (int)index + 1;
    }
    return -1;
}

static int names_equal(const char *left, const char *right) {
    if (!left || !right) return 0;
    for (u32 index = 0; index < 32; index++) {
        if (left[index] != right[index]) return 0;
        if (!left[index]) return 1;
    }
    return 0;
}

int driver_register_manifest(const struct driver_manifest *manifest) {
    if (!manifest || manifest->dependency_count > DRIVER_DEPENDENCY_MAX ||
        manifest->match_count > DRIVER_MATCH_MAX ||
        manifest->restart_policy > DRIVER_RESTART_ON_FAILURE)
        return -1;
    int module_id = driver_register(&manifest->driver);
    if (module_id < 0) return -1;
    struct driver_module *module = module_at(module_id);
    module->priority = manifest->priority;
    module->restart_policy = manifest->restart_policy;
    module->max_restarts = manifest->max_restarts;
    module->dependency_count = manifest->dependency_count;
    module->match_count = manifest->match_count;
    for (u32 index = 0; index < manifest->dependency_count; index++) {
        const char *dependency = manifest->dependencies[index];
        if (!valid_name(dependency) || names_equal(dependency, module->name)) {
            driver_unregister(module_id);
            return -1;
        }
        u32 length = 0;
        while (dependency[length]) {
            module->dependencies[index][length] = dependency[length];
            length++;
        }
        module->dependencies[index][length] = 0;
    }
    for (u32 index = 0; index < manifest->match_count; index++)
        module->matches[index] = manifest->matches[index];
    return module_id;
}

int driver_unregister(int module_id) {
    struct driver_module *module = module_at(module_id);
    if (!module || module->instances) return -1;
    module->active = 0;
    module->name[0] = 0;
    module->abi_version = 0;
    module->flags = 0;
    module->probe = 0;
    module->start = 0;
    module->stop = 0;
    module->priority = 0;
    module->restart_policy = DRIVER_RESTART_NEVER;
    module->max_restarts = 0;
    module->dependency_count = 0;
    module->match_count = 0;
    module->autobind_done = 0;
    module->instances = 0;
    return 0;
}

struct driver_instance *driver_bind(int module_id,
                                    struct kernel_object *device) {
    struct driver_module *module = module_at(module_id);
    if (!module || !device || !device->active ||
        (module->probe && module->probe(device)))
        return 0;
    for (u32 index = 0; index < DRIVER_INSTANCE_MAX; index++)
        if (instances[index].active && instances[index].device == device)
            return 0;
    for (u32 index = 0; index < DRIVER_INSTANCE_MAX; index++) {
        struct driver_instance *instance = &instances[index];
        if (instance->active) continue;
        if (object_retain(device)) return 0;
        instance->id = next_instance_id++;
        if (!next_instance_id) next_instance_id = 1;
        instance->module_id = (u32)module_id;
        instance->state = DRIVER_STOPPED;
        instance->device = device;
        instance->resource_count = 0;
        instance->restart_count = 0;
        instance->private_data = 0;
        instance->active = 1;
        module->instances++;
        return instance;
    }
    return 0;
}

int driver_add_resource(struct driver_instance *instance,
                        struct kernel_object *object, u32 rights) {
    if (!instance || !instance->active || instance->state != DRIVER_STOPPED ||
        !object || !object->active || !rights || (rights & ~KRIGHT_ALL) ||
        instance->resource_count >= DRIVER_RESOURCE_MAX)
        return -1;
    if (object_retain(object)) return -1;
    struct driver_resource_ref *resource =
        &instance->resources[instance->resource_count++];
    resource->object = object;
    resource->rights = rights;
    return 0;
}

struct kernel_object *driver_get_resource(struct driver_instance *instance,
                                          u32 index, u32 required_rights,
                                          u32 required_type) {
    if (!instance || !instance->active || index >= instance->resource_count)
        return 0;
    struct driver_resource_ref *resource = &instance->resources[index];
    if ((required_rights & ~resource->rights) || !resource->object ||
        !resource->object->active)
        return 0;
    if (required_type != KOBJECT_NONE &&
        resource->object->type != required_type)
        return 0;
    return resource->object;
}

int driver_start(struct driver_instance *instance) {
    if (!instance || !instance->active || instance->state != DRIVER_STOPPED)
        return -1;
    struct driver_module *module = module_at((int)instance->module_id);
    if (!module || !module->active || module->start(instance)) return -1;
    instance->state = DRIVER_RUNNING;
    return 0;
}

void driver_stop(struct driver_instance *instance) {
    if (!instance || !instance->active || instance->state != DRIVER_RUNNING)
        return;
    struct driver_module *module = module_at((int)instance->module_id);
    if (module && module->active && module->stop) module->stop(instance);
    instance->state = DRIVER_STOPPED;
}

void driver_unbind(struct driver_instance *instance) {
    if (!instance || !instance->active) return;
    driver_stop(instance);
    struct driver_module *module = module_at((int)instance->module_id);
    for (u32 index = 0; index < instance->resource_count; index++) {
        object_release(instance->resources[index].object);
        instance->resources[index].object = 0;
        instance->resources[index].rights = 0;
    }
    object_release(instance->device);
    if (module && module->active && module->instances) module->instances--;
    instance->id = 0;
    instance->module_id = 0;
    instance->state = DRIVER_STOPPED;
    instance->device = 0;
    instance->resource_count = 0;
    instance->restart_count = 0;
    instance->private_data = 0;
    instance->active = 0;
}

static int dependencies_ready(const struct driver_module *module) {
    for (u32 dependency = 0; dependency < module->dependency_count; dependency++) {
        int found = 0;
        for (u32 index = 0; index < DRIVER_MODULE_MAX; index++)
            if (modules[index].active && modules[index].autobind_done &&
                names_equal(modules[index].name,
                            module->dependencies[dependency]))
                found = 1;
        if (!found) return 0;
    }
    return 1;
}

static int matches_device(const struct driver_module *module,
                          struct kernel_object *device) {
    if (!module->match_count) return 1;
    const struct pci_resource *pci = pci_resource_get(device);
    if (!pci) return 0;
    for (u32 index = 0; index < module->match_count; index++) {
        const struct driver_match_rule *rule = &module->matches[index];
        if (rule->vendor_id != 0xFFFF && rule->vendor_id != pci->vendor_id)
            continue;
        if (rule->device_id != 0xFFFF && rule->device_id != pci->device_id)
            continue;
        if (rule->class_code != 0xFF && rule->class_code != pci->class_code)
            continue;
        if (rule->subclass != 0xFF && rule->subclass != pci->subclass)
            continue;
        if (rule->programming_interface != 0xFF &&
            rule->programming_interface != pci->programming_interface)
            continue;
        return 1;
    }
    return 0;
}

int driver_autobind(int module_id, struct kernel_object **devices,
                    u32 device_count) {
    struct driver_module *module = module_at(module_id);
    if (!module || !devices || !dependencies_ready(module)) return -1;
    int started = 0;
    for (u32 index = 0; index < device_count; index++) {
        if (!matches_device(module, devices[index])) continue;
        struct driver_instance *instance = driver_bind(module_id, devices[index]);
        if (!instance) continue;
        if (driver_start(instance)) {
            driver_unbind(instance);
            continue;
        }
        started++;
    }
    return started;
}

int driver_start_all(struct kernel_object **devices, u32 device_count) {
    if (!devices && device_count) return -1;
    u32 remaining = 0;
    for (u32 index = 0; index < DRIVER_MODULE_MAX; index++) {
        if (!modules[index].active) continue;
        modules[index].autobind_done = 0;
        remaining++;
    }
    int total = 0;
    while (remaining) {
        int selected = -1;
        for (u32 index = 0; index < DRIVER_MODULE_MAX; index++) {
            struct driver_module *module = &modules[index];
            if (!module->active || module->autobind_done ||
                !dependencies_ready(module))
                continue;
            if (selected < 0 || module->priority < modules[selected].priority)
                selected = (int)index;
        }
        if (selected < 0) return -1;
        int started = driver_autobind(selected + 1, devices, device_count);
        if (started < 0) return -1;
        modules[selected].autobind_done = 1;
        remaining--;
        total += started;
    }
    return total;
}

int driver_instance_failed(struct driver_instance *instance) {
    if (!instance || !instance->active) return -1;
    struct driver_module *module = module_at((int)instance->module_id);
    if (!module) return -1;
    driver_stop(instance);
    instance->state = DRIVER_FAILED;
    if (module->restart_policy != DRIVER_RESTART_ON_FAILURE ||
        instance->restart_count >= module->max_restarts)
        return -1;
    instance->state = DRIVER_STOPPED;
    instance->restart_count++;
    return driver_start(instance);
}

void driver_device_removed(struct kernel_object *device) {
    if (!device) return;
    for (u32 index = 0; index < DRIVER_INSTANCE_MAX; index++)
        if (instances[index].active && instances[index].device == device)
            driver_unbind(&instances[index]);
}

const char *driver_name(int module_id) {
    struct driver_module *module = module_at(module_id);
    return module ? module->name : 0;
}

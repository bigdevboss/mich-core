#ifndef DRIVER_H
#define DRIVER_H

#include "types.h"
#include "object.h"

#define DRIVER_ABI_VERSION 1
#define DRIVER_MODULE_MAX 32
#define DRIVER_INSTANCE_MAX 64
#define DRIVER_RESOURCE_MAX 16
#define DRIVER_DEPENDENCY_MAX 4
#define DRIVER_MATCH_MAX 8

#define DRIVER_STOPPED 0
#define DRIVER_RUNNING 1
#define DRIVER_FAILED 2

#define DRIVER_RESTART_NEVER 0
#define DRIVER_RESTART_ON_FAILURE 1

struct driver_instance;

typedef int (*driver_probe_fn)(struct kernel_object *device);
typedef int (*driver_start_fn)(struct driver_instance *instance);
typedef void (*driver_stop_fn)(struct driver_instance *instance);

struct driver_descriptor {
    const char *name;
    u32 abi_version;
    u32 flags;
    driver_probe_fn probe;
    driver_start_fn start;
    driver_stop_fn stop;
};

struct driver_match_rule {
    u16 vendor_id;
    u16 device_id;
    u8 class_code;
    u8 subclass;
    u8 programming_interface;
};

struct driver_manifest {
    struct driver_descriptor driver;
    u32 priority;
    u32 restart_policy;
    u32 max_restarts;
    u32 dependency_count;
    const char *dependencies[DRIVER_DEPENDENCY_MAX];
    u32 match_count;
    struct driver_match_rule matches[DRIVER_MATCH_MAX];
};

struct driver_resource_ref {
    struct kernel_object *object;
    u32 rights;
};

struct driver_instance {
    u32 id;
    u32 module_id;
    u32 state;
    struct kernel_object *device;
    struct driver_resource_ref resources[DRIVER_RESOURCE_MAX];
    u32 resource_count;
    u32 restart_count;
    void *private_data;
    u32 active;
};

void driver_init(void);
int driver_register(const struct driver_descriptor *descriptor);
int driver_register_manifest(const struct driver_manifest *manifest);
int driver_unregister(int module_id);
struct driver_instance *driver_bind(int module_id,
                                    struct kernel_object *device);
int driver_add_resource(struct driver_instance *instance,
                        struct kernel_object *object, u32 rights);
struct kernel_object *driver_get_resource(struct driver_instance *instance,
                                          u32 index, u32 required_rights,
                                          u32 required_type);
int driver_start(struct driver_instance *instance);
void driver_stop(struct driver_instance *instance);
void driver_unbind(struct driver_instance *instance);
int driver_autobind(int module_id, struct kernel_object **devices,
                    u32 device_count);
int driver_start_all(struct kernel_object **devices, u32 device_count);
int driver_instance_failed(struct driver_instance *instance);
void driver_device_removed(struct kernel_object *device);
const char *driver_name(int module_id);

#endif

#ifndef DRIVER_SUPERVISOR_H
#define DRIVER_SUPERVISOR_H

#include "types.h"
#include "object.h"
#include "driver_abi.h"

#define DRIVER_DOMAIN_MAX 16
#define DRIVER_DOMAIN_RESOURCE_MAX 16

#define DRIVER_DOMAIN_STOPPED 0
#define DRIVER_DOMAIN_RUNNING 1
#define DRIVER_DOMAIN_BACKOFF 2
#define DRIVER_DOMAIN_FAILED 3
#define DRIVER_DOMAIN_REMOVED 4
#define DRIVER_DOMAIN_QUARANTINED 5
#define DRIVER_DOMAIN_STOPPING 6

struct driver_domain_manifest {
    const char *name;
    u32 capabilities;
    u32 restart_policy;
    u32 max_restarts;
    u32 backoff_ticks;
    u32 image_id;
    u32 reset_policy;
    u64 argument;
};

struct driver_domain_resource {
    struct kernel_object *object;
    u32 rights;
    u32 kind;
    u32 index;
    u64 flags;
};

struct driver_domain {
    u32 id;
    u32 state;
    int pid;
    struct kernel_object *device;
    struct driver_domain_resource resources[DRIVER_DOMAIN_RESOURCE_MAX];
    u32 issued_handles[DRIVER_DOMAIN_RESOURCE_MAX];
    u32 resource_count;
    u32 bridge_index;
    u32 restart_count;
    u32 restart_deadline;
    u32 stop_deadline;
    u32 stop_ack;
    u32 generation;
    char name[32];
    u32 manifest_flags;
    u32 capabilities;
    u32 restart_policy;
    u32 max_restarts;
    u32 backoff_ticks;
    u32 image_id;
    u32 reset_policy;
    u64 argument;
    u32 firmware_count;
    char firmware[DRIVER_USER_FIRMWARE_MAX][DRIVER_USER_NAME_MAX];
    u32 active;
};

typedef int (*driver_domain_spawn_fn)(const struct driver_domain *domain);
typedef int (*driver_domain_quiesce_fn)(struct kernel_object *device);
typedef int (*driver_domain_reset_fn)(struct kernel_object *device);
typedef int (*driver_domain_revoke_fn)(int pid,
    const struct driver_domain_resource *resources, u32 count);
typedef int (*driver_domain_terminate_fn)(int pid);
typedef int (*driver_domain_release_fn)(struct driver_domain *domain);
typedef int (*driver_resource_provider_fn)(
    struct driver_domain *domain, const struct driver_user_request *request,
    struct kernel_object **objects, u32 capacity);

void driver_supervisor_init(driver_domain_spawn_fn spawn,
                            driver_domain_quiesce_fn quiesce,
                            driver_domain_reset_fn reset,
                            driver_domain_revoke_fn revoke,
                            driver_domain_terminate_fn terminate);
int driver_supervisor_set_release_backend(driver_domain_release_fn release);
struct driver_domain *driver_domain_create(
    const struct driver_domain_manifest *manifest,
    struct kernel_object *device);
int driver_user_manifest_validate(const struct driver_user_manifest *manifest);
int driver_user_manifest_matches(const struct driver_user_manifest *manifest,
                                 struct kernel_object *device);
struct driver_domain *driver_domain_create_user(
    const struct driver_user_manifest *manifest,
    struct kernel_object *device);
int driver_domain_apply_manifest(struct driver_domain *domain,
                                 const struct driver_user_manifest *manifest,
                                 driver_resource_provider_fn provider);
int driver_domain_add_resource(struct driver_domain *domain,
                               struct kernel_object *object, u32 rights);
int driver_domain_add_resource_kind(struct driver_domain *domain,
                                    struct kernel_object *object, u32 rights,
                                    u32 kind, u32 index, u64 flags);
int driver_domain_add_bridge(struct driver_domain *domain, u32 rights);
int driver_domain_start(struct driver_domain *domain);
int driver_domain_bundle(const struct driver_domain *domain, u32 *handles,
                         u32 capacity);
int driver_domain_bootstrap(int pid, struct driver_bootstrap_info *info);
void driver_supervisor_task_died(int pid, int code, u32 ticks);
void driver_supervisor_tick(u32 ticks);
int driver_domain_request_stop(struct driver_domain *domain);
int driver_domain_stop_ack(int pid);
int driver_domain_stop(struct driver_domain *domain);
int driver_domain_remove(struct driver_domain *domain);
void driver_domain_destroy(struct driver_domain *domain);
int driver_domain_admin_release(struct driver_domain *domain);
int driver_domain_quarantine(u32 id);
struct driver_domain *driver_domain_for_id(u32 id);
struct driver_domain *driver_domain_for_device(struct kernel_object *device);
struct driver_domain *driver_domain_for_pid(int pid);
int driver_domain_firmware_allowed(const struct driver_domain *domain,
                                   const char *name);

#endif

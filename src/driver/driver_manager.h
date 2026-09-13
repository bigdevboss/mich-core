#ifndef DRIVER_MANAGER_H
#define DRIVER_MANAGER_H

#include "types.h"
#include "driver_abi.h"
#include "driver_supervisor.h"

#define DRIVER_MANAGER_MANIFEST_MAX 32
#define DRIVER_MANAGER_BINDING_MAX 16
#define DRIVER_MANAGER_DEVICE_MAX 64

struct driver_manager_recovery_config {
    struct driver_recovery_profile fallback;
    struct driver_recovery_selector fallback_selector;
    struct driver_crash_circuit_policy crash_policy;
    u32 fallback_enabled;
    u32 fallback_selector_enabled;
    u32 fallback_triggers;
    u32 crash_policy_enabled;
};

void driver_manager_init(driver_resource_provider_fn provider);
int driver_manager_set_devices(struct kernel_object **devices, u32 device_count);
int driver_manager_register(const struct driver_user_manifest *manifest);
int driver_manager_register_recovery(
    const struct driver_user_manifest *manifest,
    const struct driver_manager_recovery_config *config);
int driver_manager_unregister(int manifest_id);
const struct driver_user_manifest *driver_manager_manifest(int manifest_id);
int driver_manager_set_recovery_fallback(
    int manifest_id, const struct driver_recovery_profile *profile);
int driver_manager_set_recovery_fallback_selector(
    int manifest_id, const struct driver_recovery_selector *selector);
int driver_manager_set_recovery_fallback_triggers(int manifest_id,
                                                  u32 triggers);
int driver_manager_set_crash_circuit_policy(
    int manifest_id, const struct driver_crash_circuit_policy *policy);
struct driver_domain *driver_manager_start_device(struct kernel_object *device);
int driver_manager_start_all(struct kernel_object **devices, u32 device_count);
struct driver_domain *driver_manager_domain(struct kernel_object *device);
int driver_manager_stop_device(struct kernel_object *device);
int driver_manager_restart_device(struct kernel_object *device);
void driver_manager_task_exiting(int pid, int code);
void driver_manager_tick(void);
int driver_manager_device_removed(struct kernel_object *device);
u32 driver_manager_manifest_count(void);
u32 driver_manager_binding_count(void);

#endif

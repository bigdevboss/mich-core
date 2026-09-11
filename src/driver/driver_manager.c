#include "driver_manager.h"
#include "object.h"

struct manager_manifest {
    struct driver_user_manifest manifest;
    struct driver_recovery_profile fallback;
    struct driver_crash_circuit_policy crash_policy;
    u32 fallback_enabled;
    u32 fallback_triggers;
    u32 crash_policy_enabled;
    u32 active;
};

struct manager_binding {
    struct kernel_object *device;
    struct driver_domain *domain;
    u32 manifest_slot;
    u32 enabled;
    u32 active;
};

static struct manager_manifest manifests[DRIVER_MANAGER_MANIFEST_MAX];
static struct manager_binding bindings[DRIVER_MANAGER_BINDING_MAX];
static struct kernel_object *managed_devices[DRIVER_MANAGER_DEVICE_MAX];
static u32 managed_device_count;
static driver_resource_provider_fn resource_provider;

static void clear_manifest(struct manager_manifest *entry) {
    u8 *bytes = (u8 *)entry;
    for (usize_t byte = 0; byte < sizeof(*entry); byte++) bytes[byte] = 0;
}

static int fallback_valid(const struct driver_user_manifest *manifest,
                          const struct driver_recovery_profile *profile) {
    return profile && profile->image_id < DRIVER_USER_IMAGE_MAX &&
           !(profile->capabilities & ~manifest->capabilities) &&
           (profile->image_id != manifest->image_id ||
            profile->capabilities != manifest->capabilities ||
            profile->argument != manifest->argument);
}

static int recovery_config_valid(
    const struct driver_user_manifest *manifest,
    const struct driver_manager_recovery_config *config) {
    if (!config) return 0;
    if (config->fallback_enabled > 1 || config->crash_policy_enabled > 1 ||
        (!config->fallback_enabled && config->fallback_triggers) ||
        (config->fallback_enabled &&
         (!fallback_valid(manifest, &config->fallback) ||
          driver_recovery_fallback_triggers_validate(
              config->fallback_triggers))) ||
        (config->crash_policy_enabled &&
         driver_crash_circuit_policy_validate(&config->crash_policy)))
        return -1;
    return 0;
}

static int names_equal(const char *left, const char *right) {
    for (u32 index = 0; index < DRIVER_USER_NAME_MAX; index++) {
        if (left[index] != right[index]) return 0;
        if (!left[index]) return 1;
    }
    return 1;
}

static struct manager_manifest *manifest_at(int manifest_id) {
    if (manifest_id <= 0 || manifest_id > DRIVER_MANAGER_MANIFEST_MAX)
        return 0;
    struct manager_manifest *entry = &manifests[manifest_id - 1];
    return entry->active ? entry : 0;
}

static int manifest_slot_by_name(const char *name) {
    for (u32 index = 0; index < DRIVER_MANAGER_MANIFEST_MAX; index++)
        if (manifests[index].active &&
            names_equal(manifests[index].manifest.name, name))
            return (int)index;
    return -1;
}

static int dependency_cycle_visit(u32 slot, u8 *visiting, u8 *visited) {
    if (visiting[slot]) return -1;
    if (visited[slot]) return 0;
    visiting[slot] = 1;
    const struct driver_user_manifest *manifest = &manifests[slot].manifest;
    for (u32 index = 0; index < manifest->dependency_count; index++) {
        int dependency = manifest_slot_by_name(manifest->dependencies[index]);
        if (dependency >= 0 &&
            dependency_cycle_visit((u32)dependency, visiting, visited))
            return -1;
    }
    visiting[slot] = 0;
    visited[slot] = 1;
    return 0;
}

static int dependency_cycle(void) {
    u8 visiting[DRIVER_MANAGER_MANIFEST_MAX];
    u8 visited[DRIVER_MANAGER_MANIFEST_MAX];
    for (u32 index = 0; index < DRIVER_MANAGER_MANIFEST_MAX; index++) {
        visiting[index] = 0;
        visited[index] = 0;
    }
    for (u32 index = 0; index < DRIVER_MANAGER_MANIFEST_MAX; index++)
        if (manifests[index].active &&
            dependency_cycle_visit(index, visiting, visited))
            return -1;
    return 0;
}

static struct manager_binding *binding_for(struct kernel_object *device) {
    if (!device) return 0;
    for (u32 index = 0; index < DRIVER_MANAGER_BINDING_MAX; index++)
        if (bindings[index].active && bindings[index].device == device)
            return &bindings[index];
    return 0;
}

static void clear_binding(struct manager_binding *binding) {
    if (!binding || !binding->active) return;
    binding->enabled = 0;
    driver_domain_destroy(binding->domain);
    object_release(binding->device);
    binding->device = 0;
    binding->domain = 0;
    binding->manifest_slot = 0;
    binding->enabled = 0;
    binding->active = 0;
}

void driver_manager_init(driver_resource_provider_fn provider) {
    for (u32 index = 0; index < DRIVER_MANAGER_BINDING_MAX; index++) {
        clear_binding(&bindings[index]);
        bindings[index].device = 0;
        bindings[index].domain = 0;
        bindings[index].manifest_slot = 0;
        bindings[index].enabled = 0;
        bindings[index].active = 0;
    }
    for (u32 index = 0; index < managed_device_count; index++) {
        object_release(managed_devices[index]);
        managed_devices[index] = 0;
    }
    managed_device_count = 0;
    for (u32 index = 0; index < DRIVER_MANAGER_MANIFEST_MAX; index++)
        clear_manifest(&manifests[index]);
    resource_provider = provider;
}

int driver_manager_set_devices(struct kernel_object **devices, u32 device_count) {
    if ((!devices && device_count) || device_count > DRIVER_MANAGER_DEVICE_MAX)
        return -1;
    struct kernel_object *retained[DRIVER_MANAGER_DEVICE_MAX];
    u32 completed = 0;
    while (completed < device_count) {
        struct kernel_object *device = devices[completed];
        if (!device || !device->active || device->type != KOBJECT_PCI)
            break;
        for (u32 prior = 0; prior < completed; prior++)
            if (retained[prior] == device) device = 0;
        if (!device || object_retain(device)) break;
        retained[completed++] = device;
    }
    if (completed != device_count) {
        while (completed) object_release(retained[--completed]);
        return -1;
    }
    int removal_failed = 0;
    for (u32 binding = 0; binding < DRIVER_MANAGER_BINDING_MAX; binding++) {
        if (!bindings[binding].active) continue;
        int present = 0;
        for (u32 index = 0; index < device_count; index++)
            if (retained[index] == bindings[binding].device) present = 1;
        if (!present) {
            bindings[binding].enabled = 0;
            if (driver_domain_remove(bindings[binding].domain))
                removal_failed = 1;
            clear_binding(&bindings[binding]);
        }
    }
    for (u32 index = 0; index < managed_device_count; index++)
        object_release(managed_devices[index]);
    managed_device_count = device_count;
    for (u32 index = 0; index < device_count; index++)
        managed_devices[index] = retained[index];
    for (u32 index = device_count; index < DRIVER_MANAGER_DEVICE_MAX; index++)
        managed_devices[index] = 0;
    int started = driver_manager_start_all(managed_devices,
                                           managed_device_count);
    return removal_failed || started < 0 ? -1 : started;
}

int driver_manager_register(const struct driver_user_manifest *manifest) {
    return driver_manager_register_recovery(manifest, 0);
}

int driver_manager_register_recovery(
    const struct driver_user_manifest *manifest,
    const struct driver_manager_recovery_config *config) {
    if (driver_user_manifest_validate(manifest) ||
        recovery_config_valid(manifest, config))
        return -1;
    for (u32 index = 0; index < DRIVER_MANAGER_MANIFEST_MAX; index++)
        if (manifests[index].active &&
            names_equal(manifests[index].manifest.name, manifest->name))
            return -1;
    for (u32 index = 0; index < DRIVER_MANAGER_MANIFEST_MAX; index++) {
        if (manifests[index].active) continue;
        clear_manifest(&manifests[index]);
        manifests[index].manifest = *manifest;
        if (config && config->fallback_enabled) {
            manifests[index].fallback = config->fallback;
            manifests[index].fallback_enabled = 1;
            manifests[index].fallback_triggers = config->fallback_triggers;
        }
        if (config && config->crash_policy_enabled) {
            manifests[index].crash_policy = config->crash_policy;
            manifests[index].crash_policy_enabled = 1;
        }
        manifests[index].active = 1;
        if (dependency_cycle()) {
            clear_manifest(&manifests[index]);
            return -1;
        }
        driver_manager_start_all(managed_devices, managed_device_count);
        return (int)index + 1;
    }
    return -1;
}

int driver_manager_unregister(int manifest_id) {
    struct manager_manifest *entry = manifest_at(manifest_id);
    if (!entry) return -1;
    u32 slot = (u32)manifest_id - 1;
    for (u32 index = 0; index < DRIVER_MANAGER_BINDING_MAX; index++)
        if (bindings[index].active && bindings[index].manifest_slot == slot)
            return -1;
    for (u32 index = 0; index < DRIVER_MANAGER_MANIFEST_MAX; index++) {
        if (!manifests[index].active || index == slot) continue;
        const struct driver_user_manifest *manifest = &manifests[index].manifest;
        for (u32 dependency = 0; dependency < manifest->dependency_count;
             dependency++)
            if (names_equal(manifest->dependencies[dependency],
                            entry->manifest.name))
                return -1;
    }
    clear_manifest(entry);
    return 0;
}

const struct driver_user_manifest *driver_manager_manifest(int manifest_id) {
    struct manager_manifest *entry = manifest_at(manifest_id);
    return entry ? &entry->manifest : 0;
}

int driver_manager_set_recovery_fallback(
    int manifest_id, const struct driver_recovery_profile *profile) {
    struct manager_manifest *entry = manifest_at(manifest_id);
    if (!entry || !fallback_valid(&entry->manifest, profile)) return -1;
    u32 slot = (u32)manifest_id - 1;
    for (u32 index = 0; index < DRIVER_MANAGER_BINDING_MAX; index++)
        if (bindings[index].active && bindings[index].manifest_slot == slot)
            return -1;
    entry->fallback = *profile;
    entry->fallback_enabled = 1;
    entry->fallback_triggers = DRIVER_RECOVERY_TRIGGER_CRASH_CIRCUIT;
    return 0;
}

int driver_manager_set_recovery_fallback_triggers(int manifest_id,
                                                  u32 triggers) {
    struct manager_manifest *entry = manifest_at(manifest_id);
    if (!entry || !entry->fallback_enabled ||
        driver_recovery_fallback_triggers_validate(triggers))
        return -1;
    u32 slot = (u32)manifest_id - 1;
    for (u32 index = 0; index < DRIVER_MANAGER_BINDING_MAX; index++)
        if (bindings[index].active && bindings[index].manifest_slot == slot)
            return -1;
    entry->fallback_triggers = triggers;
    return 0;
}

int driver_manager_set_crash_circuit_policy(
    int manifest_id, const struct driver_crash_circuit_policy *policy) {
    struct manager_manifest *entry = manifest_at(manifest_id);
    if (!entry || driver_crash_circuit_policy_validate(policy)) return -1;
    u32 slot = (u32)manifest_id - 1;
    for (u32 index = 0; index < DRIVER_MANAGER_BINDING_MAX; index++)
        if (bindings[index].active && bindings[index].manifest_slot == slot)
            return -1;
    entry->crash_policy = *policy;
    entry->crash_policy_enabled = 1;
    return 0;
}

static int dependencies_ready(u32 slot) {
    const struct driver_user_manifest *manifest = &manifests[slot].manifest;
    for (u32 dependency = 0; dependency < manifest->dependency_count;
         dependency++) {
        int dependency_slot =
            manifest_slot_by_name(manifest->dependencies[dependency]);
        if (dependency_slot < 0) return 0;
        int running = 0;
        for (u32 binding = 0; binding < DRIVER_MANAGER_BINDING_MAX; binding++)
            if (bindings[binding].active &&
                bindings[binding].manifest_slot == (u32)dependency_slot &&
                bindings[binding].domain->state == DRIVER_DOMAIN_RUNNING)
                running = 1;
        if (!running) return 0;
    }
    return 1;
}

static int select_manifest(struct kernel_object *device, const u8 *attempted) {
    int selected = -1;
    for (u32 index = 0; index < DRIVER_MANAGER_MANIFEST_MAX; index++) {
        if (!manifests[index].active || attempted[index] ||
            !dependencies_ready(index) ||
            !driver_user_manifest_matches(&manifests[index].manifest, device))
            continue;
        if (selected < 0 || manifests[index].manifest.priority <
                            manifests[selected].manifest.priority)
            selected = (int)index;
    }
    return selected;
}

struct driver_domain *driver_manager_start_device(struct kernel_object *device) {
    if (!device || !device->active || binding_for(device) ||
        driver_domain_for_device(device))
        return 0;
    struct manager_binding *binding = 0;
    for (u32 index = 0; index < DRIVER_MANAGER_BINDING_MAX; index++)
        if (!bindings[index].active) {
            binding = &bindings[index];
            break;
        }
    if (!binding) return 0;
    u8 attempted[DRIVER_MANAGER_MANIFEST_MAX];
    for (u32 index = 0; index < DRIVER_MANAGER_MANIFEST_MAX; index++)
        attempted[index] = 0;
    for (;;) {
        int selected = select_manifest(device, attempted);
        if (selected < 0) return 0;
        attempted[selected] = 1;
        struct manager_manifest *entry = &manifests[selected];
        const struct driver_user_manifest *manifest = &entry->manifest;
        struct driver_domain *domain = driver_domain_create_user(manifest, device);
        if (!domain) continue;
        if (driver_domain_apply_manifest(domain, manifest, resource_provider) ||
            (entry->fallback_enabled &&
             (driver_domain_set_recovery_fallback(domain, &entry->fallback) ||
              driver_domain_set_recovery_fallback_triggers(
                  domain, entry->fallback_triggers))) ||
            (entry->crash_policy_enabled &&
             driver_domain_set_crash_circuit_policy(domain,
                                                    &entry->crash_policy)) ||
            driver_domain_start(domain)) {
            driver_domain_destroy(domain);
            continue;
        }
        if (object_retain(device)) {
            driver_domain_destroy(domain);
            return 0;
        }
        binding->device = device;
        binding->domain = domain;
        binding->manifest_slot = (u32)selected;
        binding->enabled = 1;
        binding->active = 1;
        return domain;
    }
}

int driver_manager_start_all(struct kernel_object **devices, u32 device_count) {
    if (!devices && device_count) return -1;
    int started = 0;
    for (;;) {
        int progress = 0;
        for (u32 index = 0; index < device_count; index++)
            if (!binding_for(devices[index]) &&
                driver_manager_start_device(devices[index])) {
                started++;
                progress++;
            }
        if (!progress) return started;
    }
}

struct driver_domain *driver_manager_domain(struct kernel_object *device) {
    struct manager_binding *binding = binding_for(device);
    return binding ? binding->domain : 0;
}

int driver_manager_stop_device(struct kernel_object *dev) {
    struct manager_binding *b = binding_for(dev);
    if (!b) return -1;
    b->enabled = 0;
    if (b->domain->manifest_flags & DRIVER_MANIFEST_GRACEFUL_STOP)
        return driver_domain_request_stop(b->domain);
    return driver_domain_stop(b->domain);
}

int driver_manager_restart_device(struct kernel_object *device) {
    struct manager_binding *binding = binding_for(device);
    if (!binding) return -1;
    binding->enabled = 0;
    if (driver_domain_stop(binding->domain)) return -1;
    binding->enabled = 1;
    if (!dependencies_ready(binding->manifest_slot)) return 0;
    if (driver_domain_start(binding->domain)) {
        binding->enabled = 0;
        return -1;
    }
    return 0;
}

void driver_manager_task_exiting(int pid, int code) {
    if (!pid || code) return;
    for (u32 index = 0; index < DRIVER_MANAGER_BINDING_MAX; index++)
        if (bindings[index].active &&
            bindings[index].domain->state == DRIVER_DOMAIN_RUNNING &&
            bindings[index].domain->pid == pid)
            bindings[index].enabled = 0;
}

void driver_manager_tick(void) {
    for (u32 index = 0; index < DRIVER_MANAGER_BINDING_MAX; index++) {
        struct manager_binding *binding = &bindings[index];
        if (!binding->active || !binding->enabled) continue;
        int ready = dependencies_ready(binding->manifest_slot);
        if (!ready && binding->domain->state == DRIVER_DOMAIN_RUNNING) {
            if (driver_domain_stop(binding->domain)) binding->enabled = 0;
        } else if (ready && binding->domain->state == DRIVER_DOMAIN_STOPPED) {
            if (driver_domain_start(binding->domain)) binding->enabled = 0;
        }
    }
}

int driver_manager_device_removed(struct kernel_object *device) {
    struct manager_binding *binding = binding_for(device);
    int found = binding != 0;
    int result = 0;
    if (binding) {
        binding->enabled = 0;
        result = driver_domain_remove(binding->domain);
        clear_binding(binding);
    }
    for (u32 index = 0; index < managed_device_count; index++) {
        if (managed_devices[index] != device) continue;
        object_release(managed_devices[index]);
        for (u32 move = index + 1; move < managed_device_count; move++)
            managed_devices[move - 1] = managed_devices[move];
        managed_device_count--;
        managed_devices[managed_device_count] = 0;
        found = 1;
        break;
    }
    return found ? result : -1;
}

u32 driver_manager_manifest_count(void) {
    u32 count = 0;
    for (u32 index = 0; index < DRIVER_MANAGER_MANIFEST_MAX; index++)
        if (manifests[index].active) count++;
    return count;
}

u32 driver_manager_binding_count(void) {
    u32 count = 0;
    for (u32 index = 0; index < DRIVER_MANAGER_BINDING_MAX; index++)
        if (bindings[index].active) count++;
    return count;
}

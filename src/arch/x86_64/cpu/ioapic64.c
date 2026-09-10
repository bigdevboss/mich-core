#include "ioapic64.h"
#include "acpi64.h"
#include "vm64.h"
#include "irq.h"

struct ioapic64_controller {
    volatile u32 *registers;
    u32 gsi_base;
    u32 entries;
};

static struct ioapic64_controller controllers[ACPI_MAX_IOAPICS];
static u32 controller_count;

static u32 read_register(struct ioapic64_controller *controller, u8 index) {
    controller->registers[0] = index;
    return controller->registers[4];
}

static void write_register(struct ioapic64_controller *controller,
                           u8 index, u32 value) {
    controller->registers[0] = index;
    controller->registers[4] = value;
}

static struct ioapic64_controller *find_controller(u32 gsi, u32 *entry) {
    for (u32 index = 0; index < controller_count; index++) {
        struct ioapic64_controller *controller = &controllers[index];
        if (gsi >= controller->gsi_base &&
            gsi - controller->gsi_base < controller->entries) {
            *entry = gsi - controller->gsi_base;
            return controller;
        }
    }
    return 0;
}

int ioapic64_init(const struct acpi_madt_info *madt) {
    if (!madt || !madt->ioapic_count ||
        madt->ioapic_count > ACPI_MAX_IOAPICS)
        return -1;
    controller_count = 0;
    for (u32 index = 0; index < madt->ioapic_count; index++) {
        struct ioapic64_controller *controller = &controllers[index];
        controller->registers = (volatile u32 *)
            vm64_ioremap(madt->ioapics[index].address, 4096,
                         VM64_CACHE_UC);
        if (!controller->registers) return -1;
        controller->gsi_base = madt->ioapics[index].gsi_base;
        u32 version = read_register(controller, 1);
        controller->entries = ((version >> 16) & 0xFF) + 1;
        if (!controller->entries || controller->entries > 120 ||
            controller->gsi_base > 0xFFFFFFFFu - controller->entries)
            return -1;
        for (u32 previous = 0; previous < index; previous++) {
            u32 first_end = controllers[previous].gsi_base +
                            controllers[previous].entries;
            u32 second_end = controller->gsi_base + controller->entries;
            if (controller->gsi_base < first_end &&
                controllers[previous].gsi_base < second_end)
                return -1;
        }
        controller_count++;
        for (u32 entry = 0; entry < controller->entries; entry++) {
            write_register(controller, (u8)(0x10 + entry * 2),
                           (1u << 16) | 0xFFu);
            write_register(controller, (u8)(0x11 + entry * 2), 0);
        }
    }
    return 0;
}

int ioapic64_route(u32 gsi, u8 vector, u8 destination,
                   int level, int active_low, int masked) {
    if (vector < 32) return -1;
    u32 entry;
    struct ioapic64_controller *controller = find_controller(gsi, &entry);
    if (!controller) return -1;
    u32 low = vector;
    if (active_low) low |= 1u << 13;
    if (level) low |= 1u << 15;
    if (masked) low |= 1u << 16;
    irq_state_t state = irq_save();
    write_register(controller, (u8)(0x11 + entry * 2),
                   (u32)destination << 24);
    write_register(controller, (u8)(0x10 + entry * 2), low);
    irq_restore(state);
    return 0;
}

int ioapic64_mask(u32 gsi, int masked) {
    u32 entry;
    struct ioapic64_controller *controller = find_controller(gsi, &entry);
    if (!controller) return -1;
    irq_state_t state = irq_save();
    u8 reg = (u8)(0x10 + entry * 2);
    u32 low = read_register(controller, reg);
    if (masked) low |= 1u << 16;
    else low &= ~(1u << 16);
    write_register(controller, reg, low);
    irq_restore(state);
    return 0;
}

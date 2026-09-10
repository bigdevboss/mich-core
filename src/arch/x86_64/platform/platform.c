#include "types.h"
#include "platform64.h"
#include "acpi64.h"
#include "apic64.h"
#include "ioapic64.h"
#include "resource.h"

static inline void outb(u16 port, u8 value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline u8 inb(u16 port) {
    u8 value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void disable_pic(void) {
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
}

static int route_legacy_irqs(const struct acpi_madt_info *madt) {
    u8 destination = apic64_id();
    u32 routed[16];
    u32 routed_count = 0;
    for (u32 irq = 0; irq < 16; irq++) {
        u32 gsi = irq;
        int level = 0;
        int active_low = 0;
        for (u32 index = 0; index < madt->iso_count; index++) {
            const struct acpi_iso_info *override = &madt->overrides[index];
            if (override->bus != 0 || override->source != irq) continue;
            gsi = override->gsi;
            level = ((override->flags >> 2) & 3) == 3;
            active_low = (override->flags & 3) == 3;
            break;
        }
        int duplicate = 0;
        for (u32 index = 0; index < routed_count; index++)
            if (routed[index] == gsi) duplicate = 1;
        if (duplicate) continue;
        if (ioapic64_route(gsi, (u8)(0x30 + irq), destination,
                           level, active_low, 1))
            return -1;
        routed[routed_count++] = gsi;
    }
    return 0;
}

int platform64_interrupts_init(const struct acpi_madt_info *madt) {
    if (!madt) return -1;
    disable_pic();
    if (inb(0x21) != 0xFF || inb(0xA1) != 0xFF) return -1;
    if (apic64_init(madt->local_apic_address)) return -1;
    if (ioapic64_init(madt)) return -1;
    if (irq_resource_set_backend(IRQ_CONTROLLER_IOAPIC,
                                 ioapic64_mask, 0))
        return -1;
    if (route_legacy_irqs(madt)) return -1;
    if (apic64_timer_start(100)) return -1;
    return 0;
}

void platform64_eoi(void) {
    apic64_eoi();
}

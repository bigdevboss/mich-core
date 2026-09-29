#ifndef VTD64_H
#define VTD64_H

#include "types.h"

struct kernel_object;

struct vtd64_fault {
    u64 address;
    u32 owner;
    u16 segment;
    u16 source_id;
    u8 reason;
    u8 write;
};

int vtd64_init(void);
int vtd64_register_backend(void);
int vtd64_present(void);
u32 vtd64_unit_count(void);
u32 vtd64_host_address_width(void);
int vtd64_translation_enabled(void);
int vtd64_enable(void);
int vtd64_ir_active(void);
int vtd64_ir_allocate(u16 source_id, u8 vector, u8 destination,
                      int level, u16 count, u16 *handle);
int vtd64_ir_release(u16 handle, u16 count);
int vtd64_ir_entry(u16 handle, u64 *low, u64 *high);
void vtd64_ir_compose_msi(u16 handle, int multi, u32 *address, u32 *data);
void vtd64_ir_compose_ioapic(u16 handle, u8 vector, int level, int active_low,
                             int masked, u32 *low, u32 *high);
int vtd64_ir_ioapic_source_id(u16 *source_id);
u32 vtd64_fault_count(void);
int vtd64_fault_decode(u32 unit, u64 low, u64 high,
                       struct vtd64_fault *fault);
int vtd64_fault_poll(struct vtd64_fault *fault);
int vtd64_domain_create(u32 owner, struct kernel_object *pci);
int vtd64_domain_map(u32 owner, paddr_t physical, u32 pages, u64 *iova);
int vtd64_domain_unmap(u32 owner, u64 iova, u32 pages);
int vtd64_domain_translate(u32 owner, u64 iova, paddr_t *physical);
int vtd64_domain_exists(u32 owner);
int vtd64_domain_suspend(u32 owner);
int vtd64_domain_resume(u32 owner);
int vtd64_domain_destroy(u32 owner);

#endif

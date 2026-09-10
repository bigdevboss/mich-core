#ifndef APIC64_H
#define APIC64_H

#include "types.h"

#define APIC64_TIMER_VECTOR 0x40
#define APIC64_SPURIOUS_VECTOR 0xFF

int apic64_init(paddr_t physical);
int apic64_ap_start(paddr_t physical, u32 expected_id);
int apic64_ipi(u8 destination, u8 vector);
int apic64_send_init(u8 destination);
int apic64_send_sipi(u8 destination, u8 vector);
int apic64_timer_start(u32 hz);
int apic64_timer_enable(void);
void apic64_timer_mask(void);
int apic64_delay_ms(u32 milliseconds);
void apic64_eoi(void);
u8 apic64_id(void);
u32 apic64_esr(void);
u64 apic64_tsc_now(void);

#endif

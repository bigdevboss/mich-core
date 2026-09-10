#ifndef SMP64_H
#define SMP64_H

#include "types.h"

#define SMP64_MAX 8
#define SMP64_TRAMP_BASE 0x8000
// SIPI vector is the 4 KiB page number of the trampoline (Intel SDM).
#define SMP64_SIPI_VECTOR (SMP64_TRAMP_BASE >> 12)
//
// AP bring-up progress flags: written by the AP trampoline, read by the
// BSP. They live in the trampoline page at 0x550-0x553. The AP writes
// these instead of the shared UART: at bring-up the BSP is using the
// UART at the same moment the AP starts.
//
#define SMP64_AP_FLAG_ENTRY   (SMP64_TRAMP_BASE + 0x550) // 'A' real mode
#define SMP64_AP_FLAG_PROT    (SMP64_TRAMP_BASE + 0x551) // 'B' protected
#define SMP64_AP_FLAG_LONG    (SMP64_TRAMP_BASE + 0x552) // 'C' long mode
#define SMP64_AP_FLAG_CENTRY  (SMP64_TRAMP_BASE + 0x553) // 'D' C entry
#define SMP64_IPI_ACK 0x41
#define SMP64_IPI_WORK 0x42
#define SMP64_IPI_TIMER 0x43
#define SMP64_IPI_TIMER_OFF 0x44
#define SMP64_IPI_USER 0x45
#define SMP64_IPI_TSS 0x46
#define SMP64_IPI_PARK 0x47
#define SMP64_IPI_FIRST 0x41
#define SMP64_IPI_LAST 0x4F
#define SMP64_CURRENT_NONE 0xFFFFFFFFu
#define SMP64_SYSCALL_STACK 32768
#define SMP64_SYSCALL_CANARY 0x4D49434853595343ULL
#define SMP64_SPIN_TURNS 20000
#define SMP64_BOOT_TIMEOUT_MS 1000

// Per-CPU syscall frame at GS base after SWAPGS. Offsets are baked
// into syscall.asm (SC_*); keep the field order stable.
struct smp64_syscall {
    u64 kernel_rsp;
    u64 rsp;
    u64 rdi;
    u64 rsi;
    u64 rdx;
    u64 r8;
    u64 r9;
    u64 r10;
    u64 rip;
    u64 rflags;
    u64 rbx;
    u64 rbp;
    u64 r12;
    u64 r13;
    u64 r14;
    u64 r15;
};

int smp64_init(void);
int smp64_timer_tick(u64 cs, u64 rsp);
int smp64_syscall_ok(void);
int smp64_catch_ap_user(u64 number);
void smp64_syscall_prepare(void);
void smp64_gs_user(void);
void smp64_ipi_dispatch(u32 vector);
void smp64_set_current(u32 slot);
u32 smp64_running_slot(void);
void smp64_note_preempt(void);
struct smp64_syscall *smp64_syscall(void);
u32 smp64_cpu_index(void);
u32 smp64_cpu_count(void);
u8 smp64_bsp_apic_id(void);
u8 smp64_cpu_apic_id(u32 index);
int smp64_ipi_cpu(u32 index, u32 vector);
u32 smp64_ipi_ack(u32 index);
void smp64_clear_ipi_ack(u32 index);
u32 smp64_work_done(u32 index);
void smp64_clear_work_done(u32 index);
u32 smp64_ticks(u32 index);
void smp64_clear_ticks(u32 index);
u32 smp64_tr(u32 index);
void smp64_clear_tr(u32 index);
u32 smp64_cpu_current(u32 index);
u32 smp64_user_done(u32 index);
u32 smp64_user_irq(u32 index);
u32 smp64_preempts(u32 index);
void smp64_set_syscall_live(u32 index, u32 live);
int smp64_gs_is_bsp(void);
int smp64_sysstacks_ok(void);
int smp64_sysstack_ok_cpu(u32 index);
void smp64_spin_reset(void);
void smp64_spin_bsp(u32 turns);
u64 smp64_spin_count(void);
int smp64_arm_user(u32 index, const u8 *stub, u32 size);
int smp64_pin_user(u32 index, const u8 *stub, u32 size, u32 *slot_out);
void smp64_disarm_user(u32 index);
int smp64_pick_next(u32 current);
int smp64_host_start(void);
int smp64_host_note(u64 cs);

#endif

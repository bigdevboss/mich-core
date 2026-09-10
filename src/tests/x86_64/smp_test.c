#include "tests64.h"
#include "test_report.h"
#include "smp64.h"
#include "apic64.h"
#include "gdt64.h"
#include "irq.h"
#include "scheduler.h"
#include "task.h"
#include "vm64.h"
#include "kernel64_internal.h"
#include "serial64.h"

#define SMP64_USER_GETPID 16
#define SMP64_USER_STUB_AFTER 7
#define SMP64_AP 1

// mov eax, 16; syscall; jmp $
static const u8 smp64_user_stub[] = {
    0xB8, SMP64_USER_GETPID, 0x00, 0x00, 0x00,
    0x0F, 0x05,
    0xEB, 0xFE
};

// jmp $ — stays in ring3 with IF=1 so the AP timer can use TSS.RSP0.
static const u8 smp64_user_spin[] = {
    0xEB, 0xFE
};

static int wait_flag(u32 (*load)(u32), u32 index) {
    u32 deadline = SMP64_BOOT_TIMEOUT_MS / 10;
    while (deadline--) {
        if (load(index)) return 0;
        apic64_delay_ms(10);
    }
    return -1;
}

static int test_smp64_ipi(void) {
    u32 n = smp64_cpu_count();
    for (u32 index = 1; index < n; index++) {
        smp64_clear_ipi_ack(index);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        if (smp64_ipi_cpu(index, SMP64_IPI_ACK)) return -1;
        if (wait_flag(smp64_ipi_ack, index)) return -1;
    }
    return 0;
}

static int test_smp64_spin(void) {
    u32 n = smp64_cpu_count();
    smp64_spin_reset();
    for (u32 index = 1; index < n; index++)
        smp64_clear_work_done(index);
    for (u32 index = 1; index < n; index++)
        if (smp64_ipi_cpu(index, SMP64_IPI_WORK))
            return -1;
    smp64_spin_bsp(SMP64_SPIN_TURNS);
    for (u32 index = 1; index < n; index++)
        if (wait_flag(smp64_work_done, index)) return -1;
    if (smp64_spin_count() != (u64)n * SMP64_SPIN_TURNS)
        return -1;
    return 0;
}

static int test_smp64_timer(void) {
    u32 n = smp64_cpu_count();
    int failed = 0;
    for (u32 index = 1; index < n; index++) {
        smp64_clear_ticks(index);
        if (smp64_ipi_cpu(index, SMP64_IPI_TIMER))
            return -1;
    }
    for (u32 index = 1; index < n; index++)
        if (wait_flag(smp64_ticks, index)) failed = 1;
    for (u32 index = 1; index < n; index++)
        smp64_ipi_cpu(index, SMP64_IPI_TIMER_OFF);
    return failed ? -1 : 0;
}

static int test_smp64_tss(void) {
    u16 tr;
    u32 n = smp64_cpu_count();
    __asm__ volatile("str %0" : "=m"(tr));
    if (tr != gdt64_tss_selector(0)) return -1;
    if (!gdt64_tss_ok()) return -1;
    for (u32 index = 1; index < n; index++) {
        smp64_clear_tr(index);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        if (smp64_ipi_cpu(index, SMP64_IPI_TSS)) return -1;
        if (wait_flag(smp64_tr, index)) return -1;
        if (smp64_tr(index) != gdt64_tss_selector(index)) return -1;
        if (smp64_tr(index) == gdt64_tss_selector(0)) return -1;
    }
    return 0;
}

static int test_smp64_current(void) {
    u32 n = smp64_cpu_count();
    int pinned = 2;
    int saved;
    int next;
    if (smp64_cpu_current(0) == SMP64_CURRENT_NONE)
        return -1;
    for (u32 index = 1; index < n; index++) {
        if (smp64_cpu_current(index) != SMP64_CURRENT_NONE)
            return -1;
        if (smp64_cpu_current(index) == smp64_cpu_current(0))
            return -1;
    }
    // Pin init64-two so BSP pick_next must skip it. Restores on_cpu.
    if (pinned >= task_pool_count ||
        task_pool[pinned].state != TASK_RUNNING)
        return -1;
    saved = task_pool[pinned].on_cpu;
    task_pool[pinned].on_cpu = (int)SMP64_AP;
    next = scheduler_pick_next(1);
    task_pool[pinned].on_cpu = saved;
    if (next == pinned || next < 0) return -1;
    return 0;
}

static int test_smp64_syscall(void) {
    return smp64_sysstacks_ok() ? 0 : -1;
}

static int test_smp64_user(void) {
    u32 slot;
    if (smp64_arm_user(SMP64_AP, smp64_user_stub, sizeof(smp64_user_stub)))
        return -1;
    slot = smp64_cpu_current(SMP64_AP);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    if (smp64_ipi_cpu(SMP64_AP, SMP64_IPI_USER)) {
        smp64_disarm_user(SMP64_AP);
        return -1;
    }
    if (wait_flag(smp64_user_done, SMP64_AP) ||
        smp64_cpu_current(SMP64_AP) != slot ||
        task_pool[slot].on_cpu != (int)SMP64_AP ||
        !smp64_sysstack_ok_cpu(SMP64_AP)) {
        smp64_disarm_user(SMP64_AP);
        return -1;
    }
    smp64_disarm_user(SMP64_AP);
    return 0;
}

static int test_smp64_user_irq(void) {
    u32 slot;
    u32 bsp = current_task_slot;
    u64 bsp_rip;
    if (bsp >= MAX_TASKS) return -1;
    bsp_rip = task_contexts[bsp].rip;
    if (smp64_arm_user(SMP64_AP, smp64_user_spin, sizeof(smp64_user_spin)))
        return -1;
    slot = smp64_cpu_current(SMP64_AP);
    smp64_clear_ticks(SMP64_AP);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    if (smp64_ipi_cpu(SMP64_AP, SMP64_IPI_TIMER) ||
        smp64_ipi_cpu(SMP64_AP, SMP64_IPI_USER)) {
        smp64_ipi_cpu(SMP64_AP, SMP64_IPI_TIMER_OFF);
        smp64_disarm_user(SMP64_AP);
        return -1;
    }
    if (wait_flag(smp64_user_irq, SMP64_AP)) {
        smp64_ipi_cpu(SMP64_AP, SMP64_IPI_TIMER_OFF);
        smp64_disarm_user(SMP64_AP);
        return -1;
    }
    smp64_ipi_cpu(SMP64_AP, SMP64_IPI_TIMER_OFF);
    if (smp64_ipi_cpu(SMP64_AP, SMP64_IPI_PARK)) {
        smp64_disarm_user(SMP64_AP);
        return -1;
    }
    if (wait_flag(smp64_user_done, SMP64_AP) ||
        smp64_cpu_current(SMP64_AP) != slot ||
        !gdt64_stack_ok() ||
        !smp64_sysstack_ok_cpu(SMP64_AP)) {
        smp64_disarm_user(SMP64_AP);
        return -1;
    }
    if (!smp64_preempts(SMP64_AP) ||
        current_task_slot != bsp ||
        task_contexts[bsp].rip != bsp_rip ||
        task_contexts[slot].rip != VM64_PROGRAM_BASE ||
        task_contexts[slot].rsp != VM64_STACK_TOP) {
        smp64_disarm_user(SMP64_AP);
        return -1;
    }
    smp64_disarm_user(SMP64_AP);
    return 0;
}

static int test_smp64_live(void) {
    u32 slot;
    u32 bsp = current_task_slot;
    u64 bsp_rip;
    u64 pid;
    u32 deadline;
    if (bsp >= MAX_TASKS) return -1;
    bsp_rip = task_contexts[bsp].rip;
    if (smp64_arm_user(SMP64_AP, smp64_user_stub, sizeof(smp64_user_stub)))
        return -1;
    slot = smp64_cpu_current(SMP64_AP);
    pid = (u64)(u32)task_pool[slot].id;
    smp64_clear_ticks(SMP64_AP);
    smp64_set_syscall_live(SMP64_AP, 1);
    if (smp64_ipi_cpu(SMP64_AP, SMP64_IPI_TIMER) ||
        smp64_ipi_cpu(SMP64_AP, SMP64_IPI_USER)) {
        smp64_ipi_cpu(SMP64_AP, SMP64_IPI_TIMER_OFF);
        smp64_disarm_user(SMP64_AP);
        return -1;
    }
    // First ring3 tick can land before syscall; wait for saved getpid.
    deadline = SMP64_BOOT_TIMEOUT_MS / 10;
    while (deadline--) {
        if (smp64_preempts(SMP64_AP) && task_contexts[slot].rax == pid)
            break;
        apic64_delay_ms(10);
    }
    if (task_contexts[slot].rax != pid) {
        smp64_ipi_cpu(SMP64_AP, SMP64_IPI_TIMER_OFF);
        smp64_disarm_user(SMP64_AP);
        return -1;
    }
    smp64_ipi_cpu(SMP64_AP, SMP64_IPI_TIMER_OFF);
    if (smp64_ipi_cpu(SMP64_AP, SMP64_IPI_PARK)) {
        smp64_disarm_user(SMP64_AP);
        return -1;
    }
    if (wait_flag(smp64_user_done, SMP64_AP) ||
        smp64_cpu_current(SMP64_AP) != slot ||
        !gdt64_stack_ok() ||
        !smp64_sysstack_ok_cpu(SMP64_AP)) {
        smp64_disarm_user(SMP64_AP);
        return -1;
    }
    if (!smp64_preempts(SMP64_AP) ||
        current_task_slot != bsp ||
        task_contexts[bsp].rip != bsp_rip ||
        task_contexts[slot].rax != pid ||
        task_contexts[slot].rax == (u64)(u32)task_pool[bsp].id ||
        task_contexts[slot].rip != VM64_PROGRAM_BASE + SMP64_USER_STUB_AFTER ||
        task_contexts[slot].rsp != VM64_STACK_TOP) {
        smp64_disarm_user(SMP64_AP);
        return -1;
    }
    smp64_disarm_user(SMP64_AP);
    return 0;
}

static void drop_ap_tasks(u32 a, u32 b) {
    smp64_ipi_cpu(SMP64_AP, SMP64_IPI_TIMER_OFF);
    smp64_disarm_user(SMP64_AP);
    if (a < MAX_TASKS && task_pool[a].state != TASK_FREE)
        task_free_slot(&task_pool[a]);
    if (b < MAX_TASKS && b != a && task_pool[b].state != TASK_FREE)
        task_free_slot(&task_pool[b]);
}

static int test_smp64_schedule(void) {
    u32 a;
    u32 b;
    u32 seen_a = 0;
    u32 seen_b = 0;
    u32 deadline;
    u32 bsp = current_task_slot;
    if (smp64_pin_user(SMP64_AP, smp64_user_spin, sizeof(smp64_user_spin), &b))
        return -1;
    if (smp64_arm_user(SMP64_AP, smp64_user_spin, sizeof(smp64_user_spin))) {
        task_free_slot(&task_pool[b]);
        return -1;
    }
    a = smp64_cpu_current(SMP64_AP);
    if (a >= MAX_TASKS || b >= MAX_TASKS || a == b) {
        drop_ap_tasks(a, b);
        return -1;
    }
    smp64_clear_ticks(SMP64_AP);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    if (smp64_ipi_cpu(SMP64_AP, SMP64_IPI_TIMER) ||
        smp64_ipi_cpu(SMP64_AP, SMP64_IPI_USER)) {
        drop_ap_tasks(a, b);
        return -1;
    }
    deadline = SMP64_BOOT_TIMEOUT_MS / 10;
    while (deadline--) {
        u32 cur = smp64_cpu_current(SMP64_AP);
        if (cur == a) seen_a = 1;
        if (cur == b) seen_b = 1;
        if (seen_a && seen_b) break;
        apic64_delay_ms(10);
    }
    smp64_ipi_cpu(SMP64_AP, SMP64_IPI_TIMER_OFF);
    if (smp64_ipi_cpu(SMP64_AP, SMP64_IPI_PARK)) {
        drop_ap_tasks(a, b);
        return -1;
    }
    if (wait_flag(smp64_user_done, SMP64_AP) ||
        current_task_slot != bsp ||
        !seen_a || !seen_b) {
        drop_ap_tasks(a, b);
        return -1;
    }
    drop_ap_tasks(a, b);
    return 0;
}

int tests64_run_smp(void) {
    irq_state_t irq_state = irq_save();
    if (smp64_cpu_count() < 2) {
        irq_restore(irq_state);
        return 0;
    }
    if (test_report_record(TEST_ID_SMP_IPI, test_smp64_ipi())) {
        irq_restore(irq_state);
        return -1;
    }
    serial64_write("Mich x86_64: SMP IPI round-trip pass\n");
    if (test_report_record(TEST_ID_SMP_SPIN, test_smp64_spin())) {
        irq_restore(irq_state);
        return -1;
    }
    serial64_write("Mich x86_64: SMP spinlock stress pass\n");
    if (test_report_record(TEST_ID_SMP_TIMER, test_smp64_timer())) {
        irq_restore(irq_state);
        return -1;
    }
    serial64_write("Mich x86_64: SMP AP timer pass\n");
    if (test_report_record(TEST_ID_SMP_TSS, test_smp64_tss())) {
        irq_restore(irq_state);
        return -1;
    }
    serial64_write("Mich x86_64: SMP per-CPU TSS pass\n");
    if (test_report_record(TEST_ID_SMP_CURRENT, test_smp64_current())) {
        irq_restore(irq_state);
        return -1;
    }
    serial64_write("Mich x86_64: SMP per-CPU current pass\n");
    if (test_report_record(TEST_ID_SMP_SYSCALL, test_smp64_syscall())) {
        irq_restore(irq_state);
        return -1;
    }
    serial64_write("Mich x86_64: SMP per-CPU syscall pass\n");
    if (test_report_record(TEST_ID_SMP_USER, test_smp64_user())) {
        irq_restore(irq_state);
        return -1;
    }
    serial64_write("Mich x86_64: SMP AP userspace pass\n");
    if (test_report_record(TEST_ID_SMP_USER_IRQ, test_smp64_user_irq())) {
        irq_restore(irq_state);
        return -1;
    }
    serial64_write("Mich x86_64: SMP AP user IRQ pass\n");
    serial64_write("Mich x86_64: SMP AP preempt pass\n");
    if (test_report_record(TEST_ID_SMP_LIVE, test_smp64_live())) {
        irq_restore(irq_state);
        return -1;
    }
    serial64_write("Mich x86_64: SMP AP live syscall pass\n");
    if (test_report_record(TEST_ID_SMP_SCHED, test_smp64_schedule())) {
        irq_restore(irq_state);
        return -1;
    }
    serial64_write("Mich x86_64: SMP AP scheduler pass\n");
    irq_restore(irq_state);
    return 0;
}

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

// jmp $ - stays in ring3 with IF=1 so the AP timer can use TSS.RSP0.
static const u8 smp64_user_spin[] = {
    0xEB, 0xFE
};

// inc qword [VM64_STACK_TOP - 8]; jmp back - counts this task's turns in
// the stack page, the writable one, where the kernel can read the count.
// The pin does not zero that page, so the tests clear the count through
// the kernel pointer before the task is allowed to start.
static const u8 smp64_spin_stub[] = {
    0x48, 0xFF, 0x05, 0xF1, 0xFF, 0x1F, 0x00, 0xEB, 0xF7
};

// Waits for the word at VM64_STACK_TOP - 8 to go nonzero, then returns from
// a syscall with a stack pointer that maps nowhere: that is the bad return
// the containment has to answer. The stack pointer is the register to
// plant, not rcx, because syscall overwrites rcx with its return address;
// the entry saves rsp before it switches to the kernel stack. The gate is
// there because the entry path clears the per-CPU syscall flag: the test
// has to arm the real dispatch after this task is already in ring 3, and
// only then open the gate.
//   movabs rax, [VM64_STACK_TOP - 8]; test rax, rax; jz -15
//   mov rsp, 1; mov eax, 16; syscall; jmp $
static const u8 smp64_bad_return_stub[] = {
    0x48, 0xA1, 0xF8, 0xFF, 0x1F, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x48, 0x85, 0xC0,
    0x74, 0xF1,
    0x48, 0xC7, 0xC4, 0x01, 0x00, 0x00, 0x00,
    0xB8, SMP64_USER_GETPID, 0x00, 0x00, 0x00,
    0x0F, 0x05,
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

// Park the CPU before the slot goes: freeing a task another CPU is still
// running would leave that CPU on a recycled slot.
static int drop_pinned(u32 slot, u32 index) {
    smp64_ipi_cpu(index, SMP64_IPI_PARK);
    if (wait_flag(smp64_user_done, index)) return -1;
    smp64_ipi_cpu(index, SMP64_IPI_TIMER_OFF);
    smp64_disarm_user(index);
    if (slot < MAX_TASKS && task_pool[slot].state != TASK_FREE)
        task_free_slot(&task_pool[slot]);
    return 0;
}

// The kick is asynchronous: the counter may already sit past the sample,
// so the wait has to be for the IPI being handled, which is the preempt
// this CPU records when it takes the frame path.
static int wait_preempts(u64 above) {
    u32 deadline = SMP64_BOOT_TIMEOUT_MS / 10 * 4;
    while (deadline--) {
        if (smp64_preempts(SMP64_AP) > above) return 0;
        apic64_delay_ms(10);
    }
    return -1;
}

static int wait_counter(volatile u64 *counter, u64 above) {
    u32 deadline = SMP64_BOOT_TIMEOUT_MS / 10 * 4;
    while (deadline--) {
        if (__atomic_load_n(counter, __ATOMIC_ACQUIRE) > above) return 0;
        apic64_delay_ms(10);
    }
    return -1;
}

// An ordinary task pinned to an AP: with the AP parked in ring 0 and its
// tick masked, only the reschedule IPI can start it, and the AP picks it
// out of the pool on its own.
static int test_smp64_pinned(void) {
    volatile u64 *counter = 0;
    u64 turns;
    u64 preempts;
    u32 slot;
    if (smp64_pin_task(SMP64_AP, smp64_spin_stub, sizeof(smp64_spin_stub),
                       &slot, (u64 **)&counter))
        return -1;
    counter[0] = 0;
    apic64_delay_ms(20);
    if (counter[0] || smp64_cpu_current(SMP64_AP) != SMP64_CURRENT_NONE) {
        drop_pinned(slot, SMP64_AP);
        return -1;
    }
    if (smp64_resched_cpu(SMP64_AP) ||
        wait_counter(counter, 0) ||
        smp64_cpu_current(SMP64_AP) != slot ||
        task_pool[slot].state != TASK_RUNNING) {
        drop_pinned(slot, SMP64_AP);
        return -1;
    }
    // A second kick lands in ring 3: the frame is rewritten the way the
    // tick rewrites it and the same task keeps the CPU.
    turns = counter[0];
    preempts = (u64)smp64_preempts(SMP64_AP);
    if (smp64_resched_cpu(SMP64_AP) ||
        wait_preempts(preempts) ||
        counter[0] <= turns ||
        smp64_cpu_current(SMP64_AP) != slot) {
        drop_pinned(slot, SMP64_AP);
        return -1;
    }
    if (drop_pinned(slot, SMP64_AP)) return -1;
    if (smp64_cpu_current(SMP64_AP) != SMP64_CURRENT_NONE) return -1;
    return 0;
}

// A pinned task that returns from a syscall to an unmapped address is
// contained: the task goes, the CPU stays, and the next task pinned to it
// takes over. Without the handover the AP would sit in cli; hlt for the
// rest of the boot and every later test on it would hang.
static int test_smp64_contain(void) {
    volatile u64 *gate = 0;
    volatile u64 *counter = 0;
    u32 bad;
    u32 next;
    u32 deadline;
    if (smp64_pin_task(SMP64_AP, smp64_bad_return_stub,
                       sizeof(smp64_bad_return_stub), &bad,
                       (u64 **)&gate)) {
        smp64_disarm_user(SMP64_AP);
        return -1;
    }
    if (smp64_pin_task(SMP64_AP, smp64_spin_stub, sizeof(smp64_spin_stub),
                       &next, (u64 **)&counter)) {
        smp64_disarm_user(SMP64_AP);
        return -1;
    }
    gate[0] = 0;
    counter[0] = 0;
    // The bad task has to be the one this CPU picks, or the test would prove
    // nothing; the pool hands out the lowest free slot, and this checks that
    // instead of trusting it.
    if (smp64_pick_pinned(SMP64_AP) != (int)bad) {
        drop_pinned(next, SMP64_AP);
        drop_pinned(bad, SMP64_AP);
        return -1;
    }
    if (smp64_resched_cpu(SMP64_AP)) {
        drop_pinned(next, SMP64_AP);
        drop_pinned(bad, SMP64_AP);
        return -1;
    }
    // The pickup records the slot before it enters ring 3, so the record is
    // the fact to wait for. The gate holds the syscall back, which is what
    // lets the test arm the real dispatch after the entry cleared it.
    deadline = SMP64_BOOT_TIMEOUT_MS / 10;
    while (deadline-- && smp64_cpu_current(SMP64_AP) != bad)
        apic64_delay_ms(10);
    if (smp64_cpu_current(SMP64_AP) != bad) {
        drop_pinned(next, SMP64_AP);
        drop_pinned(bad, SMP64_AP);
        return -1;
    }
    smp64_set_syscall_live(SMP64_AP, 1);
    gate[0] = 1;
    if (wait_counter(counter, 0)) {
        smp64_set_syscall_live(SMP64_AP, 0);
        drop_pinned(next, SMP64_AP);
        drop_pinned(bad, SMP64_AP);
        return -1;
    }
    if (smp64_cpu_current(SMP64_AP) != next ||
        task_pool[bad].state == TASK_RUNNING) {
        smp64_set_syscall_live(SMP64_AP, 0);
        drop_pinned(next, SMP64_AP);
        drop_pinned(bad, SMP64_AP);
        return -1;
    }
    smp64_set_syscall_live(SMP64_AP, 0);
    smp64_ipi_cpu(SMP64_AP, SMP64_IPI_TIMER_OFF);
    if (drop_pinned(next, SMP64_AP)) return -1;
    if (drop_pinned(bad, SMP64_AP)) return -1;
    return 0;
}

// Two ordinary pinned tasks on two CPUs at once. Both counts have to move
// inside one sampling window: that is what concurrent means here, and two
// sequential runs would not move both counts in the same window.
static int test_smp64_parallel(void) {
    const u32 index_a = SMP64_AP;
    const u32 index_b = SMP64_AP + 1;
    volatile u64 *ca = 0;
    volatile u64 *cb = 0;
    u64 a0;
    u64 b0;
    u64 a1;
    u64 b1;
    u32 slot_a;
    u32 slot_b;
    if (smp64_cpu_count() < 3) {
        serial64_write("Mich x86_64: SMP two-task parallel skipped (");
        serial64_hex(smp64_cpu_count());
        serial64_write(" cpus)\n");
        return 0;
    }
    if (smp64_pin_task(index_a, smp64_spin_stub, sizeof(smp64_spin_stub),
                       &slot_a, (u64 **)&ca))
        return -1;
    if (smp64_pin_task(index_b, smp64_spin_stub, sizeof(smp64_spin_stub),
                       &slot_b, (u64 **)&cb)) {
        drop_pinned(slot_a, index_a);
        return -1;
    }
    ca[0] = 0;
    cb[0] = 0;
    apic64_delay_ms(20);
    if (ca[0] || cb[0] ||
        smp64_cpu_current(index_a) != SMP64_CURRENT_NONE ||
        smp64_cpu_current(index_b) != SMP64_CURRENT_NONE) {
        drop_pinned(slot_a, index_a);
        drop_pinned(slot_b, index_b);
        return -1;
    }
    if (smp64_resched_cpu(index_a) || smp64_resched_cpu(index_b) ||
        wait_counter(ca, 0) || wait_counter(cb, 0) ||
        smp64_cpu_current(index_a) != slot_a ||
        smp64_cpu_current(index_b) != slot_b) {
        drop_pinned(slot_a, index_a);
        drop_pinned(slot_b, index_b);
        return -1;
    }
    a0 = ca[0];
    b0 = cb[0];
    apic64_delay_ms(20);
    a1 = ca[0];
    b1 = cb[0];
    if (a1 <= a0 || b1 <= b0) {
        drop_pinned(slot_a, index_a);
        drop_pinned(slot_b, index_b);
        return -1;
    }
    serial64_write("Mich x86_64: SMP two-task parallel pass (d1=");
    serial64_hex(a1 - a0);
    serial64_write(" d2=");
    serial64_hex(b1 - b0);
    serial64_write(")\n");
    if (drop_pinned(slot_a, index_a)) return -1;
    if (drop_pinned(slot_b, index_b)) return -1;
    return 0;
}

// Two CPU-bound tasks with a fixed amount of work each: the same turn
// count has to be reached once with the pair sharing a CPU and once with a
// CPU each, so the ratio of the two wall times is the speedup. The wall
// time is guest TSC ticks, and the turns are the stub's own increments.
#define SPEEDUP_TURNS 20000000ull
#define SPEEDUP_DEADLINE_MS 8000
#define SPEEDUP_QUANTUM_MS 10

// The measured pair should have the host to itself, and a spinning BSP
// takes a core from it. A ring-0 tick is accounted and returns without
// touching a task context, so this waits in hlt between ticks and leaves
// the caller's interrupt state alone.
static void speedup_wait_ms(u64 milliseconds) {
    u64 deadline = apic64_tsc_now() + milliseconds * 1000000u;
    while (apic64_tsc_now() < deadline)
        __asm__ volatile("sti; hlt; cli" ::: "memory");
}

static void speedup_line(const char *label, u64 us) {
    serial64_write("Mich x86_64: SMP speedup ");
    serial64_write(label);
    serial64_write(" us=");
    serial64_hex(us);
    serial64_write("\n");
}

// Polls the pair until both reach the turn count, kicking a shared pair so
// it alternates the way a tick would. Pinning is the caller's CPU choice.
static int speedup_work(u32 index_a, u32 index_b, u64 *us_out) {
    volatile u64 *ca = 0;
    volatile u64 *cb = 0;
    u64 start;
    u64 stop;
    u64 done = 0;
    u32 slot_a;
    u32 slot_b;
    u32 elapsed;
    if (smp64_pin_task(index_a, smp64_spin_stub, sizeof(smp64_spin_stub),
                       &slot_a, (u64 **)&ca))
        return -1;
    if (smp64_pin_task(index_b, smp64_spin_stub, sizeof(smp64_spin_stub),
                       &slot_b, (u64 **)&cb)) {
        drop_pinned(slot_a, index_a);
        return -1;
    }
    ca[0] = 0;
    cb[0] = 0;
    if (smp64_resched_cpu(index_a) || wait_counter(ca, 0)) {
        drop_pinned(slot_a, index_a);
        drop_pinned(slot_b, index_b);
        return -1;
    }
    if (index_b != index_a &&
        (smp64_resched_cpu(index_b) || wait_counter(cb, 0))) {
        drop_pinned(slot_a, index_a);
        drop_pinned(slot_b, index_b);
        return -1;
    }
    start = apic64_tsc_now();
    for (elapsed = 0; elapsed < SPEEDUP_DEADLINE_MS;
         elapsed += SPEEDUP_QUANTUM_MS) {
        if (ca[0] >= SPEEDUP_TURNS && cb[0] >= SPEEDUP_TURNS) {
            done = 1;
            break;
        }
        speedup_wait_ms(SPEEDUP_QUANTUM_MS);
        if (index_b == index_a && smp64_resched_cpu(index_a)) break;
    }
    stop = apic64_tsc_now();
    if (drop_pinned(slot_a, index_a)) return -1;
    if (drop_pinned(slot_b, index_b)) return -1;
    if (!done) return -1;
    // The TSC runs at a fixed 1 GHz for the emulated CPU, the same rate
    // apic64_delay_ms is built on.
    *us_out = (stop - start) / 1000u;
    return 0;
}

// The claim as a number: two tasks doing the same fixed work take less wall
// time on two CPUs than while sharing one. The table this feeds is a QEMU
// measurement, so it is labelled as one.
static int test_smp64_speedup(void) {
    u64 shared_us = 0;
    u64 split_us = 0;
    if (speedup_work(SMP64_AP, SMP64_AP, &shared_us)) {
        serial64_write("Mich x86_64: SMP speedup shared pair failed\n");
        return -1;
    }
    speedup_line("two-tasks one-cpu", shared_us);
    if (smp64_cpu_count() < 3) {
        serial64_write("Mich x86_64: SMP speedup two-tasks two-cpu skipped\n");
        return 0;
    }
    if (speedup_work(SMP64_AP, SMP64_AP + 1, &split_us)) {
        serial64_write("Mich x86_64: SMP speedup split pair failed\n");
        return -1;
    }
    serial64_write("Mich x86_64: SMP speedup two-tasks two-cpu us=");
    serial64_hex(split_us);
    serial64_write(" x100=");
    serial64_hex(split_us ? (shared_us * 100u) / split_us : 0);
    serial64_write("\n");
    // The ratio is printed, not asserted: the host this runs on has fewer
    // cores than the guest has CPUs, so what the split pair saves is the
    // host's to give. Both pairs still had to reach the turn count.
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
    if (test_report_record(TEST_ID_SMP_PINNED, test_smp64_pinned())) {
        irq_restore(irq_state);
        return -1;
    }
    serial64_write("Mich x86_64: SMP AP pinned task pass\n");
    if (test_report_record(TEST_ID_SMP_CONTAIN, test_smp64_contain())) {
        irq_restore(irq_state);
        return -1;
    }
    serial64_write("Mich x86_64: SMP AP syscall containment pass\n");
    // This one reports its own pass line: the counts it measured are part
    // of the claim, and at two CPUs it says it was skipped instead.
    if (test_report_record(TEST_ID_SMP_PARALLEL, test_smp64_parallel())) {
        irq_restore(irq_state);
        return -1;
    }
    if (test_report_record(TEST_ID_SMP_SPEEDUP, test_smp64_speedup())) {
        irq_restore(irq_state);
        return -1;
    }
    irq_restore(irq_state);
    return 0;
}

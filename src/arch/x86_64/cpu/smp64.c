#include "smp64.h"
#include "spinlock.h"
#include "types.h"
#include "serial64.h"
#include "apic64.h"
#include "gdt64.h"
#include "idt64.h"
#include "acpi64.h"
#include "irq.h"
#include "scheduler.h"
#include "task.h"
#include "vm64.h"
#include "kernel64_internal.h"

// Trampoline chunks (smp_tramp.asm), placed by the BSP into the page
// at SMP64_TRAMP_BASE: entry 0x000, overlay 0x500, GDTs 0x100, code
// 0x200/0x300/0x400. SIPI starts the AP at that page (vector =
// physical >> 12).
extern const u8 smp_tramp_gdt[];
extern const u32 smp_tramp_gdt_size;
extern const u8 smp_tramp_code16[];
extern const u32 smp_tramp_code16_size;
extern const u8 smp_tramp_code32[];
extern const u32 smp_tramp_code32_size;
extern const u8 smp_tramp_code64[];
extern const u32 smp_tramp_code64_size;
extern const u8 smp_tramp_entry[];
extern const u32 smp_tramp_entry_size;

#define SMP64_STATE_OFF 0
#define SMP64_STATE_BOOTING 2
#define SMP64_STATE_ONLINE 3
#define SMP64_STACK_SIZE 16384
#define MSR_EFER 0xC0000080ULL
#define MSR_STAR 0xC0000081ULL
#define MSR_LSTAR 0xC0000082ULL
#define MSR_FMASK 0xC0000084ULL
#define MSR_GS_BASE 0xC0000101ULL
#define MSR_KERNEL_GS_BASE 0xC0000102ULL
#define SMP64_USER_GETPID 16
#define SMP64_USER_STUB_AFTER 7

struct smp64_cpu {
    struct smp64_syscall sc;
    u32 active;
    u32 apic_id;
    u32 acpi_id;
    u32 index;
    volatile u32 state;
    volatile u32 ipi_ack;
    volatile u32 work_done;
    volatile u32 ticks;
    volatile u32 current;
    volatile u32 user_done;
    volatile u32 user_irq;
    volatile u32 preempts;
    volatile u32 tr;
    // When set, AP syscalls reach dispatch instead of the test catch.
    volatile u32 syscall_live;
};

struct smp64_stack {
    u64 canary;
    u8 bytes[SMP64_STACK_SIZE];
} __attribute__((aligned(16)));

struct smp64_sysstack {
    u64 canary;
    u64 reserved;
    u8 bytes[SMP64_SYSCALL_STACK];
} __attribute__((aligned(16)));

static struct smp64_cpu smp64_cpus[SMP64_MAX];
static struct smp64_stack smp64_stacks[SMP64_MAX];
static struct smp64_sysstack smp64_sysstacks[SMP64_MAX];
static u32 smp64_online;
static u8 smp64_bsp_id;
static struct smp64_cpu *smp64_cpu_by_apic[256];
static struct spinlock smp64_spin = SPINLOCK_INIT;
static volatile u64 smp64_spin_counter;
static u64 smp64_kernel_cr3;
static u64 smp64_kernel_cr4;
static u64 smp64_kernel_efer;
static u64 smp64_kernel_star;
static u64 smp64_kernel_lstar;
static u64 smp64_kernel_fmask;
static u64 smp64_user_rip;
static u64 smp64_user_rsp;
static u64 smp64_user_cr3;

extern void user64_enter(u64 rip, u64 rsp, u64 argument);

_Static_assert(__builtin_offsetof(struct smp64_syscall, rsp) == 8,
               "syscall.asm SC_RSP");
_Static_assert(__builtin_offsetof(struct smp64_syscall, r15) == 120,
               "syscall.asm SC_R15");
_Static_assert(__builtin_offsetof(struct smp64_cpu, sc) == 0,
               "GS base is smp64_cpu");
_Static_assert(SMP64_MAX == GDT64_CPU_MAX, "TSS slots match SMP");

static void smp64_copy(u8 *dst, const u8 *src, u32 size) {
    for (u32 index = 0; index < size; index++)
        dst[index] = src[index];
}

static u64 smp64_rdmsr(u32 msr) {
    u32 low;
    u32 high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((u64)high << 32) | low;
}

static void smp64_wrmsr(u32 msr, u64 value) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((u32)value),
                     "d"((u32)(value >> 32)) : "memory");
}

static struct smp64_cpu *smp64_this(void) {
    struct smp64_cpu *cpu = smp64_cpu_by_apic[apic64_id()];
    return cpu ? cpu : &smp64_cpus[0];
}

u32 smp64_cpu_index(void) {
    return smp64_this()->index;
}

void smp64_set_current(u32 slot) {
    smp64_this()->current = slot;
}

u32 smp64_running_slot(void) {
    struct smp64_cpu *cpu = smp64_this();
    if (cpu->index != 0 && cpu->current != SMP64_CURRENT_NONE)
        return cpu->current;
    return current_task_slot;
}

void smp64_note_preempt(void) {
    __atomic_fetch_add(&smp64_this()->preempts, 1, __ATOMIC_RELEASE);
}

struct smp64_syscall *smp64_syscall(void) {
    return &smp64_this()->sc;
}

int smp64_syscall_ok(void) {
    return smp64_sysstacks[smp64_cpu_index()].canary == SMP64_SYSCALL_CANARY;
}

void smp64_syscall_prepare(void) {
    for (u32 index = 0; index < SMP64_MAX; index++) {
        smp64_sysstacks[index].canary = SMP64_SYSCALL_CANARY;
        smp64_cpus[index].sc.kernel_rsp =
            (u64)(uptr_t)&smp64_sysstacks[index].bytes[SMP64_SYSCALL_STACK];
    }
}

//
// Kernel: GS_BASE = pcpu, KERNEL_GS_BASE = 0. User: the reverse, so
// SWAPGS on syscall lands on this CPU's frame. Explicit MSRs, not
// SWAPGS, so this does not depend on the current GS.
//
static void smp64_gs_kernel(struct smp64_cpu *cpu) {
    u64 addr = (u64)(uptr_t)cpu;
    smp64_wrmsr(MSR_GS_BASE, addr);
    smp64_wrmsr(MSR_KERNEL_GS_BASE, 0);
}

void smp64_gs_user(void) {
    u64 addr = (u64)(uptr_t)smp64_this();
    smp64_wrmsr(MSR_GS_BASE, 0);
    smp64_wrmsr(MSR_KERNEL_GS_BASE, addr);
}

//
// Live AP userspace test: never fall through to BSP current_task_slot.
// Park on the kernel CR3 so the BSP can free the task's space.
//
int smp64_catch_ap_user(u64 number) {
    struct smp64_cpu *cpu = smp64_this();
    u64 rsp;
    u32 ds;
    if (cpu->index == 0 || cpu->current == SMP64_CURRENT_NONE)
        return 0;
    if (__atomic_load_n(&cpu->syscall_live, __ATOMIC_ACQUIRE))
        return 0;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    u64 base = (u64)(uptr_t)smp64_sysstacks[cpu->index].bytes;
    u64 end = base + SMP64_SYSCALL_STACK;
    int ok = number == SMP64_USER_GETPID &&
             rsp > base && rsp <= end &&
             smp64_rdmsr(MSR_GS_BASE) == (u64)(uptr_t)cpu &&
             cpu->sc.rip == smp64_user_rip + SMP64_USER_STUB_AFTER;
    ds = GDT64_KERNEL_DS;
    __asm__ volatile("mov %0, %%ds; mov %0, %%es" : : "r"(ds));
    smp64_gs_kernel(cpu);
    vm64_activate(smp64_kernel_cr3);
    apic64_timer_mask();
    if (ok)
        __atomic_store_n(&cpu->user_done, 1, __ATOMIC_RELEASE);
    return 1;
}

//
// IPI from hlt is same-privilege: the hardware frame has no SS/RSP, so
// we cannot rewrite it into a user iret. EOI here because this path
// never returns through irq64_common.
//
static void smp64_ap_enter_user(struct smp64_cpu *cpu) {
    if (!cpu || cpu->current == SMP64_CURRENT_NONE) return;
    apic64_eoi();
    smp64_gs_user();
    vm64_activate(smp64_user_cr3);
    user64_enter(smp64_user_rip, smp64_user_rsp, 0);
    for (;;)
        __asm__ volatile("cli; hlt" ::: "memory");
}

//
// Runs on the secondary CPU in long mode after the trampoline. The
// trampoline still uses the bootloader PML4 at 0x70000; switch to the
// kernel CR3 before any ioremap or NX data access. ONLINE is stored
// after sti so an IPI cannot land with IF=0.
//
static void smp64_ap_entry(u32 expected_id) {
    const struct acpi_madt_info *madt = acpi64_madt();
    struct smp64_cpu *cpu = 0;
    int rc;
    *(volatile u8 *)(uptr_t)SMP64_AP_FLAG_CENTRY = 'D';
    *(volatile u32 *)(uptr_t)(SMP64_TRAMP_BASE + 0x554) = expected_id;
    *(volatile u32 *)(uptr_t)(SMP64_TRAMP_BASE + 0x55C) = 0xC0DEU;
    smp64_wrmsr(MSR_EFER, smp64_kernel_efer);
    smp64_wrmsr(MSR_STAR, smp64_kernel_star);
    smp64_wrmsr(MSR_LSTAR, smp64_kernel_lstar);
    smp64_wrmsr(MSR_FMASK, smp64_kernel_fmask);
    __asm__ volatile("mov %0, %%cr4" : : "r"(smp64_kernel_cr4) : "memory");
    __asm__ volatile("mov %0, %%cr3" : : "r"(smp64_kernel_cr3) : "memory");
    *(volatile u32 *)(uptr_t)(SMP64_TRAMP_BASE + 0x55C) = 0xC0DFU;
    for (u32 index = 0; index < SMP64_MAX; index++) {
        if (smp64_cpus[index].active &&
            smp64_cpus[index].apic_id == expected_id)
            cpu = &smp64_cpus[index];
    }
    rc = (!madt || !cpu) ? -2 :
         apic64_ap_start(madt->local_apic_address, expected_id);
    *(volatile u32 *)(uptr_t)(SMP64_TRAMP_BASE + 0x558) = (u32)rc;
    if (rc) {
        if (cpu) cpu->state = SMP64_STATE_OFF;
        for (;;)
            __asm__ volatile("cli; hlt" ::: "memory");
    }
    // Shared GDT, private TSS descriptor. LTR of the BSP selector #GPs
    // because that TSS is already busy; this CPU's slot is still free.
    // Reload GDTR first: the trampoline only installed the 7-entry view.
    idt64_reload();
    gdt64_load_cpu(cpu->index);
    smp64_cpu_by_apic[expected_id] = cpu;
    smp64_gs_kernel(cpu);
    __asm__ volatile("sti" ::: "memory");
    __atomic_thread_fence(__ATOMIC_RELEASE);
    cpu->state = SMP64_STATE_ONLINE;
    for (;;)
        __asm__ volatile("hlt" ::: "memory");
}

static void smp64_install_trampoline(void) {
    u8 *page = (u8 *)(uptr_t)SMP64_TRAMP_BASE;
    for (u32 index = 0; index < 0x1000; index++)
        page[index] = 0;
    smp64_copy(page + 0x100, smp_tramp_gdt, smp_tramp_gdt_size);
    smp64_copy(page + 0x200, smp_tramp_code16, smp_tramp_code16_size);
    smp64_copy(page + 0x300, smp_tramp_code32, smp_tramp_code32_size);
    smp64_copy(page + 0x400, smp_tramp_code64, smp_tramp_code64_size);
    smp64_copy(page + 0x000, smp_tramp_entry, smp_tramp_entry_size);
}

static void smp64_fill_overlay(struct smp64_cpu *cpu, u64 stack_top) {
    *(volatile u64 *)(uptr_t)(SMP64_TRAMP_BASE + 0x500) = gdt64_base();
    *(volatile u32 *)(uptr_t)(SMP64_TRAMP_BASE + 0x508) = cpu->apic_id;
    *(volatile u64 *)(uptr_t)(SMP64_TRAMP_BASE + 0x510) = stack_top;
    *(volatile u64 *)(uptr_t)(SMP64_TRAMP_BASE + 0x518) =
        (u64)(uptr_t)smp64_ap_entry;
}

static int smp64_start_cpu(struct smp64_cpu *cpu, u64 stack_top) {
    volatile u8 *page = (volatile u8 *)(uptr_t)SMP64_TRAMP_BASE;
    for (u32 index = 0; index < 0x40; index++)
        page[0x500 + index] = 0;
    cpu->state = SMP64_STATE_BOOTING;
    cpu->ipi_ack = 0;
    cpu->work_done = 0;
    smp64_fill_overlay(cpu, stack_top);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    if (apic64_send_init(cpu->apic_id)) return -1;
    smp64_install_trampoline();
    smp64_fill_overlay(cpu, stack_top);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    // No serial in the INIT/SIPI window: the AP and BSP share COM1.
    apic64_delay_ms(10);
    if (apic64_send_sipi(cpu->apic_id, SMP64_SIPI_VECTOR)) return -1;
    apic64_delay_ms(10);
    if (apic64_send_sipi(cpu->apic_id, SMP64_SIPI_VECTOR)) return -1;
    u32 deadline = SMP64_BOOT_TIMEOUT_MS / 10;
    u32 state = SMP64_STATE_BOOTING;
    while (deadline--) {
        state = __atomic_load_n(&cpu->state, __ATOMIC_ACQUIRE);
        if (state == SMP64_STATE_ONLINE || state == SMP64_STATE_OFF)
            break;
        apic64_delay_ms(10);
    }
    return (state == SMP64_STATE_ONLINE) ? 0 : -1;
}

int smp64_init(void) {
    irq_state_t irq_state = irq_save();
    const struct acpi_madt_info *madt = acpi64_madt();
    if (!madt) {
        irq_restore(irq_state);
        return -1;
    }
    __asm__ volatile("mov %%cr3, %0" : "=r"(smp64_kernel_cr3));
    __asm__ volatile("mov %%cr4, %0" : "=r"(smp64_kernel_cr4));
    smp64_kernel_efer = smp64_rdmsr((u32)MSR_EFER);
    smp64_kernel_star = smp64_rdmsr((u32)MSR_STAR);
    smp64_kernel_lstar = smp64_rdmsr((u32)MSR_LSTAR);
    smp64_kernel_fmask = smp64_rdmsr((u32)MSR_FMASK);
    smp64_bsp_id = apic64_id();
    smp64_cpus[0].active = 1;
    smp64_cpus[0].apic_id = smp64_bsp_id;
    smp64_cpus[0].index = 0;
    smp64_cpus[0].state = SMP64_STATE_ONLINE;
    smp64_cpu_by_apic[smp64_bsp_id] = &smp64_cpus[0];
    smp64_online = 1;
    smp64_gs_kernel(&smp64_cpus[0]);
    if (madt->enabled_cpus < 2) {
        serial64_write("Mich x86_64: SMP disabled (single CPU)\n");
        irq_restore(irq_state);
        return 0;
    }
    serial64_write("Mich x86_64: SMP MADT enumeration pass\n");
    for (u32 index = 0; index < SMP64_MAX; index++)
        smp64_stacks[index].canary = 0x4D494348534D5031ULL;
    serial64_write("Mich x86_64: SMP per-CPU storage pass\n");
    smp64_install_trampoline();
    u32 started = 1;
    for (u32 index = 0;
         index < madt->cpu_count && started < SMP64_MAX; index++) {
        const struct acpi_cpu_info *info = &madt->cpus[index];
        if (info->apic_id == smp64_bsp_id) continue;
        struct smp64_cpu *cpu = &smp64_cpus[started];
        cpu->active = 1;
        cpu->apic_id = info->apic_id;
        cpu->acpi_id = info->acpi_id;
        cpu->index = started;
        cpu->current = SMP64_CURRENT_NONE;
        cpu->state = SMP64_STATE_OFF;
        u64 stack_top =
            (u64)(uptr_t)&smp64_stacks[started].bytes[SMP64_STACK_SIZE];
        stack_top &= ~0xFULL;
        if (smp64_start_cpu(cpu, stack_top)) {
            serial64_write("Mich x86_64: SMP AP bring-up failed\n");
            serial64_write("Mich x86_64: continuing as single CPU\n");
            cpu->active = 0;
            cpu->state = SMP64_STATE_OFF;
            smp64_online = 1;
            irq_restore(irq_state);
            return 0;
        }
        smp64_online++;
        started++;
    }
    if (started < 2) {
        serial64_write("Mich x86_64: SMP AP bring-up failed\n");
        serial64_write("Mich x86_64: continuing as single CPU\n");
        irq_restore(irq_state);
        return 0;
    }
    serial64_write("Mich x86_64: SMP AP bring-up pass (");
    serial64_hex(smp64_online);
    serial64_write(" cpus online)\n");
    irq_restore(irq_state);
    return 0;
}

static struct smp64_cpu *smp64_cpu(u32 index) {
    if (index >= smp64_online || !smp64_cpus[index].active)
        return 0;
    return &smp64_cpus[index];
}

u8 smp64_cpu_apic_id(u32 index) {
    struct smp64_cpu *cpu = smp64_cpu(index);
    return cpu ? (u8)cpu->apic_id : 0;
}

int smp64_ipi_cpu(u32 index, u32 vector) {
    struct smp64_cpu *cpu = smp64_cpu(index);
    if (!cpu) return -1;
    return apic64_ipi((u8)cpu->apic_id, (u8)vector);
}

u32 smp64_ipi_ack(u32 index) {
    struct smp64_cpu *cpu = smp64_cpu(index);
    return cpu ? __atomic_load_n(&cpu->ipi_ack, __ATOMIC_ACQUIRE) : 0;
}

void smp64_clear_ipi_ack(u32 index) {
    struct smp64_cpu *cpu = smp64_cpu(index);
    if (cpu) cpu->ipi_ack = 0;
}

u32 smp64_work_done(u32 index) {
    struct smp64_cpu *cpu = smp64_cpu(index);
    return cpu ? __atomic_load_n(&cpu->work_done, __ATOMIC_ACQUIRE) : 0;
}

void smp64_clear_work_done(u32 index) {
    struct smp64_cpu *cpu = smp64_cpu(index);
    if (cpu) cpu->work_done = 0;
}

u32 smp64_ticks(u32 index) {
    struct smp64_cpu *cpu = smp64_cpu(index);
    return cpu ? __atomic_load_n(&cpu->ticks, __ATOMIC_ACQUIRE) : 0;
}

void smp64_clear_ticks(u32 index) {
    struct smp64_cpu *cpu = smp64_cpu(index);
    if (cpu) cpu->ticks = 0;
}

u32 smp64_tr(u32 index) {
    struct smp64_cpu *cpu = smp64_cpu(index);
    return cpu ? __atomic_load_n(&cpu->tr, __ATOMIC_ACQUIRE) : 0;
}

void smp64_clear_tr(u32 index) {
    struct smp64_cpu *cpu = smp64_cpu(index);
    if (cpu) cpu->tr = 0;
}

u32 smp64_cpu_current(u32 index) {
    struct smp64_cpu *cpu = smp64_cpu(index);
    return cpu ? cpu->current : SMP64_CURRENT_NONE;
}

u32 smp64_user_done(u32 index) {
    struct smp64_cpu *cpu = smp64_cpu(index);
    return cpu ? __atomic_load_n(&cpu->user_done, __ATOMIC_ACQUIRE) : 0;
}

u32 smp64_user_irq(u32 index) {
    struct smp64_cpu *cpu = smp64_cpu(index);
    return cpu ? __atomic_load_n(&cpu->user_irq, __ATOMIC_ACQUIRE) : 0;
}

u32 smp64_preempts(u32 index) {
    struct smp64_cpu *cpu = smp64_cpu(index);
    return cpu ? __atomic_load_n(&cpu->preempts, __ATOMIC_ACQUIRE) : 0;
}

void smp64_set_syscall_live(u32 index, u32 live) {
    struct smp64_cpu *cpu = smp64_cpu(index);
    if (cpu)
        __atomic_store_n(&cpu->syscall_live, live, __ATOMIC_RELEASE);
}

int smp64_gs_is_bsp(void) {
    return smp64_rdmsr(MSR_GS_BASE) == (u64)(uptr_t)&smp64_cpus[0];
}

int smp64_sysstack_ok_cpu(u32 index) {
    if (index >= smp64_online) return 0;
    return smp64_sysstacks[index].canary == SMP64_SYSCALL_CANARY;
}

int smp64_sysstacks_ok(void) {
    if (!smp64_gs_is_bsp()) return 0;
    for (u32 index = 0; index < smp64_online; index++) {
        u64 rsp = smp64_cpus[index].sc.kernel_rsp;
        u64 base = (u64)(uptr_t)smp64_sysstacks[index].bytes;
        u64 end = base + SMP64_SYSCALL_STACK;
        if (smp64_sysstacks[index].canary != SMP64_SYSCALL_CANARY)
            return 0;
        if (rsp <= base || rsp > end) return 0;
        for (u32 other = 0; other < index; other++)
            if (smp64_cpus[other].sc.kernel_rsp == rsp) return 0;
    }
    return 1;
}

void smp64_spin_reset(void) {
    smp64_spin_counter = 0;
}

void smp64_spin_bsp(u32 turns) {
    for (u32 turn = 0; turn < turns; turn++) {
        spin_lock(&smp64_spin);
        smp64_spin_counter++;
        spin_unlock(&smp64_spin);
    }
}

u64 smp64_spin_count(void) {
    return smp64_spin_counter;
}

//
// Dedicated AP user slot, never init64-two. Stub bytes come from the
// caller: tests pass their fixtures, host_start passes the idle spin.
// pin maps the task; arm also makes it this CPU's current for IPI_USER.
//
int smp64_pin_user(u32 index, const u8 *stub, u32 size, u32 *slot_out) {
    struct smp64_cpu *cpu = smp64_cpu(index);
    struct task *task;
    struct task_context64 *context;
    u32 slot;
    u32 space;
    paddr_t code;
    paddr_t stack;
    u8 *bytes;
    if (!cpu || !stub || !size || size > 4096 || !slot_out) return -1;
    task = task_alloc_slot();
    if (!task) return -1;
    slot = (u32)(task - task_pool);
    context = &task_contexts[slot];
    if (vm64_create_space(&space)) {
        task_free_slot(task);
        return -1;
    }
    context->vm_space = space;
    context->vm_valid = 1;
    code = vm64_alloc_page();
    stack = vm64_alloc_page();
    if (!code || !stack) {
        if (code) vm64_free_page(code);
        if (stack) vm64_free_page(stack);
        task_free_slot(task);
        return -1;
    }
    bytes = (u8 *)(uptr_t)code;
    for (u32 byte = 0; byte < 4096; byte++) bytes[byte] = 0;
    for (u32 byte = 0; byte < size; byte++) bytes[byte] = stub[byte];
    if (vm64_map(space, VM64_PROGRAM_BASE, code, 0, 1)) {
        vm64_free_page(code);
        vm64_free_page(stack);
        task_free_slot(task);
        return -1;
    }
    if (vm64_map(space, VM64_STACK_TOP - 4096, stack, 1, 0)) {
        vm64_free_page(stack);
        task_free_slot(task);
        return -1;
    }
    task->ring = 3;
    task->page_dir = vm64_root(space);
    task->parent_id = 0;
    task->on_cpu = (int)cpu->index;
    task_set_name(task, "ap-user");
    context->rflags = USER_EFLAGS;
    context->rip = VM64_PROGRAM_BASE;
    context->rsp = VM64_STACK_TOP;
    *slot_out = slot;
    return 0;
}

int smp64_arm_user(u32 index, const u8 *stub, u32 size) {
    struct smp64_cpu *cpu = smp64_cpu(index);
    struct task_context64 *context;
    u32 slot;
    if (!cpu) return -1;
    if (smp64_pin_user(index, stub, size, &slot)) return -1;
    context = &task_contexts[slot];
    smp64_user_rip = VM64_PROGRAM_BASE;
    smp64_user_rsp = VM64_STACK_TOP;
    smp64_user_cr3 = task_pool[slot].page_dir;
    // Cleared so a later AP timer save must refill them from the frame.
    context->rip = 0;
    context->rsp = 0;
    cpu->current = slot;
    cpu->user_done = 0;
    cpu->user_irq = 0;
    cpu->preempts = 0;
    cpu->syscall_live = 0;
    return 0;
}

// Only tasks already on this CPU. Does not steal TASK_CPU_NONE (init64-two).
int smp64_pick_next(u32 current) {
    int cpu = (int)smp64_this()->index;
    if (current >= MAX_TASKS) return -1;
    for (int offset = 1; offset <= task_pool_count; offset++) {
        int candidate = ((int)current + offset) % task_pool_count;
        if (task_pool[candidate].state != TASK_RUNNING) continue;
        if (task_pool[candidate].on_cpu != cpu) continue;
        return candidate;
    }
    return (int)current;
}

void smp64_disarm_user(u32 index) {
    struct smp64_cpu *cpu = smp64_cpu(index);
    u32 slot;
    if (!cpu || cpu->current == SMP64_CURRENT_NONE) return;
    slot = cpu->current;
    cpu->syscall_live = 0;
    cpu->current = SMP64_CURRENT_NONE;
    if (slot < MAX_TASKS)
        task_free_slot(&task_pool[slot]);
}

// jmp $ — AP host idle in ring3. Timer stays masked: TCG timeslice
// vs the 12s smoke if the AP runs 100 Hz.
static const u8 smp64_host_spin[] = { 0xEB, 0xFE };
static u32 smp64_host_noted;

int smp64_host_start(void) {
    u32 slot;
    if (smp64_online < 2) return 0;
    if (smp64_arm_user(1, smp64_host_spin, sizeof(smp64_host_spin)))
        return -1;
    slot = smp64_cpus[1].current;
    if (slot < MAX_TASKS)
        task_set_name(&task_pool[slot], "ap-idle");
    __atomic_thread_fence(__ATOMIC_RELEASE);
    if (smp64_ipi_cpu(1, SMP64_IPI_USER)) {
        smp64_disarm_user(1);
        return -1;
    }
    return 0;
}

int smp64_host_note(u64 cs) {
    if (smp64_host_noted || smp64_online < 2) return 0;
    if ((cs & 3) != 3) return 0;
    if (smp64_cpus[1].current == SMP64_CURRENT_NONE) return 0;
    smp64_host_noted = 1;
    return 1;
}

int smp64_timer_tick(u64 cs, u64 rsp) {
    if (smp64_online < 2) return 0;
    struct smp64_cpu *cpu = smp64_cpu_by_apic[apic64_id()];
    if (!cpu || cpu->index == 0) return 0;
    __atomic_fetch_add(&cpu->ticks, 1, __ATOMIC_RELEASE);
    if ((cs & 3) == 3 &&
        cpu->current != SMP64_CURRENT_NONE &&
        gdt64_on_ring0(cpu->index, rsp))
        __atomic_fetch_add(&cpu->user_irq, 1, __ATOMIC_RELEASE);
    return 1;
}

void smp64_ipi_dispatch(u32 vector) {
    struct smp64_cpu *cpu = smp64_cpu_by_apic[apic64_id()];
    if (!cpu) return;
    if (vector == SMP64_IPI_ACK) {
        if (smp64_rdmsr(MSR_GS_BASE) == (u64)(uptr_t)cpu)
            cpu->ipi_ack++;
    } else if (vector == SMP64_IPI_WORK) {
        for (u32 turn = 0; turn < SMP64_SPIN_TURNS; turn++) {
            spin_lock(&smp64_spin);
            smp64_spin_counter++;
            spin_unlock(&smp64_spin);
        }
        __atomic_store_n(&cpu->work_done, 1, __ATOMIC_RELEASE);
    } else if (vector == SMP64_IPI_TIMER) {
        apic64_timer_enable();
    } else if (vector == SMP64_IPI_TIMER_OFF) {
        apic64_timer_mask();
    } else if (vector == SMP64_IPI_USER) {
        smp64_ap_enter_user(cpu);
    } else if (vector == SMP64_IPI_PARK) {
        u32 ds;
        apic64_eoi();
        apic64_timer_mask();
        ds = GDT64_KERNEL_DS;
        __asm__ volatile("mov %0, %%ds; mov %0, %%es" : : "r"(ds));
        smp64_gs_kernel(cpu);
        vm64_activate(smp64_kernel_cr3);
        __atomic_store_n(&cpu->user_done, 1, __ATOMIC_RELEASE);
        // sti: a later IPI_USER must be able to land (cli;hlt blocked it).
        for (;;)
            __asm__ volatile("sti; hlt" ::: "memory");
    } else if (vector == SMP64_IPI_TSS) {
        u16 tr;
        __asm__ volatile("str %0" : "=m"(tr));
        __atomic_store_n(&cpu->tr, tr, __ATOMIC_RELEASE);
    }
}

u32 smp64_cpu_count(void) {
    return smp64_online;
}

u8 smp64_bsp_apic_id(void) {
    return smp64_bsp_id;
}
